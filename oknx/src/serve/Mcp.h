/**
 * @file        Mcp.h
 * @brief       MCP on /mcp of `oknx serve`, both generations on one path: 2026-07-28 (per-request `_meta`, no
 *              sessions) and 2025-03-26 … 2025-11-25 (`initialize`, `Mcp-Session-Id`).
 * @details     Era per request: `initialize` selects the legacy semantics of that session; a request carrying
 *              `_meta["io.modelcontextprotocol/protocolVersion"]` is served statelessly by the 2026-07-28 rules
 *              (headers MCP-Protocol-Version, Mcp-Method, Mcp-Name must match the body, else 400 HeaderMismatch
 *              -32020; an unsupported version 400 -32022; a missing required `_meta` field 400 -32602; an unknown
 *              method 404 -32601). Tools are the operations of serve, every call goes through the same guard as
 *              the API. Long calls answer as an SSE stream with notifications/progress at least every 30 s; on
 *              the modern generation closing that stream cancels the call, on the legacy one only
 *              notifications/cancelled does. No result text exceeds 60 000 characters; a larger result stands
 *              summarised in the call and in full as a resource.
 * @date        2026-10-19
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include "serve/JsonGet.h"
#include "serve/Http.h"
#include "serve/Key.h"
#include "serve/LeaderApi.h"
#include "serve/Paths.h"

#include <atomic>
#include <cstdlib>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace ftc
{
namespace serve
{

static const char* MCP_MODERN = "2026-07-28";
static const char* MCP_LEGACY[] = {"2025-11-25", "2025-06-18", "2025-03-26"};
static constexpr size_t MCP_TEXT_MAX = 60000;

inline bool mcpIsLegacy(const std::string& v)
{
    for (const char* x : MCP_LEGACY)
        if (v == x) return true;
    return false;
}

/** @brief Decode the `=?base64?…?=` sentinel of an Mcp-Name header; anything else stays as it is. */
inline std::string mcpHeaderValue(const std::string& v)
{
    const std::string pre = "=?base64?", suf = "?=";
    if (v.size() < pre.size() + suf.size() || v.compare(0, pre.size(), pre) != 0 ||
        v.compare(v.size() - suf.size(), suf.size(), suf) != 0)
        return v;
    const std::string b = v.substr(pre.size(), v.size() - pre.size() - suf.size());
    static const std::string tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, bits = -8;
    for (char c : b)
    {
        if (c == '=') break;
        const size_t p = tbl.find(c);
        if (p == std::string::npos) return std::string("\x01invalid");
        val = (val << 6) + (int)p;
        bits += 6;
        if (bits >= 0)
        {
            out += (char)((val >> bits) & 0xFF);
            bits -= 8;
        }
    }
    return out;
}

class McpServer
{
  public:
    void post(LeaderApi& L, const httplib::Request& req, httplib::Response& res, const Access& acc)
    {
        if (acc.kind != Access::Key)
        {
            rpcError(res, 401, json(), -32600, "this needs the key of serve.key in X-Oknx-Key");
            return;
        }
        json m = json::parse(req.body, nullptr, false);
        if (!m.is_object() || m.value("jsonrpc", std::string()) != "2.0")
        {
            rpcError(res, 400, json(), -32700, "not a JSON-RPC 2.0 message");
            return;
        }
        const bool isRequest = m.contains("id") && !m["id"].is_null();
        const json id = isRequest ? m["id"] : json();
        if ((m.contains("method") && !m["method"].is_string()) || (m.contains("params") && !m["params"].is_object()) ||
            (m.contains("params") && m["params"].contains("_meta") && !m["params"]["_meta"].is_object()))
        {
            rpcError(res, 400, id, -32600, "method is a string, params and params._meta are objects");
            return;
        }
        const std::string method = m.value("method", std::string());
        if (method.empty())
        {
            // a client never sends responses on this transport
            rpcError(res, 400, id, -32600, "a POST carries one request or notification");
            return;
        }
        const json params = m.value("params", json::object());
        const json meta = params.value("_meta", json::object());
        const bool modern = meta.contains("io.modelcontextprotocol/protocolVersion");
        const std::string hdrVer = req.get_header_value("MCP-Protocol-Version");
        if (debug()) // method and protocol headers only, never argument values
            std::fprintf(stderr, "[mcp] %s id=%s modern=%d ver='%s' sid='%s'\n", method.c_str(), id.dump().c_str(), modern ? 1 : 0,
                         hdrVer.c_str(), req.get_header_value("Mcp-Session-Id").c_str());

        if (method == "initialize") return legacyInitialize(L, req, res, id, params);
        if (modern) return modernRequest(L, req, res, m, id, method, params, meta, hdrVer, acc);

        // legacy (or a modern request without its metadata)
        if (hdrVer == MCP_MODERN)
        {
            rpcError(res, 400, id, -32602, "io.modelcontextprotocol/protocolVersion is missing in _meta");
            return;
        }
        const std::string ver = hdrVer.empty() ? std::string("2025-03-26") : hdrVer;
        if (!mcpIsLegacy(ver))
        {
            unsupported(res, id, ver);
            return;
        }
        const std::string sid = req.get_header_value("Mcp-Session-Id");
        {
            std::lock_guard<std::mutex> lk(_m);
            if (sid.empty() || !_sessions.count(sid))
            {
                rpcError(res, sid.empty() ? 400 : 404, id, -32600,
                         sid.empty() ? "send initialize first (no Mcp-Session-Id)" : "unknown or ended session");
                return;
            }
            _sessions[sid].lastUse = monoMs();
        }
        res.set_header("Mcp-Session-Id", sid);
        if (!isRequest)
        {
            if (method == "notifications/cancelled")
            {
                const std::string rid = params.contains("requestId") ? params["requestId"].dump() : std::string();
                std::lock_guard<std::mutex> lk(_m);
                auto it = _legacyCalls.find(sid + "|" + rid);
                if (it != _legacyCalls.end()) *it->second = true;
            }
            baseHeaders(res);
            res.status = 202;
            return;
        }
        handle(L, req, res, id, method, params, false, ver, sid, acc);
    }

    /** @brief GET /mcp: no standalone stream in either generation here. */
    void get(LeaderApi&, const httplib::Request&, httplib::Response& res, const Access&)
    {
        baseHeaders(res);
        res.status = 405;
        res.set_header("Allow", "POST, DELETE");
    }

    /** @brief DELETE /mcp ends a legacy session; the modern generation has none (405). */
    void del(LeaderApi&, const httplib::Request& req, httplib::Response& res, const Access& acc)
    {
        baseHeaders(res);
        const std::string sid = req.get_header_value("Mcp-Session-Id");
        if (acc.kind != Access::Key || sid.empty())
        {
            res.status = sid.empty() ? 405 : 401;
            return;
        }
        std::lock_guard<std::mutex> lk(_m);
        res.status = _sessions.erase(sid) ? 200 : 404;
    }

  private:
    static bool debug()
    {
        static const bool on = [] {
            const char* e = std::getenv("OKNX_SERVE_DEBUG");
            return e != nullptr && *e == '1';
        }();
        return on;
    }

    struct LegacySession
    {
        std::string version;
        std::string client;
        uint64_t created = 0, lastUse = 0;
    };

    // --- JSON-RPC helpers ---------------------------------------------------------------------------
    static void rpcError(httplib::Response& res, int status, const json& id, int code, const std::string& msg,
                         const json& data = json())
    {
        json e = {{"jsonrpc", "2.0"}, {"error", {{"code", code}, {"message", msg}}}};
        if (!id.is_null()) e["id"] = id;
        if (!data.is_null()) e["error"]["data"] = data;
        sendJson(res, status, e);
    }
    static void unsupported(httplib::Response& res, const json& id, const std::string& ver)
    {
        json sup = json::array({MCP_MODERN});
        for (const char* x : MCP_LEGACY) sup.push_back(x);
        rpcError(res, 400, id, -32022, "Unsupported protocol version", {{"supported", sup}, {"requested", ver}});
    }

    // --- legacy --------------------------------------------------------------------------------------
    void legacyInitialize(LeaderApi& L, const httplib::Request&, httplib::Response& res, const json& id, const json& params)
    {
        std::string ver = jget(params, "protocolVersion", "");
        if (!mcpIsLegacy(ver)) ver = MCP_LEGACY[0]; // answer with the newest legacy version we speak
        const std::string sid = randomHex(16);
        {
            std::lock_guard<std::mutex> lk(_m);
            const uint64_t now = monoMs();
            for (auto it = _sessions.begin(); it != _sessions.end();)
                it = now - it->second.lastUse > 24ull * 3600 * 1000 ? _sessions.erase(it) : std::next(it);
            _sessions[sid] = {ver, jget(jget<json>(params, "clientInfo", json::object()), "name", ""), now, now};
        }
        res.set_header("Mcp-Session-Id", sid);
        sendJson(res, 200,
                 {{"jsonrpc", "2.0"},
                  {"id", id},
                  {"result",
                   {{"protocolVersion", ver},
                    {"capabilities", {{"tools", {{"listChanged", false}}}, {"resources", {{"listChanged", false}}}}},
                    {"serverInfo", {{"name", "oknx-serve"}, {"version", L.version()}}},
                    {"instructions", instructions()}}}});
    }

    // --- modern --------------------------------------------------------------------------------------
    void modernRequest(LeaderApi& L, const httplib::Request& req, httplib::Response& res, const json& m, const json& id,
                       const std::string& method, const json& params, const json& meta, const std::string& hdrVer,
                       const Access& acc)
    {
        (void)m;
        const std::string ver = meta["io.modelcontextprotocol/protocolVersion"].is_string()
                                    ? meta["io.modelcontextprotocol/protocolVersion"].get<std::string>()
                                    : std::string();
        // header and body must agree, and the standard headers must be there
        if (hdrVer.empty() || hdrVer != ver)
        {
            rpcError(res, 400, id, -32020, "Header mismatch: MCP-Protocol-Version '" + hdrVer + "' does not match _meta '" + ver + "'");
            return;
        }
        if (ver != MCP_MODERN)
        {
            unsupported(res, id, ver);
            return;
        }
        if (!meta.contains("io.modelcontextprotocol/clientCapabilities"))
        {
            rpcError(res, 400, id, -32602, "io.modelcontextprotocol/clientCapabilities is missing in _meta");
            return;
        }
        if (req.get_header_value("Mcp-Method") != method)
        {
            rpcError(res, 400, id, -32020, "Header mismatch: Mcp-Method does not match the body method");
            return;
        }
        if (method == "tools/call" || method == "resources/read" || method == "prompts/get")
        {
            const std::string bodyName = jget(params, method == "resources/read" ? "uri" : "name", "");
            if (!req.has_header("Mcp-Name") || mcpHeaderValue(req.get_header_value("Mcp-Name")) != bodyName)
            {
                rpcError(res, 400, id, -32020, "Header mismatch: Mcp-Name does not match the body");
                return;
            }
        }
        const bool isRequest = !id.is_null();
        if (!isRequest)
        {
            baseHeaders(res);
            res.status = 202;
            return;
        }
        handle(L, req, res, id, method, params, true, ver, std::string(), acc);
    }

    // --- methods --------------------------------------------------------------------------------------
    void handle(LeaderApi& L, const httplib::Request& req, httplib::Response& res, const json& id, const std::string& method,
                const json& params, bool modern, const std::string& ver, const std::string& sid, const Access& acc)
    {
        (void)req;
        // 2026-07-28: the cacheable results carry ttlMs and cacheScope (server/utilities/caching)
        auto ok = [&](json result, int ttlMs = -1, const char* scope = nullptr) {
            if (modern)
            {
                result["resultType"] = "complete";
                if (ttlMs >= 0)
                {
                    result["ttlMs"] = ttlMs;
                    result["cacheScope"] = scope;
                }
                result["_meta"] = {{"io.modelcontextprotocol/serverInfo", {{"name", "oknx-serve"}, {"version", L.version()}}}};
            }
            sendJson(res, 200, {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
        };
        if (method == "server/discover")
        {
            if (!modern) // a legacy session does not know this method
            {
                rpcError(res, 200, id, -32601, "Method not found");
                return;
            }
            json sup = json::array({MCP_MODERN});
            for (const char* x : MCP_LEGACY) sup.push_back(x);
            ok({{"supportedVersions", sup},
                {"capabilities", {{"tools", {{"listChanged", false}}}, {"resources", {{"listChanged", false}}}}},
                {"instructions", instructions()}},
               300000, "public");
            return;
        }
        if (method == "ping")
        {
            ok(json::object());
            return;
        }
        if (method == "tools/list")
        {
            ok({{"tools", toolList(L, modern)}}, 300000, "public"); // fixed for the life of this serve
            return;
        }
        if (method == "resources/list")
        {
            ok({{"resources", resourceList(L)}}, 0, "private"); // captures and reports appear all the time
            return;
        }
        if (method == "resources/read")
        {
            const std::string uri = jget(params, "uri", "");
            std::string text, mime;
            if (!readResource(L, uri, text, mime))
            {
                rpcError(res, 200, id, modern ? -32602 : -32002, "Resource not found", {{"uri", uri}});
                return;
            }
            ok({{"contents", json::array({{{"uri", uri}, {"mimeType", mime}, {"text", text}}})}}, 0, "private");
            return;
        }
        if (method == "tools/call")
        {
            toolCall(L, res, id, params, modern, ver, sid, acc);
            return;
        }
        rpcError(res, modern ? 404 : 200, id, -32601, "Method not found");
    }

    static std::string instructions()
    {
        return "oknx serve owns the KNXnet/IP connections of this machine. Every tool goes through serve's guard: "
               "the rules file decides which interface allows reading, monitoring or writing. A refusal comes back as "
               "a tool error with its code (GUARD_DENIED, BENCH_LOCKED, BUSY, ...) and a reason; do not retry a "
               "GUARD_DENIED or BENCH_LOCKED call unchanged. Start with discover or bench_state.";
    }

    json toolList(LeaderApi& L, bool modern)
    {
        (void)modern;
        json a = json::array();
        for (const auto& d : opTable())
        {
            json t = {{"name", d.name},
                      {"title", L.lang() == "de" ? d.titleDe : d.titleEn},
                      {"description", (L.lang() == "de" ? d.helpDe : d.helpEn) + " Class: " + opClassName(d.cls) +
                                          (d.changes ? ". Changes the bus or the device." : ". Reads only.")},
                      {"inputSchema", d.input}};
            t["annotations"] = {{"readOnlyHint", !d.changes}, {"destructiveHint", d.changes}, {"openWorldHint", false}};
            a.push_back(t);
        }
        return a;
    }

    void toolCall(LeaderApi& L, httplib::Response& res, const json& id, const json& params, bool modern,
                  const std::string& ver, const std::string& sid, const Access& acc0)
    {
        const std::string name = jget(params, "name", "");
        const OpDesc* d = findOp(name);
        if (!d)
        {
            rpcError(res, 200, id, -32602, "Unknown tool: " + name);
            return;
        }
        Access acc = acc0;
        acc.caller = "mcp" + std::string(modern ? "" : "-legacy") + (acc0.caller.empty() ? "" : ":" + acc0.caller);
        const json args = params.value("arguments", json::object());
        const json meta = params.value("_meta", json::object());
        const json progressToken = meta.contains("progressToken") ? meta["progressToken"] : json();
        auto cancel = std::make_shared<std::atomic<bool>>(false);
        if (!modern)
        {
            std::lock_guard<std::mutex> lk(_m);
            _legacyCalls[sid + "|" + id.dump()] = cancel;
        }
        (void)ver;
        if (!d->progress)
        {
            json env = L.invoke(name, args, acc, nullptr, cancel, true);
            dropCall(sid, id, modern);
            sendJson(res, 200, {{"jsonrpc", "2.0"}, {"id", id}, {"result", toolResult(env, modern, L)}});
            return;
        }
        // long: an SSE stream with progress, keep-alive comments, then the result
        struct Shared
        {
            std::mutex m;
            std::vector<json> progress;
            bool done = false;
            json env;
            double count = 0;
            double lastValue = 0; // the progress value must rise with every notification
            uint64_t lastProgress = monoMs();
        };
        auto sh = std::make_shared<Shared>();
        std::thread([&L, sh, name, args, acc, cancel, progressToken] {
            json env;
            try
            {
            env = L.invoke(
                name, args, acc,
                [sh, progressToken](const json& ev) {
                    if (progressToken.is_null()) return;
                    std::lock_guard<std::mutex> lk(sh->m);
                    sh->count += 1;
                    json p = {{"progressToken", progressToken}, {"progress", sh->count}, {"message", ev.dump()}};
                    double v = sh->count;
                    if (ev.contains("seconds") && ev.contains("elapsed_s"))
                    {
                        v = jget(ev, "elapsed_s", sh->count);
                        p["total"] = ev["seconds"];
                    }
                    if (v <= sh->lastValue) v = sh->lastValue + 0.001;
                    sh->lastValue = v;
                    p["progress"] = v;
                    sh->progress.push_back({{"jsonrpc", "2.0"}, {"method", "notifications/progress"}, {"params", p}});
                    sh->lastProgress = monoMs();
                },
                cancel, true);
            }
            catch (const std::exception& e) // the call fails, serve and the stream go on
            {
                env = errorEnvelope("INTERNAL", std::string("the operation failed: ") + e.what());
            }
            std::lock_guard<std::mutex> lk(sh->m);
            sh->env = env;
            sh->done = true;
        }).detach();
        auto lastWrite = std::make_shared<uint64_t>(monoMs());
        auto sent = std::make_shared<bool>(false);
        auto opened = std::make_shared<bool>(false);
        res.set_header("X-Accel-Buffering", "no"); // the session id is already set by post(); set_header appends
        res.set_chunked_content_provider(
            "text/event-stream",
            guardedProvider([this, sh, id, modern, lastWrite, sent, opened, cancel, sid, progressToken, &L](size_t, httplib::DataSink& sink) {
                if (*opened) std::this_thread::sleep_for(std::chrono::milliseconds(200));
                std::string out;
                bool done = false;
                json env;
                {
                    std::lock_guard<std::mutex> lk(sh->m);
                    // a caller that asked for progress hears from us at least every 25 s, whatever the operation reports
                    if (!progressToken.is_null() && !sh->done && monoMs() - sh->lastProgress >= 25000)
                    {
                        sh->lastValue += 0.001;
                        sh->progress.push_back({{"jsonrpc", "2.0"},
                                                {"method", "notifications/progress"},
                                                {"params", {{"progressToken", progressToken}, {"progress", sh->lastValue}, {"message", "still running"}}}});
                        sh->lastProgress = monoMs();
                    }
                    for (const auto& p : sh->progress)
                        out += "event: message\ndata: " + p.dump(-1, ' ', false, json::error_handler_t::replace) + "\n\n";
                    sh->progress.clear();
                    done = sh->done;
                    if (done) env = sh->env;
                }
                if (done && !*sent)
                {
                    *sent = true;
                    dropCall(sid, id, modern);
                    json answer;
                    try
                    {
                        answer = {{"jsonrpc", "2.0"}, {"id", id}, {"result", toolResult(env, modern, L)}};
                    }
                    catch (const std::exception& e) // the caller still gets an answer to its id
                    {
                        answer = {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", -32603}, {"message", std::string("result not encodable: ") + e.what()}}}};
                    }
                    out += "event: message\ndata: " + answer.dump(-1, ' ', false, json::error_handler_t::replace) + "\n\n";
                }
                // a comment at once (opens the stream) and every 2 s: a closed stream is noticed within about 2 s
                if (out.empty() && (!*opened || monoMs() - *lastWrite >= 2000)) out = ":\n\n";
                *opened = true;
                if (!out.empty())
                {
                    if (!sink.write(out.data(), out.size()))
                    {
                        // modern: a closed response stream IS the cancellation; legacy: it is not
                        if (modern) *cancel = true;
                        return false;
                    }
                    *lastWrite = monoMs();
                }
                if (done)
                {
                    sink.done();
                    return true;
                }
                return true;
            }));
    }

    void dropCall(const std::string& sid, const json& id, bool modern)
    {
        if (modern) return;
        std::lock_guard<std::mutex> lk(_m);
        _legacyCalls.erase(sid + "|" + id.dump());
    }

    /** @brief An operation envelope as a CallToolResult; 2026-07-28 adds resultType and serverInfo. */
    json toolResult(const json& env, bool modern, LeaderApi& L)
    {
        json r = toolResultBody(env);
        if (modern)
        {
            r["resultType"] = "complete";
            r["_meta"] = {{"io.modelcontextprotocol/serverInfo", {{"name", "oknx-serve"}, {"version", L.version()}}}};
        }
        return r;
    }

    /** @brief The result body; oversized data goes to a resource. */
    json toolResultBody(const json& env)
    {
        if (!env.value("ok", false))
        {
            const json e = env.value("error", json::object());
            const std::string text = e.value("code", std::string("INTERNAL")) + ": " + e.value("reason", std::string());
            return {{"content", json::array({{{"type", "text"}, {"text", text + "\n" + clip(e.dump(1))}}})},
                    {"structuredContent", {{"ok", false}, {"error", e}, {"id", env.value("id", std::string())}}},
                    {"isError", true}};
        }
        json data = env.value("data", json::object());
        std::string full = data.dump(1);
        if (full.size() <= MCP_TEXT_MAX)
            return {{"content", json::array({{{"type", "text"}, {"text", full}}})},
                    {"structuredContent", data},
                    {"isError", false}};
        // too large for one result: keep it as a resource, return a summary
        const std::string opId = env.value("id", std::string("op"));
        const std::string path = serveDir(true) + PATH_SEP + "result-" + opId + ".json";
        std::ofstream(path, std::ios::binary | std::ios::trunc) << data.dump(1);
        json summary = json::object();
        for (auto it = data.begin(); it != data.end(); ++it)
        {
            const std::string v = it.value().dump();
            summary[it.key()] = v.size() <= 2000 ? it.value() : json("(" + std::to_string(v.size()) + " characters, see the resource)");
        }
        summary["truncated"] = true;
        summary["resource"] = "oknx://results/" + opId;
        return {{"content", json::array({{{"type", "text"}, {"text", clip(summary.dump(1))}},
                                         {{"type", "resource_link"}, {"uri", "oknx://results/" + opId}, {"name", "result-" + opId + ".json"},
                                          {"mimeType", "application/json"}}})},
                {"structuredContent", summary},
                {"isError", false}};
    }

    static std::string clip(const std::string& s)
    {
        return s.size() <= MCP_TEXT_MAX ? s : s.substr(0, MCP_TEXT_MAX - 40) + "\n… (cut at 60000 characters)";
    }

    // --- resources -----------------------------------------------------------------------------------
    static std::string dirOf(const std::string& p)
    {
        const size_t c = p.find_last_of("/\\");
        return c == std::string::npos ? std::string(".") : p.substr(0, c);
    }
    static std::vector<std::pair<std::string, time_t>> listDir(const std::string& dir)
    {
        std::vector<std::pair<std::string, time_t>> out;
        DIR* d = opendir(dir.c_str());
        if (!d) return out;
        while (dirent* e = readdir(d))
        {
            const std::string n = e->d_name;
            if (n == "." || n == "..") continue;
            struct stat st{};
            if (stat((dir + PATH_SEP + n).c_str(), &st) == 0) out.push_back({n, st.st_mtime});
        }
        closedir(d);
        std::sort(out.begin(), out.end(), [](const std::pair<std::string, time_t>& a, const std::pair<std::string, time_t>& b) {
            return a.second > b.second;
        });
        return out;
    }
    static bool readFile(const std::string& p, std::string& out)
    {
        std::ifstream f(p, std::ios::binary);
        if (!f) return false;
        std::stringstream ss;
        ss << f.rdbuf();
        out = ss.str();
        return true;
    }
    static bool safeName(const std::string& n)
    {
        return !n.empty() && n.find("..") == std::string::npos && n.find_first_of("/\\") == std::string::npos;
    }

    std::string reportsDir(LeaderApi& L) const
    {
        const std::string bc = L.benchConfigPath();
        return bc.empty() ? std::string() : dirOf(bc) + PATH_SEP + "Reports";
    }

    json resourceList(LeaderApi& L)
    {
        json a = json::array();
        if (L.rules()->loaded()) a.push_back({{"uri", "oknx://rules"}, {"name", "rules.json"}, {"mimeType", "application/json"},
                                            {"description", "the rules file serve guards with"}});
        if (!L.benchConfigPath().empty())
            a.push_back({{"uri", "oknx://bench-config"}, {"name", "Test-Bench-Config.json"}, {"mimeType", "application/json"},
                         {"description", "the test bench of the suite"}});
        const std::string rd = reportsDir(L);
        if (!rd.empty())
        {
            int n = 0;
            for (const auto& run : listDir(rd))
            {
                if (n >= 10) break;
                for (const auto& f : listDir(rd + PATH_SEP + run.first))
                    if (f.first.size() > 5 && f.first.compare(f.first.size() - 5, 5, ".json") == 0)
                    {
                        a.push_back({{"uri", "oknx://reports/" + run.first + "/" + f.first}, {"name", run.first + "/" + f.first},
                                     {"mimeType", "application/json"}});
                        n++;
                    }
            }
        }
        for (const auto& f : listDir(serveDir(true)))
        {
            const bool cap = f.first.compare(0, 8, "capture-") == 0 || f.first.compare(0, 8, "compare-") == 0;
            const bool res = f.first.compare(0, 7, "result-") == 0;
            if (!cap && !res) continue;
            a.push_back({{"uri", std::string(cap ? "oknx://captures/" : "oknx://results/") +
                                     (res ? f.first.substr(7, f.first.size() - 12) : f.first)},
                         {"name", f.first},
                         {"mimeType", f.first.find(".xml") != std::string::npos ? "application/xml" : "application/json"}});
            if (a.size() > 200) break;
        }
        return a;
    }

    bool readResource(LeaderApi& L, const std::string& uri, std::string& text, std::string& mime)
    {
        mime = "application/json";
        if (uri == "oknx://rules")
        {
            const auto rules = L.rules();
            return rules->loaded() && readFile(rules->path(), text);
        }
        if (uri == "oknx://bench-config") return !L.benchConfigPath().empty() && readFile(L.benchConfigPath(), text);
        const std::string rp = "oknx://reports/", cp = "oknx://captures/", xp = "oknx://results/";
        if (uri.compare(0, rp.size(), rp) == 0)
        {
            const std::string rest = uri.substr(rp.size());
            const size_t sl = rest.find('/');
            if (sl == std::string::npos || !safeName(rest.substr(0, sl)) || !safeName(rest.substr(sl + 1))) return false;
            return readFile(reportsDir(L) + PATH_SEP + rest.substr(0, sl) + PATH_SEP + rest.substr(sl + 1), text) && clipTo(text);
        }
        if (uri.compare(0, cp.size(), cp) == 0)
        {
            const std::string n = uri.substr(cp.size());
            if (!safeName(n)) return false;
            if (n.find(".xml") != std::string::npos) mime = "application/xml";
            return readFile(serveDir(true) + PATH_SEP + n, text) && clipTo(text);
        }
        if (uri.compare(0, xp.size(), xp) == 0)
        {
            const std::string n = uri.substr(xp.size());
            if (!safeName(n)) return false;
            return readFile(serveDir(true) + PATH_SEP + "result-" + n + ".json", text);
        }
        return false;
    }

    /** @brief A resource body stays under 16 MiB (Claude Code's limit for one body). */
    static bool clipTo(std::string& t)
    {
        const size_t cap = 15u << 20;
        if (t.size() > cap) t = t.substr(0, cap);
        return true;
    }

    std::mutex _m;
    std::map<std::string, LegacySession> _sessions;
    std::map<std::string, std::shared_ptr<std::atomic<bool>>> _legacyCalls;
};

} // namespace serve
} // namespace ftc

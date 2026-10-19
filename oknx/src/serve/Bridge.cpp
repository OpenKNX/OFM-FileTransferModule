/**
 * @file        Bridge.cpp
 * @brief       `oknx mcp`: MCP over stdio for programs that speak only stdio; every message goes on to the running
 *              `oknx serve` (POST /mcp). It opens no bus connection of its own.
 * @details     One JSON-RPC message per line in, the answers one per line out. Each request runs on its own HTTP
 *              connection and thread, so a long tool call does not hold up the next one. The mirrored headers
 *              (MCP-Protocol-Version, Mcp-Method, Mcp-Name) are taken from the message; a legacy session id from
 *              the initialize answer is sent on with every later message. notifications/cancelled closes the HTTP
 *              stream of that request (the cancel of the 2026-07-28 generation) and is passed on as well (the
 *              cancel of the legacy one).
 * @date        2026-10-19
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#include "core/Discovery.h" // winsock2 before windows.h

#include "serve/Bridge.h"
#include "serve/JsonGet.h"
#include "serve/Key.h"
#include "serve/Leader.h"
#include "serve/Mcp.h"
#include "serve/HttpLib.h"
#include "third_party/json.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#ifdef _WIN32
    #include <fcntl.h>
    #include <io.h>
#endif
#include <iostream>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace ftc
{
namespace serve
{

namespace
{
std::mutex g_out;
void emit(const std::string& line)
{
    std::lock_guard<std::mutex> lk(g_out);
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}
void emitError(const json& id, int code, const std::string& msg)
{
    json e = {{"jsonrpc", "2.0"}, {"error", {{"code", code}, {"message", msg}}}};
    if (!id.is_null()) e["id"] = id;
    emit(e.dump());
}

/** @brief A header value safe as plain ASCII, else the =?base64?…?= form the transport defines. */
std::string headerValue(const std::string& v)
{
    bool plain = !v.empty() && v.front() != ' ' && v.back() != ' ' &&
                 !(v.rfind("=?base64?", 0) == 0 && v.size() >= 2 && v.compare(v.size() - 2, 2, "?=") == 0);
    for (unsigned char c : v)
        if (c < 0x20 || c > 0x7E) plain = false;
    if (plain) return v;
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, bits = -6;
    for (unsigned char c : v)
    {
        val = (val << 8) + c;
        bits += 8;
        while (bits >= 0)
        {
            out += T[(val >> bits) & 0x3F];
            bits -= 6;
        }
    }
    if (bits > -6) out += T[((val << 8) >> (bits + 8)) & 0x3F];
    while (out.size() % 4) out += '=';
    return "=?base64?" + out + "?=";
}

/** @brief Shared with the request threads, which may outlive the read loop by a few seconds. */
struct State
{
    std::mutex m;
    std::mutex renewM; ///< one session renewal at a time
    std::string legacySession, legacyVersion;
    std::string initBody; ///< the client's initialize, sent again when serve has forgotten the session
    std::map<std::string, std::shared_ptr<httplib::Client>> inflight; // request id -> its connection
    std::set<std::string> cancelled;                                  // request ids the client cancelled
};

std::shared_ptr<httplib::Client> makeClient(int port)
{
    auto c = std::make_shared<httplib::Client>("127.0.0.1", port);
    c->set_connection_timeout(2, 0);
    c->set_read_timeout(3600, 0); // a 300-s monitor plus its drain: the stream carries keep-alives
    return c;
}

/** @brief What one POST /mcp brought back; SSE messages are passed on while they arrive. */
struct Reply
{
    bool ok = false;     ///< an HTTP answer came
    std::string err;     ///< else why not
    int status = 0;
    bool sse = false;
    bool answered = false; ///< a response to this request id went out (SSE) or the body is JSON-RPC
    std::string whole;     ///< the body when it was no SSE stream
    std::string sessionHdr;
};

Reply postOnce(httplib::Client& cli, const httplib::Headers& h, const std::string& body, const json& id)
{
    Reply r;
    std::string buf, ctype;
    auto onData = [&](const char* d, size_t n) {
        buf.append(d, n);
        for (;;) // SSE: one event per blank line; each data line is one JSON-RPC message
        {
            const size_t end = buf.find("\n\n");
            if (end == std::string::npos) break;
            const std::string ev = buf.substr(0, end);
            buf.erase(0, end + 2);
            std::string data;
            size_t p = 0;
            while (p < ev.size())
            {
                size_t nl = ev.find('\n', p);
                if (nl == std::string::npos) nl = ev.size();
                const std::string l = ev.substr(p, nl - p);
                if (l.rfind("data:", 0) == 0) data += l.substr(l.size() > 5 && l[5] == ' ' ? 6 : 5);
                p = nl + 1;
            }
            if (data.empty()) continue;
            const json j = json::parse(data, nullptr, false);
            if (j.is_object() && !id.is_null() && j.contains("id") && j["id"] == id && (j.contains("result") || j.contains("error")))
                r.answered = true;
            emit(data);
        }
        return true;
    };
    httplib::Request req;
    req.method = "POST";
    req.path = "/mcp";
    req.headers = h;
    req.headers.emplace("Content-Type", "application/json");
    req.body = body;
    req.response_handler = [&](const httplib::Response& res) {
        r.status = res.status;
        ctype = res.get_header_value("Content-Type");
        r.sse = ctype.find("text/event-stream") != std::string::npos;
        r.sessionHdr = res.get_header_value("Mcp-Session-Id");
        return true;
    };
    req.content_receiver = [&](const char* d, size_t n, uint64_t, uint64_t) {
        if (r.sse) return onData(d, n);
        r.whole.append(d, n);
        return true;
    };
    httplib::Response res;
    httplib::Error e = httplib::Error::Success;
    r.ok = cli.send(req, res, e);
    if (!r.ok) r.err = httplib::to_string(e);
    return r;
}

/**
 * @brief serve answered 404 to the legacy session @p stale (it was restarted): open a new one with the client's own
 *        initialize and notifications/initialized. Returns the session to use, empty when none could be opened.
 */
std::string renewSession(State& st, int port, const std::string& key, const std::string& stale)
{
    std::lock_guard<std::mutex> rl(st.renewM);
    std::string init;
    {
        std::lock_guard<std::mutex> lk(st.m);
        if (st.legacySession != stale) return st.legacySession; // another call renewed it already
        init = st.initBody;
    }
    if (init.empty()) return std::string();
    httplib::Client c("127.0.0.1", port);
    c.set_connection_timeout(2, 0);
    c.set_read_timeout(10, 0);
    httplib::Headers h = {{"X-Oknx-Key", key}, {"X-Oknx-Caller", "oknx-mcp"}, {"Accept", "application/json, text/event-stream"},
                          {"Mcp-Method", "initialize"}};
    auto r = c.Post("/mcp", h, init, "application/json");
    if (!r || r->status != 200) return std::string();
    const std::string sid = r->get_header_value("Mcp-Session-Id");
    if (sid.empty()) return std::string();
    const json j = json::parse(r->body, nullptr, false);
    std::string ver;
    {
        std::lock_guard<std::mutex> lk(st.m);
        ver = st.legacyVersion;
    }
    if (j.is_object() && j.contains("result")) ver = jget(j["result"], "protocolVersion", ver);
    httplib::Headers h2 = {{"X-Oknx-Key", key}, {"X-Oknx-Caller", "oknx-mcp"}, {"Accept", "application/json, text/event-stream"},
                           {"Mcp-Method", "notifications/initialized"}, {"Mcp-Session-Id", sid}};
    if (!ver.empty()) h2.emplace("MCP-Protocol-Version", ver);
    c.Post("/mcp", h2, R"({"jsonrpc":"2.0","method":"notifications/initialized"})", "application/json");
    std::lock_guard<std::mutex> lk(st.m);
    st.legacySession = sid;
    st.legacyVersion = ver;
    return sid;
}
} // namespace

int mcpBridgeMain(int port)
{
    std::string err;
    const std::string key = loadKey(false, err);
    if (key.empty())
    {
        std::fprintf(stderr, "oknx mcp: %s\n", err.c_str());
        return 1;
    }
    {
        httplib::Client probe("127.0.0.1", port);
        probe.set_connection_timeout(0, 500000);
        probe.set_read_timeout(2, 0);
        auto r = probe.Get("/api/health");
        if (!r)
        {
            std::fprintf(stderr, "oknx mcp: no oknx serve on 127.0.0.1:%d; start it with `oknx serve` (or `oknx serve --detach`)\n", port);
            return 1;
        }
    }
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    auto st = std::make_shared<State>();
    std::mutex& m = st->m;
    auto& inflight = st->inflight;
    std::string line;
    while (std::getline(std::cin, line))
    {
        if (line.empty()) continue;
        json msg = json::parse(line, nullptr, false);
        if (!msg.is_object())
        {
            emitError(json(), -32700, "Parse error");
            continue;
        }
        const json id = msg.contains("id") ? msg["id"] : json();
        if ((msg.contains("method") && !msg["method"].is_string()) || (msg.contains("params") && !msg["params"].is_object()))
        {
            emitError(id, -32600, "Invalid Request: method is a string, params an object");
            continue;
        }
        const std::string method = jget(msg, "method", "");
        const json params = jget<json>(msg, "params", json::object());
        const json meta = jget<json>(params, "_meta", json::object());
        const std::string rid = id.is_null() ? std::string() : id.dump();
        if (method == "notifications/cancelled")
        {
            std::lock_guard<std::mutex> lk(m);
            const std::string cid = params.contains("requestId") ? params["requestId"].dump() : std::string();
            auto it = inflight.find(cid);
            if (it != inflight.end()) // a cancel for a call that is not running any more has nothing to stop
            {
                st->cancelled.insert(cid);
                it->second->stop(); // modern: closing the stream is the cancel
            }
        }
        std::string ver = jget(meta, "io.modelcontextprotocol/protocolVersion", "");
        if (ver.empty() && method == "initialize") ver = jget(params, "protocolVersion", "");
        else if (ver.empty())
        {
            std::lock_guard<std::mutex> lk(m);
            ver = st->legacyVersion;
        }
        httplib::Headers h = {{"X-Oknx-Key", key}, {"X-Oknx-Caller", "oknx-mcp"}, {"Accept", "application/json, text/event-stream"}};
        if (!ver.empty() && method != "initialize") h.emplace("MCP-Protocol-Version", ver);
        if (!method.empty()) h.emplace("Mcp-Method", method);
        if (method == "tools/call" || method == "prompts/get") h.emplace("Mcp-Name", headerValue(jget(params, "name", "")));
        if (method == "resources/read") h.emplace("Mcp-Name", headerValue(jget(params, "uri", "")));
        const bool isInit = method == "initialize";
        std::string usedSession;
        {
            std::lock_guard<std::mutex> lk(m);
            if (isInit) st->initBody = line;
            if (!st->legacySession.empty() && !isInit)
            {
                usedSession = st->legacySession;
                h.emplace("Mcp-Session-Id", usedSession);
            }
        }
        auto cli = makeClient(port);
        if (!rid.empty())
        {
            std::lock_guard<std::mutex> lk(m);
            inflight[rid] = cli; // a client reusing a live id replaces the older call here
            st->cancelled.erase(rid);
        }
        const std::string body = line;
        std::thread([st, port, key, cli, h, body, id, rid, isInit, ver, usedSession] {
            std::shared_ptr<httplib::Client> c = cli;
            Reply r = postOnce(*c, h, body, id);
            if (r.ok && r.status == 404 && !usedSession.empty())
            {
                // serve was restarted and has forgotten the legacy session: a new one, then this call once more
                const std::string fresh = renewSession(*st, port, key, usedSession);
                auto c2 = makeClient(port);
                bool retry = !fresh.empty();
                {
                    // checked and swapped under one lock: a cancel after this stops the new connection
                    std::lock_guard<std::mutex> lk(st->m);
                    if (!rid.empty() && st->cancelled.count(rid) > 0) retry = false; // cancelled during the renewal
                    auto it = st->inflight.find(rid);
                    if (retry && !rid.empty() && it != st->inflight.end() && it->second == cli) it->second = c2;
                }
                if (retry)
                {
                    httplib::Headers h2;
                    for (const auto& kv : h)
                        if (kv.first != "Mcp-Session-Id") h2.emplace(kv.first, kv.second);
                    h2.emplace("Mcp-Session-Id", fresh);
                    c = c2;
                    r = postOnce(*c, h2, body, id);
                }
            }
            bool cancelled = false;
            {
                std::lock_guard<std::mutex> lk(st->m);
                cancelled = !rid.empty() && st->cancelled.count(rid) > 0;
            }
            if (cancelled)
            {
                // the client cancelled this call: it expects no answer, not even an error
            }
            else if (!r.ok)
            {
                if (!id.is_null()) emitError(id, -32603, std::string("oknx serve: ") + r.err);
            }
            else if (!r.sse && !r.whole.empty())
            {
                json j = json::parse(r.whole, nullptr, false);
                if (j.is_object() && j.contains("jsonrpc"))
                {
                    // serve refuses before it reads the body (401 after a rekey) with id null: the client needs its own
                    if (j.contains("error") && !id.is_null() && (!j.contains("id") || j["id"].is_null()))
                    {
                        j["id"] = id;
                        emit(j.dump());
                    }
                    else
                        emit(r.whole);
                    r.answered = true;
                }
                else if (!id.is_null()) // serve's own envelope (401 after a rekey, 403, ...): answer the id anyway
                {
                    std::string why = "HTTP " + std::to_string(r.status);
                    if (j.is_object() && j.contains("error") && j["error"].is_object())
                        why += ": " + jget(j["error"], "code", "") + " " + jget(j["error"], "reason", "");
                    emitError(id, -32603, "oknx serve answered " + why);
                    r.answered = true;
                }
            }
            if (r.ok && !r.answered && !id.is_null() && !cancelled) emitError(id, -32603, "oknx serve ended the call without an answer");
            if (isInit && !r.sessionHdr.empty())
            {
                std::lock_guard<std::mutex> lk(st->m);
                st->legacySession = r.sessionHdr;
                const json j = json::parse(r.whole, nullptr, false);
                st->legacyVersion = j.is_object() && j.contains("result") ? jget(j["result"], "protocolVersion", ver) : ver;
            }
            std::lock_guard<std::mutex> lk(st->m);
            if (!rid.empty())
            {
                auto it = st->inflight.find(rid);
                if (it != st->inflight.end() && it->second == c) st->inflight.erase(it);
                st->cancelled.erase(rid);
            }
        }).detach();
    }
    // stdin closed: wait for the calls still on their way (at most a few seconds), then leave
    for (int i = 0; i < 50; ++i)
    {
        {
            std::lock_guard<std::mutex> lk(m);
            if (inflight.empty()) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    bool left = false;
    {
        std::lock_guard<std::mutex> lk(m);
        left = !inflight.empty();
    }
    if (left) // a call thread still writes to stdout: leave without running destructors under it
    {
        std::fflush(stdout);
        std::_Exit(0);
    }
    return 0;
}

} // namespace serve
} // namespace ftc

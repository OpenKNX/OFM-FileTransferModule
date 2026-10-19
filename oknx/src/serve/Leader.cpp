/**
 * @file        Leader.cpp
 * @brief       The leader of `oknx serve`: HTTP on 127.0.0.1 and ::1, the guard, the allocation of connections,
 *              the workers, the shared monitors, the bench lock, reservations and sessions.
 * @details     Every operation passes the guard in this order: known, valid, rate, class allowed
 *              for the interface, target allowed, bench free, connection free. The first rule that refuses
 *              decides. Management operations of one interface run one after the other; a busmonitor and a
 *              management tunnel exclude each other on one interface (03_08_04 2.2.4).
 * @date        2026-10-19
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#include "core/Discovery.h" // winsock2 before windows.h

#include "serve/JsonGet.h"
#include "serve/Bench.h"
#include "serve/FrameJson.h"
#include "serve/Http.h"
#include "serve/Key.h"
#include "serve/Leader.h"
#include "serve/LeaderApi.h"
#include "serve/LeaderState.h"
#include "serve/Mdns.h"
#include "serve/Ops.h"
#include "serve/Paths.h"
#include "serve/Rules.h"
#include "serve/LeaderCompare.h"
#include "serve/Mcp.h"
#include "serve/Ui.h"
#include "cli/Prompt.h"
#include "cli/Term.h"
#include "cli/Theme.h"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#ifndef _WIN32
    #include <unistd.h>
#endif

namespace ftc
{
namespace serve
{

namespace
{
std::atomic<bool> g_stop{false};
#ifndef _WIN32
void onStopSignal(int) { g_stop = true; }
#else
std::atomic<bool> g_shutdownDone{false};
BOOL WINAPI onConsoleCtrl(DWORD ev)
{
    g_stop = true;
    // closing the console, log-off and shutdown end the process as soon as this returns: wait (Windows allows
    // about 5 s) until the workers had their bye, so the interfaces get their DISCONNECT
    if (ev == CTRL_CLOSE_EVENT || ev == CTRL_LOGOFF_EVENT || ev == CTRL_SHUTDOWN_EVENT)
        for (int i = 0; i < 45 && !g_shutdownDone; ++i) Sleep(100);
    return TRUE;
}
#endif

/** @brief Thread per connection with a cap: a long SSE stream never blocks another request. */
class ConnQueue : public httplib::TaskQueue
{
  public:
    explicit ConnQueue(std::atomic<int>& active, int cap) : _active(active), _cap(cap) {}
    bool enqueue(std::function<void()> fn) override
    {
        if (_active.fetch_add(1) >= _cap)
        {
            _active.fetch_sub(1);
            return false; // httplib closes the socket
        }
        std::atomic<int>* active = &_active; // the queue may be gone when a stream ends after shutdown()
        std::thread([active, fn = std::move(fn)] {
            fn();
            active->fetch_sub(1);
        }).detach();
        return true;
    }
    void shutdown() override
    {
        for (int i = 0; i < 100 && _active.load() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

  private:
    std::atomic<int>& _active;
    int _cap;
};

std::string isoNow() { return isoUtc(std::time(nullptr)); }

json redact(const OpDesc& d, const json& args)
{
    json r = args;
    for (const auto& s : d.secret)
        if (r.is_object() && r.contains(s)) r[s] = "***";
    return r;
}
} // namespace

/**********************************************************************
 ****************************** LEADER ******************************
 **********************************************************************/
class Leader : public LeaderApi
{
  public:
    Leader(const std::string& self, int port, const std::string& rulesPath, const std::string& lang,
           const std::string& version)
        : _self(selfExePath(self)), _port(port), _rulesPath(rulesPath), _lang(lang), _version(version)
    {
    }

    int run()
    {
        std::string err;
        _key = loadKey(true, err);
        if (_key.empty())
        {
            std::fprintf(stderr, "serve: %s\n", err.c_str());
            return 1;
        }
        if (!_rules.load(_rulesPath, err))
        {
            std::fprintf(stderr, "serve: %s: %s\n", _rulesPath.c_str(), err.c_str());
            return 1;
        }
        // One instance: if something answers on the port, say what it is and leave.
        {
            httplib::Client probe("127.0.0.1", _port);
            probe.set_connection_timeout(0, 300000);
            probe.set_read_timeout(0, 500000);
            auto r = probe.Get("/api/health");
            if (r)
            {
                json h = json::parse(r->body, nullptr, false);
                if (h.is_object() && h.value("name", std::string()) == "oknx serve")
                    std::fprintf(stderr, "serve: already running on 127.0.0.1:%d (version %s)\n", _port,
                                 h.value("version", std::string("?")).c_str());
                else
                    std::fprintf(stderr, "serve: port %d is taken by another program (%s)\n", _port,
                                 r->get_header_value("Server").empty() ? "unknown" : r->get_header_value("Server").c_str());
                return 1;
            }
        }
#ifdef _WIN32
        SetConsoleCtrlHandler(onConsoleCtrl, TRUE);
#else
        std::signal(SIGPIPE, SIG_IGN);
        std::signal(SIGINT, onStopSignal);
        std::signal(SIGTERM, onStopSignal);
#endif
        logLine("start", {{"port", _port}, {"rules", _rules.loaded() ? _rules.path() : std::string("(none)")}});
        setupServer(_svr4);
        setupServer(_svr6);
        if (!_svr4.bind_to_port("127.0.0.1", _port))
        {
            std::fprintf(stderr, "serve: cannot bind 127.0.0.1:%d\n", _port);
            return 1;
        }
        const bool v6 = _svr6.bind_to_port("::1", _port);
        std::thread t4([this] { _svr4.listen_after_bind(); });
        std::thread t6;
        if (v6) t6 = std::thread([this] { _svr6.listen_after_bind(); });
        std::thread bench([this] { benchLoop(); });
        std::thread wd([this] { watchdogLoop(); });
        std::printf("oknx serve %s on http://127.0.0.1:%d%s  ·  rules: %s  ·  Ctrl+C stops\n", _version.c_str(), _port,
                    v6 ? " and http://[::1]" : "", _rules.loaded() ? _rules.path().c_str() : "none (read-only defaults)");
        std::fflush(stdout);
        while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(100));

        std::printf("serve: stopping\n");
        std::fflush(stdout);
        shutdownAll();
#ifdef _WIN32
        g_shutdownDone = true; // the workers had their bye: a waiting console handler may return
#endif
        _svr4.stop();
        if (v6) _svr6.stop();
        t4.join();
        if (t6.joinable()) t6.join();
        bench.join();
        wd.join();
        logLine("stop", json::object());
        // detached threads (connections, operations, a worker reader's tail) may still run inside this object:
        // leave without running destructors under them; the log is written per line, the workers got their bye
        std::fflush(nullptr);
        std::_Exit(0);
    }

    // --- LeaderApi (used by compare, MCP, UI) --------------------------------------------------
    const std::string& lang() const override { return _lang; }
    const std::string& version() const override { return _version; }
    int port() const override { return _port; }
    json invoke(const std::string& name, const json& args, const Access& acc, ProgressFn progress,
                std::shared_ptr<std::atomic<bool>> cancel, bool wait) override
    {
        return runOperation(name, args, acc, std::move(progress), std::move(cancel), wait);
    }
    std::shared_ptr<Reader> openReader(const std::string& ip, uint16_t port, const std::string& mode, bool raw,
                                       const std::string& who, json& error) override
    {
        return attachReader(ifaceKey(ip, port), ip, port, mode, raw, who, error);
    }
    void closeReader(const std::string& ip, uint16_t port, const std::string& mode, const std::shared_ptr<Reader>& r) override
    {
        detachReader(ifaceKey(ip, port), mode, r);
    }
    json opsList() override
    {
        std::lock_guard<std::mutex> lk(_m);
        json a = json::array();
        for (auto& o : _ops) a.push_back(opSummary(*o.second));
        return a;
    }
    const Rules& rules() const override { return _rules; }
    std::string benchConfigPath() const override
    {
        for (const auto& s : _rules.sites())
            if (!s.benchConfig.empty()) return s.benchConfig;
        return std::string();
    }

  private:
    /*******************************************************************
     ***************************** HTTP ******************************
     *******************************************************************/
    void setupServer(httplib::Server& svr)
    {
        svr.new_task_queue = [this] { return new ConnQueue(_activeConns, 128); };
        // the port is ours alone: httplib's default SO_REUSEPORT would let a second serve bind it beside this one
        svr.set_socket_options([](socket_t sock) {
            int one = 1;
#ifdef _WIN32
            setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&one), sizeof(one));
#else
            setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const void*>(&one), sizeof(one));
#endif
        });
        svr.set_payload_max_length(64 * 1024);
        svr.set_read_timeout(10, 0);
        svr.set_write_timeout(10, 0);
        svr.set_keep_alive_max_count(100);
        svr.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
            baseHeaders(res);
            res.set_header("Server", "oknx-serve");
            if (!hostAllowed(req.get_header_value("Host"), _port))
            {
                sendJson(res, 403, errorEnvelope("GUARD_DENIED", "Host must be 127.0.0.1, [::1] or localhost with serve's port"));
                return httplib::Server::HandlerResponse::Handled;
            }
            const std::string sfs = lower(req.get_header_value("Sec-Fetch-Site"));
            if (!originAllowed(req.get_header_value("Origin"), _port) || sfs == "cross-site")
            {
                sendJson(res, 403, errorEnvelope("GUARD_DENIED", "a request from another web page is refused"));
                return httplib::Server::HandlerResponse::Handled;
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });

        svr.Get("/api/health", [this](const httplib::Request& req, httplib::Response& res) { health(req, res); });
        svr.Post(R"(/api/ops/([a-z_]+))", [this](const httplib::Request& req, httplib::Response& res) { postOp(req, res); });
        svr.Get(R"(/api/ops/(op-[0-9]+))", [this](const httplib::Request& req, httplib::Response& res) { getOp(req, res); });
        svr.Delete(R"(/api/ops/(op-[0-9]+))", [this](const httplib::Request& req, httplib::Response& res) { deleteOp(req, res); });
        svr.Get(R"(/api/ops/(op-[0-9]+)/events)", [this](const httplib::Request& req, httplib::Response& res) { opEvents(req, res); });
        svr.Get("/api/ops", [this](const httplib::Request& req, httplib::Response& res) {
            Access a;
            if (!auth(req, res, true, a)) return;
            sendJson(res, 200, {{"v", 1}, {"ok", true}, {"data", {{"ops", opsList()}}}});
        });
        svr.Get("/api/describe", [this](const httplib::Request& req, httplib::Response& res) {
            Access a;
            if (!auth(req, res, true, a)) return;
            sendJson(res, 200, {{"v", 1}, {"ok", true}, {"data", describeAll()}});
        });
        svr.Get("/api/monitor/stream", [this](const httplib::Request& req, httplib::Response& res) { monitorStream(req, res, false); });
        svr.Get("/stream", [this](const httplib::Request& req, httplib::Response& res) { monitorStream(req, res, true); });
        svr.Get("/discover", [this](const httplib::Request& req, httplib::Response& res) { viewerDiscover(req, res); });
        svr.Post("/api/lease", [this](const httplib::Request& req, httplib::Response& res) { postLease(req, res); });
        svr.Delete(R"(/api/lease/(L-[0-9a-f]+))", [this](const httplib::Request& req, httplib::Response& res) { deleteLease(req, res); });
        svr.Post("/api/serve/stop", [this](const httplib::Request& req, httplib::Response& res) {
            Access a;
            if (!auth(req, res, false, a)) return;
            sendJson(res, 200, {{"v", 1}, {"ok", true}});
            g_stop = true;
        });
        svr.Post("/api/serve/rekey", [this](const httplib::Request& req, httplib::Response& res) {
            Access a;
            if (!auth(req, res, false, a)) return;
            std::string err;
            const std::string k = newKey(err);
            if (k.empty())
            {
                sendJson(res, 500, errorEnvelope("INTERNAL", err));
                return;
            }
            {
                std::lock_guard<std::mutex> lk(_m);
                _key = k;
                _sessions.clear();
            }
            logLine("rekey", json::object());
            sendJson(res, 200, {{"v", 1}, {"ok", true}});
        });
        svr.Post("/api/session/link", [this](const httplib::Request& req, httplib::Response& res) { sessionLink(req, res); });
        svr.Post("/api/session/logout", [this](const httplib::Request& req, httplib::Response& res) {
            Access a;
            if (!auth(req, res, false, a)) return;
            std::lock_guard<std::mutex> lk(_m);
            const size_t n = _sessions.size();
            _sessions.clear();
            sendJson(res, 200, {{"v", 1}, {"ok", true}, {"data", {{"ended", n}}}});
        });
        svr.Get("/ui/login", [this](const httplib::Request& req, httplib::Response& res) { uiLogin(req, res); });
        svr.Get(R"(/ui(/.*)?)", [this](const httplib::Request& req, httplib::Response& res) { uiPage(req, res); });
        svr.Post("/mcp", [this](const httplib::Request& req, httplib::Response& res) { _mcp.post(*this, req, res, keyAccess(req)); });
        svr.Get("/mcp", [this](const httplib::Request& req, httplib::Response& res) { _mcp.get(*this, req, res, keyAccess(req)); });
        svr.Delete("/mcp", [this](const httplib::Request& req, httplib::Response& res) { _mcp.del(*this, req, res, keyAccess(req)); });
    }

    /** @brief Key or (with @p allowSession) a browser session; otherwise 401 and false. */
    bool auth(const httplib::Request& req, httplib::Response& res, bool allowSession, Access& a)
    {
        a = keyAccess(req);
        if (a.kind == Access::Key) return true;
        if (allowSession && sessionAccess(req, a)) return true;
        sendJson(res, 401, errorEnvelope("GUARD_DENIED", "this needs the key of serve.key in X-Oknx-Key"));
        return false;
    }
    Access keyAccess(const httplib::Request& req)
    {
        Access a;
        std::string key;
        {
            std::lock_guard<std::mutex> lk(_m);
            key = _key;
        }
        if (sameSecret(req.get_header_value("X-Oknx-Key"), key))
        {
            a.kind = Access::Key;
            a.id = "key";
            a.caller = req.get_header_value("X-Oknx-Caller").substr(0, 64);
        }
        return a;
    }
    bool sessionAccess(const httplib::Request& req, Access& a, bool topLevel = false)
    {
        const std::string sfs = lower(req.get_header_value("Sec-Fetch-Site"));
        if (!(sfs == "same-origin" || (topLevel && sfs == "none"))) return false;
        const std::string sid = cookieValue(req.get_header_value("Cookie"), "oknx_session");
        if (sid.empty()) return false;
        std::lock_guard<std::mutex> lk(_m);
        auto it = _sessions.find(sid);
        if (it == _sessions.end()) return false;
        const uint64_t now = monoMs();
        if (now - it->second.created > 12ull * 3600 * 1000 || now - it->second.lastUse > 3600ull * 1000)
        {
            _sessions.erase(it);
            return false;
        }
        it->second.lastUse = now;
        a.kind = Access::Session;
        a.id = "session:" + sid.substr(0, 8);
        a.caller = "browser";
        return true;
    }

    void health(const httplib::Request& req, httplib::Response& res)
    {
        json h = {{"name", "oknx serve"}, {"version", _version}, {"ok", true}};
        Access a = keyAccess(req);
        if (a.kind == Access::Key)
        {
            std::lock_guard<std::mutex> lk(_m);
            h["pid"] = (long)selfPid();
            h["port"] = _port;
            h["rules"] = {{"path", _rules.path()}, {"loaded", _rules.loaded()}};
            json ws = json::array();
            for (auto& w : _workers)
                ws.push_back({{"iface", w.first}, {"alive", w.second->alive.load()}, {"pid", w.second->child.pidValue()},
                              {"since_ms", monoMs() - w.second->since}});
            h["workers"] = ws;
            h["leases"] = leasesJsonLocked();
            h["bench"] = _bench.toJson();
            h["released"] = _released;
            h["connections"] = connectionsJsonLocked();
        }
        sendJson(res, 200, h);
    }

    json describeAll()
    {
        json a = json::array();
        for (const auto& d : opTable())
            a.push_back({{"name", d.name},
                         {"title", _lang == "de" ? d.titleDe : d.titleEn},
                         {"help", _lang == "de" ? d.helpDe : d.helpEn},
                         {"class", opClassName(d.cls)},
                         {"runs_in", d.where == OpWhere::Worker ? "worker" : "leader"},
                         {"input", d.input},
                         {"timeout_ms", d.timeoutMs},
                         {"progress", d.progress},
                         {"changes", d.changes}});
        return a;
    }

    /*******************************************************************
     ************************** OPERATIONS ***************************
     *******************************************************************/
    void postOp(const httplib::Request& req, httplib::Response& res)
    {
        Access a;
        if (!auth(req, res, true, a)) return;
        const std::string name = req.matches[1];
        json args = req.body.empty() ? json::object() : json::parse(req.body, nullptr, false);
        if (args.is_discarded())
        {
            sendJson(res, 400, errorEnvelope("INVALID_ARGUMENT", "the body is not JSON"));
            return;
        }
        const OpDesc* d = findOp(name);
        // Long operations answer at once with their id; the result comes over /api/ops/<id>[/events].
        const bool wait = !(d && d->progress) || req.get_param_value("wait") == "1";
        json env = runOperation(name, args, a, nullptr, nullptr, wait);
        sendJson(res, statusFor(env), env);
    }

    static int statusFor(const json& env)
    {
        if (env.value("ok", false)) return env.contains("accepted") ? 202 : 200;
        const std::string c = env["error"].value("code", std::string());
        if (c == "INVALID_ARGUMENT") return 400;
        if (c == "GUARD_DENIED") return 403;
        if (c == "RATE_LIMITED") return 429;
        if (c == "BENCH_LOCKED" || c == "BUSY") return 409;
        if (c == "TIMEOUT" || c == "NO_ANSWER") return 504;
        if (c == "CONNECT_FAILED") return 502;
        if (c == "CANCELLED") return 499;
        return 500;
    }

    void getOp(const httplib::Request& req, httplib::Response& res)
    {
        Access a;
        if (!auth(req, res, true, a)) return;
        auto op = findOpRec(req.matches[1]);
        if (!op)
        {
            sendJson(res, 404, errorEnvelope("INVALID_ARGUMENT", "no such operation (results are kept 15 minutes)"));
            return;
        }
        std::lock_guard<std::mutex> lk(op->m);
        if (!op->done)
        {
            sendJson(res, 200, {{"v", 1}, {"ok", true}, {"op", op->name}, {"id", op->id}, {"running", true},
                                {"events", op->events.size()}});
            return;
        }
        sendJson(res, statusFor(op->result), op->result);
    }

    void deleteOp(const httplib::Request& req, httplib::Response& res)
    {
        Access a;
        if (!auth(req, res, true, a)) return;
        auto op = findOpRec(req.matches[1]);
        if (!op)
        {
            sendJson(res, 404, errorEnvelope("INVALID_ARGUMENT", "no such operation"));
            return;
        }
        cancelOp(*op, "cancelled by the caller");
        sendJson(res, 200, {{"v", 1}, {"ok", true}, {"id", op->id}, {"cancelled", true}});
    }

    /** @brief SSE of one operation: every progress event, then the result. */
    void opEvents(const httplib::Request& req, httplib::Response& res)
    {
        Access a;
        if (!auth(req, res, true, a)) return;
        auto op = findOpRec(req.matches[1]);
        if (!op)
        {
            sendJson(res, 404, errorEnvelope("INVALID_ARGUMENT", "no such operation"));
            return;
        }
        res.set_header("Cache-Control", "no-store");
        auto cursor = std::make_shared<size_t>(0);
        auto lastWrite = std::make_shared<uint64_t>(monoMs());
        res.set_chunked_content_provider("text/event-stream", guardedProvider([op, cursor, lastWrite](size_t, httplib::DataSink& sink) {
            if (g_stop) return false;
            std::vector<json> evs;
            bool done = false;
            json result;
            {
                std::unique_lock<std::mutex> lk(op->m);
                op->cv.wait_for(lk, std::chrono::milliseconds(500), [&] { return op->done || op->events.size() > *cursor; });
                while (*cursor < op->events.size()) evs.push_back(op->events[(*cursor)++]);
                done = op->done;
                if (done) result = op->result;
            }
            std::string out;
            const auto R = json::error_handler_t::replace;
            for (const auto& e : evs) out += "event: progress\ndata: " + e.dump(-1, ' ', false, R) + "\n\n";
            if (done) out += "event: result\ndata: " + result.dump(-1, ' ', false, R) + "\n\n";
            if (out.empty() && monoMs() - *lastWrite >= 15000) out = ": ping\n\n";
            if (!out.empty())
            {
                if (!sink.write(out.data(), out.size())) return false;
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

    /**
     * @brief The one path every operation takes, from HTTP and from MCP: guard, allocation, run.
     * @param wait false = return at once with the id (202), the result follows on /api/ops/<id>.
     */
    json runOperation(const std::string& name, const json& args, const Access& acc, ProgressFn progress,
                      std::shared_ptr<std::atomic<bool>> extCancel, bool wait)
    {
        json env = runOperationInner(name, args, acc, std::move(progress), std::move(extCancel), wait);
        // a refusal before any operation existed is logged here; the others in finishOp
        if (!env.contains("id"))
        {
            const OpDesc* d = findOp(name);
            logLine("op", {{"op", name},
                           {"access", acc.id},
                           {"caller", acc.caller},
                           {"args", d ? redact(*d, args) : json("(unknown operation)")},
                           {"code", env.value("error", json::object()).value("code", std::string())},
                           {"ms", 0}});
        }
        return env;
    }

    json runOperationInner(const std::string& name, const json& args, const Access& acc, ProgressFn progress,
                           std::shared_ptr<std::atomic<bool>> extCancel, bool wait)
    {
        // 1 known
        const OpDesc* d = findOp(name);
        if (!d) return errorEnvelope("GUARD_DENIED", "unknown operation", {{"rule", "known"}, {"op", name}});
        // 2 valid
        std::string where, why;
        if (!checkSchema(d->input, args, "", where, why))
            return errorEnvelope("INVALID_ARGUMENT", why, {{"at", where}, {"op", name}});
        // 3 rate
        if (!rateOk(acc.id))
            return errorEnvelope("RATE_LIMITED", "too many calls in one minute",
                                 {{"retry_after_ms", 1000}, {"limit_per_minute", _rules.ratePerMinute()}});
        const std::string ip = args.value("ip", std::string());
        const uint16_t port = (uint16_t)args.value("port", 3671);
        const std::string ref = args.value("ref", std::string());
        const uint16_t refPort = (uint16_t)args.value("ref_port", (int)port);
        // 4 + 5 class and target per interface
        for (const auto& i : {std::make_pair(ip, port), std::make_pair(ref, refPort)})
        {
            if (i.first.empty()) continue;
            const std::string r = _rules.check(*d, args, i.first, i.second);
            if (!r.empty()) return errorEnvelope("GUARD_DENIED", r, {{"rule", r}, {"op", name}, {"ip", i.first}});
        }
        // 6 bench free
        const OpClass cls = classFor(*d, args);
        if (cls != OpClass::Passive)
        {
            std::lock_guard<std::mutex> lk(_m);
            for (const std::string& i : {ip, ref})
                if (!i.empty() && _bench.valid && _bench.names(i))
                    return errorEnvelope("BENCH_LOCKED", "a suite run holds the bench",
                                         {{"run", _bench.run}, {"since", _bench.start}, {"heartbeat_age_s", _bench.heartbeatAgeS}});
        }

        // suite_run names no interface: it needs its own permission in the rules
        if (name == "suite_run")
        {
            const RuleSite* site = nullptr;
            for (const auto& s : _rules.sites())
                if (s.suiteRun) site = &s;
            if (!site) return errorEnvelope("GUARD_DENIED", "no site of the rules allows suite_run", {{"rule", "suite_run"}});
            std::lock_guard<std::mutex> lk(_m);
            if (_bench.valid)
                return errorEnvelope("BENCH_LOCKED", "a suite run holds the bench", {{"run", _bench.run}, {"since", _bench.start}});
        }
        json fullArgs = args;
        if (name == "ftc_login")
        {
            // the password never comes over MCP or the API: serve's own environment, else its own terminal
            std::string why;
            const std::string pw = ftcPassword(args.value("pa", std::string()), acc, why);
            if (pw.empty()) return errorEnvelope("INVALID_ARGUMENT", why, {{"op", name}});
            fullArgs["password"] = pw;
        }

        auto op = std::make_shared<OpRec>();
        op->name = name;
        op->argsFull = fullArgs;
        op->args = redact(*d, args);
        op->access = acc;
        op->iface = ip.empty() ? std::string() : ifaceKey(ip, port);
        op->submitted = monoMs();
        op->timeoutMs = d->timeoutMs;
        op->onWorker = d->where == OpWhere::Worker;
        if (extCancel) op->cancel = extCancel;
        {
            std::lock_guard<std::mutex> lk(_m);
            op->id = "op-" + std::to_string(++_opSeq);
            _ops[op->id] = op;
            pruneOpsLocked();
        }
        if (progress)
        {
            // MCP: mirror every progress event to the caller; the op keeps its own copy for SSE.
            std::thread([op, progress] {
                size_t cur = 0;
                for (;;)
                {
                    std::vector<json> evs;
                    bool done = false;
                    {
                        std::unique_lock<std::mutex> lk(op->m);
                        op->cv.wait_for(lk, std::chrono::milliseconds(1000), [&] { return op->done || op->events.size() > cur; });
                        while (cur < op->events.size()) evs.push_back(op->events[cur++]);
                        done = op->done;
                    }
                    try
                    {
                        for (const auto& e : evs) progress(e);
                    }
                    catch (const std::exception&) // a progress event of an unexpected shape costs that event only
                    {
                    }
                    if (done) return;
                }
            }).detach();
        }

        // 7 connection free + run
        if (d->where == OpWhere::Worker)
        {
            json busy;
            if (!enqueueWorkerOp(op, ip, port, busy))
            {
                finishOp(*op, busy);
                return op->result;
            }
        }
        else
        {
            std::thread([this, op, d] {
                op->started = monoMs();
                json env;
                try
                {
                    env = runLeaderOp(*d, *op);
                }
                catch (const std::exception& e) // a device answer of an unexpected shape ends this operation, not serve
                {
                    env = errorEnvelope("INTERNAL", std::string("the operation failed: ") + e.what());
                }
                finishOp(*op, env);
            }).detach();
        }
        if (!wait) return {{"v", 1}, {"ok", true}, {"op", name}, {"id", op->id}, {"accepted", true}};
        op->waitDone((uint64_t)d->timeoutMs + 15000);
        if (!op->done) finishOp(*op, errorEnvelope("TIMEOUT", "no result inside the time limit", {{"timeout_ms", d->timeoutMs}}));
        std::lock_guard<std::mutex> lk(op->m);
        return op->result;
    }

    void finishOp(OpRec& op, json env)
    {
        if (!env.contains("v")) env["v"] = 1;
        env["op"] = op.name;
        env["id"] = op.id;
        env["ms"] = monoMs() - (op.started ? op.started : op.submitted);
        op.finish(env);
        // the next queued management operation of this interface may start now
        if (op.onWorker) dispatchNext(op.iface);
        const json err = env.value("error", json::object());
        logLine("op", {{"id", op.id},
                       {"op", op.name},
                       {"access", op.access.id},
                       {"caller", op.access.caller},
                       {"args", op.args},
                       {"code", env.value("ok", false) ? std::string("OK") : err.value("code", std::string())},
                       {"ms", env["ms"]}});
    }

    void cancelOp(OpRec& op, const std::string& why)
    {
        *op.cancel = true;
        if (op.onWorker)
        {
            std::shared_ptr<WorkerConn> w;
            bool queued = false;
            {
                std::lock_guard<std::mutex> lk(_m);
                auto it = _workers.find(op.iface);
                if (it != _workers.end()) w = it->second;
                auto& q = _mgmtQueue[op.iface];
                for (auto qi = q.begin(); qi != q.end(); ++qi)
                    if ((*qi)->id == op.id)
                    {
                        q.erase(qi);
                        queued = true;
                        break;
                    }
            }
            if (queued)
            {
                finishOp(op, errorEnvelope("CANCELLED", why));
                return;
            }
            if (w) w->send({{"t", "cancel"}, {"id", op.id}});
        }
    }

    std::shared_ptr<OpRec> findOpRec(const std::string& id)
    {
        std::lock_guard<std::mutex> lk(_m);
        auto it = _ops.find(id);
        return it == _ops.end() ? nullptr : it->second;
    }

    void pruneOpsLocked()
    {
        const uint64_t now = monoMs();
        for (auto it = _ops.begin(); it != _ops.end();)
        {
            bool drop = false;
            {
                std::lock_guard<std::mutex> lk(it->second->m);
                drop = it->second->done && now - it->second->ended > 15ull * 60 * 1000;
            }
            it = drop ? _ops.erase(it) : std::next(it);
        }
        // the last 100 finished stay even past 15 minutes; older ones beyond 100 go
        size_t finished = 0;
        for (auto it = _ops.rbegin(); it != _ops.rend(); ++it)
            if (it->second->done) finished++;
        if (finished <= 100) return;
        for (auto it = _ops.begin(); it != _ops.end() && finished > 100;)
        {
            if (it->second->done)
            {
                it = _ops.erase(it);
                finished--;
            }
            else
                ++it;
        }
    }

    json opSummary(OpRec& o)
    {
        std::lock_guard<std::mutex> lk(o.m);
        json j = {{"id", o.id}, {"op", o.name}, {"args", o.args}, {"access", o.access.id}, {"iface", o.iface},
                  {"running", !o.done}, {"age_ms", monoMs() - o.submitted}};
        if (o.done) j["code"] = o.result.value("ok", false) ? std::string("OK") : o.result["error"].value("code", std::string());
        return j;
    }

    bool rateOk(const std::string& who)
    {
        std::lock_guard<std::mutex> lk(_m);
        auto& q = _rate[who.empty() ? std::string("none") : who];
        const uint64_t now = monoMs();
        while (!q.empty() && now - q.front() >= 60000) q.pop_front();
        if ((int)q.size() >= _rules.ratePerMinute()) return false;
        q.push_back(now);
        return true;
    }

    /*******************************************************************
     ************************ WORKER OPERATIONS ***********************
     *******************************************************************/
    /** @brief Allocation for a management operation, then queue it on its interface. */
    bool enqueueWorkerOp(const std::shared_ptr<OpRec>& op, const std::string& ip, uint16_t port, json& busy)
    {
        const std::string key = ifaceKey(ip, port);
        // A busmonitor and a management tunnel exclude each other: one with readers wins, one without goes.
        std::shared_ptr<SharedMon> bm = findMon(key, "bm");
        if (bm)
        {
            if (bm->readerCount() > 0)
            {
                busy = errorEnvelope("BUSY", "a busmonitor with readers holds this interface",
                                     {{"holder", "serve-busmon"}, {"readers", bm->readerCount()}, {"since_ms", monoMs() - bm->since}});
                return false;
            }
            stopMonitor(key, "bm", "CANCELLED");
        }
        {
            std::lock_guard<std::mutex> lk(_m);
            for (auto& l : _leases)
                for (const auto& it : l.second->items)
                    if (ifaceKey(it.ip, it.port) == key && it.kind == "bm")
                    {
                        busy = errorEnvelope("BUSY", "the command line holds a busmonitor here",
                                             {{"holder", "cli:" + std::to_string(l.second->pid)}, {"since_ms", monoMs() - l.second->since}});
                        return false;
                    }
        }
        json dev;
        if (deviceBusmonForeign(ip, dev))
        {
            busy = errorEnvelope("BUSY", "the interface reports a busmonitor that is not serve's",
                                 {{"holder", "device-busmon:" + dev.value("owner", std::string("unknown"))}});
            return false;
        }
        bool start = false;
        {
            std::lock_guard<std::mutex> lk(_m);
            auto& q = _mgmtQueue[key];
            q.push_back(op);
            start = _mgmtRunning[key].empty() && q.size() == 1;
        }
        if (start) dispatchNext(key);
        return true;
    }

    /** @brief Start the next queued management operation of @p key, if none runs. */
    void dispatchNext(const std::string& key)
    {
        std::shared_ptr<OpRec> op;
        {
            std::lock_guard<std::mutex> lk(_m);
            auto& running = _mgmtRunning[key];
            if (!running.empty())
            {
                auto it = _ops.find(running);
                bool stillRunning = false;
                if (it != _ops.end())
                {
                    std::lock_guard<std::mutex> lk2(it->second->m);
                    stillRunning = !it->second->done;
                }
                if (stillRunning) return;
                running.clear();
            }
            auto& q = _mgmtQueue[key];
            while (!q.empty() && q.front()->done) q.pop_front();
            if (q.empty()) return;
            op = q.front();
            q.pop_front();
            running = op->id;
        }
        const size_t colon = key.rfind(':');
        const std::string ip = key.substr(0, colon);
        const uint16_t port = (uint16_t)std::atoi(key.substr(colon + 1).c_str());
        json err;
        auto w = ensureWorker(ip, port, err);
        if (!w)
        {
            {
                std::lock_guard<std::mutex> lk(_m);
                _mgmtRunning[key].clear();
            }
            finishOp(*op, errorEnvelope("INTERNAL", "the worker for this interface did not start", err));
            return;
        }
        op->started = monoMs();
        json args = op->argsFull;
        if (!w->send({{"t", "job"}, {"id", op->id}, {"op", op->name}, {"args", args}}))
            finishOp(*op, errorEnvelope("INTERNAL", "the worker pipe is closed"));
    }

    /*******************************************************************
     ***************************** WORKERS ****************************
     *******************************************************************/
    std::shared_ptr<WorkerConn> ensureWorker(const std::string& ip, uint16_t port, json& err)
    {
        const std::string key = ifaceKey(ip, port);
        std::shared_ptr<WorkerConn> w;
        {
            std::lock_guard<std::mutex> lk(_m);
            auto it = _workers.find(key);
            if (it != _workers.end() && it->second->alive) w = it->second;
            if (!w)
            {
                w = std::make_shared<WorkerConn>();
                w->ip = ip;
                w->port = port;
                std::string serr;
                if (!spawnChild(_self, {"--_worker", "-i", ip, "--port", std::to_string(port)}, workerLogPath(ip, port),
                                w->child, serr))
                {
                    err = {{"reason", serr}};
                    return nullptr;
                }
                w->alive = true;
                w->since = monoMs();
                _workers[key] = w;
                w->reader = std::thread([this, w] { workerReader(w); });
                w->reader.detach();
                logLine("worker", {{"iface", key}, {"pid", w->child.pidValue()}});
            }
        }
        std::unique_lock<std::mutex> lk(w->hm);
        if (!w->hcv.wait_for(lk, std::chrono::seconds(5), [&] { return w->ready.load() || !w->alive.load(); }) || !w->alive)
        {
            err = {{"reason", "no hello from the worker"}};
            return nullptr;
        }
        return w;
    }

    /** @brief One event line of the worker of @p key. */
    void workerLine(const std::shared_ptr<WorkerConn>& w, const std::string& key, const json& m)
    {
        const std::string t = m.value("t", std::string());
        if (t == "hello")
        {
            {
                std::lock_guard<std::mutex> lk(w->hm);
                w->hello = m;
                w->ready = true;
            }
            w->hcv.notify_all();
            if (m.contains("released") && !m["released"].empty()) logLine("released", {{"iface", key}, {"conns", m["released"]}});
        }
        else if (t == "result")
        {
            auto op = findOpRec(m.value("id", std::string()));
            if (op)
            {
                json env = {{"ok", m.value("ok", false)}};
                if (m.contains("data")) env["data"] = m["data"];
                if (m.contains("error")) env["error"] = m["error"];
                if (op->timedOut && !env["ok"].get<bool>())
                    env["error"] = {{"code", "TIMEOUT"}, {"reason", "the operation ran past its time limit"},
                                    {"detail", {{"timeout_ms", op->timeoutMs}}}};
                finishOp(*op, env);
            }
        }
        else if (t == "progress")
        {
            auto op = findOpRec(m.value("id", std::string()));
            if (op) op->addEvent(m.value("data", json::object()));
        }
        else if (t == "frame" || t == "lost" || t == "mon")
            monitorEvent(key, m);
        else if (t == "mgmt_closed")
        {
            {
                std::lock_guard<std::mutex> lk(w->hm);
                w->closed.insert(m.value("id", std::string()));
            }
            w->hcv.notify_all();
        }
    }

    void workerReader(std::shared_ptr<WorkerConn> w)
    {
        const std::string key = ifaceKey(w->ip, w->port);
        LineReader rd(w->child.fromChild);
        std::string line;
        while (rd.next(line))
        {
            json m = json::parse(line, nullptr, false);
            if (!m.is_object()) continue;
            try
            {
                workerLine(w, key, m);
            }
            catch (const json::exception& e) // a line of an unexpected shape costs that line, never serve
            {
                logLine("worker_line", {{"iface", key}, {"error", e.what()}});
            }
        }
        // the pipe closed: the worker is gone
        w->alive = false;
        w->hcv.notify_all();
        w->child.wait();
        pipeClose(w->child.toChild);
        pipeClose(w->child.fromChild);
        logLine("worker_end", {{"iface", key}});
        std::vector<std::shared_ptr<OpRec>> lost;
        {
            std::lock_guard<std::mutex> lk(_m);
            auto it = _workers.find(key);
            if (it != _workers.end() && it->second == w) _workers.erase(it);
            auto& running = _mgmtRunning[key];
            if (!running.empty())
            {
                auto o = _ops.find(running);
                if (o != _ops.end()) lost.push_back(o->second);
            }
        }
        for (auto& o : lost) finishOp(*o, errorEnvelope("INTERNAL", "the worker of this interface ended during the operation"));
        for (const char* mode : {"gm", "bm"})
        {
            auto mon = findMon(key, mode);
            if (mon) endMonitor(key, mode, "worker_ended");
        }
    }

    /*******************************************************************
     ************************ SHARED MONITORS ************************
     *******************************************************************/
    std::shared_ptr<SharedMon> findMon(const std::string& key, const std::string& mode)
    {
        std::lock_guard<std::mutex> lk(_m);
        auto it = _mons.find(key + "/" + mode);
        return it == _mons.end() ? nullptr : it->second;
    }

    void monitorEvent(const std::string& key, const json& m)
    {
        std::string s = m.value("s", std::string());
        const bool raw = s.size() > 4 && s.compare(s.size() - 4, 4, ".raw") == 0;
        const std::string mode = raw ? s.substr(0, s.size() - 4) : s;
        auto mon = findMon(key, mode);
        if (!mon) return;
        const std::string t = m.value("t", std::string());
        if (t == "frame")
        {
            if (!raw) mon->frames++;
            mon->broadcast(m, raw);
            return;
        }
        if (t == "lost")
        {
            mon->lostPipe += m.value("count", (uint64_t)0);
            mon->broadcastAll(m);
            return;
        }
        const std::string ev = m.value("ev", std::string());
        {
            std::lock_guard<std::mutex> lk(mon->m);
            if (ev == "up") mon->state = SharedMon::Up;
            else if (ev == "down")
                mon->state = SharedMon::Down;
            else if (ev == "fail")
            {
                mon->state = SharedMon::Failed;
                mon->failInfo = m;
            }
            else if (ev == "end")
                mon->state = SharedMon::Ended;
        }
        mon->cv.notify_all();
        mon->broadcastAll(m);
        if (ev == "fail" || ev == "end")
        {
            std::vector<std::shared_ptr<Reader>> rs;
            {
                std::lock_guard<std::mutex> lk(mon->m);
                rs = mon->readers;
            }
            for (auto& r : rs) r->close(ev == "fail" ? m.value("code", std::string("CONNECT_FAILED")) : "ended");
            std::lock_guard<std::mutex> lk(_m);
            auto it = _mons.find(key + "/" + mode);
            if (it != _mons.end() && it->second == mon) _mons.erase(it);
        }
    }

    /** @brief A reader on the shared monitor @p mode of an interface; starts the monitor when it is the first. */
    std::shared_ptr<Reader> attachReader(const std::string& key, const std::string& ip, uint16_t port, const std::string& mode,
                                         bool raw, const std::string& who, json& error)
    {
        if (mode != "gm" && mode != "bm")
        {
            error = errorEnvelope("INVALID_ARGUMENT", "mode is gm or bm");
            return nullptr;
        }
        {
            std::lock_guard<std::mutex> lk(_m);
            if (_bench.valid && _bench.names(ip))
            {
                error = errorEnvelope("BENCH_LOCKED", "a suite run holds the bench", {{"run", _bench.run}});
                return nullptr;
            }
            for (auto& l : _leases)
                for (const auto& it : l.second->items)
                    if (ifaceKey(it.ip, it.port) == key && (it.kind == "bm" || mode == "bm"))
                    {
                        error = errorEnvelope("BUSY", "the command line holds this interface",
                                              {{"holder", "cli:" + std::to_string(l.second->pid)}, {"kind", it.kind}});
                        return nullptr;
                    }
        }
        const std::string other = mode == "bm" ? "gm" : "bm";
        auto otherMon = findMon(key, other);
        if (otherMon) // a busmonitor excludes every other tunnel on its interface
        {
            if (otherMon->readerCount() > 0)
            {
                error = errorEnvelope("BUSY", other == "bm" ? "a busmonitor with readers holds this interface"
                                                            : "a group monitor with readers holds this interface",
                                      {{"holder", other == "bm" ? "serve-busmon" : "serve-groupmon"}, {"readers", otherMon->readerCount()}});
                return nullptr;
            }
            stopMonitor(key, other, "CANCELLED");
        }
        if (mode == "bm")
        {
            std::lock_guard<std::mutex> lk(_m);
            if (!_mgmtRunning[key].empty() || !_mgmtQueue[key].empty())
            {
                error = errorEnvelope("BUSY", "a management operation runs on this interface",
                                      {{"holder", "serve-op:" + (_mgmtRunning[key].empty() ? _mgmtQueue[key].front()->id : _mgmtRunning[key])}});
                return nullptr;
            }
        }
        auto r = std::make_shared<Reader>();
        r->raw = raw;
        r->who = who;
        r->since = monoMs();
        std::shared_ptr<SharedMon> mon;
        bool fresh = false;
        {
            std::lock_guard<std::mutex> lk(_m);
            r->id = ++_readerSeq;
            auto& slot = _mons[key + "/" + mode];
            if (!slot)
            {
                slot = std::make_shared<SharedMon>();
                slot->iface = key;
                slot->mode = mode;
                slot->since = monoMs();
                fresh = true;
            }
            mon = slot;
        }
        {
            std::lock_guard<std::mutex> lk(mon->m);
            mon->readers.push_back(r);
            mon->lingerUntil = 0;
        }
        if (fresh || (raw && !mon->rawRequested))
        {
            if (fresh && mode == "bm")
            {
                json dev;
                if (deviceBusmonForeign(ip, dev))
                {
                    endMonitor(key, mode, "BUSY");
                    error = errorEnvelope("BUSY", "the interface reports a busmonitor that is not serve's",
                                          {{"holder", "device-busmon:" + dev.value("owner", std::string("unknown"))}});
                    return nullptr;
                }
            }
            json werr;
            auto w = ensureWorker(ip, port, werr);
            if (!w)
            {
                endMonitor(key, mode, "INTERNAL");
                error = errorEnvelope("INTERNAL", "the worker for this interface did not start", werr);
                return nullptr;
            }
            if (raw) mon->rawRequested = true;
            w->send({{"t", "mon"}, {"act", "start"}, {"mode", mode}, {"raw", mon->rawRequested}});
        }
        // wait for the monitor to come up (or fail)
        std::unique_lock<std::mutex> lk(mon->m);
        mon->cv.wait_for(lk, std::chrono::seconds(6), [&] { return mon->state != SharedMon::Starting; });
        if (mon->state == SharedMon::Failed || mon->state == SharedMon::Starting)
        {
            const json fi = mon->failInfo;
            lk.unlock();
            detachReader(key, mode, r);
            const std::string code = fi.value("code", std::string("CONNECT_FAILED"));
            error = errorEnvelope(code == "BUSY" ? "BUSY" : "CONNECT_FAILED",
                                  mon->state == SharedMon::Starting ? std::string("the monitor did not come up")
                                                                    : fi.value("reason", std::string("the interface refused the tunnel")),
                                  fi);
            return nullptr;
        }
        return r;
    }

    void detachReader(const std::string& key, const std::string& mode, const std::shared_ptr<Reader>& r)
    {
        if (r) r->close("ended");
        auto mon = findMon(key, mode);
        if (!mon) return;
        std::lock_guard<std::mutex> lk(mon->m);
        mon->readers.erase(std::remove(mon->readers.begin(), mon->readers.end(), r), mon->readers.end());
        if (mon->readers.empty()) mon->lingerUntil = monoMs() + 3000; // stays 3 s for a quick next reader
    }

    /** @brief End a shared monitor with all its readers (they get @p code) and stop it in the worker. */
    void stopMonitor(const std::string& key, const std::string& mode, const std::string& code)
    {
        auto mon = findMon(key, mode);
        if (!mon) return;
        std::shared_ptr<WorkerConn> w;
        {
            std::lock_guard<std::mutex> lk(_m);
            auto it = _workers.find(key);
            if (it != _workers.end()) w = it->second;
        }
        {
            std::lock_guard<std::mutex> lk(mon->m);
            for (auto& r : mon->readers) r->close(code);
        }
        if (w && w->alive)
        {
            w->send({{"t", "mon"}, {"act", "stop"}, {"mode", mode}});
            std::unique_lock<std::mutex> lk(mon->m);
            mon->cv.wait_for(lk, std::chrono::seconds(3), [&] { return mon->state == SharedMon::Ended || mon->state == SharedMon::Failed; });
        }
        std::lock_guard<std::mutex> lk(_m);
        auto it = _mons.find(key + "/" + mode);
        if (it != _mons.end() && it->second == mon) _mons.erase(it);
    }

    /** @brief Forget a monitor whose worker is gone or that never came up. */
    void endMonitor(const std::string& key, const std::string& mode, const std::string& code)
    {
        auto mon = findMon(key, mode);
        if (!mon) return;
        {
            std::lock_guard<std::mutex> lk(mon->m);
            for (auto& r : mon->readers) r->close(code);
            mon->state = SharedMon::Ended;
        }
        mon->cv.notify_all();
        std::lock_guard<std::mutex> lk(_m);
        auto it = _mons.find(key + "/" + mode);
        if (it != _mons.end() && it->second == mon) _mons.erase(it);
    }

    /** @brief SSE of a shared monitor: decoded (/api/monitor/stream) or the viewer's raw cEMI (/stream). */
    void monitorStream(const httplib::Request& req, httplib::Response& res, bool viewer)
    {
        Access a;
        if (!auth(req, res, true, a)) return;
        const std::string ip = req.get_param_value("ip");
        const uint16_t port = (uint16_t)(req.has_param("port") ? std::atoi(req.get_param_value("port").c_str()) : 3671);
        std::string mode = req.get_param_value("mode");
        if (viewer)
        {
            const std::string layer = req.get_param_value("layer");
            mode = layer == "link" ? "gm" : "bm";
        }
        if (!isIpv4(ip) || port == 0 || (mode != "gm" && mode != "bm"))
        {
            sendJson(res, 400, errorEnvelope("INVALID_ARGUMENT", "needs ip, port and mode gm|bm (viewer: layer busmon|link)"));
            return;
        }
        // the same guard as the monitor operation
        json probeArgs = {{"ip", ip}, {"mode", mode}, {"seconds", 1}};
        const std::string r = _rules.check(*findOp("monitor"), probeArgs, ip, port);
        if (!r.empty())
        {
            sendJson(res, 403, errorEnvelope("GUARD_DENIED", r, {{"rule", r}}));
            return;
        }
        if (!rateOk(a.id))
        {
            sendJson(res, 429, errorEnvelope("RATE_LIMITED", "too many calls in one minute"));
            return;
        }
        json err;
        auto reader = attachReader(ifaceKey(ip, port), ip, port, mode, viewer, viewer ? "viewer" : "sse", err);
        if (!reader)
        {
            if (viewer)
            {
                // the viewer reads an error event, as the bridge sends it
                const std::string msg = err["error"].value("reason", std::string("refused"));
                res.set_content("event: error\ndata: " + json({{"msg", msg}, {"code", err["error"].value("code", std::string())}}).dump() + "\n\n",
                                "text/event-stream");
                return;
            }
            sendJson(res, statusFor(err), err);
            return;
        }
        const std::string key = ifaceKey(ip, port);
        auto first = std::make_shared<bool>(true);
        auto lastWrite = std::make_shared<uint64_t>(monoMs());
        res.set_chunked_content_provider(
            "text/event-stream",
            guardedProvider([reader, viewer, ip, port, mode, first, lastWrite](size_t, httplib::DataSink& sink) {
                if (g_stop) return false;
                std::string out;
                if (*first)
                {
                    *first = false;
                    if (viewer)
                        out += "event: hello\ndata: " +
                               json({{"mode", mode == "bm" ? "busmon" : "link"}, {"iface", ip + ":" + std::to_string(port)}}).dump() + "\n\n";
                    else
                        out += "event: hello\ndata: " + json({{"mode", mode}, {"iface", ip + ":" + std::to_string(port)}}).dump() + "\n\n";
                }
                bool closed = false;
                std::string code;
                auto evs = reader->take(500, 500, closed, code);
                for (const auto& e : evs)
                {
                    const std::string t = jget(e, "t", "");
                    if (viewer)
                    {
                        if (t == "frame")
                        {
                            std::string hex = jget(e, "hex", "");
                            for (auto& c : hex) c = (char)std::toupper((unsigned char)c);
                            // busmon layer: L_Busmon.ind only; link layer: everything, as the bridge does
                            if (mode == "bm" && hex.compare(0, 2, "2B") != 0) continue;
                            out += "data: " + json({{"hex", hex}, {"ts", jget(e, "ts", (uint64_t)0)}}).dump() + "\n\n";
                        }
                        else if (t == "lost")
                            out += "event: lost\ndata: " + json({{"count", jget(e, "count", (uint64_t)0)}}).dump() + "\n\n";
                    }
                    else if (t == "frame")
                        out += "event: frame\ndata: " + jget<json>(e, "f", json::object()).dump(-1, ' ', false, json::error_handler_t::replace) + "\n\n";
                    else if (t == "lost")
                        out += "event: lost\ndata: " + e.dump() + "\n\n";
                    else if (t == "mon")
                        out += "event: state\ndata: " + e.dump() + "\n\n";
                }
                if (closed)
                {
                    out += viewer ? "event: error\ndata: " + json({{"msg", "monitor ended: " + code}}).dump() + "\n\n"
                                  : "event: end\ndata: " + json({{"code", code}}).dump() + "\n\n";
                }
                if (out.empty() && monoMs() - *lastWrite >= 4000) out = ": ping\n\n";
                if (!out.empty())
                {
                    if (!sink.write(out.data(), out.size())) return false;
                    *lastWrite = monoMs();
                }
                if (closed)
                {
                    sink.done();
                    return true;
                }
                return true;
            }),
            [this, reader, key, mode](bool) { detachReader(key, mode, reader); });
    }

    /*******************************************************************
     ************************ LEADER OPERATIONS ***********************
     *******************************************************************/
    json runLeaderOp(const OpDesc& d, OpRec& op)
    {
        const json& a = op.argsFull;
        if (d.name == "discover") return opDiscover(a);
        if (d.name == "device_state") return opDeviceGet(a, "state");
        if (d.name == "device_bus") return opDeviceGet(a, "bus");
        if (d.name == "tunnels") return opTunnels(a);
        if (d.name == "bench_state") return opBenchState();
        if (d.name == "monitor") return opMonitor(a, op);
        if (d.name == "monitor_stop") return opMonitorStop(a);
        if (d.name == "compare") return runCompareOp(*this, a, op);
        if (d.name == "device_tunnel_close") return opDevicePost(a, "close?ch=" + std::to_string(a.value("ch", 0)), true);
        if (d.name == "device_busmon_stop") return opDevicePost(a, "busmon/stop", false);
        if (d.name == "device_reset") return opDevicePost(a, "reset", true);
        if (d.name == "suite_run") return opSuiteRun(a);
        return errorEnvelope("INTERNAL", "no implementation for this operation", {{"op", d.name}});
    }

    json opDiscover(const json& a)
    {
        const int to = a.value("timeout_ms", 2500);
        std::vector<DiscoveredIface> found;
        std::vector<MdnsDevice> md;
        std::thread t1([&] { found = discoverInterfaces(3671, to); });
        std::thread t2([&] { md = mdnsOpenKnx(to); });
        t1.join();
        t2.join();
        std::map<std::string, json> byIp;
        auto paStr = [](uint16_t p) {
            return std::to_string((p >> 12) & 0x0F) + "." + std::to_string((p >> 8) & 0x0F) + "." + std::to_string(p & 0xFF);
        };
        for (const auto& f : found)
        {
            json fam = json::array();
            bool rt = false, tn = false;
            for (const auto& p : f.families)
            {
                fam.push_back({{"id", p.first}, {"version", p.second}});
                if (p.first == 0x05) rt = true;
                if (p.first == 0x04) tn = true;
            }
            char mac[18], sn[13];
            std::snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", f.mac[0], f.mac[1], f.mac[2], f.mac[3], f.mac[4], f.mac[5]);
            std::snprintf(sn, sizeof(sn), "%02X%02X%02X%02X%02X%02X", f.serial[0], f.serial[1], f.serial[2], f.serial[3],
                          f.serial[4], f.serial[5]);
            byIp[f.ip] = {{"ip", f.ip},      {"port", f.port},       {"name", f.name},
                          {"kind", rt ? "router" : tn ? "interface" : ""},
                          {"pa", paStr(f.ia)}, {"families", fam},     {"mac", mac},
                          {"serial", sn},     {"mdns_name", ""},      {"source", "knxnetip"}};
        }
        for (const auto& m : md)
        {
            json& e = byIp[m.ip];
            if (e.is_null()) e = {{"ip", m.ip}, {"port", 3671}, {"name", m.name}, {"kind", ""}, {"pa", ""}, {"families", json::array()},
                                  {"mac", ""}, {"serial", ""}, {"source", "openknx"}};
            else
                e["source"] = "both";
            e["mdns_name"] = m.instance;
            if (!m.name.empty()) e["name"] = m.name;
            if (e.value("pa", std::string()).empty() || e["pa"] == "0.0.0")
                e["pa"] = m.txt.count("address") ? m.txt.at("address") : std::string();
            for (const char* k : {"version", "configured", "firmware", "otamode"})
                e[k] = m.txt.count(k) ? m.txt.at(k) : std::string();
            const std::string ota = m.txt.count("ota") ? m.txt.at("ota") : std::string();
            e["chip"] = ota == "3232" ? "ESP32" : ota == "2040" ? "RP2040/RP2350" : "";
            if (m.txt.count("serial") && e.value("serial", std::string()).empty()) e["serial"] = m.txt.at("serial");
        }
        json arr = json::array();
        for (auto& kv : byIp)
        {
            for (const char* k : {"version", "configured", "firmware", "otamode", "chip"})
                if (!kv.second.contains(k)) kv.second[k] = "";
            arr.push_back(kv.second);
        }
        return {{"ok", true}, {"data", {{"interfaces", arr}}}};
    }

    /** @brief GET a firmware document of an OpenKNX interface: /ipif/<doc>, else /ipro/<doc>. */
    bool deviceGet(const std::string& ip, const std::string& doc, json& out, std::string& family)
    {
        for (const char* fam : {"ipif", "ipro"})
        {
            {
                std::lock_guard<std::mutex> lk(_m);
                auto it = _devFamily.find(ip);
                if (it != _devFamily.end() && it->second.first != fam && monoMs() - it->second.second < 60000) continue;
            }
            httplib::Client c(ip, 80);
            c.set_connection_timeout(1, 0);
            c.set_read_timeout(2, 0);
            auto r = c.Get(std::string("/") + fam + "/" + doc);
            if (!r || r->status != 200) continue;
            json j = json::parse(r->body, nullptr, false);
            if (!j.is_object()) continue;
            out = j;
            family = fam;
            std::lock_guard<std::mutex> lk(_m);
            _devFamily[ip] = {fam, monoMs()};
            return true;
        }
        return false;
    }

    json opDeviceGet(const json& a, const std::string& doc)
    {
        json raw;
        std::string fam;
        if (!deviceGet(a.value("ip", std::string()), doc, raw, fam))
            return errorEnvelope("NO_ANSWER", "no /ipif or /ipro document from this address (not an OpenKNX interface, or not reachable)");
        json data = {{"raw", raw}, {"family", fam}};
        if (doc == "state")
        {
            // the device's answer: every member read by type, whatever an older or foreign firmware sends
            data["busmon"] = raw.contains("busmon") && raw["busmon"].is_boolean() ? json(raw["busmon"].get<bool>() ? "active" : "idle")
                                                                                  : json("unknown");
            data["busmon_owner"] = jget(raw, "bmown", 0);
            if (raw.contains("tun")) data["tunnels"] = raw["tun"];
            data["pa"] = jget(raw, "pa", "");
            data["mask"] = jget(raw, "mask", "");
        }
        return {{"ok", true}, {"data", data}};
    }

    /** @brief A busmonitor the interface reports that serve does not hold (OpenKNX only; others: unknown). */
    bool deviceBusmonForeign(const std::string& ip, json& info)
    {
        json raw;
        std::string fam;
        if (!deviceGet(ip, "state", raw, fam)) return false;
        if (!raw.contains("busmon") || !raw["busmon"].is_boolean() || !raw["busmon"].get<bool>()) return false;
        // ours? then serve holds a bm monitor on this interface
        bool ours = false;
        {
            std::lock_guard<std::mutex> lk(_m);
            for (auto& m : _mons)
                if (m.first.compare(0, ip.size() + 1, ip + ":") == 0 && m.second->mode == "bm") ours = true;
        }
        if (ours) return false;
        const int own = jget(raw, "bmown", 0);
        info = {{"owner", own == 1 ? "tunnel" : own == 2 ? "console" : own == 3 ? "tunnel+console" : "unknown"}};
        return true;
    }

    json opTunnels(const json& a)
    {
        const std::string ip = a.value("ip", std::string());
        const uint16_t port = (uint16_t)a.value("port", 3671);
        json raw;
        std::string fam;
        json slots = json::array();
        std::string busmon = "unknown", owner;
        std::string source = "none";
        if (deviceGet(ip, "state", raw, fam) && raw.contains("tun"))
        {
            source = "device-json";
            const json tun = jget<json>(raw, "tun", json::object());
            int mx = jget(tun, "max", 0);
            if (mx < 0) mx = 0;
            if (mx > 255) mx = 255; // a tunnel slot count, never a loop bound the device chooses freely
            std::map<int, json> bySlot;
            json other = json::array();
            const json act = jget<json>(tun, "act", json::array());
            for (const auto& t : act.is_array() ? act : json::array())
            {
                if (!t.is_object()) continue;
                json e = {{"pa", jget(t, "pa", "")}, {"type", jget(t, "t", "")}, {"client", jget(t, "ip", "")},
                          {"ch", jget(t, "ch", 0)}, {"since", jget(t, "start", "")}};
                if (t.contains("s")) bySlot[jget(t, "s", 0)] = e;
                else
                    other.push_back(e);
            }
            json ias = jget<json>(tun, "ia", json::array());
            if (!ias.is_array()) ias = json::array();
            for (int n = 1; n <= mx; ++n)
            {
                json s = {{"n", n}, {"pa", (size_t)(n - 1) < ias.size() ? ias[n - 1] : json("")}};
                if (bySlot.count(n))
                {
                    s["state"] = "taken";
                    s["type"] = bySlot[n]["type"];
                    s["client"] = bySlot[n]["client"];
                    s["ch"] = bySlot[n]["ch"];
                    s["since"] = bySlot[n]["since"];
                }
                else
                    s["state"] = "free";
                slots.push_back(s);
            }
            for (auto& o : other) // device management and busmonitor connections hold no slot
            {
                o["n"] = nullptr;
                o["state"] = "taken";
                slots.push_back(o);
            }
            if (raw.contains("busmon") && raw["busmon"].is_boolean()) busmon = raw["busmon"].get<bool>() ? "active" : "idle";
            const int own = jget(raw, "bmown", 0);
            owner = own == 1 ? "tunnel" : own == 2 ? "console" : own == 3 ? "tunnel+console" : "";
        }
        else
        {
            // no firmware document: only what serve itself holds there
            source = "serve";
            std::lock_guard<std::mutex> lk(_m);
            const std::string key = ifaceKey(ip, port);
            for (auto& m : _mons)
                if (m.second->iface == key) slots.push_back({{"n", nullptr}, {"state", "taken"}, {"type", m.second->mode}, {"client", "serve"}});
            if (!_mgmtRunning[key].empty()) slots.push_back({{"n", nullptr}, {"state", "taken"}, {"type", "mgmt"}, {"client", "serve"}});
            busmon = _mons.count(key + "/bm") ? "active" : "unknown";
        }
        return {{"ok", true}, {"data", {{"slots", slots}, {"busmon", busmon}, {"owner", owner}, {"source", source}}}};
    }

    json opBenchState()
    {
        std::lock_guard<std::mutex> lk(_m);
        json ops = json::array();
        for (auto& o : _ops) ops.push_back(opSummary(*o.second));
        return {{"ok", true},
                {"data",
                 {{"connections", connectionsJsonLocked()},
                  {"leases", leasesJsonLocked()},
                  {"bench", _bench.toJson()},
                  {"released", _released},
                  {"ops", ops}}}};
    }

    json connectionsJsonLocked()
    {
        json c = json::array();
        for (auto& m : _mons)
            c.push_back({{"iface", m.second->iface},
                         {"kind", m.second->mode},
                         {"state", (int)m.second->state},
                         {"since_ms", monoMs() - m.second->since},
                         {"readers", m.second->readerCount()},
                         {"frames", m.second->frames},
                         {"lost_pipe", m.second->lostPipe}});
        for (auto& r : _mgmtRunning)
            if (!r.second.empty()) c.push_back({{"iface", r.first}, {"kind", "mgmt"}, {"op", r.second}});
        return c;
    }

    json leasesJsonLocked()
    {
        json a = json::array();
        for (auto& l : _leases)
        {
            json items = json::array();
            for (const auto& it : l.second->items) items.push_back({{"ip", it.ip}, {"port", it.port}, {"kind", it.kind}});
            a.push_back({{"id", l.first}, {"pid", l.second->pid}, {"purpose", l.second->purpose}, {"items", items},
                         {"since_ms", monoMs() - l.second->since}});
        }
        return a;
    }

    /** @brief `monitor`: a reader on the shared monitor for `seconds`; statistics, frames, full capture as a file. */
    json opMonitor(const json& a, OpRec& op)
    {
        const std::string ip = a.value("ip", std::string());
        const uint16_t port = (uint16_t)a.value("port", 3671);
        const std::string mode = a.value("mode", std::string("gm"));
        const int secs = a.value("seconds", 10);
        const size_t maxFrames = (size_t)a.value("max_frames", 100);
        std::vector<uint16_t> filter;
        for (const auto& g : a.value("filter_ga", json::array()))
        {
            uint16_t ga = 0;
            if (!parseGa3(g.get<std::string>(), ga))
                return errorEnvelope("INVALID_ARGUMENT", "filter_ga takes single addresses x/y/z", {{"ga", g}});
            filter.push_back(ga);
        }
        json err;
        auto r = attachReader(ifaceKey(ip, port), ip, port, mode, false, "op:" + op.id, err);
        if (!r) return err;
        // the reader goes on every way out, also an exception: a reader left behind holds the interface
        struct Detach
        {
            std::function<void()> f;
            ~Detach() { f(); }
        } detach{[this, ip, port, mode, r] { detachReader(ifaceKey(ip, port), mode, r); }};
        const std::string cap = serveDir(true) + PATH_SEP + "capture-" + op.id + "-" + mode + ".jsonl";
        std::ofstream capf(cap, std::ios::binary | std::ios::trunc);
        json frames = json::array();
        std::map<std::string, uint64_t> bySvc, bySrc, byDst;
        uint64_t total = 0, lost = 0;
        std::string endReason = "time";
        const uint64_t t0 = monoMs(), end = t0 + (uint64_t)secs * 1000;
        uint64_t lastProg = t0;
        while (monoMs() < end)
        {
            if (*op.cancel)
            {
                endReason = "cancelled";
                break;
            }
            bool closed = false;
            std::string code;
            auto evs = r->take(500, 200, closed, code);
            for (const auto& e : evs)
            {
                const std::string t = jget(e, "t", "");
                if (t == "lost")
                {
                    lost += jget(e, "count", (uint64_t)0);
                    continue;
                }
                if (t != "frame") continue;
                const json f = jget<json>(e, "f", json::object());
                if (!filter.empty())
                {
                    const uint16_t dst = jget(f, "dst", (uint16_t)0);
                    if (!jget(f, "group", false) || std::find(filter.begin(), filter.end(), dst) == filter.end()) continue;
                }
                total++;
                capf << f.dump(-1, ' ', false, json::error_handler_t::replace) << "\n";
                if (frames.size() < maxFrames) frames.push_back(f);
                const std::string body = jget(f, "body", "");
                const size_t lb = body.find('['), rb = body.find(']');
                bySvc[lb != std::string::npos && rb != std::string::npos && rb > lb ? body.substr(lb + 1, rb - lb - 1) : "other"]++;
                const uint16_t s = jget(f, "src", (uint16_t)0), d = jget(f, "dst", (uint16_t)0);
                bySrc[std::to_string((s >> 12) & 15) + "." + std::to_string((s >> 8) & 15) + "." + std::to_string(s & 255)]++;
                byDst[jget(f, "group", false) ? std::to_string((d >> 11) & 31) + "/" + std::to_string((d >> 8) & 7) + "/" + std::to_string(d & 255)
                                              : std::to_string((d >> 12) & 15) + "." + std::to_string((d >> 8) & 15) + "." + std::to_string(d & 255)]++;
            }
            if (closed)
            {
                endReason = code.empty() ? "ended" : code;
                break;
            }
            if (monoMs() - lastProg >= 5000)
            {
                op.addEvent({{"elapsed_s", (monoMs() - t0) / 1000}, {"seconds", secs}, {"frames", total}, {"lost", lost}});
                lastProg = monoMs();
            }
        }
        capf.close();
        auto top = [](const std::map<std::string, uint64_t>& m) {
            std::vector<std::pair<std::string, uint64_t>> v(m.begin(), m.end());
            std::sort(v.begin(), v.end(), [](const std::pair<std::string, uint64_t>& x, const std::pair<std::string, uint64_t>& y) {
                return x.second > y.second;
            });
            json j = json::object();
            for (size_t i = 0; i < v.size() && i < 20; ++i) j[v[i].first] = v[i].second;
            return j;
        };
        const json stats = {{"by_service", top(bySvc)}, {"by_source", top(bySrc)}, {"by_destination", top(byDst)}};
        if (endReason == "cancelled")
            return errorEnvelope("CANCELLED", "cancelled", {{"frames_so_far", total}, {"capture", cap}});
        return {{"ok", true},
                {"data",
                 {{"stats", stats},
                  {"frames", frames},
                  {"truncated", total > frames.size()},
                  {"total", total},
                  {"lost", lost},
                  {"capture", cap},
                  {"end_reason", endReason}}}};
    }

    json opMonitorStop(const json& a)
    {
        const std::string ip = a.value("ip", std::string());
        const uint16_t port = (uint16_t)a.value("port", 3671);
        const std::string mode = a.value("mode", std::string());
        auto mon = findMon(ifaceKey(ip, port), mode);
        if (!mon) return {{"ok", true}, {"data", {{"stopped", false}, {"reason", "no such monitor"}}}};
        const size_t readers = mon->readerCount();
        stopMonitor(ifaceKey(ip, port), mode, "CANCELLED");
        return {{"ok", true}, {"data", {{"stopped", true}, {"readers_ended", readers}}}};
    }

    /** @brief POST a firmware endpoint of an OpenKNX interface: /ipif/<path>, and on a router /ipro/<path>. */
    json opDevicePost(const json& a, const std::string& path, bool routerToo)
    {
        const std::string ip = a.value("ip", std::string());
        for (const char* fam : {"ipif", "ipro"})
        {
            if (std::string(fam) == "ipro" && !routerToo) break;
            httplib::Client c(ip, 80);
            c.set_connection_timeout(1, 0);
            c.set_read_timeout(3, 0);
            auto r = c.Post(std::string("/") + fam + "/" + path, std::string(), "application/json");
            if (!r || r->status == 404) continue;
            json body = json::parse(r->body, nullptr, false);
            if (r->status >= 200 && r->status < 300)
                return {{"ok", true}, {"data", {{"status", r->status}, {"family", fam}, {"answer", body.is_discarded() ? json(r->body) : body}}}};
            return errorEnvelope(r->status == 409 ? "BUSY" : "NO_ANSWER",
                                 "the interface refused: HTTP " + std::to_string(r->status),
                                 {{"family", fam}, {"answer", body.is_discarded() ? json(r->body) : body}});
        }
        return errorEnvelope("NO_ANSWER", "no OpenKNX interface answers this endpoint at " + ip);
    }

    /** @brief Start Run-Tests.ps1 -TestProfile <profile> next to the bench config; the run takes the bench itself. */
    json opSuiteRun(const json& a)
    {
        const std::string profile = a.value("profile", std::string());
        if (profile.empty() || profile.size() > 64 || profile.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-_") != std::string::npos)
            return errorEnvelope("INVALID_ARGUMENT", "profile is a name like quick-check");
        std::string bc;
        for (const auto& s : _rules.sites())
            if (s.suiteRun) bc = s.benchConfig;
        const size_t cut = bc.find_last_of("/\\");
        const std::string dir = cut == std::string::npos ? std::string(".") : bc.substr(0, cut);
        const std::string script = dir + PATH_SEP + "Run-Tests.ps1";
        const std::string prof = dir + PATH_SEP + "Profiles" + PATH_SEP + profile + ".json";
        if (!std::ifstream(script)) return errorEnvelope("INTERNAL", "Run-Tests.ps1 not found next to the bench config", {{"path", script}});
        if (!std::ifstream(prof)) return errorEnvelope("INVALID_ARGUMENT", "no such profile", {{"profile", profile}, {"looked_in", dir + PATH_SEP + "Profiles"}});
        const std::string log = serveDir(true) + PATH_SEP + "suite-" + profile + "-" + std::to_string((unsigned long long)wallMs()) + ".log";
        Child c;
        std::string err;
#ifdef _WIN32
        const std::string pwsh = "pwsh.exe";
#else
        const std::string pwsh = findInPath("pwsh");
#endif
        if (pwsh.empty()) return errorEnvelope("INTERNAL", "pwsh is not on the PATH of serve");
        if (!spawnChild(pwsh, {"-NoProfile", "-File", script, "-TestProfile", profile, "-Yes"}, log, c, err))
            return errorEnvelope("INTERNAL", "could not start the suite: " + err);
        pipeClose(c.toChild);
        const long pid = c.pidValue();
        std::thread([c]() mutable {
            pipeClose(c.fromChild);
            c.wait();
        }).detach();
        logLine("suite_run", {{"profile", profile}, {"pid", pid}, {"log", log}});
        return {{"ok", true},
                {"data", {{"started", true}, {"pid", pid}, {"profile", profile}, {"log", log},
                          {"note", "the run writes its lock file and its reports; follow it with bench_state"}}}};
    }

#ifndef _WIN32
    static std::string findInPath(const std::string& exe)
    {
        const char* p = std::getenv("PATH");
        std::string path = p ? p : "/usr/local/bin:/usr/bin:/bin:/opt/homebrew/bin";
        size_t s = 0;
        while (s <= path.size())
        {
            size_t e = path.find(':', s);
            if (e == std::string::npos) e = path.size();
            const std::string cand = path.substr(s, e - s) + "/" + exe;
            if (::access(cand.c_str(), X_OK) == 0) return cand;
            s = e + 1;
        }
        for (const char* d : {"/opt/homebrew/bin/", "/usr/local/bin/"})
            if (::access((std::string(d) + exe).c_str(), X_OK) == 0) return std::string(d) + exe;
        return std::string();
    }
#endif

    /**
     * @brief The FTC password for @p paStr: OKNX_FTC_PW_<a>_<l>_<d> of serve's environment, else serve asks in
     *        its own terminal (only in the foreground). It is passed to the worker and nowhere else.
     */
    std::string ftcPassword(const std::string& paStr, const Access& acc, std::string& why)
    {
        unsigned a = 0, l = 0, d = 0;
        if (std::sscanf(paStr.c_str(), "%u.%u.%u", &a, &l, &d) != 3)
        {
            why = "pa is not an individual address";
            return std::string();
        }
        char name[32];
        std::snprintf(name, sizeof(name), "OKNX_FTC_PW_%u_%u_%u", a, l, d);
        if (const char* v = std::getenv(name))
            if (*v) return v;
        Term t;
        if (!t.isTty())
        {
            why = std::string("no password: set ") + name + " in the environment of serve, or run serve in the foreground (it then asks)";
            return std::string();
        }
        std::lock_guard<std::mutex> lk(_promptM); // one question at a time on the terminal
        Theme th(t);
        std::string pw;
        std::printf("\n");
        const std::string label = "FTC password for " + paStr + " (asked by " + (acc.caller.empty() ? acc.id : acc.caller) + ")";
        if (!readSecret(t, th, label, pw) || pw.empty())
        {
            why = "no password was typed in serve's terminal";
            return std::string();
        }
        return pw;
    }

    /** @brief /discover for the viewer: the bridge's array form. */
    void viewerDiscover(const httplib::Request& req, httplib::Response& res)
    {
        Access a;
        if (!auth(req, res, true, a)) return;
        json env = runOperation("discover", {{"timeout_ms", 2500}}, a, nullptr, nullptr, true);
        if (!env.value("ok", false))
        {
            sendJson(res, statusFor(env), env);
            return;
        }
        json arr = json::array();
        for (const auto& i : env["data"]["interfaces"])
            arr.push_back({{"name", i.value("name", std::string())},       {"ip", i.value("ip", std::string())},
                           {"pa", i.value("pa", std::string())},           {"version", i.value("version", std::string())},
                           {"chip", i.value("chip", std::string())},       {"kind", i.value("kind", std::string())},
                           {"source", i.value("source", std::string())},   {"otamode", i.value("otamode", std::string())},
                           {"configured", i.value("configured", std::string())}, {"firmware", i.value("firmware", std::string())}});
        baseHeaders(res);
        res.set_content(arr.dump(), "application/json");
    }

    /*******************************************************************
     *************************** RESERVATIONS **************************
     *******************************************************************/
    /** @brief Whether serve's own monitors on @p key leave no room for a command line lease of @p kind. */
    bool leaseMonitorsBusy(const std::string& key, const std::string& kind, json& busy)
    {
        auto bm = findMon(key, "bm");
        auto gm = findMon(key, "gm");
        if (bm && bm->readerCount() > 0)
        {
            busy = errorEnvelope("BUSY", "serve holds a busmonitor here",
                                 {{"holder", "serve-busmon"}, {"readers", bm->readerCount()}, {"iface", key}});
            return true;
        }
        if (kind == "bm" && gm && gm->readerCount() > 0)
        {
            busy = errorEnvelope("BUSY", "serve uses this interface; a busmonitor needs it alone", {{"holder", "serve-groupmon"}, {"iface", key}});
            return true;
        }
        return false;
    }

    void postLease(const httplib::Request& req, httplib::Response& res)
    {
        Access a;
        if (!auth(req, res, false, a)) return;
        json body = json::parse(req.body, nullptr, false);
        if (!body.is_object() || !body.contains("items") || !body["items"].is_array() || body["items"].empty())
        {
            sendJson(res, 400, errorEnvelope("INVALID_ARGUMENT", "a lease needs items [{ip, port, kind}]"));
            return;
        }
        auto lease = std::make_shared<Lease>();
        lease->purpose = body.value("purpose", std::string()).substr(0, 120);
        lease->pid = body.value("pid", 0L);
        lease->since = monoMs();
        for (const auto& it : body["items"])
        {
            Lease::Item li;
            li.ip = it.value("ip", std::string());
            li.port = (uint16_t)it.value("port", 3671);
            li.kind = it.value("kind", std::string());
            if (!isIpv4(li.ip) || (li.kind != "mgmt" && li.kind != "gm" && li.kind != "bm"))
            {
                sendJson(res, 400, errorEnvelope("INVALID_ARGUMENT", "each item needs an IPv4 ip and kind mgmt|gm|bm"));
                return;
            }
            lease->items.push_back(li);
        }
        // all or none: check every item before taking any
        for (const auto& it : lease->items)
        {
            const std::string key = ifaceKey(it.ip, it.port);
            json busy;
            if (leaseMonitorsBusy(key, it.kind, busy))
            {
                sendJson(res, 409, busy);
                return;
            }
        }
        // checked and entered under one lock: an operation queued after this sees the lease and is refused
        {
            std::lock_guard<std::mutex> lk(_m);
            for (const auto& it : lease->items)
            {
                const std::string key = ifaceKey(it.ip, it.port);
                if (_bench.valid && _bench.names(it.ip))
                {
                    sendJson(res, 409, errorEnvelope("BENCH_LOCKED", "a suite run holds the bench", {{"run", _bench.run}}));
                    return;
                }
                for (const auto& l : _leases) // a busmonitor excludes every other tunnel, also of another command line
                    for (const auto& o : l.second->items)
                        if (ifaceKey(o.ip, o.port) == key && (o.kind == "bm" || it.kind == "bm"))
                        {
                            sendJson(res, 409, errorEnvelope("BUSY", "another command line holds this interface",
                                                             {{"holder", "cli:" + std::to_string(l.second->pid)},
                                                              {"kind", o.kind},
                                                              {"iface", key}}));
                            return;
                        }
                if (it.kind == "bm" && (!_mgmtRunning[key].empty() || !_mgmtQueue[key].empty()))
                {
                    sendJson(res, 409, errorEnvelope("BUSY", "serve uses this interface; a busmonitor needs it alone",
                                                     {{"holder", "serve-op"}, {"iface", key}}));
                    return;
                }
            }
            lease->id = "L-" + randomHex(6);
            _leases[lease->id] = lease;
        }
        // a reader that attached between the two checks
        for (const auto& it : lease->items)
        {
            json busy;
            if (leaseMonitorsBusy(ifaceKey(it.ip, it.port), it.kind, busy))
            {
                endLease(lease->id, false); // never granted: no lease_end line
                sendJson(res, 409, busy);
                return;
            }
        }
        // take: stop what lingers without readers, make the worker give its idle tunnel back for a busmonitor
        for (const auto& it : lease->items)
        {
            const std::string key = ifaceKey(it.ip, it.port);
            if (findMon(key, "bm")) stopMonitor(key, "bm", "CANCELLED");
            if (it.kind == "bm")
            {
                if (findMon(key, "gm")) stopMonitor(key, "gm", "CANCELLED");
                std::shared_ptr<WorkerConn> w;
                {
                    std::lock_guard<std::mutex> lk(_m);
                    auto wi = _workers.find(key);
                    if (wi != _workers.end()) w = wi->second;
                }
                if (w && w->alive)
                {
                    // granted only once the tunnel is gone: the busmonitor connect of the command line comes next
                    const std::string cid = "lease-" + randomHex(4);
                    if (w->send({{"t", "mgmt_close"}, {"id", cid}}))
                    {
                        std::unique_lock<std::mutex> lk(w->hm);
                        w->hcv.wait_for(lk, std::chrono::seconds(5), [&] { return w->closed.count(cid) > 0 || !w->alive; });
                        w->closed.erase(cid);
                    }
                }
            }
        }
        logLine("lease", {{"id", lease->id}, {"pid", lease->pid}, {"purpose", lease->purpose}});
        auto sent = std::make_shared<bool>(false);
        res.set_chunked_content_provider(
            "application/x-ndjson",
            guardedProvider([lease, sent](size_t, httplib::DataSink& sink) {
                std::string out;
                if (!*sent)
                {
                    *sent = true;
                    out = json({{"v", 1}, {"ok", true}, {"lease", lease->id}}).dump() + "\n";
                }
                else
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                    out = "\n"; // a write to a closed connection fails: that ends the lease
                }
                if (g_stop || lease->ended) return false;
                return sink.write(out.data(), out.size());
            }),
            [this, lease](bool) { endLease(lease->id); });
    }

    void deleteLease(const httplib::Request& req, httplib::Response& res)
    {
        Access a;
        if (!auth(req, res, false, a)) return;
        const bool had = endLease(req.matches[1]);
        sendJson(res, had ? 200 : 404, had ? json{{"v", 1}, {"ok", true}} : errorEnvelope("INVALID_ARGUMENT", "no such lease"));
    }

    bool endLease(const std::string& id, bool log = true)
    {
        std::shared_ptr<Lease> l;
        {
            std::lock_guard<std::mutex> lk(_m);
            auto it = _leases.find(id);
            if (it == _leases.end()) return false;
            l = it->second;
            _leases.erase(it);
        }
        l->ended = true;
        if (log) logLine("lease_end", {{"id", id}});
        return true;
    }

    /*******************************************************************
     ***************************** SESSIONS ****************************
     *******************************************************************/
    void sessionLink(const httplib::Request& req, httplib::Response& res)
    {
        Access a;
        if (!auth(req, res, false, a)) return;
        const std::string t = randomHex(16);
        {
            std::lock_guard<std::mutex> lk(_m);
            const uint64_t now = monoMs();
            for (auto it = _links.begin(); it != _links.end();) it = now - it->second > 60000 ? _links.erase(it) : std::next(it);
            _links[t] = now;
        }
        sendJson(res, 200, {{"v", 1}, {"ok", true}, {"data", {{"url", "http://127.0.0.1:" + std::to_string(_port) + "/ui/login?t=" + t},
                                                              {"valid_s", 60}}}});
    }

    void uiLogin(const httplib::Request& req, httplib::Response& res)
    {
        const std::string t = req.get_param_value("t");
        bool ok = false;
        {
            std::lock_guard<std::mutex> lk(_m);
            auto it = _links.find(t);
            if (it != _links.end())
            {
                ok = monoMs() - it->second <= 60000;
                _links.erase(it); // once, valid or not
            }
        }
        baseHeaders(res);
        if (!ok)
        {
            res.status = 403;
            res.set_content(uiLoginFailedPage(_lang), "text/html; charset=utf-8");
            return;
        }
        const std::string sid = randomHex(32);
        {
            std::lock_guard<std::mutex> lk(_m);
            _sessions[sid] = {sid, monoMs(), monoMs()};
        }
        res.set_header("Set-Cookie", "oknx_session=" + sid + "; HttpOnly; SameSite=Strict; Path=/; Max-Age=43200");
        res.set_redirect("/ui/", 303);
    }

    void uiPage(const httplib::Request& req, httplib::Response& res)
    {
        Access a;
        baseHeaders(res);
        if (!sessionAccess(req, a, true))
        {
            res.status = 401;
            res.set_content(uiNoSessionPage(_lang), "text/html; charset=utf-8");
            return;
        }
        const std::string path = req.matches.size() > 1 ? std::string(req.matches[1]) : std::string();
        std::string type;
        std::string body = uiAssetFrom(path, _lang, benchConfigPath(), type);
        if (body.empty())
        {
            res.status = 404;
            res.set_content("not found", "text/plain");
            return;
        }
        res.set_header("Content-Security-Policy", "default-src 'self'; style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline'; "
                                                  "img-src 'self' data:; connect-src 'self'; frame-ancestors 'none'");
        res.set_content(body, type);
    }

    /*******************************************************************
     **************************** BENCH LOCK ***************************
     *******************************************************************/
    void benchLoop()
    {
        std::string lastRun;
        while (!g_stop)
        {
            BenchLock b;
            for (const auto& s : _rules.sites())
                if (!s.lockFile.empty())
                {
                    BenchLock x = readBenchLock(s.lockFile);
                    if (x.present) b = x;
                    if (b.valid) break;
                }
            {
                std::lock_guard<std::mutex> lk(_m);
                _bench = b;
            }
            if (b.valid && b.run != lastRun)
            {
                lastRun = b.run;
                releaseForBench(b);
            }
            if (!b.valid) lastRun.clear();
            if (b.valid)
            {
                // released once no command-line reservation is open on the named interfaces
                bool open = false;
                {
                    std::lock_guard<std::mutex> lk(_m);
                    for (auto& l : _leases)
                        for (const auto& it : l.second->items)
                            if (b.names(it.ip)) open = true;
                    _released = open ? std::string() : b.run;
                }
            }
            else
            {
                std::lock_guard<std::mutex> lk(_m);
                _released.clear();
            }
            for (int i = 0; i < 20 && !g_stop; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    void releaseForBench(const BenchLock& b)
    {
        logLine("bench_lock", {{"run", b.run}, {"interfaces", b.interfaces}});
        std::vector<std::string> keys;
        {
            std::lock_guard<std::mutex> lk(_m);
            for (auto& w : _workers)
                if (b.names(w.second->ip)) keys.push_back(w.first);
            for (auto& m : _mons)
            {
                const std::string ip = m.second->iface.substr(0, m.second->iface.rfind(':'));
                if (b.names(ip) && std::find(keys.begin(), keys.end(), m.second->iface) == keys.end()) keys.push_back(m.second->iface);
            }
        }
        for (const auto& key : keys)
        {
            for (const char* mode : {"gm", "bm"})
                if (findMon(key, mode)) stopMonitor(key, mode, "bench_locked");
            std::vector<std::shared_ptr<OpRec>> ops;
            {
                std::lock_guard<std::mutex> lk(_m);
                for (auto& o : _ops)
                    if (o.second->iface == key && !o.second->done) ops.push_back(o.second);
            }
            for (auto& o : ops) cancelOp(*o, "a suite run took the bench");
            std::shared_ptr<WorkerConn> w;
            {
                std::lock_guard<std::mutex> lk(_m);
                auto it = _workers.find(key);
                if (it != _workers.end()) w = it->second;
            }
            if (w) w->send({{"t", "bye"}}); // the worker closes every connection and ends
        }
    }

    /*******************************************************************
     ***************************** WATCHDOG ****************************
     *******************************************************************/
    void watchdogLoop()
    {
        while (!g_stop)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            std::vector<std::shared_ptr<OpRec>> late, over;
            std::vector<std::pair<std::string, std::string>> linger;
            {
                std::lock_guard<std::mutex> lk(_m);
                const uint64_t now = monoMs();
                for (auto& o : _ops)
                {
                    std::lock_guard<std::mutex> lk2(o.second->m);
                    if (o.second->done || !o.second->started) continue;
                    const uint64_t age = now - o.second->started;
                    if (age > o.second->timeoutMs && !o.second->timedOut) late.push_back(o.second);
                    else if (o.second->timedOut && age > (uint64_t)o.second->timeoutMs + 5000) over.push_back(o.second);
                }
                for (auto& m : _mons)
                {
                    std::lock_guard<std::mutex> lk2(m.second->m);
                    if (m.second->readers.empty() && m.second->lingerUntil && now >= m.second->lingerUntil)
                        linger.push_back({m.second->iface, m.second->mode});
                }
            }
            for (auto& o : late)
            {
                o->timedOut = true;
                cancelOp(*o, "time limit");
            }
            for (auto& o : over) finishOp(*o, errorEnvelope("TIMEOUT", "the operation ran past its time limit", {{"timeout_ms", o->timeoutMs}}));
            for (auto& l : linger) stopMonitor(l.first, l.second, "ended");
        }
    }

    void shutdownAll()
    {
        std::vector<std::shared_ptr<OpRec>> ops;
        std::vector<std::shared_ptr<WorkerConn>> ws;
        std::vector<std::pair<std::string, std::string>> mons;
        {
            std::lock_guard<std::mutex> lk(_m);
            for (auto& o : _ops)
                if (!o.second->done) ops.push_back(o.second);
            for (auto& w : _workers) ws.push_back(w.second);
            for (auto& m : _mons) mons.push_back({m.second->iface, m.second->mode});
            for (auto& l : _leases) l.second->ended = true;
        }
        for (auto& o : ops) cancelOp(*o, "serve is stopping");
        for (auto& m : mons) stopMonitor(m.first, m.second, "CANCELLED");
        for (auto& w : ws) w->send({{"t", "bye"}});
        const uint64_t until = monoMs() + 8000;
        for (auto& w : ws)
            while (w->alive && monoMs() < until) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        for (auto& w : ws)
            if (w->alive) w->child.kill();
        for (auto& o : ops)
            if (!o->done) finishOp(*o, errorEnvelope("CANCELLED", "serve stopped"));
    }

    /*******************************************************************
     ***************************** LOGGING *****************************
     *******************************************************************/
    void logLine(const std::string& what, json j)
    {
        j["t"] = isoNow();
        j["ev"] = what;
        std::lock_guard<std::mutex> lk(_logM);
        std::ofstream f(opLogPath(), std::ios::app | std::ios::binary);
        if (f) f << j.dump(-1, ' ', false, json::error_handler_t::replace) << "\n";
    }

    static long selfPid()
    {
#ifdef _WIN32
        return (long)GetCurrentProcessId();
#else
        return (long)::getpid();
#endif
    }

    std::string _self;
    int _port;
    std::string _rulesPath;
    std::string _lang;
    std::string _version;
    std::string _key;
    Rules _rules;
    httplib::Server _svr4, _svr6;
    std::atomic<int> _activeConns{0};

    std::mutex _m;
    std::mutex _logM;
    std::mutex _promptM;
    std::map<std::string, std::shared_ptr<WorkerConn>> _workers;
    std::map<std::string, std::shared_ptr<SharedMon>> _mons; ///< "ip:port/mode"
    std::map<std::string, std::shared_ptr<OpRec>> _ops;
    std::map<std::string, std::deque<std::shared_ptr<OpRec>>> _mgmtQueue;
    std::map<std::string, std::string> _mgmtRunning;
    std::map<std::string, std::shared_ptr<Lease>> _leases;
    std::map<std::string, Session> _sessions;
    std::map<std::string, uint64_t> _links;
    std::map<std::string, std::deque<uint64_t>> _rate;
    std::map<std::string, std::pair<std::string, uint64_t>> _devFamily;
    BenchLock _bench;
    std::string _released;
    uint64_t _opSeq = 0;
    uint64_t _readerSeq = 0;
    McpServer _mcp;
};

/**********************************************************************
 ************************** SUBCOMMANDS *****************************
 **********************************************************************/
namespace
{
/** @brief Call the running serve with the key; empty optional-like result on no answer. */
bool callServe(int port, const std::string& method, const std::string& path, const json& body, json& out, std::string& err)
{
    const std::string key = loadKey(false, err);
    if (key.empty()) return false;
    httplib::Client c("127.0.0.1", port);
    c.set_connection_timeout(1, 0);
    c.set_read_timeout(10, 0);
    httplib::Headers h = {{"X-Oknx-Key", key}, {"X-Oknx-Caller", "oknx-cli"}};
    httplib::Result r = method == "POST"     ? c.Post(path, h, body.dump(), "application/json")
                        : method == "DELETE" ? c.Delete(path, h)
                                             : c.Get(path, h);
    if (!r)
    {
        err = "serve does not answer on 127.0.0.1:" + std::to_string(port);
        return false;
    }
    out = json::parse(r->body, nullptr, false);
    if (!out.is_object())
    {
        err = "serve sent no JSON";
        return false;
    }
    return true;
}

int usageServe()
{
    std::fprintf(stderr,
                 "usage: oknx serve [--port N] [--rules <file>] [--detach]\n"
                 "       oknx serve stop | status | open [--open] | logout | rekey | lease rm <id> | describe [--markdown|--json]\n");
    return 2;
}

bool detachSelf(const std::string& self, const std::vector<std::string>& args, std::string& err)
{
    std::vector<std::string> a = {"serve"};
    for (const auto& x : args)
        if (x != "--detach") a.push_back(x);
    Child c;
    if (!spawnChild(selfExePath(self), a, serveLogPath(), c, err, true)) return false;
    std::printf("serve started in the background (pid %ld); log: %s\n", c.pidValue(), serveLogPath().c_str());
    pipeClose(c.toChild); // its stdin closes; serve does not read it
    pipeClose(c.fromChild);
    return true;
}
} // namespace

int serveMain(const std::string& selfPath, const std::vector<std::string>& args, const std::string& lang,
              const std::string& version)
{
    int port = SERVE_PORT_DEFAULT;
    std::string rules = rulesPathDefault();
    bool detach = false;
    std::vector<std::string> rest;
    for (size_t i = 0; i < args.size(); ++i)
    {
        if (args[i] == "--port" && i + 1 < args.size()) port = std::atoi(args[++i].c_str());
        else if (args[i] == "--rules" && i + 1 < args.size())
            rules = args[++i];
        else if (args[i] == "--detach")
            detach = true;
        else
            rest.push_back(args[i]);
    }
    if (port <= 0 || port > 65535) return usageServe();
    std::string err;
    json out;
    if (rest.empty())
    {
        if (detach) return detachSelf(selfPath, args, err) ? 0 : (std::fprintf(stderr, "serve: %s\n", err.c_str()), 1);
        Leader l(selfPath, port, rules, lang, version);
        return l.run();
    }
    const std::string cmd = rest[0];
    if (cmd == "describe")
    {
        const std::string fmt = rest.size() > 1 ? rest[1] : std::string();
        const bool de = lang == "de";
        if (fmt == "--json")
        {
            json a = json::array();
            for (const auto& d : opTable())
                a.push_back({{"name", d.name}, {"title", de ? d.titleDe : d.titleEn}, {"help", de ? d.helpDe : d.helpEn},
                             {"class", opClassName(d.cls)}, {"runs_in", d.where == OpWhere::Worker ? "worker" : "leader"},
                             {"changes", d.changes}, {"progress", d.progress}, {"timeout_ms", d.timeoutMs}, {"input", d.input}});
            std::printf("%s\n", a.dump(1).c_str());
            return 0;
        }
        const bool md = fmt == "--markdown";
        for (const auto& d : opTable())
        {
            const json props = d.input.value("properties", json::object());
            const json req = d.input.value("required", json::array());
            if (md)
            {
                std::printf("### `%s` — %s\n\n%s\n\n", d.name.c_str(), (de ? d.titleDe : d.titleEn).c_str(), (de ? d.helpDe : d.helpEn).c_str());
                std::printf("%s: `%s` · %s · %s: %u s%s\n\n", de ? "Klasse" : "Class", opClassName(d.cls),
                            d.where == OpWhere::Worker ? (de ? "im Arbeiter" : "in the worker") : (de ? "im Leitprozess" : "in the leader"),
                            de ? "Zeitgrenze" : "time limit", (unsigned)(d.timeoutMs / 1000),
                            d.progress ? (de ? " · meldet Fortschritt" : " · reports progress") : "");
                if (!props.empty())
                {
                    std::printf("| %s | %s | %s |\n|---|---|---|\n", de ? "Argument" : "Argument", de ? "Typ / Bereich" : "type / range",
                                de ? "Bedeutung" : "meaning");
                    for (auto it = props.begin(); it != props.end(); ++it)
                    {
                        const json& p = it.value();
                        std::string type = p.value("type", std::string());
                        if (p.contains("enum")) type += " " + p["enum"].dump();
                        if (p.contains("minimum")) type += " " + std::to_string(p["minimum"].get<long long>()) + ".." + std::to_string(p["maximum"].get<long long>());
                        if (p.contains("format")) type += " (" + p["format"].get<std::string>() + ")";
                        bool must = false;
                        for (const auto& r : req)
                            if (r == it.key()) must = true;
                        std::printf("| `%s`%s | %s | %s |\n", it.key().c_str(), must ? " *" : "", type.c_str(), p.value("description", std::string()).c_str());
                    }
                    std::printf("\n");
                }
            }
            else
            {
                std::string argl;
                for (auto it = props.begin(); it != props.end(); ++it)
                {
                    bool must = false;
                    for (const auto& r : req)
                        if (r == it.key()) must = true;
                    argl += (argl.empty() ? "" : " ") + (must ? it.key() : "[" + it.key() + "]");
                }
                std::printf("%-20s %-14s %s%s\n    %s\n", d.name.c_str(), opClassName(d.cls), d.changes ? "CHANGES  " : "", argl.c_str(),
                            (de ? d.helpDe : d.helpEn).c_str());
            }
        }
        return 0;
    }
    if (cmd == "stop")
    {
        if (!callServe(port, "POST", "/api/serve/stop", json::object(), out, err)) return std::fprintf(stderr, "serve: %s\n", err.c_str()), 1;
        std::printf("serve: stopping\n");
        return 0;
    }
    if (cmd == "status")
    {
        if (!callServe(port, "GET", "/api/health", json::object(), out, err)) return std::fprintf(stderr, "serve: %s\n", err.c_str()), 1;
        std::printf("%s\n", out.dump(2).c_str());
        return 0;
    }
    if (cmd == "open")
    {
        const bool launch = rest.size() > 1 && rest[1] == "--open";
        if (!callServe(port, "POST", "/api/session/link", json::object(), out, err)) return std::fprintf(stderr, "serve: %s\n", err.c_str()), 1;
        const std::string url = out["data"].value("url", std::string());
        std::printf("%s\n(valid 60 s, once)\n", url.c_str());
        if (launch)
        {
#ifdef _WIN32
            Child c;
            std::string e2;
            spawnChild("rundll32.exe", {"url.dll,FileProtocolHandler", url}, std::string(), c, e2);
#elif defined(__APPLE__)
            Child c;
            std::string e2;
            spawnChild("/usr/bin/open", {url}, std::string(), c, e2);
#else
            Child c;
            std::string e2;
            spawnChild("/usr/bin/xdg-open", {url}, std::string(), c, e2);
#endif
        }
        return 0;
    }
    if (cmd == "logout")
    {
        if (!callServe(port, "POST", "/api/session/logout", json::object(), out, err)) return std::fprintf(stderr, "serve: %s\n", err.c_str()), 1;
        std::printf("serve: %d browser session(s) ended\n", out["data"].value("ended", 0));
        return 0;
    }
    if (cmd == "rekey")
    {
        // A running serve makes and switches to the new key itself; otherwise the file is replaced here.
        if (callServe(port, "POST", "/api/serve/rekey", json::object(), out, err) && out.value("ok", false))
        {
            std::printf("serve: new key in %s, in use now; connect Claude again with it\n", keyPath().c_str());
            return 0;
        }
        const std::string k = newKey(err);
        if (k.empty()) return std::fprintf(stderr, "serve: %s\n", err.c_str()), 1;
        std::printf("serve: new key in %s; connect Claude again with it\n", keyPath().c_str());
        return 0;
    }
    if (cmd == "lease" && rest.size() == 3 && rest[1] == "rm")
    {
        if (!callServe(port, "DELETE", "/api/lease/" + rest[2], json::object(), out, err)) return std::fprintf(stderr, "serve: %s\n", err.c_str()), 1;
        std::printf("serve: %s\n", out.value("ok", false) ? "reservation ended" : "no such reservation");
        return out.value("ok", false) ? 0 : 1;
    }
    return usageServe();
}

} // namespace serve
} // namespace ftc

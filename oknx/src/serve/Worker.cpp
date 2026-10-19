/**
 * @file        Worker.cpp
 * @brief       The worker process behind `oknx serve`: one per interface, jobs in, events out, over one pipe.
 * @details     Threads: a reader (jobs, cancels, monitor requests), the management thread (g_knxTunnel and the
 *              FTC client, one job at a time, the tunnel closed after 5 s without a job), one thread per monitor,
 *              and the writer in EventWriter. The pipe closing ends the worker: every connection gets a
 *              DISCONNECT_REQUEST and the state file is emptied.
 * @date        2026-10-19
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#include "core/Discovery.h" // winsock2 before anything that pulls in windows.h

#include "FileTransferClient.h"
#include "knx_ip_tunnel.h"
#include "cli/I18n.h"
#include "cli/Monitor.h"
#include "cli/Templates.h"
#include "cli/Term.h"
#include "cli/Theme.h"
#include "core/DeviceMgmt.h"
#include "serve/FrameJson.h"
#include "serve/Paths.h"
#include "serve/Proto.h"
#include "serve/Worker.h"
#include "serve/WorkerOps.h"

#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ftc
{
namespace serve
{

namespace
{

/** @brief Lines the FTC client narrates during the running job; the hook runs in the management thread. */
std::vector<std::string>* g_jobLines = nullptr;
static constexpr size_t JOB_LINES_MAX = 400;

bool workerLineHook(const std::string& line, uint8_t)
{
    if (g_jobLines && g_jobLines->size() < JOB_LINES_MAX) g_jobLines->push_back(line);
    return true; // never printed: the worker has no terminal
}

/** @brief Send one DISCONNECT_REQUEST for @p ch from local port @p localPort; true when the interface confirmed it. */
bool releaseChannel(const std::string& ip, uint16_t port, uint8_t ch, uint16_t localPort, bool& portBusy)
{
    using namespace detail;
    portBusy = false;
    socket_t s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (!sockValid(s)) return false;
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(localPort);
    if (::bind(s, (sockaddr*)&local, sizeof(local)) != 0)
    {
        portBusy = true; // a foreign interface may check the port; say so instead of pretending
        local.sin_port = 0;
        if (::bind(s, (sockaddr*)&local, sizeof(local)) != 0)
        {
            sockClose(s);
            return false;
        }
    }
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &dst.sin_addr) != 1)
    {
        sockClose(s);
        return false;
    }
    uint8_t pkt[16] = {0x06, 0x10, 0x02, 0x09, 0x00, 0x10, ch, 0x00, 0x08, 0x01, 0, 0, 0, 0,
                       (uint8_t)(localPort >> 8), (uint8_t)(localPort & 0xFF)};
    ::sendto(s, (const char*)pkt, sizeof(pkt), 0, (sockaddr*)&dst, sizeof(dst));
    bool confirmed = false;
    const uint64_t until = monoMs() + 1000;
    while (!confirmed && monoMs() < until)
    {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(s, &rf);
        timeval tv{0, 100000};
        if (::select((int)s + 1, &rf, nullptr, nullptr, &tv) <= 0) continue;
        uint8_t buf[64];
        const int n = (int)::recv(s, (char*)buf, sizeof(buf), 0);
        if (n >= 8 && buf[2] == 0x02 && buf[3] == 0x0A && buf[6] == ch) confirmed = true; // DISCONNECT_RESPONSE
    }
    sockClose(s);
    return confirmed;
}

} // namespace

/**********************************************************************
 ******************************* STATE ******************************
 **********************************************************************/
/** @brief One open connection as the state file records it. */
struct ConnEntry
{
    std::string kind; // mgmt · bm · gm
    uint8_t ch = 0;
    uint16_t localPort = 0;
    uint64_t lastSendWallMs = 0;
};

/**********************************************************************
 ****************************** WORKER ******************************
 **********************************************************************/
class WorkerProc : public WorkerApi
{
  public:
    WorkerProc(const std::string& ip, uint16_t port, PipeEnd in, PipeEnd out)
        : _ip(ip), _port(port), _in(in), _w(out), _statePath(workerStatePath(ip, port))
    {
    }

    int run()
    {
        json released = releaseStale();
        _w.control({{"t", "hello"}, {"pid", (long)processId()}, {"ip", _ip}, {"port", _port}, {"released", released}});
        writeState();

        std::thread mgmt([this] { mgmtLoop(); });
        std::thread state([this] { stateLoop(); });
        readerLoop(); // returns when the pipe closed or the leader said bye

        _quit = true;
        cancelAll();
        _jcv.notify_all();
        mgmt.join();
        stopAllMonitors();
        state.join();
        _conns.clear();
        writeState(); // empty: nothing of ours is open any more
        _w.stop();
        return 0;
    }

    // --- WorkerApi (used by the operations in WorkerOps.h; management thread only) ---------------------
    const std::string& ip() const override { return _ip; }
    uint16_t port() const override { return _port; }
    void progress(const Job& job, const json& data) override
    {
        _w.control({{"t", "progress"}, {"id", job.id}, {"seq", ++_opSeq}, {"data", data}});
    }
    bool ensureTunnel(json& error) override
    {
        if (g_knxTunnel.connected()) return true;
        if (busmonActive())
        {
            error = errorJson("BUSY", "a bus monitor holds this interface", {{"holder", "serve-busmon"}});
            return false;
        }
        if (!g_knxTunnel.connect(_ip, _port))
        {
            const int st = g_knxTunnel.lastConnectStatus();
            char hx[8];
            std::snprintf(hx, sizeof(hx), "0x%02X", st < 0 ? 0 : st);
            error = errorJson("CONNECT_FAILED",
                              st < 0 ? std::string("no CONNECT_RESPONSE") : std::string("CONNECT_RESPONSE ") + hx,
                              {{"status", st < 0 ? json() : json(hx)}, {"outside_serve", st == 0x24}});
            return false;
        }
        noteMgmtConn();
        return true;
    }
    void closeTunnel() override { closeMgmt(); }
    void prepareFtc() override
    {
        if (!_ftcSetup)
        {
            openknxFileTransferClient.setup(true);
            _ftcSetup = true;
        }
        // the interface's max APDU drives the FTC framing, as on the command line; asked once per worker
        if (_apdu == 0) _apdu = queryMaxApduDeviceMgmt(_ip, _port, nullptr, nullptr, 0);
        if (_apdu) openknxFileTransferClient.setApduHint(_apdu);
    }
    void setJobLines(std::vector<std::string>* v) override { g_jobLines = v; }
    /** @brief Refresh the management entry of the state file; cheap, call it from every pump loop. */
    void pumpTick() override
    {
        if (!g_knxTunnel.connected() || monoMs() - _lastMgmtNote < 1000) return;
        _lastMgmtNote = monoMs();
        std::lock_guard<std::mutex> lk(_sm);
        auto it = _conns.find("mgmt");
        if (it != _conns.end()) it->second.lastSendWallMs = g_knxTunnel.lastSendWallMs();
    }

  private:
    // --- reader ------------------------------------------------------------------------------------
    void readerLoop()
    {
        LineReader rd(_in);
        std::string line;
        while (!_quit && rd.next(line))
        {
            json m = json::parse(line, nullptr, false);
            if (!m.is_object()) continue;
            const std::string t = m.value("t", std::string());
            if (t == "job")
            {
                Job j;
                j.id = m.value("id", std::string());
                j.op = m.value("op", std::string());
                j.args = m.value("args", json::object());
                j.cancel = std::make_shared<std::atomic<bool>>(false);
                {
                    std::lock_guard<std::mutex> lk(_jm);
                    _cancels[j.id] = j.cancel;
                    _jobs.push_back(std::move(j));
                }
                _jcv.notify_one();
            }
            else if (t == "cancel")
            {
                std::lock_guard<std::mutex> lk(_jm);
                auto it = _cancels.find(m.value("id", std::string()));
                if (it != _cancels.end()) *it->second = true;
            }
            else if (t == "mon")
            {
                const std::string act = m.value("act", std::string());
                const std::string mode = m.value("mode", std::string());
                if (act == "start") startMonitor(mode, m.value("raw", false));
                else if (act == "stop")
                    stopMonitor(mode);
            }
            else if (t == "mgmt_close") // a reservation for a busmonitor elsewhere: give the idle tunnel back now
            {
                std::unique_lock<std::mutex> lk(_jm);
                if (_running.empty() && _jobs.empty())
                {
                    _closeIdle = true;
                    _jcv.notify_all();
                    _closeIdleDone.wait_for(lk, std::chrono::seconds(3), [this] { return !_closeIdle; });
                }
                lk.unlock();
                _w.control({{"t", "mgmt_closed"}, {"id", m.value("id", std::string())}});
            }
            else if (t == "bye")
                break;
        }
    }

    // --- management thread -------------------------------------------------------------------------
    void mgmtLoop()
    {
        OpenKNX::Log::Logger::setLineHook(&workerLineHook);
        uint64_t lastJobEnd = monoMs();
        while (!_quit)
        {
            Job job;
            bool have = false;
            {
                std::unique_lock<std::mutex> lk(_jm);
                const auto idle = std::chrono::milliseconds(g_knxTunnel.connected() ? 20 : 200);
                _jcv.wait_for(lk, idle, [this] { return _quit || !_jobs.empty() || _closeIdle; });
                if (_closeIdle && _jobs.empty())
                {
                    closeMgmt();
                    _closeIdle = false;
                    _closeIdleDone.notify_all();
                }
                if (!_jobs.empty())
                {
                    job = std::move(_jobs.front());
                    _jobs.pop_front();
                    have = true;
                    _running = job.id;
                }
            }
            if (have)
            {
                runJob(job);
                std::lock_guard<std::mutex> lk(_jm);
                _cancels.erase(job.id);
                _running.clear();
                lastJobEnd = monoMs();
                continue;
            }
            if (g_knxTunnel.connected())
            {
                g_knxTunnel.pump();
                openknxFileTransferClient.loop(true);
                pumpTick();
                if (monoMs() - lastJobEnd > 5000) closeMgmt(); // 5 s without a job: give the slot back
            }
            else
                closeMgmt(); // dropped by itself: no stale entry in the state file
        }
        closeMgmt();
        OpenKNX::Log::Logger::setLineHook(nullptr);
    }

    void runJob(const Job& job)
    {
        _opSeq = 0;
        json res;
        const uint64_t t0 = monoMs();
        if (*job.cancel)
            res = {{"ok", false}, {"error", errorJson("CANCELLED", "cancelled before it started", json::object())}};
        else
            res = runWorkerOp(*this, job);
        res["t"] = "result";
        res["id"] = job.id;
        res["op"] = job.op;
        res["ms"] = monoMs() - t0;
        res["seq"] = ++_opSeq;
        res["last"] = _opSeq;
        _w.control(res);
    }

    void closeMgmt()
    {
        if (g_knxTunnel.connected()) g_knxTunnel.disconnect();
        std::lock_guard<std::mutex> lk(_sm);
        if (_conns.erase("mgmt")) _stateNow = true; // also after the tunnel dropped by itself
    }

    void noteMgmtConn()
    {
        std::lock_guard<std::mutex> lk(_sm);
        _conns["mgmt"] = {"mgmt", g_knxTunnel.channelId(), g_knxTunnel.localPort(), g_knxTunnel.lastSendWallMs()};
        _stateNow = true;
    }

    void cancelAll()
    {
        std::lock_guard<std::mutex> lk(_jm);
        for (auto& c : _cancels) *c.second = true;
    }

    // --- monitors ----------------------------------------------------------------------------------
    struct Mon
    {
        std::thread thr;
        std::atomic<bool> stop{false};
        std::atomic<bool> raw{false};
        bool bus = false;
    };

    bool busmonActive()
    {
        std::lock_guard<std::mutex> lk(_mm);
        return _mons.count("bm") != 0;
    }

    void startMonitor(const std::string& mode, bool raw)
    {
        if (mode != "bm" && mode != "gm")
        {
            _w.streamEvent(mode, {{"t", "mon"}, {"ev", "fail"}, {"code", "INVALID_ARGUMENT"}}, true);
            return;
        }
        {
            std::lock_guard<std::mutex> lk(_mm);
            auto it = _mons.find(mode);
            if (it != _mons.end())
            {
                it->second->raw = it->second->raw || raw;
                _w.streamEvent(mode, {{"t", "mon"}, {"ev", "already"}});
                return;
            }
        }
        if (mode == "bm")
        {
            // A busmonitor and a management tunnel exclude each other on one interface (03_08_04 2.2.4).
            std::unique_lock<std::mutex> lk(_jm);
            if (!_running.empty())
            {
                lk.unlock();
                _w.streamEvent(mode, {{"t", "mon"}, {"ev", "fail"}, {"code", "BUSY"}, {"holder", "serve-op:" + _running}}, true);
                return;
            }
            _closeIdle = true;
            _jcv.notify_all();
            _closeIdleDone.wait_for(lk, std::chrono::seconds(3), [this] { return !_closeIdle; });
        }
        auto m = std::make_unique<Mon>();
        m->bus = mode == "bm";
        m->raw = raw;
        Mon* mp = m.get();
        std::lock_guard<std::mutex> lk(_mm);
        mp->thr = std::thread([this, mp, mode] { monitorLoop(*mp, mode); });
        _mons[mode] = std::move(m);
    }

    void stopMonitor(const std::string& mode)
    {
        std::unique_ptr<Mon> m;
        {
            std::lock_guard<std::mutex> lk(_mm);
            auto it = _mons.find(mode);
            if (it == _mons.end()) return;
            m = std::move(it->second);
            _mons.erase(it);
        }
        m->stop = true;
        if (m->thr.joinable()) m->thr.join();
    }

    void stopAllMonitors()
    {
        std::vector<std::string> modes;
        {
            std::lock_guard<std::mutex> lk(_mm);
            for (auto& m : _mons) modes.push_back(m.first);
        }
        for (const auto& md : modes) stopMonitor(md);
    }

    void monitorLoop(Mon& m, const std::string& stream)
    {
        Term term;
        Theme theme(term);
        I18n i18n;
        Tpl tpl(term, theme, &i18n);
        Monitor mon(term, theme, tpl, i18n);
        mon.target(_ip, _port);
        mon.setQuietVerbose(true, false);
        const Monitor::Mode mode = m.bus ? Monitor::Mode::Bus : Monitor::Mode::Group;
        mon.sink([&](const MonFrame& f) { _w.frame(stream, {{"f", frameToJson(f)}}); });
        mon.rawSink([&](const uint8_t* c, int n) {
            if (m.raw) _w.frame(stream + ".raw", {{"hex", hexOf(c, n)}, {"ts", wallMs()}});
        });
        std::string err;
        if (!mon.openTunnel(mode, err))
        {
            _w.streamEvent(stream, {{"t", "mon"}, {"ev", "fail"}, {"code", "CONNECT_FAILED"}, {"reason", err}}, true);
            // Leave the list on our own: this thread detaches itself and touches nothing of `m` afterwards.
            std::lock_guard<std::mutex> lk(_mm);
            auto it = _mons.find(stream);
            if (it != _mons.end() && it->second.get() == &m)
            {
                it->second->thr.detach();
                _mons.erase(it);
            }
            return;
        }
        auto note = [&] {
            std::lock_guard<std::mutex> lk(_sm);
            _conns[stream] = {stream, mon.channel(), mon.localPort(), mon.lastSendWallMs()};
        };
        note();
        {
            std::lock_guard<std::mutex> lk(_sm);
            _stateNow = true;
        }
        _w.streamEvent(stream, {{"t", "mon"}, {"ev", "up"}, {"ch", mon.channel()}, {"pa", mon.assignedPA()},
                                {"ms", monoMs()}});
        uint64_t lastNote = monoMs();
        bool down = false;
        uint64_t backoff = 500, nextTry = 0;
        while (!m.stop && !_quit)
        {
            if (!down)
            {
                if (!mon.tick(mode))
                {
                    down = true;
                    mon.flushBus();
                    _w.streamEvent(stream, {{"t", "mon"}, {"ev", "down"}, {"ms", monoMs()}});
                    {
                        std::lock_guard<std::mutex> lk(_sm);
                        _conns.erase(stream);
                        _stateNow = true;
                    }
                    nextTry = monoMs() + backoff;
                }
                else if (monoMs() - lastNote >= 1000)
                {
                    note();
                    lastNote = monoMs();
                }
                continue;
            }
            if (monoMs() < nextTry)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            if (mon.reopen(mode, err))
            {
                down = false;
                backoff = 500;
                note();
                {
                    std::lock_guard<std::mutex> lk(_sm);
                    _stateNow = true;
                }
                _w.streamEvent(stream, {{"t", "mon"}, {"ev", "up"}, {"ch", mon.channel()}, {"pa", mon.assignedPA()},
                                        {"ms", monoMs()}});
            }
            else
            {
                if (backoff < 8000) backoff *= 2;
                nextTry = monoMs() + backoff;
            }
        }
        mon.flushBus();
        mon.closeTunnel();
        {
            std::lock_guard<std::mutex> lk(_sm);
            _conns.erase(stream);
            _stateNow = true;
        }
        _w.streamEvent(stream, {{"t", "mon"}, {"ev", "end"}, {"ms", monoMs()}}, true);
    }

    // --- state file ----------------------------------------------------------------------------------
    void stateLoop()
    {
        uint64_t lastWrite = 0;
        while (!_quit)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            bool now = false;
            {
                std::lock_guard<std::mutex> lk(_sm);
                now = _stateNow;
            }
            // Opening or closing a connection is written at once; the send times at most every 5 s, so the
            // stored time is never later than the real one.
            if (now || monoMs() - lastWrite >= 5000)
            {
                writeState();
                lastWrite = monoMs();
            }
        }
    }

    void writeState()
    {
        std::string body;
        {
            std::lock_guard<std::mutex> lk(_sm);
            json arr = json::array();
            for (const auto& c : _conns)
                arr.push_back({{"kind", c.second.kind},
                               {"ch", c.second.ch},
                               {"localPort", c.second.localPort},
                               {"lastSendWallMs", c.second.lastSendWallMs}});
            body = json({{"pid", (long)processId()}, {"ip", _ip}, {"port", _port}, {"written", wallMs()}, {"conns", arr}})
                       .dump(1);
            _stateNow = false;
        }
        const std::string tmp = _statePath + ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) return;
            f << body;
        }
        std::remove(_statePath.c_str());
        std::rename(tmp.c_str(), _statePath.c_str());
    }

    /** @brief A previous worker for this interface died with connections open: close those still alive. */
    json releaseStale()
    {
        json out = json::array();
        std::ifstream f(_statePath, std::ios::binary);
        if (!f) return out;
        std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        json st = json::parse(body, nullptr, false);
        if (!st.is_object() || !st.contains("conns") || !st["conns"].is_array()) return out;
        for (const auto& c : st["conns"])
        {
            const uint64_t last = c.value("lastSendWallMs", (uint64_t)0);
            const uint8_t ch = c.value("ch", (uint8_t)0);
            const uint16_t lp = c.value("localPort", (uint16_t)0);
            const uint64_t age = wallMs() > last ? wallMs() - last : 0;
            json r = {{"kind", c.value("kind", std::string())}, {"ch", ch}, {"age_ms", age}};
            // The interface ends a connection 120 s after the last frame it got (03_08_02 5.4); past 110 s the
            // channel may already belong to someone else, so it is left alone.
            if (ch == 0 || last == 0 || age >= 110000)
                r["action"] = "skipped";
            else
            {
                bool portBusy = false;
                const bool ok = releaseChannel(_ip, _port, ch, lp, portBusy);
                r["action"] = ok ? "released" : "unconfirmed";
                if (portBusy) r["port_busy"] = true;
            }
            out.push_back(r);
        }
        return out;
    }

    static long processId()
    {
#ifdef _WIN32
        return (long)GetCurrentProcessId();
#else
        return (long)::getpid();
#endif
    }

    std::string _ip;
    uint16_t _port;
    PipeEnd _in;
    EventWriter _w;
    std::string _statePath;
    std::atomic<bool> _quit{false};

    std::mutex _jm;
    std::condition_variable _jcv;
    std::condition_variable _closeIdleDone;
    std::deque<Job> _jobs;
    std::map<std::string, std::shared_ptr<std::atomic<bool>>> _cancels;
    std::string _running;
    bool _closeIdle = false;
    uint64_t _opSeq = 0;
    bool _ftcSetup = false;
    uint16_t _apdu = 0;

    std::mutex _mm;
    std::map<std::string, std::unique_ptr<Mon>> _mons;

    std::mutex _sm;
    std::map<std::string, ConnEntry> _conns;
    bool _stateNow = false;
    uint64_t _lastMgmtNote = 0;
};

int workerMain(int argc, char** argv)
{
    std::string ip;
    uint16_t port = 3671;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if ((a == "-i" || a == "--ip") && i + 1 < argc) ip = argv[++i];
        else if (a == "--port" && i + 1 < argc)
            port = (uint16_t)std::atoi(argv[++i]);
    }
#ifdef _WIN32
    SetConsoleCtrlHandler(nullptr, TRUE); // only the leader ends a worker
#else
    std::signal(SIGINT, SIG_IGN);
    std::signal(SIGPIPE, SIG_IGN);
#endif
    PipeEnd in = PIPE_NONE, out = PIPE_NONE;
    if (ip.empty() || !claimProtocolPipes(in, out))
    {
        std::fprintf(stderr, "worker: needs -i <ip> and the pipe from oknx serve\n");
        return 2;
    }
    WorkerProc w(ip, port, in, out);
    return w.run();
}

} // namespace serve
} // namespace ftc

/**
 * @file        LeaderState.h
 * @brief       What the leader of `oknx serve` keeps: workers, shared monitors and their readers, operations,
 *              reservations, sessions.
 * @details     The leader never opens a KNXnet/IP socket. A worker per interface does; the leader starts it,
 *              reads its events in one thread per worker, hands telegrams to the readers of a shared monitor and
 *              results to their operations. Each reader has its own bounded queue: on overflow its oldest
 *              telegrams go, it gets `lost` with the count, and after 30 s of overflow it is closed.
 * @date        2026-10-19
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include "serve/Http.h"
#include "serve/Ops.h"
#include "serve/Pipe.h"
#include "serve/Proto.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace ftc
{
namespace serve
{

inline std::string ifaceKey(const std::string& ip, uint16_t port) { return ip + ":" + std::to_string((unsigned)port); }

/**********************************************************************
 ***************************** READERS ******************************
 **********************************************************************/
/** @brief One consumer of a shared monitor (an SSE client, a `monitor` or `compare` operation, the CLI). */
struct Reader
{
    static constexpr size_t QUEUE_MAX = 4000;
    static constexpr uint64_t OVERFLOW_CLOSE_MS = 30000;

    uint64_t id = 0;
    bool raw = false;      ///< raw cEMI (viewer) instead of decoded telegrams
    std::string who;       ///< for bench_state: "sse", "op:<id>", "cli:<pid>"
    uint64_t since = 0;

    std::mutex m;
    std::condition_variable cv;
    std::deque<json> q;    ///< frame / lost / mon events in arrival order
    uint64_t lost = 0;     ///< telegrams this reader lost to its own overflow
    uint64_t lostPending = 0; ///< not yet reported as an event
    uint64_t overflowSince = 0;
    bool closed = false;
    std::string closeCode; ///< CANCELLED / overflow / worker_ended / bench_locked / ended

    /** @brief Queue an event; on overflow drop the oldest telegram and count it. */
    void push(json ev)
    {
        {
            std::lock_guard<std::mutex> lk(m);
            if (closed) return;
            if (q.size() >= QUEUE_MAX)
            {
                for (auto it = q.begin(); it != q.end(); ++it)
                    if (it->value("t", std::string()) == "frame")
                    {
                        q.erase(it);
                        lost++;
                        lostPending++;
                        break;
                    }
                if (!overflowSince) overflowSince = monoMs();
                else if (monoMs() - overflowSince > OVERFLOW_CLOSE_MS)
                {
                    closed = true;
                    closeCode = "overflow";
                }
            }
            else if (q.size() < QUEUE_MAX / 2)
                overflowSince = 0;
            q.push_back(std::move(ev));
        }
        cv.notify_all();
    }
    void close(const std::string& code)
    {
        {
            std::lock_guard<std::mutex> lk(m);
            if (closed) return;
            closed = true;
            closeCode = code;
        }
        cv.notify_all();
    }
    /** @brief Take what is queued (up to @p max), waiting up to @p waitMs; a pending loss comes first. */
    std::vector<json> take(size_t max, uint64_t waitMs, bool& isClosed, std::string& code)
    {
        std::vector<json> out;
        std::unique_lock<std::mutex> lk(m);
        cv.wait_for(lk, std::chrono::milliseconds(waitMs), [this] { return closed || !q.empty() || lostPending; });
        if (lostPending)
        {
            out.push_back({{"t", "lost"}, {"count", lostPending}, {"reason", "reader"}});
            lostPending = 0;
        }
        while (!q.empty() && out.size() < max)
        {
            out.push_back(std::move(q.front()));
            q.pop_front();
        }
        isClosed = closed && q.empty();
        code = closeCode;
        return out;
    }
};

/** @brief One monitor of one interface, shared by all its readers. */
struct SharedMon
{
    enum State
    {
        Starting,
        Up,
        Down,
        Ended,
        Failed
    };
    std::string iface; ///< ip:port
    std::string mode;  ///< gm | bm
    State state = Starting;
    json failInfo;
    uint64_t since = 0;
    uint64_t lingerUntil = 0; ///< no readers left: stop at this time (3 s after the last one left)
    uint64_t frames = 0;
    uint64_t lostPipe = 0;
    bool rawRequested = false;

    std::mutex m;
    std::condition_variable cv;
    std::vector<std::shared_ptr<Reader>> readers;

    void broadcast(const json& ev, bool rawStream)
    {
        std::vector<std::shared_ptr<Reader>> rs;
        {
            std::lock_guard<std::mutex> lk(m);
            rs = readers;
        }
        for (auto& r : rs)
            if (r->raw == rawStream) r->push(ev);
    }
    void broadcastAll(const json& ev)
    {
        std::vector<std::shared_ptr<Reader>> rs;
        {
            std::lock_guard<std::mutex> lk(m);
            rs = readers;
        }
        for (auto& r : rs) r->push(ev);
    }
    size_t readerCount()
    {
        std::lock_guard<std::mutex> lk(m);
        size_t n = 0;
        for (auto& r : readers)
            if (!r->closed) n++;
        return n;
    }
};

/**********************************************************************
 **************************** OPERATIONS ****************************
 **********************************************************************/
/** @brief One operation call: its arguments (secrets removed), its events, its result. */
struct OpRec
{
    std::string id;
    std::string name;
    json args;      ///< as given, with secret arguments replaced by "***"
    json argsFull;  ///< as given (never logged, never returned)
    Access access;
    std::string iface; ///< ip:port it runs on ("" for net/passive)
    uint64_t submitted = 0, started = 0, ended = 0;
    uint64_t wallSubmitted = 0; ///< wall clock (ms since 1970) when it came in, for the pages
    bool done = false;
    json result;    ///< the envelope {v, ok, op, id, ms, data | error}
    std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);
    uint32_t timeoutMs = 30000;
    bool timedOut = false;
    bool onWorker = false;

    std::mutex m;
    std::condition_variable cv;
    std::vector<json> events; ///< progress events, for SSE subscribers
    uint64_t lastProgressMs = 0;

    void addEvent(const json& e)
    {
        {
            std::lock_guard<std::mutex> lk(m);
            if (events.size() < 2000) events.push_back(e);
            lastProgressMs = monoMs();
        }
        cv.notify_all();
    }
    void finish(const json& envelope)
    {
        {
            std::lock_guard<std::mutex> lk(m);
            if (done) return;
            done = true;
            ended = monoMs();
            result = envelope;
        }
        cv.notify_all();
    }
    bool waitDone(uint64_t ms)
    {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, std::chrono::milliseconds(ms), [this] { return done; });
    }
};

/**********************************************************************
 ***************************** WORKERS ******************************
 **********************************************************************/
struct WorkerConn
{
    std::string ip;
    uint16_t port = 3671;
    Child child;
    std::thread reader;
    std::mutex wm; ///< one writer at a time
    std::atomic<bool> alive{false};
    std::atomic<bool> ready{false};
    json hello;
    uint64_t since = 0;
    std::mutex hm;
    std::condition_variable hcv;
    std::set<std::string> closed; ///< ids of the mgmt_close requests the worker has answered (under hm)

    bool send(const json& j)
    {
        const std::string line = j.dump() + "\n";
        std::lock_guard<std::mutex> lk(wm);
        if (!alive) return false;
        return pipeWriteAll(child.toChild, line.data(), line.size());
    }
};

/** @brief A reservation of the command line: valid while its HTTP request stays open. */
struct Lease
{
    struct Item
    {
        std::string ip;
        uint16_t port = 3671;
        std::string kind; ///< mgmt | gm | bm
    };
    std::string id;
    std::vector<Item> items;
    std::string purpose;
    long pid = 0;
    uint64_t since = 0;
    std::atomic<bool> ended{false};
};

/** @brief A browser session from `oknx serve open`. */
struct Session
{
    std::string id;
    uint64_t created = 0;
    uint64_t lastUse = 0;
};

} // namespace serve
} // namespace ftc

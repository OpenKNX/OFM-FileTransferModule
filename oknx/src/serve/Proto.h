/**
 * @file        Proto.h
 * @brief       JSON lines between `oknx serve` and a worker, and the worker's one writer thread.
 * @details     Every line is one JSON object with a type `t`. Leader -> worker: job, cancel, mon, bye.
 *              Worker -> leader: hello, progress, result, frame, lost, mon, log. Each line of a stream carries
 *              the stream id `s` and a running number `seq`; a stream's last line names its last number, so the
 *              leader can tell "everything arrived" from "something is still in the pipe".
 *
 *              The writer keeps two queues. Control lines (results, progress, monitor state) are never dropped.
 *              Frame lines are bounded: on overflow the oldest frame goes, and before the next line of that
 *              stream the leader gets `lost` with the dropped number range and its time span. A monitor thread
 *              therefore never waits for the pipe.
 * @date        2026-10-19
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include "serve/Pipe.h"
#include "third_party/json.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace ftc
{
namespace serve
{

using json = nlohmann::json;

/** @brief Monotonic milliseconds, the clock both processes share on one machine. */
inline uint64_t monoMs()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/** @brief Wall-clock milliseconds since the epoch (state files, logs). */
inline uint64_t wallMs()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/**
 * @brief The worker's only writer to the pipe.
 */
class EventWriter
{
  public:
    static constexpr size_t FRAME_QUEUE_MAX = 20000;

    explicit EventWriter(PipeEnd out) : _out(out) { _thr = std::thread([this] { run(); }); }
    ~EventWriter() { stop(); }

    /** @brief A line that must arrive: result, progress, monitor state. */
    void control(const json& j)
    {
        std::string line = j.dump(-1, ' ', false, json::error_handler_t::replace);
        line += '\n';
        {
            std::lock_guard<std::mutex> lk(_m);
            _ctrl.push_back(std::move(line));
        }
        _cv.notify_one();
    }

    /**
     * @brief A telegram of stream @p s. Takes the next number of that stream; may be dropped on overflow.
     * @return the number it got.
     */
    uint64_t frame(const std::string& s, json j)
    {
        Item it;
        uint64_t seq = 0;
        {
            std::lock_guard<std::mutex> lk(_m);
            it.stream = s;
            it.seq = ++_seq[s];
            it.ms = monoMs();
            it.droppable = true;
            j["t"] = "frame";
            j["s"] = s;
            j["seq"] = it.seq;
            it.line = j.dump(-1, ' ', false, json::error_handler_t::replace) + "\n";
            if (_frames.size() >= FRAME_QUEUE_MAX) dropOldestLocked();
            seq = it.seq;
            _frames.push_back(std::move(it));
        }
        _cv.notify_one();
        return seq;
    }

    /**
     * @brief A state change of stream @p s (up, gap, end), queued behind its telegrams and never dropped.
     * @details The line gets `seq` = the next number; with @p end also `last` = that number.
     */
    void streamEvent(const std::string& s, json j, bool end = false)
    {
        {
            std::lock_guard<std::mutex> lk(_m);
            Item it;
            it.stream = s;
            it.seq = ++_seq[s];
            it.ms = monoMs();
            it.droppable = false;
            j["s"] = s;
            j["seq"] = it.seq;
            if (end) j["last"] = it.seq;
            it.line = j.dump(-1, ' ', false, json::error_handler_t::replace) + "\n";
            _frames.push_back(std::move(it));
        }
        _cv.notify_one();
    }

    /** @brief Flush what is queued, then end the thread. */
    void stop()
    {
        {
            std::lock_guard<std::mutex> lk(_m);
            if (_stopping) return;
            _stopping = true;
        }
        _cv.notify_one();
        if (_thr.joinable()) _thr.join();
    }

    bool broken() const { return _broken; }

  private:
    struct Item
    {
        std::string stream;
        uint64_t seq = 0;
        uint64_t ms = 0;
        bool droppable = true;
        std::string line;
    };
    struct Drop
    {
        uint64_t from = 0, to = 0, count = 0, t0 = 0, t1 = 0;
    };

    void dropOldestLocked()
    {
        for (auto it = _frames.begin(); it != _frames.end(); ++it)
            if (it->droppable)
            {
                Drop& d = _drops[it->stream];
                if (d.count == 0)
                {
                    d.from = it->seq;
                    d.t0 = it->ms;
                }
                d.to = it->seq;
                d.t1 = it->ms;
                d.count++;
                _frames.erase(it);
                return;
            }
    }

    void run()
    {
        for (;;)
        {
            std::string line;
            {
                std::unique_lock<std::mutex> lk(_m);
                _cv.wait(lk, [this] { return _stopping || !_ctrl.empty() || !_frames.empty(); });
                if (!_ctrl.empty())
                {
                    line = std::move(_ctrl.front());
                    _ctrl.pop_front();
                }
                else if (!_frames.empty())
                {
                    Item it = std::move(_frames.front());
                    _frames.pop_front();
                    auto d = _drops.find(it.stream);
                    if (d != _drops.end() && d->second.count && d->second.to < it.seq)
                    {
                        json lost = {{"t", "lost"},          {"s", it.stream},          {"from", d->second.from},
                                     {"to", d->second.to},   {"count", d->second.count}, {"t0", d->second.t0},
                                     {"t1", d->second.t1}};
                        line = lost.dump() + "\n";
                        _drops.erase(d);
                    }
                    line += it.line;
                }
                else if (_stopping)
                    return;
            }
            if (!_broken && !pipeWriteAll(_out, line.data(), line.size())) _broken = true;
        }
    }

    PipeEnd _out;
    std::thread _thr;
    std::mutex _m;
    std::condition_variable _cv;
    std::deque<std::string> _ctrl;
    std::deque<Item> _frames;
    std::map<std::string, uint64_t> _seq;
    std::map<std::string, Drop> _drops;
    bool _stopping = false;
    bool _broken = false;
};

} // namespace serve
} // namespace ftc

/**
 * @file        AttachClient.h
 * @brief       The command line as one more reader of a busmonitor `oknx serve` holds: the raw cEMI of
 *              /stream, handed to the command line's own Monitor, so the display is the one of a local run.
 * @details     A busmonitor excludes every other tunnel on its interface; when serve holds one for others, the
 *              command line reads along instead of being refused.
 * @date        2026-10-19
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include "serve/HttpLib.h"
#include "serve/JsonGet.h"
#include "serve/Key.h"
#include "third_party/json.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ftc
{
namespace serve
{

class AttachClient
{
  public:
    ~AttachClient() { stop(); }

    /** @brief Open the stream of @p ip:@p port (layer busmon|link); false with @p why when serve refuses. */
    bool start(int servePort, const std::string& ip, uint16_t port, const std::string& layer, std::string& why)
    {
        std::string err;
        const std::string key = loadKey(false, err);
        if (key.empty())
        {
            why = err;
            return false;
        }
        _cli.reset(new httplib::Client("127.0.0.1", servePort));
        _cli->set_connection_timeout(1, 0);
        _cli->set_read_timeout(3600 * 24, 0);
        auto st = _st;
        const std::string path = "/stream?ip=" + ip + "&port=" + std::to_string((unsigned)port) + "&layer=" + layer;
        _thr = std::thread([cli = _cli.get(), st, key, path] {
            std::string buf;
            cli->Get(path, httplib::Headers{{"X-Oknx-Key", key}, {"X-Oknx-Caller", "oknx-cli"}},
                     [st, &buf](const char* d, size_t n) {
                         buf.append(d, n);
                         for (;;)
                         {
                             const size_t end = buf.find("\n\n");
                             if (end == std::string::npos) break;
                             const std::string ev = buf.substr(0, end);
                             buf.erase(0, end + 2);
                             handleEvent(*st, ev);
                         }
                         return !st->stop;
                     });
            std::lock_guard<std::mutex> lk(st->m);
            st->ended = true;
            st->cv.notify_all();
        });
        std::unique_lock<std::mutex> lk(_st->m);
        _st->cv.wait_for(lk, std::chrono::seconds(8), [this] { return _st->hello || _st->ended || !_st->error.empty(); });
        if (_st->hello) return true;
        why = _st->error.empty() ? std::string("serve sent no stream") : _st->error;
        lk.unlock();
        stop();
        return false;
    }

    /** @brief The Monitor feed: one cEMI into @p out, waiting up to @p waitMs; <0 once the stream ended. */
    int read(uint8_t* out, int cap, int waitMs)
    {
        std::unique_lock<std::mutex> lk(_st->m);
        _st->cv.wait_for(lk, std::chrono::milliseconds(waitMs), [this] { return !_st->q.empty() || _st->ended; });
        if (_st->q.empty()) return _st->ended ? -1 : 0;
        std::vector<uint8_t> f = std::move(_st->q.front());
        _st->q.pop_front();
        const int n = (int)f.size() < cap ? (int)f.size() : cap;
        std::memcpy(out, f.data(), (size_t)n);
        return n;
    }

    /** @brief Telegrams that did not reach the display: serve's lost events plus the ones dropped here. */
    uint64_t lost() const
    {
        std::lock_guard<std::mutex> lk(_st->m);
        return _st->lost;
    }

    void stop()
    {
        if (_st) _st->stop = true;
        if (_cli) _cli->stop();
        if (_thr.joinable()) _thr.join();
        _cli.reset();
    }

  private:
    struct Shared
    {
        std::mutex m;
        std::condition_variable cv;
        std::deque<std::vector<uint8_t>> q;
        bool hello = false, ended = false;
        uint64_t lost = 0;
        std::atomic<bool> stop{false};
        std::string error;
    };

    static void handleEvent(Shared& st, const std::string& ev)
    {
        std::string type = "message", data;
        size_t p = 0;
        while (p < ev.size())
        {
            size_t nl = ev.find('\n', p);
            if (nl == std::string::npos) nl = ev.size();
            const std::string l = ev.substr(p, nl - p);
            if (l.rfind("event:", 0) == 0) type = l.substr(l.size() > 6 && l[6] == ' ' ? 7 : 6);
            else if (l.rfind("data:", 0) == 0)
                data += l.substr(l.size() > 5 && l[5] == ' ' ? 6 : 5);
            p = nl + 1;
        }
        std::lock_guard<std::mutex> lk(st.m);
        if (type == "hello") st.hello = true;
        else if (type == "error")
        {
            nlohmann::json j = nlohmann::json::parse(data, nullptr, false);
            st.error = j.is_object() ? jget(j, "msg", "error") : data;
            st.ended = true;
        }
        else if (type == "message" && !data.empty())
        {
            nlohmann::json j = nlohmann::json::parse(data, nullptr, false);
            const std::string hex = jget(j, "hex", "");
            std::vector<uint8_t> b;
            for (size_t i = 0; i + 1 < hex.size(); i += 2) b.push_back((uint8_t)std::strtoul(hex.substr(i, 2).c_str(), nullptr, 16));
            if (!b.empty() && st.q.size() < 20000) st.q.push_back(std::move(b));
            else if (!b.empty())
                ++st.lost; // the display does not keep up
        }
        else if (type == "lost")
        {
            nlohmann::json j = nlohmann::json::parse(data, nullptr, false);
            st.lost += jget(j, "count", (uint64_t)0);
        }
        st.cv.notify_all();
    }

    std::shared_ptr<Shared> _st = std::make_shared<Shared>();
    std::unique_ptr<httplib::Client> _cli;
    std::thread _thr;
};

} // namespace serve
} // namespace ftc

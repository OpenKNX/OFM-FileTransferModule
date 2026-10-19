/**
 * @file        LeaseClient.h
 * @brief       The command line reserves its interfaces at a running `oknx serve` and then works
 *              locally as before.
 * @details     The reservation lives as long as one HTTP request stays open: serve streams a line now and then,
 *              and when this process ends (or dies) the operating system closes the connection and the
 *              reservation ends at once. The connection is not inherited by child processes, and the children
 *              of a reserving command line see OKNX_SERVE_LEASED and do not reserve again.
 *              No serve on the port, or another program on it: nothing to do. The port takes the connection but
 *              does not answer: refused, so the command line never works past a serve it cannot see.
 *              OKNX_BENCH_RUN (a suite run) and --no-serve: local.
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
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
    #include <fcntl.h>
#endif

namespace ftc
{
namespace serve
{

struct LeaseItem
{
    std::string ip;
    uint16_t port = 3671;
    std::string kind; ///< mgmt | gm | bm
};

class LeaseClient
{
  public:
    enum Outcome
    {
        NoServe,   ///< nothing listens: work locally
        Skipped,   ///< suite run, --no-serve, or already reserved by the parent process
        Granted,   ///< reserved; held until this process ends
        Refused,   ///< serve said no (BUSY, BENCH_LOCKED, …): do not touch the interface
        Unknown    ///< the port takes the connection but does not answer: refused, never work past an unseen serve
    };

    ~LeaseClient() { release(); }

    /** @brief Reserve @p items; @p why gets serve's reason when refused. */
    Outcome reserve(int servePort, const std::vector<LeaseItem>& items, const std::string& purpose, std::string& why)
    {
        if (std::getenv("OKNX_BENCH_RUN") || std::getenv("OKNX_SERVE_LEASED")) return Skipped;
        // health with a short limit: serve answers it from its own thread, whatever runs
        {
            httplib::Client c("127.0.0.1", servePort);
            c.set_connection_timeout(0, 300000);
            c.set_read_timeout(0, 300000);
            auto r = c.Get("/api/health");
            if (!r)
            {
                // refused: no serve. A timed-out connect means the same only on Windows, where a refused loopback
                // connect outlasts the 300 ms; on POSIX it is a serve whose listen backlog is full
                if (r.error() == httplib::Error::Connection) return NoServe;
#ifdef _WIN32
                if (r.error() == httplib::Error::ConnectionTimeout) return NoServe;
#endif
                // connected, no answer: a serve whose connections are all taken; never work past it unseen
                why = "127.0.0.1:" + std::to_string(servePort) +
                      " takes the connection but does not answer (an oknx serve with all its connections taken?); "
                      "--no-serve works without asking it";
                return Unknown;
            }
            nlohmann::json h = nlohmann::json::parse(r->body, nullptr, false);
            // serve sets the header on every answer, also a 403; a health answer naming serve counts without it
            const bool isServe = r->get_header_value("Server") == "oknx-serve" ||
                                 (r->status == 200 && jget(h, "name", "") == "oknx serve");
            if (isServe && (r->status != 200 || jget(h, "name", "") != "oknx serve"))
            {
                why = "oknx serve on 127.0.0.1:" + std::to_string(servePort) + " answered " + std::to_string(r->status) + " to its health check";
                return Unknown;
            }
            if (!isServe) return NoServe; // another program owns the port: no serve can run there

        }
        std::string err;
        const std::string key = loadKey(false, err);
        if (key.empty())
        {
            // another user's command line cannot read the key: serve sees its connections only at the interface
            why = "oknx serve runs, but its key is not readable here (" + err + "); working without a reservation";
            return Skipped;
        }
        nlohmann::json body = {{"purpose", purpose}, {"pid", pidNow()}, {"items", nlohmann::json::array()}};
        for (const auto& it : items) body["items"].push_back({{"ip", it.ip}, {"port", it.port}, {"kind", it.kind}});

        _cli.reset(new httplib::Client("127.0.0.1", servePort));
        _cli->set_connection_timeout(1, 0);
        _cli->set_read_timeout(3600 * 24, 0);
        _cli->set_socket_options([](socket_t s) {
#ifdef _WIN32
            SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0);
#else
            ::fcntl(s, F_SETFD, FD_CLOEXEC); // a child of the command line must not keep the reservation alive
#endif
        });
        auto st = _st;
        _thr = std::thread([cli = _cli.get(), st, key, body] {
            httplib::Request req;
            req.method = "POST";
            req.path = "/api/lease";
            req.headers = {{"X-Oknx-Key", key}, {"X-Oknx-Caller", "oknx-cli"}, {"Content-Type", "application/json"}};
            req.body = body.dump();
            std::string buf;
            req.response_handler = [st](const httplib::Response& r) {
                std::lock_guard<std::mutex> lk(st->m);
                st->status = r.status;
                return true;
            };
            req.content_receiver = [st, &buf](const char* d, size_t n, uint64_t, uint64_t) {
                buf.append(d, n);
                const size_t nl = buf.find('\n');
                if (nl != std::string::npos && !st->answered)
                {
                    std::lock_guard<std::mutex> lk(st->m);
                    st->first = buf.substr(0, nl);
                    st->answered = true;
                    st->cv.notify_all();
                }
                return !st->stop;
            };
            httplib::Response res;
            httplib::Error e = httplib::Error::Success;
            cli->send(req, res, e);
            std::lock_guard<std::mutex> lk(st->m);
            if (!st->answered)
            {
                st->first = res.body.empty() ? buf : res.body;
                st->answered = true;
            }
            st->ended = true;
            st->cv.notify_all();
        });
        std::unique_lock<std::mutex> lk(_st->m);
        // serve may take up to 11 s for a busmonitor lease (two lingering monitors, then the worker's tunnel)
        _st->cv.wait_for(lk, std::chrono::seconds(15), [this] { return _st->answered; });
        nlohmann::json j = nlohmann::json::parse(_st->first, nullptr, false);
        if (_st->status == 200 && j.is_object() && j.value("ok", false))
        {
            _id = j.value("lease", std::string());
#ifdef _WIN32
            _putenv_s("OKNX_SERVE_LEASED", "1");
#else
            ::setenv("OKNX_SERVE_LEASED", "1", 1);
#endif
            return Granted;
        }
        if (j.is_object() && j.contains("error"))
        {
            const auto& er = j["error"];
            why = er.value("code", std::string()) + ": " + er.value("reason", std::string());
            if (er.contains("detail") && er["detail"].is_object()) _holder = er["detail"].value("holder", std::string());
            if (er.contains("detail") && !er["detail"].empty()) why += " (" + er["detail"].dump() + ")";
        }
        else
            why = "serve did not grant the reservation";
        lk.unlock();
        release();
        return Refused;
    }

    const std::string& id() const { return _id; }
    /** @brief Who serve named as the holder of a refused reservation (serve-busmon, serve-op, …). */
    const std::string& holder() const { return _holder; }

    void release()
    {
        if (_st) _st->stop = true;
        if (_cli) _cli->stop();
        if (_thr.joinable()) _thr.join();
        _cli.reset();
    }

  private:
    static long pidNow()
    {
#ifdef _WIN32
        return (long)GetCurrentProcessId();
#else
        return (long)::getpid();
#endif
    }
    struct Shared
    {
        std::mutex m;
        std::condition_variable cv;
        bool answered = false, ended = false;
        std::atomic<bool> stop{false};
        int status = 0;
        std::string first;
    };
    std::shared_ptr<Shared> _st = std::make_shared<Shared>();
    std::unique_ptr<httplib::Client> _cli;
    std::thread _thr;
    std::string _id;
    std::string _holder;
};

} // namespace serve
} // namespace ftc

/**
 * @file        BenchCmd.cpp
 * @brief       `oknx bench lock <run> <ip…>` and `oknx bench unlock`: hold the bench by hand, the way a suite run
 *              does, with the same lock file and the same heartbeat.
 * @details     lock writes the file, waits for a running serve to report the release (10 s), renews the heartbeat
 *              every 10 s while it runs, and removes the file on Ctrl+C if it is still its own. The file comes from
 *              --file, else from the first site of the rules file that names a lock_file.
 * @date        2026-10-19
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#include "core/Discovery.h" // winsock2 before windows.h

#include "serve/Bench.h"
#include "serve/BenchCmd.h"
#include "serve/Key.h"
#include "serve/Paths.h"
#include "serve/Proto.h"
#include "serve/Rules.h"
#include "serve/HttpLib.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
    #include <unistd.h>
#endif

namespace ftc
{
namespace serve
{

namespace
{
std::atomic<bool> g_benchStop{false};
void onBenchSignal(int) { g_benchStop = true; }

long ownPid()
{
#ifdef _WIN32
    return (long)GetCurrentProcessId();
#else
    return (long)::getpid();
#endif
}

std::string lockPathFrom(const std::string& explicitPath)
{
    if (!explicitPath.empty()) return explicitPath;
    Rules r;
    std::string err;
    if (!r.load(rulesPathDefault(), err)) return std::string();
    for (const auto& s : r.sites())
        if (!s.lockFile.empty()) return s.lockFile;
    return std::string();
}

/** @brief released == @p run on a running serve within @p ms; true also when no serve answers at all. */
bool waitReleased(int port, const std::string& run, int ms, std::string& why)
{
    std::string err;
    const std::string key = loadKey(false, err);
    httplib::Client c("127.0.0.1", port);
    c.set_connection_timeout(0, 300000);
    c.set_read_timeout(2, 0);
    const httplib::Headers h = {{"X-Oknx-Key", key}};
    auto first = c.Get("/api/health", h);
    if (!first) return true; // no serve: nothing to wait for
    if (key.empty())
    {
        why = "serve runs, but the key is not readable: " + err;
        return false;
    }
    for (int waited = 0; waited <= ms; waited += 250)
    {
        auto r = c.Get("/api/health", h);
        if (r)
        {
            nlohmann::json j = nlohmann::json::parse(r->body, nullptr, false);
            if (j.is_object() && j.value("released", std::string()) == run) return true;
            if (j.is_object() && j.contains("leases") && !j["leases"].empty())
                why = "reservations of the command line are still open: " + j["leases"].dump();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (why.empty()) why = "serve did not report the release within 10 s";
    return false;
}
} // namespace

int benchMain(const std::vector<std::string>& args, int servePort)
{
    std::string file;
    std::vector<std::string> rest;
    for (size_t i = 0; i < args.size(); ++i)
    {
        if (args[i] == "--file" && i + 1 < args.size()) file = args[++i];
        else
            rest.push_back(args[i]);
    }
    const std::string path = lockPathFrom(file);
    if (rest.empty() || (rest[0] != "lock" && rest[0] != "unlock" && rest[0] != "status"))
    {
        std::fprintf(stderr, "usage: oknx bench lock <run-id> <ip…> [--file <lock>] | bench unlock [--file <lock>] | bench status\n");
        return 2;
    }
    if (path.empty())
    {
        std::fprintf(stderr, "bench: no lock file: give --file, or name lock_file in a site of %s\n", rulesPathDefault().c_str());
        return 2;
    }
    if (rest[0] == "status")
    {
        std::printf("%s\n", readBenchLock(path).toJson().dump(1).c_str());
        return 0;
    }
    if (rest[0] == "unlock")
    {
        const BenchLock b = readBenchLock(path);
        if (!b.present)
        {
            std::printf("bench: no lock file (%s)\n", path.c_str());
            return 0;
        }
        if (std::remove(path.c_str()) != 0)
        {
            std::fprintf(stderr, "bench: cannot remove %s\n", path.c_str());
            return 1;
        }
        std::printf("bench: released (%s, run %s)\n", path.c_str(), b.run.c_str());
        return 0;
    }
    // lock
    if (rest.size() < 3)
    {
        std::fprintf(stderr, "usage: oknx bench lock <run-id> <ip…>\n");
        return 2;
    }
    const std::string run = rest[1];
    std::vector<std::string> ifaces(rest.begin() + 2, rest.end());
    for (const auto& i : ifaces)
        if (!isIpv4(i))
        {
            std::fprintf(stderr, "bench: not an IPv4 address: %s\n", i.c_str());
            return 2;
        }
    const BenchLock cur = readBenchLock(path);
    if (cur.valid && cur.run != run)
    {
        std::fprintf(stderr, "bench: held by run %s (pid %ld, heartbeat %ld s ago)\n", cur.run.c_str(), cur.pid, cur.heartbeatAgeS);
        return 1;
    }
    const std::string start = isoUtc(std::time(nullptr));
    auto write = [&]() -> bool {
        std::string err;
        const std::string tmp = path + ".tmp";
        FILE* f = std::fopen(tmp.c_str(), "wb");
        if (!f) return false;
        const std::string body = benchLockBody(run, ownPid(), start, ifaces);
        const bool ok = std::fwrite(body.data(), 1, body.size(), f) == body.size();
        std::fclose(f);
        if (!ok) return false;
        std::remove(path.c_str());
        return std::rename(tmp.c_str(), path.c_str()) == 0;
    };
    if (!write())
    {
        std::fprintf(stderr, "bench: cannot write %s\n", path.c_str());
        return 1;
    }
    std::string why;
    if (!waitReleased(servePort, run, 10000, why))
    {
        std::remove(path.c_str());
        std::fprintf(stderr, "bench: %s; lock removed again\n", why.c_str());
        return 1;
    }
    std::printf("bench: held as run %s (%s); heartbeat every 10 s; Ctrl+C releases it\n", run.c_str(), path.c_str());
    std::fflush(stdout);
    std::signal(SIGINT, onBenchSignal);
    std::signal(SIGTERM, onBenchSignal);
    uint64_t last = monoMs();
    while (!g_benchStop)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (monoMs() - last >= 10000)
        {
            const BenchLock now = readBenchLock(path);
            if (!now.present || now.run != run || now.pid != ownPid())
            {
                std::fprintf(stderr, "bench: the lock file was replaced or removed by someone else; stopping\n");
                return 1;
            }
            if (!write()) std::fprintf(stderr, "bench: heartbeat not written\n");
            last = monoMs();
        }
    }
    const BenchLock now = readBenchLock(path);
    if (now.present && now.run == run && now.pid == ownPid()) std::remove(path.c_str());
    std::printf("bench: released\n");
    return 0;
}

} // namespace serve
} // namespace ftc

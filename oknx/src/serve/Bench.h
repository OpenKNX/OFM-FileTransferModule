/**
 * @file        Bench.h
 * @brief       The bench lock file `Reports/.bench.lock`: a suite run (or `oknx bench lock`) holds the bench.
 * @details     JSON, version 1: run, host, pid, start, interfaces[], heartbeat (UTC, ISO 8601). The holder renews
 *              the heartbeat every 10 s; the lock is valid while it is younger than 60 s and only on the machine
 *              it names. serve never deletes it; an older heartbeat makes it orphaned, and the bench counts as free.
 * @date        2026-10-19
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include "serve/JsonGet.h"
#include "serve/Key.h"
#include "third_party/json.hpp"

#include <cctype>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
    #include <winsock2.h>
#else
    #include <unistd.h>
#endif

namespace ftc
{
namespace serve
{

inline std::string hostName()
{
    char b[256] = {0};
#ifdef _WIN32
    DWORD n = sizeof(b);
    // the DNS host name, as [Dns]::GetHostName() of the suite writes it (GetComputerNameA is the 15-char NetBIOS one)
    if (!GetComputerNameExA(ComputerNameDnsHostname, b, &n)) return std::string();
#else
    if (::gethostname(b, sizeof(b) - 1) != 0) return std::string();
#endif
    return b;
}

/** @brief UTC "YYYY-MM-DDTHH:MM:SSZ" of @p t. */
inline std::string isoUtc(std::time_t t)
{
    std::tm tmv{};
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    char b[32];
    std::strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    return b;
}

/** @brief Parse the UTC form above; 0 when it is not one. */
inline std::time_t parseIsoUtc(const std::string& s)
{
    std::tm tmv{};
    int Y, M, D, h, m, sec;
    char z = 0;
    if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d%c", &Y, &M, &D, &h, &m, &sec, &z) != 7 || z != 'Z') return 0;
    tmv.tm_year = Y - 1900;
    tmv.tm_mon = M - 1;
    tmv.tm_mday = D;
    tmv.tm_hour = h;
    tmv.tm_min = m;
    tmv.tm_sec = sec;
#ifdef _WIN32
    return _mkgmtime(&tmv);
#else
    return timegm(&tmv);
#endif
}

/** @brief Host names as different tools spell them: case-insensitive, without a trailing ".local". */
inline bool sameHost(std::string a, std::string b)
{
    auto norm = [](std::string& s) {
        for (auto& c : s) c = (char)std::tolower((unsigned char)c);
        const std::string tail = ".local";
        if (s.size() > tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0) s.resize(s.size() - tail.size());
    };
    norm(a);
    norm(b);
    return a == b;
}

struct BenchLock
{
    bool present = false;    ///< a file is there and parses
    bool valid = false;      ///< present, this host, heartbeat less than 60 s old (or up to 15 s ahead)
    bool orphaned = false;   ///< present, but its heartbeat is older
    bool foreignHost = false;
    std::string run, host, start, heartbeat;
    long pid = 0;
    long heartbeatAgeS = -1;
    std::vector<std::string> interfaces;
    std::string error; ///< why a present file did not parse

    bool names(const std::string& ip) const
    {
        for (const auto& i : interfaces)
            if (i == ip) return true;
        return false;
    }
    nlohmann::json toJson() const
    {
        return nlohmann::json{{"present", present},   {"valid", valid},         {"orphaned", orphaned},
                              {"run", run},           {"host", host},           {"pid", pid},
                              {"start", start},       {"heartbeat", heartbeat}, {"heartbeat_age_s", heartbeatAgeS},
                              {"interfaces", interfaces}, {"foreign_host", foreignHost}, {"error", error}};
    }
};

inline BenchLock readBenchLock(const std::string& path)
{
    BenchLock b;
    if (path.empty()) return b;
    std::ifstream f(path, std::ios::binary);
    if (!f) return b;
    std::stringstream ss;
    ss << f.rdbuf();
    nlohmann::json j = nlohmann::json::parse(ss.str(), nullptr, false);
    b.present = true;
    if (!j.is_object() || jget(j, "version", 0) != 1)
    {
        b.error = "not a version 1 lock file";
        return b;
    }
    // a member of another type counts as missing: "pid": "123" does not undo a lock with a fresh heartbeat
    b.run = jget(j, "run", "");
    b.host = jget(j, "host", "");
    b.pid = jget(j, "pid", 0L);
    b.start = jget(j, "start", "");
    b.heartbeat = jget(j, "heartbeat", "");
    if (j.contains("interfaces") && j["interfaces"].is_array())
        for (const auto& i : j["interfaces"])
            if (i.is_string()) b.interfaces.push_back(i.get<std::string>());
    const std::time_t hb = parseIsoUtc(b.heartbeat);
    if (hb == 0)
    {
        b.error = "heartbeat is not a UTC time";
        return b;
    }
    b.heartbeatAgeS = (long)(std::time(nullptr) - hb);
    b.foreignHost = !b.host.empty() && !sameHost(b.host, hostName());
    if (b.foreignHost) return b; // valid only on the machine it names
    // up to 15 s ahead counts as fresh too (the clock stepped back; the holder renews within 10 s); more ahead
    // is orphaned, so a dead run's lock never outlives 60 s by more than those 15 s
    if (b.heartbeatAgeS > -15 && b.heartbeatAgeS < 60) b.valid = true;
    else
        b.orphaned = true;
    return b;
}

/** @brief The lock file body for `oknx bench lock` (and the reference for Run-Tests.ps1). */
inline std::string benchLockBody(const std::string& run, long pid, const std::string& start,
                                 const std::vector<std::string>& ifaces)
{
    nlohmann::json j = {{"version", 1},
                        {"run", run},
                        {"host", hostName()},
                        {"pid", pid},
                        {"start", start},
                        {"interfaces", ifaces},
                        {"heartbeat", isoUtc(std::time(nullptr))}};
    return j.dump(1) + "\n";
}

} // namespace serve
} // namespace ftc

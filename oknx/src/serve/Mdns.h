/**
 * @file        Mdns.h
 * @brief       mDNS / DNS-SD search for OpenKNX devices (`_openknx._tcp.local`), as the busmonitor bridge does it.
 * @details     One PTR query with the QU bit to 224.0.0.251:5353; every answer's PTR, SRV, A and TXT records are
 *              collected for the timeout, then joined per instance. Every read is bounds-checked against the
 *              datagram; compression pointers are followed at most 128 times.
 * @date        2026-10-19
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include "core/Discovery.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace ftc
{
namespace serve
{

struct MdnsDevice
{
    std::string instance; ///< full instance name
    std::string name;     ///< instance without "._openknx._tcp.local"
    std::string ip;
    std::map<std::string, std::string> txt; ///< address, version, ota, configured, firmware, serial, otamode, …
};

namespace mdns_detail
{
/** @brief Read a possibly compressed name at @p off; @p next gets the offset after it. */
inline std::string readName(const uint8_t* b, int n, int off, int& next)
{
    std::string out;
    bool jumped = false;
    next = off;
    for (int guard = 0; guard < 128; ++guard)
    {
        if (off < 0 || off >= n) break;
        const uint8_t len = b[off];
        if (len == 0)
        {
            if (!jumped) next = off + 1;
            break;
        }
        if ((len & 0xC0) == 0xC0)
        {
            if (off + 1 >= n) break;
            const int ptr = ((len & 0x3F) << 8) | b[off + 1];
            if (!jumped) next = off + 2;
            jumped = true;
            off = ptr;
            continue;
        }
        ++off;
        if (off + len > n) break;
        if (!out.empty()) out += '.';
        out.append((const char*)b + off, len);
        off += len;
    }
    return out;
}
} // namespace mdns_detail

/** @brief Ask the LAN for OpenKNX devices for @p timeoutMs; sorted by name. */
inline std::vector<MdnsDevice> mdnsOpenKnx(int timeoutMs = 2500)
{
    using namespace ftc::detail;
    std::vector<MdnsDevice> out;
    socket_t s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (!sockValid(s)) return out;
    int one = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = 0;
    if (::bind(s, (sockaddr*)&local, sizeof(local)) != 0)
    {
        sockClose(s);
        return out;
    }
    // Header (id 0, flags 0, 1 question) + _openknx._tcp.local + type PTR (12) + class IN with the QU bit.
    std::vector<uint8_t> q = {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0};
    for (const char* label : {"_openknx", "_tcp", "local"})
    {
        q.push_back((uint8_t)std::strlen(label));
        q.insert(q.end(), label, label + std::strlen(label));
    }
    q.insert(q.end(), {0, 0, 12, 0x80, 0x01});
    sockaddr_in grp{};
    grp.sin_family = AF_INET;
    grp.sin_port = htons(5353);
    grp.sin_addr.s_addr = inet_addr("224.0.0.251");
    ::sendto(s, (const char*)q.data(), (int)q.size(), 0, (sockaddr*)&grp, sizeof(grp));

    std::set<std::string> ptr;
    std::map<std::string, std::string> srvTarget;
    std::map<std::string, std::string> addr;
    std::map<std::string, std::map<std::string, std::string>> txt;
    const uint64_t deadline = nowMs() + (uint64_t)timeoutMs;
    while (nowMs() < deadline)
    {
        const uint64_t rem = deadline - nowMs();
        timeval tv;
        tv.tv_sec = (long)(rem / 1000);
        tv.tv_usec = (long)((rem % 1000) * 1000);
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(s, &rf);
        if (::select((int)s + 1, &rf, nullptr, nullptr, &tv) <= 0) continue;
        uint8_t b[1500];
        const int n = (int)::recv(s, (char*)b, sizeof(b), 0);
        if (n < 12) continue;
        const int qd = (b[4] << 8) | b[5];
        const int rr = ((b[6] << 8) | b[7]) + ((b[8] << 8) | b[9]) + ((b[10] << 8) | b[11]);
        int pos = 12, next = 0;
        for (int i = 0; i < qd && pos < n; ++i)
        {
            mdns_detail::readName(b, n, pos, next);
            pos = next + 4;
        }
        for (int i = 0; i < rr; ++i)
        {
            if (pos + 10 > n) break;
            const std::string name = mdns_detail::readName(b, n, pos, next);
            pos = next;
            if (pos + 10 > n) break;
            const int type = (b[pos] << 8) | b[pos + 1];
            const int rdlen = (b[pos + 8] << 8) | b[pos + 9];
            const int rd = pos + 10;
            if (rd + rdlen > n) break;
            if (type == 12)
                ptr.insert(mdns_detail::readName(b, n, rd, next));
            else if (type == 33 && rdlen >= 7)
                srvTarget[name] = mdns_detail::readName(b, n, rd + 6, next);
            else if (type == 1 && rdlen >= 4)
                addr[name] = std::to_string(b[rd]) + "." + std::to_string(b[rd + 1]) + "." + std::to_string(b[rd + 2]) + "." +
                             std::to_string(b[rd + 3]);
            else if (type == 16)
            {
                std::map<std::string, std::string> kv;
                for (int tp = rd; tp < rd + rdlen;)
                {
                    const int l = b[tp++];
                    if (l == 0 || tp + l > rd + rdlen) break;
                    const std::string item((const char*)b + tp, l);
                    tp += l;
                    const size_t eq = item.find('=');
                    if (eq != std::string::npos) kv[item.substr(0, eq)] = item.substr(eq + 1);
                }
                txt[name] = kv;
            }
            pos = rd + rdlen;
        }
    }
    sockClose(s);
    std::set<std::string> inst = ptr;
    for (const auto& kv : srvTarget) inst.insert(kv.first);
    for (const auto& i : inst)
    {
        const auto st = srvTarget.find(i);
        std::string ip;
        if (st != srvTarget.end()) ip = addr.count(st->second) ? addr[st->second] : st->second;
        if (ip.empty()) continue;
        MdnsDevice d;
        d.instance = i;
        d.name = i;
        const std::string tail = "._openknx._tcp.local";
        if (d.name.size() > tail.size() && d.name.compare(d.name.size() - tail.size(), tail.size(), tail) == 0)
            d.name.resize(d.name.size() - tail.size());
        d.ip = ip;
        if (txt.count(i)) d.txt = txt[i];
        out.push_back(d);
    }
    std::sort(out.begin(), out.end(), [](const MdnsDevice& a, const MdnsDevice& b) { return a.name < b.name; });
    return out;
}

} // namespace serve
} // namespace ftc

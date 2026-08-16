/**
 * @file        Discovery.h
 * @brief       KNXnet/IP interface discovery (SEARCH_REQUEST -> SEARCH_RESPONSE), CLI/terminal-agnostic.
 * @details     Part of the reusable CORE: returns a structured list, no printing. The CLI's `--discover`
 *              and `progscan locate` call it. Header-only; the CLI TU already pulls the socket headers.
 * @date        2026-08-03
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <arpa/inet.h>
    #include <ifaddrs.h>
    #include <net/if.h>
    #include <netinet/in.h>
    #include <sys/select.h>
    #include <sys/socket.h>
    #include <unistd.h>
#endif

namespace ftc
{

/**
 * @brief One interface found on the LAN. Extend (secure flag, service families) as the core grows.
 */
struct DiscoveredIface
{
    std::string ip;
    std::string name;
    uint8_t medium = 0; // KNX medium code from the Device Information DIB
    uint16_t ia = 0;    // the interface's own individual address — its LINE decides what it can reach
};

namespace detail
{
#ifdef _WIN32
using socket_t = SOCKET;
inline bool sockValid(socket_t s) { return s != INVALID_SOCKET; }
inline void sockClose(socket_t s) { closesocket(s); }
#else
using socket_t = int;
inline bool sockValid(socket_t s) { return s >= 0; }
inline void sockClose(socket_t s) { ::close(s); }
#endif

inline uint64_t nowMs()
{
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

/**
 * @brief Local source IP the OS would use to reach the KNX multicast group (network order; 0 = route-back).
 */
inline uint32_t localIpForMulticast()
{
    socket_t s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (!sockValid(s)) return 0;
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(3671);
    dst.sin_addr.s_addr = inet_addr("224.0.23.12");
    uint32_t ip = 0;
    if (::connect(s, (sockaddr*)&dst, sizeof(dst)) == 0)
    {
        sockaddr_in loc{};
        socklen_t ll = sizeof(loc);
        if (::getsockname(s, (sockaddr*)&loc, &ll) == 0) ip = loc.sin_addr.s_addr;
    }
    sockClose(s);
    return ip;
}

/**
 * @brief All usable local IPv4 addresses (network order), loopback excluded.
 * @details Discovery sends the SEARCH_REQUEST out EACH of them so a multi-NIC host (Wi-Fi + Ethernet,
 *          a VPN, docker/hyper-v adapters — the usual reason `--discover` finds nothing on Windows)
 *          still reaches the NIC that carries the KNX LAN.
 */
inline std::vector<uint32_t> localIPv4s()
{
    std::vector<uint32_t> ips;
#ifdef _WIN32
    socket_t s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockValid(s))
    {
        INTERFACE_INFO info[64];
        DWORD ret = 0;
        if (::WSAIoctl(s, SIO_GET_INTERFACE_LIST, nullptr, 0, info, sizeof(info), &ret, nullptr, nullptr) == 0)
        {
            const int n = (int)(ret / sizeof(INTERFACE_INFO));
            for (int i = 0; i < n; ++i)
            {
                const u_long fl = info[i].iiFlags;
                if (!(fl & IFF_UP) || (fl & IFF_LOOPBACK)) continue;
                const uint32_t a = ((sockaddr_in*)&info[i].iiAddress)->sin_addr.s_addr;
                if (a) ips.push_back(a);
            }
        }
        sockClose(s);
    }
#else
    struct ifaddrs* head = nullptr;
    if (::getifaddrs(&head) == 0)
    {
        for (struct ifaddrs* p = head; p; p = p->ifa_next)
        {
            if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
            if (!(p->ifa_flags & IFF_UP) || (p->ifa_flags & IFF_LOOPBACK)) continue;
            const uint32_t a = ((sockaddr_in*)p->ifa_addr)->sin_addr.s_addr;
            if (a) ips.push_back(a);
        }
        ::freeifaddrs(head);
    }
#endif
    return ips;
}
} // namespace detail

/**
 * @brief Multicast SEARCH_REQUEST; collect responders for `timeoutMs`. Returns the list (may be empty).
 */
inline std::vector<DiscoveredIface> discoverInterfaces(uint16_t port = 3671, int timeoutMs = 3000)
{
    using namespace detail;
    std::vector<DiscoveredIface> found;
    socket_t s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (!sockValid(s)) return found;

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = 0;
    if (::bind(s, (sockaddr*)&local, sizeof(local)) != 0)
    {
        sockClose(s);
        return found;
    }
    socklen_t ll = sizeof(local);
    ::getsockname(s, (sockaddr*)&local, &ll);
    const uint16_t boundPort = ntohs(local.sin_port);

    unsigned char ttl = 4;
    ::setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof(ttl));

    sockaddr_in grp{};
    grp.sin_family = AF_INET;
    grp.sin_port = htons(port);
    grp.sin_addr.s_addr = inet_addr("224.0.23.12");

    // Send the SEARCH_REQUEST out EVERY local NIC (with that NIC as the multicast egress + its IP in the
    // discovery-endpoint HPAI so responders unicast the SEARCH_RESPONSE straight back). This is the fix for
    // multi-NIC hosts (esp. Windows) where a single default-route send misses the KNX LAN entirely.
    std::vector<uint32_t> nics = localIPv4s();
    const uint32_t routed = localIpForMulticast(); // the OS's default choice — always include it
    if (routed)
    {
        bool have = false;
        for (uint32_t a : nics)
            if (a == routed) have = true;
        if (!have) nics.push_back(routed);
    }
    if (nics.empty()) nics.push_back(routed); // last resort: route-back (0 = INADDR_ANY)

    bool anySent = false;
    for (uint32_t nic : nics)
    {
        struct in_addr mif;
        mif.s_addr = nic;
        ::setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&mif, sizeof(mif)); // egress NIC
        unsigned char pkt[14] = {0x06, 0x10, 0x02, 0x01, 0x00, 0x0E, 0x08, 0x01, 0, 0, 0, 0,
                                 (unsigned char)(boundPort >> 8), (unsigned char)(boundPort & 0xFF)};
        std::memcpy(&pkt[8], &nic, 4); // HPAI = this NIC's IP -> unicast response comes back here
        if (::sendto(s, (const char*)pkt, sizeof(pkt), 0, (sockaddr*)&grp, sizeof(grp)) >= 0) anySent = true;
    }
    if (!anySent)
    {
        sockClose(s);
        return found;
    }

    const uint64_t deadline = nowMs() + (uint64_t)timeoutMs;
    for (;;)
    {
        const uint64_t now = nowMs();
        if (now >= deadline) break;
        const uint64_t rem = deadline - now;
        timeval tv;
        tv.tv_sec = (long)(rem / 1000);
        tv.tv_usec = (long)((rem % 1000) * 1000);
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(s, &rfds);
        if (::select((int)s + 1, &rfds, nullptr, nullptr, &tv) <= 0) continue;

        unsigned char buf[512];
        sockaddr_in from{};
        socklen_t fl = sizeof(from);
        const int n = (int)::recvfrom(s, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
        // SEARCH_RESPONSE (0x0202): 6-hdr + 8-HPAI(control) + Device-Info DIB (medium @+2, name @+24).
        if (n < 6 + 8 + 54) continue;
        if (!(buf[2] == 0x02 && buf[3] == 0x02)) continue;
        DiscoveredIface f;
        char ip[16];
        std::snprintf(ip, sizeof(ip), "%u.%u.%u.%u", buf[8], buf[9], buf[10], buf[11]);
        f.ip = ip;
        f.medium = buf[16];
        // Device Information DIB +4: the individual address. Which line an interface sits on decides
        // whether it can reach a given device at all — withholding it makes the choice a guess.
        f.ia = (uint16_t)((buf[18] << 8) | buf[19]);
        char nm[31];
        std::memcpy(nm, &buf[38], 30);
        nm[30] = 0;
        f.name = nm[0] ? nm : "(unnamed)";
        // de-dupe (a router can answer on several interfaces)
        bool dup = false;
        for (const auto& e : found)
            if (e.ip == f.ip)
            {
                dup = true;
                break;
            }
        if (!dup) found.push_back(f);
    }
    sockClose(s);
    return found;
}

} // namespace ftc

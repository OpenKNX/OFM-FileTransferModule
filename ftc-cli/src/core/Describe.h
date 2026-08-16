/**
 * @file        Describe.h
 * @brief       The ONE unicast KNXnet/IP DESCRIPTION probe (no connection, no bus traffic), CLI-agnostic CORE.
 * @details     `queryInterface` fills the full `IfaceDesc` (identity + service families + IP config +
 *              Extended DIB) — the single DESCRIPTION parser shared by the one-shot CLI and `ftc mc`.
 *              `describeInterface` is a thin wrapper returning the small `IfaceInfo` the mc info panel needs.
 *              Also the single KNX-medium-name table. Reuses the socket helpers from Discovery.h. Header-only.
 * @date        2026-08-03
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include <cstdint>
#include <cstring>
#include <string>

#include "Discovery.h" // ftc::detail:: socket helpers + includes

namespace ftc
{

/**
 * @brief KNX medium code -> full name (cEMI/DIB medium field). The one place this table lives.
 */
inline const char* knxMediumName(uint8_t m)
{
    switch (m)
    {
        case 0x01: return "TP0";
        case 0x02: return "TP1 (twisted pair)";
        case 0x04: return "PL110";
        case 0x08: return "PL132";
        case 0x10: return "RF";
        case 0x20: return "IP";
        default: return "unknown";
    }
}
/**
 * @brief KNX medium code -> short name.
 */
inline const char* knxMediumShort(uint8_t m)
{
    switch (m)
    {
        case 0x01: return "TP0";
        case 0x02: return "TP1";
        case 0x04: return "PL110";
        case 0x08: return "PL132";
        case 0x10: return "RF";
        case 0x20: return "IP";
        default: return "";
    }
}

/**
 * @brief Full interface identity from DESCRIPTION_RESPONSE.
 * @details DESCRIPTION fields are filled by queryInterface(); the APDU-detection fields (apdu*) are
 *          filled SEPARATELY by the host (device-mgmt / memory probe), not by the core parser.
 */
struct IfaceDesc
{
    bool ok = false;
    uint8_t medium = 0;
    uint8_t status = 0; // bit0 = programming mode
    uint16_t ia = 0;
    uint8_t serial[6] = {0};
    uint8_t mac[6] = {0};
    char name[31] = {0};
    uint8_t famVer[256] = {0}; // service family id -> version (0 = absent)
    bool hasExt = false;
    uint16_t maxLocalApdu = 0;
    uint16_t mask = 0;
    bool hasIp = false;
    uint32_t ip = 0, subnet = 0, gw = 0;
    uint8_t ipMethod = 0;
    bool haveIpMethod = false; // the DIB carried a method byte -- "not reported" is not "unset"
    // Filled by the HOST after DESCRIPTION (NOT by the core parser): APDU auto-detection results.
    uint16_t apduReported = 0; // 0 = not obtained
    uint8_t apduReportedPid = 0;
    uint16_t apduMeasured = 0; // 0 = not probed / no answer
    char apduReason[64] = {0}; // why apduReported is 0 (device-mgmt diagnostic)
};

/**
 * @brief Walk the DIB block (Core 7.5.4): each DIB = [structLen][typeCode][payload...].
 */
inline void parseDibs(const unsigned char* d, int len, IfaceDesc& o)
{
    int off = 0;
    while (off + 2 <= len)
    {
        int dl = d[off], dt = d[off + 1];
        if (dl < 2 || off + dl > len) break;
        const unsigned char* b = d + off;
        if (dt == 0x01 && dl >= 54) // Device Information DIB
        {
            o.medium = b[2];
            o.status = b[3];
            o.ia = (uint16_t)((b[4] << 8) | b[5]);
            std::memcpy(o.serial, b + 8, 6);
            std::memcpy(o.mac, b + 18, 6);
            std::memcpy(o.name, b + 24, 30);
            o.name[30] = 0;
        }
        else if (dt == 0x02) // Supported Service Families DIB
        {
            for (int i = 2; i + 1 < dl; i += 2)
                o.famVer[b[i]] = b[i + 1];
        }
        else if (dt == 0x04 && dl >= 20) // IP Current Config DIB
        {
            o.hasIp = true;
            o.ip = (uint32_t)((b[2] << 24) | (b[3] << 16) | (b[4] << 8) | b[5]);
            o.subnet = (uint32_t)((b[6] << 24) | (b[7] << 16) | (b[8] << 8) | b[9]);
            o.gw = (uint32_t)((b[10] << 24) | (b[11] << 16) | (b[12] << 8) | b[13]);
            o.ipMethod = b[18];
            o.haveIpMethod = true;
        }
        else if (dt == 0x08 && dl >= 8) // Extended Device Information DIB (v2): maxLocalApdu + mask
        {
            o.hasExt = true;
            o.maxLocalApdu = (uint16_t)((b[4] << 8) | b[5]);
            o.mask = (uint16_t)((b[6] << 8) | b[7]);
        }
        off += dl;
    }
}

/**
 * @brief The ONE DESCRIPTION probe: unicast DESCRIPTION_REQUEST (0x0203) -> DESCRIPTION_RESPONSE (0x0204).
 * @details Parses the DIBs into `o`. Route-back HPAI (0.0.0.0). Returns false on no answer within timeoutMs.
 */
inline bool queryInterface(const std::string& ip, uint16_t port, IfaceDesc& o, int timeoutMs = 2000)
{
    using namespace detail;
    socket_t s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (!sockValid(s)) return false;
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = 0;
    ::bind(s, (sockaddr*)&local, sizeof(local));
    socklen_t ll = sizeof(local);
    ::getsockname(s, (sockaddr*)&local, &ll);
    const uint16_t bp = ntohs(local.sin_port);
    // header(6) + control HPAI(8): IP 0.0.0.0 => the server replies to the packet source (route-back).
    unsigned char pkt[14] = {0x06, 0x10, 0x02, 0x03, 0x00, 0x0E, 0x08, 0x01, 0, 0, 0, 0,
                             (unsigned char)(bp >> 8), (unsigned char)(bp & 0xFF)};
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    dst.sin_addr.s_addr = inet_addr(ip.c_str());
    ::sendto(s, (const char*)pkt, sizeof(pkt), 0, (sockaddr*)&dst, sizeof(dst));

    unsigned char buf[1024];
    bool got = false;
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
        sockaddr_in from{};
        socklen_t fl = sizeof(from);
        const int n = (int)::recvfrom(s, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
        if (n >= 6 && buf[2] == 0x02 && buf[3] == 0x04) // DESCRIPTION_RESPONSE
        {
            parseDibs(buf + 6, n - 6, o);
            o.ok = true;
            got = true;
            break;
        }
    }
    sockClose(s);
    return got;
}

/**
 * @brief The small subset `ftc mc`'s info panel needs (name + medium + IA + mask + routing/tunnelling).
 */
struct IfaceInfo
{
    bool ok = false;
    std::string name;
    uint8_t medium = 0;
    uint16_t ia = 0;
    uint16_t mask = 0;       // from the Extended Device Info DIB (v2); 0 if absent
    bool routing = false;    // advertises the ROUTING service family (0x05)
    bool tunnelling = false; // advertises Tunnelling (0x04)
};

/**
 * @brief Convenience wrapper over queryInterface -> the small IfaceInfo (mc's info panel). ONE parser.
 */
inline bool describeInterface(const std::string& ip, uint16_t port, IfaceInfo& out, int timeoutMs = 1500)
{
    IfaceDesc d;
    if (!queryInterface(ip, port, d, timeoutMs)) return false;
    out.ok = true;
    out.name = d.name[0] ? d.name : "(unnamed)";
    out.medium = d.medium;
    out.ia = d.ia;
    out.mask = d.mask;
    out.tunnelling = d.famVer[0x04] != 0;
    out.routing = d.famVer[0x05] != 0;
    return true;
}

} // namespace ftc

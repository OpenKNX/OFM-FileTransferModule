/**
 * @file        DeviceMgmt.h
 * @brief       KNXnet/IP Device Management (Local Device Management, cEMI M_PropRead) as a reusable CORE.
 * @details     A single DEVICE_MGMT_CONNECTION is opened once and reused for MANY property reads (far
 *              cheaper than a connect-per-property). No bus traffic: everything is server-local property
 *              access, so it works even on a spec-conform interface that never routes group communication.
 *              Header-only, cross-platform via the ftc::detail:: socket helpers in Discovery.h. Nothing prints.
 *
 *              Layout of the M_PropRead exchange (03_06_03 cEMI + 03_08_03 Management):
 *                CONNECT      06 10 02 05 00 18  <ctrl HPAI> <data HPAI>  02 03      (DEVICE_MGMT_CONNECTION)
 *                READ (req)   06 10 03 10 00 11  04 <ch> <seq> 00  FC <IOTh><IOTl> <inst> <PID> <NoE|sIdxH><sIdxL>
 *                CON (server) 06 10 03 10 00 ..  04 <ch> <sSeq> 00  FB <IOTh><IOTl> <inst> <PID> <NoE|..><..> <data…>
 *                             -> we MUST ACK: 06 10 03 11 00 0A 04 <ch> <sSeq> 00
 *                DISCONNECT   06 10 02 09 00 10  <ch> 00  <ctrl HPAI>                (best-effort on close)
 * @date        2026-08-03
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "Discovery.h" // ftc::detail:: socket helpers (socket_t, sockValid, sockClose, nowMs) + socket includes

namespace ftc
{

/**
 * @brief KNX interface-object type codes used here (03_04_01).
 */
enum : uint16_t
{
    IOT_DEVICE = 0,          // Device Object
    IOT_CEMI_SERVER = 8,     // cEMI Server Object
    IOT_KNXNETIP_PARAM = 11, // KNXnet/IP Parameter Object
};

/**
 * @brief Diagnostics: KNXnet/IP CONNECT_RESPONSE status code -> name.
 */
inline const char* dmConnectStatusName(uint8_t s)
{
    switch (s)
    {
        case 0x00: return "NO_ERROR";
        case 0x22: return "E_CONNECTION_TYPE";
        case 0x23: return "E_CONNECTION_OPTION";
        case 0x24: return "E_NO_MORE_CONNECTIONS";
        case 0x25: return "E_NO_MORE_UNIQUE_CONNECTIONS";
        case 0x28: return "E_AUTHORISATION_ERROR";
        case 0x29: return "E_TUNNELLING_LAYER";
        case 0x2D: return "E_NO_TUNNELLING_ADDRESS";
        case 0x2E: return "E_CONNECTION_IN_USE";
        default: return "unknown";
    }
}
/**
 * @brief Diagnostics: cEMI property-access error code -> name.
 */
inline const char* dmPropErrorName(uint8_t e)
{
    switch (e)
    {
        case 0x00: return "Unspecified Error";
        case 0x01: return "Out of Range";
        case 0x02: return "Out of MaxRange";
        case 0x03: return "Out of MinRange";
        case 0x04: return "Memory Error";
        case 0x05: return "Read Only";
        case 0x06: return "Illegal Command";
        case 0x07: return "Void DP (no such Property)";
        case 0x08: return "Type Conflict";
        case 0x09: return "Prop. Index Range Error";
        case 0x0A: return "Value temp. not writeable";
        default: return "unknown";
    }
}

/**
 * @brief KNX mask version -> friendly device-class name (the DIB / PID_DEVICE_DESCRIPTOR value).
 */
inline const char* knxMaskName(uint16_t m)
{
    switch (m)
    {
        case 0x0010: return "TP1 BCU1 (System 1)";
        case 0x0012: return "TP1 BCU1 tunnel-only";
        case 0x0013: return "TP1 BCU1";
        case 0x0020: return "TP1 BCU2 (System 2)";
        case 0x0021: return "TP1 BCU2";
        case 0x0025: return "TP1 BCU2";
        case 0x0700: return "TP1 System 7";
        case 0x0701: return "TP1 System 7";
        case 0x0705: return "TP1 System B";
        case 0x07B0: return "TP1 System B device";
        case 0x091A: return "KNXnet/IP router";
        case 0x1012: return "PL110 BCU1";
        case 0x1013: return "PL110 BCU1";
        case 0x17B0: return "PL110 System B";
        case 0x2010: return "RF";
        case 0x2705: return "RF System B";
        case 0x27B0: return "RF System B device";
        case 0x57B0: return "TP1/IP System B device";
        default: return "";
    }
}

/**
 * @brief Result of one property read (distinguishes a value from a refused/absent/silent property).
 */
enum class DmRead
{
    Data,     // positive M_PropRead.con with >=1 element -> out/outLen filled
    Negative, // negative con (NoE==0): out is empty, propErr holds the cEMI error code
    NoAnswer, // connect ok but the server never answered this read
    NotOpen,  // session not open
};

/**
 * @brief One reusable Local-Device-Management connection: open once, read many properties, close.
 * @details All reads share the channel and increment the sequence counter; each server CON is ACKed as the
 *          spec requires. Bounds-checked throughout — a short/malformed datagram yields NoAnswer, never an
 *          over-read.
 */
class DevMgmtSession
{
  public:
    ~DevMgmtSession() { close(); }

    /**
     * @brief Open a DEVICE_MGMT_CONNECTION to ip:port. On failure fills *err (if given) and returns false.
     */
    bool open(const std::string& ip, uint16_t port, std::string* err = nullptr, int timeoutMs = 1500)
    {
        using namespace detail;
        close();
        _timeoutMs = timeoutMs;
        _ip = ip;
        _port = port;
        _s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (!sockValid(_s))
        {
            if (err) *err = "socket error";
            return false;
        }
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = INADDR_ANY;
        local.sin_port = 0;
        ::bind(_s, (sockaddr*)&local, sizeof(local));
        socklen_t ll = sizeof(local);
        ::getsockname(_s, (sockaddr*)&local, &ll);
        const uint16_t bp = ntohs(local.sin_port);
        _dst = sockaddr_in{};
        _dst.sin_family = AF_INET;
        _dst.sin_port = htons(port);
        _dst.sin_addr.s_addr = inet_addr(ip.c_str());
        _hpai[0] = 0x08;
        _hpai[1] = 0x01;
        _hpai[2] = _hpai[3] = _hpai[4] = _hpai[5] = 0;
        _hpai[6] = (unsigned char)(bp >> 8);
        _hpai[7] = (unsigned char)(bp & 0xFF);

        unsigned char req[64];
        int k = 0;
        const unsigned char hdr[6] = {0x06, 0x10, 0x02, 0x05, 0x00, 0x18};
        std::memcpy(req + k, hdr, 6);
        k += 6;
        std::memcpy(req + k, _hpai, 8);
        k += 8; // control HPAI (route-back)
        std::memcpy(req + k, _hpai, 8);
        k += 8; // data HPAI (route-back)
        req[k++] = 0x02;
        req[k++] = 0x03; // CRI: length 2, DEVICE_MGMT_CONNECTION
        send(req, k);

        unsigned char buf[512];
        const int n = recvSvc(buf, sizeof(buf), 0x0206, 1500); // CONNECT_RESPONSE
        if (n < 0)
        {
            if (err) *err = "no CONNECT_RESPONSE (timeout)";
            close();
            return false;
        }
        if (n < 8)
        {
            if (err) *err = "malformed CONNECT_RESPONSE";
            close();
            return false;
        }
        if (buf[7] != 0x00)
        {
            if (err)
            {
                char t[64];
                std::snprintf(t, sizeof(t), "connect refused (0x%02X %s)", buf[7], dmConnectStatusName(buf[7]));
                *err = t;
            }
            close();
            return false;
        }
        _chId = buf[6];
        _seq = 0;
        _open = true;
        if (err) err->clear();
        return true;
    }

    bool isOpen() const { return _open; }

    /**
     * @brief Read `noe` element(s) of a property at `startIdx` (one exchange, ROBUST).
     * @details Big-endian data span -> out/outLen. startIdx 0 returns the array element count. *propErr holds
     *          the cEMI error on Negative. Hardened for a multi-read session: paces before each request;
     *          retransmits with the SAME sequence on silence; matches the .con to OUR channel + IOT/inst/PID
     *          so a stale/duplicate .con from a previous read cannot be consumed as this one's answer (the
     *          classic "first read ok, rest lost" desync); advances the client sequence only on a confirmed
     *          matching answer.
     */
    DmRead readEx(uint16_t iot, uint8_t inst, uint8_t pid, uint8_t startIdx, uint8_t noe,
                  uint8_t* out, size_t cap, size_t& outLen, uint8_t* propErr = nullptr)
    {
        using namespace detail;
        outLen = 0;
        if (propErr) *propErr = 0;
        if (!_open) return DmRead::NotOpen;

        // Pace: the interface's local device management drops requests fired back-to-back too fast.
        if (_gapMs) std::this_thread::sleep_for(std::chrono::milliseconds(_gapMs));

        const uint8_t iotHi = (uint8_t)(iot >> 8), iotLo = (uint8_t)(iot & 0xFF);
        unsigned char req[64];
        int m = 0;
        req[m++] = 0x06;
        req[m++] = 0x10;
        req[m++] = 0x03;
        req[m++] = 0x10;
        req[m++] = 0x00;
        req[m++] = 0x11; // hdr (17)
        req[m++] = 0x04;
        req[m++] = _chId;
        req[m++] = _seq;
        req[m++] = 0x00; // connection header
        req[m++] = 0xFC; // M_PropRead.req
        req[m++] = iotHi;
        req[m++] = iotLo;
        req[m++] = inst;
        req[m++] = pid;
        req[m++] = (unsigned char)(((noe & 0x0F) << 4) | ((startIdx >> 8) & 0x0F)); // NoE | startIndex high nibble
        req[m++] = (unsigned char)(startIdx & 0xFF);                                // startIndex low

        unsigned char buf[1024];
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            send(req, m); // retransmit reuses the SAME sequence (KNX rule: re-send is not a new frame)
            const uint64_t deadline = nowMs() + 500;
            while (nowMs() < deadline)
            {
                const int n = recvSvc(buf, sizeof(buf), 0, (int)(deadline - nowMs()));
                if (n < 0) break; // window elapsed -> retransmit
                const uint16_t svc = (uint16_t)((buf[2] << 8) | buf[3]);
                if (svc != 0x0310) continue;             // 0x0311 (ACK of our req) or other -> ignore
                if (n < 17 || buf[7] != _chId) continue; // short frame / a frame for a different channel
                const uint8_t srvSeq = buf[8];
                unsigned char ack[10] = {0x06, 0x10, 0x03, 0x11, 0x00, 0x0A, 0x04, _chId, srvSeq, 0x00};
                send(ack, 10);                 // ACK EVERY server DEVICE_CONFIGURATION_REQUEST (also quiets its retransmits)
                if (buf[10] != 0xFB) continue; // not an M_PropRead.con
                // Must echo OUR request; otherwise it is a stale/duplicate .con from a previous read -> drain.
                if (buf[11] != iotHi || buf[12] != iotLo || buf[13] != inst || buf[14] != pid) continue;
                _seq = (uint8_t)(_seq + 1); // matching answer confirms delivery -> advance client sequence
                const uint8_t rnoe = (uint8_t)(buf[15] >> 4);
                if (rnoe == 0) // negative con: NoE==0, data byte (if present) = cEMI error code
                {
                    if (propErr && n >= 18) *propErr = buf[17];
                    return DmRead::Negative;
                }
                // positive con: data span is [17 .. totalLen), clamped to the actual datagram + caller cap
                const int total = (int)((buf[4] << 8) | buf[5]);
                int end = total > 0 && total <= n ? total : n;
                int len = end - 17;
                if (len < 0) len = 0;
                if ((size_t)len > cap) len = (int)cap;
                if (len > 0) std::memcpy(out, buf + 17, (size_t)len);
                outLen = (size_t)len;
                return DmRead::Data;
            }
        }
        return DmRead::NoAnswer; // silent after retries -> caller (readReliable) may re-open the channel
    }

    /**
     * @brief Same as readEx but self-heals a degraded/desynced session.
     * @details On total silence re-open a fresh channel once (sequence resets) and retry. Absent properties
     *          answer Negative and are NOT retried.
     */
    DmRead readReliable(uint16_t iot, uint8_t inst, uint8_t pid, uint8_t startIdx, uint8_t noe,
                        uint8_t* out, size_t cap, size_t& outLen, uint8_t* propErr = nullptr)
    {
        DmRead r = readEx(iot, inst, pid, startIdx, noe, out, cap, outLen, propErr);
        if ((r == DmRead::NoAnswer || r == DmRead::NotOpen) && reopen())
            r = readEx(iot, inst, pid, startIdx, noe, out, cap, outLen, propErr);
        return r;
    }

    /**
     * @brief Convenience: reliably read one element (NoE=1) at startIdx. true iff positive data arrived.
     */
    bool read(uint16_t iot, uint8_t inst, uint8_t pid, uint8_t startIdx, uint8_t* out, size_t cap, size_t& outLen)
    {
        return readReliable(iot, inst, pid, startIdx, 1, out, cap, outLen) == DmRead::Data;
    }

    /**
     * @brief Best-effort DISCONNECT and socket teardown; safe to call on an already-closed session.
     */
    void close()
    {
        using namespace detail;
        if (_open && sockValid(_s))
        {
            unsigned char dis[16] = {0x06, 0x10, 0x02, 0x09, 0x00, 0x10, _chId, 0x00};
            std::memcpy(dis + 8, _hpai, 8);
            send(dis, 16); // DISCONNECT_REQUEST
            unsigned char buf[64];
            recvSvc(buf, sizeof(buf), 0x020A, 400); // wait for DISCONNECT_RESPONSE so the device frees the channel
        }
        if (sockValid(_s)) sockClose(_s);
        _s = (detail::socket_t)(-1);
        _open = false;
    }

  private:
    /**
     * @brief Re-open a fresh channel (max _reopenBudget times per session) to recover from a stall/desync.
     */
    bool reopen()
    {
        if (_ip.empty() || _reopenBudget <= 0) return false;
        --_reopenBudget;
        close(); // frees the old (possibly degraded) channel on the device
        return open(_ip, _port, nullptr, _timeoutMs);
    }
    /**
     * @brief Send one datagram to the session's destination.
     */
    void send(const unsigned char* p, int n)
    {
        ::sendto(_s, (const char*)p, n, 0, (sockaddr*)&_dst, sizeof(_dst));
    }
    /**
     * @brief Wait up to `ms` for one datagram matching service `want` (0 = any). Returns length or -1.
     */
    int recvSvc(unsigned char* buf, int cap, uint16_t want, int ms)
    {
        using namespace detail;
        const uint64_t deadline = nowMs() + (uint64_t)ms;
        while (nowMs() < deadline)
        {
            const uint64_t rem = deadline - nowMs();
            timeval tv;
            tv.tv_sec = (long)(rem / 1000);
            tv.tv_usec = (long)((rem % 1000) * 1000);
            fd_set rf;
            FD_ZERO(&rf);
            FD_SET(_s, &rf);
            if (::select((int)_s + 1, &rf, nullptr, nullptr, &tv) <= 0) continue;
            sockaddr_in from{};
            socklen_t fl = sizeof(from);
            const int n = (int)::recvfrom(_s, (char*)buf, cap, 0, (sockaddr*)&from, &fl);
            if (n >= 6 && (want == 0 || ((buf[2] << 8 | buf[3]) == want))) return n;
        }
        return -1;
    }

    detail::socket_t _s = (detail::socket_t)(-1);
    sockaddr_in _dst{};
    unsigned char _hpai[8] = {0};
    std::string _ip; // remembered for self-reopen on a stalled session
    uint16_t _port = 0;
    uint8_t _chId = 0;
    uint8_t _seq = 0;
    bool _open = false;
    int _timeoutMs = 1500;
    int _gapMs = 10;       // inter-request pacing (ms) — keep the device's local-DM state machine in step
    int _reopenBudget = 3; // max self-reopens per session (bounds worst-case time on a truly dead PID)
};

/**
 * @brief The curated interface steckbrief filled by queryInterfaceDetails.
 */
struct InterfaceDetails
{
    bool ok = false;       // connect succeeded (individual PIDs may still be absent)
    char reason[64] = {0}; // why ok==false (connect diagnostic); empty on success

    // Device Object (IOT 0)
    bool haveSerial = false;
    uint8_t serial[6] = {0};
    bool haveMfr = false;
    uint16_t manufacturer = 0;
    bool haveOrder = false;
    char order[33] = {0}; // PID_ORDER_INFO = PDT_GENERIC_10 (10 bytes; spec-fixed)
    bool haveHardware = false;
    uint8_t hardware[6] = {0};
    size_t hardwareLen = 0;
    bool haveMask = false;
    uint16_t mask = 0; // PID_DEVICE_DESCRIPTOR = KNX mask version
    bool haveFirmware = false;
    uint16_t firmware = 0; // PID_FIRMWARE_REVISION

    // max interface APDU (cEMI Server IOT 8 PID 68/69, else Device IOT 0 PID 56)
    bool haveApdu = false;
    uint16_t maxApdu = 0;
    uint16_t apduIot = 0;
    uint8_t apduPid = 0;

    // KNXnet/IP Parameter Object (IOT 11)
    bool haveFriendly = false;
    char friendly[31] = {0};
    bool haveMac = false;
    uint8_t mac[6] = {0};
    bool haveIp = false;
    uint32_t ip = 0;
    bool haveSubnet = false;
    uint32_t subnet = 0;
    bool haveGateway = false;
    uint32_t gateway = 0;
    bool haveIpMethod = false;
    uint8_t ipMethod = 0;
    bool haveProjId = false;
    uint16_t projInstId = 0;
    std::vector<uint16_t> tunnelAddrs; // PID_ADDITIONAL_INDIVIDUAL_ADDRESSES: the tunnel-slot PAs
};

/**
 * @brief Big-endian bytes -> uint16 / uint32 (bounds already known by caller).
 */
namespace detail
{
inline uint16_t be16(const uint8_t* b) { return (uint16_t)((b[0] << 8) | b[1]); }
inline uint32_t be32(const uint8_t* b) { return (uint32_t)((b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]); }
} // namespace detail

/**
 * @brief Read a whole property ARRAY into `out` (bytes). Returns the total byte count.
 * @details Index 0 gives the element count, then batched reads of up to 15 elements each (the M_PropRead
 *          NoE field is 4-bit). `elemSize` is the size of one element (chars=1, U16 addresses=2). Bounded
 *          by cap and maxCount.
 */
inline size_t readPropArray(DevMgmtSession& s, uint16_t iot, uint8_t inst, uint8_t pid,
                            uint8_t elemSize, uint8_t* out, size_t cap, uint16_t maxCount = 64)
{
    uint8_t d[256];
    size_t L = 0;
    if (!s.read(iot, inst, pid, 0, d, sizeof(d), L) || L < 1) return 0;
    uint16_t count = L >= 2 ? detail::be16(d) : d[0];
    if (count == 0 || count > maxCount) count = count > maxCount ? maxCount : 0;
    size_t total = 0;
    uint16_t idx = 1;
    while (idx <= count && total < cap)
    {
        uint8_t noe = (uint8_t)((count - idx + 1) > 15 ? 15 : (count - idx + 1));
        if (s.readReliable(iot, inst, pid, (uint8_t)idx, noe, d, sizeof(d), L) != DmRead::Data || L == 0) break;
        size_t take = L;
        if (take > cap - total) take = cap - total;
        std::memcpy(out + total, d, take);
        total += take;
        idx = (uint16_t)(idx + noe);
        (void)elemSize;
    }
    return total;
}

/**
 * @brief Open ONE device-management session and read the curated interface steckbrief into `out`.
 * @details Absent/refused PIDs simply leave their have* flag false — never an error, never an over-read.
 */
inline bool queryInterfaceDetails(const std::string& ip, uint16_t port, InterfaceDetails& out, int timeoutMs = 1500)
{
    out = InterfaceDetails{};
    DevMgmtSession s;
    std::string err;
    if (!s.open(ip, port, &err, timeoutMs))
    {
        std::snprintf(out.reason, sizeof(out.reason), "%s", err.c_str());
        return false;
    }
    out.ok = true;

    uint8_t d[256];
    size_t L = 0;

    // --- Device Object (IOT 0, instance 1) ---
    if (s.read(IOT_DEVICE, 1, 11, 1, d, sizeof(d), L) && L >= 6)
    {
        std::memcpy(out.serial, d, 6);
        out.haveSerial = true;
    }
    if (s.read(IOT_DEVICE, 1, 12, 1, d, sizeof(d), L) && L >= 2)
    {
        out.manufacturer = detail::be16(d);
        out.haveMfr = true;
    }
    if (s.read(IOT_DEVICE, 1, 15, 1, d, sizeof(d), L) && L > 0)
    {
        // PID_ORDER_INFO is a manufacturer order code: ASCII for OpenKNX ("OpenKnxIPI"), but BINARY for many
        // others (e.g. a Weinzierl N148). Show clean printable ASCII as text; otherwise hex ($..) like ETS —
        // so a non-ASCII order code never renders as garbage.
        bool printable = true;
        size_t nz = 0;
        for (size_t i = 0; i < L; ++i)
        {
            if (d[i]) ++nz;
            if (d[i] && (d[i] < 0x20 || d[i] >= 0x7F)) printable = false;
        }
        size_t j = 0;
        if (printable && nz)
        {
            for (size_t i = 0; i < L && j < sizeof(out.order) - 1; ++i)
                if (d[i] >= 0x20 && d[i] < 0x7F) out.order[j++] = (char)d[i];
            out.order[j] = 0;
            while (j > 0 && out.order[j - 1] == ' ')
                out.order[--j] = 0;
        }
        else // binary order code -> "$" + hex, matching ETS's Geräteinfo
        {
            out.order[j++] = '$';
            for (size_t i = 0; i < L && j + 2 < sizeof(out.order); ++i)
            {
                std::snprintf(out.order + j, 3, "%02X", d[i]);
                j += 2;
            }
            out.order[j] = 0;
        }
        out.haveOrder = j > 0;
    }
    if (s.read(IOT_DEVICE, 1, 78, 1, d, sizeof(d), L) && L > 0)
    {
        out.hardwareLen = L > sizeof(out.hardware) ? sizeof(out.hardware) : L;
        std::memcpy(out.hardware, d, out.hardwareLen);
        out.haveHardware = true;
    }
    if (s.read(IOT_DEVICE, 1, 83, 1, d, sizeof(d), L) && L >= 2)
    {
        out.mask = detail::be16(d);
        out.haveMask = true;
    }
    if (s.read(IOT_DEVICE, 1, 9, 1, d, sizeof(d), L) && L >= 1)
    {
        out.firmware = L >= 2 ? detail::be16(d) : d[0];
        out.haveFirmware = true;
    }

    // --- max interface APDU: cEMI Server (IOT 8) PID 68/69, else Device (IOT 0) PID 56 ---
    struct
    {
        uint16_t iot;
        uint8_t pid;
    } apduCands[] = {{IOT_CEMI_SERVER, 68}, {IOT_CEMI_SERVER, 69}, {IOT_DEVICE, 56}};
    for (auto& c : apduCands)
    {
        if (s.read(c.iot, 1, c.pid, 1, d, sizeof(d), L) && L >= 2)
        {
            out.maxApdu = detail::be16(d);
            out.apduIot = c.iot;
            out.apduPid = c.pid;
            out.haveApdu = out.maxApdu != 0;
            if (out.haveApdu) break;
        }
    }

    // --- KNXnet/IP Parameter Object (IOT 11, instance 1) ---
    // PID_FRIENDLY_NAME is a 30-element char array -> read the whole array (NoE=1 would give one char).
    {
        uint8_t fn[64];
        const size_t fl = readPropArray(s, IOT_KNXNETIP_PARAM, 1, 76, 1, fn, sizeof(fn), 30);
        size_t j = 0;
        for (size_t i = 0; i < fl && j < sizeof(out.friendly) - 1; ++i)
            if (fn[i] >= 0x20 && fn[i] < 0x7F) out.friendly[j++] = (char)fn[i];
        out.friendly[j] = 0;
        while (j > 0 && out.friendly[j - 1] == ' ')
            out.friendly[--j] = 0;
        out.haveFriendly = j > 0;
    }
    if (s.read(IOT_KNXNETIP_PARAM, 1, 64, 1, d, sizeof(d), L) && L >= 6)
    {
        std::memcpy(out.mac, d, 6);
        out.haveMac = true;
    }
    if (s.read(IOT_KNXNETIP_PARAM, 1, 57, 1, d, sizeof(d), L) && L >= 4)
    {
        out.ip = detail::be32(d);
        out.haveIp = true;
    }
    if (s.read(IOT_KNXNETIP_PARAM, 1, 58, 1, d, sizeof(d), L) && L >= 4)
    {
        out.subnet = detail::be32(d);
        out.haveSubnet = true;
    }
    if (s.read(IOT_KNXNETIP_PARAM, 1, 59, 1, d, sizeof(d), L) && L >= 4)
    {
        out.gateway = detail::be32(d);
        out.haveGateway = true;
    }
    if (s.read(IOT_KNXNETIP_PARAM, 1, 54, 1, d, sizeof(d), L) && L >= 1)
    {
        out.ipMethod = d[0];
        out.haveIpMethod = true;
    }
    if (s.read(IOT_KNXNETIP_PARAM, 1, 51, 1, d, sizeof(d), L) && L >= 2)
    {
        out.projInstId = detail::be16(d);
        out.haveProjId = true;
    }

    // PID_ADDITIONAL_INDIVIDUAL_ADDRESSES (array of U16 PAs = the tunnel slots): read the whole array.
    {
        uint8_t ta[160];
        const size_t tl = readPropArray(s, IOT_KNXNETIP_PARAM, 1, 53, 2, ta, sizeof(ta), 64);
        for (size_t i = 0; i + 1 < tl; i += 2)
            out.tunnelAddrs.push_back(detail::be16(ta + i));
    }

    s.close();
    return true;
}

/**
 * @brief Back-compat thin wrapper (the former main.cpp one-shot): read max interface APDU via one session.
 * @details Mirrors the original diagnostic `reason` behaviour so both the one-shot CLI and `ftc mc` keep working.
 */
inline uint16_t queryMaxApduDeviceMgmt(const std::string& ip, uint16_t port, uint8_t* outPid,
                                       char* outReason = nullptr, size_t reasonCap = 0)
{
    if (outPid) *outPid = 0;
    auto setReason = [&](const char* r) { if (outReason && reasonCap) std::snprintf(outReason, reasonCap, "%s", r); };
    setReason("");

    DevMgmtSession s;
    std::string err;
    if (!s.open(ip, port, &err, 1500))
    {
        setReason(err.c_str());
        return 0;
    }

    struct
    {
        uint16_t iot;
        uint8_t pid;
    } cands[] = {{IOT_CEMI_SERVER, 68}, {IOT_CEMI_SERVER, 69}, {IOT_CEMI_SERVER, 56}, {IOT_DEVICE, 56}};
    uint16_t value = 0;
    bool anyCon = false, anyPropErr = false;
    uint8_t lastPropErr = 0;
    uint8_t d[64];
    size_t L = 0;
    for (auto& c : cands)
    {
        uint8_t pe = 0;
        const DmRead r = s.readReliable(c.iot, 1, c.pid, 1, 1, d, sizeof(d), L, &pe);
        if (r == DmRead::Data && L >= 2)
        {
            value = detail::be16(d);
            anyCon = true;
            if (outPid) *outPid = c.pid;
            if (value) break;
        }
        else if (r == DmRead::Negative)
        {
            anyCon = true;
            anyPropErr = true;
            lastPropErr = pe;
        }
        else if (r == DmRead::Data)
        {
            anyCon = true;
        } // positive con but no usable U16 (empty)
    }
    if (!value)
    {
        if (anyPropErr)
        {
            if (outReason && reasonCap) std::snprintf(outReason, reasonCap, "property refused (0x%02X %s)", lastPropErr, dmPropErrorName(lastPropErr));
        }
        else if (anyCon)
            setReason("property present but empty");
        else
            setReason("connect ok, no M_PropRead answer");
    }
    s.close();
    return value;
}

} // namespace ftc

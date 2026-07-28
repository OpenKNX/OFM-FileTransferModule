// ┬────┴  OFM-FileTransferModule / ftc-cli
// ■ KNX   2026 OpenKNX - Erkan Çolak
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026, Erkan Çolak
//
// main.cpp — the FTC ftc-cli entrypoint.
//
// Drives the UNCHANGED embedded FileTransferClient (../src) over a KNXnet/IP tunnel (g_knxTunnel). It
// parses the CLI, opens the tunnel (or runs KNXnet/IP discovery), submits ONE ftc command through the
// same Module entry the on-device console uses (processCommand), then runs the device's cooperative
// loop verbatim: pump the tunnel, step the client, poll for completion. `console` gets an interactive,
// non-blocking-stdin loop instead of a completion poll.
//
// Contract seams this file relies on (built by other agents):
//   * g_knxTunnel        — src/knx_ip_tunnel.{h,cpp}: connect/disconnect/pump/assignedPA.
//   * openknxFileTransferClient / FtcPhase / openknx — the compiled ../src set + shim (OpenKNX.h).
//   * openknx.console.feedLine(const char*) — ASSUMED host-only Console helper that forwards a typed
//     line to the line-sink the client installed on `requestConsole`. See report note; single call site.

#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

// --- cross-platform sockets (discovery only; the tunnel owns its own socket) --------------------
#ifdef _WIN32
    #include <conio.h>
    #include <io.h>
    #include <windows.h>
    #include <winsock2.h>
    #include <ws2tcpip.h>
typedef SOCKET sock_t;
    #define CLOSESOCK closesocket
#else
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <sys/select.h>
    #include <sys/socket.h>
    #include <unistd.h>
typedef int sock_t;
    #define INVALID_SOCKET (-1)
    #define CLOSESOCK close
#endif

// The transport seam + the compiled embedded client (pulls the shim's OpenKNX.h -> `openknx`).
#include "FileTransferClient.h"
#include "knx_ip_tunnel.h"

// ================================================================================================
// small helpers
// ================================================================================================

// One-shot socket layer init/teardown (no-op on POSIX).
static bool socketStartup()
{
#ifdef _WIN32
    WSADATA w;
    return WSAStartup(MAKEWORD(2, 2), &w) == 0;
#else
    return true;
#endif
}
static void socketCleanup()
{
#ifdef _WIN32
    WSACleanup();
#endif
}

// millis-free monotonic clock in ms (host CLI may use std::chrono directly; the embedded rule is loop-only).
static uint64_t nowMs()
{
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// ================================================================================================
// KNXnet/IP discovery (SEARCH_REQUEST -> SEARCH_RESPONSE), self-contained
// ================================================================================================

// Learn the local interface IP the OS would use to reach the KNX multicast group. A connected UDP
// socket needs no packet: getsockname() reports the chosen source address. 0.0.0.0 fallback = route-back.
static uint32_t localIpForMulticast()
{
    sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return 0;
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(3671);
    dst.sin_addr.s_addr = inet_addr("224.0.23.12");
    uint32_t ip = 0;
    if (connect(s, (sockaddr*)&dst, sizeof(dst)) == 0)
    {
        sockaddr_in loc{};
        socklen_t ll = sizeof(loc);
        if (getsockname(s, (sockaddr*)&loc, &ll) == 0) ip = loc.sin_addr.s_addr; // network order
    }
    CLOSESOCK(s);
    return ip;
}

// Send a KNXnet/IP SEARCH_REQUEST to 224.0.23.12:3671 and print every responding interface (name + IP).
// Returns the number of interfaces found.
static int knxDiscover(uint16_t port)
{
    sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET)
    {
        std::fprintf(stderr, "discover: socket() failed\n");
        return -1;
    }
    // Bind to an ephemeral local port; that port + the local IP become our discovery HPAI.
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = 0;
    if (bind(s, (sockaddr*)&local, sizeof(local)) != 0)
    {
        std::fprintf(stderr, "discover: bind() failed\n");
        CLOSESOCK(s);
        return -1;
    }
    socklen_t ll = sizeof(local);
    getsockname(s, (sockaddr*)&local, &ll);
    uint16_t boundPort = ntohs(local.sin_port);
    uint32_t hpaiIp = localIpForMulticast(); // network-order; 0 => advertise 0.0.0.0 (NAT route-back)

    unsigned char ttl = 4;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof(ttl));

    // SEARCH_REQUEST = 6-byte KNXnet/IP header + 8-byte discovery-endpoint HPAI.
    unsigned char pkt[14];
    pkt[0] = 0x06; // header length
    pkt[1] = 0x10; // protocol version 1.0
    pkt[2] = 0x02;
    pkt[3] = 0x01; // service type SEARCH_REQUEST (0x0201)
    pkt[4] = 0x00;
    pkt[5] = 0x0E;                             // total length 14
    pkt[6] = 0x08;                             // HPAI length
    pkt[7] = 0x01;                             // host protocol code: IPv4 UDP
    std::memcpy(&pkt[8], &hpaiIp, 4);          // IP (network order)
    pkt[12] = (unsigned char)(boundPort >> 8); // port (big-endian)
    pkt[13] = (unsigned char)(boundPort & 0xFF);

    sockaddr_in grp{};
    grp.sin_family = AF_INET;
    grp.sin_port = htons(port);
    grp.sin_addr.s_addr = inet_addr("224.0.23.12");
    if (sendto(s, (const char*)pkt, sizeof(pkt), 0, (sockaddr*)&grp, sizeof(grp)) < 0)
    {
        std::fprintf(stderr, "discover: sendto() failed\n");
        CLOSESOCK(s);
        return -1;
    }

    std::printf("Searching for KNXnet/IP interfaces on 224.0.23.12:%u ...\n", (unsigned)port);
    int found = 0;
    const uint64_t deadline = nowMs() + 3000; // 3 s collection window
    for (;;)
    {
        uint64_t now = nowMs();
        if (now >= deadline) break;
        struct timeval tv;
        uint64_t rem = deadline - now;
        tv.tv_sec = (long)(rem / 1000);
        tv.tv_usec = (long)((rem % 1000) * 1000);
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(s, &rfds);
        int r = select((int)s + 1, &rfds, nullptr, nullptr, &tv);
        if (r <= 0) continue;

        unsigned char buf[512];
        sockaddr_in from{};
        socklen_t fl = sizeof(from);
        int n = (int)recvfrom(s, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
        // SEARCH_RESPONSE (0x0202): 6-hdr + 8-HPAI(control) + Device-Info DIB. Name is 30 bytes at DIB+24.
        if (n < 6 + 8 + 54) continue;
        if (!(buf[2] == 0x02 && buf[3] == 0x02)) continue;
        char ip[16];
        std::snprintf(ip, sizeof(ip), "%u.%u.%u.%u", buf[8], buf[9], buf[10], buf[11]);
        const unsigned char* name = &buf[6 + 8 + 24]; // friendly name (30 bytes, NUL-padded)
        char nm[31];
        std::memcpy(nm, name, 30);
        nm[30] = 0;
        std::printf("  %-30s  %s\n", nm[0] ? nm : "(unnamed)", ip);
        ++found;
    }
    if (!found) std::printf("  (no interfaces responded)\n");
    CLOSESOCK(s);
    return found;
}

// ================================================================================================
// non-blocking stdin line reader (console mode)
// ================================================================================================

// Pulls completed input lines without blocking. POSIX: select() on fd 0 then read(). Windows: _kbhit()
// + _getch() to assemble a line (with local echo). Returns true and fills `line` when a line is ready.
class StdinLines
{
  public:
    bool poll(std::string& line)
    {
#ifdef _WIN32
        while (_kbhit())
        {
            int c = _getch();
            if (c == '\r' || c == '\n')
            {
                std::printf("\n");
                std::fflush(stdout);
                line.swap(_buf);
                _buf.clear();
                return true;
            }
            if (c == '\b' || c == 127) // backspace
            {
                if (!_buf.empty())
                {
                    _buf.pop_back();
                    std::printf("\b \b");
                    std::fflush(stdout);
                }
                continue;
            }
            _buf.push_back((char)c);
            std::putchar(c); // echo
            std::fflush(stdout);
        }
        return false;
#else
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(0, &rfds);
        struct timeval tv{0, 0}; // non-blocking probe
        if (select(1, &rfds, nullptr, nullptr, &tv) <= 0) return false;
        char tmp[256];
        int n = (int)read(0, tmp, sizeof(tmp));
        if (n <= 0) return false;
        for (int i = 0; i < n; ++i)
        {
            if (tmp[i] == '\n')
            {
                line.swap(_buf);
                _buf.clear();
                return true; // one line per poll; the rest stays buffered
            }
            _buf.push_back(tmp[i]);
        }
        return false;
#endif
    }

  private:
    std::string _buf;
};

// ================================================================================================
// CLI
// ================================================================================================

#define FTC_CLI_VERSION "1.0.0"

// ---- pretty output --------------------------------------------------------------------------------
static bool g_color = false; // set from isatty()/NO_COLOR in main()

// Enable UTF-8 + ANSI on the current terminal; return whether we should emit color.
static bool initTerminal()
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    bool tty = _isatty(_fileno(stdout)) != 0;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (tty && GetConsoleMode(h, &mode)) SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#else
    bool tty = isatty(fileno(stdout)) != 0;
#endif
    return tty && std::getenv("NO_COLOR") == nullptr; // honor the NO_COLOR convention
}

// SGR helpers (empty strings when color is off, so the same printf works either way).
static const char* Cg() { return g_color ? "\x1b[38;5;40m" : ""; } // OpenKNX green
static const char* Cb() { return g_color ? "\x1b[1m" : ""; }       // bold
static const char* Cc() { return g_color ? "\x1b[36m" : ""; }      // cyan (commands)
static const char* Cy() { return g_color ? "\x1b[33m" : ""; }      // yellow (headers)
static const char* Cd() { return g_color ? "\x1b[2m" : ""; }       // dim
static const char* Cr() { return g_color ? "\x1b[0m" : ""; }       // reset

// The OpenKNX logo mark + copyright banner.
static void banner()
{
    std::printf("\n");
    std::printf("  Open %s■%s\n", Cg(), Cr());
    std::printf("  %s┬────┴%s  %s%sftc%s  %snative KNX FileTransferClient over KNXnet/IP%s\n",
                Cg(), Cr(), Cb(), Cc(), Cr(), Cd(), Cr());
    std::printf("  %s■%s KNX   %s© 2026 OpenKNX · Erkan Çolak · GPL-3.0%s\n", Cg(), Cr(), Cd(), Cr());
    std::printf("          %swiki.openknx.de · forum.openknx.de%s\n", Cd(), Cr());
    std::printf("\n");
}

static void printVersion()
{
    banner();
    std::printf("  %sftc-cli%s    %s%s%s\n", Cd(), Cr(), Cb(), FTC_CLI_VERSION, Cr());
    std::printf("  %sprotocol%s   FTC %s\n", Cd(), Cr(), MODULE_FileTransferModule_Version);
    std::printf("  %sbuilt%s      %s%s %s%s\n", Cd(), Cr(), Cd(), __DATE__, __TIME__, Cr());
    std::printf("\n");
}

// One "  name         description" row: name in cyan, description dimmed, aligned.
static void cmdRow(const char* name, const char* desc)
{
    std::printf("  %s%-34s%s %s%s%s\n", Cc(), name, Cr(), Cd(), desc, Cr());
}

// ================================================================================================
// Interface steckbrief: unicast DESCRIPTION_REQUEST -> parse the DIBs -> identity line / full panel
// All fields come straight from the KNXnet/IP DESCRIPTION_RESPONSE (03_08_02 Core 7.5.4), no
// connection needed, so it works against any interface (v1 or v2). Missing DIBs -> "n/a".
// ================================================================================================

static const char* knxMediumName(uint8_t m)
{
    switch (m)
    {
    case 0x01: return "TP0";
    case 0x02: return "TP1 (twisted pair)";
    case 0x04: return "PL110";
    case 0x08: return "PL132";
    case 0x10: return "RF";
    case 0x20: return "IP";
    default:   return "unknown";
    }
}
static const char* svcFamilyName(uint8_t id)
{
    switch (id)
    {
    case 0x02: return "Core";
    case 0x03: return "DevMgmt";
    case 0x04: return "Tunnelling";
    case 0x05: return "Routing";
    case 0x06: return "RemoteLog";
    case 0x07: return "RemoteCfg";
    case 0x08: return "ObjServer";
    case 0x09: return "Security";
    default:   return nullptr;
    }
}
static const char* ipMethodName(uint8_t m)
{
    // Current IP assignment method (03_08_03): bitset, but a device reports the one in use.
    if (m & 0x04) return "DHCP";
    if (m & 0x02) return "BootP";
    if (m & 0x08) return "AutoIP";
    if (m & 0x01) return "manual";
    return "?";
}

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
    // APDU auto-detection (filled after DESCRIPTION): "reported" = read from the interface over Device
    // Management (M_PropRead, zero KNX-bus traffic, spec-exact); "measured" = the largest response frame that
    // actually round-trips, probed via A_Memory_Read (a few small reads on the bus; --no-mem-probe disables).
    uint16_t apduReported = 0; // 0 = not obtained
    uint8_t apduReportedPid = 0;
    uint16_t apduMeasured = 0; // 0 = not probed / no answer
};

// Walk the DIB block (Core 7.5.4): each DIB = [structLen][typeCode][payload...].
static void parseDibs(const unsigned char* d, int len, IfaceDesc& o)
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
            for (int i = 2; i + 1 < dl; i += 2) o.famVer[b[i]] = b[i + 1];
        }
        else if (dt == 0x04 && dl >= 20) // IP Current Config DIB
        {
            o.hasIp = true;
            o.ip = (uint32_t)((b[2] << 24) | (b[3] << 16) | (b[4] << 8) | b[5]);
            o.subnet = (uint32_t)((b[6] << 24) | (b[7] << 16) | (b[8] << 8) | b[9]);
            o.gw = (uint32_t)((b[10] << 24) | (b[11] << 16) | (b[12] << 8) | b[13]);
            o.ipMethod = b[18];
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

// Unicast DESCRIPTION_REQUEST (0x0203) -> DESCRIPTION_RESPONSE (0x0204). ~2 s best-effort.
static bool queryInterface(const std::string& ip, uint16_t port, IfaceDesc& o)
{
    sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = 0;
    bind(s, (sockaddr*)&local, sizeof(local));
    socklen_t ll = sizeof(local);
    getsockname(s, (sockaddr*)&local, &ll);
    uint16_t bp = ntohs(local.sin_port);
    // header(6) + control HPAI(8): IP 0.0.0.0 => the server replies to the packet source (route-back).
    unsigned char pkt[14] = {0x06, 0x10, 0x02, 0x03, 0x00, 0x0E, 0x08, 0x01, 0, 0, 0, 0,
                             (unsigned char)(bp >> 8), (unsigned char)(bp & 0xFF)};
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    dst.sin_addr.s_addr = inet_addr(ip.c_str());
    sendto(s, (const char*)pkt, sizeof(pkt), 0, (sockaddr*)&dst, sizeof(dst));
    unsigned char buf[1024];
    bool got = false;
    uint64_t deadline = nowMs() + 2000;
    while (nowMs() < deadline)
    {
        uint64_t rem = deadline - nowMs();
        struct timeval tv;
        tv.tv_sec = (long)(rem / 1000);
        tv.tv_usec = (long)((rem % 1000) * 1000);
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(s, &rf);
        if (select((int)s + 1, &rf, nullptr, nullptr, &tv) <= 0) continue;
        sockaddr_in from{};
        socklen_t fl = sizeof(from);
        int n = (int)recvfrom(s, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
        if (n >= 6 && buf[2] == 0x02 && buf[3] == 0x04) // DESCRIPTION_RESPONSE
        {
            parseDibs(buf + 6, n - 6, o);
            o.ok = true;
            got = true;
            break;
        }
    }
    CLOSESOCK(s);
    return got;
}

// Read the interface's max-APDU over a KNXnet/IP Device-Management connection (M_PropRead of the cEMI Server
// Object). This is a pure IP-side query to the interface -- it puts NOTHING on the KNX bus -- and works on v1
// interfaces too (they advertise DevMgmt even without the v2 Extended Device Info DIB that would carry the
// value in DESCRIPTION). Spec: 03_08_03 Device Management (DEVICE_CONFIGURATION_REQUEST 0x0310, cEMI
// M_PropRead.req 0xFC / .con 0xFB). Tries cEMI Server Object (IOT 8) PID 68 (max interface APDU) -> 69 (max
// local APDU) -> 56 (max APDU) -> Device Object (IOT 0) PID 56. Returns the value (>0) + the PID that
// answered via *outPid; 0 on no answer. Self-contained on a side socket, ~1.5 s worst case.
static uint16_t queryMaxApduDeviceMgmt(const std::string& ip, uint16_t port, uint8_t* outPid)
{
    if (outPid) *outPid = 0;
    sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return 0;
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = 0;
    bind(s, (sockaddr*)&local, sizeof(local));
    socklen_t ll = sizeof(local);
    getsockname(s, (sockaddr*)&local, &ll);
    const uint16_t bp = ntohs(local.sin_port);
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    dst.sin_addr.s_addr = inet_addr(ip.c_str());

    auto sendTo = [&](const unsigned char* p, int n) { sendto(s, (const char*)p, n, 0, (sockaddr*)&dst, sizeof(dst)); };
    // Wait up to `ms` for one datagram matching service `want` (0 = any). Returns length or -1.
    auto recvSvc = [&](unsigned char* buf, int cap, uint16_t want, int ms) -> int {
        const uint64_t deadline = nowMs() + (uint64_t)ms;
        while (nowMs() < deadline)
        {
            uint64_t rem = deadline - nowMs();
            struct timeval tv;
            tv.tv_sec = (long)(rem / 1000);
            tv.tv_usec = (long)((rem % 1000) * 1000);
            fd_set rf;
            FD_ZERO(&rf);
            FD_SET(s, &rf);
            if (select((int)s + 1, &rf, nullptr, nullptr, &tv) <= 0) continue;
            sockaddr_in from{};
            socklen_t fl = sizeof(from);
            int n = (int)recvfrom(s, (char*)buf, cap, 0, (sockaddr*)&from, &fl);
            if (n >= 6 && (want == 0 || ((buf[2] << 8 | buf[3]) == want))) return n;
        }
        return -1;
    };

    // --- CONNECT (DEVICE_MGMT_CONNECTION, CRI type 0x03; control+data HPAI use 0.0.0.0:bp route-back) ---
    unsigned char req[64];
    unsigned char hpai[8] = {0x08, 0x01, 0, 0, 0, 0, (unsigned char)(bp >> 8), (unsigned char)(bp & 0xFF)};
    int k = 0;
    const unsigned char connHdr[6] = {0x06, 0x10, 0x02, 0x05, 0x00, 0x18};
    memcpy(req + k, connHdr, 6); k += 6;
    memcpy(req + k, hpai, 8); k += 8;
    memcpy(req + k, hpai, 8); k += 8;
    req[k++] = 0x02; req[k++] = 0x03; // CRI: length 2, DEVICE_MGMT_CONNECTION
    sendTo(req, k);
    unsigned char buf[512];
    int n = recvSvc(buf, sizeof(buf), 0x0206, 1500); // CONNECT_RESPONSE
    if (n < 8 || buf[7] != 0x00) { CLOSESOCK(s); return 0; }
    const uint8_t chId = buf[6];

    // --- M_PropRead.req for each candidate (IOT, PID), reusing the channel; seq per request ---
    struct Cand { uint16_t iot; uint8_t pid; };
    const Cand cands[] = {{8, 68}, {8, 69}, {8, 56}, {0, 56}};
    uint16_t value = 0;
    uint8_t seq = 0;
    for (const Cand& c : cands)
    {
        int m = 0;
        req[m++] = 0x06; req[m++] = 0x10; req[m++] = 0x03; req[m++] = 0x10; req[m++] = 0x00; req[m++] = 0x11; // hdr, total 17
        req[m++] = 0x04; req[m++] = chId; req[m++] = seq; req[m++] = 0x00; // connection header
        req[m++] = 0xFC; // M_PropRead.req
        req[m++] = (unsigned char)(c.iot >> 8); req[m++] = (unsigned char)(c.iot & 0xFF);
        req[m++] = 0x01;   // object instance 1
        req[m++] = c.pid;
        req[m++] = 0x10; req[m++] = 0x01; // NoE = 1, start index = 1
        sendTo(req, m);
        seq = (uint8_t)(seq + 1);

        // Collect frames until the server's DEVICE_CONFIGURATION_REQUEST carrying M_PropRead.con arrives.
        const uint64_t candDeadline = nowMs() + 1200;
        while (nowMs() < candDeadline && value == 0)
        {
            n = recvSvc(buf, sizeof(buf), 0, (int)(candDeadline - nowMs()));
            if (n < 6) break;
            const uint16_t svc = (uint16_t)((buf[2] << 8) | buf[3]);
            if (svc != 0x0310) continue; // ignore our own ACK echo etc.
            const uint8_t srvSeq = buf[8];
            unsigned char ack[10] = {0x06, 0x10, 0x03, 0x11, 0x00, 0x0A, 0x04, chId, srvSeq, 0x00};
            sendTo(ack, 10); // DEVICE_CONFIGURATION_ACK
            if (n < 17 || buf[10] != 0xFB) continue; // not an M_PropRead.con
            const uint8_t noe = (uint8_t)(buf[15] >> 4);
            if (noe > 0 && n >= 19) // data present: U16 big-endian
            {
                value = (uint16_t)((buf[17] << 8) | buf[18]);
                if (outPid) *outPid = c.pid;
            }
            break; // this candidate answered (value or error) -> next candidate or done
        }
        if (value) break;
    }

    unsigned char dis[16] = {0x06, 0x10, 0x02, 0x09, 0x00, 0x10, chId, 0x00};
    memcpy(dis + 8, hpai, 8);
    sendTo(dis, 16); // DISCONNECT_REQUEST (best-effort)
    CLOSESOCK(s);
    return value;
}

static std::string famList(const IfaceDesc& o)
{
    std::string s;
    for (int id = 2; id <= 9; ++id)
        if (o.famVer[id])
        {
            const char* nm = svcFamilyName((uint8_t)id);
            if (!nm) continue;
            if (!s.empty()) s += " · ";
            s += nm;
            s += " v";
            s += std::to_string(o.famVer[id]);
        }
    return s;
}

// Default mode: one compact identity line.
static void printIfaceLine(const IfaceDesc& o)
{
    if (!o.ok)
    {
        std::printf("Interface  %s(no DESCRIPTION_RESPONSE)%s\n", Cd(), Cr());
        return;
    }
    const uint16_t apduVal = o.apduReported ? o.apduReported : o.maxLocalApdu; // device-mgmt read, else DESCRIPTION DIB
    char apdu[40];
    if (o.apduMeasured && o.apduMeasured != apduVal)
        std::snprintf(apdu, sizeof(apdu), "APDU %u/meas %u", apduVal, o.apduMeasured);
    else if (apduVal)
        std::snprintf(apdu, sizeof(apdu), "max APDU %u", apduVal);
    else if (o.apduMeasured)
        std::snprintf(apdu, sizeof(apdu), "APDU ~%u (measured)", o.apduMeasured);
    else
        std::snprintf(apdu, sizeof(apdu), "max APDU n/a");
    std::printf("Interface  %s%s%s · %s · %s%s\n", Cb(), o.name[0] ? o.name : "(unnamed)", Cr(),
                knxMediumName(o.medium), apdu, o.famVer[0x05] ? " · ROUTING(!)" : "");
}

// Verbose mode: the full interface steckbrief.
static void printIfacePanel(const std::string& ip, const IfaceDesc& o)
{
    std::printf("\n  %s── KNX Interface · %s ───────────────────────────────%s\n", Cg(), ip.c_str(), Cr());
    if (!o.ok)
    {
        std::printf("     %s(no DESCRIPTION_RESPONSE within 2 s)%s\n\n", Cd(), Cr());
        return;
    }
    char ser[32];
    std::snprintf(ser, sizeof(ser), "%02X%02X:%02X %02X %02X %02X", o.serial[0], o.serial[1], o.serial[2], o.serial[3], o.serial[4], o.serial[5]);
    std::printf("     Friendly name  %s%s%s\n", Cb(), o.name[0] ? o.name : "(unnamed)", Cr());
    std::printf("     KNX medium     %s\n", knxMediumName(o.medium));
    std::printf("     Individual IA  %u.%u.%u\n", (o.ia >> 12) & 0x0F, (o.ia >> 8) & 0x0F, o.ia & 0xFF);
    if (o.hasExt) std::printf("     Mask / descr.  0x%04X\n", o.mask);
    std::printf("     Serial         %s\n", ser);
    std::printf("     MAC            %02X:%02X:%02X:%02X:%02X:%02X\n", o.mac[0], o.mac[1], o.mac[2], o.mac[3], o.mac[4], o.mac[5]);
    std::printf("     Prog mode      %s\n", (o.status & 0x01) ? "ON" : "off");
    if (o.hasIp)
        std::printf("     IP / method    %u.%u.%u.%u / %s\n", (o.ip >> 24) & 0xFF, (o.ip >> 16) & 0xFF, (o.ip >> 8) & 0xFF, o.ip & 0xFF, ipMethodName(o.ipMethod));
    std::printf("     Service fam.   %s\n", famList(o).c_str());
    std::printf("     Routing        %s\n", o.famVer[0x05] ? "advertised (!) -> acts as a router" : "-- not advertised (spec-conform interface)");
    // APDU auto-detection: "reported" via Device-Management M_PropRead (zero bus, spec-exact) and, when the
    // mem-probe ran, "measured" via A_Memory_Read (what actually round-trips). DESCRIPTION's Extended DIB
    // value is shown too when present (v2).
    if (o.hasExt && o.maxLocalApdu)
        std::printf("     APDU (DIB)     %u B   (Extended Device Info)\n", o.maxLocalApdu);
    if (o.apduReported)
        std::printf("     %sAPDU reported  %u B%s   (via device-mgmt PID %u)%s · no bus traffic%s\n",
                    Cb(), o.apduReported, Cr(), o.apduReportedPid, Cd(), Cr());
    else if (!o.hasExt)
        std::printf("     APDU reported  n/a   (interface did not answer device-mgmt)\n");
    if (o.apduMeasured)
        std::printf("     %sAPDU measured  %u B%s   (A_Memory_Read probe)\n", Cb(), o.apduMeasured, Cr());
    std::printf("\n");
}

static void usage()
{
    banner();
    std::printf("%sUSAGE%s\n", Cy(), Cr());
    std::printf("  ftc [%s--verbose%s] [%s--ip%s A.B.C.D] [%s--port%s N] <pa> <cmd> [args...]\n", Cc(), Cr(), Cc(), Cr(), Cc(), Cr());
    std::printf("  ftc %s--discover%s          find KNXnet/IP interfaces on the LAN\n", Cc(), Cr());
    std::printf("  %s--verbose | -V%s      print the full interface + target steckbrief before the command\n", Cc(), Cr());
    std::printf("  %s--quiet | -q%s        suppress ALL chrome (tunnel/identity/console hints) -- scriptable output only\n", Cc(), Cr());
    std::printf("  ftc %s--version%s | %s--help%s\n\n", Cc(), Cr(), Cc(), Cr());

    std::printf("%sOPTIONS%s\n", Cy(), Cr());
    cmdRow("--ip A.B.C.D", "KNXnet/IP interface / router to tunnel through");
    cmdRow("--port N", "KNXnet/IP port (default 3671)");
    cmdRow("--discover", "multicast SEARCH_REQUEST; list interfaces and exit");
    std::printf("\n");

    std::printf("%sINFO  %s(read-only)%s\n", Cy(), Cd(), Cr());
    cmdRow("<pa> ping", "is the target there? (module-version round trip)");
    cmdRow("<pa> info [ga|<file>]", "device fingerprint / group comm / file info");
    cmdRow("<pa> df", "target filesystem usage");
    cmdRow("<pa> ll [dir] | ls [dir]", "list a directory (with / without CRC + bar)");
    cmdRow("scan <a.l | a b> [deep N]", "discover devices on a line / range");
    std::printf("\n");

    std::printf("%sTRANSFER & FILES%s\n", Cy(), Cr());
    cmdRow("<pa> send <src> [opts]", "upload a host file -> remote /<basename>   (alias: upload)");
    cmdRow("<pa> get <remote> [local]", "download a target file to the host   (alias: download / receive)");
    cmdRow("<pa> fwupdate <remote>", "apply an already-uploaded firmware -> reboots the target");
    cmdRow("<pa> perf [kb] [pkg] [mode]", "throughput test (RAM pattern)");
    cmdRow("<pa> rm | mkdir | rmdir | mv", "delete / create / remove / rename");
    cmdRow("<pa> format yes", "erase the WHOLE target filesystem (gated)");
    std::printf("\n");

    std::printf("%sTRANSFER OPTIONS  %s(send/upload; order-independent)%s\n", Cy(), Cd(), Cr());
    cmdRow("pkg = <n> | auto", "APDU payload size; auto (default) = detected interface max");
    cmdRow("mode = safe | fast | forget", "safe = CRC per chunk . fast = windowed (win) . forget = blind (faf)");
    cmdRow("apply | on | yes", "also flash + reboot the target after upload (default: off)");
    cmdRow("no-resume | nr | fresh", "ignore a resumable partial; start the upload from zero");
    cmdRow("verbose | v", "print a 1 Hz progress line during the transfer");
    std::printf("\n");

    std::printf("%sDEVICE%s\n", Cy(), Cr());
    cmdRow("<pa> led [on | off | blink]", "drive the target's prog-mode LED (locate a device)");
    std::printf("\n");

    std::printf("%sCONSOLE%s\n", Cy(), Cr());
    cmdRow("<pa> console | con [N|max]", "interactive remote console; N=drain cap for small interfaces, max=full");
    std::printf("\n");

#ifdef OPENKNX_FTC_SECURITY
    std::printf("%sACCESS (password-protected targets)%s\n", Cy(), Cr());
    cmdRow("<pa> login <pw>", "unlock write actions (password -> MAC locally, never on the wire)");
    cmdRow("<pa> logout", "lock the target's write actions again now");
    std::printf("\n");
#endif

    std::printf("%sEXAMPLES%s\n", Cy(), Cr());
    std::printf("  %sftc --discover%s\n", Cd(), Cr());
    std::printf("  %sftc --ip 11.11.0.126 5.0.3 info%s\n", Cd(), Cr());
    std::printf("  %sftc --ip 11.11.0.126 5.0.3 send fw.bin.gz 254 fast%s\n", Cd(), Cr());
    std::printf("  %sftc --ip 11.11.0.126 5.0.3 send ../build/firmware.bin.gz apply%s   (upload + flash)\n", Cd(), Cr());
    std::printf("  %sftc --ip 11.11.0.126 5.0.3 fwupdate /firmware.bin.gz%s   (flash an uploaded file)\n", Cd(), Cr());
    std::printf("  %sftc --ip 11.11.0.126 5.0.3 get /firmware.bin.gz ./fw.bin.gz%s\n", Cd(), Cr());
    std::printf("  %sftc --ip 11.11.0.126 5.0.3 led blink%s   (locate: flash the target's prog LED)\n", Cd(), Cr());
    std::printf("  %sftc --ip 11.11.0.5   5.0.3 console%s   (small interface? try 'con' default, or 'con max')\n", Cd(), Cr());
#ifdef OPENKNX_FTC_SECURITY
    std::printf("  %sftc --ip 11.11.0.126 5.0.3 login geheim%s\n", Cd(), Cr());
#endif
    std::printf("\n");
}

// Deliver one typed line to the client's console line-sink during an active console session.
// ASSUMED shim helper (see file header + report). No-op if the shim leaves the sink unset.
static void feedConsoleLine(const std::string& line)
{
    openknx.console.feedLine(line.c_str());
}

// Invariant guard: a `login`/`logout` line typed INSIDE a remote console would be relayed as plaintext over
// the tunnel (obj 160) before the target could act -- leaking the password. We never relay such a line; the
// user runs login as a separate one-shot invocation, where the password is turned into a MAC locally. Match
// any whitespace-delimited "login"/"logout" token, case-insensitive.
static bool lineHasAuthKeyword(const std::string& line)
{
    std::string tok;
    for (size_t i = 0; i <= line.size(); i++)
    {
        char ch = (i < line.size()) ? line[i] : ' ';
        if (ch == ' ' || ch == '\t')
        {
            if (tok == "login" || tok == "logout") return true;
            tok.clear();
        }
        else
            tok += (char)std::tolower((unsigned char)ch);
    }
    return false;
}

// Ctrl+C / SIGTERM: request a GRACEFUL abort. The run loops see this, send an FTC Cancel to the target
// (which closes the half-written file + tears down the CO connection) and then let the tunnel disconnect
// cleanly -- so the interface frees the channel at once instead of blocking until its connection timeout.
// The handler also re-arms the default disposition, so a SECOND Ctrl+C hard-kills as an escape hatch.
static volatile std::sig_atomic_t g_abort = 0;
static void onAbortSignal(int) { g_abort = 1; std::signal(SIGINT, SIG_DFL); }

// Common cleanup when an abort was requested mid-run: Cancel the transfer and let it + the CO teardown reach
// the wire before the caller disconnects the tunnel. Returns 130 (128 + SIGINT), the conventional code.
static int abortCleanly(const char* what)
{
    std::fprintf(stderr, "\n[abort] %s -- cancelling + closing cleanly (Ctrl+C again to force)...\n", what);
    openknxFileTransferClient.requestCancel();
    const uint64_t until = nowMs() + 1500;
    while (nowMs() < until)
    {
        g_knxTunnel.pump();
        openknxFileTransferClient.loop(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return 130;
}

// Drive the cooperative loop until the client reaches a terminal phase, or until it goes QUIET: read-chain
// commands (info/df/scan) run on their own sub-state and never set isBusy() or a terminal phase, so we detect
// "finished" as "no TX frame, no phase/progress change, not busy for QUIET_MS". Anything still working keeps
// the timer alive, so nothing is cut off mid-flight. Returns 0 (done) / 1 (failed) / 2 (overall timeout).
// Shared by the one-shot command path and the --verbose target-info probe.
static int runOneShotToQuiescence()
{
    const uint64_t QUIET_MS = 1500;
    const uint64_t ABS_CAP_MS = 1800000; // 30 min
    const uint64_t t0 = nowMs();
    uint64_t lastActivity = t0;
    uint32_t lastTx = knxTunnelActivity();
    FtcPhase lastPhase = openknxFileTransferClient.status().phase;
    uint32_t lastDone = openknxFileTransferClient.status().done;
    for (;;)
    {
        if (g_abort) return abortCleanly("user abort (Ctrl+C)");
        g_knxTunnel.pump();
        openknxFileTransferClient.loop(true);
        const FtcStatus& st = openknxFileTransferClient.status();
        if (st.phase == FtcPhase::Done || st.phase == FtcPhase::Failed)
        {
            const int rc = (st.phase == FtcPhase::Failed) ? 1 : 0;
            // The final report is queued into the cooperative ftcOut buffer in the SAME loop() pass that sets
            // Done -- drained only on the NEXT. Give loop() a few more passes to flush it before we return.
            for (int i = 0; i < 64; ++i)
            {
                g_knxTunnel.pump();
                openknxFileTransferClient.loop(true);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return rc;
        }
        const uint64_t now = nowMs();
        const uint32_t tx = knxTunnelActivity();
        if (tx != lastTx || st.phase != lastPhase || st.done != lastDone || openknxFileTransferClient.isBusy())
        {
            lastTx = tx;
            lastPhase = st.phase;
            lastDone = st.done;
            lastActivity = now;
        }
        if (now - lastActivity > QUIET_MS) return 0; // gone quiet -> finished
        if (now - t0 > ABS_CAP_MS)
        {
            std::fprintf(stderr, "error: overall timeout — aborting.\n");
            openknxFileTransferClient.requestCancel();
            return 2;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

int main(int argc, char** argv)
{
    if (!socketStartup())
    {
        std::fprintf(stderr, "fatal: socket init failed\n");
        return 1;
    }
    g_color = initTerminal(); // UTF-8 + ANSI, TTY-aware, NO_COLOR-honoring
    std::signal(SIGINT, onAbortSignal);  // Ctrl+C -> graceful cancel (portable: Win CRT maps console Ctrl+C to SIGINT)
    std::signal(SIGTERM, onAbortSignal); // kill/terminate -> same graceful path

    std::string ip;
    uint16_t port = 3671;
    bool discover = false;
    bool verbose = false; // --verbose: read + print the FULL interface/target steckbrief before the command
    bool quiet = false;   // --quiet: suppress ALL chrome (tunnel-up, iface identity, console hints) for scripting
    std::vector<std::string> pos; // positional tokens -> the `ftc ...` command tail

    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--help" || a == "-h")
        {
            usage();
            socketCleanup();
            return 0;
        }
        else if (a == "--version" || a == "-v")
        {
            printVersion();
            socketCleanup();
            return 0;
        }
        else if (a == "--discover")
            discover = true;
        else if (a == "--verbose" || a == "-V")
            verbose = true;
        else if (a == "--quiet" || a == "-q")
            quiet = true;
        else if (a == "--ip" && i + 1 < argc)
            ip = argv[++i];
        else if (a == "--port" && i + 1 < argc)
            port = (uint16_t)std::atoi(argv[++i]);
        else if (!a.empty() && a[0] == '-' && !(a.size() > 1 && (isdigit((unsigned char)a[1]))))
        {
            std::fprintf(stderr, "unknown option: %s\n", a.c_str());
            socketCleanup();
            return 1;
        }
        else
            pos.push_back(a);
    }

    // --- discovery: no tunnel needed --------------------------------------------------------------
    if (discover)
    {
        int n = knxDiscover(port);
        socketCleanup();
        return n > 0 ? 0 : 1;
    }

    if (pos.empty())
    {
        usage();
        socketCleanup();
        return 1;
    }
    if (ip.empty())
    {
        std::fprintf(stderr, "error: --ip is required (or use --discover). See --help.\n");
        socketCleanup();
        return 1;
    }

    // --quiet is the dominant verbosity: it forces the full steckbrief off too, so a script gets nothing but
    // the command's own output (and errors on stderr). --verbose + --quiet together -> quiet wins.
    if (quiet) verbose = false;

    // --- open the tunnel --------------------------------------------------------------------------
    if (!g_knxTunnel.connect(ip, port))
    {
        std::fprintf(stderr, "error: could not open KNXnet/IP tunnel to %s:%u\n", ip.c_str(), (unsigned)port);
        socketCleanup();
        return 1;
    }
    if (!quiet)
        std::printf("Tunnel up to %s:%u (assigned PA %u.%u.%u)\n", ip.c_str(), (unsigned)port,
                    (g_knxTunnel.assignedPA() >> 12) & 0x0F, (g_knxTunnel.assignedPA() >> 8) & 0x0F,
                    g_knxTunnel.assignedPA() & 0xFF);

    // Interface steckbrief (DESCRIPTION_REQUEST on a side socket; independent of the tunnel). Default = a
    // one-line identity so you always see WHICH interface you are on; --verbose = the full property panel.
    uint16_t ifaceApdu = 0; // detected interface max APDU -> drives auto-framing (console cap, upload pkg start)
    {
        IfaceDesc idesc;
        queryInterface(ip, port, idesc);
        // APDU auto-detection (primary): read the interface's max APDU over Device Management -- zero KNX-bus
        // traffic, spec-exact, works on v1 interfaces where DESCRIPTION carries no Extended Device Info DIB.
        idesc.apduReported = queryMaxApduDeviceMgmt(ip, port, &idesc.apduReportedPid);
        ifaceApdu = idesc.apduReported ? idesc.apduReported : idesc.maxLocalApdu;
        // --quiet keeps the query above (it drives the APDU auto-framing) but prints no identity at all.
        if (quiet) { /* no output */ }
        else if (verbose) printIfacePanel(ip, idesc);
        else printIfaceLine(idesc);
    }

    // Register the built-in (default LittleFS-shim -> host filesystem) backend once. The shim maps the
    // default backend onto the host FS, so `send <hostpath>` / `get <remote> <hostpath>` work as-is —
    // no explicit FtcFileSource/FtcFileSink needed (kept minimal, per the task).
    openknxFileTransferClient.setup(true);

    // Feed the detected interface max APDU to the client -> auto-framing for every sized operation (upload
    // pkg start, download chunk) begins at the right frame instead of degrading down from the max.
    openknxFileTransferClient.setApduHint(ifaceApdu);

    // --verbose: read + print the full TARGET steckbrief before the actual command, reusing the device-info
    // discovery chain (mask/class · manufacturer · order · hw · version · prog mode · app program · table
    // load states · File-Transfer features). pos[0] is the target PA; all small property reads over the
    // tunnel -> fits any interface. Runs to quiescence, then the real command proceeds normally.
    if (verbose && !pos.empty())
    {
        std::printf("\n  %s── KNX Device · %s via %s ──────────────────────────%s\n", Cg(), pos[0].c_str(), ip.c_str(), Cr());
        openknxFileTransferClient.processCommand(std::string("ftc ") + pos[0] + " info", false);
        runOneShotToQuiescence();
    }

    // Build the command exactly as the on-device console receives it ("ftc <pa> <cmd> ...").
    std::string cmd = "ftc";
    for (const auto& t : pos)
    {
        cmd += ' ';
        cmd += t;
    }

    // `console` mode is interactive; everything else is one-shot. Detect it: "<pa> console|con".
    bool consoleMode = pos.size() >= 2 && (pos[1] == "console" || pos[1] == "con");

    // Drain-cap default for the console. A KNXnet/IP interface only relays a bus frame that fits its max
    // APDU; a constrained interface (e.g. Siemens/MDT, max APDU 15) drops the target's oversized (extended)
    // drain answer entirely. So a plain `con` (no size) defaults to a STANDARD-frame-safe cap -> the console
    // works through such an interface out of the box. `con max` = full window (fast on OpenKNX interfaces
    // that carry extended frames); `con <N>` = an explicit cap. Mirrors requestConsole()'s range check.
    if (consoleMode)
    {
        // auto (default): derive the drain cap from the interface's detected max APDU (device-mgmt), so the
        // console runs as fast as the link carries with NO static guess and NO slow probing. cap = APDU - 7
        // (the FunctionPropertyState_Response header), clamped to [4, 246]; a >=254-APDU link -> full window.
        // `con max` = full window; `con <N>` = fixed cap; `con auto`/no arg = the derived value. When
        // detection failed (APDU unknown), fall back to a standard-frame-safe 8.
        unsigned autoCap = 8; // fallback when the interface did not report an APDU
        if (ifaceApdu >= 254) autoCap = 247;
        else if (ifaceApdu >= 11) { int c = (int)ifaceApdu - 7; autoCap = (c > 246) ? 246u : (unsigned)c; }

        const std::string arg = pos.size() >= 3 ? pos[2] : std::string();
        unsigned eff;
        const char* how;
        if (arg == "max") { eff = 247; how = "full window (fixed)"; }
        else if (!arg.empty() && arg != "auto")
        {
            unsigned v = 0;
            eff = (std::sscanf(arg.c_str(), "%u", &v) == 1 && v >= 4 && v < 247) ? v : 247;
            how = (eff >= 247) ? "full window (fixed)" : "fixed";
        }
        else { eff = autoCap; how = ifaceApdu ? "auto (from detected interface APDU)" : "auto (fallback; APDU unknown)"; }

        // Rebuild the command so the shared parser reads the resolved numeric cap (247 -> full window).
        cmd = std::string("ftc ") + pos[0] + ' ' + pos[1] + ' ' + std::to_string(eff);
        if (!quiet) std::printf("[console] drain cap %u B/answer -- %s\n", eff, how);
    }

    // Submit the command through the same Module entry the console uses.
    openknxFileTransferClient.processCommand(cmd, false);

    int exitCode = 0;

    if (consoleMode)
    {
        // Interactive remote console. processCommand opened the session (probe -> obj 160). Typed lines
        // go to the client's console line path; the shim's log/line-sink streams remote output to stdout.
        StdinLines stdinLines;
        if (!quiet) std::printf("[console] connected — type commands; 'quit' or 'exit' to leave.\n");
        std::fflush(stdout);
        bool running = true;
        while (running)
        {
            if (g_abort) // Ctrl+C in an interactive console: send a clean CLOSE, then leave
            {
                feedConsoleLine("quit"); // -> conClose(sendClose=true): releases the target's console session
                for (int i = 0; i < 64; ++i)
                {
                    g_knxTunnel.pump();
                    openknxFileTransferClient.loop(true);
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                exitCode = 130;
                running = false;
                break;
            }
            g_knxTunnel.pump();
            openknxFileTransferClient.loop(true);

            std::string line;
            if (stdinLines.poll(line))
            {
                if (lineHasAuthKeyword(line))
                {
                    // Never relay a login/logout line (would send the password in clear over the tunnel).
                    std::printf("[console] 'login'/'logout' is not relayed here (it would send the password in\n"
                                "          clear). Leave the console and run it as a one-shot:  ftc <pa> login <pw>\n");
                    std::fflush(stdout);
                }
                else
                {
                    feedConsoleLine(line); // client catches quit/exit locally and closes the session
                    if (line == "quit" || line == "exit") running = false;
                }
            }

            // The session ending on the wire (reboot, remote close, cancel, remote busy) lands the client in
            // a terminal phase. The close banner (e.g. "remote busy") is queued into the cooperative ftcOut
            // buffer in the SAME loop() pass that sets the phase and is only drained on the NEXT -> give
            // loop() a few passes to flush it (same as the one-shot path), set the exit code, then leave.
            FtcPhase ph = openknxFileTransferClient.status().phase;
            if (ph == FtcPhase::Done || ph == FtcPhase::Failed)
            {
                exitCode = (ph == FtcPhase::Failed) ? 1 : 0;
                for (int i = 0; i < 64; ++i)
                {
                    g_knxTunnel.pump();
                    openknxFileTransferClient.loop(true);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                running = false;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    else
    {
        // One-shot (upload/get/info/df/scan/…): run the cooperative loop to completion (see the helper).
        exitCode = runOneShotToQuiescence();
    }

    // --- clean shutdown ---------------------------------------------------------------------------
    g_knxTunnel.disconnect();
    if (exitCode == 130)
        std::fprintf(stderr, "Aborted by user (Ctrl+C) -- transfer cancelled, tunnel closed cleanly (no interface lockout).\n");
    socketCleanup();
    return exitCode;
}

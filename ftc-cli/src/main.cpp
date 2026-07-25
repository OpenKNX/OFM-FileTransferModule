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

static void usage()
{
    banner();
    std::printf("%sUSAGE%s\n", Cy(), Cr());
    std::printf("  ftc [%s--ip%s A.B.C.D] [%s--port%s N] <pa> <cmd> [args...]\n", Cc(), Cr(), Cc(), Cr());
    std::printf("  ftc %s--discover%s          find KNXnet/IP interfaces on the LAN\n", Cc(), Cr());
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
    cmdRow("<pa> send <src> [pkg] [mode]", "upload a host file (auto-resume)");
    cmdRow("<pa> get <remote> [local]", "download a target file to the host");
    cmdRow("<pa> perf [kb] [pkg] [mode]", "throughput test (RAM pattern)");
    cmdRow("<pa> rm | mkdir | rmdir | mv", "delete / create / remove / rename");
    cmdRow("<pa> format yes", "erase the WHOLE target filesystem (gated)");
    std::printf("\n");

    std::printf("%sCONSOLE%s\n", Cy(), Cr());
    cmdRow("<pa> console | con", "interactive remote console ('quit' to leave)");
    std::printf("\n");

    std::printf("%sEXAMPLES%s\n", Cy(), Cr());
    std::printf("  %sftc --discover%s\n", Cd(), Cr());
    std::printf("  %sftc --ip 11.11.0.126 5.0.3 info%s\n", Cd(), Cr());
    std::printf("  %sftc --ip 11.11.0.126 5.0.3 send fw.bin.gz 253 fast%s\n", Cd(), Cr());
    std::printf("  %sftc --ip 11.11.0.126 5.0.3 console%s\n", Cd(), Cr());
    std::printf("\n");
}

// Deliver one typed line to the client's console line-sink during an active console session.
// ASSUMED shim helper (see file header + report). No-op if the shim leaves the sink unset.
static void feedConsoleLine(const std::string& line)
{
    openknx.console.feedLine(line.c_str());
}

int main(int argc, char** argv)
{
    if (!socketStartup())
    {
        std::fprintf(stderr, "fatal: socket init failed\n");
        return 1;
    }
    g_color = initTerminal(); // UTF-8 + ANSI, TTY-aware, NO_COLOR-honoring

    std::string ip;
    uint16_t port = 3671;
    bool discover = false;
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

    // --- open the tunnel --------------------------------------------------------------------------
    if (!g_knxTunnel.connect(ip, port))
    {
        std::fprintf(stderr, "error: could not open KNXnet/IP tunnel to %s:%u\n", ip.c_str(), (unsigned)port);
        socketCleanup();
        return 1;
    }
    std::printf("Tunnel up to %s:%u (assigned PA %u.%u.%u)\n", ip.c_str(), (unsigned)port,
                (g_knxTunnel.assignedPA() >> 12) & 0x0F, (g_knxTunnel.assignedPA() >> 8) & 0x0F,
                g_knxTunnel.assignedPA() & 0xFF);

    // Register the built-in (default LittleFS-shim -> host filesystem) backend once. The shim maps the
    // default backend onto the host FS, so `send <hostpath>` / `get <remote> <hostpath>` work as-is —
    // no explicit FtcFileSource/FtcFileSink needed (kept minimal, per the task).
    openknxFileTransferClient.setup(true);

    // Build the command exactly as the on-device console receives it ("ftc <pa> <cmd> ...").
    std::string cmd = "ftc";
    for (const auto& t : pos)
    {
        cmd += ' ';
        cmd += t;
    }

    // `console` mode is interactive; everything else is one-shot. Detect it: "<pa> console|con".
    bool consoleMode = pos.size() >= 2 && (pos[1] == "console" || pos[1] == "con");

    // Submit the command through the same Module entry the console uses.
    openknxFileTransferClient.processCommand(cmd, false);

    int exitCode = 0;

    if (consoleMode)
    {
        // Interactive remote console. processCommand opened the session (probe -> obj 160). Typed lines
        // go to the client's console line path; the shim's log/line-sink streams remote output to stdout.
        StdinLines stdinLines;
        std::printf("[console] connected — type commands; 'quit' or 'exit' to leave.\n");
        std::fflush(stdout);
        bool running = true;
        while (running)
        {
            g_knxTunnel.pump();
            openknxFileTransferClient.loop(true);

            std::string line;
            if (stdinLines.poll(line))
            {
                feedConsoleLine(line); // client catches quit/exit locally and closes the session
                if (line == "quit" || line == "exit") running = false;
            }

            // The session ending on the wire (reboot, remote close, cancel) lands the client in a
            // terminal phase -> leave too.
            FtcPhase ph = openknxFileTransferClient.status().phase;
            if (ph == FtcPhase::Done || ph == FtcPhase::Failed) running = false;

            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    else
    {
        // One-shot. Drive the cooperative loop until the client reaches a terminal phase, or until it goes
        // QUIET: read-chain commands (info/df/scan) run on their own sub-state and never set isBusy() or a
        // terminal phase, so we detect "finished" as "no TX frame, no phase/progress change, not busy for
        // QUIET_MS". Anything still working (an upload with per-chunk progress, a chain mid-read) keeps the
        // timer alive, so nothing is cut off mid-flight. A synchronous command (help/status) is quiet at
        // once and exits after one QUIET_MS. Absolute cap backstops a genuine hang.
        const uint64_t QUIET_MS = 1500;
        const uint64_t ABS_CAP_MS = 1800000; // 30 min

        const uint64_t t0 = nowMs();
        uint64_t lastActivity = t0;
        uint32_t lastTx = knxTunnelActivity();
        FtcPhase lastPhase = openknxFileTransferClient.status().phase;
        uint32_t lastDone = openknxFileTransferClient.status().done;

        for (;;)
        {
            g_knxTunnel.pump();
            openknxFileTransferClient.loop(true);

            const FtcStatus& st = openknxFileTransferClient.status();
            if (st.phase == FtcPhase::Done || st.phase == FtcPhase::Failed)
            {
                exitCode = (st.phase == FtcPhase::Failed) ? 1 : 0;
                // A command queues its final report into the cooperative ftcOut buffer in the SAME loop()
                // pass that sets Done — drained only on the NEXT pass. Give loop() a few more passes to
                // flush it before we leave, or df/info/etc. lose their summary block.
                for (int i = 0; i < 64; ++i)
                {
                    g_knxTunnel.pump();
                    openknxFileTransferClient.loop(true);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                break;
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
            if (now - lastActivity > QUIET_MS) break; // gone quiet -> finished
            if (now - t0 > ABS_CAP_MS)
            {
                std::fprintf(stderr, "error: overall timeout — aborting.\n");
                openknxFileTransferClient.requestCancel();
                exitCode = 2;
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    // --- clean shutdown ---------------------------------------------------------------------------
    g_knxTunnel.disconnect();
    socketCleanup();
    return exitCode;
}

/**
 * @file        main.cpp
 * @brief       The oknx entry point — CLI parsing, command dispatch and the cooperative run loops.
 * @details     Drives the UNCHANGED embedded FileTransferClient over a KNXnet/IP tunnel (g_knxTunnel):
 *              parses the CLI, opens the tunnel (or runs discovery), submits ONE ftc command through the
 *              same Module entry the on-device console uses (processCommand), then pumps the device's
 *              cooperative loop to completion. `console` gets an interactive, non-blocking-stdin loop.
 * @date        2026-08-03
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/

#include <cctype>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <mutex>
#include <map>
#include <memory>
#include <atomic>
#include <algorithm>
#include <vector>
#include <set>

// --- cross-platform sockets (discovery only; the tunnel owns its own socket) --------------------
#ifdef _WIN32
    #include <winsock2.h> // MUST precede windows.h, else windows.h pulls in the legacy winsock.h (-> the ordering warning)
    #include <ws2tcpip.h>
    #include <windows.h>
    #include <conio.h>
    #include <io.h>
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
#include "KnxDeviceMap.h"
#include "knx_ip_tunnel.h"

// oknx CLI presentation layer (host-only, header-only, NO lib/knx dependency): terminal caps, i18n,
// the phosphor theme registry, and the reusable Ui building blocks. main() stays thin — Ui draws.
#include "cli/Config.h"
#include "cli/Compare.h"
#include "cli/ConsoleUi.h"
#include "cli/I18n.h"
#include "cli/KnxOta.h"
#include "cli/OtaSession.h"
#include "cli/Prompt.h"
#include "core/Access.h"
#include "core/FastScan.h"
#include "core/Reach.h"
#include "core/ScanDetails.h"
#include "cli/Monitor.h"
#include "cli/ProgScan.h"
#include "cli/Templates.h"
#include "cli/Term.h"
#include "cli/Theme.h"
#include "cli/Ui.h"
#include "cli/Watch.h"
#include "cli/WebConsole.h"
#include "core/Delta.h"
#include "core/DeltaProbe.h"
#include "cli/Browser.h"
#include "core/DeltaBase.h"
#include "core/Describe.h"
#include "core/GaImport.h"
#include "core/GaTable.h"
#include "core/DeviceMgmt.h"
#include "core/Discovery.h"
#include "core/Gzip.h"
#include "core/SelfInstall.h"
static ftc::Term g_term;
static ftc::I18n g_i18n;
static ftc::Theme g_theme(g_term);
static ftc::Ui g_ui(g_term, g_theme, g_i18n);
static ftc::Tpl g_tpl(g_term, g_theme, &g_i18n);
static ftc::Watch g_watch(g_theme, g_i18n); // auto/recurring console commands (/every), cached per target PA
static ftc::Config g_cfg;

// `busprop` answer sink: the tunnel property callback is a plain function pointer, so the one pending
// request is matched here against its own PA / object index / PID and everything else is ignored.
static volatile bool g_bpSeen = false;
static uint16_t g_bpPa = 0;
static uint8_t g_bpObj = 0, g_bpPid = 0, g_bpLen = 0;
static uint8_t g_bpData[64] = {0};

static void busPropAnswer(uint16_t pa, uint8_t objectIndex, uint8_t propertyId, const uint8_t* data, uint8_t length)
{
    if (pa != g_bpPa || objectIndex != g_bpObj || propertyId != g_bpPid) return;
    g_bpLen = (uint8_t)(length > sizeof(g_bpData) ? sizeof(g_bpData) : length);
    if (data != nullptr && g_bpLen > 0) std::memcpy(g_bpData, data, g_bpLen);
    g_bpSeen = true;
}

// ── oknx throughput-graph width, in % of the terminal width (100 = full). ──
// Live shows a sliding window of the last <width> samples (older ones scroll off
// the left); the end recap shows the whole run downsampled to <width>. Tune here.
static const uint8_t GRAPH_WIDTH_PCT = 100;

static int graphWidth(int reserve) // graph columns = GRAPH_WIDTH_PCT of (cols - reserve), clamped to the terminal
{
    int avail = ftc::Tpl::cols() - reserve;
    if (avail < 8) avail = 8;
    int w = (int)((long)avail * (long)GRAPH_WIDTH_PCT / 100);
    if (w > avail) w = avail; // clamp: never wider than the terminal (>100% not allowed)
    if (w < 8) w = 8;
    return w;
}
// The sweep is serial on one tunnel; only the identity read runs in parallel, as child processes.
static std::string g_selfPath;   // argv[0] -> re-invoked for the identity children
static std::string g_ip;         // interface address, as given -> the detail children reach the same one
static uint16_t g_port = 3671;
static uint16_t g_ifaceApdu = 0;  // the interface's own max APDU -- shown beside the target's
// Enough tunnels to hide the per-device read behind the sweep, few enough that an interface still grants
// them all; `--workers N` overrides. A reader whose tunnel is refused just drops out.
static constexpr int FTC_DETAIL_WORKERS = 5;
// How many identity readers run in parallel for `details`/`openknx` (each takes one tunnel of the
// interface). 0 = the built-in default. This is the ONLY place parallelism measurably pays: ~20 found
// devices instead of 255 addresses, and the bus is not the limit there.
static int g_workers = 0;
static bool g_probeSlot = false; // internal: this process is a tunnel-slot probe child (connect, emit SLOT, hold, exit)

// Probe which tunnel additional-addresses are currently free: spawn one short-lived tunnel per slot (children
// hold the slot briefly so the interface hands each a distinct free PA); the PAs that come back are free.
struct TunnelProbe
{
    std::set<uint16_t> freeSet; ///< addresses the interface actually handed out
    bool conclusive = true;     ///< false = at least one probe got no answer, so "the rest is busy" is a guess
};
static TunnelProbe probeTunnelSlots(const std::string& ip, uint16_t port, int slots);

/**********************************************************************
 *************************** SMALL HELPERS ****************************
 **********************************************************************/

/**
 * @brief One frame of the bus-flow line: `◉──▪──◉  <label>` with the packet at position `step`. CR-anchored.
 */
static std::string busAnimFrame(int step, const std::string& label)
{
    const std::string node = g_theme.green(g_term.glyph("◉", "O"));
    const int span = 24, p = ((step % span) + span) % span;
    std::string road;
    for (int i = 0; i < span; ++i)
        road += (i == p) ? g_theme.bright(g_term.glyph("▪", "*")) : g_theme.mut(g_term.glyph("─", "-"));
    return std::string("\r  ") + node + road + node + "  " + g_theme.dim(label) + "\x1b[K";
}

/**
 * @brief Run a blocking device read on a worker thread while the OpenKNX bus-flow animates in the foreground.
 * @details The read can't yield, so a naked call looks frozen. This threads it and animates the packet-flow
 *          until it finishes, then wipes the line so the result renders clean. Gate on tty + non-quiet at the
 *          call site (scan draws its own live progress). @p out lets a caller keep stdout clean (e.g. stderr).
 */
template <class Work>
static void runWithBusAnim(const std::string& label, Work work, FILE* out = stdout)
{
    std::atomic<bool> done(false);
    std::thread th([&]() { work(); done.store(true); });
    for (int step = 0; !done.load(); ++step)
    {
        std::fprintf(out, "%s", busAnimFrame(step, label).c_str());
        std::fflush(out);
        std::this_thread::sleep_for(std::chrono::milliseconds(45));
    }
    th.join();
    std::fprintf(out, "\r\x1b[K"); // wipe the animation line; the result follows
    std::fflush(out);
}

/**
 * @brief One-shot socket layer init/teardown (no-op on POSIX).
 */
static bool socketStartup()
{
#ifdef _WIN32
    WSADATA w;
    return WSAStartup(MAKEWORD(2, 2), &w) == 0;
#else
    return true;
#endif
}
static void forgetSessionPasswords(); // defined with the session password store, below

static void socketCleanup()
{
    // Hand the tunnel back, on EVERY way out. A session the interface still believes in occupies one of
    // its few slots for about two minutes, and an interface serving several sessions splits its bus
    // queue between them -- which is what turned a quick answer into "the device did not answer" after
    // a couple of cancelled runs. disconnect() is a no-op when nothing is open.
    g_knxTunnel.disconnect();
#ifdef _WIN32
    WSACleanup();
#endif
    forgetSessionPasswords(); // every exit path goes through here
}

/**
 * @brief Monotonic clock in ms (host CLI may use std::chrono directly; the embedded loop-only rule does not apply here).
 */
static uint64_t nowMs()
{
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

/**********************************************************************
 ************************* KNXNET/IP DISCOVERY **********************
 **********************************************************************/
// Discovery is delegated to the reusable CORE (core/Discovery.h; also used by `oknx mc`).

/**
 * @brief Windows-only hint printed when discovery comes back empty.
 * @details Almost always the Firewall silently dropping the (unicast, from a different address than we
 *          multicast to) SEARCH_RESPONSE. We already send from every NIC, so this covers the firewall cause.
 */
static void discoverEmptyHint(ftc::Theme& c, ftc::I18n& L)
{
#ifdef _WIN32
    char path[512];
    if (!GetModuleFileNameA(nullptr, path, sizeof(path))) std::strcpy(path, "oknx.exe");
    std::printf("  %s\n", c.amber(L.tr("Windows Firewall likely blocked the response.",
                                       "Die Windows-Firewall hat die Antwort vermutlich blockiert."))
                              .c_str());
    std::printf("  %s\n", c.dim(L.tr("Allow oknx through the Firewall, or run once as Administrator:",
                                     "oknx in der Firewall freigeben, oder einmal als Administrator:"))
                              .c_str());
    std::printf("  %s\n", c.cyan(std::string("netsh advfirewall firewall add rule name=\"oknx KNX\" dir=in action=allow protocol=UDP program=\"") + path + "\"").c_str());
#else
    (void)c;
    (void)L;
#endif
}

/**
 * @brief Print every responding interface via the shared core discovery (phosphor + DE/EN).
 * @details Returns the number of interfaces found.
 */
static int knxDiscover(uint16_t port)
{
    ftc::Theme& c = g_theme;
    ftc::I18n& L = g_i18n;
    std::printf("  %s\n", c.dim(L.tr("searching 224.0.23.12:3671 …", "suche 224.0.23.12:3671 …")).c_str());
    const auto ifaces = ftc::discoverInterfaces(port, 3000); // shared CORE discovery (also used by oknx mc)
    if (ifaces.empty())
    {
        std::printf("  %s\n", c.dim(L.tr("(no interfaces responded)", "(keine Interfaces geantwortet)")).c_str());
        discoverEmptyHint(c, L);
        return 0;
    }
    for (const auto& f : ifaces)
        std::printf("  %s %s  %s  %s\n", c.green(g_term.glyph("●", "*")).c_str(), c.txt(f.ip).c_str(),
                    c.bright(f.name).c_str(), c.dim(ftc::knxMediumName(f.medium)).c_str());
    std::printf("  %s\n", c.green(std::to_string(ifaces.size()) + " " + L.tr("interface(s)", "Interface(s)")).c_str());
    return (int)ifaces.size();
}

static void renderUiDemoExtras(ftc::Tpl& t); // the second half of the catalogue (defined below)

/**
 * @brief --ui-demo: render the whole console template catalogue (cli/Templates.h) with sample data.
 * @details Presentation only — lets the look be reviewed and tuned in isolation before real commands migrate onto it.
 */
static void renderUiDemo()
{
    ftc::Theme& c = g_theme;
    ftc::Tpl& t = g_tpl;
    g_ui.banner();
    std::printf("  %s\n", c.dim("console template catalogue · cli/Templates.h · sample data").c_str());

    // T1 — status lines (the dotted headline in all five kinds).
    t.section("«status» · status()  — dotted headline");
    t.status(ftc::Tpl::Stat::Ok, "tunnel up", {"11.11.0.5:3671", "as 5.0.50"});
    t.status(ftc::Tpl::Stat::Info, "IP Interface N148 — ALT", {std::string("TP1 (twisted pair)") + " · max APDU 254"});
    t.status(ftc::Tpl::Stat::Warn, "APDU re-adjusted", {"55 B", "device answered short"});
    t.status(ftc::Tpl::Stat::Err, "connect refused", {"E_NO_MORE_CONNECTIONS", "all 16 tunnels busy"});
    t.status(ftc::Tpl::Stat::Idle, "busmonitor", {"not connected"});

    // T2/T3 — a titled panel with kv rows (the interface steckbrief).
    t.section("«panel» · panelTop() · kv() · panelEnd()  — titled box");
    t.panelTop("KNX Interface", "11.11.0.5");
    t.kv("Friendly name", c.bold("IP Interface N148 — ALT"));
    t.kv("KNX medium", c.txt("TP1 (twisted pair)"));
    t.kv("Individual IA", c.txt("5.0.50"));
    t.kv("Prog mode", c.mut(std::string(g_term.glyph("○", "o")) + " off"));
    t.kv("Service fam.", c.txt("Core v1 · DevMgmt v1 · Tunnelling v1"));
    t.kv("Routing", c.green("— not advertised (spec-conform interface)"));
    t.kv("APDU reported", c.bold("55 B") + c.dim("  · via device-mgmt"));
    t.panelEnd();

    // T3 — a standalone section rule.
    t.section("«section» · section()  — half-open heading (also seen above)");

    // T4 — inline chips (all five hues).
    t.section("«chip» · chip()  — inline badge");
    std::printf("  %s  %s  %s  %s  %s\n", t.chip("OpenKNX").c_str(), t.chip("TUNNEL", 'c').c_str(),
                t.chip("FAST", 'a').c_str(), t.chip("PROG", 'o').c_str(), t.chip("ROUTING (!)", 'r').c_str());

    // T5 — block gauges, incl. the per-transfer-mode fill colours (mock: safe green · fast cyan).
    t.section("«bar» · bar()  — block gauge  (df usage · transfer-mode colours)");
    std::printf("  %s  %s\n", t.bar(0.62).c_str(), c.dim("62 %  · flash 1.24 / 2.00 MB").c_str());
    std::printf("  %s %s  %s\n", t.chip("safe", 'g').c_str(), t.bar(0.62, 20, 'g').c_str(), c.dim("CRC per chunk · reliable").c_str());
    std::printf("  %s %s  %s\n", t.chip("fast", 'c').c_str(), t.bar(0.62, 20, 'c').c_str(), c.dim("windowed · paced").c_str());

    // T6 — a follow-up note / hint.
    t.section("«note» · note()  — follow-up hint");
    t.status(ftc::Tpl::Stat::Warn, "no devices answered", {});
    t.note("check the line/area filter, or widen with `scan deep 5`");

    // T7 — an aligned table (a directory listing).
    t.section("«table» · tableRow()  — aligned columns");
    t.tableRow({c.dim("MODE"), c.dim("SIZE"), c.dim("CRC"), c.dim("NAME")}, {5, -10, 10, 0});
    t.tableRow({c.cyan("dir"), c.dim("—"), c.dim("—"), c.txt("logs/")}, {5, -10, 10, 0});
    t.tableRow({c.txt("file"), c.txt("12'480"), c.mut("a1b2c3d4"), c.txt("config.toml") + "  " + t.chip("OpenKNX")}, {5, -10, 10, 0});
    t.tableRow({c.txt("file"), c.txt("2'048"), c.mut("00ff8821"), c.txt("firmware.bin.gz")}, {5, -10, 10, 0});

    // T8/T9 — animated transfer bar + live rate sparkline (upload then download). The percentage caps at
    // 99,99 % while data flows and only turns 100 % on the confirmed final chunk (see Tpl::progress).
    t.section("«progress» · progress() + spark()  — animated (per mode · bar & graph styles · 99,99 % → 100 %)");
    // barStyle: 'b' block · 'l' line · 'p' pill    sparkStyle: 'b' bars · 'd' dots · 'l' line — one combo per row.
    auto animate = [&](char dir, const std::string& file, double totalMB, const std::string& mode,
                       char barStyle, char sparkStyle) {
        const char col = ftc::Tpl::modeColor(mode);                      // safe=g · fast=c
        const std::string label = t.chip(mode, col) + " " + c.txt(file); // mode chip + filename (caller-coloured)
        std::vector<double> win;                                         // rolling window feeding the live inline sparkline
        std::vector<double> hist;                                        // the FULL run history -> the wide recap graph at the end
        const bool tty = g_term.isTty();
        const int steps = tty ? 48 : 1;
        for (int k = 0; k < steps; ++k)
        {
            const double frac = (double)k / steps; // 0 .. <1 (never reaches 1 in the loop)
            const double doneMB = frac * totalMB;
            const double rate = 380.0 + 220.0 * std::sin(k * 0.45) + (k % 6) * 22.0; // KB/s (pseudo)
            win.push_back(rate);
            hist.push_back(rate);
            if (win.size() > 16) win.erase(win.begin());
            const double etaS = rate > 1 ? (1 - frac) * totalMB * 1024.0 / rate : 0;
            char det[80];
            std::snprintf(det, sizeof(det), "%.2f/%.2f MB · %.0f KB/s · ETA %.0fs", doneMB, totalMB, rate, etaS);
            t.progress(frac, false, dir, label, det, t.spark(win, 0, 0, sparkStyle), col, barStyle);
            if (tty) std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        // The 99,99 % hold: last chunk is on the wire, awaiting the CRC/confirm. Bar is full, % stays 99,99.
        char det[80];
        std::snprintf(det, sizeof(det), "%.2f/%.2f MB · %s", totalMB, totalMB, "finalising · CRC check");
        t.progress(0.9999, false, dir, label, det, t.spark(win, 0, 0, sparkStyle), col, barStyle);
        if (tty) std::this_thread::sleep_for(std::chrono::milliseconds(650));
        // Confirmed -> 100 %.
        char okd[48];
        std::snprintf(okd, sizeof(okd), "%.2f MB · %s", totalMB, "verified");
        t.progress(1.0, true, dir, label, okd, "", col, barStyle);
        // Recap: the whole run's throughput as a wide history graph (same spark style as the live line).
        t.graph("throughput", hist, 48, "KB/s", sparkStyle);
    };
    animate('^', "config.toml", 0.30, "safe", 'b', 'b');     // block bar · bars graph
    animate('v', "firmware.bin.gz", 2.00, "fast", 'l', 'd'); // line bar  · dots graph

    // T10 — the driveable Info-LED simulation (busmon RX blink + active-tunnel count), animated on a tty.
    t.section("«led» · led() · ledRow()  — Info-LED simulation  (busmon · tunnel count)");
    const bool tty = g_term.isTty();
    if (tty)
    {
        // busmon: the LED pulses once per received bus frame.
        std::vector<double> spk;
        for (int k = 0; k < 22; ++k)
        {
            const bool on = (k % 2) == 0;
            spk.push_back(on ? 700 : 120);
            if (spk.size() > 16) spk.erase(spk.begin());
            std::printf("\r  %s %s  %s  %s\x1b[K", t.led(on, 'g').c_str(), c.txt("busmon RX").c_str(),
                        c.dim("bus frame → LED pulse").c_str(), t.spark(spk).c_str());
            std::fflush(stdout);
            std::this_thread::sleep_for(std::chrono::milliseconds(85));
        }
        std::printf("\r  %s %s\x1b[K\n", t.led(false, 'g').c_str(), c.dim("busmon idle").c_str());
        // tunnel count: connections light up one by one, then one drops.
        const int total = 5;
        for (int lit = 0; lit <= total; ++lit)
        {
            std::printf("\r  %s  %s\x1b[K", t.ledRow(lit, total, 'c').c_str(),
                        c.dim(std::to_string(lit) + "/" + std::to_string(total) + " tunnels connected").c_str());
            std::fflush(stdout);
            std::this_thread::sleep_for(std::chrono::milliseconds(180));
        }
        std::printf("\n");
    }
    else
    {
        std::printf("  %s %s\n", t.led(true, 'g').c_str(), c.txt("busmon RX (blinks on a tty)").c_str());
        std::printf("  %s  %s\n", t.ledRow(3, 5, 'c').c_str(), c.dim("3/5 tunnels connected").c_str());
    }

    // T11 — the programming-mode indicator INLINE, and the interface identity in every display option. The
    // interface is normally reached at a tunnel-pool PA (e.g. 5.0.50), but once ETS programs it, it also
    // carries its OWN real device PA (e.g. 1.1.5) — both are shown, plus prog-mode (off + a blinking ON).
    t.section("«identity» · progMode() + identity  — inline prog-mode · interface as a real PA (all options)");
    // (a) prog-mode inline, both states (the panel row, reused inline).
    std::printf("  %s %s        %s %s\n", c.dim("Prog mode").c_str(), t.progMode(false).c_str(),
                c.dim("Prog mode").c_str(), t.progMode(true).c_str());
    // (b) the identity as a compact status line — tunnel connection vs. the device's own programmed PA.
    t.status(ftc::Tpl::Stat::Ok, "tunnel up", {"11.11.0.5:3671", "as " + ftc::Tpl::pa(0x500A)});
    std::printf("  %s %s  %s  %s  %s  %s\n", t.statusDot('g').c_str(), c.bold("OpenKNX IP-Interface").c_str(),
                t.chip(ftc::Tpl::pa(0x1105), 'c').c_str(), c.dim("TP1").c_str(),
                (std::string("prog ") + t.progMode(false)).c_str(), t.chip("OpenKNX").c_str());
    // (c) the PA shown every way: plain · chip · with role, tunnel-pool vs own.
    t.tableRow({c.dim("PA (role)"), c.dim("plain"), c.dim("chip"), c.dim("note")}, {14, 10, 12, 0});
    t.tableRow({c.txt("tunnel pool"), c.txt(ftc::Tpl::pa(0x500A)), t.chip(ftc::Tpl::pa(0x500A), 'g'), c.dim("connection address (this session)")}, {14, 10, 12, 0});
    t.tableRow({c.txt("own device"), c.txt(ftc::Tpl::pa(0x1105)), t.chip(ftc::Tpl::pa(0x1105), 'c'), c.dim("programmed by ETS · self-programmable")}, {14, 10, 12, 0});
    // (d) prog-mode ON blinking inline (animated on a tty) — same indicator, driven.
    if (tty)
    {
        for (int k = 0; k < 14; ++k)
        {
            std::printf("\r  %s %s  %s\x1b[K", c.dim("Prog mode").c_str(), t.progMode(true, (k % 2) == 0).c_str(),
                        c.dim("← device in programming mode (button held / ETS)").c_str());
            std::fflush(stdout);
            std::this_thread::sleep_for(std::chrono::milliseconds(220));
        }
        std::printf("\r  %s %s\x1b[K\n", c.dim("Prog mode").c_str(), t.progMode(false).c_str());
    }

    // T12 — an animated PA (bus) scan and its result table with traffic-light status dots: green = answered
    // + info read, amber = answered but no/partial info, red = read error / no response.
    t.section("«scan» · scan + statusDot()  — animated bus scan · result table (● red/amber/green)");
    struct Hit
    {
        uint16_t ia;
        char st;
        const char* name;
        const char* medium;
        bool okx;
    };
    static const Hit hits[] = {
        {0x1100, 'g', "IP-Router N146", "TP1", false},
        {0x1105, 'g', "KNeoPixel", "TP1", true},
        {0x1108, 'g', "OpenKNX Sensor", "TP1", true},
        {0x110C, 'a', "(no device info)", "TP1", false},
        {0x1114, 'r', "(no response)", "—", false},
        {0x1120, 'g', "Switch Actuator", "TP1", false},
    };
    const int nHits = (int)(sizeof(hits) / sizeof(hits[0]));
    if (tty)
    {
        // sweep 1.1.0 .. 1.1.32, revealing hits as the scan passes their address.
        std::vector<double> spk;
        int found = 0;
        for (int addr = 0; addr <= 32; ++addr)
        {
            while (found < nHits && (hits[found].ia & 0xFF) <= addr)
                ++found;
            spk.push_back(found);
            if (spk.size() > 20) spk.erase(spk.begin());
            std::printf("\r  %s %s  %s  %s\x1b[K", c.cyan(std::string(g_term.glyph("⠿", "*"))).c_str(),
                        c.txt("scanning 1.1." + std::to_string(addr)).c_str(),
                        c.dim(std::to_string(found) + " found").c_str(), t.spark(spk).c_str());
            std::fflush(stdout);
            std::this_thread::sleep_for(std::chrono::milliseconds(70));
        }
        std::printf("\r%s\x1b[K", ""); // clear the sweep line; the table follows
    }
    t.status(ftc::Tpl::Stat::Ok, "scan complete", {std::to_string(nHits) + " devices on 1.1", "4 answered · 1 no-info · 1 error"});
    t.tableRow({c.dim("ST"), c.dim("PA"), c.dim("NAME"), c.dim("MEDIUM"), c.dim("INFO")}, {2, 10, 26, 8, 0});
    for (int i = 0; i < nHits; ++i)
    {
        const char* info = hits[i].st == 'g' ? "ok" : (hits[i].st == 'a' ? "no info" : "read error");
        std::string name = hits[i].st == 'r' ? c.mut(hits[i].name) : c.txt(hits[i].name);
        std::string infoCell = hits[i].st == 'g' ? c.green(info) : (hits[i].st == 'a' ? c.amber(info) : c.red(info));
        if (hits[i].okx) name += "  " + t.chip("OpenKNX");
        t.tableRow({t.statusDot(hits[i].st), c.txt(ftc::Tpl::pa(hits[i].ia)), name, c.dim(hits[i].medium), infoCell},
                   {2, 10, 26, 8, 0});
    }

    // T13 — a legend of the various status displays in one place (the dot vocabulary + traffic-light dots).
    t.section("«status-legend» · status vocabulary  — the various status displays at a glance");
    t.status(ftc::Tpl::Stat::Ok, "up / reachable / done", {});
    t.status(ftc::Tpl::Stat::Warn, "degraded / caution", {});
    t.status(ftc::Tpl::Stat::Err, "failed / refused", {});
    t.status(ftc::Tpl::Stat::Info, "neutral notice", {});
    t.status(ftc::Tpl::Stat::Idle, "off / absent / not probed", {});
    std::printf("  %s %s   %s %s   %s %s\n", t.statusDot('g').c_str(), c.dim("ok / answered").c_str(),
                t.statusDot('a').c_str(), c.dim("no / partial info").c_str(),
                t.statusDot('r').c_str(), c.dim("error / timeout").c_str());

    // T14 — group monitor: decoded telegrams, ETS-style. Service tag colours are shared with the bus monitor.
    t.section("«groupmon» · group monitor  — decoded telegrams (Write · Read · Response)");
    t.tableRow({c.dim("#"), c.dim("TIME"), c.dim("SRC"), c.dim("DEST"), c.dim("SERVICE"), c.dim("DPT"), c.dim("VALUE")},
               {2, 13, 8, 8, 9, 8, 0});
    struct Tg
    {
        const char* no;
        const char* time;
        const char* src;
        const char* dst;
        char svc;
        const char* dpt;
        const char* val;
    };
    static const Tg tgs[] = {
        {"1", "10:23:45.120", "1.1.5", "1/2/3", 'W', "1.001", "On"},
        {"2", "10:23:45.201", "1.1.8", "0/0/1", 'R', "—", "—"},
        {"3", "10:23:45.233", "1.1.1", "0/0/1", 'r', "5.001", "72 %"},
        {"4", "10:23:46.010", "1.1.5", "1/2/4", 'W', "9.001", "21.5 °C"},
        {"5", "10:23:46.402", "1.7.2", "1/2/3", 'W', "1.001", "Off"},
    };
    for (const auto& g : tgs)
        t.tableRow({c.dim(g.no), c.txt(g.time), c.txt(g.src), c.cyan(g.dst), t.svcTag(g.svc), c.dim(g.dpt), c.bold(g.val)},
                   {2, 13, 8, 8, 9, 8, 0});

    // «busmon» — raw LPDU + interpretation, ETS ACK colour (GREEN = NOT acknowledged). Standard AND extended
    // frames; wide / extended frames keep ALL bytes — the hex WRAPS with a hanging indent (resize-safe), and
    // only the explicit compact mode clips with an ellipsis. Frames arrive animated on a tty.
    t.section("«busmon» · busmonitor  — STD + EXT frames · ETS ACK colour · full-byte wrap");
    struct Bm
    {
        const char* type;
        const char* raw;
        const char* dec;
        bool acked;
    };
    static const Bm bms[] = {
        {"STD", "BC 11 05 0A 03 E1 00 81 3C", "1.1.5 → 1/2/3  GroupValueWrite  DPT1.001 On", true},
        {"STD", "BC 11 08 00 01 E1 00 00 A6", "1.1.8 → 0/0/1  GroupValueRead", true},
        {"EXT", "BC E0 11 05 0A FF 03 41 40 00 12 34 56 78 9A BC DE F0 11 22 33 44 55 66 77 88 99 AA BB CC 5D",
         "1.1.5 → 0/10/255  A_PropertyValue_Write  (28-byte APDU · extended frame)", true},
        {"STD", "BC 17 02 0A 03 E1 00 80 7B", "1.7.2 → 1/2/3  GroupValueWrite  Off", false},
        {"EXT", "BC E0 11 09 00 05 3F 43 80 00 00 00 40 00 DE AD BE EF CA FE BA BE 01 02 03 04 05 06 07 08 09 "
                "0A 0B 0C 0D 0E 0F 10 11 12 13 14 15 16 17 18 19 1A 1B 1C 1D 1E 1F 20 21 22 9C",
         "1.1.9 → 0/0/5  A_Memory_Write  (max-size extended frame · 55 bytes)", true},
    };
    auto showFrame = [&](const Bm& b) {
        std::string tag = std::strcmp(b.type, "EXT") == 0 ? t.chip("EXT", 'o') : t.chip("STD", 'c');
        std::string dec = b.acked ? c.txt(b.dec) : c.green(b.dec); // ETS: un-ACKed -> green
        std::string ack = b.acked ? c.dim("ACK") : c.green("— NAK");
        std::printf("  %s %s   %s\n", tag.c_str(), dec.c_str(), ack.c_str());
        t.wrap(b.raw, ftc::Tpl::cols() - 4, 6); // FULL bytes, hanging indent, resize-safe (no truncation)
    };
    for (const auto& b : bms)
    {
        showFrame(b);
        if (tty) std::this_thread::sleep_for(std::chrono::milliseconds(280));
    }
    t.note("ETS colour: green = NOT acknowledged; grey/white = acknowledged · extended frames keep ALL bytes (wrapped)");
    std::printf("  %s %s\n", c.dim("compact mode →").c_str(), t.clip(c.txt(bms[4].raw), ftc::Tpl::cols() - 22).c_str());

    renderUiDemoExtras(t); // «panel-box», discover/pa short+long, keybar, crumb, stepper, settings, KOs, errors, queue, bus-anim, bar/graph styles
    std::printf("\n");
}

/**
 * @brief The second half of the --ui-demo catalogue (chrome, list forms, alternative bar/graph styles).
 * @details Info-frame builder, short/long list forms, keybar/breadcrumb/stepper/settings/group-objects/error/
 *          queue/bus-animation, plus the less-dominant bar/graph variants. Each element is prefixed with its «name».
 */
static void renderUiDemoExtras(ftc::Tpl& t)
{
    ftc::Theme& c = g_theme;
    ftc::I18n& L = g_i18n;
    const bool tty = g_term.isTty();

    // «oknx-logo» — the OpenKNX console mark: 3-line, sizes S/M/L, static then animated,
    // plus «brand-card» = the About / identity block (static mark + product line, copyright, links).
    t.section("«oknx-logo» · oknxLogo()  — the OpenKNX mark · sizes S/M/L · static + animated");
    std::printf("  %s\n", c.dim("small (bus 2):").c_str());
    t.oknxLogo('s');
    std::printf("  %s\n", c.dim("medium (bus 4):").c_str());
    t.oknxLogo('m');
    std::printf("  %s\n", c.dim("large (bus 6):").c_str());
    t.oknxLogo('l');
    if (tty)
    {
        // Animate the large mark IN PLACE for ~5 s (cursor up 3, reprint): packet travels, nodes pulse ■/□.
        t.oknxLogo('l');             // static baseline (cursor now below the 3 lines)
        for (int p = 0; p < 66; ++p) // 66 × 75 ms ≈ 5.0 s
        {
            std::printf("\x1b[3A");
            t.oknxLogo('l', p);
            std::fflush(stdout);
            std::this_thread::sleep_for(std::chrono::milliseconds(75));
        }
        std::printf("\x1b[3A");
        t.oknxLogo('l'); // end on the static mark -> no packet left on screen
    }

    // «brand-card» — the About / identity form (static mark + product line + copyright + links).
    t.section("«brand-card» · brandCard()  — the About / identity block (static mark)");
    t.brandCard();

    // «bar-styles» — less-dominant alternatives to the solid block bar.
    t.section("«bar-styles» · bar()  — block · line · pill  (pick the least dominant)");
    std::printf("  %s %s\n", c.dim("block").c_str(), t.bar(0.62, 22, 'g', 'b').c_str());
    std::printf("  %s %s\n", c.dim("line ").c_str(), t.bar(0.62, 22, 'g', 'l').c_str());
    std::printf("  %s %s\n", c.dim("pill ").c_str(), t.bar(0.62, 22, 'g', 'p').c_str());

    // «graph-styles» — softer sparkline variants.
    std::vector<double> series;
    for (int k = 0; k < 40; ++k)
        series.push_back(380 + 220 * std::sin(k * 0.35) + (k % 5) * 25);
    t.section("«graph-styles» · spark()  — bars · soft dots · line");
    std::printf("  %s %s\n", c.dim("bars").c_str(), t.spark(series, 0, 40, 'b').c_str());
    std::printf("  %s %s\n", c.dim("dots").c_str(), t.spark(series, 0, 40, 'd').c_str());
    std::printf("  %s %s\n", c.dim("line").c_str(), t.spark(series, 0, 40, 'l').c_str());

    // «panel-box» — the reusable info frame at three widths (auto-fit · fixed · fill).
    t.section("«panel-box» · Panel  — info frame: auto-fit · fixed · fill");
    ftc::Panel(t, "KNX Interface", "11.11.0.5")
        .kv("Friendly name", c.bold("IP Interface N148 — ALT"))
        .kv("Individual IA", c.txt("5.0.50"))
        .kv("Routing", c.green("— not advertised"))
        .line(c.dim("summaries / notes go here too"))
        .render(0);
    std::printf("  %s\n", c.dim("↑ render(0) — auto-fit to content").c_str());
    ftc::Panel(t, "Fixed 40").kv("Mode", c.cyan("fixed width")).kv("Cols", c.txt("40")).render(40);
    std::printf("  %s\n", c.dim("↑ render(40) — fixed width").c_str());
    ftc::Panel(t, "Fill").kv("Mode", c.cyan("fill terminal")).render(-1);
    std::printf("  %s\n", c.dim("↑ render(-1) — fill the terminal width").c_str());

    // «section-width» — the section header rule also does capped-auto vs. dynamic-fill (same as the panel).
    t.section("«section:auto» · section()  — capped-auto width (default)");
    t.section("«section:fill» · section(-1)  — dynamic, fills the whole terminal", -1);
    std::printf("  %s\n", c.dim("↑ pass -1 for a section rule that spans the terminal (fixed cols with >0)").c_str());

    // «discover-short» / «discover-long»
    struct Di
    {
        const char* ip;
        const char* name;
        const char* pa;
        const char* medium;
        const char* svc;
    };
    static const Di ifs[] = {
        {"11.11.0.5", "IP-Router N146", "1.1.0", "TP1", "Core·DevMgmt·Tunnel·Routing"},
        {"11.11.0.2", "SCN-IP000.03 Interface", "1.1.2", "TP1", "Core·DevMgmt·Tunnel"},
        {"11.11.0.126", "OpenKNX IP-Interface REG2", "5.0.10", "TP1", "Core·DevMgmt·Tunnel"},
    };
    t.section("«discover-short» · discover  — one compact line per interface");
    for (const auto& d : ifs)
        std::printf("  %s %s  %s  %s\n", t.statusDot('g').c_str(), c.txt(d.ip).c_str(),
                    c.bright(d.name).c_str(), c.dim(d.medium).c_str());
    t.section("«discover-long» · discover  — detailed table (PA · services)");
    t.tableRow({c.dim("ST"), c.dim("IP"), c.dim("NAME"), c.dim("PA"), c.dim("MED"), c.dim("SERVICES")}, {2, 13, 26, 8, 5, 0});
    for (const auto& d : ifs)
        t.tableRow({t.statusDot('g'), c.txt(d.ip), c.txt(d.name), t.chip(d.pa, 'c'), c.dim(d.medium), c.dim(d.svc)},
                   {2, 13, 26, 8, 5, 0});

    // «palist-short» / «palist-long»
    struct Pd
    {
        const char* pa;
        const char* name;
        char st;
        bool okx;
    };
    static const Pd pds[] = {
        {"1.1.0", "IP-Router N146", 'g', false},
        {"1.1.5", "KNeoPixel", 'g', true},
        {"1.1.8", "OpenKNX Sensor", 'g', true},
        {"1.1.12", "(no info)", 'a', false},
        {"1.1.20", "(no response)", 'r', false},
    };
    t.section("«palist-short» · PA list  — dot + address + name");
    for (const auto& p : pds)
        std::printf("  %s %s  %s\n", t.statusDot(p.st).c_str(), c.txt(p.pa).c_str(),
                    (p.st == 'r' ? c.mut(p.name) : c.txt(p.name)).c_str());
    t.section("«palist-long» · PA list  — full table (OpenKNX · state)");
    t.tableRow({c.dim("ST"), c.dim("PA"), c.dim("NAME"), c.dim("INFO")}, {2, 9, 26, 0});
    for (const auto& p : pds)
    {
        std::string name = (p.st == 'r' ? c.mut(p.name) : c.txt(p.name));
        if (p.okx) name += "  " + t.chip("OpenKNX");
        const char* inf = p.st == 'g' ? "ok" : (p.st == 'a' ? "no info" : "read error");
        std::string ic = p.st == 'g' ? c.green(inf) : (p.st == 'a' ? c.amber(inf) : c.red(inf));
        t.tableRow({t.statusDot(p.st), c.txt(p.pa), name, ic}, {2, 9, 26, 0});
    }

    // «keybar»
    t.section("«keybar» · keybar()  — function-key action bar (console)");
    t.keybar({{"F3", "View"}, {"F4", "Edit"}, {"F5", "Copy"}, {"F7", "Mkdir"}, {"F8", "Delete"}, {"F9", "Menu"}, {"F10", "Quit"}});

    t.section("«counters» · counters()  — stat tiles (monitor compare / multi summary)");
    t.counters({{"1284", "A", 'g'}, {"1281", "B", 'a'}, {"1279", "common", 'd'}, {"4", "only-A", 'g'}, {"1", "only-B", 'a'}, {"1", "mismatch", 'r'}});
    t.keybar({{"v", "layout"}, {"d", "diff"}, {"l", "save.xml"}, {"p", "pause"}, {"r", "reconnect"}, {"x", "exit"}, {"?", "help"}});

    // «selrow» — the MC-style current-row highlight bar.
    t.section("«selrow» · selection highlight  — the current-row bar (Theme::sel)");
    std::printf("  %s\n", c.txt("  config.toml         12'480").c_str());
    {
        std::string sel = "  logs/                <dir>";
        while ((int)sel.size() < 42)
            sel += ' ';
        std::printf("%s\n", c.sel(sel).c_str());
    }
    std::printf("  %s\n", c.txt("  firmware.bin.gz       2'048").c_str());

    // «crumb»
    t.section("«crumb» · crumb()  — breadcrumb path");
    std::printf("  %s\n", t.crumb({"sd", "logs", "2026", "07"}).c_str());

    // «stepper»
    t.section("«stepper» · stepper()  — multi-step progress");
    t.stepper({"discover", "connect", "identify", "program", "verify"}, 2);

    // «settings»
    t.section("«settings» · seg()  — segmented on/off toggles");
    t.seg("Sprache", {"de", "en"}, 0);
    t.seg("Verbosity", {"quiet", "normal", "verbose"}, 1);
    t.seg("Charset", {"utf-8", "ascii"}, 0);

    // «groupobjects» — the KO table with C R W T U flags AND a legend() explaining them: the reusable
    // table-with-legend pattern (any table whose columns use flags/symbols/codes gets a legend underneath).
    t.section(L.tr("«groupobjects» · group objects  — flags C R W T U (set = accent · unset = dim ·) + legend",
                   "«groupobjects» · Gruppenobjekte  — Flags K L S Ü A (gesetzt = Akzent · aus = dim ·) + Legende"));
    t.tableRow({c.dim("#"), c.dim("NAME"), c.dim("DPT"), c.dim("FLAGS"), c.dim("VALUE")}, {2, 16, 8, 12, 0});
    struct Ko
    {
        const char* no;
        const char* name;
        const char* dpt;
        const char* fl;
        const char* val;
    };
    static const Ko kos[] = {
        {"1", "Switch", "1.001", "CW", "On"},          // receives writes
        {"2", "Status", "1.001", "CRT", "On"},         // readable + transmits on change
        {"3", "Brightness", "5.001", "CRWTU", "72 %"}, // all flags
        {"4", "Scene", "17.001", "CW", "—"},
    };
    for (const auto& k : kos)
        t.tableRow({c.dim(k.no), c.txt(k.name), c.dim(k.dpt), t.koFlags(k.fl), c.bold(k.val)}, {2, 16, 8, 12, 0});
    t.legend({{L.tr("C", "K"), L.tr("Comm", "Kommunikation")}, {L.tr("R", "L"), L.tr("Read", "Lesen")}, {L.tr("W", "S"), L.tr("Write", "Schreiben")}, {L.tr("T", "Ü"), L.tr("Transmit", "Übertragen")}, {L.tr("U", "A"), L.tr("Update", "Aktualisieren")}});

    // «ga-table» — a group-address table: a second example of the same table + legend() explain pattern.
    t.section("«ga-table» · group addresses  — table + legend (reusable explain pattern)");
    t.tableRow({c.dim("GA"), c.dim("NAME"), c.dim("DPT"), c.dim("S"), c.dim("LINKED OBJECTS")}, {9, 22, 8, 2, 0});
    struct Ga
    {
        const char* ga;
        const char* name;
        const char* dpt;
        bool sending;
        const char* linked;
    };
    static const Ga gas[] = {
        {"1/0/1", "Light Kitchen On/Off", "1.001", true, "Switch 1.1.5 · Status 1.1.5"},
        {"1/2/3", "Blind Living Up/Down", "1.008", true, "Move 1.1.8"},
        {"3/0/1", "Temp Setpoint", "9.001", false, "Setpoint 1.1.12"},
    };
    for (const auto& g : gas)
        t.tableRow({c.cyan(g.ga), c.txt(g.name), c.dim(g.dpt),
                    g.sending ? t.statusDot('g') : c.mut(g_term.glyph("·", ".")), c.dim(g.linked)},
                   {9, 22, 8, 2, 0});
    t.legend({{"S ●", "sending group address (the object transmits here)"}, {"GA", "group address"}, {"DPT", "datapoint type"}});

    // «error» — the reusable error / warning block (Ui::errorBlock).
    t.section("«error» · errorBlock()  — error + warning");
    g_ui.errorBlock(false, "connect refused — E_NO_MORE_CONNECTIONS",
                    {"all 16 tunnels busy on 11.11.0.5", "the interface allows 16 concurrent tunnels"},
                    "retry shortly, or free a tunnel (close an ETS session)");
    g_ui.errorBlock(true, "APDU re-adjusted to 55 B",
                    {"device announced 254 B but answered short"}, "informational — the transfer continues");

    // «queue»
    t.section("«queue» · transfer queue  (F5 add · F9 run)");
    t.tableRow({c.dim("#"), c.dim("OP"), c.dim("FILE"), c.dim("SIZE"), c.dim("STATE")}, {2, 6, 22, 8, 0});
    t.tableRow({c.dim("1"), c.cyan("send"), c.txt("config.toml"), c.txt("12'480"), t.chip("done", 'g')}, {2, 6, 22, 8, 0});
    t.tableRow({c.dim("2"), c.cyan("send"), c.txt("firmware.bin.gz"), c.txt("2'048"), t.chip("running", 'c')}, {2, 6, 22, 8, 0});
    t.tableRow({c.dim("3"), c.cyan("get"), c.txt("telemetry.log"), c.txt("750"), t.chip("queued", 'a')}, {2, 6, 22, 8, 0});

    // «bus-anim» — the OpenKNX packet-flow animation (a packet travelling node -> node).
    t.section("«bus-anim» · OpenKNX bus  — packet flow (animated)");
    const std::string node = c.green(g_term.glyph("◉", "O"));
    if (tty)
    {
        const int span = 24;
        for (int step = 0; step < 108; ++step) // ~5 s: the packet travels the bus several times
        {
            const int pos = step % span;
            std::string road;
            for (int i = 0; i < span; ++i)
                road += (i == pos) ? c.bright(g_term.glyph("▪", "*")) : c.mut(g_term.glyph("─", "-"));
            std::printf("\r  %s%s%s\x1b[K", node.c_str(), road.c_str(), node.c_str());
            std::fflush(stdout);
            std::this_thread::sleep_for(std::chrono::milliseconds(45));
        }
        std::string road;
        for (int i = 0; i < span; ++i)
            road += c.green(g_term.glyph("─", "-"));
        std::printf("\r  %s%s%s  %s\x1b[K\n", node.c_str(), road.c_str(), node.c_str(), c.dim("delivered").c_str());
    }
    else
        std::printf("  %s%s%s  %s\n", node.c_str(), c.mut("──────────▪──────────").c_str(), node.c_str(),
                    c.dim("(animates on a tty)").c_str());
}

/**********************************************************************
 ***************** NON-BLOCKING STDIN LINE READER *******************
 **********************************************************************/

/**
 * @brief Pulls completed input lines without blocking (console mode).
 * @details POSIX: select() on fd 0 then read() into a staging buffer, emitting one full line per poll (bytes
 *          past the first newline stay staged, so piped/pasted multi-line input is never dropped). Windows:
 *          _kbhit() + _getch() assembles a line with local echo. eof() reports a closed pipe so the caller
 *          can leave cleanly instead of spinning forever.
 */
class StdinLines
{
  public:
    /**
     * @brief Fill @p line with the next complete input line if one is ready; returns true when it is.
     */
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
        if (extract(line)) return true; // a full line was already staged from a previous read
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(0, &rfds);
        struct timeval tv{0, 0}; // non-blocking probe
        if (select(1, &rfds, nullptr, nullptr, &tv) <= 0) return false;
        char tmp[256];
        int n = (int)read(0, tmp, sizeof(tmp));
        if (n == 0)
        {
            _eof = true;
            return false;
        } // stdin closed (piped input exhausted)
        if (n < 0) return false;
        _pending.append(tmp, (size_t)n);
        return extract(line);
#endif
    }

    /**
     * @brief True once stdin has hit EOF (a closed pipe) — lets the console loop exit instead of spinning.
     */
    bool eof() const { return _eof; }

  private:
#ifndef _WIN32
    /**
     * @brief Split one complete line off the staging buffer; keep the remainder for the next poll (no byte loss).
     */
    bool extract(std::string& line)
    {
        const size_t nl = _pending.find('\n');
        if (nl == std::string::npos) return false;
        line = _pending.substr(0, nl);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        _pending.erase(0, nl + 1);
        return true;
    }
    std::string _pending;
#endif
    std::string _buf;
    bool _eof = false;
};

/**********************************************************************
 ******************************** CLI *******************************
 **********************************************************************/

#define FTC_CLI_VERSION "1.0.5"

// The colour + glyph layer lives in ftc::Term / Theme / Ui; g_color mirrors Term's decision so the few
// remaining plain-printf sites can gate on it.
static bool g_color = false;

// Optional console session log (`--log`): when open, ftcLineHook tees every remote line here verbatim
// (no ANSI), giving a clean transcript. g_logPath is shown in the console header/footer.
static std::FILE* g_logFp = nullptr;
static std::string g_logPath;

// When an interactive console TUI is running, remote output is routed into its scroll region (above the
// pinned status box) instead of straight to stdout. Null outside a rich console session.
static ftc::ConsoleUi* g_consoleUi = nullptr;

// Count of device-side console-ring overflows: the target dropped output because a burst (e.g. a big
// `help`) filled its console ring faster than the tunnel could drain it. The client marks each with the
// "[...output truncated...]" sentinel; we tally them and show TRUNC in the status bar so the loss is visible.
static uint32_t g_conTrunc = 0;

// Stage 2: while a <pa> command is rendered from the client's STRUCTURED getters, its raw text lines are
// suppressed here (still tee'd to the session log). The structured renderer owns the output.
static bool g_ftcSuppress = false;
static bool g_quiet = false;   // -q: a transfer then reports itself as key<TAB>value facts, nothing else
static bool g_verbose = false; // -V: the live line then also shows what the window regulation decided on

/**
 * @brief Emit one line — into the console TUI scroll region if one is active, else straight to stdout.
 */
static inline void emitLine(const std::string& s)
{
    if (g_consoleUi) g_consoleUi->emit(s);
    else
        std::printf("%s\n", s.c_str());
}

/**
 * @brief Strip ANSI/CSI escape sequences so the session log stays clean text.
 * @details The remote device's own colour codes ride along in the drained console bytes; they must not leak
 *          into the transcript file.
 */
static std::string stripAnsi(const std::string& s)
{
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '\x1b')
        {
            size_t j = i + 1;
            if (j < s.size() && s[j] == '[') // CSI: ESC [ … final byte 0x40-0x7e
            {
                ++j;
                while (j < s.size() && !((unsigned char)s[j] >= 0x40 && (unsigned char)s[j] <= 0x7e))
                    ++j;
                i = j; // skip through the final byte
            }
            // a bare ESC (no '[') is just dropped
        }
        else
            o += s[i];
    }
    return o;
}

/**
 * @brief Enable UTF-8 + ANSI, detect tty/NO_COLOR/COLORTERM via Term; return whether we should emit colour.
 */
static bool initTerminal()
{
    g_term.init();
    g_color = g_term.useColor();
    return g_color;
}

enum FtcSev
{
    SEV_NONE,
    SEV_ERR,
    SEV_WARN,
    SEV_OK
};

/**
 * @brief Classify an [FTC] status line into a severity (host mirror of the web console's console.js classifier).
 * @details Keyword-based; recognises every [FTC] status line. Order matters — WARN wins over ERR so soft
 *          fallbacks that carry an error word (e.g. "cannot self-apply ... skipped") stay warnings.
 */
static FtcSev ftcClassify(const std::string& low)
{
    static const char* WARN[] = {"still busy", "not sent", "busy --", "retry ", "transient", "restarting",
                                 "overwriting", "-> classic", "classic mode", "discarded", "duplicate answer", "old server",
                                 "without the space check", "without it", "skipped", "self-apply", "confirm with", "confirm:",
                                 "really know what i am doing", "erases all", "sweeps the whole", "ignoring", "stale", "gap recovery",
                                 "no-resume", "same size but different", "does not match", "more than our"};
    static const char* ERRK[] = {"no answer", "not reachable", "no console feature", "abort", "failed",
                                 "rejected", "refused", "mismatch", "no response", "not enough space", "out of range", "too long",
                                 "bad pa", "bad end", "unknown command", "unknown backend", "unknown path", "no writable",
                                 "not available", "no file backend", "write error", "sink write failed", "verify failed", "e_no_more",
                                 "send failed", "missing command", "cannot", "not found", "error 0x", "error (0x", "empty scan range",
                                 "source is empty", "aborting", "truncated"};
    static const char* OKK[] = {"login ok", "logged out", "scan saved", "negotiated", "update triggered",
                                "up to date", "resuming at chunk", "matching ", "login not needed", "not password-protected"};
    for (auto k : WARN)
        if (low.find(k) != std::string::npos) return SEV_WARN;
    for (auto k : ERRK)
        if (low.find(k) != std::string::npos) return SEV_ERR;
    for (auto k : OKK)
        if (low.find(k) != std::string::npos) return SEV_OK;
    return SEV_NONE;
}
/**
 * @brief Style an [FTC] status line as a coloured severity block (✖ error / ⚠ warning / ✓ ok); empty if none.
 * @details Colour-mode only — the caller applies it after the useColor() gate, so quiet/pipe output stays raw.
 */
static std::string ftcSeverityStyle(const std::string& in)
{
    ftc::Theme& c = g_theme;
    // The device console-ring overflow marker (no [FTC] prefix) -> a warning.
    if (in.find("[...output truncated...]") != std::string::npos)
        return c.amber(std::string(g_term.glyph("⚠ ", "! ")) + g_i18n.tr("output truncated — console ring overflowed",
                                                                         "Ausgabe abgeschnitten — Konsolen-Ring übergelaufen"));
    const size_t p = in.find("[FTC] ");
    if (p == std::string::npos) return std::string();
    std::string msg = in.substr(p + 6);
    std::string low = msg;
    for (auto& ch : low)
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
    const FtcSev sev = ftcClassify(low);
    if (sev == SEV_NONE) return std::string();
    if (msg == "NOT sent -- still busy, retype your command")
        msg = "COMMAND NOT SENT -- Still busy, retype it again!"; // friendlier wording (explicit rewrite)
    switch (sev)
    {
        case SEV_ERR: return c.red(g_term.glyph("✖ ", "x ") + msg);
        case SEV_WARN: return c.amber(g_term.glyph("⚠ ", "! ") + msg);
        case SEV_OK: return c.green(g_term.glyph("✓ ", "+ ") + msg);
        default: return std::string();
    }
}

/**
 * @brief Body reformatter: re-render each complete line the embedded client emits in the phosphor style.
 * @details Conservative + safe (never touches the client): piped/-q/no-color -> raw line; an all-[#/-] progress
 *          bar -> themed block bar; the client's headline colour (color != 0) -> amber; anything else passes
 *          through. Unknown lines never break; they just print.
 */
static bool ftcLineHook(const std::string& in, uint8_t color)
{
    if (g_logFp) // tee the line into the session log (ANSI-stripped -> clean transcript)
    {
        const std::string clean = stripAnsi(in);
        std::fputs(clean.c_str(), g_logFp);
        std::fputc('\n', g_logFp);
    }
    if (in.find("[...output truncated...]") != std::string::npos) g_conTrunc++; // device console-ring overflow
    // -q during a TRANSFER: the facts block is the whole report. Narrowed to a running transfer -- `info -q`
    // emits the key/value protocol the identity pool parses, and that must keep flowing.
    if (g_quiet && openknxFileTransferClient.transferSetup().valid) return true;
    // Detail lines are written for the log. On screen they belong to -V only -- and there without the marker,
    // which is a filter tag, not something a reader needs to see.
    // Detail lines go to the log only: the live block repaints by counting its rows, so one stray line
    // between repaints shifts the region and it stacks. Their content is in the control block anyway.
    if (in.find("[dbg]") != std::string::npos) return true;
    // A structured renderer owns this command's output -> swallow the raw line. But NOT a refusal: the
    // apply decision happens inside the transfer state machine, so its "fwupdate NOT triggered" landed
    // here and was swallowed. The transfer then reported success for an update that never ran.
    if (g_ftcSuppress)
    {
        std::string low = in;
        for (char& ch : low) ch = (char)std::tolower((unsigned char)ch);
        const bool mustSee = low.find("not triggered") != std::string::npos ||
                             low.find("not triggering") != std::string::npos ||
                             low.find("existence check") != std::string::npos ||
                             low.find("session expired") != std::string::npos ||
                             low.find("apply skipped") != std::string::npos ||
                             low.find("version probe") != std::string::npos ||
                             low.find("apply aborted") != std::string::npos ||
                             low.find("did not route that drive") != std::string::npos ||
                             low.find("refuses writes") != std::string::npos;
        if (!mustSee) return true;
    }
    if (!g_term.useColor())
    {
        emitLine(in); // plain voice (pipe / quiet / NO_COLOR) — no glyphs/colour, stays scriptable
        return true;
    }
    // FTC status lines -> ✖/⚠/✓ severity blocks (colour mode only; same look as the web console + errorBlock).
    {
        const std::string sev = ftcSeverityStyle(in);
        if (!sev.empty())
        {
            emitLine(sev);
            return true;
        }
    }
    ftc::Theme& c = g_theme;
    std::string line = in;
    const size_t lb = line.find('[');
    if (lb != std::string::npos)
    {
        const size_t rb = line.find(']', lb);
        if (rb != std::string::npos && rb > lb + 1)
        {
            // A progress/usage bar is a bracket run of fill (#|=) and track (-|space); at least one fill.
            bool isBar = true, hasFill = false;
            for (size_t i = lb + 1; i < rb; ++i)
            {
                const char ch = line[i];
                if (ch == '#' || ch == '=') hasFill = true;
                else if (ch != '-' && ch != ' ')
                {
                    isBar = false;
                    break;
                }
            }
            if (isBar && hasFill)
            {
                std::string bar;
                for (size_t i = lb + 1; i < rb; ++i)
                    bar += (line[i] == '#' || line[i] == '=') ? c.green(g_term.glyph("▓", "#")) : c.dim(g_term.glyph("░", "-"));
                line = line.substr(0, lb) + bar + line.substr(rb + 1);
            }
        }
    }
    // Semantic status coloring on "key: value" body rows (info/df): recolor a trailing status word.
    if (color == 0 && line.find(':') != std::string::npos)
    {
        size_t end = line.size();
        while (end > 0 && (line[end - 1] == ' ' || line[end - 1] == '\r'))
            --end;
        size_t start = end;
        while (start > 0 && line[start - 1] != ' ')
            --start;
        const std::string w = line.substr(start, end - start);
        std::string col;
        ftc::I18n& L = g_i18n;
        if (w == L.tr("loaded", "geladen") || w == "ok") col = c.green(w);
        else if (w == "off" || w == "n/a" || w == "none" || w == L.tr("unloaded", "nicht geladen"))
            col = c.mut(w);
        else if (w == "on")
            col = c.amber(w);
        if (!col.empty()) line = line.substr(0, start) + col + line.substr(end);
    }
    emitLine(color != 0 ? c.amber(line) : line); // headline/box rule -> amber; else the reformatted line
    return true;
}

/**
 * @brief The OpenKNX console mark + copyright banner (via Ui -> Theme/I18n; DE/EN).
 */
static void banner() { g_ui.banner(); }

/**
 * @brief Print the oknx + FileTransferModule version line.
 */
static void printVersion() { g_ui.version(FTC_CLI_VERSION, MODULE_FileTransferModule_Version, __DATE__, __TIME__); }

/**********************************************************************
 ************************ INTERFACE STECKBRIEF **********************
 **********************************************************************/
// Unicast DESCRIPTION_REQUEST -> parse the DIBs -> identity line / full panel. All fields come straight from
// the KNXnet/IP DESCRIPTION_RESPONSE (03_08_02 Core 7.5.4), no connection needed, so it works against any
// interface (v1 or v2). Missing DIBs -> "n/a".

/**
 * @brief KNXnet/IP service-family id -> short name (nullptr for an unknown id).
 */
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
        default: return nullptr;
    }
}
/**
 * @brief Current IP-assignment method name from the method byte (DHCP/BootP/AutoIP/manual).
 */
static const char* ipMethodName(uint8_t m, bool reported)
{
    // 03_08_03 2.5.5 is a bitset, but a device names the one method actually in use.
    if (m & 0x04) return "DHCP";
    if (m & 0x02) return "BootP";
    if (m & 0x08) return "AutoIP";
    if (m & 0x01) return g_i18n.tr("manual / static", "manuell / statisch");
    // Telling these two apart matters: a device that never answered is a different problem from one that
    // answered with a method nobody has filled in.
    if (!reported) return g_i18n.tr("not reported", "nicht gemeldet");
    return g_i18n.tr("unset on the device", "auf dem Gerät nicht gesetzt");
}

// IfaceDesc + parseDibs() + queryInterface() live in core/Describe.h now (ftc::) — the ONE DESCRIPTION
// parser, shared with `oknx mc`. The apdu* fields of ftc::IfaceDesc are filled by the host below.

/**
 * @brief CONNECT_RESPONSE status byte -> short human meaning (03_08_02 Core, connection response error codes).
 */
static const char* connectStatusStr(int st)
{
    switch (st)
    {
        case 0x22: return "E_CONNECTION_TYPE (tunnelling not supported)";
        case 0x23: return "E_CONNECTION_OPTION (requested option not supported)";
        case 0x24: return "E_NO_MORE_CONNECTIONS (all tunnel slots busy)";
        case 0x25: return "E_NO_MORE_UNIQUE_CONNECTIONS (no free tunnel address)";
        case 0x29: return "E_TUNNELING_LAYER (requested tunnelling layer not supported)";
        default: return "unknown error";
    }
}

/**
 * @brief Read the interface's max-APDU over a KNXnet/IP Device-Management connection (M_PropRead, cEMI Server Object).
 * @details A pure IP-side query — puts NOTHING on the KNX bus — and works on v1 interfaces too (they advertise
 *          DevMgmt even without the v2 Extended Device Info DIB). Delegates to the reusable core; this global
 *          wrapper is the entry point the one-shot paths call.
 */
uint16_t queryMaxApduDeviceMgmt(const std::string& ip, uint16_t port, uint8_t* outPid,
                                char* outReason, size_t reasonCap)
{
    return ftc::queryMaxApduDeviceMgmt(ip, port, outPid, outReason, reasonCap);
}

/**
 * @brief "Fam vN · Fam vN …" — the advertised service families with versions, joined for display.
 */
static std::string famList(const ftc::IfaceDesc& o)
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

/**
 * @brief Default mode: one compact interface identity line.
 */
static void printIfaceLine(const ftc::IfaceDesc& o)
{
    ftc::Theme& c = g_theme;
    ftc::I18n& L = g_i18n;
    ftc::Term& T = g_term;
    if (!o.ok)
    {
        std::printf("  %s %s\n", c.mut(T.glyph("○", "o")).c_str(),
                    c.dim(L.tr("interface — no DESCRIPTION_RESPONSE", "Interface — keine DESCRIPTION_RESPONSE")).c_str());
        return;
    }
    const uint16_t apduVal = o.apduReported ? o.apduReported : o.maxLocalApdu; // device-mgmt read, else DESCRIPTION DIB
    char apdu[48];
    if (o.apduMeasured && o.apduMeasured != apduVal)
        std::snprintf(apdu, sizeof(apdu), "APDU %u/meas %u", apduVal, o.apduMeasured);
    else if (apduVal)
        std::snprintf(apdu, sizeof(apdu), "max APDU %u", apduVal);
    else if (o.apduMeasured)
        std::snprintf(apdu, sizeof(apdu), "APDU ~%u", o.apduMeasured);
    else
        std::snprintf(apdu, sizeof(apdu), "max APDU n/a");
    const char* nm = o.name[0] ? o.name : L.tr("(unnamed)", "(ohne Namen)");
    const std::string routing = o.famVer[0x05] ? ("  " + c.red("· ROUTING (!)")) : std::string();
    std::printf("  %s %s  %s%s\n", c.green(T.glyph("●", "*")).c_str(), c.bold(nm).c_str(),
                c.dim(std::string(ftc::knxMediumName(o.medium)) + " · " + apdu).c_str(), routing.c_str());
}

/**
 * @brief Verbose mode: the full interface steckbrief (phosphor panel, DE/EN labels).
 */
static void printIfacePanel(const std::string& ip, const ftc::IfaceDesc& o)
{
    ftc::Theme& c = g_theme;
    ftc::I18n& L = g_i18n;
    ftc::Term& T = g_term;
    ftc::Tpl& t = g_tpl;
    ftc::Panel p(t, L.tr("KNX Interface", "KNX-Interface"), ip); // new Panel template (parity with `oknx info`)
    if (!o.ok)
    {
        p.line(c.dim(L.tr("(no DESCRIPTION_RESPONSE within 2 s)", "(keine DESCRIPTION_RESPONSE binnen 2 s)")));
        p.render(0);
        return;
    }
    char buf[64];
    p.kv(L.tr("Friendly name", "Name"), c.bold(o.name[0] ? o.name : L.tr("(unnamed)", "(ohne Namen)")));
    p.kv(L.tr("KNX medium", "KNX-Medium"), c.txt(ftc::knxMediumName(o.medium)));
    std::snprintf(buf, sizeof(buf), "%u.%u.%u", (o.ia >> 12) & 0x0F, (o.ia >> 8) & 0x0F, o.ia & 0xFF);
    p.kv(L.tr("Individual IA", "Individual-Adr."), t.chip(buf, 'c'));
    if (o.hasExt)
    {
        std::snprintf(buf, sizeof(buf), "0x%04X", o.mask);
        p.kv(L.tr("Mask / descr.", "Maske"), c.txt(buf));
    }
    char ser[32];
    std::snprintf(ser, sizeof(ser), "%02X%02X:%02X %02X %02X %02X", o.serial[0], o.serial[1], o.serial[2], o.serial[3], o.serial[4], o.serial[5]);
    p.kv(L.tr("Serial", "Seriennr."), c.txt(ser));
    std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", o.mac[0], o.mac[1], o.mac[2], o.mac[3], o.mac[4], o.mac[5]);
    p.kv("MAC", c.txt(buf));
    p.kv(L.tr("Prog mode", "Prog-Modus"),
         (o.status & 0x01) ? t.chip("PROG", 'o') : c.mut(std::string(T.glyph("○", "o")) + " " + L.tr("off", "aus")));
    if (o.hasIp)
    {
        p.sep();
        std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u / %s", (o.ip >> 24) & 0xFF, (o.ip >> 16) & 0xFF, (o.ip >> 8) & 0xFF, o.ip & 0xFF, ipMethodName(o.ipMethod, o.haveIpMethod));
        p.kv(L.tr("IP / method", "IP / Methode"), c.txt(buf));
    }
    p.sep();
    p.kv(L.tr("Service fam.", "Dienste"), c.txt(famList(o)));
    p.kv(L.tr("Routing", "Routing"),
         o.famVer[0x05] ? c.red(L.tr("advertised (!) — acts as a router", "beworben (!) — agiert als Router"))
                        : c.green(L.tr("— not advertised (spec-conform interface)", "— nicht beworben (konformes Interface)")));
    if (o.hasExt && o.maxLocalApdu)
    {
        p.sep();
        std::snprintf(buf, sizeof(buf), "%u B", o.maxLocalApdu);
        p.kv(L.tr("APDU (DIB)", "APDU (DIB)"), c.txt(buf));
    }
    if (o.apduReported)
    {
        std::snprintf(buf, sizeof(buf), "%u B", o.apduReported);
        p.kv(L.tr("APDU reported", "APDU gemeldet"), c.bold(buf) + c.dim(L.tr("  · via device-mgmt", "  · via Device-Mgmt")));
    }
    else if (o.apduReason[0] || !o.hasExt)
    {
        std::string na = L.tr("n/a", "n/a");
        if (o.apduReason[0]) na += std::string(" — ") + o.apduReason; // why device-mgmt yielded no value
        p.kv(L.tr("APDU reported", "APDU gemeldet"), c.dim(na));
    }
    if (o.apduMeasured)
    {
        std::snprintf(buf, sizeof(buf), "%u B", o.apduMeasured);
        p.kv(L.tr("APDU measured", "APDU gemessen"), c.bold(buf) + c.dim(L.tr("  · A_Memory_Read probe", "  · A_Memory_Read-Probe")));
    }
    p.render(0);
}

/**
 * @brief Print the full help/usage screen (banner + options + command groups + examples, DE/EN).
 */
/**
 * @brief `oknx --examples`: worked examples, grouped by the job rather than by the command.
 * @details --help is the reference - every option, one line each. This is the teaching: whole commands
 *          that run as printed, with the reason they exist. Keeping them apart is what lets --help stay
 *          scannable while the tool grows.
 */
static void examples()
{
    ftc::Theme& c = g_theme;
    ftc::I18n& L = g_i18n;
    ftc::Ui& U = g_ui;
    banner();

    struct Ex { const char* cmd; const char* en; const char* de; };
    struct Grp { const char* en; const char* de; const Ex* ex; size_t n; };

    static const Ex gFind[] = {
        {"oknx --discover", "which interfaces are on the network?", "welche Interfaces gibt es im Netz?"},
        {"oknx -i 11.11.0.126 info", "what this interface is and can", "was dieses Interface ist und kann"},
        {"oknx -i 11.11.0.126 scan 1.1 openknx", "every device on line 1.1, with its identity",
         "jedes Gerät auf Linie 1.1, samt Identität"},
        {"oknx -i 11.11.0.126 ps", "who is in programming mode right now",
         "wer gerade im Programmiermodus ist"},
    };
    // Whole jobs, in the order they actually happen - that is what the page is for; single commands
    // are what --help is for.
    static const Ex gNew[] = {
        {"oknx -i 11.11.0.126 ps", "1 · nobody in programming mode yet? then press the button on the device",
         "1 · noch niemand im Programmiermodus? dann die Taste am Gerät drücken"},
        {"oknx -i 11.11.0.126 setpa 1.1.42", "2 · give it its address — refuses unless exactly one answers",
         "2 · ihm seine Adresse geben — verweigert, wenn nicht genau eines antwortet"},
        {"oknx -i 11.11.0.126 1.1.42 progmode off", "3 · programming mode off again, it is not a resting state",
         "3 · Programmiermodus wieder aus, das ist kein Dauerzustand"},
        {"oknx -i 11.11.0.126 1.1.42 info", "4 · did it take? mask, manufacturer, application",
         "4 · hat es gegriffen? Maske, Hersteller, Applikation"},
    };
    static const Ex gSwap[] = {
        {"oknx -i 11.11.0.126 1.1.42 info ga", "1 · write down what it had: addresses, flags, sizes",
         "1 · festhalten, was es hatte: Adressen, Flags, Größen"},
        {"oknx -i 11.11.0.126 1.1.42 unload yes", "2 · clear the old device before it leaves the installation",
         "2 · das alte Gerät leeren, bevor es die Anlage verlässt"},
        {"oknx -i 11.11.0.126 setpa 1.1.42", "3 · new device into programming mode, same address",
         "3 · neues Gerät in den Programmiermodus, dieselbe Adresse"},
        {"(ETS)", "4 · the application has to come from ETS — oknx cannot write it",
         "4 · die Applikation muss aus ETS kommen — oknx kann sie nicht schreiben"},
    };
    static const Ex gWhy[] = {
        {"oknx -i 11.11.0.126 ga import GA-Export.xml", "once per interface: names + datapoint types from your project",
         "einmal je Interface: Namen + Datenpunkttypen aus deinem Projekt"},
        {"oknx -i 11.11.0.126 ga read 1/4/5", "1 · what does the bus say this address currently is?",
         "1 · was sagt der Bus, was auf dieser Adresse steht?"},
        {"oknx -i 11.11.0.126 gm 1/4/5", "2 · watch it: does the switch send at all, does the actuator answer?",
         "2 · zusehen: sendet der Taster überhaupt, antwortet der Aktor?"},
        {"oknx -i 11.11.0.126 ga write 1/4/5 1", "3 · drive it yourself — if this works, the actuator is fine",
         "3 · selbst schalten — wenn das geht, liegt es nicht am Aktor"},
        {"oknx -i 11.11.0.126 bm", "4 · still nothing? the raw bus shows repeats and missing acknowledges",
         "4 · immer noch nichts? der rohe Bus zeigt Wiederholungen und fehlende Quittungen"},
    };
    static const Ex gDev[] = {
        {"oknx -i 11.11.0.126 5.0.3 ping", "is it there, and how fast", "ist es da, und wie schnell"},
        {"oknx -i 11.11.0.126 5.0.3 info ga", "its group objects: addresses, flags, size",
         "seine Gruppenobjekte: Adressen, Flags, Größe"},
        {"oknx -i 11.11.0.126 5.0.3 led blink", "make it blink in the cabinet (this is programming mode)",
         "im Schrank blinken lassen (das ist der Programmiermodus)"},
        {"oknx -i 11.11.0.126 5.0.3 restart", "reboot it", "neu starten"},
        {"oknx -i 11.11.0.126 5.0.3 masterreset confirmedrestart yes",
         "master reset; --help lists what each erase code erases",
         "Master-Reset; was welcher Erase-Code löscht, steht in --help"},
    };
    static const Ex gMon[] = {
        {"oknx -i 11.11.0.126 gm", "every group telegram, decoded", "jedes Gruppentelegramm, dekodiert"},
        {"oknx -i 11.11.0.126 gm 0/7/0,0/7/1,1/4/5", "only these addresses, with their project names",
         "nur diese Adressen, mit ihren Projektnamen"},
        {"oknx -i 11.11.0.126 gm 0/7/0:9.004", "without an import: give the type for this run",
         "ohne Import: den Typ für diesen Lauf angeben"},
        {"oknx -i 11.11.0.126 bm --seconds 60", "raw bus, stops by itself — for a script",
         "roher Bus, hört von selbst auf — fürs Skript"},
        {"oknx -i 11.11.0.126 gm compare 11.11.0.151", "two interfaces on the same bus: do they see the same?",
         "zwei Interfaces am selben Bus: sehen sie dasselbe?"},
    };
    static const Ex gFile[] = {
        {"oknx -i 11.11.0.126 5.0.3 ll", "what is on the device", "was auf dem Gerät liegt"},
        {"oknx -i 11.11.0.126 5.0.3 send ./cfg.json /cfg.json", "upload a file", "Datei hochladen"},
        {"oknx -i 11.11.0.126 5.0.3 get /log.txt ./log.txt", "fetch one back", "eine zurückholen"},
        {"oknx -i 11.11.0.126 5.0.3 login 'secret'", "a device that asks for a password",
         "ein Gerät, das ein Passwort verlangt"},
        {"oknx -i 11.11.0.126 5.0.3 con", "its console, over the tunnel", "seine Konsole, über den Tunnel"},
    };
    static const Ex gFw[] = {
        {"oknx knxota firmware.uf2 --check", "FIRST: compare and report, write nothing",
         "ZUERST: vergleichen und berichten, nichts schreiben"},
        {"oknx -i 11.11.0.126 5.0.3 knxota firmware.uf2", "then the real run",
         "dann der echte Lauf"},
        {"oknx knxota firmware.uf2 --from ../Produkt-0.7.0", "send only the difference — minutes instead of an hour",
         "nur die Differenz senden — Minuten statt einer Stunde"},
        {"oknx knxota resume list", "a run that broke off: what is still lying around",
         "ein abgebrochener Lauf: was noch herumliegt"},
        {"oknx -i 11.11.0.126 5.0.3 perf 100", "how fast this path really is",
         "wie schnell dieser Weg wirklich ist"},
    };
    static const Grp groups[] = {
        {"FINDING YOUR WAY", "ÜBERBLICK", gFind, sizeof(gFind) / sizeof(Ex)},
        {"COMMISSIONING A NEW DEVICE", "EIN NEUES GERÄT IN BETRIEB NEHMEN", gNew, sizeof(gNew) / sizeof(Ex)},
        {"REPLACING A DEVICE", "EIN GERÄT TAUSCHEN", gSwap, sizeof(gSwap) / sizeof(Ex)},
        {"WHY DOES THIS LAMP NOT SWITCH", "WARUM SCHALTET DIESE LAMPE NICHT", gWhy, sizeof(gWhy) / sizeof(Ex)},
        {"ONE DEVICE", "EIN GERÄT", gDev, sizeof(gDev) / sizeof(Ex)},
        {"WATCHING THE BUS", "DEN BUS BEOBACHTEN", gMon, sizeof(gMon) / sizeof(Ex)},
        {"FILES ON A DEVICE", "DATEIEN AUF EINEM GERÄT", gFile, sizeof(gFile) / sizeof(Ex)},
        {"UPDATING FIRMWARE", "FIRMWARE AKTUALISIEREN", gFw, sizeof(gFw) / sizeof(Ex)},
    };

    constexpr int COL = 50;
    const bool wide = ftc::Tpl::cols() >= COL + 46;
    for (const Grp& g : groups)
    {
        U.sectionPlain(L.tr(g.en, g.de));
        for (size_t i = 0; i < g.n; i++)
        {
            const Ex& e = g.ex[i];
            const char* note = L.tr(e.en, e.de);
            const int pad = COL - (int)std::strlen(e.cmd);
            if (wide) std::printf("  %s%*s %s\n", c.txt(e.cmd).c_str(), pad > 1 ? pad : 1, "", c.dim(note).c_str());
            else std::printf("  %s\n      %s\n", c.txt(e.cmd).c_str(), c.dim(note).c_str());
        }
        std::printf("\n");
    }
    U.flushHelp();
    std::printf("  %s\n\n", c.dim(L.tr("every option, one line each:  oknx --help",
                                       "jede Option, je eine Zeile:  oknx --help")).c_str());
}

static void usage()
{
    ftc::Theme& c = g_theme;
    ftc::I18n& L = g_i18n;
    ftc::Ui& U = g_ui;
    banner();

    // Each role keeps one fixed colour (violet=who, bold=what, teal=thing, blue=how) so the line doubles
    // as the legend; role colours don't follow the theme accent (which can be the danger colour).
    U.section(L.tr("USAGE", "AUFRUF"));
    std::printf("  %s %s %s %s %s %s\n", c.dim("oknx").c_str(),
                c.blue(L.tr("[global options]", "[globale optionen]")).c_str(),
                c.violet(L.tr("<subject>", "<subjekt>")).c_str(),
                c.bold(L.tr("<verb>", "<verb>")).c_str(),
                c.oper(L.tr("[operands]", "[operanden]")).c_str(),
                c.blue(L.tr("[options]", "[optionen]")).c_str());
    // The shape above is also the colour legend for every command line below - but only say so when
    // colour is actually being printed (NO_COLOR, a pipe or -q turn it off and the claim would be false).
    if (g_term.useColor())
        std::printf("  %s\n", c.dim(L.tr("these colours repeat in every line below: <pa> violet · verb bold · <operand> teal · --option blue",
                                         "diese Farben wiederholen sich in jeder Zeile unten: <pa> violett · Verb fett · <operand> türkis · --option blau")).c_str());
    std::printf("\n");
    // One column for the shapes, one for what they are for -- the same grid the rest of the help uses.
    {
        // The coloured form carries escape codes, so its byte length is not its width. The plain shape is
        // spelled out once for measuring -- shorter than teaching this one spot to strip ANSI.
        struct { std::string form; const char* plain; const char* en; const char* de; } shapes[] = {
            {std::string(c.dim("oknx ")) + c.blue("-i") + " " + c.txt("A.B.C.D") + " " +
                 c.violet("<pa>") + " " + c.bold("<cmd>") + " " +
                 c.blue("[" + std::string(L.tr("options", "optionen")) + "]"),
             L.tr("oknx -i A.B.C.D <pa> <cmd> [options]", "oknx -i A.B.C.D <pa> <cmd> [optionen]"),
             "a device on the bus", "ein Gerät am Bus"},
            {std::string(c.dim("oknx ")) + c.blue("-i") + " " + c.txt("A.B.C.D") + " " + c.bold("<cmd>"),
             "oknx -i A.B.C.D <cmd>",
             "the interface/router itself: info · con · scan · ps · ga · gm · bm · prop",
             "das Interface/der Router selbst: info · con · scan · ps · ga · gm · bm · prop"},
            {std::string(c.dim("oknx ")) + c.blue("--discover"),
             "oknx --discover",
             "step 1: which interfaces are on the network at all",
             "Schritt 1: welche Interfaces es im Netz ueberhaupt gibt"},
            {std::string(c.dim("oknx ")) + c.bold("<cmd>"),
             "oknx <cmd>",
             "no bus at all: knxota · gzip · decode · config · install",
             "ganz ohne Bus: knxota · gzip · decode · config · install"},
        };
        const bool wide = ftc::Tpl::cols() >= 46 + 56; // the longest note still has to fit beside it
        for (const auto& sh : shapes)
        {
            const char* note = L.tr(sh.en, sh.de);
            if (!wide)
            {
                std::printf("  %s\n      %s\n", sh.form.c_str(), c.dim(note).c_str());
                continue;
            }
            const int pad = 46 - (int)std::strlen(sh.plain);
            std::printf("  %s%*s %s\n", sh.form.c_str(), pad > 1 ? pad : 1, "", c.dim(note).c_str());
        }
    }
    std::printf("\n  %s = %s\n", c.violet("<pa>").c_str(),
                c.dim(L.tr("the individual address of the device you mean — not the one of the interface",
                           "die physikalische Adresse (Individualadresse) des gemeinten Geräts — nicht die des Interfaces")).c_str());
    // The most common mistake with this tool is confusing the two addresses, so they stand next to each
    // other once, labelled, in a line that runs as printed.
    std::printf("  %s %s %s %s\n", c.dim("oknx").c_str(), c.blue("-i 192.168.1.50").c_str(),
                c.violet("1.1.42").c_str(), c.bold("ping").c_str());
    std::printf("      %s\n", c.dim(L.tr("192.168.1.50 = the WAY there (the IP interface) · 1.1.42 = the TARGET (the KNX device)",
                                          "192.168.1.50 = der WEG dorthin (das IP-Interface) · 1.1.42 = das ZIEL (das KNX-Gerät)")).c_str());
    std::printf("      %s\n", c.dim(L.tr("the tunnel is that connection: oknx speaks IP to the interface, the interface speaks KNX to the device",
                                          "der Tunnel ist diese Verbindung: oknx spricht IP mit dem Interface, das Interface spricht KNX mit dem Gerät")).c_str());
    std::printf("\n  %s\n", c.dim(L.tr("ready-made command lines, sorted by the job:  oknx --examples",
                                        "fertige Befehlszeilen, nach Aufgabe sortiert:  oknx --examples")).c_str());
    std::printf("  %s\n\n", c.dim(L.tr("--version · --help · a persisted default: oknx config <key> <value> (kept in ~/.config/oknx/)",
                                        "--version · --help · dauerhafte Vorgabe: oknx config <key> <wert> (liegt unter ~/.config/oknx/)")).c_str());

    U.section(L.tr("OPTIONS", "OPTIONEN"));
    U.cmdRow("--ip A.B.C.D | -i", L.tr("interface / router to tunnel through", "Interface/Router, durch das getunnelt wird"));
    U.cmdRow("--port N", L.tr("KNXnet/IP port (default 3671)", "KNXnet/IP-Port (Vorgabe 3671)"));
    U.cmdRow("--workers N | -W", L.tr("how many identity reads run in parallel (each takes one tunnel)",
                                     "wie viele Identitäten parallel gelesen werden (je ein Tunnel)"));
    U.cmdRow("--discover | -D", L.tr("list interfaces on the LAN and exit", "Interfaces im LAN auflisten und beenden"));
    U.cmdRow("--verbose | -V", L.tr("full interface + target steckbrief first", "voller Interface-+Ziel-Steckbrief vorweg"));
    U.cmdRow("--quiet | -q", L.tr("no chrome, TSV — scriptable (auto on non-TTY)", "keine Deko, TSV — skriptbar (automatisch bei Nicht-TTY)"));
    U.cmdRow("--log [=path]", L.tr("log console session to a file (auto: ~/con_<pa>_<ts>.log)", "Konsolen-Sitzung mitschreiben (auto: ~/con_<pa>_<ts>.log)"));
    U.cmdRow("--prio low|normal|urgent|system", L.tr("KNX priority of the FTC frames (default low; elevated warns + gates)",
                                                     "KNX-Priorität der FTC-Frames (Vorgabe low; erhöht warnt + fragt)"));
    U.cmdRow("--prio-force", L.tr("confirm an elevated --prio in a non-TTY/scripted run", "erhöhtes --prio in Nicht-TTY/Skript bestätigen"));
    U.cmdRow("--lang de|en", L.tr("force language (else OKNX_LANG / locale)", "Sprache erzwingen (sonst OKNX_LANG/Locale)"));
    U.cmdRow("--theme green|amber|cyan", L.tr("accent theme (persist: oknx config theme <name>)", "Akzent-Theme (dauerhaft: oknx config theme <name>)"));
    U.cmdRow("--ascii", L.tr("ASCII instead of box/mark glyphs — colour is NOT affected",
                             "ASCII statt Rahmen- und Markenzeichen — die Farbe bleibt davon unberührt"));
    U.cmdRow("", L.tr("plain text without colour: NO_COLOR=1, a pipe, or --quiet",
                      "reiner Text ohne Farbe: NO_COLOR=1, eine Pipe, oder --quiet"));
    U.cmdRow("--examples", L.tr("worked examples, grouped by the job", "Beispiele, nach Aufgabe sortiert"));
    U.cmdRow("-VqD (bundled)", L.tr("these bundle: -VD = -V -D. They belong BEFORE the command; the transfer flags "
                                    "(-faknqv) come after it",
                                    "diese bündeln: -VD = -V -D. Sie stehen VOR dem Kommando; die Transfer-Flags "
                                    "(-faknqv) danach"));

    U.section(L.tr("THE INTERFACE ITSELF", "DAS INTERFACE SELBST"), L.tr("(--ip, no <pa>)", "(--ip, kein <pa>)"));
    U.cmdRow("info", L.tr("full interface report (DESCRIPTION + device-mgmt)", "kompletter Interface-Report (DESCRIPTION + Device-Mgmt)"));
    U.cmdRow("con | console", L.tr("the interface's OWN console via its webconsole (WebSocket, no tunnel)",
                                   "die EIGENE Konsole des Interfaces über dessen Webconsole (WebSocket, kein Tunnel)"));
    U.cmdRow("prop dump", L.tr("every readable property of every interface object",
                              "jede lesbare Eigenschaft jedes Interface-Objekts"));
    U.section(L.tr("GROUP ADDRESSES", "GRUPPENADRESSEN"), L.tr("(--ip, no <pa>)", "(--ip, kein <pa>)"));
    U.cmdRow("ga read <x/y/z>", L.tr("read a group address (GroupValue_Read)", "Gruppenadresse lesen (GroupValue_Read)"));
    U.cmdRow("ga write <x/y/z> <v>", L.tr("write it: 0-63 plain, or hex octets", "schreiben: 0-63 blank, oder Hex-Oktette"));
    U.cmdRow("ga import <export>", L.tr("learn an ETS group address export: names + datapoint types",
                                       "ETS-Gruppenadressexport einlesen: Namen + Datenpunkttypen"));
    U.cmdRow("", L.tr("XML or CSV, all four ETS layouts; CSV only WITH header lines. Kept per interface",
                      "XML oder CSV, alle vier ETS-Formate; CSV nur MIT Kopfzeilen. Je Interface abgelegt"));
    U.cmdRow("ga table", L.tr("which interface has which table", "welches Interface hat welche Tabelle"));
    U.cmdRow("ga table move <from> <to>", L.tr("re-point a table after the interface changed address",
                                              "Tabelle umhängen, wenn das Interface eine neue Adresse hat"));
    U.cmdRow("ga table rm <iface>", L.tr("drop one table", "eine Tabelle entfernen"));
    U.section(L.tr("WATCHING THE BUS", "DEN BUS BEOBACHTEN"), L.tr("(--ip, no <pa>)", "(--ip, kein <pa>)"));
    U.cmdRow("groupmon | gm", L.tr("live group monitor — decoded telegrams", "Live-Gruppenmonitor — dekodierte Telegramme"));
    U.cmdRow("gm <x/y/z>", L.tr("the same, restricted to one group address", "dasselbe, auf eine Gruppenadresse beschränkt"));
    U.cmdRow("gm <x/y/z>,<x/y/z>,…", L.tr("several addresses at once (comma or space)",
                                         "mehrere Adressen gleichzeitig (Komma oder Leerzeichen)"));
    U.cmdRow("gm <x/y/z>:<DPT>", L.tr("give the datapoint type for this run, without importing",
                                     "den Datenpunkttyp für diesen Lauf angeben, ohne Import"));
    U.cmdRow("ga monitor <x/y/z>", L.tr("alias for gm <x/y/z>", "Kurzform für gm <x/y/z>"));
    U.cmdRow("busmon | bm", L.tr("live bus monitor — raw LPDU, ETS ACK colour", "Live-Busmonitor — Roh-LPDU, ETS-ACK-Farbe"));
    U.cmdRow("gm|bm compare <ipB> [--grace ms] [--raw]", L.tr("A/B busmon fidelity diff (reassembles fragments; --raw = per-piece; --multi = two-stream)",
                                                              "A/B-Busmon-Treuevergleich (setzt Fragmente zusammen; --raw = Einzelstücke; --multi = Zweistrom)"));
    U.cmdRow("  compare readability", L.tr("keys f/c/m/t · flags --only-diff --collapse --no-markers --skew",
                                          "Tasten f/c/m/t · Flags --only-diff --collapse --no-markers --skew"));
    U.cmdRow("gm|bm --frames N | --seconds N", L.tr("stop the monitor after N (scripted)", "Monitor nach N stoppen (skriptbar)"));
    U.section(L.tr("FINDING DEVICES", "GERÄTE FINDEN"));
    U.cmdRow("scan <a.l | a b> [ets] [deep N]", L.tr("discover devices on a line / range (ets = CO probe, slower, finds BCU1/BCU2)",
                                                    "Geräte auf Linie/Bereich finden (ets = CO-Probe, langsamer, findet BCU1/BCU2)"));
    U.cmdRow("scan … --no-details", L.tr("skip the identity read — a scan reads it by default",
                                         "Identität nicht lesen — ein Scan liest sie standardmäßig"));
    U.cmdRow("scan … pace <ms> | drain <ms> | tmo <ms>",
             L.tr("sweep tuning: gap between probes · wait for slow answers at the end · how long one probe "
                  "may stay unconfirmed. Defaults find everything; raise pace on a slow interface",
                  "Feineinstellung: Abstand zwischen Abfragen · Wartezeit am Ende für langsame Antworten · "
                  "wie lange eine Abfrage unbestätigt bleiben darf. Die Vorgaben finden alles; pace erhöhen "
                  "bei einem trägen Interface"));
    U.cmdRow("scan … --no-details", L.tr("sweep only — skip the identity read (identity is read by default)",
                                         "nur suchen — die Identität nicht lesen (sie wird sonst gelesen)"));
    U.cmdRow("scan … openknx", L.tr("read the identity of the System B candidates only, not of every device",
                                    "Identität nur der System-B-Kandidaten lesen, nicht jedes Geräts"));
    U.cmdRow("scan … --workers N | -W N", L.tr("how many identity reads run in parallel (default 5)",
                                               "wie viele Identitätsabfragen parallel laufen (Vorgabe 5)"));
    U.cmdRow("progscan | ps [global|locate]", L.tr("find devices in programming mode + localise the line",
                                                   "Geräte im Programmiermodus finden + Linie lokalisieren"));
    U.cmdRow("ps --seconds 0", L.tr("continuous watch — devices in prog mode appear/disappear live · Ctrl+C",
                                    "Dauer-Überwachung — Geräte im Prog-Modus erscheinen/verschwinden live · Ctrl+C"));

    U.section(L.tr("READING ONE DEVICE", "EIN GERÄT LESEN"), L.tr("(read-only)", "(nur lesen)"));
    U.cmdRow("<pa> ping", L.tr("is the target there? round-trip + ms", "ist das Ziel da? Round-Trip + ms"));
    U.cmdRow("", L.tr("send/get/perf/fwupdate/con ask this first, in one frame; a silent target is reported "
                      "in ~2.5 s instead of after minutes of retries. --force skips the question",
                      "send/get/perf/fwupdate/con fragen das vorab, mit einem Telegramm; ein stummes Ziel "
                      "wird nach ~2,5 s gemeldet statt nach Minuten voller Wiederholungen. --force überspringt"));
    U.cmdRow("<pa> feat | f", L.tr("what the target supports, and why a write is refused",
                                   "was das Ziel kann — und warum ein Schreibzugriff abgelehnt wird"));
    U.cmdRow("<pa> exists | e <path>", L.tr("is that file or folder there?", "gibt es diese Datei / diesen Ordner?"));
    U.cmdRow("<pa> info [ga|<file>]", L.tr("device fingerprint / group comm / file info", "Geräte-Steckbrief (Maske, Hersteller, Applikation) / Gruppenkomm. / Datei-Info"));
    U.cmdRow("<pa> df [sd|efc]", L.tr("target filesystem usage (drive optional)", "Dateisystem-Belegung des Ziels (Laufwerk optional)"));
    U.cmdRow("<pa> ll|ls [sd/|efc/][dir]", L.tr("list a directory (+ CRC, storage bar)", "Verzeichnis listen (+ CRC, Speicher-Balken)"));

    U.section(L.tr("DRIVING ONE DEVICE", "EIN GERÄT STEUERN"));
    U.cmdRow("<pa> led [on|off|blink]",
             L.tr("light the device's prog LED to find it in the cabinet - this SETS PROGRAMMING MODE (PID 54)",
                  "die Prog-LED am Gerät leuchten lassen, um es im Schrank zu finden - das setzt den PROGRAMMIERMODUS (PID 54)"));
    U.cmdRow("", L.tr("while it is on, ANY tool on the bus can overwrite this device's address - switch it off again",
                      "solange er an ist, kann JEDES Werkzeug am Bus die Adresse dieses Geräts überschreiben - wieder ausschalten"));
    U.cmdRow("", L.tr("blink switches it on and off once a second until the next command",
                      "blink schaltet ihn im Sekundentakt an und aus, bis zum nächsten Befehl"));
    U.cmdRow("<pa> progmode [on|off|blink]", L.tr("the same thing, named after what it does",
                                                 "dasselbe, benannt nach dem, was es tut"));
    U.cmdRow("", L.tr("who is in programming mode right now:  oknx -i <ip> ps",
                      "wer gerade im Programmiermodus ist:  oknx -i <ip> ps"));
    U.cmdRow("<pa> restart", L.tr("reboot it (A_Restart over a connection - the form every profile must support)",
                                 "neu starten (A_Restart über eine Verbindung - die Form, die jedes Profil können muss)"));
    U.cmdRow("<pa> masterreset <code> yes",
             L.tr("master reset, erase code BY NAME: confirmedrestart (restart only, erases nothing) · "
                  "resetparam (parameters) · resetlinks (group addresses + associations) · resetap "
                  "(application program) · resetia (address -> 15.15.255) · factoryresetwithoutia "
                  "(all but the address) · factoryreset (EVERYTHING)",
                  "Master-Reset, Erase-Code als NAME: confirmedrestart (nur Neustart, löscht nichts) · "
                  "resetparam (Parameter) · resetlinks (Gruppenadressen + Assoziationen) · resetap "
                  "(Applikationsprogramm) · resetia (Adresse -> 15.15.255) · factoryresetwithoutia "
                  "(alles außer der Adresse) · factoryreset (ALLES)"));
    U.cmdRow("setpa <x.y.z>", L.tr("give the one device in PROGRAMMING MODE this address (no <pa> needed)",
                                   "dem einen Gerät im PROGRAMMIERMODUS diese Adresse geben (kein <pa> nötig)"));
    U.cmdRow("", L.tr("refuses unless exactly one device answers, and reads the new address back as proof",
                      "verweigert, wenn nicht genau eines antwortet, und liest die neue Adresse zur Kontrolle zurück"));
    U.cmdRow("<pa> unload yes", L.tr("ERASE its configuration: address + association table and application go to UNLOADED",
                                     "Konfiguration LÖSCHEN: Adress- + Assoziationstabelle und Applikation auf UNLOADED"));
    U.cmdRow("", L.tr("only an ETS download brings it back; asks again on a terminal",
                      "nur ein ETS-Download holt sie zurück; fragt am Terminal zusätzlich nach"));
    U.cmdRow("<pa> runstate [start|stop]",
             L.tr("read or control the application: 0 halted · 1 running · 3 terminated (03_05_01 4.24)",
                  "Applikation lesen oder steuern: 0 angehalten · 1 läuft · 3 beendet (03_05_01 4.24)"));
    U.cmdRow("", L.tr("stop holds GROUP communication only - properties, memory and restart stay reachable",
                      "stop hält nur die GRUPPEN-Kommunikation an — Properties, Speicher und Neustart bleiben erreichbar"));
    U.cmdRow("", L.tr("the state lives in RAM: a device reset brings the application back",
                      "der Zustand liegt im RAM: ein Geräte-Reset holt die Applikation zurück"));

    U.section(L.tr("PROPERTIES", "EIGENSCHAFTEN"), L.tr("(prop = the interface · busprop = a device on the bus)", "(prop = das Interface · busprop = ein Gerät am Bus)"));
    U.cmdRow("prop read|write <iot> <inst> <pid> [start] [hex]",
             L.tr("the INTERFACE's own objects (local device management, by object TYPE + instance)",
                  "die EIGENEN Objekte des Interfaces (lokales Device Management, nach Objekt-TYP + Instanz)"));
    U.cmdRow("busprop read|write <pa> <objIdx> <pid> [start] [hex]",
             L.tr("a REMOTE device's objects over the bus, by object INDEX (A_PropertyValue_Read/Write)",
                  "die Objekte eines ENTFERNTEN Geräts über den Bus, nach Objekt-INDEX (A_PropertyValue_Read/Write)"));
    U.cmdRow("busprop dump <pa>", L.tr("every readable property of a remote device",
                                      "jede lesbare Eigenschaft eines entfernten Geräts"));

    U.section(L.tr("CONSOLE", "KONSOLE"));
    U.cmdRow("<pa> con [N|max] [apdu M]", L.tr("remote console · N = output drain 4-246 B/answer (max = full · omit = auto from APDU) · apdu M overrides the APDU · ? = help inside",
                                               "Remote-Konsole · N = Ausgabe-Drain 4-246 B/Antwort (max = voll · leer = auto aus APDU) · apdu M überschreibt die APDU · ? = Hilfe drin"));
    U.cmdRow("  · /job add watch every <int> <cmd>", L.tr("in console: recurring auto-command · /job list · /job help",
                                                          "in der Konsole: wiederkehrender Auto-Befehl · /job list · /job help"));
    U.cmdRow("  · /stat  ·  ?", L.tr("in console: full session stats · local shortcut help",
                                     "in der Konsole: komplette Session-Statistik · lokale Shortcut-Hilfe"));

#ifdef OPENKNX_FTC_SECURITY
    U.section(L.tr("FILES ON A DEVICE", "DATEIEN AUF EINEM GERÄT"));
    U.cmdRow("<pa> send <src> [sd/|efc/]<dst>", L.tr("upload a host file (alias: upload)", "Host-Datei hochladen (Alias: upload)"));
    U.cmdRow("<pa> get [sd/|efc/]<remote> [local]", L.tr("download a file (alias: download/receive)", "Datei herunterladen (Alias: download/receive)"));
    U.cmdRow("<pa> rm | mkdir | rmdir | mv", L.tr("delete / create / remove / rename", "löschen / anlegen / entfernen / umbenennen"));
    U.cmdRow("<pa> format yes", L.tr("erase the WHOLE filesystem (gated)", "GANZES Dateisystem löschen (gesichert)"));
    U.cmdRow("<pa> perf [kb] [pkg] [mode]", L.tr("throughput test, no file needed — sd|efc picks the target drive",
                                                 "Durchsatz-Test, ohne Datei — sd|efc wählt das Ziel-Laufwerk"));
    U.cmdRow("sd/ | efc/", L.tr("prefix a REMOTE path (else LittleFS): df ll ls rm mkdir rmdir mv info get perf",
                                "REMOTE-Pfad voranstellen (sonst LittleFS): df ll ls rm mkdir rmdir mv info get perf"));

    U.section(L.tr("UPDATING FIRMWARE", "FIRMWARE AKTUALISIEREN"), L.tr("(knxOTA — over the KNX bus)", "(knxOTA — über den KNX-Bus)"));
    U.cmdRow("knxota <file.uf2|.bin>", L.tr("update a device from a firmware file on THIS computer; without --ip and "
                                            "address it asks for interface and device",
                                            "ein Gerät aus einer Firmware-Datei auf DIESEM Rechner aktualisieren; ohne "
                                            "--ip und Adresse fragt es Interface und Gerät ab"));
    U.cmdRow("knxota ... --from <folder|.app.bin>",
             L.tr("send only the difference to that release; without it knxota offers what it finds",
                  "nur die Differenz zu diesem Release senden; ohne die Angabe bietet knxota an, was es findet"));
    U.cmdRow("knxota ... --no-delta",
             L.tr("always send the whole image, even where a difference would do",
                  "immer das Voll-Image senden, auch wo eine Differenz genügen würde"));
    U.cmdRow("--check | --dry-run", L.tr("only compare and report — writes nothing (try this first)",
                             "nur prüfen und berichten — schreibt nichts (damit zuerst testen)"));
    U.cmdRow("--force", L.tr("allow a downgrade, or a file that states no identity",
                             "Downgrade zulassen, oder eine Datei ohne Kennung"));
    U.cmdRow("--no-compress", L.tr("send it uncompressed (takes about twice as long)",
                                   "unkomprimiert senden (dauert etwa doppelt so lang)"));
    U.cmdRow("--keep-temp", L.tr("keep the prepared firmware on disk", "die vorbereitete Firmware behalten"));
    U.cmdRow("delta make <old> <new> <out.okd>", L.tr("build a difference file from two firmware images - no bus",
                                                      "Differenzdatei aus zwei Firmware-Abbildern bauen - ohne Bus"));
    U.cmdRow("delta show <file.okd>", L.tr("what is in it", "was darin steht"));
    U.cmdRow("delta apply <old> <okd> <out>", L.tr("replay it locally with the same interpreter the device runs",
                                                   "lokal nachspielen, mit demselben Interpreter wie im Gerät"));
    U.cmdRow("", L.tr("--pack compresses the patch · --limit <n> caps the rebuilt size",
                      "--pack komprimiert den Patch · --limit <n> begrenzt die Größe beim Nachbauen"));
    U.cmdRow("knxota resume [list]", L.tr("firmware runs that were interrupted - no bus involved",
                                          "abgebrochene Firmware-Läufe anzeigen - ohne Bus"));
    U.cmdRow("knxota resume clear <pa>|all", L.tr("forget one of them, or all",
                                                 "einen davon vergessen, oder alle"));
    U.cmdRow("<pa> fwupdate <remote>", L.tr("flash a firmware the device already has -> reboots it",
                                            "eine Firmware flashen, die das Gerät schon hat -> Reboot"));
    U.cmdRow("", L.tr("exit: 0 ok · 1 nothing to do · 2 usage · 3 device refuses writes · 6 no answer",
                      "Ende: 0 ok · 1 nichts zu tun · 2 Aufruf · 3 Gerät sperrt · 6 keine Antwort"));


    U.section(L.tr("TRANSFER OPTIONS", "TRANSFER-OPTIONEN"), L.tr("(order-independent, three equal spellings)",
                                                                  "(reihenfolgeunabhängig, drei gleichwertige Schreibweisen)"));
    U.cmdRow("--mode safe | fast", L.tr("safe = confirm every chunk · fast = windowed. --mode=fast works too",
                                        "safe = jeder Chunk wird bestätigt · fast = Fenster. --mode=fast geht auch"));
    U.cmdRow("--pkg <16..254> | auto", L.tr("APDU payload; auto = the interface maximum we detected",
                                            "APDU-Nutzlast; auto = das erkannte Interface-Maximum"));
    U.cmdRow("--window <4..64>", L.tr("pin the fast window instead of letting it adapt — needs --mode fast",
                                      "das fast-Fenster festnageln statt es regeln zu lassen — verlangt --mode fast"));
    U.cmdRow("--apply | --no-apply", L.tr("flash + reboot after a verified upload, or explicitly not",
                                          "nach geprüftem Upload flashen + Reboot, oder ausdrücklich nicht"));
    U.cmdRow("--no-resume", L.tr("ignore a partial on the target; upload from zero",
                                 "Fragment auf dem Ziel ignorieren; von vorn hochladen"));
    U.cmdRow("--keep", L.tr("perf: leave the test file behind instead of deleting it",
                            "perf: die Testdatei stehen lassen statt sie zu löschen"));
    U.cmdRow("retry [max|transfer|backoff [n]]",
             L.tr("show or set how often a transfer retries and how long it waits between attempts",
                  "anzeigen oder setzen, wie oft ein Transfer wiederholt wird und wie lange er dazwischen wartet"));
    U.cmdRow("--progress | --quiet", L.tr("output level for THIS command: live 1 Hz · result line only — not the global --quiet above",
                                          "Ausgabestufe für DIESEN Aufruf: live 1 Hz · nur die Ergebniszeile — nicht das globale --quiet oben"));
    U.cmdRow("", L.tr("same things, shorter: -f -a -k -n -q -v[0-2], bundled as -fa. And the bare words the "
                      "device console always took: fast · safe · auto · w16 · apply · nr · keep · verbose",
                      "dieselben Dinge, kürzer: -f -a -k -n -q -v[0-2], gebündelt als -fa. Und die bloßen "
                      "Wörter, die die Gerätekonsole immer nahm: fast · safe · auto · w16 · apply · nr · "
                      "keep · verbose"));
    U.cmdRow("", L.tr("a value out of range, a window without fast, or an unknown word is refused — never "
                      "silently ignored",
                      "ein Wert außerhalb des Bereichs, ein Fenster ohne fast oder ein unbekanntes Wort wird "
                      "abgelehnt — nie stillschweigend übergangen"));

    U.section(L.tr("ACCESS", "ZUGRIFF"), L.tr("(password-protected targets)", "(passwortgeschützte Ziele)"));
    U.cmdRow("<pa> login <pw>", L.tr("unlock write actions (password -> MAC locally)", "Schreibaktionen freischalten (Passwort -> MAC lokal)"));
    U.cmdRow("<pa> logout", L.tr("lock the write actions again", "Schreibaktionen wieder sperren"));
#endif

    U.section(L.tr("SHORT FORMS & FLAGS", "KURZFORMEN & FLAGS"), L.tr("(everywhere)", "(überall)"));
    U.cmdRow("p i d l u g a m", L.tr("ping · info · df · ll · send · get · apply · mv", "ping · info · df · ll · send · get · apply · mv"));
    U.cmdRow("md rd li lo fw pf f e", L.tr("mkdir · rmdir · login · logout · fwupdate · perf · feat · exists",
                                           "mkdir · rmdir · login · logout · fwupdate · perf · feat · exists"));
    U.cmdRow("s c", L.tr("status · cancel — rm/rmdir/format have no short form on purpose",
                         "status · cancel — rm/rmdir/format bewusst ohne Kurzform"));
    U.cmdRow("-faknqv (bundled)", L.tr("AFTER the command: f fast · a apply · k keep · n no-resume · q quiet · "
                                       "v[0-2] level. An unknown letter is reported, never half-applied",
                                       "NACH dem Kommando: f fast · a apply · k keep · n no-resume · q leise · "
                                       "v[0-2] Stufe. Ein unbekannter Buchstabe wird gemeldet, nie halb angewendet"));
    U.cmdRow("-v0 | -v1 | -v2", L.tr("output level: quiet · compact (default) · live 1 Hz",
                                     "Ausgabestufe: leise · kompakt (Vorgabe) · live 1 Hz"));
    U.cmdRow("verbose [0|1|2]", L.tr("set that level permanently (no value = show it)",
                                     "diese Stufe dauerhaft setzen (ohne Wert = anzeigen)"));

    U.section(L.tr("LOCAL TOOLS", "LOKALE WERKZEUGE"), L.tr("(no bus, no interface)", "(ohne Bus, ohne Interface)"));
    U.cmdRow("gzip <in> <out>", L.tr("gzip a local file (RP firmware prep)", "lokale Datei gzip'en (RP-Firmware vorbereiten)"));
    U.cmdRow("decode <hex LPDU>", L.tr("decode a raw TP1 frame offline (APCI + FTC/console)",
                                       "Roh-TP1-Frame offline dekodieren (APCI + FTC/Console)"));
    U.cmdRow("--ui-demo", L.tr("show every output element this tool draws - for checking a theme",
                               "alle Ausgabebausteine dieses Werkzeugs zeigen - zum Prüfen eines Themes"));
    U.cmdRow("_conprobe <a.l | a b>", L.tr("presence by LINK-LAYER confirm only: finds devices that answer nothing else",
                                           "Anwesenheit nur über die Sicherungsschicht: findet Geräte, die sonst nichts beantworten"));
    U.cmdRow("browse [<start>]", L.tr("the file chooser on its own, no bus involved (also --browse / --file-browser)",
                                     "der Dateiwähler allein, ganz ohne Bus (auch --browse / --file-browser)"));
    U.cmdRow("config <key> <value>", L.tr("a persistent default (theme, lang, ip, …); bare = show all",
                                         "dauerhafte Vorgabe (theme, lang, ip, …); ohne Wert = alles zeigen"));
    U.cmdRow("--force-install", L.tr("install without asking, even when it is a downgrade (scripts)",
                                     "installieren ohne Rückfrage, auch bei einem Downgrade (für Skripte)"));
    U.cmdRow("install | uninstall", L.tr("put this oknx on the PATH, or take it off again — version-aware, asks before a "
                                         "downgrade. --system = /usr/local/bin, else ~/.local/bin, --dir <path> overrides",
                                         "dieses oknx in den PATH legen oder entfernen — versionsbewusst, fragt vor einem "
                                         "Downgrade. --system = /usr/local/bin, sonst ~/.local/bin, --dir <pfad> überschreibt"));

    U.flushHelp();
    std::printf("\n  %s\n\n", c.dim(L.tr("more, grouped by the job:  oknx --examples",
                                        "mehr, nach Aufgabe sortiert:  oknx --examples")).c_str());
}

/**
 * @brief Deliver one typed line to the client's console line-sink during an active console session.
 * @details No-op if the shim leaves the sink unset; also records the input in the transcript when logging.
 */
static void feedConsoleLine(const std::string& line)
{
    if (g_logFp) // record the user's input in the transcript (prefixed so it is not mistaken for output)
    {
        std::fprintf(g_logFp, "> %s\n", line.c_str());
        std::fflush(g_logFp);
    }
    openknx.console.feedLine(line.c_str());
}

/**
 * @brief Open a unique per-session console log; sets g_logFp/g_logPath and returns the path ("" on failure).
 * @details Default location is the user's home dir as con_<pa>_<YYYYMMDD_HHMMSS>.log (OS-independent); an
 *          explicit path overrides it.
 */
static std::string openSessionLog(const std::string& pa, const std::string& explicitPath)
{
    std::string path = explicitPath;
    if (path.empty())
    {
#ifdef _WIN32
        const char* home = std::getenv("USERPROFILE");
        const char sep = '\\';
#else
        const char* home = std::getenv("HOME");
        const char sep = '/';
#endif
        const std::string dir = (home && *home) ? std::string(home) : std::string(".");
        std::time_t now = std::time(nullptr);
        std::tm tmv{};
#ifdef _WIN32
        localtime_s(&tmv, &now);
#else
        localtime_r(&now, &tmv);
#endif
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tmv);
        std::string safePa = pa; // keep the filename portable (no path separators / colon)
        for (char& ch : safePa)
            if (ch == '/' || ch == '\\' || ch == ':') ch = '_';
        path = dir + sep + "con_" + safePa + "_" + stamp + ".log";
    }
    g_logFp = std::fopen(path.c_str(), "w");
    if (!g_logFp) return std::string();
    g_logPath = path;
    std::time_t t = std::time(nullptr);
    std::fprintf(g_logFp, "# oknx console session log — target %s — %s", pa.c_str(), std::ctime(&t)); // ctime ends in \n
    std::fflush(g_logFp);
    return path;
}

/**
 * @brief Path to the persistent console command history (<config-dir>/history, shared across sessions).
 * @details Ensures the directory exists so the first-ever session can append; empty on failure (history off).
 */
/** @brief A file next to the config; the directory is made if it is missing. */
static std::string configSidecar(const char* name)
{
    const std::string cfg = g_cfg.path();
    const size_t sl = cfg.find_last_of("/\\");
    if (sl == std::string::npos) return std::string(name);
    const std::string dir = cfg.substr(0, sl);
#ifdef _WIN32
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    std::string mkErr;
    ftc::makeDirs(dir, mkErr); // best-effort: no record is not worth failing an update over
    return dir + sep + name;
}

/** @brief Where the list of unfinished knxOTA runs lives. */
static std::string otaResumePath() { return configSidecar("knxota_resume"); }

/** @brief The single record this used to be. Read once, taken over into the list, then removed. */
static std::string otaLegacyPath() { return configSidecar("knxota_last"); }

/** @brief Where the "what this computer installed last" list lives (<config-dir>/knxota_bases). */
static std::string knxotaBaseCachePath()
{
    const std::string cfg = g_cfg.path();
    const size_t sl = cfg.find_last_of("/\\");
    if (sl == std::string::npos) return std::string("knxota_bases");
    const std::string dir = cfg.substr(0, sl);
#ifdef _WIN32
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    std::string mkErr;
    ftc::makeDirs(dir, mkErr); // best-effort: no list is not worth failing an update over
    return dir + sep + "knxota_bases";
}

static std::string consoleHistoryPath()
{
    const std::string cfg = g_cfg.path(); // .../oknx/config.toml
    const size_t sl = cfg.find_last_of("/\\");
    if (sl == std::string::npos) return std::string("history");
    const std::string dir = cfg.substr(0, sl);
#ifdef _WIN32
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    std::error_code ec; // no shell -> a config dir with a quote/metachar can't inject
    std::string mkErr;
    ftc::makeDirs(dir, mkErr); // best-effort: no history is not worth failing the session over
    return dir + sep + "history";
}

/**
 * @brief Per-target-PA cache file for the /every auto-commands (<config-dir>/watch_<pa>.txt).
 * @details Empty on failure -> jobs just do not persist across sessions.
 */
static std::string watchPath(const std::string& pa)
{
    const std::string cfg = g_cfg.path();
    const size_t sl = cfg.find_last_of("/\\");
    if (sl == std::string::npos) return std::string();
    const std::string dir = cfg.substr(0, sl);
    const char sep = (cfg.find('\\') != std::string::npos) ? '\\' : '/';
    std::string safePa = pa;
    for (char& ch : safePa)
        if (ch == '/' || ch == '\\' || ch == ':') ch = '_';
    return dir + sep + "watch_" + safePa + ".txt";
}

/**
 * @brief Security gate: true if the line contains a whitespace-delimited "login"/"logout" token (case-insensitive).
 * @details Such a line typed INSIDE a remote console would be relayed as plaintext over the tunnel (obj 160)
 *          before the target could act — leaking the password. We never relay it; login is a separate one-shot
 *          invocation where the password becomes a MAC locally.
 */
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

static volatile std::sig_atomic_t g_abort = 0;
static volatile std::sig_atomic_t g_abortSig = 0; // the terminating signal (SIGINT = real Ctrl+C · SIGTERM = kill)

/**
 * @brief Ctrl+C / SIGTERM handler: request a graceful abort and re-arm the default so a 2nd signal hard-kills.
 * @details Records WHICH signal so the cause is reported honestly — only a real SIGINT is "user abort (Ctrl+C)".
 */
static void onAbortSignal(int s)
{
    g_abort = 1;
    g_abortSig = s;
    std::signal(s, SIG_DFL);
}

/**
 * @brief The honest reason a run is ending: a real Ctrl+C vs a terminate/kill (e.g. a supervisor or a dropped
 *        connection killing the process). Never call a SIGTERM "Ctrl+C".
 */
static std::string abortReason()
{
    return g_abortSig == SIGTERM
               ? g_i18n.tr("terminated (signal) — connection ended", "beendet (Signal) — Verbindung getrennt")
               : g_i18n.tr("user abort (Ctrl+C)", "Abbruch durch Nutzer (Strg+C)");
}

/**
 * @brief The escalating danger wording for a raised FTC priority (>= normal).
 */
static const char* prioDangerText(const std::string& name)
{
    if (name == "normal")
        return g_i18n.tr("Prio NORMAL — preempts normal building communication. Only in a controlled maintenance window.",
                         "Prio NORMAL — verdrängt normale Gebäudekommunikation. Nur im kontrollierten Wartungsfenster.");
    if (name == "urgent")
        return g_i18n.tr("Prio URGENT — preempts ALARMS. Dangerous and NOT spec-conformant for file transfer.",
                         "Prio URGENT — verdrängt ALARME. Gefährlich und NICHT spec-konform für Dateitransfer.");
    return g_i18n.tr("Prio SYSTEM — network-management layer (ETS-reserved). Abusive.",
                     "Prio SYSTEM — Netzwerk-Management-Ebene (ETS-reserviert). Missbräuchlich.");
}

/**
 * @brief The `PRIO: <LEVEL> ⚠…` header indicator (amber for normal, red for urgent/system); empty for low.
 */
static std::string prioHeaderTag(const std::string& name)
{
    if (name == "low") return std::string();
    const char* warn = name == "normal" ? "\xE2\x9A\xA0" : name == "urgent" ? "\xE2\x9A\xA0\xE2\x9A\xA0"
                                                                            : "\xE2\x9A\xA0\xE2\x9A\xA0\xE2\x9A\xA0";
    std::string up = name;
    for (char& ch : up)
        ch = (char)std::toupper((unsigned char)ch);
    const std::string tag = std::string("PRIO: ") + up + " " + warn;
    return name == "normal" ? g_theme.amber(tag) : g_theme.red(tag);
}

/**
 * @brief Gate an elevated FTC priority: warn, and require an explicit confirmation before it is applied.
 * @details Interactive TTY -> a red DANGER block + `Proceed? [y/N]`, proceed only on y/j. Non-interactive
 *          (piped / -q) never hangs on a prompt: it requires --prio-force, else refuses. --prio-force always
 *          proceeds (after surfacing the warning). Returns true = proceed.
 */
static bool confirmElevatedPriority(const std::string& name, bool force, bool interactive)
{
    const int bars = name == "urgent" ? 2 : name == "system" ? 3 : 1;
    std::string mark;
    for (int i = 0; i < bars; ++i)
        mark += "\xE2\x9A\xA0";
    std::fprintf(stderr, "\n  %s %s\n", g_theme.red(mark).c_str(), g_theme.red(prioDangerText(name)).c_str());
    if (force) return true;
    if (!interactive)
    {
        std::fprintf(stderr, "  %s\n",
                     g_i18n.tr("refusing an elevated priority without --prio-force (non-interactive).",
                               "erhöhte Priorität ohne --prio-force verweigert (nicht-interaktiv)."));
        return false;
    }
    std::fprintf(stderr, "  %s ", g_i18n.tr("Proceed? [y/N]", "Fortfahren? [j/N]"));
    std::fflush(stderr);
    char buf[16];
    if (!std::fgets(buf, sizeof(buf), stdin)) return false;
    const char ch = (char)std::tolower((unsigned char)buf[0]);
    return ch == 'y' || ch == 'j';
}

/**
 * @brief Common mid-run abort cleanup: cancel the transfer and let it + the CO teardown reach the wire.
 * @details Returns 130 (128 + SIGINT), the conventional code, so the interface frees the channel at once
 *          instead of blocking until its connection timeout.
 */
static int abortCleanly()
{
    std::fprintf(stderr, "\n[abort] %s -- cancelling + closing cleanly (signal again to force)...\n", abortReason().c_str());
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

/**********************************************************************
 ************************* TRANSFER PRESENTER ***********************
 **********************************************************************/
// send / get / perf — the CLI OWNS the host presentation, drawn from the client's structured info-API
// (transferSetup()/status()/transferResult()). The shared module keeps rendering its own console box
// on-device. Host-only (non-quiet): g_ftcSuppress swallows the SM's own lines so we render one clean Panel
// plus an in-place xx.xx% progress line.

/**
 * @brief KB with one decimal, no libm surprises (matches the ETS-style figures).
 */
static std::string kbStr(uint32_t bytes)
{
    char b[24];
    std::snprintf(b, sizeof(b), "%u.%u", bytes >> 10, (unsigned)(((bytes & 1023) * 10) >> 10));
    return b;
}

/**
 * @brief The transfer modes, named for a UI.
 */
static const char* xferModeName(uint8_t m) { return m == 1 ? "fast" : "safe"; }

/**
 * @brief The setup Panel (Source/Target/Size/Mode/Framing/Options) — drawn once when transferSetup() is valid.
 */
static void renderXferSetup(const FtcTransferSetup& s)
{
    ftc::Theme& c = g_theme;
    ftc::I18n& L = g_i18n;
    ftc::Tpl& t = g_tpl;
    auto pa = [&](uint16_t a) { char b[16]; std::snprintf(b, sizeof(b), "%u.%u.%u", (a >> 12) & 0xF, (a >> 8) & 0xF, a & 0xFF); return std::string(b); };
    const char* title = s.kind == FtcXferKind::Perf       ? L.tr("Speed test", "Speed-Test")
                        : s.kind == FtcXferKind::Download ? L.tr("Download", "Download")
                                                          : L.tr("Upload", "Upload");
    ftc::Panel p(t, std::string("FileTransfer · ") + title, pa(s.target));
    p.kv(L.tr("Source", "Quelle"), c.txt(s.kind == FtcXferKind::Download ? s.remote : s.local));
    p.kv(L.tr("Target", "Ziel"), c.txt(s.kind == FtcXferKind::Download ? s.local : s.remote));
    char buf[80];
    if (s.size)
    {
        if (s.hasCrc) std::snprintf(buf, sizeof(buf), "%u B  ·  CRC32 0x%08X", (unsigned)s.size, (unsigned)s.crc);
        else
            std::snprintf(buf, sizeof(buf), "%u B", (unsigned)s.size);
        p.kv(L.tr("Size", "Größe"), c.txt(buf));
    }
    if (s.kind != FtcXferKind::Download)
    {
        const char* sem = s.mode == 1 ? L.tr("windowed · CRC per window (report)", "windowed · CRC pro Fenster (Report)")
                                      : L.tr("CRC per chunk · reliable", "CRC pro Chunk · zuverlässig");
        const char mc = s.mode == 1 ? 'c' : 'g';
        std::string modeRow = t.chip(xferModeName(s.mode), mc) + "  " + c.dim(sem);
        if (s.mode == 1)
        {
            // Say which window is in force -- otherwise pinned and adaptive look the same.
            char wb[48];
            if (s.fixedWindow)
                std::snprintf(wb, sizeof(wb), L.tr("  ·  window %u (pinned)", "  ·  Fenster %u (fest)"),
                              (unsigned)s.fixedWindow);
            else
                std::snprintf(wb, sizeof(wb), "%s", L.tr("  ·  window adapts", "  ·  Fenster regelt sich"));
            modeRow += c.dim(wb);
        }
        p.kv(L.tr("Mode", "Modus"), modeRow);
    }
    if (s.chunkSize && s.chunks) // download learns size/chunks only after the open answer -> skip until known
    {
        std::snprintf(buf, sizeof(buf), "%u B/chunk  ·  %u chunks", (unsigned)s.chunkSize, (unsigned)s.chunks);
        p.kv(L.tr("Framing", "Framing"), c.txt(buf));
    }
    std::string opt;
    if (s.kind == FtcXferKind::Perf) opt = s.keep ? L.tr("keep test file", "Testdatei behalten") : L.tr("remove test file", "Testdatei löschen");
    else if (s.kind == FtcXferKind::Upload)
    {
        opt = s.noResume ? L.tr("fresh (no resume)", "frisch (kein Resume)") : L.tr("resume = auto", "Resume = auto");
        if (s.willApply) opt += L.tr("  ·  apply + reboot", "  ·  flashen + Reboot");
    }
    if (!opt.empty()) p.kv(L.tr("Options", "Optionen"), c.dim(opt));
    p.render(0);
    std::fflush(stdout);
}

/**
 * @brief One in-place progress line (stderr, resize-safe) from FtcStatus; `up` picks the arrow.
 * @details Polled every loop -> refreshes ~1 Hz+ even on a slow link, with fine xx.xx% (percentX100) and the
 *          live chunk/window.
 */
static void renderXferProgress(const FtcStatus& st, bool up)
{
    ftc::Theme& c = g_theme;
    const uint16_t px = st.percentX100(); // 4567 -> 45.67 %
    const std::string bar = g_tpl.bar(st.total ? (double)st.done / (double)st.total : 0.0, 22, 'g');
    char pct[16];
    std::snprintf(pct, sizeof(pct), "%u.%02u%%", px / 100, px % 100);
    const uint32_t rem = st.total > st.done ? st.total - st.done : 0;
    const uint32_t eta = st.bps ? rem / st.bps : 0;
    char tail[96];
    std::snprintf(tail, sizeof(tail), "seq %u/%u  ·  %s/%s KB  ·  %u B/s  ·  ETA %um%02us",
                  (unsigned)st.chunk, (unsigned)st.chunks, kbStr(st.done).c_str(), kbStr(st.total).c_str(),
                  (unsigned)st.bps, (unsigned)(eta / 60), (unsigned)(eta % 60));
    std::string line = std::string("  ") + c.cyan(g_term.glyph(up ? "▲" : "▼", up ? "^" : "v")) + " " + bar +
                       "  " + c.bold(pct) + "  " + c.dim(tail);
    if (st.window > 0)
    {
        char wb[24];
        std::snprintf(wb, sizeof(wb), "  ·  win %u", (unsigned)st.window);
        line += c.amber(wb);
    }
    std::fprintf(stderr, "\r%s\x1b[K", g_tpl.clip(line, ftc::Tpl::cols() - 1).c_str());
    std::fflush(stderr);
}

/**
 * @brief Wall clock + eta seconds -> "HH:MM" (the expected-finish time).
 */
static std::string finishClock(uint32_t etaSec)
{
    std::time_t tt = std::time(nullptr) + (std::time_t)etaSec;
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &tt);
#else
    localtime_r(&tt, &tmv);
#endif
    char b[8];
    std::snprintf(b, sizeof(b), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    return b;
}

/**
 * @brief Draw/redraw an in-place multi-line region on stderr (TTY only); tracks its height in @p rows.
 * @details The next call moves the cursor back up over exactly that many lines; each line is cleared + resize-clipped.
 */
static void drawLiveBlock(const std::vector<std::string>& lines, int& rows)
{
    if (rows > 0) std::fprintf(stderr, "\x1b[%dA", rows); // back to the top of the region
    for (const auto& ln : lines)
        std::fprintf(stderr, "\r\x1b[K%s\n", g_tpl.clip(ln, ftc::Tpl::cols()).c_str());
    rows = (int)lines.size();
    std::fflush(stderr);
}
/**
 * @brief Erase the live region (cursor up + clear-to-end-of-screen), leaving the cursor at its top.
 */
static void clearLiveBlock(int& rows)
{
    if (rows > 0)
    {
        std::fprintf(stderr, "\x1b[%dA\x1b[J", rows);
        std::fflush(stderr);
        rows = 0;
    }
}

/**
 * @brief A throughput scale robust to the startup spike (rejects samples above 4× the median).
 * @details A chunk finishing in ~ms reports a huge instantaneous B/s that TP1 can never sustain (~468 B/s
 *          ceiling), which would otherwise flatten the whole curve and jump the display.
 */
static double robustMax(const std::vector<double>& v)
{
    if (v.empty()) return 1.0;
    std::vector<double> s(v);
    std::sort(s.begin(), s.end());
    const double med = s[s.size() / 2];
    const double cap = med > 0 ? med * 4.0 : s.back();
    double mx = 0;
    for (double x : s)
        if (x <= cap && x > mx) mx = x;
    return mx > 0 ? mx : (s.back() > 0 ? s.back() : 1.0);
}

/**
 * @brief Percentile band [lo,hi] for the scope auto-zoom: p12..p94 of the spike-rejected samples (+headroom).
 * @details Lets a steady throughput with small jitter fill the vertical range instead of flat-lining; degenerate -> [0,hi].
 */
static void robustBand(const std::vector<double>& v, double& lo, double& hi)
{
    lo = 0.0;
    hi = robustMax(v);
    if (v.size() < 6) return;
    std::vector<double> s(v);
    std::sort(s.begin(), s.end());
    const double med = s[s.size() / 2];
    const double cap = med > 0 ? med * 4.0 : s.back();
    std::vector<double> f;
    for (double x : s)
        if (x <= cap) f.push_back(x); // drop the startup spikes
    if (f.size() < 6) return;
    auto pct = [&](double p) { return f[(size_t)(p * (f.size() - 1))]; };
    lo = pct(0.12);
    hi = pct(0.94);
    if (hi - lo < 1e-6)
    {
        lo = f.front();
        hi = f.back();
        if (hi - lo < 1e-6) hi = lo + 1;
    } // flat -> widen
    const double pad = (hi - lo) * 0.15;
    lo -= pad;
    hi += pad;
    if (lo < 0) lo = 0;
}

/**
 * @brief Resample a per-sample event bitset to @p width columns, mirroring sparkRows' geometry.
 * @details Left-pad a short history to the right edge; bucket-OR a long one — so the marker lane lines up 1:1
 *          with the curve above it.
 */
static std::vector<uint8_t> resampleEvents(const std::vector<uint8_t>& evt, int width)
{
    std::vector<uint8_t> ev((size_t)width, 0);
    if (evt.empty() || width < 1) return ev;
    if ((int)evt.size() >= width)
        for (int col = 0; col < width; ++col)
        {
            const size_t a = (size_t)col * evt.size() / width, b = (size_t)(col + 1) * evt.size() / width;
            uint8_t e = 0;
            for (size_t j = a; j < b && j < evt.size(); ++j)
                e |= evt[j];
            if (a == b && a < evt.size()) e |= evt[a];
            ev[(size_t)col] = e;
        }
    else
    {
        const int off = width - (int)evt.size();
        for (size_t k = 0; k < evt.size(); ++k)
            ev[(size_t)off + k] = evt[k];
    }
    return ev;
}

/**
 * @brief One marker-lane string aligned to the curve: CRC-error ✗ (red) · resend ↻ (amber) · confirm ╵ (cyan) · idle · (dim).
 * @details Priority error > resend > confirm, so the worst event in a bucket wins.
 */
static std::string markerLane(const std::vector<uint8_t>& evt, int width)
{
    ftc::Theme& c = g_theme;
    const std::vector<uint8_t> ev = resampleEvents(evt, width);
    std::string lane;
    for (uint8_t e : ev)
    {
        if (e & 4) lane += c.red(g_term.glyph("✗", "x"));
        else if (e & 2)
            lane += c.amber(g_term.glyph("↻", "!"));
        else if (e & 1)
            lane += c.cyan(g_term.glyph("╵", "'"));
        else
            lane += c.dim(g_term.glyph("·", "."));
    }
    return lane;
}

/**
 * @brief The marker legend WITH live counts: CRC/ack · resend · CRC-error totals. Same glyphs/colours as
 *        the curve markers, so the numbers read against the lane above them.
 */
static std::string markerCounts(uint16_t acks, uint16_t resends, uint16_t crcErrors)
{
    ftc::Theme& c = g_theme;
    ftc::I18n& Lz = g_i18n;
    char b[16];
    std::snprintf(b, sizeof(b), "%u", (unsigned)acks);
    std::string s = c.cyan(g_term.glyph("╵", "'")) + " " + c.bold(std::string(b)) + c.dim(Lz.tr(" ack", " ack"));
    std::snprintf(b, sizeof(b), "%u", (unsigned)resends);
    s += "   " + c.amber(g_term.glyph("↻", "!")) + " " + c.bold(std::string(b)) + c.dim(Lz.tr(" resend", " Resend"));
    std::snprintf(b, sizeof(b), "%u", (unsigned)crcErrors);
    s += "   " + c.red(g_term.glyph("✗", "x")) + " " + c.bold(std::string(b)) + c.dim(Lz.tr(" CRC-err", " CRC-Fehler"));
    return s;
}

/**
 * @brief The throughput scope: green dotted curve (auto-zoomed to [lo,hi]) with coloured event trigger markers.
 * @details A vertical line rises from the curve to the top at columns where an event happened — CRC/ack (cyan) ·
 *          resend (amber) · CRC-error (red) — so events pop against the trace. Returns @p rows strings top-to-bottom.
 */
static std::vector<std::string> scopeRows(const std::vector<double>& hist, const std::vector<uint8_t>& evt,
                                          int W, int rows, double lo, double hi)
{
    ftc::Theme& c = g_theme;
    ftc::Term& tm = g_term;
    std::vector<std::string> out;
    if (rows < 1 || W < 1) return out;
    // resample hist -> W raw values (mirror sparkRows: left-pad a short history to the right edge; bucket-average long)
    std::vector<double> v((size_t)W, 0.0);
    if (!hist.empty())
    {
        if ((int)hist.size() >= W)
            for (int col = 0; col < W; col++)
            {
                const size_t a = (size_t)col * hist.size() / W, b = (size_t)(col + 1) * hist.size() / W;
                double s = 0;
                size_t n = 0;
                for (size_t j = a; j < b && j < hist.size(); ++j)
                {
                    s += hist[j];
                    ++n;
                }
                v[(size_t)col] = n ? s / n : (a < hist.size() ? hist[a] : 0.0);
            }
        else
        {
            const int off = W - (int)hist.size();
            for (size_t k = 0; k < hist.size(); ++k)
                v[(size_t)off + k] = hist[k];
        }
    }
    const std::vector<uint8_t> ev = resampleEvents(evt, W);
    const double span = (hi - lo) > 1e-9 ? (hi - lo) : (hi > 0 ? hi : 1.0);
    static const char* dots[8] = {"⣀", "⣀", "⣄", "⣤", "⣦", "⣶", "⣷", "⣿"};
    static const char* adots[8] = {".", ".", ":", ":", "-", "=", "+", "#"};
    const char** ramp = tm.ascii() ? adots : dots;
    const char* vline = tm.glyph("│", "|");
    for (int row = 0; row < rows; row++) // row 0 = top
    {
        const double rlo = (double)(rows - 1 - row) / rows, rhi = (double)(rows - row) / rows;
        std::string line;
        for (int col = 0; col < W; col++)
        {
            double f = (v[(size_t)col] - lo) / span;
            if (f < 0) f = 0;
            if (f > 1) f = 1;
            if (f >= rhi) line += c.green(ramp[7]);
            else if (f > rlo)
            {
                int idx = (int)(((f - rlo) / (rhi - rlo)) * 7 + 0.5);
                if (idx < 0) idx = 0;
                if (idx > 7) idx = 7;
                line += c.green(ramp[idx]);
            }
            else // empty cell above the curve -> draw the event line here (worst event wins)
            {
                const uint8_t e = ev[(size_t)col];
                if (e & 4) line += c.red(vline);
                else if (e & 2)
                    line += c.amber(vline);
                else if (e & 1)
                    line += c.cyan(vline);
                else
                    line += " ";
            }
        }
        out.push_back(line);
    }
    return out;
}

static constexpr uint32_t WIN_FRESH_MS = 4000; // how long a window change stays "fresh" (shown as from>to, blinking)
static constexpr uint32_t WIN_BLINK_MS = 450;  // blink half-period of a fresh change

/**
 * @brief The window field: what the regulation is doing, not just the number it arrived at.
 * @details Colour carries the state (amber = still searching, red = backing off after an overrun, green =
 *          settled, blue = pinned by the user, which is the ABSENCE of regulation). "32>40" and the blink
 *          show only while a change is fresh -- a permanently blinking field stops being read. Verbose adds
 *          the two numbers the window trades against each other: clean windows and the report round trip.
 */
static std::string winField(const FtcStatus& st, bool verbose)
{
    const ftc::Theme& c = g_theme;
    ftc::I18n& L = g_i18n;
    const uint32_t age = st.windowSinceMs ? (uint32_t)(nowMs() - st.windowSinceMs) : 0xFFFFFFFFu;
    const bool fresh = st.windowSinceMs && age < WIN_FRESH_MS;
    // Blink by redraw phase, not SGR 5 -- half the terminals ignore or reinterpret that attribute.
    const bool on = !fresh || ((nowMs() / WIN_BLINK_MS) & 1u) == 0u;

    std::string val = std::to_string(st.window);
    if (fresh && st.windowFrom) val = std::to_string(st.windowFrom) + g_term.glyph("\u203a", ">") + val;

    std::string body = std::string(g_term.glyph("\u27e6", "[")) + val + g_term.glyph("\u27e7", "]");
    std::string painted;
    switch (st.windowState)
    {
        case 2: painted = c.blue(body); break;
        case 3: painted = c.red(body); break;
        case 1: painted = c.green(body); break;
        default: painted = c.amber(body); break;
    }
    if (!on) painted = c.mut(body); // the off half of the blink -- same width, so the line never jitters

    std::string out = c.dim("    win ") + c.bold(painted);
    if (!verbose) return out;

    const char* word = st.windowState == 2   ? L.tr("pinned", "fest")
                       : st.windowState == 3 ? L.tr("backing off", "weicht aus")
                       : st.windowState == 1 ? L.tr("settled", "steht")
                                             : L.tr("probing", "tastet");
    out += " " + c.mut(word);
    // Both sides of the trade: a bigger window amortises the report, a smaller one survives a slow target.
    char ex[64];
    std::snprintf(ex, sizeof(ex), "%s%u %s  %s %u ms", g_term.glyph("\u00b7 ", "- "), (unsigned)st.windowClean,
                  L.tr("clean", "sauber"), L.tr("report", "Report"), (unsigned)st.reportMs);
    out += " " + c.dim(ex);
    return out;
}

/**
 * @brief The regulation block (-V only): why the transfer runs at the speed it runs at.
 * @details Each line answers one question that costs a measurement session to answer otherwise:
 *          is the window still searching, is OUR pacer the brake, and how much of the TP line is in use.
 *          The line figure is derived: frame = payload + ~16 octets of KNX header/FCS, TP1 = 11 bits/octet at 9600.
 */
static std::vector<std::string> regulationBlock(const FtcStatus& st, const FtcTransferSetup& setup, uint32_t avgBps)
{
    const ftc::Theme& c = g_theme;
    ftc::I18n& L = g_i18n;
    std::vector<std::string> out;
    if (setup.mode != 1 || st.window == 0) return out; // windowed transfers only -- safe has no regulation

    // window trajectory: one bar per distinct size the regulation settled on
    static std::vector<uint16_t> hist;
    static uint16_t lastW = 0;
    if (st.window != lastW) { lastW = st.window; hist.push_back(st.window); if (hist.size() > 24) hist.erase(hist.begin()); }
    std::string spark, path;
    uint16_t hi = 1;
    for (uint16_t w : hist) if (w > hi) hi = w;
    static const char* BARS[8] = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
    for (uint16_t w : hist) spark += g_term.glyph(BARS[(size_t)((uint32_t)w * 7u / hi)], "#");
    for (size_t i = 0; i < hist.size() && i < 4; i++)
        path += (i ? g_term.glyph("›", ">") : "") + std::to_string(hist[i]);
    if (hist.size() > 4) path += g_term.glyph("›", ">") + std::to_string(hist.back());

    const std::string lead = std::string("   ") + c.gold(L.tr("Control ", "Regelung")) + "  ";
    const std::string pad = std::string("             ");

    char b[220];
    std::snprintf(b, sizeof(b), "%s, %s %u %s", path.c_str(), L.tr("held for", "seit"), (unsigned)st.windowClean,
                  L.tr("windows", "Fenstern unverändert"));
    out.push_back(lead + c.dim(L.tr("window  ", "Fenster ")) + "  " + c.green(spark) + "  " + c.dim(b));

    // framing: what the two ends allowed, and what we made of it
    std::snprintf(b, sizeof(b), "pkg %u", (unsigned)(setup.chunkSize + 8));
    std::string fr = pad + c.dim(L.tr("framing ", "Rahmen  ")) + "  " + c.txt(b);
    std::snprintf(b, sizeof(b), " (%s %u · %s %u)", L.tr("target", "Ziel"), (unsigned)setup.targetApdu,
                  L.tr("interface", "Interface"), (unsigned)g_ifaceApdu);
    fr += c.mut(b);
    std::snprintf(b, sizeof(b), " %s %u B/chunk", g_term.glyph("→", "->"), (unsigned)setup.chunkSize);
    fr += c.txt(b) + (st.done > setup.chunkSize ? c.mut(L.tr(" · proven", " · bewährt")) : std::string());
    out.push_back(fr);

    // are WE the brake? the one question that decides whether tuning the client is worth anything
    // Judge on the AVERAGE: the momentary rate reads five-digit nonsense while the send queue fills. Below
    // one delivered window there is nothing honest to say at all.
    const uint32_t pace = g_knxTunnel.paceBps();
    if (avgBps && st.done > (uint32_t)st.window * setup.chunkSize)
    {
        const bool binds = pace && pace < avgBps + avgBps / 10u;
        std::snprintf(b, sizeof(b), "Pacer %u B/s", (unsigned)pace);
        out.push_back(pad + c.dim(L.tr("rate    ", "Tempo   ")) + "  " + c.txt(b) +
                      (binds ? c.amber(L.tr(" · binds", " · bremst")) : c.mut(L.tr(" · does not bind", " · bindet nicht"))) +
                      c.dim(std::string(" ") + g_term.glyph("—", "-") + " ") +
                      c.txt(std::to_string((unsigned)avgBps) + L.tr(" B/s from the link", " B/s liefert die Strecke")));
    }

    // how much of the wire is actually in use -- 100 % would mean the line is the ceiling, and it is not
    if (avgBps && setup.chunkSize)
    {
        const double airMs = (setup.chunkSize + 16.0) * 11.0 / 9.6;   // one frame on TP1 at 9600 bit/s
        const double beatMs = 1000.0 * setup.chunkSize / (double)avgBps; // measured chunk cadence
        if (beatMs > airMs)
        {
            std::snprintf(b, sizeof(b), "%s %.0f ms · %s %.0f ms", L.tr("frame", "Rahmen"), airMs,
                          L.tr("beat", "Takt"), beatMs);
            std::string ln = pad + c.dim(L.tr("line    ", "Linie   ")) + "  " + c.txt(b) + c.dim(" " + std::string(g_term.glyph("→", "->")) + " ");
            std::snprintf(b, sizeof(b), "%.0f %% %s", airMs / beatMs * 100.0, L.tr("in use", "belegt"));
            ln += c.oper(b);
            std::snprintf(b, sizeof(b), " · %.0f ms %s", beatMs - airMs, L.tr("idle per chunk", "Leerlauf je Chunk"));
            out.push_back(ln + c.mut(b));
        }
    }

    // the last loss, as the pattern that diagnoses it: contiguous tail = full receiver, scattered = interference
    if (st.lastGapKind)
    {
        std::snprintf(b, sizeof(b), "%u %s %s", (unsigned)st.lastGapCount, L.tr("chunks", "Chunks"),
                      st.lastGapKind == 1 ? L.tr("in one tail (receiver was full)", "am Stück am Ende (Empfänger war voll)")
                                          : L.tr("scattered (line disturbance)", "verstreut (Störung auf der Linie)"));
        out.push_back(pad + c.dim(L.tr("last loss", "Verlust ")) + "  " + c.amber(b));
    }

    // what the window is buying: the report amortised over the chunks it covers
    if (st.reportMs && avgBps)
    {
        const double winMs = 1000.0 * st.window * setup.chunkSize / (double)avgBps;
        std::snprintf(b, sizeof(b), "%s %u ms / %u B", L.tr("report per window", "Report je Fenster"),
                      (unsigned)st.reportMs, (unsigned)(st.window * setup.chunkSize));
        std::string ln = pad + c.dim(L.tr("cost    ", "Kosten  ")) + "  " + c.txt(b) + c.dim(" = ");
        std::snprintf(b, sizeof(b), "%.0f %%", st.reportMs / (winMs + st.reportMs) * 100.0);
        out.push_back(ln + c.oper(b));
    }
    return out;
}

/**
 * @brief The rich "speed-test scope" block: progress line + 4-row dotted throughput curve + speed + time + legend.
 * @details Auto-zoomed to the data band so it reads as a curve, not a flat line; now/avg/peak (speed is the star)
 *          plus elapsed/ETA/expected-finish. Peak is a robust stat (spike-rejected).
 */
static std::vector<std::string> buildTransferDash(const FtcStatus& st, bool up, const std::vector<double>& hist,
                                                  const std::vector<uint8_t>& evt, uint32_t elapsedMs, uint32_t avgBps,
                                                  uint8_t retries, uint8_t retryMax)
{
    ftc::Theme& c = g_theme;
    ftc::I18n& Lz = g_i18n;
    ftc::Tpl& t = g_tpl;
    std::vector<std::string> L;
    const int W = graphWidth(12); // full window width (x GRAPH_WIDTH_PCT), resize-safe
    // live = a sliding window of the last W samples -> older ones scroll off the left, not compressed
    const size_t tail = (hist.size() > (size_t)W) ? (size_t)W : hist.size();
    const std::vector<double> lh(hist.end() - (long)tail, hist.end());
    const std::vector<uint8_t> le = (evt.size() >= tail) ? std::vector<uint8_t>(evt.end() - (long)tail, evt.end()) : evt;
    // 1) progress (+ a reconnect/auto-retry badge so a jump back to 0 % is explained, not a frozen bar)
    const uint16_t px = st.percentX100();
    char pct[16];
    std::snprintf(pct, sizeof(pct), "%u.%02u%%", px / 100, px % 100);
    const std::string bar = t.bar(st.total ? (double)st.done / (double)st.total : 0.0, 26, 'g');
    char pl[96];
    std::snprintf(pl, sizeof(pl), "seq %u/%u  ·  %s/%s KB", (unsigned)st.chunk, (unsigned)st.chunks, kbStr(st.done).c_str(), kbStr(st.total).c_str());
    std::string prog = std::string("  ") + c.cyan(g_term.glyph(up ? "▲" : "▼", up ? "^" : "v")) + " " + bar + "  " + c.bold(pct) + "   " + c.dim(pl);
    if (retries > 0)
    {
        char rb[48];
        std::snprintf(rb, sizeof(rb), " %s %s %u/%u", g_term.glyph("↻", "~"), Lz.tr("reconnect", "reconnect"), (unsigned)retries, (unsigned)retryMax);
        prog += "   " + c.amber(rb);
    }
    L.push_back(prog);
    // 2) dotted throughput scope (4 rows), auto-zoomed to the robust data band [lo,hi]; the y-axis shows the band
    double lo, hi;
    robustBand(lh, lo, hi);                          // auto-zoom to the visible window
    const uint32_t peak = (uint32_t)robustMax(hist); // peak stat stays the whole-run peak
    const std::vector<std::string> rows = scopeRows(lh, le, W, 4, lo, hi); // event lines rise through the curve
    for (size_t i = 0; i < rows.size(); ++i)
    {
        std::string ax = i == 0                   ? (std::to_string((uint32_t)hi) + " B/s")
                         : (i + 1 == rows.size()) ? std::to_string((uint32_t)lo)
                                                  : std::string(g_term.glyph("┊", ":"));
        L.push_back(std::string("   ") + rows[i] + "  " + c.dim(ax));
    }
    // 2b) marker lane aligned under the curve: confirm / resend / CRC-error
    L.push_back(std::string("   ") + markerLane(le, W));
    // 3) speed — the star: now (cyan) · avg (green) · peak (amber), bold
    char sn[16], sa[16], sp[16];
    std::snprintf(sn, sizeof(sn), "%u", (unsigned)st.bps);
    std::snprintf(sa, sizeof(sa), "%u", (unsigned)avgBps);
    std::snprintf(sp, sizeof(sp), "%u", (unsigned)peak);
    std::string speed = std::string("   ") + c.cyan(g_term.glyph("▸", ">")) + " " + c.bold(c.cyan(std::string(sn) + " B/s")) +
                        c.dim("    ") + Lz.tr("avg ", "Ø ") + c.bold(c.green(std::string(sa) + " B/s")) +
                        c.dim("    ") + Lz.tr("peak ", "Peak ") + c.bold(c.amber(std::string(sp) + " B/s"));
    if (st.window > 0) speed += winField(st, g_verbose);
    L.push_back(speed);
    // 4) time — elapsed · ETA · expected finish clock
    const uint32_t rem = st.total > st.done ? st.total - st.done : 0;
    const uint32_t eta = st.bps ? rem / st.bps : 0;
    const uint32_t es = elapsedMs / 1000;
    char tl[128];
    std::snprintf(tl, sizeof(tl), "%s %um%02us   ·   ETA %um%02us   ·   %s ~%s", g_term.glyph("⏱", "t"),
                  es / 60, es % 60, eta / 60, eta % 60, Lz.tr("done", "fertig"), finishClock(eta).c_str());
    L.push_back(std::string("   ") + c.dim(tl));
    // 5) live marker counts (CRC/ack · resend · CRC-error), same glyphs as the lane above
    L.push_back(std::string("   ") + markerCounts(st.verifies, st.resends, st.crcErrors));
    if (g_verbose)
        for (const std::string& r : regulationBlock(st, openknxFileTransferClient.transferSetup(), avgBps)) L.push_back(r);
    return L;
}

/**
 * @brief Is a second attempt worth offering, given how the transfer ended?
 * @details Only for causes a retry can clear (transient: dropped tunnel, target quiet, stall); a full
 *          target or a cancel is final. @p why receives the plain reason to show next to the question.
 */
static bool knxotaRetryable(const char* reason, std::string& why)
{
    ftc::I18n& L = g_i18n;
    if (reason == nullptr || !*reason) return false;
    struct Rule { const char* needle; bool retry; const char* en; const char* de; };
    static const Rule RULES[] = {
        {"full",        false, "the target has no room left",        "auf dem Ziel ist kein Platz mehr"},
        {"space",       false, "the target has no room left",        "auf dem Ziel ist kein Platz mehr"},
        {"cancel",      false, "you cancelled it",                   "du hast abgebrochen"},
        {"refused",     false, "the target refused the transfer",    "das Ziel hat die Übertragung abgelehnt"},
        {"source",      false, "the firmware file could not be read","die Firmware-Datei ist nicht lesbar"},
        {"cannot read", false, "the firmware file could not be read","die Firmware-Datei ist nicht lesbar"},
        {"too many",    false, "the file needs more chunks than the protocol allows",
                               "die Datei braucht mehr Chunks, als das Protokoll erlaubt"},
        {"no progress", false, "the same chunks kept failing",       "dieselben Chunks scheiterten immer wieder"},
        {"stall",       true,  "the transfer stalled",               "die Übertragung blieb stehen"},
        {"unanswered",  true,  "the target stopped answering",       "das Ziel antwortete nicht mehr"},
        {"no answer",   true,  "the target did not answer",          "das Ziel hat nicht geantwortet"},
        {"timeout",     true,  "it timed out",                       "es lief in eine Zeitüberschreitung"},
    };
    for (const Rule& r : RULES)
        if (std::strstr(reason, r.needle) != nullptr)
        {
            why = L.tr(r.en, r.de);
            return r.retry;
        }
    // Unknown cause: a transfer resumes where it stopped, so one more attempt costs little and may well work.
    why = reason;
    return true;
}

/**
 * @brief The transfer's own report, as plain label/value pairs.
 * @details One source for both the Result Panel and the log file, so the two can never tell different
 *          stories about the same run. Plain text on purpose: the log is read later, by someone without the
 *          screen, and colour codes and box characters only get in the way there.
 *          `verbose` adds the reasoning -- how the window got where it is, and whether WE were the brake.
 */
static std::vector<std::pair<std::string, std::string>> xferReportRows(const FtcTransferResult& r, bool verbose)
{
    ftc::I18n& L = g_i18n;
    const FtcStatus& st = openknxFileTransferClient.status();
    const FtcTransferSetup& setup = openknxFileTransferClient.transferSetup();
    std::vector<std::pair<std::string, std::string>> rows;
    char b[220];
    static const char* WS_DE[4] = {"tastet", "steht", "fest", "weicht aus"};
    static const char* WS_EN[4] = {"probing", "settled", "pinned", "backing off"};
    const uint8_t ws = st.windowState < 4 ? st.windowState : 0;

    static const char* DENIED_EN[6] = {"", "no CheckFeatures answer", "target has no fast", "chunk cap", "target refused", "not on this server version"};
    static const char* DENIED_DE[6] = {"", "keine CheckFeatures-Antwort", "Ziel kann kein fast", "Chunk-Grenze", "Ziel lehnte ab", "gibt es auf dieser Server-Version nicht"};
    if (r.mode == 1)
        std::snprintf(b, sizeof(b), "fast · %s %u (%s)", L.tr("window", "Fenster"), (unsigned)st.window,
                      L.tr(WS_EN[ws], WS_DE[ws]));
    else if (setup.fastDenied && setup.fastDenied < 6)
        std::snprintf(b, sizeof(b), "safe · %s: %s", L.tr("fast refused", "fast abgelehnt"),
                      L.tr(DENIED_EN[setup.fastDenied], DENIED_DE[setup.fastDenied]));
    else
        std::snprintf(b, sizeof(b), "safe · %s", L.tr("one CRC per chunk", "CRC je Chunk"));
    rows.emplace_back(L.tr("Mode", "Modus"), b);

    // A run that died before the framing was agreed has nothing to report there -- "pkg 8" would be invented.
    if (r.chunkSize) {
    std::snprintf(b, sizeof(b), "pkg %u (%s %u · %s %u) %s %u B/chunk", (unsigned)(r.chunkSize + 8),
                  L.tr("target", "Ziel"), (unsigned)setup.targetApdu, L.tr("interface", "Interface"),
                  (unsigned)g_ifaceApdu, g_term.glyph("→", "->"), (unsigned)r.chunkSize);
    rows.emplace_back(L.tr("Framing", "Rahmen"), b);
    }

    // How much of the TP line the run used. Well below 100 % means the ceiling is the path's turnaround.
    if (r.avgBps && r.chunkSize)
    {
        const double airMs = (r.chunkSize + 16.0) * 11.0 / 9.6;
        const double beatMs = 1000.0 * r.chunkSize / (double)r.avgBps;
        if (beatMs > airMs)
        {
            std::snprintf(b, sizeof(b), "%s %.0f ms · %s %.0f ms %s %.0f %% %s", L.tr("frame", "Rahmen"), airMs,
                          L.tr("beat", "Takt"), beatMs, g_term.glyph("→", "->"), airMs / beatMs * 100.0,
                          L.tr("in use", "belegt"));
            rows.emplace_back(L.tr("Line", "Linie"), b);
        }
    }

    std::snprintf(b, sizeof(b), "%u %s · %u %s · %u %s", (unsigned)st.verifies, L.tr("ack", "ack"),
                  (unsigned)st.resends, L.tr("resend", "Resend"), (unsigned)st.crcErrors,
                  L.tr("CRC errors", "CRC-Fehler"));
    rows.emplace_back(L.tr("Markers", "Marker"), b);

    if (!verbose || r.mode != 1) return rows;

    std::snprintf(b, sizeof(b), "%u %s · %s %u ms", (unsigned)st.windowClean,
                  L.tr("clean windows in a row", "saubere Fenster in Folge"), L.tr("report", "Report"),
                  (unsigned)st.reportMs);
    rows.emplace_back(L.tr("Control", "Regelung"), b);

    const uint32_t pace = g_knxTunnel.paceBps();
    const bool binds = pace && r.avgBps && pace < r.avgBps + r.avgBps / 10u;
    std::snprintf(b, sizeof(b), "Pacer %u B/s · %s", (unsigned)pace,
                  binds ? L.tr("binds (we are the brake)", "bremst (wir sind die Bremse)")
                        : L.tr("does not bind (the path is the ceiling)", "bindet nicht (die Strecke ist die Decke)"));
    rows.emplace_back(L.tr("Rate", "Tempo"), b);
    return rows;
}

/**
 * @brief Write the full report into the session log, whatever the console was told to show.
 * @details The log exists to be analysed after the fact, so it always gets the verbose set -- a run recorded
 *          with -q must still be readable months later. Plain text, no ANSI, one fact per line.
 */
static void logXferReport(const FtcTransferResult& r)
{
    if (!g_logFp || !r.bytes) return;
    std::fprintf(g_logFp, "\n--- report ---\n");
    for (const auto& kv : xferReportRows(r, true))
        std::fprintf(g_logFp, "%-10s %s\n", kv.first.c_str(), kv.second.c_str());
    const uint32_t secs = r.elapsedMs / 1000;
    std::fprintf(g_logFp, "%-10s %u B in %um%02us = %u B/s\n", g_i18n.tr("Result", "Ergebnis"),
                 (unsigned)r.bytes, (unsigned)(secs / 60), (unsigned)(secs % 60), (unsigned)r.avgBps);
    std::fflush(g_logFp);
}

/**
 * @brief The result Panel — drawn once at the end from transferResult(). Mirrors the SM box, on the template.
 */
static void renderXferResult(const FtcTransferResult& r, uint32_t peakOverride = 0)
{
    ftc::Theme& c = g_theme;
    ftc::I18n& L = g_i18n;
    ftc::Tpl& t = g_tpl;
    auto pa = [&](uint16_t a) { char b[16]; std::snprintf(b, sizeof(b), "%u.%u.%u", (a >> 12) & 0xF, (a >> 8) & 0xF, a & 0xFF); return std::string(b); };
    const char* what = r.kind == FtcXferKind::Perf ? "SPEED TEST" : r.kind == FtcXferKind::Download ? "DOWNLOAD"
                                                                                                    : "UPLOAD";
    std::string head = t.chip(std::string(what) + (r.ok ? " ✓" : " ✗"), r.ok ? 'g' : 'r') + "  " +
                       c.dim(std::string(r.kind == FtcXferKind::Download ? "← " : "→ ") + pa(r.target) +
                             "  ·  " + xferModeName(r.mode));
    ftc::Panel p(t, L.tr("Result", "Ergebnis"), "");
    p.line(head);
    p.sep();
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%u  (%u chunks × %u B)", (unsigned)r.bytes, (unsigned)r.chunks, (unsigned)r.chunkSize);
    p.kv(L.tr("Bytes", "Bytes"), c.bold(buf));
    const uint32_t secs = r.elapsedMs / 1000;
    const uint32_t peakShown = peakOverride ? peakOverride : r.peakBps; // robust CLI peak overrides the SM spike
    std::snprintf(buf, sizeof(buf), "%um%02us   ·   avg %u B/s   ·   peak %u", (unsigned)(secs / 60), (unsigned)(secs % 60), (unsigned)r.avgBps, (unsigned)peakShown);
    p.kv(L.tr("Time", "Zeit"), c.txt(buf));
    const char* vf = r.verify == 1 ? L.tr("verified OK", "verifiziert OK")
                     : r.verify == 2 ? L.tr("MISMATCH", "CRC-FEHLER")
                     : r.verify == 3 ? L.tr("size OK, not verified (SD/EFC)", "Größe OK, nicht verifiziert (SD/EFC)")
                                     : L.tr("unverified (no answer)", "nicht verifiziert (keine Antwort)");
    std::snprintf(buf, sizeof(buf), "0x%08X", (unsigned)r.crc);
    p.kv("CRC32", c.txt(buf) + "   " + (r.verify == 1 ? c.green(vf) : r.verify == 2 ? c.red(vf)
                                                                                    : c.dim(vf)));
    for (const auto& kv : xferReportRows(r, g_verbose)) p.kv(kv.first, c.txt(kv.second));
    if (r.retries)
    {
        const uint32_t ls = r.retryLostMs / 1000;
        std::snprintf(buf, sizeof(buf), "%u attempt%s   ·   %um%02us recovery", (unsigned)r.retries, r.retries == 1 ? "" : "s", (unsigned)(ls / 60), (unsigned)(ls % 60));
        p.kv(L.tr("Retry", "Retry"), c.amber(buf));
    }
    if (r.resumedBytes)
    {
        std::snprintf(buf, sizeof(buf), "%u B already on the target", (unsigned)r.resumedBytes);
        p.kv(L.tr("Resumed", "Fortgesetzt"), c.dim(buf));
    }
    if (r.dupes)
    {
        std::snprintf(buf, sizeof(buf), "%u discarded (IP mirror)", (unsigned)r.dupes);
        p.kv("Dupes", c.dim(buf));
    }
    if (r.cleanup)
    {
        // A left-behind test file is a corpse the user must clean up -> RED; a clean remove/kept is dim.
        const char* cleanupTxt = r.cleanup == 1   ? L.tr("test file removed", "Testdatei entfernt")
                                 : r.cleanup == 2 ? L.tr("test file kept", "Testdatei behalten")
                                                  : L.tr("test file LEFT BEHIND — delete it", "Testdatei ZURÜCKGELASSEN — bitte löschen");
        p.kv(L.tr("Cleanup", "Aufräumen"), r.cleanup == 3 ? c.red(cleanupTxt) : c.dim(cleanupTxt));
    }
    p.render(0);
    std::fflush(stdout);
}

/**
 * @brief The FAILURE result Panel — drawn when a transfer aborts before a structured result exists.
 * @details Mirrors renderXferResult's box, but sourced from the live setup+status so the user always sees
 *          what was reached: [UPLOAD ✗] + reason + progress (bytes/% and chunks) + time/avg/peak. The
 *          throughput curve is drawn separately below (renderXferRecapBare).
 */
static void renderXferFail(const FtcTransferSetup& setup, const FtcStatus& st, const char* reason,
                           uint32_t elapsedMs, uint32_t avgBps, uint32_t peakBps)
{
    ftc::Theme& c = g_theme;
    ftc::I18n& L = g_i18n;
    ftc::Tpl& t = g_tpl;
    auto pa = [&](uint16_t a) { char b[16]; std::snprintf(b, sizeof(b), "%u.%u.%u", (a >> 12) & 0xF, (a >> 8) & 0xF, a & 0xFF); return std::string(b); };
    const bool dl = setup.kind == FtcXferKind::Download;
    const char* what = setup.kind == FtcXferKind::Perf ? "SPEED TEST" : dl ? "DOWNLOAD" : "UPLOAD";
    const uint16_t target = st.target ? st.target : setup.target;
    std::string head = t.chip(std::string(what) + " ✗", 'r') + "  " +
                       c.dim(std::string(dl ? "← " : "→ ") + pa(target) + "  ·  " + xferModeName(setup.mode));
    ftc::Panel p(t, L.tr("Result", "Ergebnis"), "");
    p.line(head);
    p.sep();
    char buf[128];
    // Reason (the abort message the SM surfaced) -- red, it is the headline of a failure.
    p.kv(L.tr("Reason", "Grund"), c.red(reason && *reason ? reason : L.tr("transfer failed", "Transfer fehlgeschlagen")));
    // Progress: bytes done / total (+%), and chunks done / total -- so a mid-transfer abort shows how far it got.
    const uint32_t total = st.total ? st.total : setup.size;
    const uint32_t done = st.done;
    const double pct = total ? (double)done * 100.0 / (double)total : 0.0;
    const uint16_t chTot = st.chunks ? st.chunks : setup.chunks;
    std::snprintf(buf, sizeof(buf), "%u / %u B   (%.1f%%)   ·   %u / %u chunks",
                  (unsigned)done, (unsigned)total, pct, (unsigned)st.chunk, (unsigned)chTot);
    p.kv(L.tr("Progress", "Fortschritt"), c.txt(buf));
    const uint32_t secs = elapsedMs / 1000;
    std::snprintf(buf, sizeof(buf), "%um%02us   ·   avg %u B/s   ·   peak %u",
                  (unsigned)(secs / 60), (unsigned)(secs % 60), (unsigned)avgBps, (unsigned)peakBps);
    p.kv(L.tr("Time", "Zeit"), c.txt(buf));
    // Markers: how many CRC/acks landed, resends fired, CRC-errors seen before the abort.
    p.kv(L.tr("Markers", "Marker"), markerCounts(st.verifies, st.resends, st.crcErrors));
    p.render(0);
    std::fflush(stdout);
}

/**
 * @brief Clean end-of-run recap: the whole-run throughput as a wide dotted curve (speed focus), under the result.
 */
static void renderXferRecap(const std::vector<double>& hist, const std::vector<uint8_t>& evt,
                            const FtcTransferResult& r, uint32_t peakOverride = 0, uint16_t window = 0,
                            uint16_t acks = 0, uint16_t resends = 0, uint16_t crcErrors = 0)
{
    ftc::Theme& c = g_theme;
    const int W = graphWidth(8); // full window width (x GRAPH_WIDTH_PCT), resize-safe
    double lo, hi;
    robustBand(hist, lo, hi); // auto-zoom the recap curve the same way
    const uint32_t peak = peakOverride ? peakOverride : (uint32_t)robustMax(hist);
    const std::vector<std::string> rows = scopeRows(hist, evt, W, 3, lo, hi); // event lines rise through the curve
    std::printf("  %s\n", c.dim(g_i18n.tr("throughput over the run", "Durchsatz über den Lauf")).c_str());
    for (const auto& ln : rows)
        std::printf("   %s\n", ln.c_str());
    std::printf("   %s\n", markerLane(evt, W).c_str()); // where confirms / resends / CRC-errors landed
    std::printf("   %s\n", markerCounts(acks, resends, crcErrors).c_str());
    std::string stats = c.dim("avg ") + c.bold(c.green(std::to_string(r.avgBps) + " B/s")) +
                        c.dim("   ·   peak ") + c.bold(c.amber(std::to_string(peak) + " B/s"));
    if (window > 0) stats += c.dim("   ·   win ") + c.bold(c.amber(std::to_string(window)));
    std::printf("   %s\n\n", stats.c_str());
    std::fflush(stdout);
}

/**
 * @brief Recap curve WITHOUT a result struct — drawn after a FAILURE so the history (where it broke) stays visible.
 * @details avg is the true byte/time average (passed in); the marker lane still shows the last resend/CRC events.
 */
static void renderXferRecapBare(const std::vector<double>& hist, const std::vector<uint8_t>& evt, uint32_t avgBps, uint16_t window = 0,
                                uint16_t acks = 0, uint16_t resends = 0, uint16_t crcErrors = 0)
{
    ftc::Theme& c = g_theme;
    const int W = graphWidth(8); // full window width (x GRAPH_WIDTH_PCT), resize-safe
    double lo, hi;
    robustBand(hist, lo, hi);
    const uint32_t peak = (uint32_t)robustMax(hist);
    // avg = the TRUE byte/time average (passed in, same as the live Ø). NOT the mean of the spiky per-sample
    // instantaneous rates -- that is dominated by the opening burst and reads far too high (e.g. 713 vs 376).
    const uint32_t avg = avgBps;
    const std::vector<std::string> rows = scopeRows(hist, evt, W, 3, lo, hi);
    std::printf("  %s\n", c.dim(g_i18n.tr("throughput up to the failure", "Durchsatz bis zum Fehler")).c_str());
    for (const auto& ln : rows)
        std::printf("   %s\n", ln.c_str());
    std::printf("   %s\n", markerLane(evt, W).c_str());
    std::printf("   %s\n", markerCounts(acks, resends, crcErrors).c_str());
    std::string stats = c.dim("avg ") + c.bold(c.green(std::to_string(avg) + " B/s")) +
                        c.dim("   ·   peak ") + c.bold(c.amber(std::to_string(peak) + " B/s"));
    if (window > 0) stats += c.dim("   ·   win ") + c.bold(c.amber(std::to_string(window)));
    std::printf("   %s\n\n", stats.c_str());
    std::fflush(stdout);
}

/**
 * @brief One-shot transfer with CLI-owned presentation (setup Panel, in-place ~1 Hz+ progress line, result Panel).
 * @details processCommand() must already be armed and g_ftcSuppress set true by the caller.
 */
// Absolute cap for a running transfer. The default suits the interactive commands; knxOTA raises it from
// the size it is about to send, because a fixed 30 minutes is shorter than a 2 MB image needs on this bus.
static uint64_t g_xferCapMs = 1800000;

static int runTransferPresenter()
{
    const uint64_t QUIET_MS = 1500, ABS_CAP_MS = g_xferCapMs; // knxOTA raises this from the image size
    const uint64_t t0 = nowMs();
    uint64_t lastActivity = t0, lastDraw = 0, lastSample = 0;
    uint32_t lastTx = knxTunnelActivity();
    FtcPhase lastPhase = openknxFileTransferClient.status().phase;
    uint32_t lastDone = openknxFileTransferClient.status().done;
    bool setupDrawn = false, plainLive = false, resumeShown = false;
    const bool rich = g_term.isTty();               // the multi-line scope only on a real terminal
    int blockRows = 0;                              // live-region height (cursor bookkeeping)
    std::vector<double> hist;                       // throughput samples (B/s) for the curve
    std::vector<uint8_t> evt;                       // per-sample event bits (1 confirm · 2 resend · 4 CRC-error)
    uint16_t lastVer = 0, lastRes = 0, lastErr = 0; // last-seen SM event counters (delta -> a marker this sample)
    uint32_t moveStartMs = 0, moveStartDone = 0;    // moving-average baseline (peak is a robust stat of hist)

    auto endLive = [&]() {
        if (rich) clearLiveBlock(blockRows);
        else if (plainLive)
        {
            std::fprintf(stderr, "\r\x1b[K");
            std::fflush(stderr);
            plainLive = false;
        }
    };

    for (;;)
    {
        if (g_abort)
        {
            endLive();
            return abortCleanly();
        }
        g_knxTunnel.pump();
        openknxFileTransferClient.loop(true);
        const FtcStatus& st = openknxFileTransferClient.status();
        const FtcTransferSetup& setup = openknxFileTransferClient.transferSetup();
        const bool up = setup.kind != FtcXferKind::Download;
        const uint64_t now = nowMs();

        // Wait for the fast probe to answer before drawing: the panel is printed once and stays in the
        // scrollback, so a header claiming fast over a classic transfer is a lie nobody can correct later.
        if (!setupDrawn && setup.valid && setup.modeSettled)
        {
            renderXferSetup(setup);
            setupDrawn = true;
        }

        // RESUME badge: FtcResumeInfo matched a partial on the target -> the transfer continues from there.
        // Printed once, right under the setup box, so a resume is never invisible (the box's "resume = auto"
        // is only the setting). resumedBytes flips >0 after the box is drawn (post FileInfo decision).
        if (setupDrawn && !resumeShown && setup.resumedBytes > 0)
        {
            char rb[80];
            std::snprintf(rb, sizeof(rb), "%s %u KB %s", g_i18n.tr("continuing from", "fortgesetzt bei"),
                          (unsigned)(setup.resumedBytes / 1024), g_i18n.tr("already on the target", "schon auf dem Ziel"));
            std::printf("  %s  %s\n", g_tpl.chip(g_i18n.tr("RESUME ACTIVE", "RESUME AKTIV"), 'o').c_str(),
                        g_theme.dim(rb).c_str());
            std::fflush(stdout);
            resumeShown = true;
        }

        if (st.phase == FtcPhase::Done || st.phase == FtcPhase::Failed)
        {
            for (int i = 0; i < 64; ++i)
            {
                g_knxTunnel.pump();
                openknxFileTransferClient.loop(true);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            endLive();
            const FtcTransferResult& r = openknxFileTransferClient.transferResult();
            const uint32_t cp = (uint32_t)robustMax(hist);
            // On failure use the TRUE byte/time average (same as the live Ø), not the spiky per-sample mean.
            const uint32_t elFail = moveStartMs ? (uint32_t)(now - moveStartMs) : 0;
            const uint32_t avgFail = (elFail && st.done > moveStartDone) ? (uint32_t)(((uint64_t)(st.done - moveStartDone) * 1000ULL) / elFail) : st.bps;
            if (r.valid) renderXferResult(r, cp);
            else if (st.phase == FtcPhase::Failed) // failed before a result -> a full Result box (reason + how far it got)
                renderXferFail(setup, st, st.message, elFail, avgFail, cp);
            // The throughput history stays visible whether we finished OR failed, so the user always sees what happened.
            if (hist.size() > 2)
            {
                if (r.valid) renderXferRecap(hist, evt, r, cp, st.window, st.verifies, st.resends, st.crcErrors);
                else
                    renderXferRecapBare(hist, evt, avgFail, st.window, st.verifies, st.resends, st.crcErrors);
            }
            return (st.phase == FtcPhase::Failed) ? 1 : 0;
        }

        // Sample throughput ~4 Hz for the curve + fold the SM event counters into a per-sample marker bit.
        if (setup.valid && st.total)
        {
            if (moveStartMs == 0 && st.done > 0)
            {
                moveStartMs = now;
                moveStartDone = st.done;
            }
            if (now - lastSample >= 250)
            {
                uint8_t e = 0;
                if (st.verifies != lastVer) e |= 1;
                if (st.resends != lastRes) e |= 2;
                if (st.crcErrors != lastErr) e |= 4;
                lastVer = st.verifies;
                lastRes = st.resends;
                lastErr = st.crcErrors;
                hist.push_back((double)st.bps);
                evt.push_back(e);
                if (hist.size() > 600)
                {
                    hist.erase(hist.begin());
                    evt.erase(evt.begin());
                }
                lastSample = now;
            }
        }

        // Redraw the live region ~10 Hz (or immediately on a byte change) once bytes start flowing.
        if (setup.valid && st.total && (st.done != lastDone || now - lastDraw > 100))
        {
            const uint32_t el = moveStartMs ? (uint32_t)(now - moveStartMs) : 0;
            const uint32_t avg = (el && st.done > moveStartDone) ? (uint32_t)(((uint64_t)(st.done - moveStartDone) * 1000ULL) / el) : st.bps;
            if (rich) drawLiveBlock(buildTransferDash(st, up, hist, evt, el, avg,
                                                      openknxFileTransferClient.transferRetries(),
                                                      openknxFileTransferClient.transferRetryMax()),
                                    blockRows);
            else
            {
                renderXferProgress(st, up);
                plainLive = true;
            }
            lastDraw = now;
        }

        const uint32_t tx = knxTunnelActivity();
        if (tx != lastTx || st.phase != lastPhase || st.done != lastDone || openknxFileTransferClient.isBusy())
        {
            lastTx = tx;
            lastPhase = st.phase;
            lastDone = st.done;
            lastActivity = now;
        }
        if (now - lastActivity > QUIET_MS)
        {
            endLive();
            return 0;
        }
        if (now - t0 > ABS_CAP_MS)
        {
            endLive();
            std::fprintf(stderr, "error: overall timeout — aborting.\n");
            openknxFileTransferClient.requestCancel();
            return 2;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

/**
 * @brief -q after a transfer: the run as data, one fact per line, key<TAB>value.
 * @details A measurement series is compared by machine, and a machine should not have to parse a dashboard.
 *          Everything here is a number someone would put in a table -- no prose, no colour, no glyphs. Printed
 *          only for a completed windowed/lockstep transfer, so `-q` on a read command stays silent as before.
 */
static void printXferFacts(const FtcStatus& st, const FtcTransferSetup& setup, uint32_t seconds, uint32_t bps)
{
    if (!setup.valid || !setup.size) return;
    static const char* WSTATE[4] = {"probing", "settled", "pinned", "backing_off"};
    std::printf("mode\t%s\n", setup.mode == 1 ? "fast" : "safe");
    std::printf("bytes\t%u\n", (unsigned)setup.size);
    std::printf("chunks\t%u\n", (unsigned)setup.chunks);
    std::printf("chunk_size\t%u\n", (unsigned)setup.chunkSize);
    std::printf("target_apdu\t%u\n", (unsigned)setup.targetApdu);
    if (setup.mode == 1)
    {
        std::printf("window\t%u\n", (unsigned)st.window);
        std::printf("window_state\t%s\n", WSTATE[st.windowState < 4 ? st.windowState : 0]);
        std::printf("report_ms\t%u\n", (unsigned)st.reportMs);
    }
    std::printf("resends\t%u\n", (unsigned)st.resends);
    std::printf("crc_errors\t%u\n", (unsigned)st.crcErrors);
    std::printf("seconds\t%u\n", (unsigned)seconds);
    std::printf("bps\t%u\n", (unsigned)bps);
    std::printf("ok\t%u\n", st.ok ? 1u : 0u);
    std::fflush(stdout);
}

/**
 * @brief Drive the cooperative loop until the client reaches a terminal phase or goes QUIET; 0 done / 1 failed / 2 timeout.
 * @details Read-chain commands (info/df/scan) never set isBusy() or a terminal phase, so "finished" = no TX frame,
 *          no phase/progress change, not busy for QUIET_MS. Shared by the one-shot path and the --verbose probe.
 */
/**
 * @brief Watch a firmware install to its end and say what happened.
 * @details FwUpdate itself answers nothing -- by design, the device restarts. FwProbe with an empty
 *          payload is the status form: busy, failed-with-a-reason, or nothing. It is the SAME question
 *          for a patch and for a whole image, so this is the same code for both.
 * @return false when the device reported a failure (and said why).
 */
static bool watchFirmwareInstall(uint16_t pa, uint32_t patienceMs = 180000);

static int runOneShotToQuiescence()
{
    const uint64_t QUIET_MS = 1500;
    const uint64_t ABS_CAP_MS = 1800000; // read chains are short; the transfer cap does not belong here
    const uint64_t t0 = nowMs();
    uint64_t lastActivity = t0;
    uint32_t lastTx = knxTunnelActivity();
    FtcPhase lastPhase = openknxFileTransferClient.status().phase;
    uint32_t lastDone = openknxFileTransferClient.status().done;
    for (;;)
    {
        if (g_abort) return abortCleanly();
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
            if (g_quiet)
            {
                const uint32_t secs = (uint32_t)((nowMs() - t0) / 1000);
                printXferFacts(st, openknxFileTransferClient.transferSetup(), secs,
                               secs ? (uint32_t)(st.done / secs) : st.bps);
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

static bool watchFirmwareInstall(uint16_t pa, uint32_t patienceMs)
{
    ftc::I18n& L = g_i18n;
    const uint64_t t0 = nowMs();
    auto pump = []() { g_knxTunnel.pump(); openknxFileTransferClient.loop(true);
                       std::this_thread::sleep_for(std::chrono::milliseconds(2)); };
    bool sawWork = false;
    while (nowMs() - t0 < patienceMs && !g_abort)
    {
        uint32_t arg = 0;
        const ftc::JobState st = ftc::probeDeltaJob(g_knxTunnel, pa, arg, pump, []() { return nowMs(); });
        if (st == ftc::JobState::Running)
        {
            sawWork = true;
            char d[80];
            std::snprintf(d, sizeof(d), "%u %s", (unsigned)arg, L.tr("bytes written", "Bytes geschrieben"));
            g_tpl.waitTick(L.tr("the device is installing the firmware", "das Gerät spielt die Firmware ein"),
                           (uint32_t)((nowMs() - t0) / 1000), d);
            continue;
        }
        if (st == ftc::JobState::Failed)
        {
            std::printf("\r\x1b[K");
            g_ui.errorBlock(false,
                            L.tr("the device could not install the firmware",
                                 "das Gerät konnte die Firmware nicht einspielen"),
                            {ftc::deltaErrorText(L, arg),
                             L.tr("the old firmware keeps running - nothing was destroyed",
                                  "die alte Firmware läuft weiter - es wurde nichts zerstört")});
            return false;
        }
        break; // no job: either done and about to restart, or this device does not report one
    }
    if (sawWork) std::printf("\r\x1b[K");
    return true;
}


/**
 * @brief `oknx --ip <ip> info` — the full interface report (no PA, no tunnel); -q emits a plain key<TAB>value dump.
 * @details Everything the KNXnet/IP DESCRIPTION exposes (identity · network · all service families · capabilities)
 *          plus the device-management Max-APDU, rendered through the reusable Panel template.
 */
static int renderInterfaceInfo(const std::string& ip, uint16_t port, bool quiet)
{
    ftc::Theme& c = g_theme;
    ftc::I18n& L = g_i18n;
    ftc::Tpl& t = g_tpl;
    ftc::IfaceDesc d;
    if (!ftc::queryInterface(ip, port, d))
    {
        g_ui.errorBlock(false, L.tr("no DESCRIPTION_RESPONSE from ", "keine DESCRIPTION_RESPONSE von ") + ip,
                        {L.tr("not a KNXnet/IP interface, or no answer within 2 s",
                              "kein KNXnet/IP-Interface oder keine Antwort binnen 2 s")},
                        L.tr("check the IP with --discover", "IP mit --discover prüfen"));
        return 1;
    }
    // ONE device-management session pulls the full steckbrief (no bus traffic): identity, order/hardware,
    // KNX mask version, max APDU AND the tunnel additional individual addresses.
    ftc::InterfaceDetails det;
    ftc::queryInterfaceDetails(ip, port, det);
    d.apduReported = det.haveApdu ? det.maxApdu : 0;
    d.apduReportedPid = det.apduPid;
    if (!det.haveApdu)
        std::snprintf(d.apduReason, sizeof(d.apduReason), "%s", det.ok ? "device management: property n/a" : det.reason);
    // Prefer the device-management network values (raw property reads) over the DESCRIPTION IP-Current-Config
    // DIB: they are authoritative, work even before the interface advertises the 0x04 DIB, and — importantly —
    // the assignment method comes straight from PID_CURRENT_IP_ASSIGNMENT_METHOD, not the DIB's info1 byte
    // (the knx-lib KnxIpConfigDIB writes info1 to the wrong offset for the *current* config DIB).
    if (det.haveIp)
    {
        d.ip = det.ip;
        d.hasIp = true;
    }
    if (det.haveSubnet) d.subnet = det.subnet;
    if (det.haveGateway) d.gw = det.gateway;
    if (det.haveIpMethod) d.ipMethod = det.ipMethod;

    char buf[80];
    auto ia = [&](uint16_t a) { std::snprintf(buf, sizeof(buf), "%u.%u.%u", (a >> 12) & 0xF, (a >> 8) & 0xF, a & 0xFF); return std::string(buf); };
    auto ip4 = [&](uint32_t v) { std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (v >> 24) & 0xFF, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF); return std::string(buf); };
    auto pa = [&](uint16_t a) { std::snprintf(buf, sizeof(buf), "%u.%u.%u", (a >> 12) & 0xF, (a >> 8) & 0xF, a & 0xFF); return std::string(buf); };

    if (quiet) // plain, scriptable key<TAB>value dump
    {
        std::printf("ip\t%s\n", ip.c_str());
        std::printf("name\t%s\n", d.name);
        std::printf("ia\t%s\n", ia(d.ia).c_str());
        std::printf("medium\t%s\n", ftc::knxMediumName(d.medium));
        std::snprintf(buf, sizeof(buf), "%02X%02X%02X%02X%02X%02X", d.serial[0], d.serial[1], d.serial[2], d.serial[3], d.serial[4], d.serial[5]);
        std::printf("serial\t%s\n", buf);
        std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", d.mac[0], d.mac[1], d.mac[2], d.mac[3], d.mac[4], d.mac[5]);
        std::printf("mac\t%s\n", buf);
        std::printf("progmode\t%d\n", (d.status & 0x01) ? 1 : 0);
        if (d.hasExt) std::printf("mask\t0x%04X\n", d.mask);
        if (d.hasIp)
        {
            std::printf("ipaddr\t%s\n", ip4(d.ip).c_str());
            std::printf("subnet\t%s\n", ip4(d.subnet).c_str());
            std::printf("gateway\t%s\n", ip4(d.gw).c_str());
            std::printf("ipmethod\t%s\n", ipMethodName(d.ipMethod, d.haveIpMethod));
        }
        std::printf("services\t%s\n", famList(d).c_str());
        std::printf("routing\t%d\n", d.famVer[0x05] ? 1 : 0);
        if (d.hasExt && d.maxLocalApdu) std::printf("apdu_dib\t%u\n", d.maxLocalApdu);
        if (d.apduReported) std::printf("apdu_devmgmt\t%u\n", d.apduReported);
        // --- device management (M_PropRead; no bus traffic) ---
        if (det.haveMfr) std::printf("manufacturer\t0x%04X\n", det.manufacturer);
        if (det.haveOrder) std::printf("order\t%s\n", det.order);
        if (det.haveHardware)
        {
            std::string h;
            for (size_t i = 0; i < det.hardwareLen; ++i)
            {
                char x[4];
                std::snprintf(x, sizeof(x), "%02X", det.hardware[i]);
                h += x;
            }
            std::printf("hardware\t%s\n", h.c_str());
        }
        if (det.haveMask) std::printf("mask_devmgmt\t0x%04X\n", det.mask);
        if (det.haveFirmware) std::printf("firmware\t%u\n", det.firmware);
        if (det.haveFriendly) std::printf("friendly_devmgmt\t%s\n", det.friendly);
        if (det.haveProjId) std::printf("proj_install_id\t0x%04X\n", det.projInstId);
        for (size_t i = 0; i < det.tunnelAddrs.size(); ++i)
            std::printf("tunnel_addr\t%s\n", pa(det.tunnelAddrs[i]).c_str());
        return 0;
    }

    t.section(L.tr("KNX Interface · ", "KNX-Interface · ") + ip, -1);
    ftc::Panel p(t, L.tr("Interface", "Interface"), ip);
    // --- Identity ---
    p.kv(L.tr("Friendly name", "Name"), c.bold(d.name[0] ? d.name : L.tr("(unnamed)", "(ohne Namen)")));
    p.kv(L.tr("Individual IA", "Individual-Adr."), t.chip(ia(d.ia), 'c'));
    p.kv(L.tr("KNX medium", "KNX-Medium"), c.txt(ftc::knxMediumName(d.medium)));
    std::snprintf(buf, sizeof(buf), "%02X%02X:%02X %02X %02X %02X", d.serial[0], d.serial[1], d.serial[2], d.serial[3], d.serial[4], d.serial[5]);
    p.kv(L.tr("Serial", "Seriennr."), c.txt(buf));
    std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", d.mac[0], d.mac[1], d.mac[2], d.mac[3], d.mac[4], d.mac[5]);
    p.kv("MAC", c.txt(buf));
    p.kv(L.tr("Prog mode", "Prog-Modus"), (d.status & 0x01)
                                              ? t.chip("PROG", 'o')
                                              : c.mut(std::string(g_term.glyph("○", "o")) + " " + L.tr("off", "aus")));
    if (d.hasExt)
    {
        std::snprintf(buf, sizeof(buf), "0x%04X", d.mask);
        p.kv(L.tr("Mask / class", "Maske / Klasse"), c.txt(buf) + (d.mask == 0x07B0 ? c.dim("  · TP1 System B device") : std::string()));
    }
    // --- Network ---
    if (d.hasIp)
    {
        p.sep();
        p.kv(L.tr("IP address", "IP-Adresse"), c.txt(ip4(d.ip)));
        p.kv(L.tr("Subnet mask", "Subnetzmaske"), c.txt(ip4(d.subnet)));
        p.kv("Gateway", c.txt(ip4(d.gw)));
        p.kv(L.tr("IP assignment", "IP-Zuweisung"), c.txt(ipMethodName(d.ipMethod, d.haveIpMethod)));
    }
    p.kv(L.tr("Control port", "Steuer-Port"), c.txt(std::to_string((unsigned)port)));
    // --- Services (all advertised families + versions) ---
    p.sep();
    for (int id = 2; id <= 9; ++id)
        if (d.famVer[id])
        {
            const char* nm = svcFamilyName((uint8_t)id);
            if (!nm) continue;
            p.kv(nm, c.txt("v" + std::to_string(d.famVer[id])) + (id == 0x05 ? c.red("   · ROUTING (!)") : std::string()));
        }
    p.kv(L.tr("Routing", "Routing"), d.famVer[0x05]
                                         ? c.red(L.tr("advertised (!) — acts as a router", "beworben (!) — agiert als Router"))
                                         : c.green(L.tr("— not advertised (spec-conform interface)", "— nicht beworben (konformes Interface)")));
    // --- Capabilities ---
    p.sep();
    if (d.hasExt && d.maxLocalApdu)
    {
        std::snprintf(buf, sizeof(buf), "%u B", d.maxLocalApdu);
        p.kv(L.tr("Max APDU (DIB)", "Max APDU (DIB)"), c.txt(buf));
    }
    if (d.apduReported)
    {
        std::snprintf(buf, sizeof(buf), "%u B", d.apduReported);
        p.kv(L.tr("Max APDU (dev-mgmt)", "Max APDU (Dev-Mgmt)"),
             c.bold(buf) + c.dim(L.tr("  · via device management", "  · via Device-Mgmt")));
    }
    else if (d.apduReason[0])
        p.kv(L.tr("Max APDU (dev-mgmt)", "Max APDU (Dev-Mgmt)"), c.dim(std::string("n/a · ") + d.apduReason));

    // --- Device management (M_PropRead; local device management, zero bus traffic) -----------------
    if (det.ok)
    {
        p.sep();
        if (det.haveMfr)
        {
            std::snprintf(buf, sizeof(buf), "0x%04X (%u)", det.manufacturer, det.manufacturer);
            p.kv(L.tr("Manufacturer ID", "Hersteller-ID"), c.txt(buf));
        }
        if (det.haveOrder)
            p.kv(L.tr("Order info", "Bestellnr."), c.txt(det.order));
        if (det.haveHardware)
        {
            std::string h;
            for (size_t i = 0; i < det.hardwareLen; ++i)
            {
                char x[4];
                std::snprintf(x, sizeof(x), "%02X ", det.hardware[i]);
                h += x;
            }
            if (!h.empty() && h.back() == ' ') h.pop_back();
            p.kv(L.tr("Hardware type", "Hardware-Typ"), c.txt(h));
        }
        if (det.haveFirmware)
            p.kv(L.tr("Firmware rev.", "Firmware-Rev."), c.txt(std::to_string((unsigned)det.firmware)));
        if (det.haveMask)
        {
            const char* mn = ftc::knxMaskName(det.mask);
            std::snprintf(buf, sizeof(buf), "0x%04X", det.mask);
            p.kv(L.tr("KNX mask version", "KNX-Maskenversion"),
                 c.bold(buf) + (mn[0] ? c.dim(std::string("  · ") + mn) : std::string()));
        }
        if (det.haveFriendly && (!d.name[0] || std::strcmp(det.friendly, d.name) != 0))
            p.kv(L.tr("Friendly name (PID)", "Name (PID)"), c.txt(det.friendly));
        if (det.haveProjId)
        {
            std::snprintf(buf, sizeof(buf), "0x%04X", det.projInstId);
            p.kv(L.tr("Project install ID", "Projekt-Install-ID"), c.txt(buf));
        }
        // The tunnel additional individual addresses — the PAs this interface hands to tunnel clients. Probe
        // which are free right now (one short-lived tunnel per slot); colour is the status: cyan free, amber busy.
        if (!det.tunnelAddrs.empty())
        {
            p.sep();
            // Probing opens one short-lived tunnel per slot (blocking) -> animate on stderr so a piped stdout
            // panel stays clean and it never looks hung.
            TunnelProbe probe;
            if (g_term.isTty())
                runWithBusAnim(L.tr("probing tunnel slots …", "prüfe Tunnel-Slots …"), [&]() { probe = probeTunnelSlots(ip, port, (int)det.tunnelAddrs.size()); }, stderr);
            else
            {
                std::fprintf(stderr, "  %s\r", c.dim(L.tr("probing tunnel slots …", "prüfe Tunnel-Slots …")).c_str());
                std::fflush(stderr);
                probe = probeTunnelSlots(ip, port, (int)det.tunnelAddrs.size());
                std::fprintf(stderr, "\x1b[K");
                std::fflush(stderr);
            }
            const std::set<uint16_t>& freeSet = probe.freeSet;
            size_t nFree = 0;
            for (uint16_t a : det.tunnelAddrs)
                if (freeSet.count(a)) ++nFree;
            std::snprintf(buf, sizeof(buf), "%zu", det.tunnelAddrs.size());
            std::string head = c.dim(std::string(buf) + L.tr(" additional individual address(es)", " zusätzliche Individualadresse(n)"));
            char fb[32];
            std::snprintf(fb, sizeof(fb), "%zu/%zu", nFree, det.tunnelAddrs.size());
            head += c.dim("  · ") + c.cyan(fb) + c.dim(L.tr(" free", " frei"));
            if (!probe.conclusive)
                head += c.dim("  · ") + c.amber(L.tr("a probe got no answer", "eine Probe blieb unbeantwortet"));
            p.kv(L.tr("Tunnel addresses", "Tunnel-Adressen"), head);
            for (size_t i = 0; i < det.tunnelAddrs.size(); ++i)
            {
                const bool isFree = freeSet.count(det.tunnelAddrs[i]) != 0;
                std::snprintf(buf, sizeof(buf), "  #%zu", i + 1);
                // A slot we were not handed is only "busy" when every refusal was E_NO_MORE_CONNECTIONS.
                // If a probe went unanswered, this address is simply not known -- saying "busy" would
                // report a failed handshake as somebody else's connection.
                const char* lbl = isFree ? nullptr
                                         : (probe.conclusive ? L.tr("busy", "belegt") : L.tr("unknown", "unbekannt"));
                p.kv(buf, t.chip(pa(det.tunnelAddrs[i]), isFree ? 'c' : 'a') +
                              (lbl ? ("   " + c.amber(lbl)) : std::string()));
            }
            // The colour IS the status — one dim legend line makes it an explicit (indirect) description.
            p.kv("", c.dim(probe.conclusive
                               ? L.tr("cyan = free · amber = busy (in use)", "cyan = frei · orange = belegt (in Benutzung)")
                               : L.tr("cyan = free · amber = not handed out, and a probe went unanswered",
                                      "cyan = frei · orange = nicht zugeteilt, und eine Probe blieb unbeantwortet")));
        }
    }
    p.render(0);
    return 0;
}

/**********************************************************************
 ******************** STRUCTURED <PA> RENDERING *********************
 **********************************************************************/
// Stage 2 — render each <pa> read/status command from the client's STRUCTURED getters (no text scraping).
// The raw client text is suppressed (g_ftcSuppress); templates draw from deviceInfo()/fsInfo()/groupObjects()/
// listing()/status(). -q emits plain TSV; the interface info/monitor/progscan paths and all aliases are
// untouched (they never reach here — see the classifier below returning false).

/**
 * @brief KNX load-state code -> friendly name (host mirror of the client's ftcLoadName; 0xFF = not read).
 */
static const char* ftcLoadNameH(uint8_t s)
{
    ftc::I18n& L = g_i18n;
    switch (s)
    {
        case 0: return L.tr("unloaded", "nicht geladen");
        case 1: return L.tr("loaded", "geladen");
        case 2: return L.tr("loading", "wird geladen");
        case 3: return L.tr("error", "Fehler");
        case 4: return L.tr("unloading", "wird entladen");
        case 5: return L.tr("load completing", "Laden wird abgeschlossen");
        default: return "";
    }
}

/**
 * @brief Drive the cooperative loop to quiescence, snapshotting listing() (released on finish) for ll/ls/scan.
 * @details mergeMode = scan (entries stream in, then isOpenKnx is set in place -> merge by name); else replace-on-grow.
 */
/** @brief One frame of the live spinner, advancing on the wall clock so it turns at a steady rate. */
static const char* liveSpinner(uint64_t nowMsV)
{
    static const char* SPU[8] = {"⣾", "⣽", "⣻", "⢿", "⡿", "⣟", "⣯", "⣷"};
    static const char* SPA[8] = {"|", "/", "-", "\\", "|", "/", "-", "\\"};
    const int si = (int)((nowMsV / 80) % 8);
    return g_term.glyph(SPU[si], SPA[si]);
}

/**
 * @brief Draw a one-line live status in place.
 * @details On stderr, so it never mixes into piped output, and clipped to the CURRENT terminal width on
 *          every call — a wrapped line leaves fragments a carriage return cannot clear.
 */
static void liveLine(const std::string& body)
{
    std::fprintf(stderr, "\r%s\x1b[K", g_tpl.clip(body, ftc::Tpl::cols() - 1).c_str());
    std::fflush(stderr);
}

// True when the absolute safety cap ended the pump while the client was still working. Without this a
// capped run looked exactly like a finished one: the renderer printed whatever had arrived and called it
// complete - the very failure the group-comm "incomplete" marker exists to prevent.
static bool g_pumpCapped = false;

static void ftcPumpStructured(std::vector<FtcEntry>& snap, bool mergeMode, bool progress = false, bool longRun = false,
                              const std::function<void(const FtcEntry&)>* onNew = nullptr)
{
    auto absorb = [&]() {
        const std::vector<FtcEntry>& lst = openknxFileTransferClient.listing();
        if (lst.empty()) return;
        if (mergeMode)
            for (const auto& e : lst)
            {
                FtcEntry* d = nullptr;
                for (auto& s : snap)
                    if (std::strcmp(s.name, e.name) == 0)
                    {
                        d = &s;
                        break;
                    }
                if (!d)
                {
                    snap.push_back(e);
                    if (onNew) (*onNew)(e); // a freshly found address -- hand it on while the sweep runs
                }
                else if (e.isOpenKnx)
                    d->isOpenKnx = true;
            }
        else if (lst.size() >= snap.size())
            snap = lst;
    };
    // A group-comm walk is hundreds of round trips over TP - the device page itself says 1-3 minutes - so
    // 60 s was a cap on normal operation, not on a hang.
    const uint64_t QUIET_MS = 1500;
    const uint64_t ABS_CAP_MS = progress ? 600000 : (longRun ? 300000 : 60000);
    g_pumpCapped = false;
    uint64_t t0 = nowMs(), last = t0, lastRender = 0;
    uint32_t lastTx = knxTunnelActivity();
    bool started = false;
    for (;;)
    {
        if (g_abort)
        {
            abortCleanly();
            break;
        }
        g_knxTunnel.pump();
        openknxFileTransferClient.loop(true);
        absorb();
        const uint64_t now = nowMs();
        const uint32_t tx = knxTunnelActivity();
        const FtcPhase ph = openknxFileTransferClient.status().phase;
        if (tx != lastTx || openknxFileTransferClient.isBusy())
        {
            lastTx = tx;
            last = now;
            started = true;
        }
        if (progress && now - lastRender > 60) // live scan line: spinner + the PA being probed + found/probed counters
        {
            lastRender = now;
            const FtcStatus& st = openknxFileTransferClient.status();
            const uint16_t pa = openknxFileTransferClient.scanCurrentPa();
            char paS[16];
            std::snprintf(paS, sizeof(paS), "%u.%u.%u", (pa >> 12) & 0x0F, (pa >> 8) & 0x0F, pa & 0xFF);
            auto& c = g_tpl.theme();
            const std::string live = std::string("  ") + c.green(liveSpinner(now)) + " " +
                                     c.cyan(std::string(g_i18n.tr("scanning ", "suche ")) + paS) + "   " +
                                     c.bold(std::to_string(openknxFileTransferClient.scanFound()) +
                                            g_i18n.tr(" found", " gefunden")) + "   " +
                                     c.dim(std::to_string(st.done) + "/" + std::to_string(st.total) +
                                           g_i18n.tr(" probed", " gefragt"));
            // resize-safe: clip to the CURRENT terminal width (re-queried every render) so the line never wraps --
            // a wrapped line leaves fragments \r cannot clear (esp. when the window is shrunk mid-scan).
            liveLine(live);
        }
        if (started && (ph == FtcPhase::Done || ph == FtcPhase::Failed))
        {
            for (int i = 0; i < 48; ++i)
            {
                g_knxTunnel.pump();
                openknxFileTransferClient.loop(true);
                absorb();
            }
            break;
        }
        if (!started && now - t0 < 500)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        if (now - t0 > ABS_CAP_MS)
        {
            g_pumpCapped = openknxFileTransferClient.isBusy(); // cut short mid-work -> the result is a prefix
            break;
        }
        if (now - last > QUIET_MS) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (progress)
    {
        std::fprintf(stderr, "\r\x1b[K");
        std::fflush(stderr);
    } // clear the live line before the final report
}

/**********************************************************************
 ***************************** CHILD PROCESSES *************************
 **********************************************************************/
// Child processes over popen/_popen + std::thread for the readers. Used by the tunnel-slot probe and by
// the identity pool; the sweep itself stays serial on the one tunnel.
#ifdef _WIN32
    #define FTC_POPEN _popen
    #define FTC_PCLOSE _pclose
#else
    #define FTC_POPEN popen
    #define FTC_PCLOSE pclose
#endif

/**
 * @brief Format a raw PA word as "a.l.d".
 */
static std::string paToStr(uint16_t pa)
{
    char b[16];
    std::snprintf(b, sizeof(b), "%u.%u.%u", (pa >> 12) & 0x0F, (pa >> 8) & 0x0F, pa & 0xFF);
    return b;
}

/** @brief "a.l.d" -> the 16-bit address; 0 when it is not one (0.0.0 is not a device address). */
static uint16_t paFromText(const std::string& t)
{
    unsigned a = 0, l = 0, d = 0;
    if (std::sscanf(t.c_str(), "%u.%u.%u", &a, &l, &d) != 3) return 0;
    if (a > 15 || l > 15 || d > 255) return 0;
    return (uint16_t)((a << 12) | (l << 8) | d);
}

/**
 * @brief The list of unfinished runs, with a cursor: continue one, or throw entries away.
 * @details Continuing means running `oknx knxota <file>` again with the answers this entry already
 *          holds -- so it is printed rather than executed from in here, where the assistant, the
 *          access check and the transfer presenter are all out of reach. The point of this view is
 *          the other half: getting rid of entries, which was not possible at all before.
 * @return 0 -- it changes nothing on any bus.
 */
static std::string agoText(uint64_t when, ftc::I18n& L); // defined below, next to paToStr

static int runResumeManager(std::vector<ftc::OtaSession>& all, ftc::Theme& c, ftc::I18n& L)
{
    if (all.empty())
    {
        g_tpl.status(ftc::Tpl::Stat::Idle, L.tr("no unfinished runs", "keine angefangenen Läufe"), {});
        return 0;
    }

    ftc::Keys keys;
    const bool interactive = g_term.isTty() && keys.active();
    size_t sel = 0;
    int drawn = 0;

    auto draw = [&]() {
        if (interactive && drawn > 0) std::printf("\x1b[%dA\x1b[J", drawn);
        drawn = 0;
        auto line = [&](const std::string& t) { std::printf("%s\n", t.c_str()); ++drawn; };

        line("");
        line("  " + c.amber(L.tr("Unfinished runs", "Angefangene Läufe")));
        line("  " + c.dim(std::string("   #  ") +
                          L.tr("TARGET    INTERFACE      FIRMWARE                        GOT     WHEN",
                               "ZIEL      INTERFACE      FIRMWARE                        STAND   WANN")));
        for (size_t i = 0; i < all.size(); ++i)
        {
            const ftc::OtaSession& e = all[i];
            std::error_code ec;
            const bool gone = !std::filesystem::is_regular_file(e.file, ec);
            char pct[24];
            std::snprintf(pct, sizeof(pct), "%5.1f %%", e.total ? e.done * 100.0 / e.total : 0.0);
            const std::string name = std::filesystem::path(e.file).filename().string();
            // clip() only shortens; a column also needs the short ones padded, or the table drifts.
            auto col = [&](const std::string& v, int w) {
                std::string t = g_tpl.clip(v, w);
                for (int n = ftc::Tpl::dispw(t); n < w; ++n) t += ' ';
                return t;
            };
            std::string row = std::string(interactive && i == sel ? " > " : "   ") +
                              std::to_string(i + 1) + "  " + col(paToStr(e.pa), 9) + " " +
                              col(e.ip, 14) + " " + col(name, 30) + " " + pct + "  " +
                              col(agoText(e.when, L), 12);
            if (gone) row += L.tr("file missing", "Datei fehlt");
            if (interactive && i == sel) line("  " + c.sel(row));
            else
                line("  " + (gone ? c.dim(row) : c.txt(row)));
        }
        line("");
        if (interactive)
            line("   " + c.bold(g_term.glyph("↑↓", "up/dn")) + " " + c.dim(L.tr("move", "wählen")) +
                 "   " + c.bold(g_term.glyph("↵", "enter")) + " " + c.dim(L.tr("how to continue", "wie fortsetzen")) +
                 "   " + c.bold("x") + " " + c.dim(L.tr("remove", "entfernen")) +
                 "   " + c.bold("a") + " " + c.dim(L.tr("remove all", "alle entfernen")) +
                 "   " + c.bold("q") + " " + c.dim(L.tr("back", "zurück")));
        else
            line("   " + c.dim(L.tr("no keyboard here - use `oknx knxota resume clear <pa>|all`",
                                    "keine Tastatur hier - `oknx knxota resume clear <pa>|all` benutzen")));
        std::fflush(stdout);
    };

    if (!interactive) { draw(); return 0; }

    for (;;)
    {
        draw();
        const int k = keys.waitKey();
        if (k == 'q' || k == 'Q' || k == ftc::K_ESC) { std::printf("\n"); return 0; }
        if (k == ftc::K_UP) { if (sel > 0) --sel; continue; }
        if (k == ftc::K_DOWN) { if (sel + 1 < all.size()) ++sel; continue; }
        if (k == ftc::K_HOME) { sel = 0; continue; }
        if (k == ftc::K_END) { sel = all.size() - 1; continue; }
        if (k == 'x' || k == 'X')
        {
            const ftc::OtaSession victim = all[sel];
            ftc::otaResumeErase(otaResumePath(), victim);
            all.erase(all.begin() + (long)sel);
            if (all.empty()) { std::printf("\n"); g_tpl.status(ftc::Tpl::Stat::Ok, L.tr("all removed", "alle entfernt"), {}); return 0; }
            if (sel >= all.size()) sel = all.size() - 1;
            continue;
        }
        if (k == 'a' || k == 'A')
        {
            keys.restore();
            std::printf("\n");
            drawn = 0;
            if (ftc::confirm(g_term, c, L, L.tr("Remove every entry?", "Alle Einträge entfernen?")))
            {
                ftc::otaResumeEraseWhere(otaResumePath(), 0);
                all.clear();
                g_tpl.status(ftc::Tpl::Stat::Ok, L.tr("all removed", "alle entfernt"), {});
                return 0;
            }
            return 0; // the raw mode is gone; leaving beats a half-live view
        }
        if (k == ftc::K_ENTER)
        {
            const ftc::OtaSession& e = all[sel];
            keys.restore();
            std::printf("\n");
            g_tpl.panelTop(L.tr("Continue this run", "Diesen Lauf fortsetzen"), paToStr(e.pa));
            g_tpl.kv(L.tr("Firmware", "Firmware"), c.txt(e.file));
            g_tpl.kv(L.tr("Interface", "Interface"), c.txt(e.ip + ":" + std::to_string((unsigned)e.port)));
            g_tpl.panelEnd();
            std::error_code ec;
            if (!std::filesystem::is_regular_file(e.file, ec))
                g_tpl.status(ftc::Tpl::Stat::Warn,
                             L.tr("that file is not there any more", "diese Datei gibt es nicht mehr"),
                             {L.tr("point at the same release again, or remove the entry",
                                   "auf dasselbe Release erneut zeigen, oder den Eintrag entfernen")});
            else
                g_tpl.note(std::string("oknx --ip ") + e.ip + " " + paToStr(e.pa) + " knxota " + e.file);
            return 0;
        }
    }
}

/** @brief "vor 2 Std" / "gestern" -- an age a human reads without doing arithmetic. */
static std::string agoText(uint64_t when, ftc::I18n& L)
{
    if (when == 0) return "—";
    const uint64_t now = (uint64_t)std::time(nullptr);
    if (now <= when) return L.tr("just now", "gerade eben");
    const uint64_t s = now - when;
    char b[64];
    if (s < 3600) { std::snprintf(b, sizeof(b), "%s %u %s", L.tr("", "vor"), (unsigned)(s / 60), L.tr("min ago", "Min")); return b; }
    if (s < 86400) { std::snprintf(b, sizeof(b), "%s %u %s", L.tr("", "vor"), (unsigned)(s / 3600), L.tr("h ago", "Std")); return b; }
    if (s < 172800) return L.tr("yesterday", "gestern");
    std::snprintf(b, sizeof(b), "%s %u %s", L.tr("", "vor"), (unsigned)(s / 86400), L.tr("days ago", "Tagen"));
    return b;
}

/**
 * @brief Parse the `scan` target tokens into an inclusive PA range [start,end]; false = fall back to serial scan.
 * @details Supports "a.l" (whole line) and "a.l.d [a.l.d]" (single / from-to). Returns false for area/full/unparseable.
 */
static bool parseScanRange(const std::vector<std::string>& pos, uint16_t& start, uint16_t& end)
{
    std::vector<std::string> addrs;
    for (size_t i = 1; i < pos.size(); ++i)
    {
        const std::string& t = pos[i];
        if (t == "ets" || t == "deep" || t == "openknx" || t == "info" || t == "save" || t == "full" || t == "area" || t == "yes")
            continue; // keyword, not an address (a bare number after deep/area is a keyword arg -> also skipped below)
        if (t.find('.') != std::string::npos) addrs.push_back(t);
    }
    if (addrs.empty()) return false;
    unsigned a = 0, l = 0, d = 0;
    const int f = std::sscanf(addrs[0].c_str(), "%u.%u.%u", &a, &l, &d);
    if (f == 2 && a <= 15 && l <= 15) // line a.l -> a.l.1 .. a.l.255
    {
        const uint16_t base = (uint16_t)((a << 12) | (l << 8));
        start = (uint16_t)(base | 0x01);
        end = (uint16_t)(base | 0xFF);
        return true;
    }
    if (f == 3 && a <= 15 && l <= 15 && d <= 255)
    {
        start = (uint16_t)((a << 12) | (l << 8) | d);
        end = start;
        if (addrs.size() >= 2)
        {
            unsigned a2 = 0, l2 = 0, d2 = 0;
            if (std::sscanf(addrs[1].c_str(), "%u.%u.%u", &a2, &l2, &d2) != 3 || a2 > 15 || l2 > 15 || d2 > 255) return false;
            end = (uint16_t)((a2 << 12) | (l2 << 8) | d2);
        }
        return end >= start;
    }
    return false;
}


/**
 * @brief Probe which tunnel additional-addresses are currently free (union of reported PAs = free; rest = busy).
 * @details Spawns one short-lived probe child per slot; each opens a tunnel and reports the free PA it is handed,
 *          holding it briefly so siblings get *distinct* free slots.
 */
static TunnelProbe probeTunnelSlots(const std::string& ip, uint16_t port, int slots)
{
    TunnelProbe out;
    std::set<uint16_t>& freeSet = out.freeSet;
    if (slots < 1 || ip.empty()) return out;
    if (slots > 16) slots = 16; // a KNXnet/IP interface tops out at 16 tunnel connections
    std::mutex mtx;
    std::vector<std::thread> threads;
    for (int i = 0; i < slots; ++i)
    {
        // Stagger the launches: an embedded interface can't service N simultaneous CONNECT_REQUESTs -- fired all
        // at once, several handshakes are dropped and their (actually FREE) slots get mis-reported as busy. A
        // small gap lets each connect land + get a distinct additional address, while all stay held open so the
        // set of assigned addresses = every free slot. The child hold time below covers the full stagger span.
        if (i > 0) std::this_thread::sleep_for(std::chrono::milliseconds(60));
        threads.emplace_back([ip, port, &out, &freeSet, &mtx]() {
            std::string cmd = "\"" + g_selfPath + "\" --_probeslot -i " + ip + " --port " + std::to_string(port);
#ifdef _WIN32
            cmd += " 2>NUL";
#else
            cmd += " 2>/dev/null";
#endif
            FILE* f = FTC_POPEN(cmd.c_str(), "r");
            if (!f)
            {
                std::lock_guard<std::mutex> lk(mtx);
                out.conclusive = false;
                return;
            }
            bool sawLine = false;
            char line[128];
            while (std::fgets(line, sizeof(line), f))
            {
                unsigned a = 0, b = 0, c = 0;
                if (std::sscanf(line, "SLOT\t%u.%u.%u", &a, &b, &c) == 3)
                {
                    std::lock_guard<std::mutex> lk(mtx);
                    freeSet.insert((uint16_t)(((a & 0x0F) << 12) | ((b & 0x0F) << 8) | (c & 0xFF)));
                    sawLine = true;
                }
                int st = 0;
                if (std::sscanf(line, "NOSLOT\t%d", &st) == 1 && st != 0x24)
                {
                    // Only E_NO_MORE_CONNECTIONS means "the interface has nothing left". A timeout or any
                    // other status says the probe failed, which is not the same as the slot being taken.
                    std::lock_guard<std::mutex> lk(mtx);
                    out.conclusive = false;
                }
                if (st == 0x24) sawLine = true;
            }
            // A child that said nothing at all -- could not be spawned, could not run, died -- is not
            // evidence that the slot is taken. Its EXIT code is no signal: a refused slot exits 3 by design.
            FTC_PCLOSE(f);
            if (!sawLine)
            {
                std::lock_guard<std::mutex> lk(mtx);
                out.conclusive = false;
            }
        });
    }
    for (auto& t : threads)
        if (t.joinable()) t.join();
    return out;
}

/**
 * @brief Per-class summary footer for a scan result (total + how many of each device class).
 */
/**
 * @brief Report a target that stayed silent, and offer the line difference only when there is one.
 * @details Whether a coupler forwards to another line is not knowable from here, so the difference is
 *          never a warning on its own -- a device that answers is never questioned, however far away it
 *          lives. It is named here, after the silence has been measured, because it is by far the most
 *          common reason for it.
 * @param detail an extra first line (e.g. how many attempts were made), or nullptr
 */
static void reportUnreachable(const std::string& paText, uint16_t targetPa, const char* detail)
{
    ftc::I18n& L = g_i18n;
    const uint16_t own = g_knxTunnel.assignedPA();
    const bool otherLine = ((own ^ targetPa) & 0xFF00) != 0;
    char ownLine[16];
    std::snprintf(ownLine, sizeof(ownLine), "%u.%u", (unsigned)((own >> 12) & 0x0F), (unsigned)((own >> 8) & 0x0F));

    std::vector<std::string> why;
    if (detail && *detail) why.push_back(detail);
    if (otherLine)
        why.push_back(std::string(L.tr("this interface/router sits on line ", "dieses Interface/dieser Router sitzt auf Linie ")) +
                      ownLine + L.tr(", the device on another one — a coupler would have to pass it through",
                                     ", das Gerät auf einer anderen — ein Koppler müsste das durchreichen"));
    std::fflush(stdout);
    g_ui.errorBlock(false, paText + L.tr(" did not answer", " hat nicht geantwortet"),
                    {why.empty() ? std::string() : why[0], why.size() > 1 ? why[1] : std::string()},
                    otherLine ? L.tr("try an interface on the device's own line, or check the coupler's filter table",
                                     "ein Interface auf der Linie des Geräts nehmen, oder die Filtertabelle des "
                                     "Kopplers prüfen")
                              : L.tr("check the address, and that the device is powered and on the bus",
                                     "Adresse prüfen, und ob das Gerät Spannung hat und am Bus hängt"));
}

/**
 * @brief Arm the acknowledgement feed for a sweep that stays inside one line.
 * @details The sweep already sends the frame a presence test needs; this only routes its acknowledgement
 *          into the scan machine. Returns false when the interface confirms everything it accepts — its
 *          acknowledgement would then report every address as present, so it must not be trusted.
 */
static bool armScanAckFeed(uint16_t lineBase)
{
    if (!g_knxTunnel.connected()) return false;
    ftc::FastScanDeps fd;
    fd.pump = []() { g_knxTunnel.pump(); };
    fd.nowMs = []() { return nowMs(); };
    fd.aborted = []() { return g_abort != 0; };
    if (!ftc::interfaceReportsAcks(g_knxTunnel, lineBase, fd)) return false;
    g_knxTunnel.setConfirmCallback(
        [](uint16_t pa, bool ok) { openknxFileTransferClient.ftcScanAck(pa, ok); });
    return true;
}

/**
 * @brief One KNXnet/IP device on the network and the individual addresses it owns.
 * @details `ia` comes from the search answer's Device Information DIB, `tun` from a device-management
 *          connection (which costs no tunnel slot). `refused` = answered the search but not the
 *          management connection, so its block stays unknown.
 */
struct IfaceOwner
{
    uint16_t ia = 0;
    std::string ip, name;
    std::vector<uint16_t> tun;
    bool refused = false;
};

/**
 * @brief Ask the network who owns which address: search, then read each answerer's additional addresses.
 * @details This is what turns a tunnel slot in the hit list from "acknowledged only" into "tunnel 3 of
 *          1.1.11". Only devices reachable by multicast appear; anything else stays unknown by design.
 */
static std::vector<IfaceOwner> buildOwnerMap(uint16_t port)
{
    std::vector<IfaceOwner> out;
    // Our own tunnel is already open and its keep-alive runs only from pump() (HEARTBEAT_MS 60 s).
    // Discovery plus one management query per device blocks ~1.5 s + 1.2 s each, so on an installation
    // with many KNXnet/IP devices the window would outlast the heartbeat and the interface would drop us.
    auto breathe = [] { if (g_knxTunnel.connected()) g_knxTunnel.pump(); };
    breathe();
    auto ifaces = ftc::discoverInterfaces(port, 1500);
    breathe();
    ftc::sortInterfaces(ifaces);
    for (const auto& f : ifaces)
    {
        breathe();
        IfaceOwner o;
        o.ia = f.ia;
        o.ip = f.ip;
        o.name = f.name;
        ftc::InterfaceDetails d;
        if (ftc::queryInterfaceDetails(f.ip, port, d, 1200))
            o.tun = d.tunnelAddrs;
        else
            o.refused = true; // answered the search, refused the management connection
        out.push_back(o);
    }
    return out;
}

/** @brief The "who is on this network" block: one row per KNXnet/IP device, with the block it owns. */
static void renderOwnerMap(const std::vector<IfaceOwner>& map)
{
    ftc::Theme& c = g_theme;
    ftc::I18n& L = g_i18n;
    ftc::Tpl& t = g_tpl;
    char hdr[64];
    std::snprintf(hdr, sizeof(hdr), "%s \xC2\xB7 %u %s", L.tr("KNXnet/IP devices", "KNXnet/IP-Geräte im Netz"),
                  (unsigned)map.size(), L.tr("found", "gefunden"));
    t.section(hdr);
    // 11, not 9: the widest address is 15.15.200 (9 columns) and the no-colour chip adds its brackets.
    // At 9 that row overflowed its cell and pushed the whole line two columns right.
    const std::vector<int> w = {11, 30, 15, 0};
    for (const auto& o : map)
    {
        char blk[48] = {0};
        if (o.refused)
            std::snprintf(blk, sizeof(blk), "%s", L.tr("refused unencrypted -- block unknown",
                                                       "verweigert unverschlüsselt -- Block unbekannt"));
        else if (o.tun.empty())
            std::snprintf(blk, sizeof(blk), "%s", L.tr("no additional addresses", "keine Zusatzadressen"));
        else
        {
            // Lowest to highest, not first to last: the device returns them in its own order, which is
            // not sorted (1.1.11 hands back 1.1.233 first). And a pool is not always contiguous -- the
            // router's own list has gaps -- so a span is marked when it is one and counted when it is not.
            auto mm = std::minmax_element(o.tun.begin(), o.tun.end());
            const bool dense = (unsigned)(*mm.second - *mm.first + 1) == (unsigned)o.tun.size();
            if (o.tun.size() == 1)
                std::snprintf(blk, sizeof(blk), "1 %s  %s", L.tr("additional", "Zusatz"), paToStr(*mm.first).c_str());
            else
                std::snprintf(blk, sizeof(blk), "%u %s  %s%s%s", (unsigned)o.tun.size(), L.tr("additional", "Zusatz"),
                              paToStr(*mm.first).c_str(), dense ? "\xE2\x80\xA6" : " .. ", paToStr(*mm.second).c_str());
        }
        t.tableRow({o.ia ? t.chip(paToStr(o.ia), 'c') : c.red("?"), c.txt(o.name), c.dim(o.ip),
                    o.refused ? c.red(blk) : c.dim(blk)}, w);
    }
    t.note(L.tr("own address from the search answer, additional addresses over device management -- no tunnel slot",
                "eigene PA aus der Suchantwort, Zusatzadressen über Device-Management — belegt keinen Tunnelplatz"));
}

/** @brief Which interface owns @p pa as a tunnel slot, and as which slot number. Empty name = nobody. */
static const IfaceOwner* ownerOf(const std::vector<IfaceOwner>& map, uint16_t pa, int& slot)
{
    for (const auto& o : map)
        for (size_t i = 0; i < o.tun.size(); ++i)
            if (o.tun[i] == pa)
            {
                slot = (int)(i + 1);
                return &o;
            }
    slot = 0;
    return nullptr;
}

static void renderScanSummary(const std::vector<FtcEntry>& devices,
                              const std::unordered_map<std::string, ftc::DetailRow>* det,
                              const std::vector<IfaceOwner>& owners);

/**
 * @brief The mask to show for a scanned device: what it said about itself beats what the sweep saw.
 * @details The sweep's descriptor read is connectionless, which a BCU1/BCU2/BIM M112 and some System B
 *          devices never answer; the detail child asks over a connection and gets one. Without this the
 *          table printed "acknowledged only" next to an order number read from that same device.
 */
static uint16_t scanMask(const FtcEntry& e, const std::unordered_map<std::string, ftc::DetailRow>* det)
{
    if (det)
    {
        const auto it = det->find(e.name);
        if (it != det->end() && it->second.mask) return it->second.mask;
    }
    return (uint16_t)e.crc;
}

/**
 * @brief The scan result table, wide when identities were read and narrow when they were not.
 * @details One renderer for the decorated run and the knxOTA picker, so a column cannot exist in one
 *          and be missing in the other.
 */
static void renderScanTable(const std::vector<FtcEntry>& devices,
                            const std::unordered_map<std::string, ftc::DetailRow>& det,
                            const std::vector<IfaceOwner>& owners = {})
{
    ftc::Theme& c = g_theme;
    ftc::I18n& L = g_i18n;
    ftc::Tpl& t = g_tpl;
    const bool cols = !det.empty();
    auto fmtSerial = [](const std::string& hex) {
        // The device answers 12 hex digits: manufacturer, then the serial itself.
        if (hex.size() != 12) return hex;
        return hex.substr(0, 4) + ":" + hex.substr(4);
    };
    if (cols)
    {
        const std::vector<int> w = {2, 9, 20, 18, 9, 8, 5, 8, 0};
        t.tableRow({c.dim("ST"), c.dim("PA"), c.dim(L.tr("CLASS", "KLASSE")),
                    c.dim(L.tr("ORDER NO. / APP", "BESTELLNR. / APP")), c.dim(L.tr("VERSION", "VERSION")),
                    c.dim("FTM"), c.dim("PEI"), c.dim(L.tr("STATE", "ZUSTAND")),
                    c.dim(L.tr("SERIAL NO.", "SERIENNR."))}, w);
        for (const auto& e : devices)
        {
            unsigned pa_a = 0, pa_l = 0, pa_d = 0;
            std::sscanf(e.name, "%u.%u.%u", &pa_a, &pa_l, &pa_d);
            int slot = 0;
            const IfaceOwner* own = ownerOf(owners, (uint16_t)((pa_a << 12) | (pa_l << 8) | pa_d), slot);
            // The owner list is a CONFIGURED list of addresses, not proof that a slot is in use and not
            // proof that no device sits there. A device that answered with its own mask outranks it.
            if (own && scanMask(e, &det) != 0) own = nullptr;
            if (own)
            {
                // Not a device: a tunnel slot of a KNXnet/IP interface. It acknowledges on the bus and
                // has no application layer, which is indistinguishable from a silent device by probing
                // alone -- only the owner's own list says what it is.
                char lbl[40];
                std::snprintf(lbl, sizeof(lbl), "Tunnel %d \xC2\xB7 ", slot);
                t.tableRow({c.violet(g_term.glyph("\xE2\x97\x8D", "o")), c.txt(e.name),
                            c.violet(lbl) + t.chip(paToStr(own->ia), 'c'),
                            c.dim(own->name), c.dim(""), c.dim(""), c.dim(""), c.dim(""), c.dim("")}, w);
                continue;
            }
            const uint16_t m = scanMask(e, &det);
            const char* cls = ftc::knxMaskName(m);
            const auto it = det.find(e.name);
            const ftc::DetailRow r = it != det.end() ? it->second : ftc::DetailRow{};
            std::string pa = c.txt(e.name);
            if (e.isOpenKnx) pa = c.cyan(e.name);
            auto cell = [&](const std::string& v) { return v.empty() ? c.dim("—") : c.txt(v); };
            // A BCU has no order number and no KNX serial: its identity is the 6-digit application
            // number, whose last BCD pair is the version. Showing a dash there would hide what WAS read.
            std::string ord = ftc::orderText(r.order), ver = r.version, ser = fmtSerial(r.serial);
            if (ord.empty() && r.bcuApp.size() >= 6) ord = "App " + r.bcuApp.substr(0, 4);
            if (ver.empty() && r.bcuApp.size() >= 6) ver = "V" + r.bcuApp.substr(4, 1) + "." + r.bcuApp.substr(5, 1);
            char pei[8] = {0};
            if (r.pei != 0xFF) std::snprintf(pei, sizeof(pei), "0x%02X", r.pei);
            // Load state where the device has one, BCU run state where it has not -- both answer "what
            // state is the application in", and a device reports exactly one of the two.
            std::string rst = r.appState != 0xFF ? ftcLoadNameH(r.appState) : "";
            if (rst.empty() && r.appState != 0xFF) rst = "LS " + std::to_string(r.appState); // named 0..5 only
            if (rst.empty() && r.runState != 0xFF) rst = "Run " + std::to_string(r.runState);
            if (r.progMode) rst += rst.empty() ? L.tr("prog", "Prog") : L.tr(" · prog", " · Prog");
            t.tableRow({t.statusDot('g'), pa,
                        c.txt(m == 0 ? L.tr("acknowledged only", "nur quittiert") : (cls[0] ? cls : "—")),
                        cell(ord), cell(ver), cell(r.ftm), cell(pei), cell(rst), cell(ser)}, w);
        }
    }
    else
    {
        const std::vector<int> wn = {2, 11, 26, 0};
        t.tableRow({c.dim("ST"), c.dim("PA"), c.dim(L.tr("CLASS", "KLASSE")), c.dim("INFO")}, wn);
        for (const auto& e : devices)
        {
            unsigned na = 0, nl = 0, nd = 0;
            std::sscanf(e.name, "%u.%u.%u", &na, &nl, &nd);
            int slot = 0;
            // The same classification the wide table makes: a tunnel slot acknowledges like a device and
            // is only distinguishable by its owner's list.
            const IfaceOwner* own = ownerOf(owners, (uint16_t)((na << 12) | (nl << 8) | nd), slot);
            if (own && (uint16_t)e.crc != 0) own = nullptr;
            if (own)
            {
                char lbl[40];
                std::snprintf(lbl, sizeof(lbl), "Tunnel %d \xC2\xB7 ", slot);
                t.tableRow({c.violet(g_term.glyph("\xE2\x97\x8D", "o")), c.txt(e.name),
                            c.violet(lbl) + t.chip(paToStr(own->ia), 'c'), c.dim(own->name)}, wn);
                continue;
            }
            const char* cls = ftc::knxMaskName((uint16_t)e.crc);
            std::string info = e.isOpenKnx ? (std::string() + t.chip("OpenKNX")) : c.dim("");
            t.tableRow({t.statusDot('g'), c.txt(e.name), c.txt(cls[0] ? cls : "—"), info}, wn);
        }
    }
    renderScanSummary(devices, cols ? &det : nullptr, owners);
}

static void renderScanSummary(const std::vector<FtcEntry>& devices,
                              const std::unordered_map<std::string, ftc::DetailRow>* det,
                              const std::vector<IfaceOwner>& owners)
{
    ftc::Theme& c = g_theme;
    ftc::I18n& L = g_i18n;
    std::vector<std::pair<std::string, int>> byClass;
    unsigned nTun = 0, nAck = 0, nDev = 0, nOkx = 0;
    for (const auto& e : devices)
    {
        // A tunnel slot is not a device and must not be counted as one -- it used to land under
        // "acknowledged only", which is exactly the sentence this map exists to stop.
        unsigned a = 0, l = 0, d = 0;
        std::sscanf(e.name, "%u.%u.%u", &a, &l, &d);
        int slot = 0;
        // Mask 0 means the address acknowledged but never answered — present on the bus, silent above.
        const uint16_t m = scanMask(e, det);
        // Same rule as the table: a device that named its mask is a device, not a configured tunnel slot.
        if (m == 0 && ownerOf(owners, (uint16_t)((a << 12) | (l << 8) | d), slot)) { nTun++; continue; }
        if (m == 0) nAck++; else nDev++;
        if (e.isOpenKnx) nOkx++;
        const char* cls = m == 0 ? L.tr("acknowledged only", "nur quittiert") : ftc::knxMaskName(m);
        std::string k = (cls && cls[0]) ? cls : "unknown";
        bool found = false;
        for (auto& p : byClass)
            if (p.first == k)
            {
                p.second++;
                found = true;
                break;
            }
        if (!found) byClass.push_back({k, 1});
    }
    std::sort(byClass.begin(), byClass.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    const std::string sep = c.dim("   \xC2\xB7   ");
    std::string s = c.bold(std::to_string(devices.size()) + L.tr(" addresses", " Adressen"));
    s += sep + c.bold(std::to_string(nDev)) + c.dim(L.tr(" devices", " Geräte"));
    if (nOkx) s += c.dim(L.tr(", of those ", ", davon ")) + c.bold(std::to_string(nOkx)) + c.dim(" OpenKNX");
    if (nTun) s += sep + c.violet(std::to_string(nTun)) + c.dim(L.tr(" tunnel addresses", " Tunnel-Adressen"));
    if (nAck) s += sep + c.amber(std::to_string(nAck)) + c.dim(L.tr(" acknowledged only", " nur quittiert"));
    std::printf("  %s\n", s.c_str());
    std::string by;
    for (const auto& p : byClass) by += (by.empty() ? "" : c.dim("   \xC2\xB7   ")) + c.txt(std::to_string(p.second) + "\xC3\x97 " + p.first);
    if (!by.empty()) std::printf("  %s\n", by.c_str());
}


/**
 * @brief Bring a target into a state that accepts writes — ask for the password, or wait for the button.
 * @details Every command that writes runs into the same four ETS access stages, and the useless answer is
 *          always the same one: print "this device is locked" and quit, leaving the user to start over once
 *          their hand has reached the enclosure. So this resolves it in place, and both the firmware update
 *          and the remote console go through it.
 * @param pa      the device to open up
 * @param waitS   how long to wait for the programming button (0 = do not offer the wait)
 * @param quiet   drop the explanatory chrome; a refusal still says one line, because silence is not an answer
 * @return the reading the target ended up at — stage Open means writes are allowed now
 */
/**
 * @brief Passwords typed during THIS run, per target. In memory only -- never written anywhere.
 * @details The device closes its write window after an idle timeout the ETS parameter sets, clamped to a
 *          minimum of 30 seconds. Between signing in and the transfer actually starting, knxOTA shows the
 *          difference panel and asks two questions -- easily longer than that. The window then closes and
 *          the upload is refused, which looked to the user like the firmware being rejected.
 *          Asking for the same password again would be theatre: it was typed a minute ago.
 */
static std::map<uint16_t, std::string> g_sessionPw;

/** @brief Wipe every remembered password. Called on the way out, however that happens. */
static void forgetSessionPasswords()
{
    for (auto& kv : g_sessionPw)
        kv.second.assign(kv.second.size(), '\0');
    g_sessionPw.clear();
}

/**
 * @brief Re-open the target's write window with the password already typed this run.
 * @details Silent on purpose: the user answered this question once, and a device that simply timed out is
 *          not a reason to ask again. False means there is nothing remembered, or it no longer works --
 *          then the caller falls back to asking.
 */
static bool refreshWriteWindow(uint16_t pa);

/**
 * @brief Until when a target refuses further sign-ins, kept across runs (<config-dir>/auth_backoff).
 * @details The device states the wait when it turns an attempt away -- and then the process ends, and the
 *          next one asks for a password again that cannot possibly work for another minute. Writing the
 *          deadline down is what lets the next run say "wait 44 s" instead of taking a secret it will
 *          throw away. It is a NOTE, not a rule: the device stays the authority, and a reboot or another
 *          client's successful login clears the lock without telling us -- hence the offer to try anyway.
 */
static std::string authBackoffPath() { return configSidecar("auth_backoff"); }

/** @brief Seconds this target still refuses logins for, 0 when nothing is known or it has expired. */
static uint32_t authBackoffRemaining(uint16_t pa)
{
    std::FILE* f = std::fopen(authBackoffPath().c_str(), "rb");
    if (f == nullptr) return 0;
    char line[128];
    long long until = 0;
    while (std::fgets(line, sizeof(line), f))
    {
        unsigned p = 0;
        long long u = 0;
        if (std::sscanf(line, "%u\t%lld", &p, &u) == 2 && p == pa) until = u;
    }
    std::fclose(f);
    const long long now = (long long)std::time(nullptr);
    return (until > now) ? (uint32_t)(until - now) : 0;
}

/** @brief Write the deadline down (seconds == 0 clears it, which is what a successful login does). */
static void authBackoffRemember(uint16_t pa, uint32_t seconds)
{
    const std::string path = authBackoffPath();
    std::vector<std::string> keep;
    if (std::FILE* f = std::fopen(path.c_str(), "rb"))
    {
        char line[128];
        const long long now = (long long)std::time(nullptr);
        while (std::fgets(line, sizeof(line), f))
        {
            unsigned p = 0;
            long long u = 0;
            if (std::sscanf(line, "%u\t%lld", &p, &u) != 2) continue;
            if (p == pa || u <= now) continue; // replaced, or long expired -- the file stays small
            char b[128];
            std::snprintf(b, sizeof(b), "%u\t%lld", p, u);
            keep.push_back(b);
        }
        std::fclose(f);
    }
    std::FILE* o = std::fopen(path.c_str(), "wb");
    if (o == nullptr) return;
    for (const auto& k : keep) std::fprintf(o, "%s\n", k.c_str());
    if (seconds)
        std::fprintf(o, "%u\t%lld\n", (unsigned)pa, (long long)std::time(nullptr) + (long long)seconds);
    std::fclose(o);
}

/** @brief "44 s" / "2 Min." -- the wait, in the unit a person would use. */
static std::string waitText(uint32_t seconds)
{
    ftc::I18n& L = g_i18n;
    char b[48];
    if (seconds < 60) std::snprintf(b, sizeof(b), "%u s", (unsigned)seconds);
    else std::snprintf(b, sizeof(b), L.tr("%u min", "%u Min."), (unsigned)((seconds + 59) / 60));
    return b;
}

/** @brief What a sign-in ended with: the device's own answer, plus how long it refuses further tries. */
struct SignInResult
{
    FileTransferClient::FtcAuthOutcome outcome = FileTransferClient::FtcAuthNone;
    uint16_t backoffS = 0;
    bool ok() const { return outcome == FileTransferClient::FtcAuthOk ||
                             outcome == FileTransferClient::FtcAuthNotNeeded; }
};

/**
 * @brief THE sign-in of this client. Every path that needs a write window goes through here.
 * @details There is exactly one login in oknx: the interactive prompt, the silent refresh before a
 *          transfer and the retry after a refusal all call this. Anything else would mean two places
 *          deciding what "signed in" means, and they would drift.
 *
 *          A lost frame is not an answer about the password, so an unanswered exchange is repeated once
 *          before anyone is asked to type again -- the challenge is fresh each time, so a repeat is
 *          harmless. A wrong password or a back-off comes back on the first try and is never repeated:
 *          the device counts guesses, and spending them silently is how a correct password ends up
 *          locked out.
 */
static SignInResult signIn(uint16_t pa, const char* pw)
{
    SignInResult r;
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        // The shared client narrates on the device logger, in English -- it is the same source that runs
        // on an RP2040. The outcome is reported here, in one voice and in the user's language.
        g_ftcSuppress = true;
        openknxFileTransferClient.requestLogin(pa, pw);
        runOneShotToQuiescence();
        g_ftcSuppress = false;
        r.outcome = openknxFileTransferClient.authOutcome();
        r.backoffS = openknxFileTransferClient.authBackoffSeconds();
        if (r.outcome != FileTransferClient::FtcAuthNoAnswer) break;
    }
    // The one place that learns the lock is the one place that records it -- and clears it, because a
    // login that went through proves the device is taking them again.
    if (r.ok()) authBackoffRemember(pa, 0);
    else if (r.outcome == FileTransferClient::FtcAuthBadPw && r.backoffS) authBackoffRemember(pa, r.backoffS);
    return r;
}

static ftc::AccessState resolveAccessInteractively(uint16_t pa, unsigned waitS, bool quiet, bool askOnly = false)
{
    ftc::I18n& L = g_i18n;
    ftc::Theme& c = g_theme;

    ftc::AccessDeps ad;
    ad.pump = []() {
        g_knxTunnel.pump();
        openknxFileTransferClient.loop(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(2)); // waits here last minutes, not ms
    };
    ad.nowMs = []() { return nowMs(); };
    ad.aborted = []() { return g_abort != 0; };
    ad.clientBusy = []() { return openknxFileTransferClient.isBusy(); };

    ftc::AccessState acc = ftc::readAccess(g_knxTunnel, pa, ad);
    // The login's own answer settles whether we are signed in; a second feature probe is only ever used to
    // REFRESH the capability bits. Merging it this way is what keeps one unanswered probe from turning a
    // correct password into "not accepted" -- and from dropping the delta/gzip bits on the floor with it.
    auto signedIn = [&](const ftc::AccessState& before) {
        ftc::AccessState s = before;
        s.stage = ftc::Access::Open;
        s.answered = true;
        s.bits = (uint8_t)(before.bits & ~ftc::FEAT_WRITES_DISABLED);
        const ftc::AccessState re = ftc::readAccess(g_knxTunnel, pa, ad);
        if (re.answered && re.stage == ftc::Access::Open) return re; // fresh and agreeing -> take it whole
        if (re.answered) s.bits = re.bits;                           // capabilities yes, its verdict no
        return s;
    };

    for (int attempt = 0; !askOnly && attempt < 3 && acc.stage != ftc::Access::Open; ++attempt)
    {
        if (acc.stage == ftc::Access::NeedPassword)
        {
            // Typed once this run and accepted then: use it before asking again. The window closes on an
            // idle timeout, which is not a reason to make someone retype what they already answered.
            const auto known = g_sessionPw.find(pa);
            if (attempt == 0 && known != g_sessionPw.end() && !known->second.empty())
            {
                if (signIn(pa, known->second.c_str()).ok())
                {
                    acc = signedIn(acc);
                    g_tpl.status(ftc::Tpl::Stat::Ok, L.tr("signed in", "angemeldet"),
                                 {L.tr("with the password from this run", "mit dem Passwort aus diesem Lauf")});
                    continue;
                }
            }
            if (!g_term.isTty()) break; // nobody there to type it — the caller reports the refusal
            // It told us last time how long it would turn attempts away. Asking for a secret we would
            // then throw away is the one thing not to do with that knowledge.
            uint32_t locked = authBackoffRemaining(pa);
            if (locked)
            {
                g_tpl.status(ftc::Tpl::Stat::Err,
                             L.tr("this device is still holding logins off", "dieses Gerät blockt Anmeldungen noch"),
                             {waitText(locked) + L.tr(" to go", " übrig"),
                              L.tr("its brute-force protection; an attempt before that neither helps nor hurts",
                                   "sein Brute-Force-Schutz; ein Versuch davor schadet nicht, bringt aber nichts")});
                // Short enough to sit out: offer it, with the clock running, instead of sending the user away.
                if (locked <= 90 &&
                    ftc::confirm(g_term, c, L, L.tr("Wait for it?", "Darauf warten?"), true))
                {
                    const uint64_t until = nowMs() + (uint64_t)locked * 1000ull + 500;
                    uint32_t drawn = 0xFFFFFFFFu;
                    while (nowMs() < until && !g_abort)
                    {
                        const uint32_t left = (uint32_t)((until - nowMs()) / 1000);
                        if (left != drawn)
                        {
                            g_tpl.waitTick(L.tr("waiting for the device", "warte auf das Gerät"), left,
                                           L.tr("ctrl-C stops", "Strg-C bricht ab"));
                            drawn = left;
                        }
                        g_knxTunnel.pump(); // the tunnel has to stay alive across the wait
                        openknxFileTransferClient.loop(true);
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    }
                    if (g_term.isTty()) std::printf("\r\x1b[K");
                    if (g_abort) break;
                    authBackoffRemember(pa, 0);
                    locked = 0;
                }
                else if (!ftc::confirm(g_term, c, L, L.tr("Try anyway?", "Trotzdem versuchen?"), false))
                    break; // the caller reports the refusal -- no password was asked for
            }
            g_tpl.status(ftc::Tpl::Stat::Warn, L.tr("this device is password protected",
                                                    "dieses Gerät ist passwortgeschützt"), {});
            if (!quiet)
                g_tpl.note(L.tr("that is the password from this device's ETS \"access protection\" parameter — "
                                "not your ETS password and not a KNX Secure key",
                                "das ist das Passwort aus dem ETS-Parameter \"Zugriffsschutz\" dieses Geräts — "
                                "nicht dein ETS-Passwort und kein KNX-Secure-Schlüssel"));
            std::string pw;
            if (!ftc::readSecret(g_term, c, L.tr("password", "Passwort"), pw)) break;
            const SignInResult si = signIn(pa, pw.c_str());
            const FileTransferClient::FtcAuthOutcome out = si.outcome;
            const uint16_t backS = si.backoffS;
            if (out == FileTransferClient::FtcAuthOk)
            {
                g_sessionPw[pa] = pw; // remembered because the DEVICE said yes, not because a probe agreed
                pw.assign(pw.size(), '\0');
                acc = signedIn(acc);
                g_tpl.status(ftc::Tpl::Stat::Ok, L.tr("signed in", "angemeldet"),
                             {L.tr("writes stay allowed until the device goes idle",
                                   "Schreibzugriff gilt, bis das Gerät in Ruhe fällt")});
                continue;
            }
            pw.assign(pw.size(), '\0'); // do not leave the local copy lying around
            if (out == FileTransferClient::FtcAuthBadPw && backS > 0)
            {
                // The device throttles guessing: 3 free tries, then 1, 2, 4 ... minutes. Saying only "not
                // accepted" here sends the user typing again into a window that refuses every attempt.
                char w[96];
                if (backS < 60)
                    std::snprintf(w, sizeof(w), L.tr("next attempt in %u s", "nächster Versuch in %u s"),
                                  (unsigned)backS);
                else
                    std::snprintf(w, sizeof(w), L.tr("next attempt in %u min", "nächster Versuch in %u Min."),
                                  (unsigned)((backS + 59) / 60));
                g_tpl.status(ftc::Tpl::Stat::Err, L.tr("too many attempts — the device is holding them off",
                                                       "zu viele Versuche — das Gerät blockt sie ab"),
                             {w, L.tr("this is the device's brute-force protection, not a fault",
                                      "das ist der Brute-Force-Schutz des Geräts, kein Fehler")});
                break; // waiting out a doubling lock inside this loop helps nobody
            }
            if (out == FileTransferClient::FtcAuthBadPw)
                g_tpl.status(ftc::Tpl::Stat::Warn, L.tr("that password was not accepted",
                                                        "dieses Passwort wurde nicht angenommen"),
                             {L.tr("the device slows down repeated attempts on purpose",
                                   "das Gerät bremst wiederholte Versuche absichtlich aus")});
            else if (out == FileTransferClient::FtcAuthNotNeeded)
            {
                // It no longer asks for one -- the stage was changed between the two readings.
                acc = signedIn(acc);
                g_tpl.status(ftc::Tpl::Stat::Ok, L.tr("this device does not ask for a password",
                                                      "dieses Gerät verlangt kein Passwort"), {});
                continue;
            }
            else
            {
                // Which of the three frames stayed unanswered. "No answer" on its own cannot be acted
                // on; the step names the suspect -- a silent capability probe is an old or busy target,
                // a silent challenge or verdict is an exchange that broke off mid-way.
                const char* step = L.tr("no step reached", "kein Schritt erreicht");
                switch (openknxFileTransferClient.authStage())
                {
                    case FileTransferClient::FtcAuthStageProbe:
                        step = L.tr("it did not answer the capability question (CheckFeatures)",
                                    "es hat die Fähigkeitsabfrage nicht beantwortet (CheckFeatures)");
                        break;
                    case FileTransferClient::FtcAuthStageChallenge:
                        step = L.tr("it did not send the challenge", "es hat die Challenge nicht geschickt");
                        break;
                    case FileTransferClient::FtcAuthStageResponse:
                        step = L.tr("it did not answer the response", "es hat die Antwort nicht quittiert");
                        break;
                    default: break;
                }
                g_tpl.status(ftc::Tpl::Stat::Warn, L.tr("the device did not answer the login",
                                                        "das Gerät hat auf die Anmeldung nicht geantwortet"),
                             {step, L.tr("that says nothing about the password — it was tried twice",
                                         "das sagt nichts über das Passwort — es wurde zweimal versucht")});
            }
            continue;
        }
        if (acc.stage == ftc::Access::Blocked || acc.stage == ftc::Access::LockedOff)
        {
            if (waitS == 0) break;
            g_tpl.status(ftc::Tpl::Stat::Warn, L.tr("this device is not accepting writes right now",
                                                    "dieses Gerät nimmt gerade keine Schreibzugriffe an"), {});
            g_tpl.note(L.tr("press the programming button on the device now — I check every second",
                            "drücke jetzt die Programmiertaste am Gerät — ich prüfe jede Sekunde nach"));
            const bool freed = ftc::waitForWrites(g_knxTunnel, pa, ad, waitS, [&](uint32_t el) {
                g_tpl.waitTick(L.tr("waiting for the device", "warte auf das Gerät"), el,
                               L.tr("ctrl-C stops", "Strg-C bricht ab"));
            });
            if (g_term.isTty()) std::printf("\r\x1b[K");
            if (freed)
            {
                acc = ftc::readAccess(g_knxTunnel, pa, ad);
                continue;
            }
            g_ui.errorBlock(false, L.tr("the button did not free it", "die Taste hat die Sperre nicht gelöst"),
                            {L.tr("then it is the access protection set in ETS",
                                  "dann ist es der Zugriffsschutz aus der ETS"),
                             L.tr("the parameter is called \"access protection\" in the OpenKNX application",
                                  "der Parameter heißt \"Zugriffsschutz\" in der OpenKNX-Applikation")},
                            L.tr("set it to Always or Programming mode and download the application once",
                                 "auf Immer oder Programmiermodus stellen und die Applikation einmal laden"));
            break;
        }
        break; // Unknown: nothing to resolve
    }
    return acc;
}

static bool refreshWriteWindow(uint16_t pa)
{
    const auto it = g_sessionPw.find(pa);
    if (it == g_sessionPw.end() || it->second.empty()) return false;

    // The device's own answer, not a second probe: a lost CheckFeatures used to report a perfectly good
    // login as failed, and the caller then asked for a password that was already accepted.
    const SignInResult r = signIn(pa, it->second.c_str());
    if (r.ok()) return true;
    const FileTransferClient::FtcAuthOutcome out = r.outcome;
    // A stored password the device now rejects is worse than none: every silent re-use spends one of the
    // three free tries and walks the target into its doubling lock-out. Drop it.
    if (out == FileTransferClient::FtcAuthBadPw)
    {
        it->second.assign(it->second.size(), '\0');
        g_sessionPw.erase(it);
    }
    return false;
}


/**
 * @brief Classify + render a <pa> read/status command from structured getters; true if it handled the command.
 * @details True => raw text suppressed, structured output drawn. False => not a structured command, so the caller
 *          runs the normal one-shot path.
 */
static bool ftcRenderStructured(const std::vector<std::string>& pos, bool quiet, int& rc)
{
    enum Kind
    {
        K_None,
        K_Ping,
        K_Info,
        K_InfoGa,
        K_Df,
        K_Ll,
        K_Ls,
        K_Scan,
        K_Simple
    };
    Kind k = K_None;
    std::string verb;
    if (!pos.empty() && pos[0] == "scan") k = K_Scan;
    else if (pos.size() >= 2)
    {
        const std::string& s = pos[1];
        if (s == "ping") k = K_Ping;
        else if (s == "df")
            k = K_Df;
        else if (s == "ll")
            k = K_Ll;
        else if (s == "ls")
            k = K_Ls;
        else if (s == "info")
        {
            if (pos.size() >= 3 && pos[2] == "ga") k = K_InfoGa;
            else if (pos.size() == 2)
                k = K_Info; // bare device info; `info <file>` stays with the client
        }
        else if (s == "rm" || s == "mkdir" || s == "rmdir" || s == "mv" || s == "format")
        {
            k = K_Simple; // fwupdate stays with the client (fire-and-forget reboot; no clean ok/fail signal)
            verb = s;
        }
    }
    if (k == K_None) return false;

    ftc::Theme& c = g_theme;
    ftc::I18n& L = g_i18n;
    ftc::Tpl& t = g_tpl;
    const std::string target = pos[0];

    // `details` asks every found device who it is; without it only the System B candidates are asked,
    // because only those can be OpenKNX. The reads run in child processes over their own tunnels, started
    // while the sweep is still going, so they cost almost no extra wall clock.
    bool wantDetails = false, wantOpenKnx = false;
    // The maximum is the default: a scan reads what each device says about itself unless told not to.
    // `details` stays accepted and does nothing, so an older command line still works.
    bool noDetails = false;
    for (const auto& a : pos)
    {
        if (a == "details") wantDetails = true;
        if (a == "openknx") wantOpenKnx = true;
        if (a == "--no-details") noDetails = true;
    }
    // `openknx` is the narrow form and must stay narrow: it asks only the System B candidates, so it must
    // not be overruled by the full-details default.
    if (k == K_Scan && !noDetails && !wantOpenKnx) wantDetails = true;
    // An explicit "off" wins over both spellings of "on". `details --no-details` used to run full details
    // and `openknx --no-details` still built the pool, so the switch did nothing in either combination.
    if (noDetails) { wantDetails = false; wantOpenKnx = false; }
    std::unique_ptr<ftc::DetailPool> pool;
    if (k == K_Scan && (wantDetails || wantOpenKnx) && !g_ip.empty())
        pool.reset(new ftc::DetailPool(g_selfPath, g_ip, g_port,
                                       g_workers > 0 ? g_workers : FTC_DETAIL_WORKERS));

    // What this run is about to do, before it does it: which probe, what it costs on the bus, and how
    // many identity reads will run alongside. Two LED rows because they are two separate tunnel groups.
    auto announceScan = [&](uint16_t aStart, uint16_t aEnd, bool coScan, int workers) {
        ftc::Theme& cc = g_theme;
        ftc::I18n& LL = g_i18n;
        ftc::Tpl& tt = g_tpl;
        const unsigned span = (unsigned)(aEnd - aStart + 1);
        char hdr[80];
        std::snprintf(hdr, sizeof(hdr), "%s \xC2\xB7 %s\xE2\x80\xA6%s", LL.tr("Sweep", "Suchlauf"),
                      paToStr(aStart).c_str(), paToStr(aEnd).c_str());
        tt.section(hdr);
        tt.kv(LL.tr("method", "Verfahren"),
              coScan ? cc.txt(LL.tr("CO probe (ets)", "CO-Probe (ets)")) + cc.dim(LL.tr("   ·   T_Connect per address — reaches BCU1/BCU2",
                                                                                       "   ·   T_Connect je Adresse — erreicht BCU1/BCU2"))
                     : cc.txt(LL.tr("sweep (connectionless)", "Sweep (verbindungslos)")) +
                           cc.dim(LL.tr("   ·   one telegram per address, presence from the TP1 acknowledge",
                                        "   ·   ein Telegramm je Adresse, Anwesenheit aus der TP1-Quittung")));
        char bus[96];
        std::snprintf(bus, sizeof(bus), "~%u %s", coScan ? span * 3u : span, LL.tr("telegrams on TP1", "Telegramme auf TP1"));
        tt.kv(LL.tr("bus load", "Buslast"), cc.txt(bus));
        tt.kv(LL.tr("search", "Suche"), tt.ledRow(1, 16, 'g') + cc.dim(LL.tr("   1 tunnel · one request in flight",
                                                                            "   1 Tunnel · eine Anfrage unterwegs")));
        char idn[96];
        std::snprintf(idn, sizeof(idn), "   %d %s", workers,
                      workers ? (wantDetails ? LL.tr("in parallel · order no., version, FTM, serial per device",
                                                     "parallel · Bestellnr., Version, FTM, Seriennr. je Gerät")
                                             : LL.tr("in parallel · identity of the System B candidates only (openknx)",
                                                     "parallel · Identität nur der System-B-Kandidaten (openknx)"))
                              : LL.tr("— identity not read (--no-details)", "— Identität wird nicht gelesen (--no-details)"));
        tt.kv(LL.tr("identity", "Identität"), tt.ledRow(workers, 16, 'c') + cc.dim(idn));
    };

    // Who owns which address. Costs one multicast search plus one management read per answering device
    // (no tunnel slot), and is what turns a tunnel slot in the hit list from "acknowledged only" into
    // "tunnel 3 of 1.1.11". Only for a scan, and only when there is a network to ask.
    std::vector<IfaceOwner> owners;
    if (k == K_Scan && !g_ip.empty())
    {
        owners = buildOwnerMap(g_port);
        if (!owners.empty() && !quiet) renderOwnerMap(owners); // before the sweep: it frames what follows
    }

    std::string cmd = "ftc";
    for (const auto& p : pos)
    {
        if (k == K_Scan && (p == "details" || p == "--no-details")) continue; // handled here, the shared parser knows neither
        // With the pool running, the sweep's post-probe would re-ask the same devices over the shared
        // tunnel (double traffic, mutual starvation); the pool's answer already carries the manufacturer.
        if (p == "openknx" && (pool || noDetails)) continue; // off means off: do not let the device probe either
        cmd += ' ';
        cmd += p;
    }

    bool ackFeed = false;
    if (k == K_Scan)
    {
        // Only for the connectionless sweep: requestScan clears _scanAckArmed for a CO scan
        // (_scanAckArmed = !_scanCo), so arming it there costs two TP telegrams and feeds nothing.
        uint16_t aStart = 0, aEnd = 0;
        const bool coScan = std::find(pos.begin(), pos.end(), std::string("ets")) != pos.end();
        if (!coScan && parseScanRange(pos, aStart, aEnd) && (aStart >> 8) == (aEnd >> 8))
            ackFeed = armScanAckFeed((uint16_t)(aStart & 0xFF00));
    }

    if (k == K_Scan && !quiet)
    {
        uint16_t bStart = 0, bEnd = 0;
        if (parseScanRange(pos, bStart, bEnd))
            announceScan(bStart, bEnd, std::find(pos.begin(), pos.end(), std::string("ets")) != pos.end(),
                         pool ? (g_workers > 0 ? g_workers : FTC_DETAIL_WORKERS) : 0);
    }

    g_ftcSuppress = true;
    std::vector<FtcEntry> snap;
    if (!quiet && k != K_Scan && g_term.isTty())
        // info/ga/df/ll/ls read the device blocking and can't yield -> thread it + animate so it never looks hung
        runWithBusAnim(L.tr("reading device…", "lese Gerät…"), [&]() {
            openknxFileTransferClient.processCommand(cmd, false);
            ftcPumpStructured(snap, false, false);
        });
    else
    {
        std::function<void(const FtcEntry&)> onNew;
        if (pool)
            onNew = [&](const FtcEntry& e) {
                // The class comes from the sweep; only a System B device can be OpenKNX. An address that
                // merely acknowledged (mask 0) says nothing about itself, so `details` includes it too.
                // One definition of "could be OpenKNX", taken from the stack's own classifier. The old
                // `& 0xFFF0 == 0x07B0` matched TP1 only, so a device reporting 0x57B0 (TP1/IP System B)
                // or an OpenKNX router (0x091A) was never asked who it is.
                const bool candidate =
                    wantDetails || KnxDeviceMap::family((uint16_t)e.crc) == KnxDeviceMap::Family::SystemB;
                if (candidate) pool->submit(e.name);
            };
        openknxFileTransferClient.processCommand(cmd, false);
        ftcPumpStructured(snap, k == K_Scan, k == K_Scan && !quiet, k == K_InfoGa,
                          pool ? &onNew : nullptr); // live progress line for the scan (unless -q)
    }
    if (ackFeed) g_knxTunnel.setConfirmCallback(nullptr);
    g_ftcSuppress = false;

    switch (k)
    {
        case K_Ping:
        {
            const FtcStatus& st = openknxFileTransferClient.status();
            rc = st.ok ? 0 : 1;
            if (quiet)
            {
                std::printf("ping\t%s\t%d\n", target.c_str(), st.ok ? 1 : 0);
                break;
            }
            if (st.ok) t.status(ftc::Tpl::Stat::Ok, target + " " + L.tr("reachable", "erreichbar"), {});
            else
                t.status(ftc::Tpl::Stat::Err, target + " " + L.tr("no answer", "keine Antwort"), {});
            break;
        }
        case K_Info:
        {
            const FtcDeviceInfo& d = openknxFileTransferClient.deviceInfo();
            rc = d.valid ? 0 : 1;
            const unsigned maj = (d.ftmVersion >> 8) & 0xFF, min = (d.ftmVersion >> 4) & 0x0F, rev = d.ftmVersion & 0x0F;
            // A legacy server has no CheckFeatures, so its feature byte is 0 because it cannot SAY.
            // Printing "(none)" there reads as "this device can nothing" -- it is shown as derived instead.
            const uint32_t fb = d.ftmLegacy ? d.ftmCan : d.features;
            std::string feat;
            if (fb & ftc::FEAT_RESUME) feat += "Resume ";
            // Derived (no CheckFeatures answer on a legacy server): FwUpdate exists on the RP2040 builds of
            // those versions and not on their ESP32 builds, and the MCU is not readable here -- so the bit
            // is shown with a question mark, never as a fact.
            if (fb & ftc::FEAT_UPDATE) feat += "Update ";
            if (fb & ftc::FEAT_FAST) feat += "Fast ";
            if (fb & ftc::FEAT_CONSOLE) feat += "Console ";
            if (fb & ftc::FEAT_GZIP_UPDATE) feat += "Gzip ";
            if (fb & ftc::FEAT_DELTA) feat += "Delta ";
            if (fb & ftc::FEAT_AUTH_REQUIRED) feat += "Password ";
            if (fb & ftc::FEAT_WRITES_DISABLED) feat += "Locked ";
            if (!feat.empty()) feat.pop_back();
            const std::string featTokens = feat.empty() ? "(none)" : feat; // the quiet form stays machine-readable
            if (d.ftmDerived && (fb & ftc::FEAT_UPDATE)) feat.replace(feat.find("Update"), 6, "Update?"); // the panel's question mark
            if (d.ftmLegacy)
                feat = feat.empty() ? L.tr("FTM before 0.2.0 - upload only", "FTM vor 0.2.0 - nur Upload")
                                    : feat + (d.ftmDerived ? L.tr("  (FTM before 0.2.0, derived from its version; resume withheld by this client; ? = RP2040/2350 builds only, unconfirmed)",
                                                                   "  (FTM vor 0.2.0, aus seiner Version abgeleitet; Fortsetzen von diesem Client zurückgehalten; ? = nur RP2040/2350-Builds, unbestätigt)")
                                                           : L.tr("  (FTM before 0.2.0; resume withheld by this client)",
                                                                  "  (FTM vor 0.2.0; Fortsetzen von diesem Client zurückgehalten)"));
            else if (feat.empty())
                feat = "(none)";
            char vbuf[16];
            std::snprintf(vbuf, sizeof(vbuf), "%u.%u.%u", maj, min, rev);
            if (quiet)
            {
                std::printf("pa\t%s\n", target.c_str());
                std::printf("mask\t0x%04X\n", d.mask);
                std::printf("class\t%s\n", d.cls);
                if (d.haveSerial)
                {
                    std::printf("manufacturer\t0x%04X\n", d.manufacturer);
                    std::printf("serial\t%02X%02X%02X%02X%02X%02X\n", d.serial[0], d.serial[1], d.serial[2], d.serial[3], d.serial[4], d.serial[5]);
                }
                if (d.haveOrder) std::printf("order\t%s\n", d.order);
                if (d.haveHw) std::printf("hardware\t%02X%02X%02X%02X%02X%02X\n", d.hardware[0], d.hardware[1], d.hardware[2], d.hardware[3], d.hardware[4], d.hardware[5]);
                if (d.haveVersion) std::printf("version\t[%u] %u.%u\n", (d.version >> 11) & 0x1F, (d.version >> 6) & 0x1F, d.version & 0x3F);
                if (d.ftmVersion) std::printf("ftm_version\t%s\n", vbuf);
                std::printf("features\t%s\n", featTokens.c_str());
                if (d.ftmLegacy) std::printf("ftm_legacy\t1\nftm_derived\t%d\n", d.ftmDerived ? 1 : 0);
                std::printf("progmode\t%d\n", d.progMode ? 1 : 0);
                // The BCU1/BCU2 identity block. Without these keys a BCU answers the child in full and the
                // caller still sees an empty row: those families have no order number and no KNX serial,
                // their identity IS the manufacturer code plus the 6-digit application number.
                if (d.haveBcu1)
                {
                    if (d.bcuMfr) std::printf("bcu_manufacturer\t0x%02X\n", d.bcuMfr);
                    if (d.haveBcuApp)
                        std::printf("bcu_app\t%02X%02X%02X\n", d.bcuApp[0], d.bcuApp[1], d.bcuApp[2]);
                    if (d.haveBcuRunState) std::printf("bcu_runstate\t%u\n", d.bcuRunState);
                    if (d.haveBcuPei) std::printf("bcu_pei\t0x%02X\n", d.bcuPeiType);
                    if (d.haveBcuRunError) std::printf("bcu_runerror\t0x%02X\n", d.bcuRunError);
                }
                if (d.appState != 0xFF)
                {
                    std::printf("app_state\t%s\n", ftcLoadNameH(d.appState));
                    std::printf("app_state_id\t%u\n", (unsigned)d.appState); // the name is translated; the id is not
                }
                if (d.addrTableState != 0xFF) std::printf("addr_table\t%s\n", ftcLoadNameH(d.addrTableState));
                if (d.assocTableState != 0xFF) std::printf("assoc_table\t%s\n", ftcLoadNameH(d.assocTableState));
                if (d.goTableState != 0xFF) std::printf("go_table\t%s\n", ftcLoadNameH(d.goTableState));
                break;
            }
            if (!d.valid)
            {
                t.status(ftc::Tpl::Stat::Err, target + " " + L.tr("no device info", "keine Geräte-Info"),
                         {L.tr("not a KnxFileTransfer / no answer", "kein KnxFileTransfer / keine Antwort")});
                break;
            }
            char mbuf[16];
            std::snprintf(mbuf, sizeof(mbuf), "0x%04X", d.mask);
            ftc::Panel p(t, L.tr("Device", "Gerät"), target);
            p.kv(L.tr("Mask / class", "Maske / Klasse"), c.txt(mbuf) + (d.cls[0] ? c.dim(std::string("  · ") + d.cls) : std::string()));
            char ib[32];
            if (d.haveSerial)
            {
                std::snprintf(ib, sizeof(ib), "0x%04X", d.manufacturer);
                p.kv(L.tr("Manufacturer", "Hersteller"), c.txt(ib));
                std::snprintf(ib, sizeof(ib), "%02X%02X:%02X%02X%02X%02X", d.serial[0], d.serial[1], d.serial[2], d.serial[3], d.serial[4], d.serial[5]);
                p.kv(L.tr("Serial", "Seriennr."), c.txt(ib));
            }
            if (d.haveOrder) p.kv(L.tr("Order number", "Bestellnr."), c.txt(d.order));
            if (d.haveHw)
            {
                std::snprintf(ib, sizeof(ib), "%02X %02X %02X %02X %02X %02X", d.hardware[0], d.hardware[1], d.hardware[2], d.hardware[3], d.hardware[4], d.hardware[5]);
                p.kv(L.tr("Hardware type", "Hardware-Typ"), c.txt(ib) + c.dim(std::string("  · app 0x") + [&] { char a[8]; std::snprintf(a, sizeof(a), "%04X", (d.hardware[2] << 8) | d.hardware[3]); return std::string(a); }()));
            }
            if (d.haveVersion)
            {
                std::snprintf(ib, sizeof(ib), "[%u] %u.%u", (d.version >> 11) & 0x1F, (d.version >> 6) & 0x1F, d.version & 0x3F);
                p.kv(L.tr("Version", "Version"), c.txt(ib));
            }
            if (d.ftmVersion) p.kv(L.tr("FTM version", "FTM-Version"), c.bold(vbuf));
            p.kv(L.tr("Features", "Funktionen"), c.txt(feat));
            if (d.maxApdu)
            {
                // Both numbers matter: a transfer uses the smaller of target and interface, so seeing them
                // side by side is what explains the frame size the tool ends up choosing.
                char ab[80];
                std::snprintf(ab, sizeof(ab), "%u B", (unsigned)d.maxApdu);
                std::string row = c.bold(ab);
                if (g_ifaceApdu)
                {
                    // Whichever is smaller decides the frame size, and it is not always the device -- an
                    // old interface in the path is the usual surprise. Name the one that limits.
                    const char* who = (d.maxApdu < g_ifaceApdu)   ? L.tr(" -> the target limits", " -> das Ziel begrenzt")
                                      : (g_ifaceApdu < d.maxApdu) ? L.tr(" -> the interface limits", " -> das Interface begrenzt")
                                                                  : "";
                    row += "  " + c.dim(std::string(L.tr("· interface ", "· Interface ")) +
                                        std::to_string((unsigned)g_ifaceApdu) + " B" + who);
                }
                p.kv(L.tr("Max APDU", "Max APDU"), row);
            }
            p.kv(L.tr("Prog mode", "Prog-Modus"),
                 d.progMode ? t.chip("PROG", 'o') : c.mut(std::string(g_term.glyph("○", "o")) + " " + L.tr("off", "aus")));
            // Shown only when the device answered it -- an empty row would claim knowledge we do not have.
            if (d.haveDevControl)
            {
                std::string st;
                if (d.devControl & 0x01) st += L.tr("safe state", "Sicherheitszustand");
                if (d.devControl & 0x04) { if (!st.empty()) st += " · "; st += L.tr("verify mode", "Verify-Modus"); }
                if (st.empty()) st = L.tr("normal", "normal");
                p.kv(L.tr("Device state", "Gerätezustand"),
                     (d.devControl & 0x01) ? c.amber(st) : c.txt(st));
            }
            if (d.isRouter)
            {
                p.sep();
                p.kv(L.tr("Coupler", "Koppler"), c.cyan(L.tr("has a router object", "hat ein Router-Objekt")));
                if (d.haveLineStatus)
                    p.kv(L.tr("Sub line", "Nebenlinie"),
                         (d.lineStatus & 0x01) ? c.green(L.tr("ok", "ok")) : c.amber(L.tr("no power / fault", "keine Spannung / Störung")));
                if (d.haveRouterApdu)
                {
                    char rb[24];
                    std::snprintf(rb, sizeof(rb), "%u B", (unsigned)d.routerApdu);
                    p.kv(L.tr("Routed APDU", "Weitergeleitete APDU"), c.txt(rb));
                }
            }
            if (d.haveBcu1)
            {
                // BCU1/BCU2 memory-map extras (ETS "Gerätehersteller / Ausführungszustand / Ausführungsfehler / PEI Typ / Applikationsprogramm").
                if (d.bcuMfr)
                {
                    std::snprintf(ib, sizeof(ib), "0x%02X", d.bcuMfr);
                    p.kv(L.tr("Manufacturer", "Hersteller"), c.txt(ib) + (d.bcuMfr == 0x01 ? c.dim("  · Siemens") : std::string()));
                }
                if (d.haveBcuApp)
                {
                    std::snprintf(ib, sizeof(ib), "%02X%02X%02X  V0.%X", d.bcuApp[0], d.bcuApp[1], d.bcuApp[2], d.bcuApp[2]);
                    p.kv(L.tr("App program", "Applikation"), c.txt(ib));
                }
                if (d.haveBusVolt)
                {
                    std::snprintf(ib, sizeof(ib), "%u.%u V", d.busVoltmV / 1000, (d.busVoltmV % 1000) / 100);
                    p.kv(L.tr("Bus voltage", "Busspannung"), c.txt(ib));
                }
                // Each byte is shown only if the identity block reached it. 0xFF is also the device's own
                // "no error", so an unread runError used to be rendered as a green OK.
                const std::string unread = c.dim(L.tr("not read", "nicht gelesen"));
                p.kv(L.tr("Run state", "Ausführungszustand"),
                     d.haveBcuRunState ? c.txt(std::to_string((unsigned)d.bcuRunState)) : unread);
                if (d.haveBcuRunError)
                {
                    std::snprintf(ib, sizeof(ib), "0x%02X", d.bcuRunError);
                    p.kv(L.tr("Run error", "Ausführungsfehler"), c.txt(ib) + (d.bcuRunError >= 0xFE ? c.green("  · OK") : c.red("  · error")));
                }
                else
                    p.kv(L.tr("Run error", "Ausführungsfehler"), unread);
                if (d.haveBcuPei)
                {
                    std::snprintf(ib, sizeof(ib), "0x%02X", d.bcuPeiType);
                    p.kv(L.tr("PEI type", "PEI-Typ"), c.txt(ib));
                }
                else
                    p.kv(L.tr("PEI type", "PEI-Typ"), unread);
            }
            const bool anyLoad = d.appState != 0xFF || d.addrTableState != 0xFF || d.assocTableState != 0xFF || d.goTableState != 0xFF;
            if (anyLoad)
            {
                p.sep();
                auto ls = [&](uint8_t st) {
                    const char* n = ftcLoadNameH(st);
                    return (st == 1) ? c.green(n) : (st == 3 ? c.red(n) : c.txt(n));
                };
                if (d.appState != 0xFF) p.kv(L.tr("App program", "Applikation"), ls(d.appState));
                if (d.addrTableState != 0xFF) p.kv(L.tr("Address table", "Adresstabelle"), ls(d.addrTableState));
                if (d.assocTableState != 0xFF) p.kv(L.tr("Assoc. table", "Assoz.-Tabelle"), ls(d.assocTableState));
                if (d.goTableState != 0xFF) p.kv(L.tr("Object table", "Objekttabelle"), ls(d.goTableState));
            }
            p.render(0);
            break;
        }
        case K_InfoGa:
        {
            uint16_t n = 0;
            const FtcGaEntry* gos = openknxFileTransferClient.groupObjects(n);
            // A run the safety cap cut short is a PREFIX, whatever arrived - say so and fail the exit
            // code, instead of printing it as the device's full table.
            const bool cut = g_pumpCapped || openknxFileTransferClient.groupObjectsTruncated();
            // A prefix is not a result -> `cut`, not just the pump cap. Zero group objects IS a result
            // (an unloaded device, or an application without KOs), but only if the read itself succeeded.
            rc = (cut || !openknxFileTransferClient.deviceInfo().valid) ? 1 : 0;
            // canonical flag letters (data side) -> the set-letter string koFlags() expects
            auto flagsActive = [](uint8_t f) { std::string s; static const char* code = "CRWTU"; for (int b = 0; b < 5; ++b) if (f & (1 << b)) s += code[b]; return s; };
            static const char* const SZ[21] = {"1 bit", "2 bit", "3 bit", "4 bit", "5 bit", "6 bit", "7 bit", "1 byte",
                                               "2 byte", "3 byte", "4 byte", "6 byte", "8 byte", "10 byte", "14 byte",
                                               "5 byte", "7 byte", "9 byte", "11 byte", "12 byte", "13 byte"};
            auto sizeName = [&](bool valid, uint8_t code) -> std::string { return (valid && code <= 20) ? SZ[code] : "-"; };
            auto prioName = [&](bool valid, uint8_t p) -> std::string {
                if (!valid) return "-";
                switch (p)
                {
                    case 0: return L.tr("system", "System");
                    case 1: return L.tr("normal", "normal");
                    case 2: return L.tr("urgent", "dringend");
                    case 3: return L.tr("low", "niedrig");
                    default: return "-";
                }
            };
            auto gaStr = [](uint16_t ga) { char b[16]; std::snprintf(b, sizeof(b), "%u/%u/%u", (ga >> 11) & 0x1F, (ga >> 8) & 0x07, ga & 0xFF); return std::string(b); };
            // Group the associations by KO (one object can carry several GAs -- the sending one first, then listeners),
            // sorted by KO number, so the output lines up 1:1 with ETS ("Obj#n: <ga> <ga> ...").
            struct KoRow
            {
                std::vector<uint16_t> gas;
                uint8_t flags = 0;
                uint8_t prio = 0xFF;
                uint8_t sizeCode = 0xFF;
                bool cfg = false;
                bool unk = false; // at least one of this KO's associations named a TSAP we could not resolve
            };
            std::map<uint16_t, KoRow> byKo;
            for (uint16_t i = 0; i < n; ++i)
            {
                KoRow& r = byKo[gos[i].co];
                if (gos[i].ga != 0) r.gas.push_back(gos[i].ga);
                if (gos[i].gaUnknown) r.unk = true;
                if (gos[i].cfgValid && !r.cfg)
                {
                    r.flags = gos[i].flags;
                    r.prio = gos[i].prio;
                    r.sizeCode = gos[i].sizeCode;
                    r.cfg = true;
                }
            }
            if (quiet)
            {
                for (auto& kv : byKo)
                {
                    std::string gl;
                    for (uint16_t g : kv.second.gas)
                    {
                        if (!gl.empty()) gl += ",";
                        gl += gaStr(g);
                    }
                    // "-" = this object has no group address. "?" = the address table could not be read,
                    // so we do not know. Conflating them reported a configured device as unlinked. Per KO,
                    // from its own rows: a run-wide flag put "?" on objects that are simply unassigned.
                    if (kv.second.unk) gl += gl.empty() ? "?" : ",?";
                    std::printf("ko\t%u\t%s\t%s\t%s\t%s\n", kv.first, gl.empty() ? "-" : gl.c_str(),
                                kv.second.cfg ? flagsActive(kv.second.flags).c_str() : "?",
                                prioName(kv.second.cfg, kv.second.prio).c_str(), sizeName(kv.second.cfg, kv.second.sizeCode).c_str());
                }
                if (cut) std::printf("truncated\t1\n");
                if (g_pumpCapped) std::printf("capped\t1\n");
                break;
            }
            t.section(L.tr("Group communication · ", "Gruppenkommunikation · ") + target);
            // The incompleteness goes FIRST: a device whose address table timed out and whose association
            // table is absent printed a clean "no group objects" for a walk that never read anything.
            if (cut)
                t.status(ftc::Tpl::Stat::Warn,
                         L.tr("incomplete - this is a prefix of the device's tables",
                              "unvollständig - das ist ein Anfang der Gerätetabellen"),
                         {g_pumpCapped
                              ? L.tr("the read was still running when the 5-minute safety limit ended it",
                                     "der Lauf war noch unterwegs, als das 5-Minuten-Sicherheitslimit ihn beendet hat")
                              : L.tr("a table was cut short: the row limit, a read timeout, the 16-bit address end, or no connection-oriented link",
                                     "eine Tabelle brach ab: Zeilengrenze, Zeitüberschreitung, Ende des 16-Bit-Adressraums, oder keine verbindungsorientierte Verbindung")});
            if (n == 0)
            {
                t.status(ftc::Tpl::Stat::Idle, L.tr("no group objects / association table", "keine Gruppenobjekte / Assoziationstabelle"));
                break;
            }
            // Pre-build each KO's GA string and size the GA column to the widest one (dynamic; a KO with 4 GAs is fully
            // visible and every row still lines up). Min = the header width.
            std::map<uint16_t, std::string> gaLine;
            int gaW = (int)std::string(L.tr("GROUP ADDRESSES", "GRUPPENADRESSEN")).size();
            for (auto& kv : byKo)
            {
                std::string gl;
                for (uint16_t g : kv.second.gas)
                {
                    if (!gl.empty()) gl += "  ";
                    gl += gaStr(g);
                }
                // "-" = this object has no group address. "?" = the address table could not be read, so
                // we do not know. A "?" behind resolved addresses says "and at least one more we could not
                // read" - the KO keeps what was proven and does not claim the rest is absent.
                if (kv.second.unk) gl += gl.empty() ? "?" : "  ?";
                if (gl.empty()) gl = "-";
                if ((int)gl.size() > gaW) gaW = (int)gl.size();
                gaLine[kv.first] = gl;
            }
            t.tableRow({c.dim(L.tr("KO", "KO")), c.dim(L.tr("GROUP ADDRESSES", "GRUPPENADRESSEN")), c.dim(L.tr("FLAGS", "FLAGS")),
                        c.dim(L.tr("PRIO", "PRIO")), c.dim(L.tr("SIZE", "GRÖSSE"))},
                       {5, gaW, 12, 9, 0});
            for (auto& kv : byKo)
            {
                t.tableRow({c.txt(std::to_string(kv.first)), c.cyan(gaLine[kv.first]),
                            t.koFlags(kv.second.cfg ? flagsActive(kv.second.flags) : "?"),
                            c.txt(prioName(kv.second.cfg, kv.second.prio)), c.txt(sizeName(kv.second.cfg, kv.second.sizeCode))},
                           {5, gaW, 12, 9, 0});
            }
            t.legend({{L.tr("C", "K"), L.tr("Comm", "Kommunikation")}, {L.tr("R", "L"), L.tr("Read", "Lesen")}, {L.tr("W", "S"), L.tr("Write", "Schreiben")}, {L.tr("T", "Ü"), L.tr("Transmit", "Übertragen")}, {L.tr("U", "A"), L.tr("Update", "Aktualisieren")}});
            t.note(L.tr("SIZE = object size read from the device; the semantic DPT is not stored on the device (ETS shows it from the product DB)",
                        "GRÖSSE = am Gerät gelesene Objektgröße; der semantische DPT liegt nicht am Gerät (ETS zeigt ihn aus der Produkt-DB)"));
            break;
        }
        case K_Df:
        {
            const FtcFsInfo& fs = openknxFileTransferClient.fsInfo();
            rc = fs.valid ? 0 : 1;
            if (quiet)
            {
                if (fs.valid)
                {
                    std::printf("total\t%u\nused\t%u\nfree\t%u\n", (unsigned)fs.total, (unsigned)fs.used, (unsigned)fs.free);
                }
                break;
            }
            if (!fs.valid)
            {
                t.status(ftc::Tpl::Stat::Err, target + " " + L.tr("no filesystem info", "keine Dateisystem-Info"), {});
                break;
            }
            const double usedFrac = fs.total ? (double)fs.used / (double)fs.total : 0.0;
            const uint64_t mul = fs.kb ? 1024ull : 1ull; // provider reports KB (>4 GB card); LittleFS reports bytes
            auto fmtSize = [](uint64_t bytes) -> std::string {
                char b[24];
                if (bytes >= 1024ull * 1024 * 1024) std::snprintf(b, sizeof(b), "%.1f GB", bytes / (1024.0 * 1024 * 1024));
                else if (bytes >= 1024 * 1024)
                    std::snprintf(b, sizeof(b), "%.1f MB", bytes / (1024.0 * 1024));
                else if (bytes >= 1024)
                    std::snprintf(b, sizeof(b), "%.0f KB", bytes / 1024.0);
                else
                    std::snprintf(b, sizeof(b), "%llu B", (unsigned long long)bytes);
                return std::string(b);
            };
            const std::string tb = fmtSize((uint64_t)fs.total * mul);
            char ub[48];
            std::snprintf(ub, sizeof(ub), "%s  (%.1f%%)", fmtSize((uint64_t)fs.used * mul).c_str(), usedFrac * 100.0);
            const std::string fb = fmtSize((uint64_t)fs.free * mul);
            ftc::Panel p(t, L.tr("Filesystem", "Dateisystem"), target);
            p.kv(L.tr("Total", "Gesamt"), c.txt(tb));
            p.kv(L.tr("Used", "Belegt"), c.txt(ub));
            p.kv(L.tr("Free", "Frei"), c.txt(fb));
            p.render(0);
            std::printf("  %s  %s\n", t.bar(usedFrac, 32, usedFrac > 0.9 ? 'r' : 'g').c_str(),
                        c.dim(std::string(ub)).c_str());
            break;
        }
        case K_Ll:
        case K_Ls:
        {
            rc = 0;
            const bool detailed = (k == K_Ll);
            if (quiet)
            {
                for (const auto& e : snap)
                {
                    if (detailed) std::printf("%s\t%s\t%u\t%08X\n", e.isDir ? "dir" : "file", e.name, (unsigned)e.size, (unsigned)e.crc);
                    else
                        std::printf("%s\t%s\n", e.isDir ? "dir" : "file", e.name);
                }
                break;
            }
            t.section(L.tr("Listing · ", "Verzeichnis · ") + target);
            if (snap.empty())
            {
                t.status(ftc::Tpl::Stat::Idle, L.tr("(empty)", "(leer)"));
                break;
            }
            if (detailed)
            {
                auto fmtSize = [](uint32_t b) -> std::string {
                    char s[16];
                    if (b >= 1024u * 1024 * 1024) std::snprintf(s, sizeof(s), "%.1f GB", b / 1073741824.0);
                    else if (b >= 1024u * 1024)
                        std::snprintf(s, sizeof(s), "%.1f MB", b / 1048576.0);
                    else if (b >= 1024)
                        std::snprintf(s, sizeof(s), "%.1f KB", b / 1024.0);
                    else
                        std::snprintf(s, sizeof(s), "%u B", (unsigned)b);
                    return s;
                };
                t.tableRow({c.dim(L.tr("MODE", "ART")), c.dim(L.tr("SIZE", "GRÖSSE")), c.dim("CRC"),
                            c.dim(L.tr("NAME", "NAME"))}, {5, -10, 10, 0});
                for (const auto& e : snap)
                {
                    char crc[12];
                    std::snprintf(crc, sizeof(crc), "%08X", (unsigned)e.crc);
                    std::string name = c.txt(e.name);
                    if (e.isOpenKnx) name += "  " + t.chip("OpenKNX");
                    std::string sizeCol = e.isDir ? c.dim("—") : c.txt(fmtSize(e.size));
                    std::string crcCol = e.isDir ? c.dim("—") : (e.hasCrc ? c.mut(crc) : c.dim("n/a"));
                    t.tableRow({e.isDir ? c.cyan(L.tr("dir", "Ordner")) : c.txt(L.tr("file", "Datei")),
                                sizeCol, crcCol, name}, {5, -10, 10, 0});
                }
            }
            else
            {
                for (const auto& e : snap)
                {
                    std::string name = e.isDir ? (c.cyan(std::string(e.name)) + c.dim("/")) : c.txt(e.name);
                    if (e.isOpenKnx) name += "  " + t.chip("OpenKNX");
                    std::printf("  %s %s\n", (e.isDir ? c.cyan(g_term.glyph("▸", ">")) : c.dim(g_term.glyph("·", "."))).c_str(), name.c_str());
                }
            }
            break;
        }
        case K_Scan:
        {
            rc = 0;
            // Drained BEFORE the quiet branch: -q used to break out first, so a scripted scan got the
            // identity read started and never collected it.
            // The devices were asked who they are while the sweep ran; wait out whatever is still in flight.
            std::unordered_map<std::string, ftc::DetailRow> det;
            if (pool)
            {
                const uint64_t t0 = nowMs();
                uint64_t drawn = 0;
                while (pool->outstanding() > 0 && !g_abort)
                {
                    const uint64_t now = nowMs();
                    if (g_term.isTty() && now - drawn > 60) // turn at the same rate as the sweep line
                    {
                        drawn = now;
                        const size_t done = pool->answered(), left = pool->outstanding();
                        const unsigned el = (unsigned)((now - t0) / 1000);
                        char cnt[32], clk[16];
                        std::snprintf(cnt, sizeof(cnt), "%u/%u", (unsigned)done, (unsigned)(done + left));
                        std::snprintf(clk, sizeof(clk), "%u:%02u", el / 60, el % 60);
                        liveLine(std::string("  ") + c.green(liveSpinner(now)) + " " +
                                 c.cyan(L.tr("reading devices", "lese Geräte")) + "   " + c.bold(cnt) + "   " +
                                 c.dim(clk));
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                if (g_abort) pool->abort();
                pool->wait();
                det = pool->rows();
                if (g_term.isTty()) std::fprintf(stderr, "\r\x1b[K");
                // What a device says about itself beats what the sweep guessed.
                for (auto& e : snap)
                {
                    const auto it = det.find(e.name);
                    if (it != det.end() && it->second.mfr == ftc::MFR_OPENKNX) e.isOpenKnx = true;
                }
            }
            if (quiet)
            {
                // One key, one tab, one value -- and the same facts the table shows, so a script does not
                // have to re-derive them: who owns which block, which hit is a tunnel slot, and the counts.
                for (const auto& o : owners)
                {
                    std::printf("ipdev\t%s\t%s\t%s", o.ia ? paToStr(o.ia).c_str() : "?", o.ip.c_str(), o.name.c_str());
                    if (o.refused) std::printf("\trefused");
                    else
                        for (uint16_t ta : o.tun) std::printf("\t%s", paToStr(ta).c_str());
                    std::printf("\n");
                }
                unsigned qTun = 0, qDev = 0, qAck = 0, qOkx = 0;
                for (const auto& e : snap)
                {
                    unsigned a2 = 0, l2 = 0, d2 = 0;
                    std::sscanf(e.name, "%u.%u.%u", &a2, &l2, &d2);
                    int slot = 0;
                    const IfaceOwner* own = ownerOf(owners, (uint16_t)((a2 << 12) | (l2 << 8) | d2), slot);
                    if (own && scanMask(e, &det) != 0) own = nullptr;
                    if (own)
                    {
                        std::printf("%s\ttunnel\t%s\t%d\n", e.name, paToStr(own->ia).c_str(), slot);
                        qTun++;
                        continue;
                    }
                    const uint16_t m = scanMask(e, &det);
                    const auto dit = det.find(e.name);
                    const ftc::DetailRow dr = dit != det.end() ? dit->second : ftc::DetailRow{};
                    // Same fields the table shows, in fixed positions: a caller must not have to run a
                    // second pass to learn what this scan already read.
                    // Field 10 is the load state as a NUMBER. Every other field here is raw or an
                    // untranslated name; a translated word would make the stream depend on the locale.
                    char ls[8] = {0};
                    if (dr.appState != 0xFF) std::snprintf(ls, sizeof(ls), "%u", (unsigned)dr.appState);
                    std::printf("%s\t0x%04X\t%s\t%d\t%s\t%s\t%s\t%s\t%s\t%s\n", e.name, (unsigned)m,
                                ftc::knxMaskName(m), e.isOpenKnx ? 1 : 0, ftc::orderText(dr.order).c_str(),
                                dr.bcuApp.c_str(), dr.version.c_str(), dr.ftm.c_str(), dr.serial.c_str(), ls);
                    if (m == 0) qAck++; else qDev++;
                    if (e.isOpenKnx) qOkx++;
                }
                // The rows are capped, so counting lines under-reports the line. Both numbers, always.
                std::printf("found\t%u\nshown\t%u\n", (unsigned)openknxFileTransferClient.scanFound(), (unsigned)snap.size());
                std::printf("devices\t%u\nopenknx\t%u\ntunnel_addrs\t%u\nacked_only\t%u\n", qDev, qOkx, qTun, qAck);
                break;
            }
            {
                // snap is capped (FTC_SCAN_MAX_LIST); scanFound() counted every responder. Printing snap.size()
                // as THE count turned a fully populated line into a confident "128 device(s)".
                const unsigned found = (unsigned)openknxFileTransferClient.scanFound();
                std::string note = std::to_string(snap.size()) + L.tr(" device(s)", " Gerät(e)");
                if (found > snap.size())
                    note += L.tr(" shown of ", " angezeigt von ") + std::to_string(found) +
                            L.tr(" found (list limit)", " gefunden (Listengrenze)");
                t.status(ftc::Tpl::Stat::Ok, L.tr("scan complete", "Scan fertig"), {note});
            }
            if (snap.empty()) break;

            renderScanTable(snap, det, owners);
            break;
        }
        case K_Simple:
        {
            const FtcStatus& st = openknxFileTransferClient.status();
            const bool ok = st.ok; // ftcSimpleCmd (rm/mkdir/rmdir/mv/format) sets ok on result byte 0x00
            rc = ok ? 0 : 1;
            if (quiet)
            {
                std::printf("%s\t%s\t%d\n", verb.c_str(), target.c_str(), ok ? 1 : 0);
                break;
            }
            if (ok) t.status(ftc::Tpl::Stat::Ok, verb + " " + L.tr("ok", "ok"),
                             {target, st.message[0] ? std::string(st.message) : std::string()});
            else
                t.status(ftc::Tpl::Stat::Err, verb + " " + L.tr("failed", "fehlgeschlagen"),
                         {target, st.message[0] ? std::string(st.message) : std::string()});
            break;
        }
        default: return false;
    }
    return true;
}

/**********************************************************************
 ******************* ARG PARSING · DISPATCH · MAIN ******************
 **********************************************************************/

/**
 * @brief The oknx entry point: init, parse args, dispatch the command, run its loop, and shut down cleanly.
 */
int main(int argc, char** argv)
{
    g_selfPath = (argc > 0 && argv[0]) ? argv[0] : "oknx"; // re-invoked for parallel-scan children
    if (!socketStartup())
    {
        std::fprintf(stderr, "fatal: socket init failed\n");
        return 1;
    }
    g_color = initTerminal(); // UTF-8 + ANSI, TTY-aware, NO_COLOR-honoring
    // Config file (~/.config/oknx/config.toml) — the persisted defaults. Precedence: CLI flags > config > env.
    g_cfg.load();
    if (!g_cfg.get("theme").empty()) g_theme.select(g_cfg.get("theme").c_str());
    if (g_cfg.get("ascii") == "true") g_term.setAscii(true);
    // Pre-scan so --lang / --ascii take effect even before --help / --version print (order-independent).
    g_i18n.detect(nullptr);                                                   // env/locale base
    if (!g_cfg.get("lang").empty()) g_i18n.detect(g_cfg.get("lang").c_str()); // config overrides env/locale
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--lang") == 0 && i + 1 < argc) g_i18n.detect(argv[i + 1]);
        else if (std::strcmp(argv[i], "--theme") == 0 && i + 1 < argc)
            g_theme.select(argv[i + 1]); // --theme overrides config
        else if (std::strcmp(argv[i], "--ascii") == 0)
            g_term.setAscii(true);
    }
    OpenKNX::Log::Logger::setLineHook(&ftcLineHook); // route the embedded client's body lines through Stage-2 reformat
    std::signal(SIGINT, onAbortSignal);              // Ctrl+C -> graceful cancel (portable: Win CRT maps console Ctrl+C to SIGINT)
    std::signal(SIGTERM, onAbortSignal);             // kill/terminate -> same graceful path

    std::string ip;
    uint16_t port = 3671;
    bool discover = false;
    bool verbose = false;         // --verbose: read + print the FULL interface/target steckbrief before the command
    bool quiet = false;           // --quiet: suppress ALL chrome (tunnel-up, iface identity, console hints) for scripting
    bool logRequested = false;    // --log: tee the console session to a file
    std::string logPathArg;       // --log=<path>: explicit log file (else auto in the home dir)
    int monFrames = 0;            // --frames N: stop a monitor after N frames (0 = unlimited; for scripting/tests)
    int monSeconds = 0;           // --seconds N: stop a monitor after N seconds (0 = unlimited)
    bool secondsSet = false;      // was --seconds given? (ps: tells default 3 s apart from an explicit --seconds 0 = loop)
    int cmpGrace = 750;           // --grace N: A/B compare match window in ms
    bool cmpMulti = false;        // --multi: start the compare in diff-off "multi" mode
    bool cmpRaw = false;          // --raw: start the compare with normalize OFF (raw per-piece frames)
    bool cmpOnlyDiff = false;     // --only-diff: compare view starts filtered to divergences only
    bool cmpCollapse = false;     // --collapse: compare view starts collapsing consecutive identical frames
    bool cmpMarkers = true;       // --no-markers: start with per-frame markers off (default on)
    bool cmpSkew = false;         // --skew: compare view starts showing the A/B time-skew
    std::string prioName = "low"; // --prio low|normal|urgent|system: KNX priority of the FTC frames we send
    uint8_t prio2 = 0x03;         // the 2-bit CTRL value (low=3 · normal=1 · urgent=2 · system=0)
    bool prioForce = false;       // --prio-force: confirm an elevated priority in a non-TTY/scripted run
    bool installSystem = false;   // --system: install/uninstall to the system dir (/usr/local/bin) instead of ~/.local/bin
    bool installForce = false;    // --force-install: install even when it would downgrade a newer installed copy
    bool knxotaForce = false;     // --force: knxOTA may downgrade, or accept a file that states no identity
    bool knxotaCheck = false;     // --check / --dry-run: compare and report, never write
    bool knxotaNoCompress = false; // --no-compress: transfer the image as it is
    std::string knxotaFrom;        // --from: the release the device is running now (base for a difference)
    bool knxotaNoDelta = false;    // --no-delta: send the whole image even when a difference would do
    bool knxotaKeepTemp = false;   // --keep-temp: do not delete the prepared payload
    bool fileBrowser = false;      // --file-browser: open the file chooser and print what was picked
    bool knxotaScan = false;       // the assistant will search the bus for a target once connected
    bool knxotaScanAsk = false;    // ...and lets the user name the line first (c instead of s)
    bool showAllPick = false;      // the picker lists only what knxOTA can update; `alle` lifts that
    std::string knxotaLine;        // the line last searched: searching again must not fall back to another one
    bool knxotaResume = false;     // continuing an unfinished run: every answer is already known
    uint16_t knxotaResumePa = 0;   // ...including the target, so the device search is skipped too
    std::string installDirArg;    // --dir <path>: explicit install/uninstall directory (overrides the default)
    std::vector<std::string> pos; // positional tokens -> the `oknx ...` command tail

    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        // Bundled single-char flags, Unix-style: -VD == -V -D. Only the VALUELESS short flags bundle
        // (h v V q D); value-taking options are long (--ip …) and never bundle. A single -x falls through.
        if (a.size() > 2 && a[0] == '-' && a[1] != '-' && a.find_first_not_of("hvVqD", 1) == std::string::npos)
        {
            for (size_t k = 1; k < a.size(); ++k)
            {
                switch (a[k])
                {
                    case 'h':
                        usage();
                        socketCleanup();
                        return 0;
                    case 'v':
                        printVersion();
                        socketCleanup();
                        return 0;
                    case 'V': verbose = true; break;
                    case 'q': quiet = true; break;
                    case 'D': discover = true; break;
                }
            }
            continue;
        }
        if (a == "--examples")
        {
            examples();
            socketCleanup();
            return 0;
        }
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
        else if (a == "--discover" || a == "-D")
            discover = true;
        else if (a == "--ui-demo")
        {
            renderUiDemo();
            socketCleanup();
            return 0;
        }
        else if (a == "--verbose" || a == "-V")
            verbose = g_verbose = true;
        else if (a == "--quiet" || a == "-q")
            quiet = g_quiet = true;
        else if (a == "--log")
            logRequested = true;
        else if (a.rfind("--log=", 0) == 0)
        {
            logRequested = true;
            logPathArg = a.substr(6);
        }
        else if ((a == "--ip" || a == "-i") && i + 1 < argc)
            ip = argv[++i];
        else if (a == "--port" && i + 1 < argc)
            port = (uint16_t)std::atoi(argv[++i]);
        else if ((a == "--workers" || a == "-W") && i + 1 < argc)
            g_workers = std::atoi(argv[++i]); // identity readers for `details`/`openknx`; each takes one tunnel
        // Retired with the parallel range scan. Named explicitly because a long option after a command is
        // otherwise handed to the shared parser, which does not know it and would drop it without a word.
        else if (a == "--tunnels" || a == "-T")
        {
            std::fprintf(stderr, "%s\n", g_i18n.tr("error: --tunnels is gone. The parallel range scan was removed (it returned a "
                                                   "different subset every run); --workers N now sets the identity readers.",
                                                   "Fehler: --tunnels gibt es nicht mehr. Der parallele Bereichsscan ist entfernt (er lieferte "
                                                   "jeden Lauf eine andere Teilmenge); --workers N setzt jetzt die Identitaets-Leser."));
            socketCleanup();
            return 1;
        }
        else if (a == "--_probeslot")
            g_probeSlot = true; // internal: tunnel-slot probe child -> connect, emit SLOT, hold, exit
        else if (a == "--frames" && i + 1 < argc)
            monFrames = std::atoi(argv[++i]);
        else if (a == "--seconds" && i + 1 < argc)
        {
            monSeconds = std::atoi(argv[++i]);
            secondsSet = true;
        }
        else if (a == "--grace")
        {
            // only consume the next token if it's a number, so `--grace --multi` keeps the default (no flag-eating)
            if (i + 1 < argc && isdigit((unsigned char)argv[i + 1][0])) cmpGrace = std::atoi(argv[++i]);
        }
        else if (a == "--multi")
            cmpMulti = true; // start the compare in diff-off "multi" mode
        else if (a == "--raw")
            cmpRaw = true; // start the compare with normalize OFF (raw per-piece frames)
        else if (a == "--only-diff")
            cmpOnlyDiff = true; // compare: start filtered to divergences only
        else if (a == "--collapse")
            cmpCollapse = true; // compare: start collapsing consecutive identical frames
        else if (a == "--no-markers")
            cmpMarkers = false; // compare: start with per-frame markers off
        else if (a == "--skew")
            cmpSkew = true; // compare: start showing the A/B time-skew
        else if (a == "--prio" && i + 1 < argc)
        {
            prioName = argv[++i];
            if (prioName == "low") prio2 = 3;
            else if (prioName == "normal")
                prio2 = 1;
            else if (prioName == "urgent")
                prio2 = 2;
            else if (prioName == "system")
                prio2 = 0;
            else
            {
                std::fprintf(stderr, "%s: %s\n", g_i18n.tr("unknown --prio (use low|normal|urgent|system)",
                                                           "unbekanntes --prio (low|normal|urgent|system)"),
                             prioName.c_str());
                socketCleanup();
                return 2;
            }
        }
        else if (a == "--prio-force")
            prioForce = true; // confirm an elevated priority in a non-TTY / scripted run (no prompt)
        else if (a == "--system")
            installSystem = true; // install/uninstall to the system dir instead of the per-user default
        else if (a == "--force-install")
            installForce = true; // allow a downgrade install without prompting (scripts)
        else if (a == "--force")
            knxotaForce = true; // knxOTA: allow a downgrade / an unidentifiable file
        else if (a == "--check" || a == "--dry-run")
            knxotaCheck = true; // knxOTA: read-only — compare and report, write nothing
        else if (a == "--no-compress")
            knxotaNoCompress = true; // send the firmware as it is, even to a device that could unpack one
        else if (a == "--from" && i + 1 < argc)
            knxotaFrom = argv[++i]; // knxOTA: the release the device runs now -> send only the difference
        else if (a == "--no-delta")
            knxotaNoDelta = true; // always send the whole image, even when a difference would do
        else if (a == "--keep-temp")
            knxotaKeepTemp = true; // leave the prepared payload on disk for inspection
        else if (a == "--file-browser" || a == "--browse")
            fileBrowser = true; // open the chooser on its own, no bus involved
        else if (a == "--dir" && i + 1 < argc)
            installDirArg = argv[++i]; // explicit install/uninstall directory
        else if (a == "--lang" && i + 1 < argc)
            ++i; // consumed here; already applied in the pre-scan above
        else if (a == "--theme" && i + 1 < argc)
            ++i; // consumed here; already applied in the pre-scan above
        else if (a == "--ascii")
        { /* consumed here; already applied in the pre-scan above */
        }
        // A `-x` token AFTER the command belongs to the command (the shared parser reads -faknqv/-v0..2);
        // only letters that parser knows pass through, so a mistyped host option is still reported here.
        else if (!pos.empty() && a.size() > 1 && a[0] == '-' &&
                 a.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-=", 1) == std::string::npos)
            pos.push_back(a); // the shared parser owns this vocabulary, so let it name the problem
        // Same for a long option after the command (--mode/--pkg/--window …): the shared parser owns it and
        // reports an unknown one. Before any command (`oknx --bogus`) there is no owner -> reported here.
        else if (!pos.empty() && a.size() > 2 && a[0] == '-' && a[1] == '-')
            pos.push_back(a);
        else if (!a.empty() && a[0] == '-' && !(a.size() > 1 && (isdigit((unsigned char)a[1]))))
        {
            std::fprintf(stderr, "unknown option: %s\n", a.c_str());
            socketCleanup();
            return 1;
        }
        else
            pos.push_back(a);
    }


    // --- internal: tunnel-slot probe child --------------------------------------------------------
    // Open ONE tunnel; on success print the assigned free PA and hold it briefly so sibling probes each get a
    // *distinct* free slot, then release. The parent (renderInterfaceInfo) collects the reported PAs = free.
    if (g_probeSlot)
    {
        int rc = 3; // 3 = refused / no free slot
        if (ip.empty() || !g_knxTunnel.connect(ip, port))
        {
            // The reason matters to the parent: 0x24 = the interface says it is full, anything else
            // (notably -1 = no CONNECT_RESPONSE) means this probe failed, not that the slot is taken.
            std::printf("NOSLOT\t%d\n", g_knxTunnel.lastConnectStatus());
            std::fflush(stdout);
        }
        else
        {
            const uint16_t a = g_knxTunnel.assignedPA();
            std::printf("SLOT\t%u.%u.%u\n", (a >> 12) & 0x0F, (a >> 8) & 0x0F, a & 0xFF);
            std::fflush(stdout);
            // Hold long enough that ALL staggered siblings are connected simultaneously (16 slots * 60 ms stagger
            // ~= 1 s span), so the interface hands each a DISTINCT free address -> the union = every free slot.
            for (int i = 0; i < 130; ++i)
            {
                g_knxTunnel.pump();
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            } // ~2.6 s hold
            g_knxTunnel.disconnect();
            rc = 0;
        }
        socketCleanup();
        return rc;
    }

    // --- discovery: no tunnel needed --------------------------------------------------------------
    if (discover)
    {
        if (verbose)
        {
            // --verbose: read + print the FULL steckbrief of every discovered interface (DESCRIPTION +
            // device-mgmt APDU, per interface) instead of the one-line list.
            ftc::Theme& c = g_theme;
            ftc::I18n& L = g_i18n;
            std::printf("  %s\n", c.dim(L.tr("searching 224.0.23.12:3671 …", "suche 224.0.23.12:3671 …")).c_str());
            const auto ifaces = ftc::discoverInterfaces(port, 3000);
            if (ifaces.empty())
            {
                std::printf("  %s\n", c.dim(L.tr("(no interfaces responded)", "(keine Interfaces geantwortet)")).c_str());
                discoverEmptyHint(c, L);
                socketCleanup();
                return 1;
            }
            for (const auto& f : ifaces)
            {
                ftc::IfaceDesc d;
                ftc::queryInterface(f.ip, port, d);
                d.apduReported = queryMaxApduDeviceMgmt(f.ip, port, &d.apduReportedPid, d.apduReason, sizeof(d.apduReason)); // zero bus traffic
                printIfacePanel(f.ip, d);
            }
            socketCleanup();
            return 0;
        }
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

    // `oknx ?` / `oknx help` -> the same usage as --help (the device console uses `ftc ?`, so accept it here too).
    if (pos[0] == "?" || pos[0] == "help")
    {
        usage();
        socketCleanup();
        return 0;
    }

    // `oknx progscan|ps locate` — self-discovering prog-mode localisation (no --ip). Enumerate every KNXnet/IP
    // interface/router on the LAN, then LOCAL-scan (hop 0) each one: a device in prog mode answers only through
    // the interface on its OWN TP line, so the responding router + the responder IA's area.line pin the line.
    if ((pos[0] == "progscan" || pos[0] == "ps") && pos.size() >= 2 && pos[1] == "locate")
    {
        ftc::Theme& c = g_theme;
        ftc::I18n& L = g_i18n;
        const int secs = monSeconds > 0 ? monSeconds : 3;
        if (!quiet) g_tpl.section(L.tr("progscan locate · discover + per-line prog-mode scan",
                                       "progscan locate · Suche + Prog-Modus-Scan je Linie"),
                                  -1);
        const auto ifaces = ftc::discoverInterfaces(port, 3000);
        if (ifaces.empty())
        {
            if (!quiet) g_tpl.status(ftc::Tpl::Stat::Idle, L.tr("no KNXnet/IP interfaces found",
                                                                "keine KNXnet/IP-Interfaces gefunden"));
            else
                std::fprintf(stderr, "no interfaces\n");
            socketCleanup();
            return 0;
        }
        int total = 0;
        for (const auto& f : ifaces)
        {
            if (g_abort) break;
            if (!quiet)
                g_tpl.status(ftc::Tpl::Stat::Info, c.txt(f.ip) + "  " + c.dim(f.name),
                             {std::string(L.tr("local scan ", "Lokal-Scan ")) + std::to_string(secs) + "s"});
            ftc::ProgScan ps(g_term, g_theme, g_tpl, g_i18n);
            ps.target(f.ip, port);
            std::vector<ftc::ProgHit> hits;
            std::string err;
            bool aborted = false;
            if (ps.scanOnce(ftc::ProgScan::Mode::Local, secs, monFrames, hits, err, &g_abort, &aborted) != 0)
            {
                if (!quiet) g_tpl.status(ftc::Tpl::Stat::Warn, c.dim(err), {f.ip});
                continue;
            }
            for (const auto& h : hits)
            {
                total++;
                const std::string pa = ftc::Tpl::pa(h.ia);
                char ln[8];
                std::snprintf(ln, sizeof(ln), "%u.%u", (h.ia >> 12) & 0x0F, (h.ia >> 8) & 0x0F);
                if (quiet)
                    std::printf("%s\t%s\t%s\n", pa.c_str(), ln, f.ip.c_str());
                else
                    g_tpl.tableRow({g_tpl.statusDot('g'), c.bold(pa), c.cyan(ln),
                                    c.dim(std::string(L.tr("line ", "Linie ")) + ln + L.tr(" · via ", " · über ")) +
                                        c.txt(f.ip)},
                                   {2, 10, 8, 0});
            }
            if (aborted) break;
        }
        if (!quiet)
        {
            if (total == 0)
                g_tpl.status(ftc::Tpl::Stat::Idle, L.tr("no device in programming mode on any line",
                                                        "kein Gerät im Programmiermodus auf einer Linie"));
            else
                g_tpl.status(ftc::Tpl::Stat::Ok,
                             std::to_string(total) + " " + L.tr("device(s) located", "Gerät(e) lokalisiert"), {});
        }
        socketCleanup();
        return g_abort ? 130 : 0;
    }

    // `oknx gzip <in> <out>` — in-process gzip via miniz (RP firmware prep; no tunnel needed).
    if (pos[0] == "gzip")
    {
        ftc::I18n& L = g_i18n;
        if (pos.size() < 3)
        {
            std::fprintf(stderr, "%s\n", L.tr("usage: oknx gzip <in> <out>", "Aufruf: oknx gzip <ein> <aus>"));
            socketCleanup();
            return 1;
        }
        const bool ok = ftc::gzipFile(pos[1], pos[2]);
        if (ok)
            std::printf("  %s %s %s %s\n", g_theme.green(g_term.glyph("●", "*")).c_str(), g_theme.txt(pos[1]).c_str(),
                        g_theme.dim(g_term.glyph("→", "->")).c_str(), g_theme.green(pos[2]).c_str());
        else
            std::fprintf(stderr, "%s\n", L.tr("gzip failed", "gzip fehlgeschlagen"));
        socketCleanup();
        return ok ? 0 : 1;
    }

    // `oknx delta make|show|apply` — build, inspect and replay an OKD1 difference file. All three run
    // locally: no tunnel, no device. `apply` drives the interpreter the firmware compiles, so a round
    // trip here exercises the device path rather than a host-only lookalike.
    if (pos[0] == "delta")
    {
        ftc::I18n& L = g_i18n;
        auto slurp = [](const std::string& path, std::vector<uint8_t>& out) {
            std::FILE* f = std::fopen(path.c_str(), "rb");
            if (f == nullptr) return false;
            std::fseek(f, 0, SEEK_END);
            const long n = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            if (n < 0) { std::fclose(f); return false; }
            out.resize((size_t)n);
            const bool ok = n == 0 || std::fread(out.data(), 1, (size_t)n, f) == (size_t)n;
            std::fclose(f);
            return ok;
        };
        auto spill = [](const std::string& path, const std::vector<uint8_t>& in) {
            std::FILE* f = std::fopen(path.c_str(), "wb");
            if (f == nullptr) return false;
            const bool ok = in.empty() || std::fwrite(in.data(), 1, in.size(), f) == in.size();
            std::fclose(f);
            return ok;
        };
        const std::string verb = pos.size() > 1 ? pos[1] : "";

        if (verb == "make" && pos.size() >= 5)
        {
            std::vector<uint8_t> oldImg, newImg, patch;
            if (!slurp(pos[2], oldImg)) { std::fprintf(stderr, "%s: %s\n", L.tr("cannot read", "nicht lesbar"), pos[2].c_str()); socketCleanup(); return 1; }
            if (!slurp(pos[3], newImg)) { std::fprintf(stderr, "%s: %s\n", L.tr("cannot read", "nicht lesbar"), pos[3].c_str()); socketCleanup(); return 1; }
            bool wantPack = false;
            for (size_t i = 5; i < pos.size(); ++i)
                if (pos[i] == "--pack") wantPack = true;
            ftc::delta::Stats st;
            if (!ftc::delta::make(oldImg, newImg, patch, &st))
            {
                std::fprintf(stderr, "%s\n", L.tr("building the patch failed", "Patch konnte nicht gebaut werden"));
                socketCleanup();
                return 1;
            }
            if (wantPack)
            {
                std::vector<uint8_t> packed;
                if (!ftc::delta::pack(patch, packed))
                {
                    std::fprintf(stderr, "%s\n", L.tr("packing the patch failed", "Patch konnte nicht gepackt werden"));
                    socketCleanup();
                    return 1;
                }
                st.patchBytes = (uint32_t)packed.size();
                patch.swap(packed);
            }
            if (!spill(pos[4], patch))
            {
                std::fprintf(stderr, "%s\n", L.tr("building the patch failed", "Patch konnte nicht gebaut werden"));
                socketCleanup();
                return 1;
            }
            std::printf("  %s %s %s %s  %u B (%.2f %% %s)\n", g_theme.green(g_term.glyph("●", "*")).c_str(),
                        g_theme.txt(pos[3]).c_str(), g_theme.dim(g_term.glyph("→", "->")).c_str(),
                        g_theme.green(pos[4]).c_str(), (unsigned)st.patchBytes,
                        newImg.empty() ? 0.0 : 100.0 * (double)st.patchBytes / (double)newImg.size(),
                        L.tr("of the image", "des Images"));
            std::printf("    %s  copy %u/%u B  add %u/%u B  ops %u B\n", g_theme.dim(L.tr("detail", "Detail")).c_str(),
                        (unsigned)st.copyOps, (unsigned)st.copyBytes, (unsigned)st.addOps,
                        (unsigned)st.literalBytes, (unsigned)st.opsBytes);
            socketCleanup();
            return 0;
        }

        if (verb == "show" && pos.size() >= 3)
        {
            std::vector<uint8_t> patch;
            ftc::delta::Info info;
            if (!slurp(pos[2], patch) || !ftc::delta::describe(patch, info))
            {
                std::fprintf(stderr, "%s\n", L.tr("not a patch file", "keine Patch-Datei"));
                socketCleanup();
                return 1;
            }
            std::printf("  version %u  flags 0x%02X  header %s\n", info.version, info.flags,
                        info.headerOk ? "ok" : g_theme.red("damaged").c_str());
            std::printf("  source  %u B  crc %08X\n", (unsigned)info.srcLen, (unsigned)info.srcCrc);
            std::printf("  target  %u B  crc %08X\n", (unsigned)info.dstLen, (unsigned)info.dstCrc);
            std::printf("  streams ops %u B  literals %u B  file %u B\n", (unsigned)info.opsLen,
                        (unsigned)info.litLen, (unsigned)patch.size());
            socketCleanup();
            return info.headerOk ? 0 : 1;
        }

        if (verb == "apply" && pos.size() >= 5)
        {
            std::vector<uint8_t> oldImg, patch, rebuilt;
            if (!slurp(pos[2], oldImg) || !slurp(pos[3], patch))
            {
                std::fprintf(stderr, "%s\n", L.tr("cannot read the inputs", "Eingaben nicht lesbar"));
                socketCleanup();
                return 1;
            }
            // Optional source limit: on a device the patch may never read past the region that holds the
            // image, so the self-test needs a way to exercise that refusal here.
            uint32_t limit = 0xFFFFFFFFu;
            for (size_t i = 5; i + 1 < pos.size(); ++i)
                if (pos[i] == "--limit") limit = (uint32_t)strtoul(pos[i + 1].c_str(), nullptr, 0);
            uint8_t err = 0;
            if (!ftc::delta::apply(oldImg, patch, rebuilt, err, limit))
            {
                std::fprintf(stderr, "%s %u\n", L.tr("apply refused, reason", "Anwenden verweigert, Grund"), err);
                socketCleanup();
                return 10 + err; // distinct exit code per reason, for the self-test
            }
            if (!spill(pos[4], rebuilt))
            {
                std::fprintf(stderr, "%s: %s\n", L.tr("cannot write", "nicht schreibbar"), pos[4].c_str());
                socketCleanup();
                return 1;
            }
            std::printf("  %s %s  %u B\n", g_theme.green(g_term.glyph("●", "*")).c_str(),
                        g_theme.green(pos[4]).c_str(), (unsigned)rebuilt.size());
            socketCleanup();
            return 0;
        }

        std::fprintf(stderr, "%s\n", L.tr("usage: oknx delta make <old.bin> <new.bin> <out.okd>",
                                          "Aufruf: oknx delta make <alt.bin> <neu.bin> <aus.okd>"));
        std::fprintf(stderr, "%s\n", L.tr("       oknx delta show <patch.okd>", "       oknx delta show <patch.okd>"));
        std::fprintf(stderr, "%s\n", L.tr("       oknx delta apply <old.bin> <patch.okd> <rebuilt.bin>",
                                          "       oknx delta apply <alt.bin> <patch.okd> <neu.bin>"));
        socketCleanup();
        return 1;
    }

    // knxOTA exit codes, and the difference matters to whoever calls it:
    //   0   installed, or there was nothing to install
    //   1   refused -- this firmware does not belong to this device
    //   2   the command line or the file was unusable
    //   3   the device is not accepting writes (also: it refused the transfer)
    //   4   the transfer did not complete -- the reason is in the result panel
    //   6   the device did not come back after the restart
    //   130 the user stopped it: quit at a chooser, or answered no. NOTHING was written.
    // --- knxOTA: `oknx [--ip <ip>] [<pa>] knxota <local firmware file>` ------------------------------
    // `knxota` takes a firmware file from THIS machine and walks the whole update; `<pa> fwupdate <remote>`
    // only flashes something already uploaded. The file is read/checked/prepared BEFORE a tunnel slot is taken.
    auto lower = [](std::string v) {
        for (char& ch : v) ch = (char)std::tolower((unsigned char)ch);
        return v;
    };
    size_t knxotaVerb = pos.size();
    size_t fwupdateVerb = pos.size();
    for (size_t i = 0; i < pos.size(); ++i)
    {
        const std::string v = lower(pos[i]);
        // We call it knxOTA everywhere, but the two halves are easy to swap from memory — accept both
        // spellings rather than answer a near miss with "unknown command".
        if ((v == "knxota" || v == "otaknx") && knxotaVerb == pos.size()) knxotaVerb = i;
        if (v == "fwupdate" && fwupdateVerb == pos.size()) fwupdateVerb = i;
    }
    ftc::FwFile knxotaFw;
    bool knxotaActive = knxotaVerb + 1 < pos.size();
    bool knxotaHeaderShown = false;
    // `oknx knxota` with nothing behind it. The firmware file is the ONE thing this command cannot work out
    // for itself -- the interface is discovered and the device is searched for, both without being asked
    // for on the command line. Answering the file with an error while asking for everything else was the
    // wrong way round, so it opens the same chooser the base picker uses.
    if (!knxotaActive && knxotaVerb < pos.size())
    {
        ftc::I18n& L = g_i18n;
        if (!g_term.isTty() || quiet)
        {
            if (!quiet) g_ui.banner();
            std::fflush(stdout);
            g_ui.errorBlock(false, L.tr("knxOTA needs a firmware file", "knxOTA braucht eine Firmware-Datei"),
                            {L.tr("there is no terminal here to pick one in",
                                  "hier ist kein Terminal, um eine auszuwählen")},
                            "oknx knxota <firmware.uf2|.bin|.gz>");
            socketCleanup();
            return 2;
        }
        g_ui.banner();
        g_tpl.section(L.tr("knxOTA · firmware update over the KNX bus",
                           "knxOTA · Firmware-Update über den KNX-Bus"));
        knxotaHeaderShown = true;
        // Short on purpose: this sits above the chooser, and a line that wraps on an 80-column terminal
        // pushes the table it introduces out of view.
        g_tpl.status(ftc::Tpl::Stat::Idle, L.tr("which firmware file?", "welche Firmware-Datei?"),
                     {L.tr("RP2040/RP2350 -> .uf2 · ESP32 -> .bin", "RP2040/RP2350 -> .uf2 · ESP32 -> .bin")});
        ftc::BrowseSpec spec;
        spec.title = "knxOTA";
        spec.suffixes.push_back(".uf2");
        spec.suffixes.push_back(".bin"); // ESP application image, and the .factory.bin beside its .image.txt
        spec.suffixes.push_back(".gz");  // an image already packed for the bus
        spec.allowFilePick = true;
        spec.allowDirPick = false; // a folder holds several variants; which one is the update is a decision
        spec.roots = ftc::driveRoots();
        spec.note = [](const std::string& path, bool isDir) -> std::string {
            if (isDir) return ftc::countAppImages(std::filesystem::path(path)) > 0
                                  ? g_i18n.tr("release images inside", "Release-Images darin")
                                  : std::string();
            return (path.size() > 4 && path.compare(path.size() - 4, 4, ".uf2") == 0)
                       ? g_i18n.tr("RP2040/RP2350 firmware", "RP2040/RP2350-Firmware")
                       : g_i18n.tr("firmware image", "Firmware-Image");
        };
        std::string picked;
        if (!ftc::browse(g_term, g_theme, L, spec, picked) || picked.empty())
        {
            std::fprintf(stderr, "  %s\n", g_theme.dim(L.tr("nothing picked - nothing was changed",
                                                            "nichts gewählt - es wurde nichts verändert")).c_str());
            socketCleanup();
            return 130;
        }
        pos.insert(pos.begin() + (long)knxotaVerb + 1, picked); // the rest of the flow reads pos[knxotaVerb+1]
        knxotaActive = true;
    }

    // `fwupdate` pointed at a file that lives here cannot mean the remote trigger — say which word does it.
    if (!knxotaActive && fwupdateVerb + 1 < pos.size())
    {
        const std::string& src = pos[fwupdateVerb + 1];
        const std::string ext = ftc::fileExt(src);
        std::error_code fec;
        if (ext == ".uf2" || ((ext == ".bin" || ext == ".gz") && std::filesystem::is_regular_file(src, fec)))
        {
            ftc::I18n& L = g_i18n;
            if (!quiet) g_ui.banner();
            std::fflush(stdout); // the block goes to stderr: flush first so a piped run keeps the order
            g_ui.errorBlock(false,
                            L.tr("fwupdate flashes a file the device already has",
                                 "fwupdate flasht eine Datei, die das Gerät bereits hat"),
                            {L.tr("this file is on this computer, not on the device",
                                  "diese Datei liegt auf diesem Rechner, nicht auf dem Gerät")},
                            std::string("oknx knxota ") + src);
            socketCleanup();
            return 2;
        }
    }
    // `oknx knxota resume [list|clear <pa>|clear all]` -- the unfinished runs, without touching a bus.
    // Caught before readFirmware, because "resume" is a word, not a firmware file.
    if (knxotaActive && pos[knxotaVerb + 1] == "resume")
    {
        ftc::I18n& L = g_i18n;
        ftc::Theme& c = g_theme;
        const std::string sub = (knxotaVerb + 2 < pos.size()) ? pos[knxotaVerb + 2] : std::string();

        if (sub == "clear")
        {
            const std::string what = (knxotaVerb + 3 < pos.size()) ? pos[knxotaVerb + 3] : std::string();
            if (what.empty())
            {
                std::fprintf(stderr, "usage: oknx knxota resume clear <pa>|all\n");
                socketCleanup();
                return 2;
            }
            const uint16_t pa = (what == "all") ? (uint16_t)0 : paFromText(what);
            if (what != "all" && pa == 0)
            {
                std::fprintf(stderr, "%s: %s\n", L.tr("not an individual address", "keine physikalische Adresse"),
                             what.c_str());
                socketCleanup();
                return 2;
            }
            const size_t gone = ftc::otaResumeEraseWhere(otaResumePath(), pa);
            char b[96];
            std::snprintf(b, sizeof(b), "%u", (unsigned)gone);
            if (!quiet)
                g_tpl.status(gone ? ftc::Tpl::Stat::Ok : ftc::Tpl::Stat::Idle,
                             std::string(b) + L.tr(" entr(y/ies) removed", " Eintrag/Einträge entfernt"), {});
            socketCleanup();
            return 0;
        }

        std::vector<ftc::OtaSession> all = ftc::otaResumeLoad(otaResumePath(), otaLegacyPath());
        ftc::otaResumeAge(all, (uint64_t)std::time(nullptr));

        if (sub == "list" || quiet || !g_term.isTty())
        {
            // One line per entry, tab separated -- for a script, not for reading.
            for (const ftc::OtaSession& e : all)
                std::printf("%s\t%s\t%s\t%u\t%u\t%llu\n", paToStr(e.pa).c_str(), e.ip.c_str(),
                            e.file.c_str(), (unsigned)e.done, (unsigned)e.total,
                            (unsigned long long)e.when);
            socketCleanup();
            return all.empty() ? 1 : 0;
        }

        if (!quiet) g_ui.banner();
        const int rc = runResumeManager(all, c, L);
        socketCleanup();
        return rc;
    }

    if (knxotaActive)
    {
        ftc::I18n& L = g_i18n;
        ftc::Theme& c = g_theme;
        if (!knxotaHeaderShown) // the chooser above already opened with both, do not repeat them
        {
            if (!quiet) g_ui.banner();
            g_tpl.section(L.tr("knxOTA · firmware update over the KNX bus",
                               "knxOTA · Firmware-Update über den KNX-Bus"));
        }
        if (!ftc::readFirmware(pos[knxotaVerb + 1], knxotaFw, L))
        {
            g_ui.errorBlock(false, L.tr("this file cannot be used", "diese Datei ist nicht verwendbar"),
                            {knxotaFw.error},
                            L.tr("OpenKNX releases ship the .uf2 for RP2040/RP2350 devices and the .bin for ESP32",
                                 "OpenKNX-Releases liefern die .uf2 für RP2040/RP2350-Geräte und die .bin für ESP32"));
            socketCleanup();
            return 2;
        }
        ftc::renderFirmwarePanel(g_tpl, c, L, knxotaFw, verbose);
        if (!knxotaFw.id.valid && !knxotaForce)
            g_tpl.status(ftc::Tpl::Stat::Warn,
                         L.tr("this file does not say which device it is for",
                              "diese Datei sagt nicht, für welches Gerät sie ist"),
                         {L.tr("the version cannot be compared", "die Version kann nicht verglichen werden")});
        // Read-only and no device given: on a terminal the assistant still asks — "do not write" is not
        // "do not ask". Only a scripted run stops at the file, because there is nobody to answer.
        if (knxotaCheck && ip.empty() && !g_term.isTty())
        {
            g_tpl.note(L.tr("nothing was transferred · add --ip <interface> <address> to compare with a device",
                            "es wurde nichts übertragen · mit --ip <Interface> <Adresse> gegen ein Gerät vergleichen"));
            std::printf("\n");
            socketCleanup();
            return 0;
        }
        // An unfinished run for THIS firmware: everything it asked is already answered, so offer to carry on.
        // Everything that would be reused is shown -- resuming blind would be worse than asking again.
        {
            std::vector<ftc::OtaSession> runs = ftc::otaResumeLoad(otaResumePath(), otaLegacyPath());
            ftc::otaResumeAge(runs, (uint64_t)std::time(nullptr));
            const uint32_t crc = ftc::otaCrc32(knxotaFw.payload.data(), knxotaFw.payload.size());
            // A named target narrows it further. Without one, every entry for this firmware is a
            // candidate -- that is exactly the case the old single record could not represent, and it
            // used to offer a run for a DIFFERENT device just because the image matched.
            const uint16_t wantPa = (knxotaVerb > 0) ? paFromText(pos[0]) : (uint16_t)0;
            std::vector<ftc::OtaSession> hits;
            for (const ftc::OtaSession& e : runs)
                if (e.crc == crc && (wantPa == 0 || e.pa == wantPa)) hits.push_back(e);

            size_t pickIdx = 0;
            bool offer = !hits.empty() && g_term.isTty() && !quiet && !knxotaForce;
            if (offer && hits.size() > 1)
            {
                // Several devices were left half-updated with this firmware. Pick before answering.
                g_tpl.section(L.tr("Unfinished runs with this firmware",
                                   "Angefangene Läufe mit dieser Firmware"));
                for (size_t i = 0; i < hits.size(); ++i)
                {
                    char pr[48];
                    std::snprintf(pr, sizeof(pr), "%5.1f %%", hits[i].total ? hits[i].done * 100.0 / hits[i].total : 0.0);
                    g_tpl.kv(std::to_string(i + 1) + "  " + paToStr(hits[i].pa),
                             c.txt(hits[i].ip) + c.dim("   " + std::string(pr) + "   " + agoText(hits[i].when, L)));
                }
                std::printf("  %s %s ", c.amber("?").c_str(),
                            c.bold(L.tr("Which one? [1-9, Enter = none]", "Welchen? [1-9, Enter = keinen]")).c_str());
                std::fflush(stdout);
                char buf[16] = {0};
                if (std::fgets(buf, sizeof(buf), stdin) == nullptr) offer = false;
                else
                {
                    const long n = std::strtol(buf, nullptr, 10);
                    if (n >= 1 && (size_t)n <= hits.size()) pickIdx = (size_t)(n - 1);
                    else offer = false;
                }
            }
            const ftc::OtaSession prev = offer ? hits[pickIdx] : ftc::OtaSession();
            if (offer)
            {
                char pa[16];
                std::snprintf(pa, sizeof(pa), "%u.%u.%u", (prev.pa >> 12) & 0x0F, (prev.pa >> 8) & 0x0F, prev.pa & 0xFF);
                g_tpl.panelTop(L.tr("Unfinished run", "Angefangener Lauf"), pa);
                g_tpl.kv(L.tr("Firmware", "Firmware"), c.txt(prev.file) +
                         (prev.version.empty() ? std::string() : c.dim("  ·  " + prev.version)) +
                         (prev.hardware.empty() ? std::string() : c.dim("  ·  " + prev.hardware)));
                g_tpl.kv(L.tr("Interface", "Interface"), c.txt(prev.ip + ":" + std::to_string((unsigned)prev.port)));
                g_tpl.kv(L.tr("Target", "Ziel"), c.txt(pa));
                if (prev.total)
                {
                    char pr[80];
                    std::snprintf(pr, sizeof(pr), "%.1f %% (%u / %u B)", prev.done * 100.0 / prev.total,
                                  (unsigned)prev.done, (unsigned)prev.total);
                    g_tpl.kv(L.tr("Got to", "Gekommen bis"), c.txt(pr));
                }
                g_tpl.panelEnd();
                g_tpl.note(L.tr("the same firmware -- the transfer continues where it stopped",
                                "dieselbe Firmware -- die Übertragung setzt dort an, wo sie aufhörte"));
                const ftc::Answer3 a = ftc::confirm3(g_term, c, L,
                                                     L.tr("Continue this run?", "Diesen Lauf fortsetzen?"),
                                                     L.tr("no, and stop asking - remove the entry",
                                                          "nein, nicht mehr fragen - Eintrag entfernen"));
                if (a == ftc::Answer3::Yes)
                {
                    if (ip.empty()) ip = prev.ip;
                    port = prev.port;
                    knxotaResumePa = prev.pa;
                    knxotaResume = true;
                    knxotaScan = false; // the target is known; nothing left to search for
                }
                else if (a == ftc::Answer3::Forget)
                {
                    // "No" and "throw it away" are different answers. Only this one removes it, and it
                    // says so -- a silent removal would look like the question simply stopped working.
                    ftc::otaResumeErase(otaResumePath(), prev);
                    g_tpl.status(ftc::Tpl::Stat::Ok,
                                 L.tr("entry removed - it will not be offered again",
                                      "Eintrag entfernt - er wird nicht mehr angeboten"),
                                 {L.tr("`oknx knxota resume` shows what is left",
                                       "`oknx knxota resume` zeigt, was übrig ist")});
                }
            }
        }

        // --- the assistant: find the interface, then the device. Nothing here sends the user away. ---
        if (ip.empty())
        {
            g_tpl.section(L.tr("Interface", "Interface"));
            for (;;)
            {
                std::vector<ftc::DiscoveredIface> found;
                // discoverInterfaces blocks for three seconds; without this the line just sits there and
                // the tool looks hung. runWithBusAnim is what every other blocking read here uses.
                if (g_term.isTty() && !quiet)
                    runWithBusAnim(L.tr("looking for KNXnet/IP interfaces …", "suche KNXnet/IP-Interfaces …"),
                                   [&]() { found = ftc::discoverInterfaces(port, 3000); });
                else
                {
                    std::printf("  %s %s\n", c.cyan(g_term.glyph("⠿", "*")).c_str(),
                                c.dim(L.tr("looking for KNXnet/IP interfaces …", "suche KNXnet/IP-Interfaces …")).c_str());
                    found = ftc::discoverInterfaces(port, 3000);
                }
                // Order comes from discoverInterfaces(): OpenKNX first, then by address. Not repeated
                // here -- one place decides it, so every list in oknx shows the same one.
                if (found.size() == 1)
                {
                    ip = found[0].ip;
                    g_tpl.status(ftc::Tpl::Stat::Ok, found[0].name, {ip, L.tr("the only one that answered", "das einzige, das geantwortet hat")});
                    break;
                }
                if (!found.empty())
                {
                    const std::vector<int> w = {3, 16, 7, 0};
                    g_tpl.tableRow({c.dim("#"), c.dim("IP"), c.dim(L.tr("LINE", "LINIE")), c.dim(L.tr("NAME", "NAME"))}, w);
                    for (size_t i = 0; i < found.size(); ++i)
                    {
                        const bool ok = found[i].name.find("OpenKNX") != std::string::npos ||
                                        found[i].name.find("OpenKnx") != std::string::npos;
                        char ln[12];
                        std::snprintf(ln, sizeof(ln), "%u.%u", (unsigned)((found[i].ia >> 12) & 0x0F),
                                      (unsigned)((found[i].ia >> 8) & 0x0F));
                        std::string nm = c.bright(found[i].name);
                        if (ok) nm += "  " + g_tpl.chip("OpenKNX");
                        g_tpl.tableRow({c.bold(std::to_string(i + 1)), c.txt(found[i].ip), c.txt(ln), nm}, w);
                    }
                    g_tpl.note(L.tr("the interface has to sit on the same LINE as your device: a device 5.0.3 "
                                    "needs an interface on line 5.0",
                                    "das Interface muss auf derselben LINIE sitzen wie dein Gerät: ein Gerät 5.0.3 "
                                    "brauchst du über ein Interface der Linie 5.0"));
                }
                else
                    g_tpl.status(ftc::Tpl::Stat::Warn, L.tr("nothing answered", "nichts geantwortet"),
                                 {L.tr("the interface may be on another network, or a firewall is blocking it",
                                       "das Interface hängt evtl. in einem anderen Netz, oder eine Firewall blockt")});
                char rngIf[16];
                if (found.size() == 1) std::snprintf(rngIf, sizeof(rngIf), "1");
                else std::snprintf(rngIf, sizeof(rngIf), "1-%u", (unsigned)found.size());
                g_tpl.keybar({{rngIf, L.tr("take it", "nehmen")}, {"r", L.tr("search again", "erneut suchen")},
                              {"i", L.tr("type an IP", "IP eingeben")}, {"q", L.tr("quit", "Ende")}});
                std::printf("  %s ", c.amber("?").c_str());
                std::fflush(stdout);
                char in[64] = {0};
                if (!g_term.isTty() || std::fgets(in, sizeof(in), stdin) == nullptr) { socketCleanup(); return 2; }
                const char k = (char)std::tolower((unsigned char)in[0]);
                if (k == 'q') { socketCleanup(); return 130; }
                if (k == 'r') continue;
                if (k == 'i')
                {
                    std::printf("  %s %s ", c.amber("?").c_str(), c.dim(L.tr("IP address", "IP-Adresse")).c_str());
                    std::fflush(stdout);
                    char v[64] = {0};
                    if (std::fgets(v, sizeof(v), stdin) != nullptr)
                    {
                        std::string t(v);
                        while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
                        if (!t.empty()) { ip = t; break; }
                    }
                    continue;
                }
                // The whole number, not its first digit: a network with more than nine interfaces made
                // rows 10+ unreachable and resolved "12" to row 1.
                std::string sel(in);
                while (!sel.empty() && (sel.back() == '\n' || sel.back() == '\r' || sel.back() == ' ')) sel.pop_back();
                bool selNum = !sel.empty();
                for (char ch : sel)
                    if (ch < '0' || ch > '9') selNum = false;
                const long selN = selNum ? std::strtol(sel.c_str(), nullptr, 10) : 0;
                if (selN >= 1 && selN <= (long)found.size()) { ip = found[selN - 1].ip; break; }
            }
        }

        // The device address. Typing it is the shortcut; searching is the offer — and the search itself
        // needs the tunnel, so it happens in the online phase once we are connected.
        if (knxotaVerb == 0) // no address in front of the verb
        {
            g_tpl.section(L.tr("OpenKNX device", "OpenKNX Gerät"));
            g_tpl.keybar({{L.tr("address", "Adresse"), L.tr("e.g. 5.0.3", "z. B. 5.0.3")},
                          {"L", L.tr("search this interface's line", "die Linie dieses Interfaces absuchen")},
                          {"c", L.tr("search another line", "eine andere Linie absuchen")},
                          {"q", L.tr("quit", "Ende")}});
            std::printf("  %s ", c.amber("?").c_str());
            std::fflush(stdout);
            char in[64] = {0};
            if (!g_term.isTty() || std::fgets(in, sizeof(in), stdin) == nullptr) { socketCleanup(); return 2; }
            std::string t(in);
            while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
            if (t == "q" || t == "Q") { socketCleanup(); return 130; }
            unsigned a = 0, l = 0, d = 0;
            if (std::sscanf(t.c_str(), "%u.%u.%u", &a, &l, &d) == 3 && a <= 15 && l <= 15 && d <= 255)
            {
                pos.insert(pos.begin(), t); // the rest of the flow reads the target from pos[0]
                knxotaVerb++;
            }
            else
            {
                knxotaScan = true;                       // resolved after the tunnel is up
                knxotaScanAsk = (t == "c" || t == "C");  // c: let the user name the line first
            }
        }
    }

    // `oknx --file-browser [<start>]` -- open the file chooser on its own. No bus, no device: it is the
    // same screen knxOTA opens, reachable for a look or a quick check without an update in progress.
    if (fileBrowser || (!pos.empty() && pos[0] == "browse"))
    {
        ftc::BrowseSpec spec;
        spec.title = "oknx";
        spec.allowDirPick = true;
        spec.allowFilePick = true;
        spec.roots = ftc::driveRoots(); // so "d" works here too, exactly as it does inside knxOTA
        const size_t at = (!pos.empty() && pos[0] == "browse") ? 1 : 0;
        if (at < pos.size()) spec.start = pos[at];
        std::string picked;
        if (!ftc::browse(g_term, g_theme, g_i18n, spec, picked))
        {
            std::fprintf(stderr, "  %s\n", g_theme.dim(g_i18n.tr("nothing picked", "nichts gewählt")).c_str());
            socketCleanup();
            return 1;
        }
        std::printf("%s\n", picked.c_str());
        socketCleanup();
        return 0;
    }

    // `oknx install|uninstall [--system] [--dir <path>]` — the running binary copies/removes itself onto PATH.
    // Zero external dependency (no pwsh/sh/python) so it works identically on macOS, Linux, Raspberry Pi and
    // Windows. No tunnel; short-circuits here like gzip/decode.
    if (!pos.empty() && (pos[0] == "install" || pos[0] == "uninstall"))
    {
        ftc::I18n& L = g_i18n;
        const std::string gOk = g_theme.green(g_term.glyph("●", "*"));
        const std::string gArr = g_theme.dim(g_term.glyph("→", "->"));
        const std::string gWarn = g_theme.amber(g_term.glyph("!", "!"));
        const std::string gErr = g_theme.red(g_term.glyph("✗", "x"));
        const std::string arr = g_term.glyph("→", "->");

        // Show the version banner first, then a dotted headline for the install/uninstall output below it.
        if (!quiet)
        {
            printVersion();
            g_tpl.section(pos[0] == "uninstall" ? L.tr("uninstall", "Deinstallation") : L.tr("install", "Installation"));
        }

        // Common success footer: a "--help" hint line + a green closing headline (skipped in --quiet).
        auto finishBanner = [&](const std::string& footer) {
            if (quiet) return;
            std::printf("\n  %s\n",
                        g_theme.dim(L.tr("Run `oknx --help` for all commands, or `oknx <pa> info` to query a device.",
                                         "`oknx --help` zeigt alle Befehle, `oknx <pa> info` fragt ein Gerät ab."))
                            .c_str());
            g_tpl.section(footer);
        };

        if (pos[0] == "uninstall")
        {
            const ftc::InstallResult res = ftc::uninstall(installSystem, installDirArg);
            if (res.ok)
            {
                std::printf("  %s %s %s %s\n", gOk.c_str(), g_theme.txt(L.tr("uninstalled", "deinstalliert")).c_str(),
                            gArr.c_str(), g_theme.green(res.dest.string()).c_str());
                finishBanner(L.tr("successfully uninstalled", "erfolgreich deinstalliert"));
            }
            else
                std::fprintf(stderr, "  %s %s\n", gErr.c_str(),
                             g_theme.txt(L.tr("uninstall failed", "Deinstallation fehlgeschlagen") +
                                         (res.note.empty() ? std::string() : ": " + res.note))
                                 .c_str());
            socketCleanup();
            return res.ok ? 0 : 1;
        }

        // install — version-aware plan first (byte-identical skip, upgrade/reinstall report, downgrade prompt)
        ftc::InstallPlan plan = ftc::planInstall(installSystem, installDirArg, FTC_CLI_VERSION);
        if (!plan.error.empty())
        {
            std::fprintf(stderr, "  %s %s\n", gErr.c_str(),
                         g_theme.txt(std::string(L.tr("install failed", "Installation fehlgeschlagen")) + ": " + plan.error).c_str());
            socketCleanup();
            return 1;
        }

        if (plan.destExists && plan.identical) // exact same binary already installed -> no-op
        {
            std::printf("  %s %s %s %s\n", gOk.c_str(),
                        g_theme.txt(std::string(L.tr("already up to date", "bereits aktuell")) + " (" + plan.selfVersion + ")").c_str(),
                        gArr.c_str(), g_theme.green(plan.dest.string()).c_str());
            finishBanner(L.tr("already up to date", "bereits aktuell"));
            socketCleanup();
            return 0;
        }

        if (plan.destExists && plan.oldKnown && plan.cmp < 0 && !installForce) // installed copy is newer
        {
            std::printf("  %s %s\n", gWarn.c_str(),
                        g_theme.amber(std::string(L.tr("the installed version is NEWER", "die installierte Version ist NEUER")) +
                                      " (" + plan.oldVersion + " > " + plan.selfVersion + ")")
                            .c_str());
            std::fflush(stdout); // keep the warning before the prompt/refusal even when piped
            if (!(g_term.isTty() && !quiet))
            {
                std::fprintf(stderr, "  %s %s\n", gErr.c_str(),
                             g_theme.txt(L.tr("refusing to downgrade in a non-interactive run (use --force)",
                                              "Downgrade in nicht-interaktivem Lauf abgelehnt (nutze --force)"))
                                 .c_str());
                socketCleanup();
                return 2;
            }
            std::printf("  %s ", g_theme.cyan(L.tr("really downgrade? [y/N]: ", "wirklich downgraden? [j/N]: ")).c_str());
            std::fflush(stdout);
            char line[16] = {0};
            const bool yes = std::fgets(line, sizeof(line), stdin) &&
                             (line[0] == 'y' || line[0] == 'Y' || line[0] == 'j' || line[0] == 'J');
            if (!yes)
            {
                std::printf("  %s %s\n", gArr.c_str(), g_theme.dim(L.tr("cancelled", "abgebrochen")).c_str());
                socketCleanup();
                return 0;
            }
        }

        const ftc::InstallResult res = ftc::commitInstall(plan);
        if (!res.ok)
        {
            std::fprintf(stderr, "  %s %s\n", gErr.c_str(),
                         g_theme.txt(L.tr("install failed", "Installation fehlgeschlagen") +
                                     (res.note.empty() ? std::string() : ": " + res.note))
                             .c_str());
            socketCleanup();
            return 1;
        }

        std::string msg;
        if (plan.selfIsDest)
            msg = std::string(L.tr("already installed here", "bereits hier installiert")) + " (" + plan.selfVersion + ")";
        else if (!plan.destExists)
            msg = std::string(L.tr("installed", "installiert")) + " " + plan.selfVersion;
        else if (!plan.oldKnown)
            msg = std::string(L.tr("replaced an existing oknx (previous version unknown), installed",
                                   "vorhandene oknx ersetzt (alte Version unbekannt), installiert")) +
                  " " + plan.selfVersion;
        else if (plan.cmp > 0)
            msg = std::string(L.tr("upgraded", "aktualisiert")) + " " + plan.oldVersion + " " + arr + " " + plan.selfVersion;
        else if (plan.cmp < 0)
            msg = std::string(L.tr("downgraded", "heruntergestuft")) + " " + plan.oldVersion + " " + arr + " " + plan.selfVersion;
        else
            msg = std::string(L.tr("reinstalled (same version, different build)",
                                   "neu installiert (gleiche Version, anderer Build)")) +
                  " " + plan.selfVersion;

        std::printf("  %s %s %s %s\n", gOk.c_str(), g_theme.txt(msg).c_str(), gArr.c_str(),
                    g_theme.green(res.dest.string()).c_str());
        if (res.note.find("PATH") != std::string::npos)
        {
            std::printf("  %s %s\n", gWarn.c_str(), g_theme.dim(res.note).c_str());
            std::printf("  %s %s\n", gArr.c_str(),
                        g_theme.cyan(ftc::pathHint(ftc::installDir(installSystem, installDirArg))).c_str());
        }
        finishBanner(L.tr("successfully installed", "erfolgreich installiert"));
        socketCleanup();
        return 0;
    }

    // `oknx decode <hex…>` — offline decode of a raw TP1 LPDU (no tunnel): addresses · TPCI/APCI · OpenKNX FTC /
    // console, coloured by the busmon interpreter. Paste bytes from a capture; mirrors BusmonViewer.html.
    if (pos[0] == "decode")
    {
        std::string hex;
        for (size_t i = 1; i < pos.size(); ++i)
            hex += pos[i]; // accept space-separated bytes or one blob
        if (hex.empty())
        {
            std::fprintf(stderr, "%s\n", g_i18n.tr("usage: oknx decode <hex LPDU>", "Aufruf: oknx decode <hex-LPDU>"));
            socketCleanup();
            return 1;
        }
        ftc::Monitor mon(g_term, g_theme, g_tpl, g_i18n);
        const int rc = mon.decodeOffline(hex, quiet, verbose);
        socketCleanup();
        return rc;
    }

    // `oknx config` — show the persisted config; `oknx config <key> <value>` sets + writes it.
    if (pos[0] == "config")
    {
        ftc::Theme& c = g_theme;
        ftc::I18n& L = g_i18n;
        if (pos.size() >= 3)
        {
            g_cfg.set(pos[1], pos[2]);
            if (!g_cfg.save())
                std::fprintf(stderr, "%s\n", L.tr("could not write config", "Konfig nicht schreibbar"));
            else
                std::printf("  %s %s = %s\n", c.green(g_term.glyph("●", "*")).c_str(), c.cyan(pos[1]).c_str(), c.txt(pos[2]).c_str());
        }
        std::printf("  %s %s\n", c.dim(L.tr("file", "Datei")).c_str(), c.dim(g_cfg.path()).c_str());
        for (const auto& kv : g_cfg.all())
        {
            std::string k = kv.first;
            while (k.size() < 14)
                k += ' ';
            std::printf("  %s %s\n", c.cyan(k).c_str(), c.txt(kv.second).c_str());
        }
        if (g_cfg.all().empty())
            std::printf("  %s\n", c.dim(L.tr("(empty — keys: theme · lang · ascii · mode; e.g. oknx config theme amber)",
                                             "(leer — Schlüssel: theme · lang · ascii · mode; z.B. oknx config theme amber)"))
                                      .c_str());
        socketCleanup();
        return 0;
    }

    // Listing, re-pointing or dropping a stored group-address table touches no bus and names no
    // interface - demanding --ip for it only gets in the way.
    if (ip.empty() && pos.size() >= 2 && pos[0] == "ga" && pos[1] == "table")
    {
        ftc::I18n& L = g_i18n;
        std::vector<std::string> names = ftc::GaTable::list();
        if (pos.size() >= 4 && (pos[2] == "rm" || pos[2] == "delete"))
        {
            const std::string f = ftc::GaTable::path(pos[3]);
            const bool gone = std::remove(f.c_str()) == 0;
            g_tpl.status(gone ? ftc::Tpl::Stat::Ok : ftc::Tpl::Stat::Err,
                         gone ? L.tr("table removed", "Tabelle entfernt") : L.tr("no such table", "keine solche Tabelle"),
                         {f});
            socketCleanup();
            return gone ? 0 : 1;
        }
        // Only listing and removing work without an interface. Anything else used to fall through to the
        // listing and exit 0, so `ga table move A B` reported success without moving anything.
        if (pos.size() >= 3)
        {
            g_ui.errorBlock(false, L.tr("this needs an interface", "dafür wird ein Interface gebraucht"),
                            {L.tr("without --ip only `ga table` and `ga table rm <name>` work",
                                  "ohne --ip gehen nur `ga table` und `ga table rm <name>`"),
                             "oknx -i <ip> ga table " + pos[2]});
            socketCleanup();
            return 1;
        }
        if (names.empty())
        {
            g_tpl.status(ftc::Tpl::Stat::Warn, L.tr("no group address table imported yet",
                                                    "noch keine Gruppenadresstabelle importiert"),
                         {ftc::GaTable::dir(), "oknx -i <ip> ga import GA-Export.xml"});
            socketCleanup();
            return 1;
        }
        for (const std::string& n : names)
        {
            ftc::GaTable t;
            t.load(n);
            size_t withDpt = 0;
            t.each([&](uint16_t, const ftc::GaInfo& gi) { if (gi.main != 0) withDpt++; });
            // A fixed format string, so the compiler still checks it, and the quiet stream stays a stream.
            if (g_quiet) std::printf("gatable\t%s\t%zu\t%zu\t0\n", n.c_str(), t.size(), withDpt);
            else
                std::printf("    %-18s %4zu %s, %4zu %s\n", n.c_str(), t.size(),
                            L.tr("addresses", "Adressen"), withDpt, L.tr("with a type", "mit Typ"));
        }
        socketCleanup();
        return 0;
    }

    if (ip.empty())
    {
        std::fprintf(stderr, "error: --ip is required (or use --discover). See --help.\n");
        socketCleanup();
        return 1;
    }

    // `oknx --ip <ip> info` (no PA) — the full interface report. No tunnel, no bus: DESCRIPTION + device-mgmt.
    if (pos.size() == 1 && pos[0] == "info")
    {
        const int rc = renderInterfaceInfo(ip, port, quiet);
        socketCleanup();
        return rc;
    }

    // `oknx -i <ip> prop dump [maxIot] [maxPid]` (no PA) — sweep every readable property of the interface
    // over ONE device-management session and print a stable, diffable line per value. Objects are probed
    // with PID 1 first (present on every interface object), so absent object types cost one request.
    if (pos.size() >= 2 && pos[0] == "prop" && pos[1] == "dump")
    {
        const uint16_t maxIot = pos.size() >= 3 ? (uint16_t)std::strtoul(pos[2].c_str(), nullptr, 0) : 20;
        const uint8_t maxPid = pos.size() >= 4 ? (uint8_t)std::strtoul(pos[3].c_str(), nullptr, 0) : 120;

        ftc::DevMgmtSession dm;
        std::string err;
        if (!dm.open(ip, port, &err))
        {
            std::fprintf(stderr, "prop dump: connect failed: %s\n", err.c_str());
            socketCleanup();
            return 1;
        }

        uint8_t out[256];
        size_t outLen = 0;
        int objects = 0, values = 0;
        for (uint16_t iot = 0; iot <= maxIot; ++iot)
        {
            if (dm.readEx(iot, 1, 1, 1, 1, out, sizeof(out), outLen) != ftc::DmRead::Data)
                continue; // no PID 1 -> object type not present
            ++objects;
            for (uint8_t pid = 1; pid <= maxPid; ++pid)
            {
                if (dm.readEx(iot, 1, pid, 1, 1, out, sizeof(out), outLen) != ftc::DmRead::Data)
                    continue;
                std::printf("IOT=%-3u PID=%-4u len=%-3zu ", iot, pid, outLen);
                for (size_t i = 0; i < outLen; ++i) std::printf("%02X", out[i]);
                std::printf("\n");
                ++values;
            }
        }
        std::printf("# objects=%d values=%d\n", objects, values);
        dm.close();
        socketCleanup();
        return 0;
    }

    // `oknx -i <ip> busprop dump <pa> [maxObj] [maxPid]` — sweep a TP device's readable properties over ONE
    // tunnel session and print a diffable line per value. Object 0 (device) always exists; a missing object
    // answers nothing on PID 1, so it costs one request.
    if (pos.size() >= 3 && pos[0] == "busprop" && pos[1] == "dump")
    {
        // paFromText range-checks; the hand-rolled scan did not, so 20.1.1 shifted past the 16 bits
        // and silently addressed 4.0.1.
        const uint16_t pa = paFromText(pos[2]);
        if (pa == 0)
        {
            std::fprintf(stderr, "busprop dump: <pa> must look like 5.0.11 (area 0-15, line 0-15, device 0-255)\n");
            socketCleanup();
            return 2;
        }
        const uint8_t maxObj = pos.size() >= 4 ? (uint8_t)std::strtoul(pos[3].c_str(), nullptr, 0) : 12;
        const uint8_t maxPid = pos.size() >= 5 ? (uint8_t)std::strtoul(pos[4].c_str(), nullptr, 0) : 90;

        KnxIpTunnel tun;
        if (!tun.connect(ip, port))
        {
            std::fprintf(stderr, "busprop dump: tunnel connect failed (status %d)\n", tun.lastConnectStatus());
            socketCleanup();
            return 1;
        }
        tun.setPropertyCallback(&busPropAnswer);
        g_bpPa = pa;

        int objects = 0, values = 0;
        for (uint8_t obj = 0; obj <= maxObj; ++obj)
        {
            bool objSeen = false;
            for (uint8_t pid = 1; pid <= maxPid; ++pid)
            {
                g_bpObj = obj; g_bpPid = pid; g_bpSeen = false; g_bpLen = 0;
                if (!tun.sendPropertyValueRead(pa, obj, pid, 1, 1)) continue;
                const uint64_t until = ftc::detail::nowMs() + 600;
                while (!g_bpSeen && ftc::detail::nowMs() < until)
                    tun.pump();
                // A_PropertyValue_Response with zero elements is the bus answer for "no such property";
                // it is not a value, and on PID 1 it means the object index itself is not populated.
                if (!g_bpSeen || g_bpLen == 0) { if (pid == 1) break; else continue; }
                if (!objSeen) { objSeen = true; ++objects; }
                std::printf("OBJ=%-3u PID=%-4u len=%-3u ", obj, pid, (unsigned)g_bpLen);
                for (uint8_t i = 0; i < g_bpLen; ++i) std::printf("%02X", g_bpData[i]);
                std::printf("\n");
                ++values;
            }
        }
        std::printf("# objects=%d values=%d\n", objects, values);
        tun.setPropertyCallback(nullptr);
        tun.disconnect();
        socketCleanup();
        return 0;
    }

    // `oknx -i <ip> <pa> runstate [start|stop]` — the Run State Machine of the application program object
    // (PID 6). 03_05_01 clause 4.24: reading gives the state (Table 95 p.299), writing
    // takes an event (Table 96 p.299: 0 NOP, 1 Restart, 2 Stop). Stop holds GROUP communication only -
    // management stays reachable - and the state lives in RAM, so a device reset undoes it.
    // A decoded front end for `busprop <pa> <objIdx> 6`; the index is looked up, never assumed.
    if (pos.size() >= 2 && pos.size() <= 3 && pos[1] == "runstate")
    {
        // paFromText rejects an out-of-range part; a hand-rolled scan would silently truncate
        // "20.1.1" to 4.0.1 and address a different device.
        const uint16_t pa = paFromText(pos[0]);
        if (pa == 0)
        {
            std::fprintf(stderr, "usage: <pa> runstate [start|stop]\n");
            socketCleanup();
            return 2;
        }
        const std::string paText = paToStr(pa);

        int event = -1; // -1 = read only
        if (pos.size() == 3)
        {
            if (pos[2] == "start" || pos[2] == "restart") event = 1;
            else if (pos[2] == "stop") event = 2;
            else
            {
                std::fprintf(stderr, "runstate: expected start or stop, got '%s'\n", pos[2].c_str());
                socketCleanup();
                return 2;
            }
        }

        KnxIpTunnel tun;
        if (!tun.connect(ip, port))
        {
            std::fprintf(stderr, "runstate: tunnel connect failed (status %d)\n", tun.lastConnectStatus());
            socketCleanup();
            return 1;
        }
        tun.setPropertyCallback(&busPropAnswer);

        // The application program object is OT 3, but its INDEX is device specific. Measured
        // 2026-09-27 on 5.0.3 and 5.0.8: index 3 is the GROUP OBJECT TABLE (OT 9) and the application
        // program sits at 4. A hard-coded 3 asks the wrong object and reports "not implemented" for a
        // property that is there, so the index is looked up through PID 1 (PID_OBJECT_TYPE).
        auto readProp = [&](uint8_t obj, uint8_t pid) -> int {
            g_bpPa = pa; g_bpObj = obj; g_bpPid = pid; g_bpSeen = false; g_bpLen = 0;
            if (!tun.sendPropertyValueRead(pa, obj, pid, 1, 1)) return -2;
            const uint64_t until = ftc::detail::nowMs() + 600;
            while (!g_bpSeen && ftc::detail::nowMs() < until) tun.pump();
            if (!g_bpSeen) return -2;
            if (g_bpLen == 0) return -3;
            return 0;
        };
        int appIdx = -1;
        for (uint8_t i = 0; i <= 12; ++i)
        {
            // Same window and stop rule as `busprop dump`: object indices are contiguous, so no
            // answer or an empty one on PID 1 ends the sweep instead of waiting out every index.
            if (readProp(i, 1) != 0) break;
            if (g_bpLen >= 2 && g_bpData[0] == 0x00 && g_bpData[1] == 0x03) { appIdx = (int)i; break; }
        }
        if (appIdx < 0)
        {
            std::printf("  runstate %s  -> no application program object (type 3) on this device\n",
                        paText.c_str());
            tun.setPropertyCallback(nullptr);
            tun.disconnect();
            socketCleanup();
            return 4;
        }
        std::printf("  application program object at index %d\n", appIdx);

        auto readState = [&]() -> int {
            g_bpPa = pa; g_bpObj = (uint8_t)appIdx; g_bpPid = 6; g_bpSeen = false; g_bpLen = 0;
            if (!tun.sendPropertyValueRead(pa, (uint8_t)appIdx, 6, 1, 1)) return -2;
            const uint64_t until = ftc::detail::nowMs() + 3000;
            while (!g_bpSeen && ftc::detail::nowMs() < until) tun.pump();
            if (!g_bpSeen) return -2;
            if (g_bpLen == 0) return -3;   // answered, but the property carries nothing: not implemented
            return (int)g_bpData[0];
        };
        auto name = [](int st) -> const char* {
            switch (st) { case 0: return "halted"; case 1: return "running"; case 2: return "ready";
                          case 3: return "terminated"; case 4: return "starting"; case 5: return "shutting down";
                          default: return "?"; }
        };

        int rc = 0;
        const int before = readState();
        if (before == -2) { std::printf("  runstate %s  -> no answer\n", paText.c_str()); rc = 4; }
        else if (before == -3)
        {
            // Exactly what a device without the property answers: a response with zero elements.
            std::printf("  runstate %s  -> PID_RUN_STATE_CONTROL is not implemented on this device\n",
                        paText.c_str());
            rc = 4;
        }
        else
        {
            std::printf("  runstate %s  -> %d (%s)\n", paText.c_str(), before, name(before));
            if (event >= 0)
            {
                const uint8_t ev = (uint8_t)event;
                if (!tun.sendPropertyValueWrite(pa, (uint8_t)appIdx, 6, 1, 1, &ev, 1)) { std::printf("  -> SEND FAILED\n"); rc = 4; }
                else
                {
                    const int after = readState();
                    if (after < 0) { std::printf("  -> %s sent, but the state could not be read back\n", pos[2].c_str()); rc = 4; }
                    else std::printf("  %-8s -> %d (%s)\n", pos[2].c_str(), after, name(after));
                }
            }
        }

        tun.setPropertyCallback(nullptr);
        tun.disconnect();
        socketCleanup();
        return rc;
    }

    // `oknx -i <ip> busprop read|write <pa> <objIdx> <pid> …` — A_PropertyValue_Read/Write over the BUS via a
    // self-owned tunnel, so it reaches any TP device, not just the interface. Note the addressing difference
    // from `prop`: the bus service takes an object INDEX (0 = device object), the management channel takes an
    // object TYPE. Diagnostic tool; prints exactly what came back.
    if (pos.size() >= 5 && pos[0] == "busprop" && (pos[1] == "read" || pos[1] == "write"))
    {
        const bool doWrite = pos[1] == "write";
        // Same range check as every other command; see busprop dump above.
        const uint16_t pa = paFromText(pos[2]);
        if (pa == 0)
        {
            std::fprintf(stderr, "busprop: <pa> must look like 5.0.7 (area 0-15, line 0-15, device 0-255)\n");
            socketCleanup();
            return 2;
        }
        const uint8_t objIdx = (uint8_t)std::strtoul(pos[3].c_str(), nullptr, 0);
        const uint8_t pid = (uint8_t)std::strtoul(pos[4].c_str(), nullptr, 0);

        uint16_t startIdx = doWrite ? 0 : 1;
        uint8_t noe = 1;
        std::vector<uint8_t> payload;
        if (doWrite)
        {
            if (pos.size() < 7)
            {
                std::fprintf(stderr, "usage: busprop write <pa> <objIdx> <pid> <startIdx> <hex...>\n");
                socketCleanup();
                return 2;
            }
            startIdx = (uint16_t)std::strtoul(pos[5].c_str(), nullptr, 0);
            std::string hex;
            for (size_t i = 6; i < pos.size(); ++i) hex += pos[i];
            for (size_t i = 0; i + 1 < hex.size(); i += 2)
                payload.push_back((uint8_t)std::strtoul(hex.substr(i, 2).c_str(), nullptr, 16));
        }
        else
        {
            if (pos.size() >= 6) startIdx = (uint16_t)std::strtoul(pos[5].c_str(), nullptr, 0);
            if (pos.size() >= 7) noe = (uint8_t)std::strtoul(pos[6].c_str(), nullptr, 0);
        }

        KnxIpTunnel tun;
        if (!tun.connect(ip, port))
        {
            std::fprintf(stderr, "busprop: tunnel connect failed (status %d)\n", tun.lastConnectStatus());
            socketCleanup();
            return 1;
        }
        g_bpPa = pa; g_bpObj = objIdx; g_bpPid = pid; g_bpSeen = false; g_bpLen = 0;
        tun.setPropertyCallback(&busPropAnswer);

        const bool sent = doWrite
            ? tun.sendPropertyValueWrite(pa, objIdx, pid, noe, startIdx, payload.data(), (uint8_t)payload.size())
            : tun.sendPropertyValueRead(pa, objIdx, pid, noe, startIdx);
        std::printf("busprop %s pa=%s objIdx=%u PID=%u startIdx=%u noe=%u",
                    doWrite ? "write" : "read ", paToStr(pa).c_str(), objIdx, pid, startIdx, noe);
        if (doWrite) { std::printf(" data="); for (uint8_t b : payload) std::printf("%02X", b); }
        std::printf("\n");

        int rc = 0;
        if (!sent) { std::printf("  -> SEND FAILED\n"); rc = 4; }
        else
        {
            const uint64_t until = ftc::detail::nowMs() + 3000;
            while (!g_bpSeen && ftc::detail::nowMs() < until)
                tun.pump();
            if (g_bpSeen)
            {
                std::printf("  -> %u byte: ", (unsigned)g_bpLen);
                for (uint8_t i = 0; i < g_bpLen; ++i) std::printf("%02X", g_bpData[i]);
                std::printf("\n");
            }
            else { std::printf("  -> NO ANSWER\n"); rc = 4; }
        }
        tun.setPropertyCallback(nullptr);
        tun.disconnect();
        socketCleanup();
        return rc;
    }

    // `oknx -i <ip> prop read|write <iot> <inst> <pid> …` (no PA) — raw Local Device Management on the
    // interface itself (cEMI M_PropRead / M_PropWrite, 03_06_03 §4.1.7.3). Diagnostic access by number:
    // it prints what the server actually answered, including the cEMI error code behind a refusal.
    //   read   <iot> <inst> <pid> [startIdx=1] [noe=1]
    //   write  <iot> <inst> <pid> <startIdx> <hex…>      startIdx 0 addresses the array element count
    if (pos.size() >= 5 && pos[0] == "prop" && (pos[1] == "read" || pos[1] == "write"))
    {
        const bool doWrite = pos[1] == "write";
        const uint16_t iot = (uint16_t)std::strtoul(pos[2].c_str(), nullptr, 0);
        const uint8_t inst = (uint8_t)std::strtoul(pos[3].c_str(), nullptr, 0);
        const uint8_t pid = (uint8_t)std::strtoul(pos[4].c_str(), nullptr, 0);

        uint16_t startIdx = doWrite ? 0 : 1;
        uint8_t noe = 1;
        std::vector<uint8_t> payload;
        if (doWrite)
        {
            if (pos.size() < 7)
            {
                std::fprintf(stderr, "usage: prop write <iot> <inst> <pid> <startIdx> <hex...>\n");
                socketCleanup();
                return 2;
            }
            startIdx = (uint16_t)std::strtoul(pos[5].c_str(), nullptr, 0);
            std::string hex;
            for (size_t i = 6; i < pos.size(); ++i) hex += pos[i];
            for (size_t i = 0; i + 1 < hex.size(); i += 2)
                payload.push_back((uint8_t)std::strtoul(hex.substr(i, 2).c_str(), nullptr, 16));
        }
        else
        {
            if (pos.size() >= 6) startIdx = (uint16_t)std::strtoul(pos[5].c_str(), nullptr, 0);
            if (pos.size() >= 7) noe = (uint8_t)std::strtoul(pos[6].c_str(), nullptr, 0);
        }

        ftc::DevMgmtSession dm;
        std::string err;
        if (!dm.open(ip, port, &err))
        {
            std::fprintf(stderr, "prop: connect failed: %s\n", err.c_str());
            socketCleanup();
            return 1;
        }

        uint8_t propErr = 0;
        int rc = 0;
        if (doWrite)
        {
            const ftc::DmRead r = dm.writeEx(iot, inst, pid, startIdx, 1, payload.data(), payload.size(), &propErr);
            std::printf("prop write IOT=%u inst=%u PID=%u startIdx=%u noe=1 data=", iot, inst, pid, startIdx);
            for (uint8_t b : payload) std::printf("%02X", b);
            std::printf("\n");
            if (r == ftc::DmRead::Data)
                std::printf("  -> POSITIVE con (accepted)\n");
            else if (r == ftc::DmRead::Negative)
            {
                std::printf("  -> NEGATIVE con  error=0x%02X (%s)\n", propErr, ftc::dmPropErrorName(propErr));
                rc = 3;
            }
            else { std::printf("  -> NO ANSWER\n"); rc = 4; }
        }
        else
        {
            uint8_t out[256];
            size_t outLen = 0;
            const ftc::DmRead r = dm.readEx(iot, inst, pid, (uint8_t)startIdx, noe, out, sizeof(out), outLen, &propErr);
            std::printf("prop read  IOT=%u inst=%u PID=%u startIdx=%u noe=%u\n", iot, inst, pid, startIdx, noe);
            if (r == ftc::DmRead::Data)
            {
                std::printf("  -> %zu byte: ", outLen);
                for (size_t i = 0; i < outLen; ++i) std::printf("%02X", out[i]);
                std::printf("\n");
            }
            else if (r == ftc::DmRead::Negative)
            {
                std::printf("  -> NEGATIVE con  error=0x%02X (%s)\n", propErr, ftc::dmPropErrorName(propErr));
                rc = 3;
            }
            else { std::printf("  -> NO ANSWER\n"); rc = 4; }
        }
        dm.close();
        socketCleanup();
        return rc;
    }

    // `oknx --ip <ip> groupmon|gm | busmon|bm` (no PA) — live monitors over a self-owned tunnel (own KNX layer).
    // -q gives plain tab-separated lines; -V adds raw hex; --frames/--seconds cap it for scripted runs; Ctrl+C stops.
    const bool isBus = pos[0] == "busmon" || pos[0] == "bm";
    // `ga import <file>` and `ga table ...` touch no bus at all: they fill, list or re-point the group
    // address table. It is keyed by the interface, because that is the separation that already exists -
    // the test rig and the productive line are reached through different interfaces.
    if (!pos.empty() && pos[0] == "ga" && pos.size() >= 2 && (pos[1] == "import" || pos[1] == "table"))
    {
        ftc::I18n& L = g_i18n;
        if (pos[1] == "import")
        {
            if (pos.size() < 3)
            {
                g_ui.errorBlock(false, L.tr("ga import needs an ETS group address export",
                                            "ga import braucht einen ETS-Gruppenadressexport"),
                                {L.tr("XML or CSV; CSV only WITH header lines",
                                      "XML oder CSV; CSV nur MIT Kopfzeilen")},
                                "oknx -i <ip> ga import GA-Export.xml");
                socketCleanup();
                return 2;
            }
            ftc::GaTable t;
            const ftc::GaImportResult res = ftc::GaImport::run(pos[2], t);
            if (!res.ok)
            {
                g_ui.errorBlock(false, L.tr("could not read that export", "dieser Export ist nicht lesbar"),
                                {res.error}, "oknx -i <ip> ga import GA-Export.xml");
                socketCleanup();
                return 2;
            }
            std::string werr;
            if (!t.save(ip, werr))
            {
                // Say WHY: on a Pi this used to fail silently because the directory could not be created.
                g_ui.errorBlock(false, L.tr("could not write the table", "Tabelle nicht schreibbar"),
                                {werr, ftc::GaTable::path(ip)}, "");
                socketCleanup();
                return 1;
            }
            char det[160];
            std::snprintf(det, sizeof(det),
                          L.tr("%zu addresses, %zu with a datapoint type", "%zu Adressen, %zu mit Datenpunkttyp"),
                          res.addresses, res.withDpt);
            if (quiet) std::printf("gaimport\t%zu\t%zu\t%s\n", res.addresses, res.withDpt, ftc::GaTable::path(ip).c_str());
            else if (res.withDpt == 0)
                g_tpl.status(ftc::Tpl::Stat::Warn, res.format,
                             {det, L.tr("this export carries no types - nothing will be decoded",
                                        "dieser Export fuehrt keine Typen - es wird nichts dekodiert"),
                              ftc::GaTable::path(ip)});
            else
                g_tpl.status(ftc::Tpl::Stat::Ok, res.format, {det, ftc::GaTable::path(ip)});
            socketCleanup();
            return res.withDpt == 0 ? 1 : 0;
        }
        // `ga table` - list, re-point after the interface changed its address, or drop one
        if (pos.size() >= 4 && (pos[2] == "move" || pos[2] == "mv"))
        {
            const std::string from = ftc::GaTable::path(pos[3]);
            const std::string to = ftc::GaTable::path(pos.size() >= 5 ? pos[4] : ip);
            if (std::rename(from.c_str(), to.c_str()) != 0)
            {
                g_ui.errorBlock(false, L.tr("no table for that interface", "keine Tabelle fuer diese Schnittstelle"),
                                {from}, "oknx ga table");
                socketCleanup();
                return 1;
            }
            g_tpl.status(ftc::Tpl::Stat::Ok, L.tr("table moved", "Tabelle umgehaengt"), {from + "  ->  " + to});
            socketCleanup();
            return 0;
        }
        if (pos.size() >= 4 && (pos[2] == "rm" || pos[2] == "delete"))
        {
            const std::string f = ftc::GaTable::path(pos[3]);
            const bool gone = std::remove(f.c_str()) == 0;
            g_tpl.status(gone ? ftc::Tpl::Stat::Ok : ftc::Tpl::Stat::Err,
                         gone ? L.tr("table removed", "Tabelle entfernt") : L.tr("no such table", "keine solche Tabelle"),
                         {f});
            socketCleanup();
            return gone ? 0 : 1;
        }
        // plain `ga table`: what is stored, for which interface
        const std::string dir = ftc::GaTable::dir();
        std::vector<std::string> names = ftc::GaTable::list();
        if (names.empty())
        {
            g_tpl.status(ftc::Tpl::Stat::Warn, L.tr("no group address table imported yet",
                                                    "noch keine Gruppenadresstabelle importiert"),
                         {dir, "oknx -i <ip> ga import GA-Export.xml"});
            socketCleanup();
            return 1;
        }
        for (const std::string& n : names)
        {
            ftc::GaTable t;
            t.load(n);
            size_t withDpt = 0;
            t.each([&](uint16_t, const ftc::GaInfo& gi) { if (gi.main != 0) withDpt++; });
            // Same output as the offline listing: one command must not speak two protocols depending
            // on whether --ip happens to be set.
            if (g_quiet)
            {
                std::printf("gatable\t%s\t%zu\t%zu\t%d\n", n.c_str(), t.size(), withDpt, n == ftc::GaTable::key(ip) ? 1 : 0);
                continue;
            }
            std::printf("    %-18s %4zu %s, %4zu %s%s\n", n.c_str(), t.size(), L.tr("addresses", "Adressen"),
                        withDpt, L.tr("with a type", "mit Typ"),
                        (n == ftc::GaTable::key(ip)) ? L.tr("   <- in use now", "   <- jetzt aktiv") : "");
        }
        socketCleanup();
        return 0;
    }

    // `ga monitor <x/y/z>` is the group monitor narrowed to one address - the same viewer, the same
    // keys, the same export, not a second implementation. Rewritten here and handled by the monitor path.
    if (pos.size() >= 3 && pos[0] == "ga" && (pos[1] == "monitor" || pos[1] == "mon"))
    {
        std::vector<std::string> rw{"gm"};
        for (size_t i = 2; i < pos.size(); i++) rw.push_back(pos[i]);
        pos = rw;
    }

    const bool isMon = isBus || pos[0] == "groupmon" || pos[0] == "gm";

    // `oknx -i <ipA> gm|bm compare <ipB> [--grace ms] [--multi]` — live A/B fidelity diff of two monitors on the
    // same TP bus (busmon or groupmon). Diff ON = compare (divergence optics + result); diff OFF (--multi / key
    // 'd') = a plain two-stream viewer for two interfaces on different lines. Keys: v layout · d diff · l save · …
    if (isMon && pos.size() == 3 && pos[1] == "compare")
    {
        ftc::I18n& L = g_i18n;
        const std::string ipB = pos[2];
        if (ipB.empty() || ipB == ip)
        {
            std::fprintf(stderr, "%s\n", L.tr("compare: need a SECOND interface IP different from -i",
                                              "compare: braucht eine ZWEITE Interface-IP, verschieden von -i"));
            socketCleanup();
            return 2;
        }
        ftc::Compare cmp(g_term, g_theme, g_tpl, g_i18n);
        const int rc = cmp.run(isBus ? ftc::Monitor::Mode::Bus : ftc::Monitor::Mode::Group, ip, port, ipB, port,
                               quiet, verbose, monFrames, monSeconds, cmpGrace, !cmpMulti, !cmpRaw,
                               cmpOnlyDiff, cmpCollapse, cmpMarkers, cmpSkew, &g_abort);
        socketCleanup();
        return rc;
    }

    // One or several group addresses, comma or space separated: `gm 0/7/0,0/7/1` and `gm 0/7/0 0/7/1`
    // both watch two addresses. Every token has to parse, otherwise this is not a filter argument at all
    // and the usual "too many arguments" path takes over.
    std::vector<uint16_t> monGas;
    static ftc::GaTable monInline; // types given on the command line
    bool monGaBad = false;
    if (isMon && pos.size() >= 2)
    {
        for (size_t i = 1; i < pos.size() && !monGaBad; i++)
        {
            std::string tok;
            std::istringstream ts(pos[i]);
            while (std::getline(ts, tok, ','))
            {
                if (tok.empty()) continue;
                // "0/7/0:9.004" - the type given here wins over the imported table for that address
                std::string dptHere;
                const size_t colon = tok.find(':');
                if (colon != std::string::npos)
                {
                    dptHere = tok.substr(colon + 1);
                    tok = tok.substr(0, colon);
                }
                unsigned a1 = 0, b1 = 0, c1 = 0;
                char tail = 0;
                if (std::sscanf(tok.c_str(), "%u/%u/%u%c", &a1, &b1, &c1, &tail) != 3 || a1 > 31 ||
                    b1 > 7 || c1 > 255)
                {
                    monGaBad = true;
                    break;
                }
                const uint16_t packed = (uint16_t)((a1 << 11) | (b1 << 8) | c1);
                monGas.push_back(packed);
                if (!dptHere.empty())
                {
                    ftc::GaInfo gi;
                    if (!ftc::GaTable::parseDpt(dptHere, gi.main, gi.sub)) { monGaBad = true; break; }
                    monInline.set(packed, gi);
                }
            }
        }
    }
    const bool monOneGa = isMon && !monGaBad && !monGas.empty();
    if ((pos.size() == 1 || monOneGa) && isMon)
    {
        ftc::Monitor mon(g_term, g_theme, g_tpl, g_i18n);
        mon.target(ip, port);
        if (monOneGa) mon.setGroupFilter(monGas);
        // The imported table of THIS interface, plus anything given inline. Missing table = raw octets.
        static ftc::GaTable monTab;
        monTab.load(ip);
        mon.setGaLookup(
            [](uint16_t ga, const uint8_t* d, uint8_t n, uint8_t small) -> std::string {
                const ftc::GaInfo* gi = monInline.find(ga);
                if (gi == nullptr || gi->main == 0) gi = monTab.find(ga);
                if (gi == nullptr || gi->main == 0) return "";
                return ftc::GaTable::decode(gi->main, gi->sub, d, n, small);
            },
            [](uint16_t ga) -> std::string {
                const ftc::GaInfo* gi = monTab.find(ga);
                return gi != nullptr ? gi->name : std::string();
            });
        const int rc = mon.run(isBus ? ftc::Monitor::Mode::Bus : ftc::Monitor::Mode::Group,
                               quiet, verbose, monFrames, monSeconds, &g_abort);
        socketCleanup();
        return rc;
    }

    // `oknx --ip <ip> progscan|ps [global]` (no PA) — broadcast A_IndividualAddress_Read over a self-owned tunnel
    // and list the devices in programming mode. `ps` = LOCAL (hop 0, this TP line only) · `ps global` = hop 6
    // (whole network). -q gives plain `pa<TAB>line`; -V logs the TX cEMI bytes; --seconds/--frames cap it; Ctrl+C stops.
    if ((pos[0] == "progscan" || pos[0] == "ps") &&
        (pos.size() == 1 || (pos.size() == 2 && pos[1] == "global")))
    {
        const bool global = pos.size() == 2 && pos[1] == "global";
        ftc::ProgScan ps(g_term, g_theme, g_tpl, g_i18n);
        ps.target(ip, port);
        const int psSecs = secondsSet ? monSeconds : 3; // default 3 s one-shot; explicit --seconds 0 = continuous loop
        const int rc = ps.run(global ? ftc::ProgScan::Mode::Global : ftc::ProgScan::Mode::Local,
                              quiet, verbose, monFrames, psSecs, &g_abort);
        socketCleanup();
        return rc;
    }

    // `oknx -i <ip> con|console` (no PA) — the INTERFACE'S OWN console via the OFM-Network webconsole (WebSocket
    // ws://<ip>:80/console), NOT the KNX tunnel: no APDU/drain, fast local streaming, no truncation. If the
    // device has no webconsole -> one clean error and exit. `oknx -i <ip> <pa> con` (with a PA) still tunnels.
    if (pos.size() == 1 && (pos[0] == "con" || pos[0] == "console"))
    {
        ftc::I18n& L = g_i18n;
        if (ip.empty())
        {
            std::fprintf(stderr, "%s\n", L.tr("console: --ip required", "console: --ip nötig"));
            socketCleanup();
            return 2;
        }
        ftc::WebConsole ws;
        std::string werr;
        if (!ws.connect(ip, 80, "/console", werr))
        {
            g_ui.errorBlock(false, L.tr("no webconsole", "keine Web-Konsole") + std::string(" \xC2\xB7 ") + ip, {werr},
                            "ws://" + ip + "/console " + L.tr("did not answer — this device has no OFM-Network webconsole", "antwortet nicht — dieses Gerät hat keine OFM-Network-Webconsole"));
            socketCleanup();
            return 1;
        }
        // Interface steckbrief for the verbose l4 (KNXnet/IP DESCRIPTION, independent of the WS).
        std::string wIfName, wIfMask;
        {
            ftc::IfaceDesc d;
            ftc::queryInterface(ip, port, d);
            if (d.ok)
            {
                wIfName = d.name;
                char mb[16];
                std::snprintf(mb, sizeof(mb), "0x%04X", d.mask);
                wIfMask = mb;
            }
        }
        if (logRequested) openSessionLog("ws_" + ip, logPathArg);

        auto styleWs = [&](const std::string& l) { const std::string s = ftcSeverityStyle(l); return s.empty() ? l : s; };

        if (!g_term.isTty() || quiet) // plain path (piped/-q): stream lines, stdin -> ws.send
        {
            StdinLines stdinLines;
            bool running = true;
            while (running && ws.connected())
            {
                if (g_abort) break;
                std::vector<std::string> lines;
                if (!ws.poll(lines))
                {
                    // Plain mode is the scripted one -- the reason belongs on stderr so a pipe still sees it.
                    if (ws.lost()) std::fprintf(stderr, "connection lost -- the device stopped answering\n");
                    break;
                }
                for (const auto& l : lines)
                {
                    std::printf("%s\n", l.c_str());
                }
                std::fflush(stdout);
                std::string line;
                if (stdinLines.poll(line))
                {
                    if (line == "quit" || line == "exit") running = false;
                    else
                        ws.send(line);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            ws.close();
            socketCleanup();
            return 0;
        }

        ftc::ConsoleUi ui(g_term, g_theme, g_i18n);
        ui.setVerbose(verbose);
        ui.setWsMode(ip);
        ui.setIface(std::string(), wIfName, wIfMask);
        ui.begin(ip, std::string(), ip, g_logPath, consoleHistoryPath());
        g_consoleUi = &ui;
        g_watch.load(watchPath("ws_" + ip));
        ui.setJobs(g_watch.activeCount());
        const uint64_t conStart = nowMs();
        uint64_t lastBar = 0;
        uint64_t lastWsSend = 0; // paces WS sends: the webconsole has a SINGLE pending-command slot, so
                                 // back-to-back sends overwrite each other -> lost commands. ~150 ms gap is safe.
        bool running = true;
        while (running && ws.connected())
        {
            if (g_abort)
            {
                running = false;
                break;
            }
            std::vector<std::string> lines;
            if (!ws.poll(lines))
            {
                // Say WHY it ended. A session that dies because the device vanished looks exactly like one the
                // user closed, and the difference is the whole message: the device is gone, nothing you typed
                // after that arrived anywhere.
                if (ws.lost())
                    ui.emit(g_theme.red(g_i18n.tr("connection lost — the device stopped answering",
                                                  "Verbindung verloren — das Gerät antwortet nicht mehr")));
                break;
            }
            for (const auto& l : lines)
            {
                const std::string shown = styleWs(l);
                ui.emit(shown);
                if (g_logFp)
                {
                    const std::string clean = stripAnsi(shown);
                    std::fputs(clean.c_str(), g_logFp);
                    std::fputc('\n', g_logFp);
                }
            }
            std::string line;
            ftc::ConsoleUi::PollResult pr = ui.poll(line);
            if (pr == ftc::ConsoleUi::PollAbort || pr == ftc::ConsoleUi::PollEof) running = false;
            else if (pr == ftc::ConsoleUi::PollHelp)
                ui.showHelp();
            else if (pr == ftc::ConsoleUi::PollToggleLog)
            {
                if (g_logFp)
                {
                    std::fclose(g_logFp);
                    g_logFp = nullptr;
                    const std::string p = g_logPath;
                    g_logPath.clear();
                    ui.setLogPath("");
                    emitLine(g_theme.amber(g_term.glyph("● ", "* ")) + g_theme.dim(g_i18n.tr("log stopped — saved: ", "Log gestoppt — gespeichert: ")) + g_theme.txt(p));
                }
                else if (!openSessionLog("ws_" + ip, std::string()).empty())
                {
                    ui.setLogPath(g_logPath);
                    emitLine(g_theme.green(g_term.glyph("● ", "* ")) + g_theme.dim(g_i18n.tr("log started: ", "Log gestartet: ")) + g_theme.txt(g_logPath));
                }
            }
            else if (pr == ftc::ConsoleUi::PollSubmit)
            {
                if (g_watch.isMeta(line))
                {
                    emitLine(g_theme.green("> ") + g_theme.txt(line));
                    if (line == "/help" || line == "/?" || line == "/h") ui.showHelp();
                    else if (line == "/stat" || line.rfind("/stat", 0) == 0)
                        for (const auto& sl : ui.statLines(ws.bytesIn(), ws.bytesOut(), 0, 0, (nowMs() - conStart) / 1000, true))
                            emitLine(sl);
                    else
                    {
                        for (const auto& fl : g_watch.handle(line))
                            emitLine(fl);
                        ui.setJobs(g_watch.activeCount());
                    }
                }
                else if (line == "quit" || line == "exit")
                    running = false;
                else
                {
                    ui.noteCommand(line);
                    ws.send(line);
                    lastWsSend = nowMs();
                } // the webconsole echoes the command back itself
            }
            const uint64_t now = nowMs();
            if (now - lastBar > 250)
            {
                lastBar = now;
                ui.tick(ws.bytesIn(), ws.bytesOut(), 0, 0, (now - conStart) / 1000);
            }
            // Auto-commands (/job): WS has NO lockstep, and the webconsole has a SINGLE pending-command slot, so
            // firing due jobs back-to-back overwrites them (lost `m`/`net`). Pace at >=150 ms between WS sends:
            // the device consumes the slot in between, and 5 one-second jobs still all fire within the second.
            if (now - lastWsSend >= 150)
            {
                const std::string autoCmd = g_watch.due(now, true);
                if (!autoCmd.empty() && !lineHasAuthKeyword(autoCmd))
                {
                    emitLine(g_theme.green(g_term.glyph("⟳ ", "~ ")) + g_theme.dim(autoCmd));
                    ui.noteCommand(autoCmd);
                    ws.send(autoCmd);
                    lastWsSend = now;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        g_consoleUi = nullptr;
        const uint32_t frx = ws.bytesIn(), ftx = ws.bytesOut();
        const uint64_t fup = (nowMs() - conStart) / 1000;
        if (verbose && g_logFp)
        {
            std::fputc('\n', g_logFp);
            for (const auto& sl : ui.statLines(frx, ftx, 0, 0, fup, false))
            {
                std::fputs(sl.c_str(), g_logFp);
                std::fputc('\n', g_logFp);
            }
        }
        if (g_logFp)
        {
            std::fclose(g_logFp);
            g_logFp = nullptr;
        }
        ui.end(frx, ftx, 0, 0, fup);
        if (verbose)
            for (const auto& sl : ui.statLines(frx, ftx, 0, 0, fup, true))
                std::printf("%s\n", sl.c_str());
        ws.close();
        socketCleanup();
        return 0;
    }

    // --quiet is the dominant verbosity: it forces the full steckbrief off too, so a script gets nothing but
    // the command's own output (and errors on stderr). --verbose + --quiet together -> quiet wins.
    if (quiet)
    {
        verbose = false;
        g_term.setColorEnabled(false); // -q is the scriptable voice: no chrome, no color
        g_color = false;
    }

    // `scan ... fast` and the parallel range scan are gone: measured 2026-10-02/03, N tunnels bought at
    // most ~2x while the result set moved between runs (8 of 68 addresses reproducible). What does pay is
    // the identity read of the FOUND devices, which is what --workers drives.
    if (!pos.empty() && pos[0] == "scan" && std::find(pos.begin(), pos.end(), std::string("fast")) != pos.end())
    {
        ftc::I18n& L2 = g_i18n;
        std::fprintf(stderr, "%s\n", L2.tr("error: `scan ... fast` no longer exists -- the parallel range scan returned a different "
                                           "subset on every run. Use `details` (parallel identity read) or `ets` (serial, reproducible).",
                                           "Fehler: `scan ... fast` gibt es nicht mehr -- der parallele Bereichsscan lieferte bei jedem "
                                           "Lauf eine andere Teilmenge. Nimm `details` (parallele Identitaet) oder `ets` (seriell, reproduzierbar)."));
        socketCleanup();
        return 1;
    }


    g_ip = ip; // the detail children reach the same interface
    g_port = port;

    // --- open the tunnel --------------------------------------------------------------------------
    if (!g_knxTunnel.connect(ip, port))
    {
        const int cst = g_knxTunnel.lastConnectStatus();
        // Probe the (never-secured) DESCRIPTION endpoint: it separates a reachable device from an
        // unreachable one, and its Supported Service Families reveal whether the device advertises
        // Tunnelling (04h -- allowed in DESCRIPTION_RESPONSE, 03_08_02 Table 3). The secure-specific DIBs
        // (Security family 09h / Tunnelling Info DIB Authorised bit) are NOT carried in DESCRIPTION_RESPONSE
        // (03_08_02 Tables 3/4), so "advertises tunnelling yet refused this unsecured request" -- not a DIB
        // flag -- is our KNX IP Secure signal.
        ftc::IfaceDesc d;
        const bool reachable = ftc::queryInterface(ip, port, d);
        const bool advertisesTunnelling = reachable && d.famVer[0x04] != 0;
        const char* q = d.name[0] ? "'" : "";
        const char* nm = d.name[0] ? d.name : "";
        const char* sp = d.name[0] ? "' " : "";

        // A secure-only interface refuses an unsecured tunnel either by CONNECT_RESPONSE E_CONNECTION_TYPE/
        // E_CONNECTION_OPTION while still advertising Tunnelling, or by ignoring the request entirely while
        // still answering DESCRIPTION. Both mean: reachable, does tunnelling, but not unsecured -> Secure.
        const bool secureRefusal =
            (advertisesTunnelling && (cst == 0x22 || cst == 0x23)) || (cst < 0 && advertisesTunnelling);

        ftc::I18n& L = g_i18n;
        char line[256];
        if (secureRefusal)
        {
            char why[64];
            if (cst > 0)
                std::snprintf(why, sizeof(why), " (CONNECT_RESPONSE 0x%02X)", (unsigned)cst);
            else
                std::snprintf(why, sizeof(why), " %s", L.tr("(no CONNECT_RESPONSE)", "(keine CONNECT_RESPONSE)"));
            std::snprintf(line, sizeof(line),
                          L.tr("%s%s%sat %s:%u supports tunnelling but refused the unsecured request%s.",
                               "%s%s%sunter %s:%u kann tunneln, lehnte die ungesicherte Anfrage aber ab%s."),
                          q, nm, sp, ip.c_str(), (unsigned)port, why);
            g_ui.errorBlock(false, L.tr("KNX IP Secure required", "KNX IP Secure erforderlich"),
                            {line, L.tr("oknx does not support KNX IP Secure.", "oknx unterstützt kein KNX IP Secure.")},
                            L.tr("use a non-secure interface, or disable Secure Tunnelling in ETS.",
                                 "nutze ein nicht-sicheres Interface, oder deaktiviere Secure Tunnelling in ETS."));
        }
        else if (cst > 0)
        {
            std::snprintf(line, sizeof(line),
                          L.tr("%s:%u refused the tunnel — CONNECT_RESPONSE 0x%02X: %s",
                               "%s:%u lehnte den Tunnel ab — CONNECT_RESPONSE 0x%02X: %s"),
                          ip.c_str(), (unsigned)port, (unsigned)cst, connectStatusStr(cst));
            g_ui.errorBlock(false, L.tr("tunnel refused", "Tunnel abgelehnt"), {line},
                            L.tr("free a tunnel slot, pick another interface, or check the address.",
                                 "gib einen Tunnel-Slot frei, nimm ein anderes Interface, oder prüfe die Adresse."));
        }
        else if (reachable)
        {
            std::snprintf(line, sizeof(line),
                          L.tr("%s%s%sat %s:%u answered DESCRIPTION but did not open a tunnel.",
                               "%s%s%sunter %s:%u antwortete auf DESCRIPTION, öffnete aber keinen Tunnel."),
                          q, nm, sp, ip.c_str(), (unsigned)port);
            g_ui.errorBlock(false, L.tr("not a tunnelling interface", "kein Tunnelling-Interface"),
                            {line, L.tr("it may not be a KNXnet/IP tunnelling interface.",
                                        "es ist evtl. kein KNXnet/IP-Tunnelling-Interface.")},
                            "oknx --discover");
        }
        else
        {
            std::snprintf(line, sizeof(line),
                          L.tr("no response from %s:%u (neither CONNECT nor DESCRIPTION).",
                               "keine Antwort von %s:%u (weder CONNECT noch DESCRIPTION)."),
                          ip.c_str(), (unsigned)port);
            g_ui.errorBlock(false, L.tr("interface unreachable", "Interface nicht erreichbar"),
                            {line, L.tr("unreachable, or the IP/port is wrong.", "nicht erreichbar, oder IP/Port falsch.")},
                            "oknx --discover");
        }
        socketCleanup();
        return 1;
    }

    // --- FTC send priority: gate an elevated one, then stamp it onto every FTC data frame. This path uses
    // g_knxTunnel (send/get/console/perf/info/ll/scan/fwupdate); gm/bm/compare/progscan use their own tunnels.
    if (prio2 != 3 && !confirmElevatedPriority(prioName, prioForce, g_term.isTty() && !quiet))
    {
        g_knxTunnel.disconnect();
        socketCleanup();
        return 3;
    }
    g_knxTunnel.setTxPriority(prio2);
    const std::string prioTag = prioHeaderTag(prioName);

    if (!quiet)
    {
        ftc::Theme& c = g_theme;
        ftc::I18n& L = g_i18n;
        char pa[16];
        std::snprintf(pa, sizeof(pa), "%u.%u.%u", (g_knxTunnel.assignedPA() >> 12) & 0x0F,
                      (g_knxTunnel.assignedPA() >> 8) & 0x0F, g_knxTunnel.assignedPA() & 0xFF);
        std::printf("  %s %s  %s  %s%s\n", c.green(g_term.glyph("●", "*")).c_str(),
                    c.green(L.tr("tunnel up", "Tunnel steht")).c_str(),
                    c.dim(ip + ":" + std::to_string((unsigned)port)).c_str(),
                    (c.dim(L.tr("as ", "als ")) + g_tpl.chip(pa, 'c')).c_str(),
                    prioTag.empty() ? "" : ("   " + prioTag).c_str());
    }

    // Interface steckbrief (DESCRIPTION_REQUEST on a side socket; independent of the tunnel). Default = a
    // one-line identity so you always see WHICH interface you are on; --verbose = the full property panel.
    uint16_t ifaceApdu = 0;     // detected interface max APDU -> drives auto-framing (console cap, upload pkg start)
    std::string ifName, ifMask; // interface steckbrief carried out to the verbose console l4 line
    {
        ftc::IfaceDesc idesc;
        ftc::queryInterface(ip, port, idesc);
        ifName = idesc.name;
        {
            char mb[16];
            std::snprintf(mb, sizeof(mb), "0x%04X", idesc.mask);
            ifMask = mb;
        }
        // APDU auto-detection (primary): read the interface's max APDU over Device Management -- zero KNX-bus
        // traffic, spec-exact, works on v1 interfaces where DESCRIPTION carries no Extended Device Info DIB.
        idesc.apduReported = queryMaxApduDeviceMgmt(ip, port, &idesc.apduReportedPid, idesc.apduReason, sizeof(idesc.apduReason));
        ifaceApdu = idesc.apduReported ? idesc.apduReported : idesc.maxLocalApdu;
        // --quiet keeps the query above (it drives the APDU auto-framing) but prints no identity at all.
        if (quiet)
        { /* no output */
        }
        else if (verbose)
            printIfacePanel(ip, idesc);
        else
            printIfaceLine(idesc);
    }

    // Register the built-in (default LittleFS-shim -> host filesystem) backend once. The shim maps the
    // default backend onto the host FS, so `send <hostpath>` / `get <remote> <hostpath>` work as-is —
    // no explicit FtcFileSource/FtcFileSink needed (kept minimal, per the task).
    openknxFileTransferClient.setup(true);

    // Feed the detected interface max APDU to the client -> auto-framing for every sized operation (upload
    // pkg start, download chunk) begins at the right frame instead of degrading down from the max.
    g_ifaceApdu = ifaceApdu;
    openknxFileTransferClient.setApduHint(ifaceApdu);

    // --verbose: read + print the full TARGET steckbrief before the actual command, reusing the device-info
    // discovery chain (mask/class · manufacturer · order · hw · version · prog mode · app program · table
    // load states · File-Transfer features). pos[0] is the target PA; all small property reads over the
    // tunnel -> fits any interface. Runs to quiescence, then the real command proceeds normally.
    // `scan` has NO target PA (pos[0] == "scan"), so an "ftc <pos[0]> info" here would run "ftc scan info" —
    // an accidental own-line scan that shadows the real command. Skip the target steckbrief for it.
    const bool isConsoleCmd = pos.size() >= 2 && (pos[1] == "console" || pos[1] == "con");
    if (verbose && !pos.empty() && pos[0] != "scan" && !isConsoleCmd && !knxotaActive) // con-open shows NO device info (clean session); one-shot keeps the panel
    {
        std::printf("\n  %s\n", g_theme.green(std::string(g_term.glyph("── ", "-- ")) + g_i18n.tr("KNX Device · ", "KNX-Gerät · ") + pos[0] + g_i18n.tr(" via ", " über ") + ip + " ──────────────────────────").c_str());
        int idrc = 0;
        ftcRenderStructured({pos[0], "info"}, quiet, idrc); // NEW structured device panel (same as `oknx <pa> info`), not the raw text
    }

    // --- hidden diagnostic: is the link-layer confirmation usable as a presence test? --------------
    // Sends one frame per address and reports only the L_Data.con. On TP1 every present device
    // acknowledges at data-link level, so this should find device classes an application read misses.
    if (!pos.empty() && pos[0] == "_conprobe")
    {
        // Optional inter-probe pacing "_conprobe pace <ms> <pa>...": wait <ms> (still pumping) after each con.
        // Host-side test of the every-3rd-con-loss = fast-fire pileup; no "pace" token -> fire immediately.
        unsigned paceMs = 0;
        size_t first = 1;
        if (pos.size() >= 3 && pos[1] == "pace")
        {
            std::sscanf(pos[2].c_str(), "%u", &paceMs);
            first = 3;
        }
        static volatile bool s_seen = false;
        static bool s_ok = false;
        static uint16_t s_pa = 0;
        g_knxTunnel.setConfirmCallback([](uint16_t pa, bool ok) {
            if (pa != s_pa) return;
            s_ok = ok;
            s_seen = true;
        });
        std::printf("  %-10s %-9s %s\n", "ADDRESS", "CONFIRM", "ms");
        for (size_t i = first; i < pos.size(); ++i)
        {
            unsigned a = 0, l = 0, d = 0;
            if (std::sscanf(pos[i].c_str(), "%u.%u.%u", &a, &l, &d) != 3) continue;
            s_pa = (uint16_t)((a << 12) | (l << 8) | d);
            s_seen = false;
            const uint64_t t0 = nowMs();
            g_knxTunnel.sendDeviceDescriptorRead(s_pa);
            while (!s_seen && nowMs() - t0 < 3000)
            {
                g_knxTunnel.pump();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            std::printf("  %-10s %-9s %llu\n", pos[i].c_str(),
                        s_seen ? (s_ok ? "yes" : "no") : "(none)",
                        (unsigned long long)(nowMs() - t0));
            // Pace before the next probe: keep pumping so late cons / device retries are ACKed and the bus
            // settles. paceMs == 0 -> the loop body never runs (immediate next probe, unchanged behaviour).
            for (uint64_t tp = nowMs(); paceMs && nowMs() - tp < paceMs;)
            {
                g_knxTunnel.pump();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        g_knxTunnel.setConfirmCallback(nullptr);
        socketCleanup();
        return 0;
    }

    // --- knxOTA online phase: read the target, compare, decide -------------------------------------
    // The tunnel is up and the client is set up. Everything here is read-only until the confirmation.
    if (knxotaActive)
    {
        ftc::I18n& L = g_i18n;
        ftc::Theme& c = g_theme;
        std::string paText = (knxotaVerb > 0) ? pos[0] : std::string();
        if (knxotaResume && paText.empty())
        {
            char b[16];
            std::snprintf(b, sizeof(b), "%u.%u.%u", (knxotaResumePa >> 12) & 0x0F, (knxotaResumePa >> 8) & 0x0F,
                          knxotaResumePa & 0xFF);
            paText = b;
        }

        // The assistant offered to search: do it now that the tunnel is up. The scan runs on the line the
        // interface itself sits on and asks only OpenKNX devices, so the list stays short and relevant.
        while (paText.empty() && knxotaScan)
        {
            // The interface's own line is the one it can reach without a coupler, so it is the default —
            // but it is only a default: a line behind a coupler is a legitimate target and the user knows
            // their topology better than we do.
            const uint16_t own = g_knxTunnel.assignedPA();
            char line[16];
            if (!knxotaLine.empty())
                std::snprintf(line, sizeof(line), "%s", knxotaLine.c_str());
            else
                std::snprintf(line, sizeof(line), "%u.%u", (unsigned)((own >> 12) & 0x0F), (unsigned)((own >> 8) & 0x0F));
            if (knxotaScanAsk)
            {
                const std::string take = std::string(L.tr("search ", "durchsuche ")) + line;
                g_tpl.section(L.tr("Which line", "Welche Linie"));
                g_tpl.seg(L.tr("line", "Linie"), {line}, 0);
                g_tpl.keybar({{"↵", take.c_str()},
                              {L.tr("area.line", "Bereich.Linie"), L.tr("another one, e.g. 5.1",
                                                                       "eine andere, z. B. 5.1")},
                              {"q", L.tr("quit", "Ende")}});
                std::printf("  %s ", c.amber("?").c_str());
                std::fflush(stdout);
                char lin[32] = {0};
                if (!g_term.isTty() || std::fgets(lin, sizeof(lin), stdin) == nullptr) { socketCleanup(); return 2; }
                std::string t3(lin);
                while (!t3.empty() && (t3.back() == '\n' || t3.back() == '\r' || t3.back() == ' ')) t3.pop_back();
                if (t3 == "q" || t3 == "Q") { socketCleanup(); return 130; }
                unsigned la = 0, ll = 0;
                if (!t3.empty())
                {
                    if (std::sscanf(t3.c_str(), "%u.%u", &la, &ll) == 2 && la <= 15 && ll <= 15)
                        std::snprintf(line, sizeof(line), "%u.%u", la, ll);
                    else
                        g_tpl.note(std::string(L.tr("not a line — searching ", "keine Linie — durchsuche ")) + line);
                }
            }
            knxotaLine = line; // searching again stays on this line instead of reverting to the default
            g_tpl.section(std::string(L.tr("Searching line ", "Suche auf Linie ")) + line);
            // No duration promise: it depends on the line, the bus load and how many devices answer.
            // The counter below reports the truth as it happens, which is worth more than an estimate.
            g_tpl.status(ftc::Tpl::Stat::Info, L.tr("asking every address on this line",
                                                    "frage jede Adresse auf dieser Linie"),
                         {L.tr("the counter below shows how far it is", "der Zähler unten zeigt den Fortschritt"),
                          L.tr("ctrl-C stops", "Strg-C bricht ab")});
            unsigned sa = 0, sl = 0;
            std::sscanf(line, "%u.%u", &sa, &sl);
            const uint16_t lineBase = (uint16_t)((sa << 12) | (sl << 8));
            std::vector<std::pair<std::string, bool>> pick;
            size_t nOther = 0; // answered, but nothing knxOTA can update -- hidden unless `alle` is typed
            std::vector<FtcEntry> hits;
            const bool ackFeed = armScanAckFeed(lineBase);
            if (!ackFeed)
                g_tpl.note(L.tr("this interface does not report acknowledgements — the search will miss "
                                "devices that never answer",
                                "dieses Interface meldet keine Quittungen — die Suche übersieht Geräte, "
                                "die nie antworten"));
            // Only an OpenKNX device can be updated from here, so only those are asked who they are —
            // and they are asked while the sweep still runs, over their own tunnels.
            std::unique_ptr<ftc::DetailPool> dpool;
            if (!g_ip.empty())
                dpool.reset(new ftc::DetailPool(g_selfPath, g_ip, g_port,
                                                g_workers > 0 ? g_workers : FTC_DETAIL_WORKERS));
            std::function<void(const FtcEntry&)> onNew = [&](const FtcEntry& e) {
                // Same predicate as the scan, or the picker and the table disagree about who is a candidate.
                if (dpool && KnxDeviceMap::family((uint16_t)e.crc) == KnxDeviceMap::Family::SystemB)
                    dpool->submit(e.name);
            };
            g_ftcSuppress = true;
            // Without the pool the sweep would run its own identity probe over the one shared tunnel; with
            // it that is the same question asked twice, and the two starve each other.
            openknxFileTransferClient.processCommand(std::string("ftc scan ") + line + (dpool ? "" : " openknx"), false);
            ftcPumpStructured(hits, true, g_term.isTty() && !quiet, false, dpool ? &onNew : nullptr);
            g_ftcSuppress = false;
            if (ackFeed) g_knxTunnel.setConfirmCallback(nullptr);

            std::unordered_map<std::string, ftc::DetailRow> det;
            if (dpool)
            {
                const uint64_t t0 = nowMs();
                uint64_t drawn = 0;
                while (dpool->outstanding() > 0 && !g_abort)
                {
                    const uint64_t now = nowMs();
                    if (g_term.isTty() && !quiet && now - drawn > 60)
                    {
                        drawn = now;
                        const size_t done = dpool->answered(), left = dpool->outstanding();
                        const unsigned el = (unsigned)((now - t0) / 1000);
                        char cnt[32], clk[16];
                        std::snprintf(cnt, sizeof(cnt), "%u/%u", (unsigned)done, (unsigned)(done + left));
                        std::snprintf(clk, sizeof(clk), "%u:%02u", el / 60, el % 60);
                        liveLine(std::string("  ") + c.green(liveSpinner(now)) + " " +
                                 c.cyan(L.tr("reading OpenKNX devices", "lese OpenKNX-Geräte")) + "   " +
                                 c.bold(cnt) + "   " + c.dim(clk));
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                if (g_abort) dpool->abort();
                dpool->wait();
                det = dpool->rows();
                if (g_term.isTty() && !quiet) std::fprintf(stderr, "\r\x1b[K");
            }
            for (const auto& e : hits)
            {
                unsigned a = 0, l = 0, d = 0;
                if (std::sscanf(e.name, "%u.%u.%u", &a, &l, &d) != 3) continue;
                const auto it = det.find(e.name);
                const bool isOk = e.isOpenKnx || (it != det.end() && it->second.mfr == ftc::MFR_OPENKNX);
                pick.emplace_back(e.name, isOk);
                if (!isOk) ++nOther;
            }
            // Same rule for the devices: OpenKNX first, then by address -- and by address means
            // numerically, or 5.0.11 would sort between 5.0.1 and 5.0.2.
            std::sort(pick.begin(), pick.end(), [](const auto& x, const auto& y) {
                if (x.second != y.second) return x.second;
                unsigned xa = 0, xl = 0, xd = 0, ya = 0, yl = 0, yd = 0;
                std::sscanf(x.first.c_str(), "%u.%u.%u", &xa, &xl, &xd);
                std::sscanf(y.first.c_str(), "%u.%u.%u", &ya, &yl, &yd);
                return ((xa << 12) | (xl << 8) | xd) < ((ya << 12) | (yl << 8) | yd);
            });

            // Only what can be updated: the numbers a person picks from must all lead somewhere. On the
            // test line that is 5 of 28 -- the other 23 are foreign devices and tunnel slots.
            if (!showAllPick)
                pick.erase(std::remove_if(pick.begin(), pick.end(), [](const std::pair<std::string, bool>& p) { return !p.second; }),
                           pick.end());
            if (!pick.empty())
            {
                const std::vector<int> w = {3, 10, 0};
                g_tpl.tableRow({c.dim("#"), c.dim(L.tr("ADDRESS", "ADRESSE")), c.dim(L.tr("DEVICE", "GERÄT"))}, w);
                for (size_t i = 0; i < pick.size(); ++i)
                {
                    std::string what;
                    if (pick[i].second)
                    {
                        what = g_tpl.chip("OpenKNX");
                        const auto it = det.find(pick[i].first);
                        // What the device says about itself, right where the user looks for it. A device
                        // that answered nothing keeps the chip alone rather than an invented description.
                        const std::string txt = it != det.end() ? ftc::describe(it->second) : std::string();
                        if (!txt.empty()) what += "  " + c.txt(txt);
                    }
                    else
                        what = c.dim(L.tr("other", "anderes"));
                    g_tpl.tableRow({c.bold(std::to_string(i + 1)), c.txt(pick[i].first), what}, w);
                }
            }
            else
                g_tpl.status(ftc::Tpl::Stat::Warn,
                             showAllPick ? L.tr("nothing answered on this line", "auf dieser Linie hat nichts geantwortet")
                                         : L.tr("no OpenKNX device on this line", "kein OpenKNX-Gerät auf dieser Linie"),
                             {std::string(L.tr("line ", "Linie ")) + line});
            if (!showAllPick && nOther)
            {
                char note[160];
                std::snprintf(note, sizeof(note), "%u %s", (unsigned)nOther,
                              L.tr("further addresses answered; knxOTA cannot update them (foreign devices, tunnel slots) -- `alle` shows them",
                                   "weitere Adressen haben geantwortet; knxOTA kann sie nicht aktualisieren (fremde Geräte, Tunnel-Plätze) -- `alle` zeigt sie"));
                g_tpl.note(note);
            }
            char rngPick[16];
            if (pick.size() == 1) std::snprintf(rngPick, sizeof(rngPick), "1");
            else std::snprintf(rngPick, sizeof(rngPick), "1-%u", (unsigned)pick.size());
            g_tpl.keybar({{rngPick, L.tr("take it", "nehmen")},
                          {L.tr("address", "Adresse"), L.tr("type one instead", "stattdessen eingeben")},
                          {"alle", L.tr("show non-OpenKNX too", "auch Nicht-OpenKNX zeigen")},
                          {"L", L.tr("search again", "erneut suchen")},
                          {"c", L.tr("another line", "andere Linie")},
                          {"q", L.tr("quit", "Ende")}});
            std::printf("  %s ", c.amber("?").c_str());
            std::fflush(stdout);
            char in[32] = {0};
            if (!g_term.isTty() || std::fgets(in, sizeof(in), stdin) == nullptr) { socketCleanup(); return 2; }
            std::string t2(in);
            while (!t2.empty() && (t2.back() == '\n' || t2.back() == '\r' || t2.back() == ' ')) t2.pop_back();
            unsigned qa = 0, ql = 0, qd = 0;
            if (t2 == "q" || t2 == "Q") { socketCleanup(); return 130; }
            if (t2 == "alle" || t2 == "all") { showAllPick = true; knxotaScanAsk = false; continue; }
            if (std::sscanf(t2.c_str(), "%u.%u.%u", &qa, &ql, &qd) == 3) paText = t2; // a typed address always wins
            else if (t2 == "L" || t2 == "l")
            {
                knxotaScanAsk = false; // same line, no question — a device may have been powered up meanwhile
                continue;
            }
            else if (t2 == "c" || t2 == "C")
            {
                knxotaScanAsk = true; // ask for the line, then search again
                continue;
            }
            else
            {
                // The whole number, not its first digit: after `alle` the list is longer than nine rows,
                // and reading one character silently resolved "12" to row 1 -- a different device.
                bool allDigits = !t2.empty();
                for (char ch : t2)
                    if (ch < '0' || ch > '9') allDigits = false;
                const long n = allDigits ? std::strtol(t2.c_str(), nullptr, 10) : 0;
                if (n >= 1 && n <= (long)pick.size()) paText = pick[n - 1].first;
                else { socketCleanup(); return 130; }
            }
        }

        if (paText.empty())
        {
            g_ui.errorBlock(false, L.tr("no device address given", "keine Geräteadresse angegeben"),
                            {L.tr("knxOTA needs to know which device to update",
                                  "knxOTA muss wissen, welches Gerät aktualisiert werden soll")},
                            "oknx --ip " + ip + " <address> fwupdate <file>");
            socketCleanup();
            return 2;
        }

        unsigned pa_a = 0, pa_l = 0, pa_d = 0;
        std::sscanf(paText.c_str(), "%u.%u.%u", &pa_a, &pa_l, &pa_d);
        const uint16_t targetPaEarly = (uint16_t)((pa_a << 12) | (pa_l << 8) | pa_d);

        // One device-info read: mask, identity (PID 78), version (PID 25), FTC features. Finish on quiescence
        // (not the first field), else the feature probe is still in flight and firmware-capable reads as not.
        g_ftcSuppress = true;
        std::vector<FtcEntry> knxotaSnap;
        openknxFileTransferClient.processCommand("ftc " + paText + " info", false);
        ftcPumpStructured(knxotaSnap, false, false);
        g_ftcSuppress = false;
        const FtcDeviceInfo& di = openknxFileTransferClient.deviceInfo();
        const ftc::DevVersion dv = ftc::devVersionFrom(di.hardware, di.haveHw, di.version, di.haveVersion);
        const ftc::Verdict verdict = ftc::compareVersions(knxotaFw.id, dv);

        g_tpl.section(L.tr("OpenKNX device", "OpenKNX Gerät") + std::string(" · ") + paText);
        if (!di.valid)
        {
            unsigned ta = 0, tl = 0, td = 0;
            std::sscanf(paText.c_str(), "%u.%u.%u", &ta, &tl, &td);
            reportUnreachable(paText, (uint16_t)((ta << 12) | (tl << 8) | td), nullptr);
            socketCleanup();
            return 6;
        }
        g_tpl.panelTop(L.tr("Version comparison", "Versionsvergleich"), paText);
        // The application id is shown on BOTH lines, always: it is what the verdict is decided on, and a
        // verdict whose grounds are off screen reads as an opinion. "0.7.0 vs 0.8.0 -> different device"
        // is baffling until the two numbers behind it are visible.
        g_tpl.kv(L.tr("Device", "Gerät"), c.txt(di.haveOrder ? di.order : "OpenKNX") +
                 c.dim(std::string("   ") + ftc::devVersionText(dv)) +
                 (dv.valid ? c.dim(std::string("   ") + L.tr("application ", "Anwendung ") +
                                   ftc::appIdText(dv.openKnxId, dv.appNumber))
                           : std::string()));
        g_tpl.kv(L.tr("File", "Datei"), c.txt(knxotaFw.hardware) +
                 c.dim(std::string("   ") + ftc::fwVersionText(knxotaFw.id)) +
                 (knxotaFw.id.valid ? c.dim(std::string("   ") + L.tr("application ", "Anwendung ") +
                                            ftc::appIdText(knxotaFw.id.openKnxId, knxotaFw.id.appNumber))
                                    : std::string()));
        g_tpl.kv(L.tr("Result", "Ergebnis"), ftc::verdictChip(g_tpl, L, verdict));
        // The chip names the outcome; these lines say what it costs. Without them "NEW APPLICATION" is a
        // label the reader has no way to weigh, and weighing it is exactly what they are here to do.
        for (const auto& e : ftc::verdictExplain(L, verdict))
            g_tpl.kv("", c.dim(e));
        g_tpl.panelEnd();

        // The Update bit is the device's own statement that it can install a firmware. Absent, we do not
        // guess why: a silent file-transfer server and an old one look identical from here, and saying
        // "too old" when the device simply did not answer would send the user after the wrong problem.
        // On a legacy server the feature byte does not exist, so "absent" means it cannot SAY, not that it
        // cannot act: FwUpdate has been there since 0.0.4 and its frame never moved. ftmCan carries what
        // the version implies, in the same bit numbering, so this one test covers both kinds of target.
        const bool canSelfApply = (di.ftmCan & 0x02) != 0;
        if (canSelfApply && di.ftmDerived)
            // Derived from the version, not reported: no feature byte arrived from this legacy server (a
            // 0.0.4 build has none to give, a 0.1.x answer was lost). Those versions carry FwUpdate on their
            // RP2040/2350 builds only, so the apply is sent unconfirmed and the restart is watched.
            g_tpl.status(ftc::Tpl::Stat::Warn,
                         L.tr("FTM before 0.2.0, no feature byte arrived: self-apply exists on its RP2040/2350 builds only",
                              "FTM vor 0.2.0, kein Merkmalsbyte erhalten: Selbst-Einspielen gibt es nur in seinen RP2040/2350-Builds"),
                         {L.tr("the apply is sent unconfirmed and the restart is watched",
                               "das Einspielen wird unbestätigt gesendet und der Neustart beobachtet")});
        else if (!canSelfApply)
            g_tpl.status(ftc::Tpl::Stat::Warn,
                         L.tr("this device did not offer to install a firmware itself",
                              "dieses Gerät hat nicht angeboten, eine Firmware selbst einzuspielen"),
                         {di.ftmVersion == 0
                              ? L.tr("its file-transfer server did not answer",
                                     "sein Dateitransfer-Server hat nicht geantwortet")
                              : L.tr("its file-transfer server reports no self-apply (not an RP2040/2350 build, or the feature is off)",
                                     "sein Dateitransfer-Server meldet kein Selbst-Einspielen (kein RP2040/2350-Build, oder abgeschaltet)"),
                          L.tr("the firmware would be transferred but not installed",
                               "die Firmware würde übertragen, aber nicht eingespielt")});

        // --- access protection: ask the device directly, not through the info chain ---------------
        // deviceInfo() zeroes the feature byte when the FT server stayed silent, and a stage-"Off" device
        // answers only CheckFeatures -- so the single probe is the reliable read (and works on a locked device).
        const ftc::AccessState acc = resolveAccessInteractively(targetPaEarly, 180, quiet, knxotaCheck);

        switch (acc.stage)
        {
            case ftc::Access::Open:
                g_tpl.status(ftc::Tpl::Stat::Ok, L.tr("access", "Zugriff"),
                             {L.tr("this device accepts the update", "dieses Gerät nimmt das Update an")});
                break;
            case ftc::Access::NeedPassword:
                g_tpl.status(ftc::Tpl::Stat::Warn, L.tr("access", "Zugriff"),
                             {L.tr("this device is password protected", "dieses Gerät ist passwortgeschützt")});
                break;
            case ftc::Access::Blocked:
                g_tpl.status(ftc::Tpl::Stat::Warn, L.tr("access", "Zugriff"),
                             {L.tr("this device is not accepting writes right now",
                                   "dieses Gerät nimmt gerade keine Schreibzugriffe an")});
                g_tpl.note(L.tr("press the programming button — if that frees it, that was the reason;"
                                " if not, file transfer is switched off in the ETS application",
                                "Programmiertaste drücken — löst das die Sperre, war es die Taste;"
                                " wenn nicht, ist der Dateitransfer in der ETS-Applikation abgeschaltet"));
                break;
            case ftc::Access::LockedOff:
                g_tpl.status(ftc::Tpl::Stat::Err, L.tr("access", "Zugriff"),
                             {L.tr("file transfer is switched off on this device",
                                   "der Dateitransfer ist auf diesem Gerät abgeschaltet")});
                break;
            default:
                // Access control arrived with FTM 0.2.0 (packed version: maj<<8 | min<<4 | rev). An older
                // server has nothing to answer, and saying "did not answer" would send the user after a fault.
                if (di.ftmVersion != 0 && di.ftmVersion < 0x0020)
                    g_tpl.status(ftc::Tpl::Stat::Ok, L.tr("access", "Zugriff"),
                                 {L.tr("this file-transfer server has no access control (it arrived with FTM 0.2.0)",
                                       "dieser Dateitransfer-Server hat keine Zugriffskontrolle (kam erst mit FTM 0.2.0)")});
                else
                    g_tpl.status(ftc::Tpl::Stat::Idle, L.tr("access", "Zugriff"),
                                 {L.tr("this device did not answer the access question",
                                       "dieses Gerät hat die Zugriffsfrage nicht beantwortet")});
                break;
        }

        // What the device can DO (delta, gzip) and what it currently ALLOWS (writes) are the same byte but
        // not the same question. The access probe is short on purpose, and when it goes unanswered its zero
        // byte used to read as "cannot do anything" -- so a device that supports differences quietly
        // received the full image, an hour instead of two minutes, with nothing said. The capability half
        // is stable, so the device-info reading of the same byte stands in for it.
        // A legacy server is a THIRD case: it answers nothing because it has no CheckFeatures, and what it
        // can is known from its version. Saying "it did not say" there would send the user after a fault
        // that does not exist -- it is simply an older server, and the reasons below are exact, not a guess.
        const bool capAnswered = acc.answered || di.features != 0 || di.ftmLegacy;
        const uint8_t capBits = acc.answered ? acc.bits : (di.ftmLegacy ? di.ftmCan : (uint8_t)di.features);
        if (di.ftmLegacy && !knxotaCheck)
            g_tpl.status(ftc::Tpl::Stat::Warn,
                         L.tr("FTM before 0.2.0 - sending the full image, uncompressed",
                              "FTM vor 0.2.0 - es geht das Voll-Image, unkomprimiert"),
                         {L.tr("a difference and compression both arrived with FTM 0.2.0",
                               "Differenz und Komprimierung kamen beide erst mit FTM 0.2.0")});
        else if (!capAnswered && !knxotaCheck)
            g_tpl.status(ftc::Tpl::Stat::Warn,
                         L.tr("the device did not say what it can do - sending the full image, uncompressed",
                              "das Gerät hat nicht gesagt, was es kann - es geht das Voll-Image, unkomprimiert"),
                         {L.tr("a difference and compression both need its answer",
                               "Differenz und Komprimierung brauchen beide seine Antwort")});

        // --- full image or only the difference -------------------------------------------------------
        // Decided here, for the same reason the compression is: only now is it known what the device can
        // take. A patch needs three things to be worth it -- the device understands one, the raw image of
        // this release is at hand, and a previous release is known to compare against. Any of them
        // missing simply means the full image goes, which is what would have happened anyway.
        std::string deltaNote;
        // What to hand the NEXT run as its base: the file the user named. Not a .app.bin beside it --
        // loadBaseImage() unwraps a package again next time, and a release ships the package, not
        // necessarily the raw image.
        std::string deltaNewApp;
        std::vector<uint8_t> newImg; // the raw application image of THIS release
        if (!knxotaCheck && (capBits & ftc::FEAT_DELTA) != 0 && !knxotaNoDelta)
        {
            // Unwrap this release the same way the base is unwrapped -- .uf2 and .factory.bin both
            // CARRY the image. Looking only for a sibling .app.bin made the difference depend on a file
            // the release does not have to ship, so a normal release quietly took the slow route.
            std::string newUsed, newWhy;
            if (!ftc::loadBaseImage(pos[knxotaVerb + 1], newImg, newUsed, newWhy))
            {
                g_tpl.status(ftc::Tpl::Stat::Idle,
                             L.tr("no raw image can be read from this file - sending the full one",
                                  "aus dieser Datei ist kein rohes Image zu lesen - es geht das Voll-Image"),
                             {newWhy});
            }
            else
            {
                std::string baseApp = knxotaFrom;
                // Nothing named on the command line: offer what this computer already knows before
                // anyone is asked to type a path. Not asking at all is how the slow route stayed
                // invisible -- the user never learned the update could have taken two minutes.
                if (baseApp.empty() && g_term.isTty() && !quiet)
                {
                    const std::vector<ftc::BaseCandidate> cands =
                        ftc::collectBaseCandidates(knxotaBaseCachePath(), targetPaEarly,
                                                   pos[knxotaVerb + 1],
                                                   L.tr("last installed from here", "zuletzt von hier eingespielt"));
                    // What it is worth, without promising a number that only building the patch can know.
                    char saving[160];
                    std::snprintf(saving, sizeof(saving),
                                  L.tr("%u KB go over the bus otherwise - a difference is usually under a tenth of that",
                                       "sonst gehen %u KB über den Bus - eine Differenz ist meist unter einem Zehntel davon"),
                                  (unsigned)(knxotaFw.payload.size() / 1024));
                    std::string picked;
                    const ftc::BasePick pick = ftc::pickBase(g_term, c, L, g_tpl, cands, saving, picked);
                    if (pick == ftc::BasePick::Quit)
                    {
                        std::fprintf(stderr, "  %s\n", c.dim(L.tr("cancelled - nothing was changed",
                                                                  "abgebrochen - es wurde nichts verändert")).c_str());
                        socketCleanup();
                        return 130;
                    }
                    // Handed over unresolved: loadBaseImage() knows how to unwrap a .uf2, which a
                    // path-only lookup cannot, and it says WHY when it cannot.
                    if (pick == ftc::BasePick::Chosen) baseApp = picked;
                }

                std::vector<uint8_t> baseImg, patch;
                std::string baseUsed, baseWhy;
                // Which hardware variant this update is for, taken from the folder the new firmware sits
                // in. A release folder holds one per variant, and the base has to be the SAME one.
                const std::string wantVariant =
                    std::filesystem::path(pos[knxotaVerb + 1]).parent_path().filename().string();
                const bool haveBase = !baseApp.empty() &&
                                      ftc::loadBaseImage(baseApp, baseImg, baseUsed, baseWhy, wantVariant);
                if (!baseApp.empty() && !haveBase)
                    g_tpl.status(ftc::Tpl::Stat::Warn,
                                 L.tr("that is not a release image - sending the full one",
                                      "das ist kein Release-Image - es geht das Voll-Image"),
                                 {baseWhy});
                // The exact length is what a base check turns on, and only the facts file states it for
                // a padded .uf2. Say so rather than let a rejected base look like the device's fault.
                if (haveBase && !newWhy.empty())
                    g_tpl.status(ftc::Tpl::Stat::Warn,
                                 L.tr("the exact image length of this release is not stated",
                                      "die genaue Image-Länge dieses Release ist nicht angegeben"),
                                 {newWhy});
                if (haveBase && !newImg.empty())
                {
                    // Remembered only once a difference was actually attempted from this file.
                    deltaNewApp = pos[knxotaVerb + 1];
                    const uint32_t bLen = (uint32_t)baseImg.size();
                    const uint32_t bCrc = ftc::delta::crc(baseImg.data(), baseImg.size());
                    ftc::BaseAnswer ans = ftc::BaseAnswer::NoMatch;
                    uint32_t arg = 0;
                    g_tpl.status(ftc::Tpl::Stat::Idle,
                                 L.tr("asking the device whether it runs the release you named",
                                      "das Gerät wird gefragt, ob es das genannte Release fährt"),
                                 {});
                    const bool baseAnswered =
                        ftc::probeBase(g_knxTunnel, targetPaEarly, bLen, bCrc, ans, arg,
                                       []() { g_knxTunnel.pump(); openknxFileTransferClient.loop(true);
                                              std::this_thread::sleep_for(std::chrono::milliseconds(2)); },
                                       []() { return nowMs(); });
                    const bool baseMatches = baseAnswered && ans == ftc::BaseAnswer::Match;
                    if (baseMatches && ftc::delta::make(baseImg, newImg, patch))
                    {
                        std::vector<uint8_t> packed;
                        bool patchPacked = false;
                        if (ftc::delta::pack(patch, packed) && packed.size() < patch.size())
                        {
                            patch.swap(packed);
                            patchPacked = true; // recorded at the swap; deducing it from the sizes afterwards
                        }                       // is wrong whenever pack() succeeded but did not help
                        // Only if it really is smaller. A difference that saves nothing costs a second
                        // mechanism for no gain, and the full image is the better-tested path.
                        if (patch.size() < knxotaFw.payload.size())
                        {
                            const size_t fullBytes = knxotaFw.payload.size();
                            const size_t diffBytes = patch.size();
                            char b[96];
                            std::snprintf(b, sizeof(b), "%u -> %u B", (unsigned)fullBytes, (unsigned)diffBytes);
                            knxotaFw.payload.swap(patch);
                            knxotaFw.compressed = true; // a patch carries its own packing; never gzip it again
                            deltaNote = b;

                            // A block, not a line. What is worth knowing here is what the difference was
                            // computed AGAINST, whether the device confirmed it, and what it buys -- three
                            // facts that a single status line had no room for.
                            const size_t saved = fullBytes - diffBytes;
                            const unsigned pctOf = (unsigned)((diffBytes * 100 + fullBytes / 2) / fullBytes);
                            const std::string baseName = std::filesystem::path(baseUsed).filename().string();
                            const std::string baseDir = std::filesystem::path(baseUsed).parent_path().string();
                            char crcTxt[24], line[160];

                            g_tpl.panelTop(L.tr("Difference update (knxOTA delta)",
                                                "Differenz-Update (knxOTA Delta)"));
                            g_tpl.kv(L.tr("Base", "Basis"),
                                     c.txt(baseName) + (dv.valid ? c.dim("   ·   " + ftc::devVersionText(dv)) : std::string()));
                            if (!baseDir.empty()) g_tpl.kv("", c.dim(g_tpl.clip(baseDir, 64)));
                            std::snprintf(crcTxt, sizeof(crcTxt), "%08X", (unsigned)bCrc);
                            g_tpl.kv(L.tr("Confirmed", "Bestätigt"),
                                     c.green(g_term.glyph("●", "*")) + " " +
                                         c.txt(L.tr("the device runs exactly this image",
                                                    "das Gerät fährt genau dieses Image")) +
                                         c.dim(std::string("      CRC ") + crcTxt));
                            std::snprintf(line, sizeof(line), "%u B", (unsigned)fullBytes);
                            g_tpl.kv(L.tr("Full image", "Voll-Image"),
                                     c.txt(line) + c.dim("   ·   " + ftc::transferEta(L, fullBytes)));
                            std::snprintf(line, sizeof(line), "%u B", (unsigned)diffBytes);
                            g_tpl.kv(L.tr("Difference", "Differenz"),
                                     c.green(line) + c.dim("   ·   " + ftc::transferEta(L, diffBytes)) +
                                         c.dim("      " + std::to_string(pctOf) + " %" +
                                               L.tr(" of it", " davon")));
                            std::snprintf(line, sizeof(line), "%u B", (unsigned)saved);
                            g_tpl.kv(L.tr("Saved", "Ersparnis"),
                                     c.txt(line) + c.dim("   ·   " + ftc::transferEta(L, saved) +
                                                         L.tr(" less", " weniger")));
                            g_tpl.kv(L.tr("Method", "Verfahren"),
                                     c.dim(std::string("OKD1") + (patchPacked ? L.tr(", packed", ", gepackt") : "") +
                                           L.tr(" - the device rebuilds the rest from the image it is running",
                                                " - das Gerät setzt den Rest aus dem laufenden Image zusammen")));
                            g_tpl.kv(L.tr("Falls back", "Fällt zurück"),
                                     c.dim(L.tr("if the checksum fails, the old firmware starts again",
                                                "schlägt die Prüfsumme fehl, startet die alte Firmware wieder")));
                            g_tpl.panelEnd();
                        }
                        else
                            g_tpl.status(ftc::Tpl::Stat::Idle,
                                         L.tr("the difference saves nothing here - sending the full image",
                                              "die Differenz spart hier nichts - es geht das Voll-Image"),
                                         {});
                    }
                    // Every other outcome ends in the full image, and each one for its own reason. Four of
                    // them used to pass in silence, which left the slow route looking like a decision
                    // nobody took.
                    else if (!baseAnswered)
                        g_tpl.status(ftc::Tpl::Stat::Warn,
                                     L.tr("the device did not answer the base check - sending the full image",
                                          "das Gerät hat die Basis-Prüfung nicht beantwortet - es geht das Voll-Image"),
                                     {L.tr("its file-transfer server may be older than the difference update",
                                           "sein Dateitransfer-Server ist evtl. älter als das Differenz-Update")});
                    else if (ans == ftc::BaseAnswer::NoMatch)
                        g_tpl.status(ftc::Tpl::Stat::Idle,
                                     L.tr("the device runs a different release than the one named - sending the full image",
                                          "das Gerät fährt ein anderes Release als das genannte - es geht das Voll-Image"),
                                     {L.tr("only the checksum settles it, never the version number",
                                           "das entscheidet nur die Prüfsumme, nie die Versionsnummer")});
                    else if (ans == ftc::BaseAnswer::Timeout)
                        g_tpl.status(ftc::Tpl::Stat::Warn,
                                     L.tr("the device was still checksumming its own image - sending the full image",
                                          "das Gerät hat sein eigenes Image noch geprüft - es geht das Voll-Image"),
                                     {L.tr("it did not say the release is wrong, only that it needed longer",
                                           "es hat nicht gesagt, das Release sei falsch, nur dass es länger braucht")});
                    else if (ans == ftc::BaseAnswer::Busy)
                        g_tpl.status(ftc::Tpl::Stat::Warn,
                                     L.tr("the device is installing an update right now",
                                          "das Gerät spielt gerade ein Update ein"),
                                     {L.tr("let it finish and start again - sending the full image otherwise",
                                           "lass es fertig werden und starte neu - sonst geht das Voll-Image")});
                    else if (ans == ftc::BaseAnswer::Failed)
                        g_tpl.status(ftc::Tpl::Stat::Warn,
                                     L.tr("the device reports that its last update failed",
                                          "das Gerät meldet, dass sein letztes Update fehlgeschlagen ist"),
                                     {ftc::deltaErrorText(L, arg)});
                    else if (ans == ftc::BaseAnswer::OutOfRange)
                        g_tpl.status(ftc::Tpl::Stat::Idle,
                                     L.tr("this image is outside what the device may read for a difference",
                                          "dieses Image liegt außerhalb dessen, was das Gerät für eine Differenz lesen darf"),
                                     {L.tr("sending the full image", "es geht das Voll-Image")});
                    else
                        g_tpl.status(ftc::Tpl::Stat::Warn,
                                     L.tr("the difference could not be built - sending the full image",
                                          "die Differenz ließ sich nicht bilden - es geht das Voll-Image"),
                                     {L.tr("the device confirmed the base, so this is on this computer",
                                           "das Gerät hat die Basis bestätigt, das liegt also an diesem Rechner")});
                }
            }
        }
        else if (!knxotaCheck && !knxotaNoDelta)
        {
            // Remembered even when this device cannot take a difference: it may be able to after its
            // next update, and then the base has to have been recorded on the run before.
            deltaNewApp = pos[knxotaVerb + 1];
        }

        // An ESP image is read raw, because only the device knows whether it can unpack one. Now that it
        // has answered, compress it if it said yes — this is where ~88 minutes on the bus become ~54.
        if (!knxotaFw.compressed && !knxotaNoCompress && (capBits & ftc::FEAT_GZIP_UPDATE) != 0)
        {
            const size_t before = knxotaFw.payload.size();
            if (ftc::compressForTarget(knxotaFw))
            {
                char b[96];
                std::snprintf(b, sizeof(b), "%u -> %u B", (unsigned)before, (unsigned)knxotaFw.payload.size());
                g_tpl.status(ftc::Tpl::Stat::Ok,
                             L.tr("this device unpacks the firmware itself",
                                  "dieses Gerät entpackt die Firmware selbst"),
                             {b, L.tr("about half the time on the bus", "etwa die halbe Zeit auf dem Bus")});
            }
        }
        else if (!knxotaFw.compressed && knxotaNoCompress)
            g_tpl.status(ftc::Tpl::Stat::Idle,
                         L.tr("sending the firmware as it is (--no-compress)",
                              "die Firmware wird unverändert gesendet (--no-compress)"), {});
        else if (!knxotaFw.compressed && capAnswered)
            g_tpl.status(ftc::Tpl::Stat::Idle,
                         L.tr("this device takes the firmware uncompressed",
                              "dieses Gerät nimmt die Firmware unkomprimiert"),
                         {L.tr("that is slower, and works with any device version",
                               "das dauert länger und funktioniert mit jeder Geräteversion")});

        if (knxotaCheck)
        {
            g_tpl.note(L.tr("nothing was transferred · drop --check to run the update",
                            "es wurde nichts übertragen · --check weglassen, um das Update auszuführen"));
            std::printf("\n");
            socketCleanup();
            // "ready" means both halves: the right firmware AND a device that would accept it. Reporting
            // success for a device that refuses every write would make --check useless in a script.
            if (acc.stage == ftc::Access::LockedOff || acc.stage == ftc::Access::Blocked) return 3;
            return verdict == ftc::Verdict::Upgrade ? 0 : 1;
        }

        // A new application number on the SAME product is what a redesign looks like. It costs the whole
        // ETS setup and the device comes back unprogrammed -- but someone who knows that may well want
        // to go ahead, so they are told exactly what it costs and asked. A script gets the refusal it
        // had before: nobody is there to weigh it up, and --force is the way to say it was meant.
        if (verdict == ftc::Verdict::DifferentApplication && !knxotaForce)
        {
            g_tpl.status(ftc::Tpl::Stat::Warn,
                         L.tr("this firmware carries a different application number",
                              "diese Firmware trägt eine andere Anwendungsnummer"),
                         {ftc::appIdText(dv.openKnxId, dv.appNumber) + L.tr(" on the device", " auf dem Gerät") +
                          "  ->  " + ftc::appIdText(knxotaFw.id.openKnxId, knxotaFw.id.appNumber) +
                          L.tr(" in the file", " in der Datei")});
            for (const char* line : {
                     L.tr("This happens when a release changes so much that it becomes a new ETS application.",
                          "Das kommt vor, wenn ein Release so viel ändert, dass daraus eine neue ETS-Applikation wird."),
                     L.tr("The transfer itself works. Afterwards the device is unprogrammed on 15.15.255 and",
                          "Die Übertragung selbst funktioniert. Danach ist das Gerät unprogrammiert auf 15.15.255 und"),
                     L.tr("has to be programmed again in ETS -- parameters, group addresses AND the address.",
                          "muss in der ETS neu programmiert werden -- Parameter, Gruppenadressen UND die Adresse."),
                     L.tr("Have the new ETS application to hand before you start.",
                          "Halte die neue ETS-Applikation bereit, bevor du startest.")})
                std::fprintf(stderr, "    %s\n", c.dim(line).c_str());
            std::fprintf(stderr, "\n");
            if (!g_term.isTty())
            {
                g_ui.errorBlock(false, L.tr("not decided -- nothing was transferred",
                                            "nicht entschieden -- es wurde nichts übertragen"),
                                {L.tr("a new application costs the device's whole setup",
                                      "eine neue Anwendung kostet die komplette Einrichtung des Geräts")},
                                L.tr("pass --force if that is what you meant",
                                     "mit --force bestätigen, wenn es so gemeint war"));
                socketCleanup();
                return 1;
            }
            if (!ftc::confirm(g_term, c, L,
                              L.tr("Install it and program the device again in ETS afterwards?",
                                   "Einspielen und das Gerät danach in der ETS neu programmieren?"), false))
            {
                std::fprintf(stderr, "  %s\n", c.dim(L.tr("cancelled -- nothing was changed",
                                                          "abgebrochen -- es wurde nichts verändert")).c_str());
                socketCleanup();
                return 130;
            }
        }

        // Guards. A file for another product is refused outright: it would wipe the device's whole setup.
        if (verdict == ftc::Verdict::DifferentDevice)
        {
            g_ui.errorBlock(false, L.tr("this firmware is for a different product",
                                        "diese Firmware gehört zu einem anderen Produkt"),
                            {ftc::appIdText(dv.openKnxId, dv.appNumber) + L.tr(" on the device", " auf dem Gerät") +
                             "  ->  " + ftc::appIdText(knxotaFw.id.openKnxId, knxotaFw.id.appNumber) +
                             L.tr(" in the file", " in der Datei"),
                             L.tr("it would wipe this device's entire setup",
                                  "damit ginge die komplette Einrichtung dieses Geräts verloren"),
                             L.tr("it would come back unprogrammed on 15.15.255",
                                  "es käme unprogrammiert auf 15.15.255 zurück")},
                            L.tr("use the firmware that belongs to this device",
                                 "die Firmware nehmen, die zu diesem Gerät gehört"));
            socketCleanup();
            return 1;
        }
        if (verdict == ftc::Verdict::AlreadyInstalled && !knxotaForce && !knxotaResume)
        {
            g_tpl.status(ftc::Tpl::Stat::Ok,
                         L.tr("this version is already installed", "diese Version läuft bereits"),
                         {ftc::devVersionText(dv)});
            // Asking beats sending the user away to re-run with a flag: re-flashing a device that behaves
            // oddly is a perfectly ordinary thing to want, and the answer is one keystroke away.
            if (!g_term.isTty())
            {
                g_tpl.note(L.tr("nothing to do — pass --force to install it again",
                                "nichts zu tun — mit --force erneut einspielen"));
                std::printf("\n");
                socketCleanup();
                return 0;
            }
            if (!ftc::confirm(g_term, c, L, L.tr("Transfer it again anyway?",
                                              "Trotzdem noch einmal übertragen?"), false))
            {
                std::fprintf(stderr, "  %s\n", c.dim(L.tr("nothing to do — the device already runs this version",
                                                          "nichts zu tun — das Gerät läuft bereits mit dieser Version")).c_str());
                socketCleanup();
                return 0;
            }
        }
        if ((verdict == ftc::Verdict::Downgrade || verdict == ftc::Verdict::Unknown) && !knxotaForce)
        {
            const std::string what = (verdict == ftc::Verdict::Downgrade)
                ? L.tr("The device is newer than this file. After a downgrade you have to re-program it in ETS.",
                       "Das Gerät ist neuer als diese Datei. Nach einem Downgrade musst du es in der ETS neu programmieren.")
                : L.tr("The versions cannot be compared, so knxOTA cannot tell whether this file fits.",
                       "Die Versionen sind nicht vergleichbar, knxOTA kann also nicht sagen, ob diese Datei passt.");
            std::fprintf(stderr, "  %s %s\n", c.amber(g_term.glyph("⚠", "!")).c_str(), c.amber(what).c_str());
            if (!ftc::confirm(g_term, c, L, L.tr("Continue anyway?", "Trotzdem fortfahren?"), false))
            {
                std::fprintf(stderr, "  %s\n", c.dim(L.tr("cancelled — nothing was changed",
                                                          "abgebrochen — es wurde nichts verändert")).c_str());
                socketCleanup();
                return 130;
            }
        }

        // Never start a half-hour transfer into a device that has already said it refuses writes.
        if (acc.stage != ftc::Access::Open && acc.stage != ftc::Access::Unknown)
        {
            g_ui.errorBlock(false, L.tr("the update cannot run like this", "so kann das Update nicht laufen"),
                            {L.tr("this device is not accepting writes",
                                  "dieses Gerät nimmt keine Schreibzugriffe an"),
                             L.tr("nothing was transferred and nothing was changed",
                                  "es wurde nichts übertragen und nichts verändert")},
                            "");
            socketCleanup();
            return 3;
        }

        // A device that cannot install a firmware itself would receive the file and leave it lying there.
        // That is half an hour for nothing, so it is a decision, not a remark.
        if (!canSelfApply)
        {
            std::fprintf(stderr, "  %s %s\n", c.amber(g_term.glyph("⚠", "!")).c_str(),
                         c.amber(L.tr("the file would be transferred and then sit unused on the device",
                                      "die Datei würde übertragen und dann ungenutzt auf dem Gerät liegen")).c_str());
            if (!ftc::confirm(g_term, c, L, L.tr("Transfer it anyway?", "Trotzdem übertragen?"), false))
            {
                std::fprintf(stderr, "  %s\n", c.dim(L.tr("cancelled — this device needs one update over USB first",
                                                          "abgebrochen — dieses Gerät braucht einmalig ein Update über USB")).c_str());
                socketCleanup();
                return 130;
            }
        }

        // The one stop before anything is written. Everything that makes this safe is stated here, because
        // this is the moment the user decides — not in the failure screen afterwards.
        {
            // Said HERE and only here: by now the difference or the full image is decided, the
            // compression is decided, and this is the payload that will really go. Still a range --
            // the interface decides the rest, and we do not know which one until bytes move.
            const std::string dur = ftc::transferEta(L, knxotaFw.payload.size());
            g_tpl.panelTop(L.tr("Ready", "Bereit"), paText);
            g_tpl.kv(L.tr("Device", "Gerät"), c.txt(di.haveOrder ? di.order : "OpenKNX") +
                     c.dim(std::string("  ·  ") + paText + L.tr("  ·  running ", "  ·  läuft mit ") + ftc::devVersionText(dv)));
            g_tpl.kv(L.tr("New", "Neu"), c.bold(ftc::fwVersionText(knxotaFw.id)));
            g_tpl.kv(L.tr("Duration", "Dauer"), c.txt(dur) +
                     c.dim(L.tr("  ·  depending on the interface (350-650 B/s)",
                                "  ·  je nach Interface (350-650 B/s)")) +
                     c.dim(L.tr("  ·  the device is away for about 30 s right at the end",
                                "  ·  das Gerät ist ganz am Ende etwa 30 Sek. weg")));
            g_tpl.panelEnd();
            for (const char* line : {
                     L.tr("The old firmware keeps running until the new one is fully transferred and its",
                          "Die alte Firmware läuft weiter, bis die neue vollständig übertragen und die"),
                     L.tr("checksum is good. If anything fails, the device starts again with the old one.",
                          "Prüfsumme in Ordnung ist. Schlägt etwas fehl, startet das Gerät wieder mit der alten."),
                     L.tr("Your ETS parameters, group addresses and the address are kept.",
                          "Deine ETS-Parameter, Gruppenadressen und die Adresse bleiben erhalten."),
                     L.tr("The rest of your installation keeps working normally.",
                          "Der Rest deiner Anlage läuft normal weiter."),
                     L.tr("You may cancel at any time — the next run continues where it stopped.",
                          "Du kannst jederzeit abbrechen — der nächste Lauf setzt dort wieder an."),
                     L.tr("Your computer must stay on during the transfer.",
                          "Dein Rechner muss während der Übertragung eingeschaltet bleiben.")})

                std::printf("    %s\n", c.dim(line).c_str());
            std::printf("\n");
            if (!knxotaForce && !ftc::confirm(g_term, c, L, L.tr("Start the update?", "Update starten?"), false))
            {
                std::fprintf(stderr, "  %s\n", c.dim(L.tr("cancelled — nothing was changed",
                                                          "abgebrochen — es wurde nichts verändert")).c_str());
                socketCleanup();
                return 130;
            }
        }

        // Hand the prepared payload to the existing upload path: it does the space check, the resume, the
        // per-packet checksum and — only on a verified checksum — the self-install.
        ftc::TempFile staged("knxota", knxotaFw.compressed ? ".bin.gz" : ".bin");
        {
            std::FILE* o = std::fopen(staged.path().c_str(), "wb");
            if (o == nullptr ||
                std::fwrite(knxotaFw.payload.data(), 1, knxotaFw.payload.size(), o) != knxotaFw.payload.size())
            {
                if (o) std::fclose(o);
                g_ui.errorBlock(false, L.tr("could not prepare the firmware", "Firmware konnte nicht vorbereitet werden"),
                                {staged.path()}, "");
                socketCleanup();
                return 2;
            }
            std::fclose(o);
        }
        if (knxotaKeepTemp)
        {
            staged.keep();
            g_tpl.note(L.tr("prepared firmware kept at: ", "vorbereitete Firmware liegt unter: ") + staged.path());
        }
        const std::string remoteName = knxotaFw.compressed ? "/fw.bin.gz" : "/fw.bin";
        const uint16_t targetPa = targetPaEarly;
        // 3x the estimate plus five minutes: enough for resends and a busy bus, still bounded.
        g_xferCapMs = (uint64_t)(knxotaFw.payload.size() / 350) * 3000ull + 300000ull;
        g_tpl.section(L.tr("Transfer", "Übertragung"));
        ftc::OtaSession sess;
        sess.crc = ftc::otaCrc32(knxotaFw.payload.data(), knxotaFw.payload.size());
        sess.bytes = (uint32_t)knxotaFw.payload.size();
        sess.file = pos[knxotaVerb + 1];
        sess.version = ftc::fwVersionText(knxotaFw.id);
        sess.hardware = knxotaFw.hardware;
        sess.ip = ip;
        sess.port = port;
        sess.pa = targetPa;
        sess.total = (uint32_t)knxotaFw.payload.size();
        sess.when = (uint64_t)std::time(nullptr);
        ftc::otaResumeUpsert(otaResumePath(), sess, otaLegacyPath());

        // A broken-off transfer resumes where it stopped, so the retry belongs HERE -- restarting the whole
        // assistant to re-answer questions nobody's answers changed is the thing worth avoiding.
        int xrc = 0;
        for (;;)
        {
            // The write window is an IDLE timeout, 30 s at the shortest, and the panels plus two questions
            // above easily take longer. Re-open it here, right before the first chunk -- reacting to the
            // refusal afterwards works too, but this is the point where the clock actually starts.
            refreshWriteWindow(targetPa);
            g_ftcSuppress = true; // arm BEFORE the request: requestUpload narrates its framing decision at once
            openknxFileTransferClient.requestUpload(targetPa, staged.path().c_str(), 0, false, 1,
                                                    canSelfApply, remoteName.c_str(), 0);
            xrc = runTransferPresenter();
            g_ftcSuppress = false;
            if (xrc == 0) break;

            sess.done = openknxFileTransferClient.status().done;
            sess.when = (uint64_t)std::time(nullptr);
            ftc::otaResumeUpsert(otaResumePath(), sess); // keep the entry, now with how far it got

            std::string why;
            // A refusal is not retried blind -- but it is almost always the write window, which closed
            // while the user was answering the questions above. That is fixable right here: ask the
            // device where it stands, let the user log in again, and go on. Sending them away to run
            // `login` by hand and start the whole update over is the wrong answer to a timeout.
            {
                const char* msg = openknxFileTransferClient.status().message;
                const bool refused = msg && (std::strstr(msg, "refused") || std::strstr(msg, "auth") ||
                                             std::strstr(msg, "locked"));
                if (refused && g_term.isTty() && !quiet && !g_abort)
                {
                    g_tpl.status(ftc::Tpl::Stat::Warn,
                                 L.tr("the device refused the transfer", "das Gerät hat die Übertragung abgelehnt"),
                                 {L.tr("its write window most likely closed while you were answering",
                                       "sein Schreibfenster ist vermutlich zugefallen, während du geantwortet hast")});
                    // The password was typed a minute ago. Use it before asking for it again.
                    if (refreshWriteWindow(targetPa))
                    {
                        g_tpl.status(ftc::Tpl::Stat::Ok,
                                     L.tr("signed in again", "erneut angemeldet"),
                                     {L.tr("with the password from this run", "mit dem Passwort aus diesem Lauf")});
                        g_tpl.section(L.tr("Transfer", "Übertragung"));
                        continue; // the bytes already on the target stay; this resumes
                    }
                    const ftc::AccessState re = resolveAccessInteractively(targetPa, 120, quiet);
                    if (re.stage == ftc::Access::Open || re.stage == ftc::Access::Unknown)
                    {
                        if (ftc::confirm(g_term, c, L,
                                         L.tr("Send it again?", "Erneut senden?"), true))
                        {
                            g_tpl.section(L.tr("Transfer", "Übertragung"));
                            continue; // the bytes already on the target stay; this resumes
                        }
                    }
                    break;
                }
            }
            const bool again = knxotaRetryable(openknxFileTransferClient.status().message, why);
            if (!again || !g_term.isTty() || quiet || g_abort) break;
            char got[80];
            std::snprintf(got, sizeof(got), "%s %.1f %% (%u / %u B)", L.tr("got to", "gekommen bis"),
                          sess.total ? sess.done * 100.0 / sess.total : 0.0, (unsigned)sess.done,
                          (unsigned)sess.total);
            g_tpl.status(ftc::Tpl::Stat::Warn, why, {got});
            g_tpl.note(L.tr("a retry continues where it stopped -- the bytes already on the target stay",
                            "ein neuer Versuch setzt dort an -- die schon übertragenen Bytes bleiben"));
            if (!ftc::confirm(g_term, c, L, L.tr("Try again?", "Erneut versuchen?"), true)) break;
            g_tpl.section(L.tr("Transfer", "Übertragung"));
        }

        if (xrc == 0 && !canSelfApply) ftc::otaResumeErase(otaResumePath(), sess); // nothing left for us to do

        // The transfer being verified is not the same as the update having happened. The device now
        // reboots into the new firmware, and only reading its version back proves that it did.
        if (xrc == 0 && canSelfApply)
        {
            g_tpl.section(L.tr("Restart", "Neustart"));
            g_tpl.note(L.tr("do not disconnect the device now — it comes back on its own",
                            "das Gerät jetzt nicht vom Strom trennen — es meldet sich von selbst zurück"));
            // The device arms PicoOTA and restarts ~2 s later, so an immediate read hits the OLD firmware.
            // Wait out the restart, then poll until the version MATCHES (merely answering proves nothing).
            const uint64_t start = nowMs();
            const std::string want = ftc::fwVersionText(knxotaFw.id);
            bool wentAway = false; // the device stopped answering -> it really restarted
            bool back = false;
            // Proof by version: the firmware that was replaced cannot report the version that replaced it.
            // Only usable when the version actually changes -- a re-flash of the same version still needs
            // the disappearance as its only evidence (see the note above observeRestart).
            bool appliedProven = false;
            std::string verBefore;   // what it reported before the restart -- the yardstick
            std::string lastSeenVer; // what it reported last -- for the diagnosis if this fails
            bool pingBack = false;   // it answers the ping again -- even if its info could not be read
            uint32_t probeCount = 0;
            uint32_t observedMs = 0;
            // Presence and version are two questions, and they must not share one query. A full `info`
            // walks 7 properties; against a device that does not answer that is 6*FTC_TIMEOUT plus the
            // optional one -- about 37 s for a single call. A reboot fits entirely inside that window, so
            // the call straddles the outage, gets its answers from the device that came back, and reports
            // "alive" the whole way through: the disappearance can never be seen. One DeviceDescriptor_Read
            // answers the same question in ~30 ms (2.4 s worst case), which is what makes polling work.
            ftc::ReachDeps rdeps;
            rdeps.pump = []() { g_knxTunnel.pump(); };
            rdeps.nowMs = []() { return nowMs(); };
            rdeps.aborted = []() { return g_abort != 0; };
            auto probeAlive = [&]() {
                int tries = 0;
                uint32_t spent = 0;
                return ftc::deviceAnswers(g_knxTunnel, targetPa, rdeps, tries, spent);
            };

            // Only asked once the ping says it is back -- this is the expensive one.
            auto readVersion = [&]() {
                g_ftcSuppress = true;
                std::vector<FtcEntry> vsnap;
                openknxFileTransferClient.processCommand("ftc " + paText + " info", false);
                ftcPumpStructured(vsnap, false, false);
                g_ftcSuppress = false;
                return openknxFileTransferClient.deviceInfo().valid;
            };

            // FIRST it has to GO. Re-flashing the same version leaves the version unchanged, so a version
            // comparison alone cannot tell an update from a device that never restarted -- and that is
            // exactly what once reported "done" for an apply that was silently refused.
            // A patch is not installed when the trigger arrives -- the device UNPACKS it first, and stays
            // reachable while it does. Measured on an 815 KB image: 16 seconds. Waiting only for it to
            // disappear therefore declares a running rebuild dead. FwProbe with an empty payload answers
            // "busy / failed / nothing", so the rebuild can be watched instead of guessed at.
            // The same watcher the standalone `fwupdate` uses -- one question, one implementation.
            auto watchRebuild = [&]() { return watchFirmwareInstall(targetPa); };

            // How long to watch. An RP2040/RP2350 reboots within seconds, but the image is applied by the
            // bootloader AFTERWARDS, so the gap can be short and the return far out. Both budgets are
            // generous on purpose; every loop leaves as soon as its outcome is decided.
            const uint32_t GONE_WINDOW_MS = 30000;   // watching for it to stop answering
            const uint32_t BACK_WINDOW_MS = 240000;  // total budget once it HAS gone
            const uint32_t CONFIRM_MS = 12000;       // gap never seen -> just re-read the version, briefly

            auto observeRestart = [&]() {
                const uint64_t t0 = nowMs();
                wentAway = false;
                back = false;
                appliedProven = false;
                pingBack = false;
                probeCount = 0; // paired with observedMs in the diagnosis -- both must count the same run
                lastSeenVer.clear();

                // The yardstick, taken before anything restarts.
                {
                    const FtcDeviceInfo& pre = openknxFileTransferClient.deviceInfo();
                    const ftc::DevVersion pv = ftc::devVersionFrom(pre.hardware, pre.haveHw, pre.version, pre.haveVersion);
                    if (pv.valid) verBefore = ftc::devVersionText(pv);
                }

                // Phase 1: it should stop answering. Polling for that is a race -- missing the gap is not
                // a verdict, so phase 2 runs either way and decides on the version.
                while (nowMs() - t0 < GONE_WINDOW_MS && !g_abort && !wentAway)
                {
                    g_tpl.waitTick(L.tr("the device is restarting", "das Gerät startet neu"),
                                   (uint32_t)((nowMs() - start) / 1000), L.tr("normally 15-40 s", "normal 15-40 s"));
                    ++probeCount;
                    if (!probeAlive()) wentAway = true;
                    else
                        std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }

                // Phase 2: it has to be BACK on the wanted version. Patient once the gap was seen, brief
                // when it was not -- there the question is only whether the version moved after all.
                // Measured from HERE, not from t0: phase 1 may have burned its whole window, and a budget
                // counted from t0 would then be spent before phase 2 runs even once -- which is exactly
                // the case this phase exists for.
                const uint64_t t2 = nowMs();
                const uint32_t budget = wentAway ? BACK_WINDOW_MS : CONFIRM_MS;
                while (nowMs() - t2 < budget && !g_abort)
                {
                    g_tpl.waitTick(wentAway ? L.tr("waiting for the device", "warte auf das Gerät")
                                            : L.tr("checking the version", "prüfe die Version"),
                                   (uint32_t)((nowMs() - start) / 1000),
                                   L.tr("normally 15-40 s", "normal 15-40 s"));
                    ++probeCount;
                    // Two separate facts: it answers again (ping), and its info could be read. Folding
                    // them into one flag reported a device that was demonstrably back as "not come back".
                    const bool alive = probeAlive();
                    if (alive) pingBack = true;
                    if (alive && readVersion())
                    {
                        back = true;
                        const FtcDeviceInfo& now = openknxFileTransferClient.deviceInfo();
                        const ftc::DevVersion seen = ftc::devVersionFrom(now.hardware, now.haveHw, now.version, now.haveVersion);
                        if (seen.valid) lastSeenVer = ftc::devVersionText(seen);
                        if (seen.valid && lastSeenVer == want)
                        {
                            // A version that moved TO the wanted one settles it on its own. Unchanged
                            // version on a re-flash proves nothing -- there the disappearance still rules.
                            appliedProven = !verBefore.empty() && verBefore != want;
                            break;
                        }
                    }
                    else
                        back = false;
                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                }
                observedMs = (uint32_t)(nowMs() - t0);
                std::printf("\r\x1b[K");
            };
            const bool rebuilt = watchRebuild();
            if (rebuilt) observeRestart();

            // It did not restart -> the apply was refused. Everything needed to put that right is known
            // here: the target, the file on it, and why it was refused. Printing a command for the user
            // to retype would be handing back a job this already has in its hands.
            if (!appliedProven && !wentAway && g_term.isTty() && !quiet && !g_abort)
            {
                g_ui.errorBlock(false,
                                L.tr("the device did not restart - the update was NOT applied",
                                     "das Gerät hat nicht neu gestartet - das Update wurde NICHT eingespielt"),
                                {L.tr("the transfer itself was fine: the file is on the device",
                                      "die Übertragung selbst war in Ordnung: die Datei liegt auf dem Gerät"),
                                 L.tr("a device that refuses writes accepts the file and declines to install it",
                                      "ein Gerät, das Schreiben verweigert, nimmt die Datei an und lehnt das Einspielen ab")});
                // Ask the device where it stands NOW -- and let the user log in again if that is the reason.
                const bool reopened = refreshWriteWindow(targetPa); // the password from this run, silently
                ftc::AccessState re;
                if (reopened) { re.stage = ftc::Access::Open; re.answered = true; }
                else
                    re = resolveAccessInteractively(targetPa, 120, quiet);
                if (re.stage == ftc::Access::Open || re.stage == ftc::Access::Unknown)
                {
                    g_tpl.panelTop(L.tr("Install now", "Jetzt einspielen"), paText);
                    g_tpl.kv(L.tr("File", "Datei"), c.txt(remoteName) + c.dim("   " + kbStr((uint32_t)knxotaFw.payload.size()) + " KB"));
                    g_tpl.kv(L.tr("Device", "Gerät"), c.txt(paText) + c.dim("   " + ip));
                    g_tpl.panelEnd();
                    if (ftc::confirm(g_term, c, L,
                                     L.tr("Trigger the update now?", "Update jetzt auslösen?"), false))
                    {
                        g_ftcSuppress = false; // this one is the point of the exercise -- never swallow it
                        openknxFileTransferClient.requestFwUpdate(targetPa, remoteName.c_str());
                        runOneShotToQuiescence();
                        observeRestart(); // same proof as before: it has to go, then come back
                    }
                }
                else
                    g_tpl.note(std::string("oknx --ip ") + ip + " " + paText + " login <pw>   &&   " +
                               "oknx --ip " + ip + " " + paText + " fwupdate " + remoteName);
            }
            const FtcDeviceInfo& after = openknxFileTransferClient.deviceInfo();
            const ftc::DevVersion nv = ftc::devVersionFrom(after.hardware, after.haveHw, after.version, after.haveVersion);
            if (!appliedProven && !wentAway)
            {
                // Still not restarted -- either the offer above was declined, or the second attempt was
                // refused too. The entry stays, so the run can be picked up once the reason is gone.
                // Say what this rests on: the verdict comes from an observation that can be missed, and
                // without the numbers the next occurrence is guesswork again.
                char diag[192];
                if (probeCount == 0)
                    // Nothing was watched at all -- the apply was never triggered (offer declined, or no
                    // rebuild to wait for). Claiming an observation here would invent evidence.
                    std::snprintf(diag, sizeof(diag), "%s",
                                  L.tr("the restart was not observed: the apply was not triggered",
                                       "der Neustart wurde nicht beobachtet: das Einspielen wurde nicht ausgelöst"));
                else
                    std::snprintf(diag, sizeof(diag),
                                  L.tr("watched %u s, %u queries, never stopped answering; version %s -> %s (expected %s)",
                                       "%u s beobachtet, %u Abfragen, hat durchgehend geantwortet; Version %s -> %s (erwartet %s)"),
                                  (unsigned)(observedMs / 1000), (unsigned)probeCount,
                                  verBefore.empty() ? "?" : verBefore.c_str(),
                                  lastSeenVer.empty() ? "?" : lastSeenVer.c_str(),
                                  ftc::fwVersionText(knxotaFw.id).c_str());
                g_tpl.status(ftc::Tpl::Stat::Err,
                             L.tr("the firmware was not applied", "die Firmware wurde nicht eingespielt"),
                             {L.tr("the file stays on the device - nothing has to be transferred again",
                                   "die Datei bleibt auf dem Gerät - es muss nichts neu übertragen werden"),
                              std::string(diag)});
            }
            else if (appliedProven || (back && nv.valid && ftc::devVersionText(nv) == ftc::fwVersionText(knxotaFw.id)))
            {
                ftc::otaResumeErase(otaResumePath(), sess); // proven done -> never offer this run again
                // Recorded only here, where the device has confirmed the version it came back with: a
                // base remembered after a transfer that never took effect would be offered as truth and
                // then refused by the device on the next run.
                if (!deltaNewApp.empty())
                    ftc::baseCacheRemember(knxotaBaseCachePath(), targetPa, deltaNewApp);
                g_tpl.status(ftc::Tpl::Stat::Ok,
                             std::string(L.tr("done — ", "fertig — ")) + paText +
                                 L.tr(" is now running ", " läuft jetzt mit ") + ftc::devVersionText(nv),
                             {std::string(L.tr("was ", "vorher ")) + ftc::devVersionText(dv)});
                g_tpl.note(L.tr("your ETS parameters and group addresses are unchanged",
                                "deine ETS-Parameter und Gruppenadressen sind unverändert"));
            }
            else if (back)
                g_tpl.status(ftc::Tpl::Stat::Warn,
                             L.tr("the device answered, but reports a different version than expected",
                                  "das Gerät antwortet, meldet aber eine andere Version als erwartet"),
                             {ftc::devVersionText(nv)});
            else
            {
                // It went away and never answered on THIS address again. Before blaming the connection,
                // ask the default address: a firmware that does not carry the physical address over comes
                // back unprogrammed on 15.15.255. An answer there proves nothing on its own -- every
                // unprogrammed device sits on it. The VERSION it reports does prove it: nothing but the
                // firmware just flashed can report the version just flashed.
                if (pingBack)
                {
                    // It answers on its own address again -- the restart happened and it is there. Only
                    // its info stayed unreadable, which is a different fault from "gone". Probing the
                    // default address here would be nonsense: it is demonstrably not on it.
                    g_ui.errorBlock(true, L.tr("the device answers again, but its info could not be read",
                                               "das Gerät antwortet wieder, seine Info war aber nicht lesbar"),
                                    {std::string(L.tr("it restarted and is back on ", "es hat neu gestartet und ist wieder auf ")) + paText,
                                     std::string(L.tr("whether the new firmware is running could not be established",
                                                      "ob die neue Firmware läuft, liess sich nicht feststellen"))},
                                    std::string("oknx --ip ") + ip + " " + paText + L.tr(" info   asks it again",
                                                                                        " info   fragt es erneut"));
                    socketCleanup();
                    return 6;
                }

                const uint16_t DEFAULT_PA = 0xFFFF; // 15.15.255
                bool defAnswers = false;
                std::string defVer;
                if (!g_abort)
                {
                    int dtries = 0;
                    uint32_t dspent = 0;
                    defAnswers = ftc::deviceAnswers(g_knxTunnel, DEFAULT_PA, rdeps, dtries, dspent);
                    if (defAnswers)
                    {
                        g_ftcSuppress = true;
                        std::vector<FtcEntry> dsnap;
                        openknxFileTransferClient.processCommand("ftc 15.15.255 info", false);
                        ftcPumpStructured(dsnap, false, false);
                        g_ftcSuppress = false;
                        const FtcDeviceInfo& di = openknxFileTransferClient.deviceInfo();
                        const ftc::DevVersion dvv = ftc::devVersionFrom(di.hardware, di.haveHw, di.version, di.haveVersion);
                        if (di.valid && dvv.valid) defVer = ftc::devVersionText(dvv);
                    }
                }

                if (defAnswers && !defVer.empty() && defVer == want)
                {
                    // Settled: the update worked, the device just lost its address with it. Treating this
                    // as a failure would send the user chasing a transfer that is already on the device.
                    ftc::otaResumeErase(otaResumePath(), sess);
                    if (!deltaNewApp.empty())
                        ftc::baseCacheRemember(knxotaBaseCachePath(), targetPa, deltaNewApp);
                    g_tpl.status(ftc::Tpl::Stat::Warn,
                                 std::string(L.tr("the firmware is on the device -- it now sits on 15.15.255 with ",
                                                  "die Firmware ist auf dem Gerät -- es sitzt jetzt auf 15.15.255 mit ")) + defVer,
                                 {L.tr("this firmware does not carry the physical address over, so it came back unprogrammed",
                                       "diese Firmware übernimmt die physikalische Adresse nicht, sie kam unprogrammiert zurück"),
                                  L.tr("the version there is the one just transferred -- nothing else can report it",
                                       "die Version dort ist die soeben übertragene -- nichts anderes kann sie melden"),
                                  L.tr("program it with ETS to give it its address and parameters back",
                                       "mit der ETS programmieren, damit es Adresse und Parameter zurückbekommt")});
                    socketCleanup();
                    return 0;
                }

                // What the default address told us, in one line -- the wording has to stay honest about
                // how much it proves, which is why the three cases read differently.
                std::string defLine;
                if (!defAnswers)
                    defLine = L.tr("nothing answers on 15.15.255 either, so it did not come back unprogrammed",
                                   "auf 15.15.255 antwortet ebenfalls nichts, es kam also nicht unprogrammiert zurück");
                else if (defVer.empty())
                    defLine = L.tr("something answers on 15.15.255 but reports no version -- it may be this device",
                                   "auf 15.15.255 antwortet etwas, meldet aber keine Version -- es kann dieses Gerät sein");
                else
                    defLine = std::string(L.tr("15.15.255 answers with ", "15.15.255 meldet ")) + defVer +
                              L.tr(", not the transferred version -- probably a different device",
                                   ", nicht die übertragene Version -- vermutlich ein anderes Gerät");

                g_ui.errorBlock(false, L.tr("the device has not come back on this address",
                                            "das Gerät hat sich auf dieser Adresse nicht zurückgemeldet"),
                                {std::string(L.tr("it did stop answering, so the restart itself happened",
                                                  "es hat aufgehört zu antworten, der Neustart hat also stattgefunden")),
                                 defLine,
                                 std::string(L.tr("a device whose flash failed starts the old firmware again by itself",
                                                  "ein Gerät, dessen Flash fehlschlug, startet die alte Firmware von selbst wieder"))},
                                std::string("oknx --ip ") + ip + L.tr(" scan   finds it under the address it has now",
                                                                    " scan   findet es unter der Adresse, die es jetzt hat"));
                socketCleanup();
                return 6;
            }
        }
        socketCleanup();
        if (xrc == 0) return 0;
        // Exit code 1 means "this firmware does not belong to this device" -- the wrappers print exactly
        // that. Returning it for ANY failed transfer told a user whose write window had closed that their
        // firmware was wrong. A refusal is 3 (not accepting writes), anything else 4 (did not complete).
        {
            const std::string why = openknxFileTransferClient.status().message;
            if (why.find("refused") != std::string::npos ||
                why.find("auth") != std::string::npos ||
                why.find("locked") != std::string::npos)
                return 3;
        }
        return 4;
    }

    // Host-side auto-gzip: `send <src> gzip` compresses the local file (in-process, miniz) BEFORE the upload
    // — RP firmware prep (a raw .bin will not fit). We rewrite the source to a temp .gz; the on-device send
    // path is unchanged. The device's fwupdate/gzip loader accepts the real gzip stream.
    if (pos.size() >= 3 && (pos[1] == "send" || pos[1] == "upload"))
    {
        for (size_t i = 3; i < pos.size(); ++i)
        {
            if (pos[i] != "gzip" && pos[i] != "gz") continue;
            pos.erase(pos.begin() + i);
            const std::string src = pos[2];
            std::string base = src;
            const size_t sl = base.find_last_of("/\\");
            if (sl != std::string::npos) base = base.substr(sl + 1);
            const char* td = std::getenv("TMPDIR");
            std::string tmp = (td && *td) ? std::string(td) : std::string("/tmp");
            if (!tmp.empty() && tmp.back() != '/') tmp += '/';
            const std::string gz = tmp + base + ".gz";
            if (ftc::gzipFile(src, gz))
            {
                if (!quiet)
                    std::printf("  %s gzip  %s %s %s\n", g_theme.green(g_term.glyph("●", "*")).c_str(),
                                g_theme.dim(src).c_str(), g_theme.dim(g_term.glyph("→", "->")).c_str(), g_theme.green(gz).c_str());
                pos[2] = gz;
            }
            else
                std::fprintf(stderr, "  %s\n", g_theme.red(g_i18n.tr("gzip failed — sending the raw file", "gzip fehlgeschlagen — sende die rohe Datei")).c_str());
            break;
        }
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
    // The connection banner (header/footer) prints only on an interactive, colored, non-quiet console.
    const bool consoleBarOn = consoleMode && !quiet && g_term.isTty();

    // Drain-cap default for the console. A KNXnet/IP interface only relays a bus frame that fits its max
    // APDU; a constrained interface (e.g. Siemens/MDT, max APDU 15) drops the target's oversized (extended)
    // drain answer entirely. So a plain `con` (no size) defaults to a STANDARD-frame-safe cap -> the console
    // works through such an interface out of the box. `con max` = full window (fast on OpenKNX interfaces
    // that carry extended frames); `con <N>` = an explicit cap. Mirrors requestConsole()'s range check.
    unsigned drainCap = 0; // resolved console drain cap (B/answer) — carried out to the ConsoleUi verbose line
    if (consoleMode)
    {
        // Manual APDU override: `con ... apdu <M>` replaces the auto-detected interface APDU for BOTH the
        // framing (setApduHint) AND the drain-cap derivation below. Default (no `apdu`) keeps the reported APDU.
        for (size_t i = 2; i + 1 < pos.size(); ++i)
        {
            if (pos[i] != "apdu") continue;
            unsigned v = 0;
            if (std::sscanf(pos[i + 1].c_str(), "%u", &v) == 1 && v >= 15 && v <= 254)
            {
                ifaceApdu = (uint16_t)v;
                g_ifaceApdu = ifaceApdu;
    openknxFileTransferClient.setApduHint(ifaceApdu); // re-apply: framing now uses the manual APDU
                if (!quiet && !consoleBarOn) std::printf("[console] APDU override -> %u B (manual)\n", (unsigned)ifaceApdu);
            }
            pos.erase(pos.begin() + i, pos.begin() + i + 2); // drop both tokens so the drain parser stays clean
            break;
        }
        // auto (default): derive the drain cap from the interface's detected max APDU (device-mgmt), so the
        // console runs as fast as the link carries with NO static guess and NO slow probing. cap = APDU - 7
        // (the FunctionPropertyState_Response header), clamped to [4, 246]; a >=254-APDU link -> full window.
        // `con max` = full window; `con <N>` = fixed cap; `con auto`/no arg = the derived value. When
        // detection failed (APDU unknown), fall back to a standard-frame-safe 8.
        unsigned autoCap = 8; // fallback when the interface did not report an APDU
        if (ifaceApdu >= 254) autoCap = 247;
        else if (ifaceApdu >= 11)
        {
            int c = (int)ifaceApdu - 7;
            autoCap = (c > 246) ? 246u : (unsigned)c;
        }

        const std::string arg = pos.size() >= 3 ? pos[2] : std::string();
        unsigned eff;
        const char* how;
        if (arg == "max")
        {
            eff = 247;
            how = "full window (fixed)";
        }
        else if (!arg.empty() && arg != "auto")
        {
            unsigned v = 0;
            eff = (std::sscanf(arg.c_str(), "%u", &v) == 1 && v >= 4 && v < 247) ? v : 247;
            how = (eff >= 247) ? "full window (fixed)" : "fixed";
        }
        else
        {
            eff = autoCap;
            how = ifaceApdu ? "auto (from detected interface APDU)" : "auto (fallback; APDU unknown)";
        }

        // Rebuild the command so the shared parser reads the resolved numeric cap (247 -> full window).
        drainCap = eff; // hand the resolved cap to the verbose status line
        cmd = std::string("ftc ") + pos[0] + ' ' + pos[1] + ' ' + std::to_string(eff);
        if (!quiet && !consoleBarOn) std::printf("[console] drain cap %u B/answer -- %s\n", eff, how);
    }

    int exitCode = 0;

    unsigned rp_a = 0, rp_l = 0, rp_d = 0;
    const bool reachHasPa = !pos.empty() &&
                            std::sscanf(pos[0].c_str(), "%u.%u.%u", &rp_a, &rp_l, &rp_d) == 3 &&
                            rp_a <= 15 && rp_l <= 15 && rp_d <= 255;
    // EVERY command that names a device probes reachability first: ~30 ms when present, ~2.5 s with a reason
    // when not (instead of a bare timeout, or minutes of retries). Excluded: ping/cancel/status.
    const bool paCmd = pos.size() >= 2 && pos[1] != "ping" && pos[1] != "p" && pos[1] != "cancel" &&
                       pos[1] != "c" && pos[1] != "status" && pos[1] != "s";
    if ((paCmd || consoleMode) && !knxotaActive && reachHasPa && !knxotaForce)
    {
        const uint16_t tgt = (uint16_t)((rp_a << 12) | (rp_l << 8) | rp_d);
        ftc::ReachDeps rd;
        rd.pump = []() { g_knxTunnel.pump(); };
        rd.nowMs = []() { return nowMs(); };
        rd.aborted = []() { return g_abort != 0; };
        int tries = 0;
        uint32_t spent = 0;
        if (!ftc::deviceAnswers(g_knxTunnel, tgt, rd, tries, spent))
        {
            ftc::I18n& L = g_i18n;
            char det[96];
            std::snprintf(det, sizeof(det), L.tr("%d attempts, %.1f s", "%d Versuche, %.1f s"), tries,
                          spent / 1000.0);
            reportUnreachable(pos[0], tgt, det);
            if (!g_term.isTty())
            {
                socketCleanup();
                return 6;
            }
            if (!ftc::confirm(g_term, g_theme, L, L.tr("try anyway?", "trotzdem versuchen?")))
            {
                socketCleanup();
                return 6;
            }
        }
    }

    // `oknx -i <ip> setpa <x.y.z>` - the ETS "programme the individual address" procedure. It addresses
    // nobody: the device is picked by being in PROGRAMMING MODE, so the whole exchange is broadcast.
    // 03_05_02: read who is in programming mode, refuse unless there is exactly one, write, read back.
    if (!pos.empty() && pos[0] == "setpa")
    {
        ftc::I18n& L = g_i18n;
        unsigned a1 = 0, b1 = 0, c1 = 0;
        char tail = 0;
        const bool ok = pos.size() >= 2 &&
                        std::sscanf(pos[1].c_str(), "%u.%u.%u%c", &a1, &b1, &c1, &tail) == 3 &&
                        a1 <= 15 && b1 <= 15 && c1 <= 255;
        if (!ok)
        {
            g_ui.errorBlock(false, L.tr("setpa needs the new individual address",
                                        "setpa braucht die neue physikalische Adresse"),
                            {L.tr("put exactly ONE device into programming mode first",
                                  "vorher genau EIN Gerät in den Programmiermodus bringen")},
                            "oknx -i <ip> setpa 1.1.42");
            socketCleanup();
            return 2;
        }
        const uint16_t want = (uint16_t)((a1 << 12) | (b1 << 8) | c1);

        static std::vector<uint16_t> s_inProg;
        s_inProg.clear();
        g_knxTunnel.setBroadcastCallback([](uint16_t src, uint16_t apci, const uint8_t*, uint8_t) {
            if ((apci & 0x03C0) != APCI_INDADDR_RESP) return; // A_IndividualAddress_Response
            for (uint16_t p : s_inProg)
                if (p == src) return;
            s_inProg.push_back(src);
        });
        auto probe = [&](uint32_t ms) {
            s_inProg.clear();
            g_knxTunnel.sendIndividualAddressRead();
            for (uint64_t t0 = nowMs(); nowMs() - t0 < ms;) g_knxTunnel.pump();
        };
        probe(2500);
        if (s_inProg.size() != 1)
        {
            g_knxTunnel.setBroadcastCallback(nullptr);
            char det[120];
            std::snprintf(det, sizeof(det),
                          L.tr("%zu devices answered - it has to be exactly one",
                               "%zu Geräte haben geantwortet - es muss genau eines sein"),
                          s_inProg.size());
            std::string who;
            for (uint16_t p : s_inProg) who += (who.empty() ? "" : " · ") + ftc::Tpl::pa(p);
            g_ui.errorBlock(false, L.tr("not exactly one device in programming mode",
                                        "nicht genau ein Gerät im Programmiermodus"),
                            {det, who.empty() ? L.tr("press the programming button on the device",
                                                     "die Programmiertaste am Gerät drücken") : who,
                             L.tr("nothing was sent", "es wurde nichts gesendet")},
                            "oknx -i <ip> ps");
            g_knxTunnel.disconnect();
            socketCleanup();
            return 2;
        }
        const uint16_t had = s_inProg[0];
        g_knxTunnel.sendIndividualAddressWrite(want);
        for (uint64_t t0 = nowMs(); nowMs() - t0 < 600;) g_knxTunnel.pump();
        probe(2500); // read back: the same device now has to answer with the new address
        g_knxTunnel.setBroadcastCallback(nullptr);
        const bool took = s_inProg.size() == 1 && s_inProg[0] == want;
        if (quiet)
            std::printf("setpa\t%s\t%s\t%d\n", ftc::Tpl::pa(had).c_str(), pos[1].c_str(), took ? 1 : 0);
        else if (took)
            g_tpl.status(ftc::Tpl::Stat::Ok,
                         ftc::Tpl::pa(had) + "  ->  " + pos[1],
                         {L.tr("confirmed by reading it back; switch programming mode off now",
                               "durch Rücklesen bestätigt; jetzt den Programmiermodus ausschalten")});
        else
            g_tpl.status(ftc::Tpl::Stat::Err, L.tr("the address was not taken", "die Adresse wurde nicht übernommen"),
                         {L.tr("the read-back did not show it", "das Rücklesen zeigt sie nicht")});
        g_knxTunnel.disconnect();
        socketCleanup();
        return took ? 0 : 1;
    }

    // `ga` is bus traffic, not a device command: it addresses a GROUP, so it names no PA, skips the
    // reachability probe above and the FTC permission gate below. A GroupValue_Read is answered by every
    // device that has the read flag set on that address, so the answers are collected, not awaited singly.
    if (!consoleMode && !knxotaActive && !pos.empty() && pos[0] == "ga")
    {
        ftc::I18n& L = g_i18n;
        const std::string op = pos.size() >= 2 ? pos[1] : "";
        unsigned m = 0, mid = 0, sub = 0;
        // `<x/y/z>:<DPT>` gives the type for this one call, without importing anything.
        std::string gaTok = pos.size() >= 3 ? pos[2] : "";
        uint16_t dptMain = 0, dptSub = 0;
        const size_t gaColon = gaTok.find(':');
        if (gaColon != std::string::npos)
        {
            ftc::GaTable::parseDpt(gaTok.substr(gaColon + 1), dptMain, dptSub);
            gaTok = gaTok.substr(0, gaColon);
        }
        // 0/0/0 is NOT a group address: 03_05_01 reserves 0000h for broadcast, and a frame with
        // address type "group" and destination 0 is a T_Data_Broadcast that every device acts on.
        const bool haveGa = !gaTok.empty() &&
                            std::sscanf(gaTok.c_str(), "%u/%u/%u", &m, &mid, &sub) == 3 &&
                            m <= 31 && mid <= 7 && sub <= 255 && (m | mid | sub) != 0;
        if ((op != "read" && op != "write") || !haveGa)
        {
            g_ui.errorBlock(false, L.tr("ga needs read or write and a group address",
                                        "ga braucht read oder write und eine Gruppenadresse"),
                            {L.tr("nothing was sent", "es wurde nichts gesendet")},
                            "oknx -i <ip> ga read 1/4/5   ·   oknx -i <ip> ga write 1/4/5 1");
            g_knxTunnel.disconnect();
            socketCleanup();
            return 2;
        }
        const uint16_t ga = (uint16_t)((m << 11) | (mid << 8) | sub);

        // 03_03_07 3.1.3 p.16-17: the A_GroupValue_Write form follows the DATAPOINT's length, not the
        // size of the number - six bits or less rides in the APCI octet. With a known type the stack's
        // converter decides; without one, a plain 0-63 is the compact form and hex octets the long one.
        uint8_t small = 0, payload[14], plen = 0;
        if (op == "write")
        {
            if (pos.size() < 4)
            {
                g_ui.errorBlock(false, L.tr("write needs a value", "write braucht einen Wert"),
                                {L.tr("with an imported type: the value itself (21.5, 1, 80)",
                                      "mit importiertem Typ: der Wert selbst (21.5, 1, 80)"),
                                 L.tr("without one: 0-63 plain, or hex octets",
                                      "ohne Typ: 0-63 blank, oder Hex-Oktette")},
                                "oknx -i <ip> ga write 1/4/5 1   ·   ga write 0/7/0:9.001 21.5");
                g_knxTunnel.disconnect();
                socketCleanup();
                return 2;
            }
            uint16_t dMain = dptMain, dSub = dptSub; // from an inline `<ga>:<DPT>`, else the table
            if (dMain == 0)
            {
                ftc::GaTable t;
                t.load(ip);
                const ftc::GaInfo* gi = t.find(ga);
                if (gi != nullptr) { dMain = gi->main; dSub = gi->sub; }
            }
            if (dMain != 0)
            {
                // Sub-byte datapoints (1 bit, 2 bit, 4 bit) are the compact form; everything else is
                // carried in its own octets, as many as the type declares.
                const bool subByte = (dMain == 1 || dMain == 2 || dMain == 3 || dMain == 23);
                const Dpt d((short)dMain, (short)dSub);
                const size_t need = subByte ? 1u : (size_t)d.dataLength();
                KNXValue v(false);
                const std::string& tok = pos[3];
                char* end = nullptr;
                if (dMain == 16) v = tok.c_str();
                else if (dMain == 9 || dMain == 14)
                {
                    const double dv = std::strtod(tok.c_str(), &end);
                    if (end == nullptr || *end != 0) dMain = 0;
                    v = dv;
                }
                else
                {
                    const long lv = std::strtol(tok.c_str(), &end, 0);
                    if (end == nullptr || *end != 0) dMain = 0;
                    v = (int64_t)lv;
                }
                uint8_t buf[16] = {0};
                if (dMain != 0 && need <= sizeof(buf) && KNX_Encode_Value(v, buf, need, d))
                {
                    if (subByte) small = (uint8_t)(buf[0] & 0x3F);
                    else { plen = (uint8_t)(need > sizeof(payload) ? sizeof(payload) : need); std::memcpy(payload, buf, plen); }
                }
                else
                {
                    g_ui.errorBlock(false,
                                    L.tr("that value does not fit the datapoint type",
                                         "dieser Wert passt nicht zum Datenpunkttyp"),
                                    {ftc::GaTable::gaStr(ga) + "  DPT " + std::to_string(dMain) + "." +
                                         std::to_string(dSub),
                                     L.tr("nothing was sent", "es wurde nichts gesendet")},
                                    "oknx -i <ip> ga write 0/7/0 21.5");
                    g_knxTunnel.disconnect();
                    socketCleanup();
                    return 2;
                }
            }
            else
            {
                const bool hex = pos[3].rfind("0x", 0) == 0 || pos[3].rfind("0X", 0) == 0;
                if (!hex && pos.size() == 4)
                {
                    char* end = nullptr;
                    long v = std::strtol(pos[3].c_str(), &end, 10);
                    if (end == nullptr || *end != 0 || v < 0 || v > 63)
                    {
                        g_ui.errorBlock(false, L.tr("a plain value must be 0-63 without a known type",
                                                    "eine blanke Zahl muss ohne bekannten Typ 0-63 sein"),
                                        {L.tr("import the project, or give the type: ga write 0/7/0:9.001 21.5",
                                              "Projekt importieren, oder Typ angeben: ga write 0/7/0:9.001 21.5")},
                                        "oknx -i <ip> ga import GA-Export.xml");
                        g_knxTunnel.disconnect();
                        socketCleanup();
                        return 2;
                    }
                    small = (uint8_t)v;
                }
                else
                {
                    for (size_t i = 3; i < pos.size(); i++)
                    {
                        char* end = nullptr;
                        long v = std::strtol(pos[i].c_str(), &end, 16);
                        if (end == nullptr || *end != 0 || v < 0 || v > 255 || plen >= sizeof(payload))
                        {
                            g_ui.errorBlock(false,
                                            plen >= sizeof(payload)
                                                ? L.tr("more than 14 octets", "mehr als 14 Oktette")
                                                : L.tr("not a hex octet", "kein Hex-Oktett") + std::string(": ") + pos[i],
                                            {L.tr("nothing was sent", "es wurde nichts gesendet")}, "0x00 .. 0xFF");
                            g_knxTunnel.disconnect();
                            socketCleanup();
                            return 2;
                        }
                        payload[plen++] = (uint8_t)v;
                    }
                }
            }
        }

        static std::vector<std::string> s_gaHits;
        s_gaHits.clear();
        g_knxTunnel.setGroupCallback([](uint16_t src, uint16_t g, uint16_t apci, const uint8_t* d, uint8_t n) {
            if ((apci & 0x03C0) != APCI_GROUPVALUE_RESP) return; // GroupValue_Response only
            char line[128];
            int k = std::snprintf(line, sizeof(line), "%u.%u.%u  %u/%u/%u  ", (src >> 12) & 0x0F,
                                  (src >> 8) & 0x0F, src & 0xFF, (g >> 11) & 0x1F, (g >> 8) & 0x07, g & 0xFF);
            if (n == 0)
                std::snprintf(line + k, sizeof(line) - k, "0x%02X (6 bit)", apci & 0x3F);
            else
                for (uint8_t i = 0; i < n && k < (int)sizeof(line) - 4; i++)
                    k += std::snprintf(line + k, sizeof(line) - k, "%02X ", d[i]);
            s_gaHits.push_back(line);
        });
        const bool sent = g_knxTunnel.sendGroupValue(ga, op == "write", small, payload, plen);
        // A read has no single addressee, so there is no timeout to fail on - collect for a fixed window.
        for (uint64_t t0 = nowMs(); nowMs() - t0 < (op == "read" ? 1500u : 400u);) g_knxTunnel.pump();
        g_knxTunnel.setGroupCallback(nullptr);

        if (quiet)
        {
            std::printf("ga\t%s\t%s\t%d\t%zu\n", op.c_str(), gaTok.c_str(), sent ? 1 : 0, s_gaHits.size());
            for (const std::string& h : s_gaHits) std::printf("gaval\t%s\n", h.c_str());
        }
        else if (!sent)
            g_tpl.status(ftc::Tpl::Stat::Err, L.tr("could not be sent", "konnte nicht gesendet werden"), {});
        else if (op == "write")
            g_tpl.status(ftc::Tpl::Stat::Ok, gaTok + " " + L.tr("written", "geschrieben"),
                         {L.tr("a group write is not acknowledged", "ein Gruppenschreiben wird nicht quittiert")});
        else if (s_gaHits.empty())
            g_tpl.status(ftc::Tpl::Stat::Warn, gaTok + " " + L.tr("no answer in 1.5 s", "keine Antwort in 1,5 s"),
                         {L.tr("no device on this address has the read flag set",
                               "kein Gerät auf dieser Adresse hat das Lese-Flag")});
        else
        {
            g_tpl.status(ftc::Tpl::Stat::Ok, gaTok + " " + std::to_string(s_gaHits.size()) + " " +
                                                 L.tr("answers", "Antworten"), {});
            for (const std::string& h : s_gaHits) std::printf("    %s\n", h.c_str());
        }
        g_knxTunnel.disconnect();
        socketCleanup();
        return sent ? 0 : 1;
    }

    // `oknx <pa> unload yes` - the ETS "Entladen". 03_05_01 4.3.4 / the load state machine: writing
    // LE_UNLOAD (4) to PID_LOAD_STATE_CONTROL (5) takes a loadable object back to LS_UNLOADED (0). Done
    // on the address table, the association table and the application program, which is what makes a
    // device stop doing its job. Not reversible from here: only an ETS download brings it back.
    if (!consoleMode && !knxotaActive && reachHasPa && pos.size() >= 2 && pos[1] == "unload")
    {
        ftc::I18n& L = g_i18n;
        const uint16_t tgt = (uint16_t)((rp_a << 12) | (rp_l << 8) | rp_d);
        const bool said = pos.size() >= 3 && pos[2] == "yes";
        if (!said)
        {
            g_ui.errorBlock(false, L.tr("unload erases this device's configuration",
                                        "entladen löscht die Konfiguration dieses Geräts"),
                            {L.tr("address table, association table and application program go to UNLOADED",
                                  "Adresstabelle, Assoziationstabelle und Applikationsprogramm gehen auf UNLOADED"),
                             L.tr("only an ETS download brings it back - oknx cannot",
                                  "nur ein ETS-Download holt das zurück - oknx kann es nicht")},
                            std::string("oknx -i <ip> ") + pos[0] + " unload yes");
            g_knxTunnel.disconnect();
            socketCleanup();
            return 2;
        }
        // The `yes` is the script's consent. On a terminal a human also gets asked, with the device named,
        // because a mistyped address here destroys a device that was working.
        if (g_term.isTty() && !quiet)
        {
            g_tpl.status(ftc::Tpl::Stat::Warn, pos[0] + " " + L.tr("will be unloaded", "wird entladen"),
                         {L.tr("configuration erased, only an ETS download brings it back",
                               "Konfiguration gelöscht, nur ein ETS-Download holt sie zurück")});
            if (!ftc::confirm(g_term, g_theme, L, L.tr("really unload this device?", "dieses Gerät wirklich entladen?")))
            {
                g_knxTunnel.disconnect();
                socketCleanup();
                return 2;
            }
        }
        static constexpr uint8_t PID_LOAD_STATE_CONTROL = 5;
        static constexpr uint8_t LE_UNLOAD = 4;
        struct Obj { uint8_t idx; const char* en; const char* de; };
        static const Obj OBJS[] = {{1, "address table", "Adresstabelle"},
                                   {2, "association table", "Assoziationstabelle"},
                                   {3, "application program", "Applikationsprogramm"}};
        SecurityControl sec{false, None};
        int done = 0;
        for (const Obj& o : OBJS)
        {
            uint8_t v = LE_UNLOAD;
            if (!g_knxTunnel.sendPropertyValueWrite(tgt, o.idx, PID_LOAD_STATE_CONTROL, 1, 1, &v, 1)) continue;
            for (uint64_t t0 = nowMs(); nowMs() - t0 < 500;) g_knxTunnel.pump();
            done++;
        }
        (void)sec;
        // Read the state back: LS_UNLOADED (0) is the proof, not the fact that a write left the socket.
        static int s_ls[3];
        static size_t s_lsIx;
        static uint8_t s_lsObj;
        static uint16_t s_lsPa;
        s_lsPa = tgt;
        s_lsIx = 0;
        for (size_t i = 0; i < 3; i++) s_ls[i] = -1;
        for (size_t i = 0; i < 3; i++)
        {
            s_lsIx = i;
            s_lsObj = OBJS[i].idx;
            // Filter on the object index: a previous table's late answer carries the same PID and would
            // otherwise be stored as THIS table's state, reporting the two tables swapped.
            g_knxTunnel.setPropertyCallback([](uint16_t sa, uint8_t oi, uint8_t pid, const uint8_t* d, uint8_t n) {
                if (sa == s_lsPa && oi == s_lsObj && pid == PID_LOAD_STATE_CONTROL && n >= 1) s_ls[s_lsIx] = d[0];
            });
            g_knxTunnel.sendPropertyValueRead(tgt, OBJS[i].idx, PID_LOAD_STATE_CONTROL, 1, 1);
            for (uint64_t t0 = nowMs(); nowMs() - t0 < 1200 && s_ls[i] < 0;) g_knxTunnel.pump();
        }
        g_knxTunnel.setPropertyCallback(nullptr);
        int unloaded = 0;
        for (size_t i = 0; i < 3; i++)
            if (s_ls[i] == 0) unloaded++;
        if (quiet)
            std::printf("unload\t%s\t%d\t%d\t%d\n", pos[0].c_str(), s_ls[0], s_ls[1], s_ls[2]);
        else
        {
            for (size_t i = 0; i < 3; i++)
                std::printf("    %-24s %s\n", L.tr(OBJS[i].en, OBJS[i].de),
                            s_ls[i] == 0 ? L.tr("unloaded", "entladen")
                                         : s_ls[i] < 0 ? L.tr("no answer", "keine Antwort")
                                                       : L.tr("still loaded", "noch geladen"));
            g_tpl.status(unloaded == 3 ? ftc::Tpl::Stat::Ok : ftc::Tpl::Stat::Err,
                         pos[0] + " " + (unloaded == 3 ? L.tr("unloaded", "entladen")
                                                       : L.tr("not fully unloaded", "nicht vollständig entladen")),
                         {});
        }
        (void)done;
        g_knxTunnel.disconnect();
        socketCleanup();
        return unloaded == 3 ? 0 : 1;
    }

    // A_Restart is a KNX service, not an FTC command: a foreign device has no FTC server to ask for
    // permission, so this bypasses everything below. Byte layout from knx application_layer.cpp (case
    // Restart). A basic restart is answered by nothing - the device just reboots.
    if (!consoleMode && !knxotaActive && reachHasPa && pos.size() >= 2 &&
        (pos[1] == "restart" || pos[1] == "masterreset"))
    {
        ftc::I18n& L = g_i18n;
        const uint16_t tgt = (uint16_t)((rp_a << 12) | (rp_l << 8) | rp_d);
        const bool master = (pos[1] == "masterreset");
        uint8_t erase = 0;
        if (master)
        {
            // Names, not numbers, because a mistyped digit here is not recoverable over the bus.
            // Values from knx_types.h EraseCode.
            static const struct { const char* name; uint8_t code; } ERASE[] = {
                {"confirmedrestart", 1}, {"factoryreset", 2}, {"resetia", 3}, {"resetap", 4},
                {"resetparam", 5}, {"resetlinks", 6}, {"factoryresetwithoutia", 7}};
            std::string want = pos.size() >= 3 ? pos[2] : "";
            for (char& c : want) c = (char)std::tolower((unsigned char)c);
            for (const auto& e : ERASE)
                if (want == e.name) erase = e.code;
            const bool confirmed = pos.size() >= 4 && pos[3] == "yes";
            if (erase == 0 || !confirmed)
            {
                g_ui.errorBlock(false,
                                L.tr("master reset needs an erase code and a confirmation",
                                     "Master-Reset braucht einen Erase-Code und eine Bestätigung"),
                                {L.tr("nothing was sent", "es wurde nichts gesendet"),
                                 "confirmedrestart · factoryreset · resetia · resetap · resetparam · resetlinks · factoryresetwithoutia"},
                                "oknx <pa> masterreset factoryreset yes");
                g_knxTunnel.disconnect();
                socketCleanup();
                return 2;
            }
        }
        // A master reset is answered, a basic restart is not: wait for the answer only where one is due,
        // and report the device's error code rather than calling an unanswered send a success.
        static int s_mrErr = -1;
        static uint16_t s_mrTime = 0;
        s_mrErr = -1;
        if (master)
            g_knxTunnel.setRestartCallback([](uint16_t, uint8_t err, uint16_t secs) {
                s_mrErr = (int)err;
                s_mrTime = secs;
            });
        // 06 Profiles 4.2 p.37 row 11: the CONNECTION-ORIENTED restart is mandatory for every profile,
        // the connectionless one is optional everywhere. The reachability probe closes its CO session
        // before returning, so without this the request goes out connectionless and a device that only
        // implements the mandatory form ignores it - and we would report "sent", exit 0, nothing rebooted.
        bool co = g_knxTunnel.scanConnect(tgt);
        if (co)
        {
            for (uint64_t t0 = nowMs(); !g_knxTunnel.scanConnected() && nowMs() - t0 < 800;) g_knxTunnel.pump();
            co = g_knxTunnel.scanConnected();
        }
        const bool sent = g_knxTunnel.sendRestart(tgt, master, erase, 0);
        const uint32_t window = master ? 3000u : 400u;
        for (uint64_t t0 = nowMs(); nowMs() - t0 < window;)
        {
            g_knxTunnel.pump();
            if (master && s_mrErr >= 0) break;
        }
        g_knxTunnel.setRestartCallback(nullptr);
        if (co) g_knxTunnel.scanDisconnect(); // the device reboots anyway; leaving the session open wedges the next call
        if (master && sent)
        {
            char det[96];
            if (s_mrErr < 0)
                std::snprintf(det, sizeof(det), "%s",
                              L.tr("no answer in 3 s - it may still have acted, check the device",
                                   "keine Antwort in 3 s - kann trotzdem gewirkt haben, am Gerät prüfen"));
            else
                std::snprintf(det, sizeof(det), L.tr("device says error %d, %u s", "Gerät meldet Fehler %d, %u s"),
                              s_mrErr, (unsigned)s_mrTime);
            if (quiet) std::printf("masterreset\t%s\t%d\t%d\n", pos[0].c_str(), s_mrErr, (int)s_mrTime);
            else if (s_mrErr == 0)
                g_tpl.status(ftc::Tpl::Stat::Ok, pos[0] + " " + L.tr("master reset accepted", "Master-Reset angenommen"), {det});
            else
                g_tpl.status(ftc::Tpl::Stat::Warn, pos[0] + " " + L.tr("master reset sent", "Master-Reset gesendet"), {det});
            g_knxTunnel.disconnect();
            socketCleanup();
            return s_mrErr == 0 ? 0 : 1;
        }
        if (quiet)
            std::printf("%s\t%s\t%d\n", master ? "masterreset" : "restart", pos[0].c_str(), sent ? 1 : 0);
        else if (sent)
            g_tpl.status(ftc::Tpl::Stat::Ok,
                         pos[0] + " " + (master ? L.tr("master reset sent", "Master-Reset gesendet")
                                                : L.tr("restart sent", "Neustart gesendet")),
                         {L.tr("a restart is not acknowledged - check the device",
                               "ein Neustart wird nicht quittiert - am Gerät prüfen")});
        else
            g_tpl.status(ftc::Tpl::Stat::Err, pos[0] + " " + L.tr("could not be sent", "konnte nicht gesendet werden"), {});
        g_knxTunnel.disconnect();
        socketCleanup();
        return sent ? 0 : 1;
    }

    // A write needs the device's permission, and we can ASK before spending a transfer on a refusal. The
    // console has done this since it was written; every other write command ran into the wall instead and
    // reported it as a failure. Mirrored from the server's own gate (secIsWriteCommand) so the two lists
    // cannot drift: reads stay open, writes do not.
    if (!consoleMode && !knxotaActive && reachHasPa && pos.size() >= 2)
    {
        static const char* WRITE_VERBS[] = {"send", "u", "upload", "perf", "pf", "apply", "a", "rm", "mv", "m",
                                            "mkdir", "md", "rmdir", "rd", "format", "fwupdate", "fw"};
        const std::string& verb = pos[1];
        bool isWrite = false;
        for (const char* w : WRITE_VERBS)
            if (verb == w) { isWrite = true; break; }
        if (isWrite)
        {
            ftc::I18n& L = g_i18n;
            const uint16_t tgt = (uint16_t)((rp_a << 12) | (rp_l << 8) | rp_d);
            const ftc::AccessState acc = resolveAccessInteractively(tgt, 60, quiet);
            if (acc.stage != ftc::Access::Open && acc.stage != ftc::Access::Unknown)
            {
                // Quiet is for scripts: one line they can act on, no prompt they cannot answer.
                if (quiet)
                    std::fprintf(stderr, "%s\n", acc.stage == ftc::Access::NeedPassword
                                                     ? "please login first"
                                                     : "target does not accept writes");
                else
                    g_ui.errorBlock(false,
                                    acc.stage == ftc::Access::NeedPassword
                                        ? L.tr("this device wants a password", "dieses Gerät verlangt ein Passwort")
                                        : L.tr("this device is not accepting writes",
                                               "dieses Gerät nimmt keine Schreibzugriffe an"),
                                    {L.tr("nothing was sent", "es wurde nichts gesendet")},
                                    acc.stage == ftc::Access::NeedPassword
                                        ? L.tr("sign in first:  oknx <pa> login <password>",
                                               "erst anmelden:  oknx <pa> login <Passwort>")
                                        : L.tr("the parameter is called \"access protection\" in the OpenKNX application",
                                               "der Parameter heißt \"Zugriffsschutz\" in der OpenKNX-Applikation"));
                g_knxTunnel.disconnect();
                socketCleanup();
                return 3; // 3 = the device refuses, same as the console path
            }
        }
    }

    // Stage 2: render read/status <pa> commands from the client's structured getters (raw text suppressed).
    // Not a structured command (or console mode) -> fall through to the normal one-shot path below.
    if (!consoleMode && ftcRenderStructured(pos, quiet, exitCode))
    {
        g_knxTunnel.disconnect();
        if (exitCode == 130)
            std::fprintf(stderr, "%s -- transfer cancelled, tunnel closed cleanly (no interface lockout).\n", abortReason().c_str());
        socketCleanup();
        return exitCode;
    }

    // Transfer commands (send/get/perf): the CLI owns the host presentation (setup + xx.xx% progress + result
    // Panels), drawn from the client's info-API. Suppress the SM's own console box so it isn't double-printed.
    // Quiet keeps the SM lines (scriptable); the embedded SM box is untouched (that path never sets suppress).
    const bool isXferCmd = !consoleMode && !quiet && pos.size() >= 2 &&
                           (pos[1] == "send" || pos[1] == "upload" || pos[1] == "get" ||
                            pos[1] == "download" || pos[1] == "receive" || pos[1] == "perf");
    // Before anything expensive: is that address occupied at all? A transfer to a silent target spends
    // over two minutes on retries before it says so, and then blames the bus. One frame settles it.
    if (isXferCmd) g_ftcSuppress = true;

    // Verbose console: fetch the TARGET steckbrief NOW, BEFORE the console session is opened below — once the
    // console owns the client (single-owner), a concurrent `info` would collide and never complete (that spun
    // isBusy() and hung the open). Captured into locals, handed to the ConsoleUi after it is created.
    std::string tgtOrder, tgtMask, tgtCls, tgtFw, tgtFeat;
    bool tgtValid = false;
    if (verbose && consoleMode)
    {
        g_ftcSuppress = true;
        openknxFileTransferClient.processCommand(std::string("ftc ") + pos[0] + " info", false);
        const bool anim = !quiet && g_term.isTty();
        const uint64_t t0 = nowMs();
        for (; !openknxFileTransferClient.deviceInfo().valid && (nowMs() - t0) < 2000;)
        {
            g_knxTunnel.pump();
            openknxFileTransferClient.loop(true);
            if (anim) // time-based step so the 2 ms pump loop doesn't spin the packet too fast
            {
                std::printf("%s", busAnimFrame((int)((nowMs() - t0) / 45), g_i18n.tr("reading device…", "lese Gerät…")).c_str());
                std::fflush(stdout);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (anim)
        {
            std::printf("\r\x1b[K");
            std::fflush(stdout);
        }
        g_ftcSuppress = false;
        const FtcDeviceInfo& d = openknxFileTransferClient.deviceInfo();
        if (d.valid)
        {
            char mb[16];
            std::snprintf(mb, sizeof(mb), "0x%04X", d.mask);
            char fw[16] = "";
            if (d.haveVersion) std::snprintf(fw, sizeof(fw), "%u.%u", (d.version >> 6) & 0x1F, d.version & 0x3F);
            // Same rule as the info block: a legacy server reports nothing, so what it CAN is derived.
            const uint32_t hb = d.ftmLegacy ? d.ftmCan : d.features;
            std::string feat;
            if (hb & ftc::FEAT_FAST) feat += "Fast ";
            if (hb & ftc::FEAT_CONSOLE) feat += "Console ";
            if (hb & ftc::FEAT_RESUME) feat += "Resume ";
            if (hb & ftc::FEAT_UPDATE) feat += d.ftmDerived ? "Update? " : "Update "; // ? = derived, see the info block
            if (hb & ftc::FEAT_DELTA) feat += "Delta ";
            if (!feat.empty()) feat.pop_back();
            if (d.ftmLegacy) feat += std::string(feat.empty() ? "" : " ") + g_i18n.tr("(FTM before 0.2.0)", "(FTM vor 0.2.0)");
            tgtOrder = d.haveOrder ? d.order : std::string();
            tgtMask = mb;
            tgtCls = d.cls;
            tgtFw = fw;
            tgtFeat = feat;
            tgtValid = true;
        }
    }

    // The console session is a WRITE on the device (it opens object 160), so it runs into the same access
    // protection as a file transfer. Clear it BEFORE the open: a refused open leaves the client with nothing
    // to report, and the bar would sit on "connecting" forever waiting for an answer that will never come.
    unsigned cpa_a = 0, cpa_l = 0, cpa_d = 0;
    const bool consoleHasPa = !pos.empty() &&
                              std::sscanf(pos[0].c_str(), "%u.%u.%u", &cpa_a, &cpa_l, &cpa_d) == 3 &&
                              cpa_a <= 15 && cpa_l <= 15 && cpa_d <= 255;
    if (consoleMode && consoleHasPa)
    {
        const uint16_t cpa = (uint16_t)((cpa_a << 12) | (cpa_l << 8) | cpa_d);
        const ftc::AccessState ca = resolveAccessInteractively(cpa, 60, quiet);
        if (ca.stage != ftc::Access::Open && ca.stage != ftc::Access::Unknown)
        {
            ftc::I18n& L = g_i18n;
            std::fflush(stdout);
            g_ui.errorBlock(false, L.tr("the console stayed shut", "die Konsole blieb zu"),
                            {ca.stage == ftc::Access::NeedPassword
                                 ? L.tr("this device wants a password", "dieses Gerät verlangt ein Passwort")
                                 : L.tr("this device is not accepting writes",
                                        "dieses Gerät nimmt keine Schreibzugriffe an")},
                            L.tr("the parameter is called \"access protection\" in the OpenKNX application",
                                 "der Parameter heißt \"Zugriffsschutz\" in der OpenKNX-Applikation"));
            socketCleanup();
            return 3; // 3 = the device refuses, same as everywhere else
        }
    }

    // -V also raises the CLIENT's verbosity, not just the UI's: the per-window report diagnostics
    // (answer latency, report timeouts) are gated on level 2 and are the only view into a fast stall.
    if (verbose) openknxFileTransferClient.setVerbosity(2);
    if (quiet) openknxFileTransferClient.setVerbosity(0); // -q reports as facts below; the client's own box would only be noise
    // A log is written to be read later, by someone who no longer has the screen. So it gets everything the
    // client can say, regardless of how quiet the console was told to be -- the console filters, the log does not.
    if (logRequested) openknxFileTransferClient.setVerbosity(2);

    // --log outside the console: the line hook tees every client line, so a transfer transcript needs
    // nothing but an open file. The console path opens its own log further down, once its PA is known.
    if (logRequested && !consoleMode)
    {
        if (openSessionLog(pos.empty() ? std::string("oknx") : pos[0], logPathArg).empty())
            std::fprintf(stderr, "could not open log file%s%s\n",
                         logPathArg.empty() ? "" : " ", logPathArg.c_str());
    }

    // `oknx <pa> login [pw]` is served HERE, not handed to the shared console parser. That parser would
    // call requestLogin a second time, from a second place -- and the two would have to agree on what
    // "signed in" means, on the retry, and on remembering the device's lock-out. One sign-in, one set of
    // rules. Without a password on the line it is asked for: a secret typed into a shell ends up in the
    // history, and this way it never does.
    if (reachHasPa && pos.size() >= 2 && (pos[1] == "login" || pos[1] == "li"))
    {
        ftc::I18n& L = g_i18n;
        const uint16_t lpa = (uint16_t)((rp_a << 12) | (rp_l << 8) | rp_d);
        std::string pw = (pos.size() >= 3) ? pos[2] : std::string();
        const uint32_t locked = authBackoffRemaining(lpa);
        if (locked)
            g_tpl.status(ftc::Tpl::Stat::Err,
                         L.tr("this device is still holding logins off", "dieses Gerät blockt Anmeldungen noch"),
                         {waitText(locked) + L.tr(" to go", " übrig"),
                          L.tr("trying before that neither helps nor hurts",
                               "ein Versuch davor schadet nicht, bringt aber nichts")});
        if (pw.empty())
        {
            if (!g_term.isTty())
            {
                std::fprintf(stderr, "%s\n", L.tr("usage: oknx <pa> login <password>",
                                                  "Aufruf: oknx <pa> login <Passwort>"));
                socketCleanup();
                return 2;
            }
            if (!ftc::readSecret(g_term, g_theme, L.tr("password", "Passwort"), pw)) { socketCleanup(); return 130; }
        }
        const SignInResult si = signIn(lpa, pw.c_str());
        pw.assign(pw.size(), '\0');
        int rc = 3;
        switch (si.outcome)
        {
            case FileTransferClient::FtcAuthOk:
                g_tpl.status(ftc::Tpl::Stat::Ok, L.tr("signed in", "angemeldet"),
                             {L.tr("writes stay allowed until the device goes idle",
                                   "Schreibzugriff gilt, bis das Gerät in Ruhe fällt")});
                rc = 0;
                break;
            case FileTransferClient::FtcAuthNotNeeded:
                g_tpl.status(ftc::Tpl::Stat::Ok, L.tr("this device does not ask for a password",
                                                      "dieses Gerät verlangt kein Passwort"), {});
                rc = 0;
                break;
            case FileTransferClient::FtcAuthBadPw:
                if (si.backoffS)
                    g_tpl.status(ftc::Tpl::Stat::Err, L.tr("too many attempts — the device is holding them off",
                                                           "zu viele Versuche — das Gerät blockt sie ab"),
                                 {L.tr("next attempt in ", "nächster Versuch in ") + waitText(si.backoffS),
                                  L.tr("this is the device's brute-force protection, not a fault",
                                       "das ist der Brute-Force-Schutz des Geräts, kein Fehler")});
                else
                    g_tpl.status(ftc::Tpl::Stat::Err, L.tr("that password was not accepted",
                                                           "dieses Passwort wurde nicht angenommen"), {});
                break;
            default:
                g_tpl.status(ftc::Tpl::Stat::Warn, L.tr("the device did not answer the login",
                                                        "das Gerät hat auf die Anmeldung nicht geantwortet"),
                             {L.tr("that says nothing about the password — it was tried twice",
                                   "das sagt nichts über das Passwort — es wurde zweimal versucht")});
                rc = 6;
                break;
        }
        socketCleanup();
        return rc;
    }

    // Submit the command through the same Module entry the console uses.
    openknxFileTransferClient.processCommand(cmd, false);

    if (consoleMode)
    {
        // Interactive remote console. processCommand opened the session (probe -> obj 160). Typed lines
        // go to the client's console line path; the shim's log/line-sink streams remote output to us.
        char cpa[16];
        std::snprintf(cpa, sizeof(cpa), "%u.%u.%u", (g_knxTunnel.assignedPA() >> 12) & 0x0F,
                      (g_knxTunnel.assignedPA() >> 8) & 0x0F, g_knxTunnel.assignedPA() & 0xFF);
        // Open the session log first (if requested) so its path can be shown in the bar.
        if (logRequested)
        {
            if (openSessionLog(pos[0], logPathArg).empty())
                std::fprintf(stderr, "[console] could not open log file%s%s\n",
                             logPathArg.empty() ? "" : " ", logPathArg.c_str());
        }
        const uint64_t conStart = nowMs();
        // How long the target gets to answer the console open before we call it a failure. Generous: a busy
        // bus plus a device mid-transfer can take a while, but not a minute.
        constexpr uint64_t CON_OPEN_TMO_MS = 60000;
        bool conOpened = false; // the session was open at least once -> a later drop is a drop, not a no-show
        bool conQuit = false;   // the user ended it themselves -> not a failure, whatever the phase said

        // A cooperative pump burst: flush the tunnel + client a few passes (drains queued close banners).
        auto pump = [&](int passes, int ms) {
            for (int i = 0; i < passes; ++i)
            {
                g_knxTunnel.pump();
                openknxFileTransferClient.loop(true);
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            }
        };
        auto terminalPhase = [&]() {
            FtcPhase ph = openknxFileTransferClient.status().phase;
            return ph == FtcPhase::Done || ph == FtcPhase::Failed;
        };

        if (consoleBarOn)
        {
            // Rich TUI: raw mode, pinned BOTTOM status box, blinking cursor, ↑↓ command history. Remote
            // output is routed into the scroll region via g_consoleUi (set below) so it never overprints
            // the box/input and native scrollback keeps working (region top margin stays at row 1).
            ftc::ConsoleUi ui(g_term, g_theme, g_i18n);
            ui.setVerbose(verbose);                                                // -V -> extra l3/l4 status lines + wider RX graph (set BEFORE begin: region depends on it)
            ui.setLimits((uint16_t)drainCap, ifaceApdu);                           // verbose l3 figures: drain cap (B/answer) + detected interface APDU
            ui.setIface(std::string(), ifName, ifMask);                            // verbose l4: the interface we tunnel THROUGH
            if (tgtValid) ui.setTarget(tgtOrder, tgtMask, tgtCls, tgtFw, tgtFeat); // fetched BEFORE the console open
            ui.setPrio(prioTag); // show an elevated FTC priority in the console header (empty for low)
            ui.begin(ip, cpa, pos[0], g_logPath, consoleHistoryPath());
            g_consoleUi = &ui;
            g_watch.load(watchPath(pos[0]));   // per-PA auto-commands (/every), cached across sessions
            ui.setJobs(g_watch.activeCount()); // ⟳ badge in the status bar
            uint64_t lastBar = 0;
            bool running = true;
            while (running)
            {
                if (g_abort)
                {
                    feedConsoleLine("quit");
                    pump(64, 2);
                    exitCode = 130;
                    break;
                }
                g_knxTunnel.pump();
                openknxFileTransferClient.loop(true);

                std::string line;
                ftc::ConsoleUi::PollResult pr = ui.poll(line);
                if (pr == ftc::ConsoleUi::PollAbort)
                {
                    feedConsoleLine("quit");
                    pump(64, 2);
                    exitCode = 130;
                    running = false;
                }
                else if (pr == ftc::ConsoleUi::PollEof)
                {
                    feedConsoleLine("quit");
                    pump(64, 2);
                    running = false;
                }
                else if (pr == ftc::ConsoleUi::PollHelp)
                    ui.showHelp(); // bare '?' -> local shortcut help (not sent to the device)
                else if (pr == ftc::ConsoleUi::PollToggleLog)
                {
                    // Ctrl-S: start/stop the session log at runtime (independent of the --log start flag).
                    if (g_logFp) // currently logging -> stop, keep the file
                    {
                        std::fclose(g_logFp);
                        g_logFp = nullptr;
                        const std::string p = g_logPath;
                        g_logPath.clear();
                        ui.setLogPath("");
                        emitLine(g_theme.amber(g_term.glyph("● ", "* ")) + g_theme.dim(g_i18n.tr("log stopped — saved: ", "Log gestoppt — gespeichert: ")) + g_theme.txt(p));
                    }
                    else // not logging -> open a fresh session log (home dir, con_<pa>_<ts>.log)
                    {
                        if (openSessionLog(pos[0], std::string()).empty())
                            emitLine(g_theme.red(g_i18n.tr("could not open log file", "Log-Datei nicht öffenbar")));
                        else
                        {
                            ui.setLogPath(g_logPath);
                            emitLine(g_theme.green(g_term.glyph("● ", "* ")) + g_theme.dim(g_i18n.tr("log started: ", "Log gestartet: ")) + g_theme.txt(g_logPath));
                        }
                    }
                }
                else if (pr == ftc::ConsoleUi::PollSubmit)
                {
                    if (lineHasAuthKeyword(line))
                    {
                        emitLine(g_theme.amber("[console] 'login'/'logout' is not relayed here (password would go in clear).").c_str());
                        emitLine(g_theme.dim("          Leave and run it as a one-shot:  oknx <pa> login <pw>").c_str());
                    }
                    else if (g_watch.isMeta(line)) // '/'-command: handled LOCALLY, never sent to the device
                    {
                        emitLine(g_theme.green("> ") + g_theme.txt(line));
                        if (line == "/help" || line == "/?" || line == "/h") // the ONE complete local help
                        {
                            ui.showHelp();
                        }
                        else if (line == "/stat" || line.rfind("/stat", 0) == 0) // full session stats block
                        {
                            for (const auto& sl : ui.statLines(knxTunnelRx(), knxTunnelActivity(), knxTunnelDrops(),
                                                               g_conTrunc, (nowMs() - conStart) / 1000, true))
                                emitLine(sl);
                        }
                        else // /job … (auto-commands)
                        {
                            for (const auto& fl : g_watch.handle(line))
                                emitLine(fl);
                            ui.setJobs(g_watch.activeCount());
                        }
                    }
                    else
                    {
                        // Local echo of the submitted line (incl. an EMPTY Enter) so the user sees it, then
                        // forward it — an empty line is relayed too (it nudges the device to a fresh prompt).
                        emitLine(g_theme.green("> ") + g_theme.txt(line));
                        ui.noteCommand(line);  // start the round-trip timer for the verbose l3 line
                        feedConsoleLine(line); // client catches quit/exit locally and closes the session
                        if (line == "quit" || line == "exit") { conQuit = true; running = false; }
                    }
                }

                const uint64_t now = nowMs();
                if (now - lastBar > 250)
                {
                    lastBar = now;
                    // Connected ⟺ the TARGET answered (consoleConnected). Tunnel down or not-yet-answered =
                    // connecting; a Failed open = no-answer. Recomputed LIVE so a drop/reconnect never stays green.
                    // "Connecting" is only honest while there is still a chance. Past the deadline the
                    // target has had every opportunity to answer, and a bar that keeps promising a
                    // connection nobody is still attempting is worse than a plain refusal.
                    const bool everOpened = openknxFileTransferClient.consoleConnected();
                    if (everOpened) conOpened = true;
                    const bool overdue = !conOpened && (now - conStart) > CON_OPEN_TMO_MS;
                    // Connected only once the TARGET answered. Anything that ends the attempt without an
                    // answer — a refusal, the client giving up, or the deadline — is red, never "connecting".
                    ui.setLink(conOpened ? ftc::ConsoleUi::Link::Connected
                                         : (terminalPhase() || overdue) ? ftc::ConsoleUi::Link::NoAnswer
                                                                        : ftc::ConsoleUi::Link::Connecting);
                    // Overdue, or the client already gave up: either way the session never opened.
                    if (overdue || (!conOpened && openknxFileTransferClient.status().phase == FtcPhase::Failed))
                        running = false; // the exit code for "never opened" is set after the loop
                    ui.tick(knxTunnelRx(), knxTunnelActivity(), knxTunnelDrops(), g_conTrunc, (now - conStart) / 1000,
                            g_knxTunnel.channelId(), g_knxTunnel.txSeq(), g_knxTunnel.rxSeq());
                }

                // Auto-commands (/every): fire the next due job ONLY when the console is ready (1-outstanding,
                // never overlaps -> bus-friendly). The echo shows what auto-fired; noteCommand keeps RTT honest.
                {
                    const std::string autoCmd = g_watch.due(now, openknxFileTransferClient.consoleIdle());
                    if (!autoCmd.empty() && !lineHasAuthKeyword(autoCmd))
                    {
                        emitLine(g_theme.green(g_term.glyph("⟳ ", "~ ")) + g_theme.dim(autoCmd));
                        ui.noteCommand(autoCmd);
                        feedConsoleLine(autoCmd);
                    }
                }

                if (terminalPhase())
                {
                    const bool failed = openknxFileTransferClient.status().phase == FtcPhase::Failed;
                    // The bar recomputes every 250 ms and the attempt can end between frames; repaint before
                    // it closes so it doesn't stay on "connecting" (the phase isn't the success test).
                    if (!conOpened)
                    {
                        ui.setLink(ftc::ConsoleUi::Link::NoAnswer);
                        ui.tick(knxTunnelRx(), knxTunnelActivity(), knxTunnelDrops(), g_conTrunc,
                                (nowMs() - conStart) / 1000, g_knxTunnel.channelId(), g_knxTunnel.txSeq(),
                                g_knxTunnel.rxSeq());
                    }
                    exitCode = failed ? 1 : 0;
                    running = false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            pump(24, 2);           // let any final close banner emit into the region before we tear down
            g_consoleUi = nullptr; // stop routing output to the TUI
            // Session stats on console EXIT — ONLY in verbose. (`/stat` stays available any time, any mode.)
            const uint32_t frx = knxTunnelRx(), ftx = knxTunnelActivity(), fdr = knxTunnelDrops();
            const uint64_t fup = (nowMs() - conStart) / 1000;
            if (verbose && g_logFp) // append the plain stats to the log so the transcript ends with the full picture
            {
                std::fputc('\n', g_logFp);
                for (const auto& sl : ui.statLines(frx, ftx, fdr, g_conTrunc, fup, false))
                {
                    std::fputs(sl.c_str(), g_logFp);
                    std::fputc('\n', g_logFp);
                }
            }
            if (g_logFp)
            {
                std::fclose(g_logFp);
                g_logFp = nullptr;
            }
            ui.end(frx, ftx, fdr, g_conTrunc, fup);
            if (verbose) // print the coloured stats block to the screen, below the freed region
                for (const auto& sl : ui.statLines(frx, ftx, fdr, g_conTrunc, fup, true))
                    std::printf("%s\n", sl.c_str());
        }
        else
        {
            // Plain path (piped / -q / non-TTY): line reader, no bar, no raw mode — scriptable.
            StdinLines stdinLines;
            bool bannerShown = false; // print "connected" ONLY once the target actually answers (not just the tunnel)
            std::fflush(stdout);
            bool running = true;
            while (running)
            {
                if (g_abort)
                {
                    feedConsoleLine("quit");
                    pump(64, 2);
                    exitCode = 130;
                    break;
                }
                g_knxTunnel.pump();
                openknxFileTransferClient.loop(true);
                if (!bannerShown && !quiet && openknxFileTransferClient.consoleConnected())
                {
                    std::printf("[console] %s%s%s\n",
                                g_i18n.tr("connected — type commands; 'quit' or 'exit' to leave.",
                                          "verbunden — Befehle eingeben; 'quit' oder 'exit' zum Beenden."),
                                g_logPath.empty() ? "" : "  log: ", g_logPath.c_str());
                    std::fflush(stdout);
                    bannerShown = true;
                }

                std::string line;
                if (stdinLines.poll(line))
                {
                    if (lineHasAuthKeyword(line))
                        std::printf("[console] 'login'/'logout' is not relayed here (it would send the password in\n"
                                    "          clear). Leave the console and run it as a one-shot:  oknx <pa> login <pw>\n");
                    else
                    {
                        feedConsoleLine(line);
                        if (line == "quit" || line == "exit") { conQuit = true; running = false; }
                    }
                }
                else if (stdinLines.eof())
                {
                    feedConsoleLine("quit");
                    pump(64, 2);
                    conQuit = conOpened; // no more input is a quit only if there was a session to quit
                    running = false;
                }

                if (openknxFileTransferClient.consoleConnected()) conOpened = true; // plain mode has no bar
                if (terminalPhase())
                {
                    exitCode = (openknxFileTransferClient.status().phase == FtcPhase::Failed) ? 1 : 0;
                    pump(64, 1);
                    running = false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (g_logFp)
            {
                std::fclose(g_logFp);
                g_logFp = nullptr;
            }
            if (!quiet && !g_logPath.empty()) std::printf("[console] session log saved: %s\n", g_logPath.c_str());
        }
        // The phase the client ends in is not a reliable verdict — it can finish "done" having never opened
        // anything. What matters is the fact: the session was never up, and the user did not end it
        // themselves. Both console modes land here, so the report is written once.
        if (!conOpened && !conQuit)
        {
            ftc::I18n& L = g_i18n;
            std::fflush(stdout);
            // The client's status says WHY: a legacy server without the tunnel (shown as "from FTM 0.2.0"), a
            // current one built without the feature, one older than 0.0.4, or a lost CheckFeatures after a
            // good ModuleVersion. Only true silence earns "check the address".
            // Exit codes here: 6 = the target never answered (as everywhere else), 5 = it answered and the
            // console is not available on it.
            // Three classes: `local` = our own send failed (nothing reached the bus), `silent` = the target
            // never answered anything, everything else = it answered and the reason is named (the known ones
            // translated, the rest verbatim). Exit 6 covers local + silent, 5 the answered ones.
            const std::string why = openknxFileTransferClient.status().message;
            const bool local = why.find("cannot probe") != std::string::npos ||      // our own send failed:
                               why.find("could not send") != std::string::npos;      // nothing reached the bus
            const bool silent = why.empty() || why.find("no module version answer") != std::string::npos;
            const bool legacyCon = why.find("no console tunnel") != std::string::npos;
            const bool noFeature = why.find("no console feature") != std::string::npos;
            const bool tooOld = why.find("not supported on this legacy") != std::string::npos;
            const bool lostFeat = why.find("no CheckFeatures answer") != std::string::npos ||
                                  why.find("no version answer after") != std::string::npos; // CheckFeatures answered, ModuleVersion lost
            const bool unexpected = why.find("unexpected answer") != std::string::npos;
            const bool openLost = why.find("console open unanswered") != std::string::npos;
            const bool needLogin = why.find("login required") != std::string::npos;
            const bool locked = why.find("console locked") != std::string::npos;
            const bool inUse = why.find("console in use") != std::string::npos;
            const bool answered = !silent && !local; // it is there; the console is not available right now / at all
            const std::string reason =
                legacyCon ? L.tr("this file-transfer server has no console tunnel - available from FTM 0.2.0",
                                 "dieser Dateitransfer-Server hat keinen Konsolentunnel - gibt es erst ab FTM 0.2.0")
                : noFeature ? L.tr("the device answers, but its firmware was built without the console",
                                   "das Gerät antwortet, seine Firmware wurde aber ohne Konsole gebaut")
                : tooOld ? L.tr("this file-transfer server is older than 0.0.4 - not supported",
                                "dieser Dateitransfer-Server ist älter als 0.0.4 - nicht unterstützt")
                : lostFeat ? L.tr("the device answers, but one probe answer was lost - try again",
                                  "das Gerät antwortet, eine Antwort ging aber verloren - noch einmal versuchen")
                : unexpected ? L.tr("the device answered the console open with something unexpected",
                                    "das Gerät hat auf das Öffnen der Konsole unerwartet geantwortet")
                : openLost ? L.tr("the device answers, but not the console open - try again",
                                  "das Gerät antwortet, aber nicht auf das Öffnen der Konsole - noch einmal versuchen")
                : needLogin ? L.tr("the console is password protected - log in first",
                                   "die Konsole ist passwortgeschützt - erst anmelden")
                : locked ? L.tr("the console is locked - programming mode, or ETS access is off",
                                "die Konsole ist gesperrt - Programmiermodus, oder der ETS-Zugriff ist aus")
                : inUse ? L.tr("the console is in use by another client - try again later",
                               "die Konsole wird von einem anderen Client benutzt - später noch einmal")
                : local ? L.tr("the probe could not be sent - no tunnel to the interface, or the send queue is full",
                               "die Anfrage ging nicht raus - kein Tunnel zur Schnittstelle, oder der Sendepuffer ist voll")
                : answered ? why
                : L.tr("the target never answered", "das Ziel hat nicht geantwortet");
            const std::string hint =
                legacyCon || noFeature ? L.tr("update the device firmware to use the console",
                                              "für die Konsole die Gerätefirmware aktualisieren")
                : needLogin ? L.tr("run: oknx --ip ", "ausführen: oknx --ip ") + ip + " " + pos[0] + " login <pw>"
                : answered || local ? std::string()
                : L.tr("check the address, or whether the device is on the bus",
                       "Adresse prüfen, oder ob das Gerät am Bus ist");
            if (quiet)
                std::fprintf(stderr, "%s: %s\n", L.tr("console", "Konsole"), reason.c_str());
            else
                g_ui.errorBlock(false, L.tr("the console did not open", "die Konsole ist nicht zustande gekommen"),
                                {reason}, hint);
            exitCode = answered ? 5 : 6;
        }
    }
    else if (isXferCmd)
    {
        // Transfer (send/get/perf) with CLI-owned presentation: setup Panel + in-place xx.xx% progress + result.
        exitCode = runTransferPresenter();
        g_ftcSuppress = false;
    }
    else
    {
        // One-shot (info/df/scan/… or quiet transfer): cooperative loop to completion; SM text passes through.
        exitCode = runOneShotToQuiescence();
        // `fwupdate` answers nothing on the wire, so the command used to end without a word about whether
        // anything was installed. Ask -- the same way knxOTA does, patch or whole image alike.
        if (exitCode == 0 && reachHasPa && pos.size() >= 2 && (pos[1] == "fwupdate" || pos[1] == "fw") && !quiet)
        {
            const uint16_t tgt = (uint16_t)((rp_a << 12) | (rp_l << 8) | rp_d);
            if (!watchFirmwareInstall(tgt)) exitCode = 4;
        }
    }

    // --- clean shutdown ---------------------------------------------------------------------------
    logXferReport(openknxFileTransferClient.transferResult());
    if (g_logFp) // non-console log (the console path closes its own before it reaches here)
    {
        std::fclose(g_logFp);
        g_logFp = nullptr;
        if (!quiet && !g_logPath.empty()) std::printf("log saved: %s\n", g_logPath.c_str());
    }
    g_knxTunnel.disconnect();
    if (exitCode == 130)
        std::fprintf(stderr, "%s -- transfer cancelled, tunnel closed cleanly (no interface lockout).\n", abortReason().c_str());
    socketCleanup();
    return exitCode;
}

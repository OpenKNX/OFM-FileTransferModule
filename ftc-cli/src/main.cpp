/**
 * @file        main.cpp
 * @brief       The ftc-cli entry point — CLI parsing, command dispatch and the cooperative run loops.
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
#include <string>
#include <system_error>
#include <thread>
#include <mutex>
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
#include "knx_ip_tunnel.h"

// ftc CLI presentation layer (host-only, header-only, NO lib/knx dependency): terminal caps, i18n,
// the phosphor theme registry, and the reusable Ui building blocks. main() stays thin — Ui draws.
#include "cli/Config.h"
#include "cli/ConsoleUi.h"
#include "cli/I18n.h"
#include "cli/Monitor.h"
#include "cli/ProgScan.h"
#include "cli/Templates.h"
#include "cli/Term.h"
#include "cli/Theme.h"
#include "cli/Ui.h"
#include "cli/Watch.h"
#include "cli/WebConsole.h"
#include "core/Describe.h"
#include "core/DeviceMgmt.h"
#include "core/Discovery.h"
#include "core/Gzip.h"
static ftc::Term g_term;
static ftc::I18n g_i18n;
static ftc::Theme g_theme(g_term);
static ftc::Ui g_ui(g_term, g_theme, g_i18n);
static ftc::Tpl g_tpl(g_term, g_theme, &g_i18n);
static ftc::Watch g_watch(g_theme, g_i18n); // auto/recurring console commands (/every), cached per target PA
static ftc::Config g_cfg;

// ── ftc-cli throughput-graph width, in % of the terminal width (100 = full). ──
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
// Parallel scan (--tunnels N): the parent self-execs N child scans over range chunks, each on its own tunnel.
static std::string g_selfPath;   // argv[0] -> re-invoked for the child scans
static int g_tunnels = 1;        // 1 = serial (default); N = N parallel tunnels; 0 = auto (as many as the interface allows)
static bool g_pchild = false;    // internal: this process is a parallel-scan child (emit the P/D line protocol)
static bool g_probeSlot = false; // internal: this process is a tunnel-slot probe child (connect, emit SLOT, hold, exit)

// Probe which tunnel additional-addresses are currently free: spawn one short-lived tunnel per slot (children
// hold the slot briefly so the interface hands each a distinct free PA); the PAs that come back are free.
static std::set<uint16_t> probeTunnelSlots(const std::string& ip, uint16_t port, int slots);

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
static void socketCleanup()
{
#ifdef _WIN32
    WSACleanup();
#endif
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
// Discovery is delegated to the reusable CORE (core/Discovery.h; also used by `ftc mc`).

/**
 * @brief Windows-only hint printed when discovery comes back empty.
 * @details Almost always the Firewall silently dropping the (unicast, from a different address than we
 *          multicast to) SEARCH_RESPONSE. We already send from every NIC, so this covers the firewall cause.
 */
static void discoverEmptyHint(ftc::Theme& c, ftc::I18n& L)
{
#ifdef _WIN32
    char path[512];
    if (!GetModuleFileNameA(nullptr, path, sizeof(path))) std::strcpy(path, "ftc.exe");
    std::printf("  %s\n", c.amber(L.tr("Windows Firewall likely blocked the response.",
                                       "Die Windows-Firewall hat die Antwort vermutlich blockiert."))
                              .c_str());
    std::printf("  %s\n", c.dim(L.tr("Allow ftc through the Firewall, or run once as Administrator:",
                                     "ftc in der Firewall freigeben, oder einmal als Administrator:"))
                              .c_str());
    std::printf("  %s\n", c.cyan(std::string("netsh advfirewall firewall add rule name=\"ftc KNX\" dir=in action=allow protocol=UDP program=\"") + path + "\"").c_str());
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
    const auto ifaces = ftc::discoverInterfaces(port, 3000); // shared CORE discovery (also used by ftc mc)
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
    t.kv("APDU reported", c.bold("55 B") + c.dim("  · via device-mgmt · no bus traffic"));
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

#define FTC_CLI_VERSION "1.0.0"

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
    if (g_ftcSuppress) return true; // a structured renderer owns this command's output -> swallow the raw line
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
        if (w == "loaded" || w == "ok") col = c.green(w);
        else if (w == "off" || w == "n/a" || w == "none" || w == "unloaded")
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
 * @brief Print the ftc-cli + FileTransferModule version line.
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
static const char* ipMethodName(uint8_t m)
{
    // Current IP assignment method (03_08_03): bitset, but a device reports the one in use.
    if (m & 0x04) return "DHCP";
    if (m & 0x02) return "BootP";
    if (m & 0x08) return "AutoIP";
    if (m & 0x01) return "manual";
    return "?";
}

// IfaceDesc + parseDibs() + queryInterface() live in core/Describe.h now (ftc::) — the ONE DESCRIPTION
// parser, shared with `ftc mc`. The apdu* fields of ftc::IfaceDesc are filled by the host below.

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
    ftc::Panel p(t, L.tr("KNX Interface", "KNX-Interface"), ip); // new Panel template (parity with `ftc info`)
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
         (o.status & 0x01) ? t.chip("PROG", 'o') : c.mut(std::string(T.glyph("○", "o")) + " off"));
    if (o.hasIp)
    {
        p.sep();
        std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u / %s", (o.ip >> 24) & 0xFF, (o.ip >> 16) & 0xFF, (o.ip >> 8) & 0xFF, o.ip & 0xFF, ipMethodName(o.ipMethod));
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
        p.kv(L.tr("APDU reported", "APDU gemeldet"), c.bold(buf) + c.dim(L.tr("  · via device-mgmt · no bus traffic", "  · via Device-Mgmt · kein Bus-Traffic")));
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
static void usage()
{
    ftc::Theme& c = g_theme;
    ftc::I18n& L = g_i18n;
    ftc::Ui& U = g_ui;
    banner();

    U.section(L.tr("USAGE", "AUFRUF"));
    std::printf("  ftc [%s] [%s A.B.C.D] [%s N] [%s de|en] %s %s [args...]\n", c.cyan("--verbose").c_str(),
                c.cyan("--ip").c_str(), c.cyan("--port").c_str(), c.cyan("--lang").c_str(),
                c.cyan("<pa>").c_str(), c.cyan("<cmd>").c_str());
    std::printf("  %s = %s  %s\n", c.cyan("<pa>").c_str(),
                c.dim(L.tr("target device on the bus (NOT the interface)", "Ziel-Gerät am Bus (NICHT das Interface)")).c_str(),
                c.dim("e.g. 5.0.3").c_str());
    std::printf("  ftc %s   %s\n", c.cyan("config [key val]").c_str(), c.dim(L.tr("show / set persisted defaults", "persistente Defaults zeigen/setzen")).c_str());
    std::printf("  ftc %s | %s\n\n", c.cyan("--version").c_str(), c.cyan("--help").c_str());

    U.section(L.tr("OPTIONS", "OPTIONEN"));
    U.cmdRow("--ip A.B.C.D | -i", L.tr("interface / router to tunnel through", "Interface/Router, durch das getunnelt wird"));
    U.cmdRow("--port N", L.tr("KNXnet/IP port (default 3671)", "KNXnet/IP-Port (Default 3671)"));
    U.cmdRow("--tunnels N | -T", L.tr("parallel scan over N tunnels (bare = auto/max)", "Parallel-Scan über N Tunnel (ohne Wert = auto/max)"));
    U.cmdRow("--discover | -D", L.tr("list interfaces on the LAN and exit", "Interfaces im LAN auflisten und beenden"));
    U.cmdRow("--verbose | -V", L.tr("full interface + target steckbrief first", "voller Interface-+Ziel-Steckbrief vorweg"));
    U.cmdRow("--quiet | -q", L.tr("no chrome, TSV — scriptable (auto on non-TTY)", "kein Chrome, TSV — skriptbar (auto bei Nicht-TTY)"));
    U.cmdRow("--log [=path]", L.tr("log console session to a file (auto: ~/con_<pa>_<ts>.log)", "Konsolen-Sitzung mitschreiben (auto: ~/con_<pa>_<ts>.log)"));
    U.cmdRow("--lang de|en", L.tr("force language (else FTC_LANG / locale)", "Sprache erzwingen (sonst FTC_LANG/Locale)"));
    U.cmdRow("--theme green|amber|cyan", L.tr("accent theme (persist: ftc config theme <name>)", "Akzent-Theme (dauerhaft: ftc config theme <name>)"));
    U.cmdRow("--ascii", L.tr("ASCII fallback for box/marks", "ASCII-Fallback für Rahmen/Marken"));
    U.cmdRow("-VqD (bundled)", L.tr("short valueless flags bundle: -VD = -V -D", "wertlose Kurzflags bündelbar: -VD = -V -D"));
    std::printf("\n");

    U.section(L.tr("INTERFACE", "INTERFACE"), L.tr("(--ip, no <pa>)", "(--ip, kein <pa>)"));
    U.cmdRow("info", L.tr("full interface report (DESCRIPTION + device-mgmt)", "kompletter Interface-Report (DESCRIPTION + Device-Mgmt)"));
    U.cmdRow("groupmon | gm", L.tr("live group monitor — decoded telegrams", "Live-Gruppenmonitor — dekodierte Telegramme"));
    U.cmdRow("busmon | bm", L.tr("live bus monitor — raw LPDU, ETS ACK colour", "Live-Busmonitor — Roh-LPDU, ETS-ACK-Farbe"));
    U.cmdRow("gm|bm --frames N | --seconds N", L.tr("stop the monitor after N (scripted)", "Monitor nach N stoppen (skriptbar)"));
    U.cmdRow("progscan | ps [global|locate]", L.tr("find devices in programming mode + localise the line",
                                                   "Geräte im Programmiermodus finden + Linie lokalisieren"));
    U.cmdRow("ps --seconds 0", L.tr("continuous watch — devices in prog mode appear/disappear live · Ctrl+C",
                                    "Dauer-Überwachung — Geräte im Prog-Modus erscheinen/verschwinden live · Ctrl+C"));
    U.cmdRow("con | console", L.tr("the interface's OWN console via its webconsole (WebSocket, no tunnel)",
                                   "die EIGENE Konsole des Interfaces über dessen Webconsole (WebSocket, kein Tunnel)"));
    std::printf("\n");

    U.section(L.tr("INFO", "INFO"), L.tr("(read-only)", "(nur lesen)"));
    U.cmdRow("<pa> ping", L.tr("is the target there? round-trip + ms", "ist das Ziel da? Round-Trip + ms"));
    U.cmdRow("<pa> info [ga|<file>]", L.tr("device fingerprint / group comm / file info", "Steckbrief / Gruppenkomm. / Datei-Info"));
    U.cmdRow("<pa> df [sd|efc]", L.tr("target filesystem usage (drive optional)", "Dateisystem-Belegung des Ziels (Drive optional)"));
    U.cmdRow("<pa> ll|ls [sd/|efc/][dir]", L.tr("list a directory (+ CRC, storage bar)", "Verzeichnis listen (+ CRC, Speicher-Balken)"));
    U.cmdRow("scan <a.l | a b> [ets] [deep N]", L.tr("discover devices on a line / range (ets = CO probe; + --tunnels)", "Geräte auf Linie/Bereich finden (ets = CO-Probe; + --tunnels)"));
    std::printf("\n");

    U.section(L.tr("TRANSFER & FILES", "TRANSFER & DATEIEN"));
    U.cmdRow("<pa> send <src> [sd/|efc/]<dst> [mode]", L.tr("upload a host file (alias: upload) — mode: safe·fast · fast w<N> pins the window",
                                                            "Host-Datei hochladen (Alias: upload) — mode: safe·fast · fast w<N> pint das Fenster"));
    U.cmdRow("<pa> get [sd/|efc/]<remote> [local]", L.tr("download a file (alias: download/receive)", "Datei herunterladen (Alias: download/receive)"));
    U.cmdRow("<pa> fwupdate <remote>", L.tr("flash an uploaded firmware -> reboots target", "hochgeladene Firmware flashen -> Ziel-Reboot"));
    U.cmdRow("<pa> perf [kb] [pkg|auto] [mode] [sd|efc]", L.tr("throughput test — mode: safe·fast · +nr/keep/verbose · fast w<N> pins the window · sd|efc = target drive",
                                                             "Durchsatz-Test — mode: safe·fast · +nr/keep/verbose · fast w<N> pint das Fenster · sd|efc = Ziel-Drive"));
    U.cmdRow("<pa> rm | mkdir | rmdir | mv", L.tr("delete / create / remove / rename", "löschen / anlegen / entfernen / umbenennen"));
    U.cmdRow("<pa> format yes", L.tr("erase the WHOLE filesystem (gated)", "GANZES Dateisystem löschen (gesichert)"));
    U.cmdRow("gzip <in> <out>", L.tr("gzip a local file (RP firmware prep; in-process, no tunnel)", "lokale Datei gzip'en (RP-Firmware; in-process, kein Tunnel)"));
    U.cmdRow("decode <hex LPDU>", L.tr("offline-decode a raw TP1 frame (APCI + FTC/console; no tunnel)", "Roh-TP1-Frame offline dekodieren (APCI + FTC/Console; kein Tunnel)"));
    U.cmdRow("[sd/|efc/] on a remote path", L.tr("prefix a REMOTE path (else LittleFS): df ll ls rm mkdir rmdir mv info get perf",
                                                 "REMOTE-Pfad voranstellen (sonst LittleFS): df ll ls rm mkdir rmdir mv info get perf"));
    std::printf("\n");

    U.section(L.tr("TRANSFER OPTIONS", "TRANSFER-OPTIONEN"), L.tr("(send; order-independent)", "(send; reihenfolgeunabhängig)"));
    U.cmdRow("pkg = <n> | auto", L.tr("APDU payload; auto = detected interface max", "APDU-Nutzlast; auto = erkannter Interface-Max"));
    U.cmdRow("mode = safe | fast", L.tr("CRC/chunk · windowed (AIMD) · fast w<N> = fixed window",
                                        "CRC/Chunk · Fenster (AIMD) · fast w<N> = festes Fenster"));
    U.cmdRow("apply | on | yes", L.tr("also flash + reboot after upload", "nach Upload auch flashen + Reboot"));
    U.cmdRow("no-resume | nr | fresh", L.tr("ignore a partial; upload from zero", "Fragment ignorieren; von vorn hochladen"));
    U.cmdRow("verbose | v", L.tr("1 Hz progress line during the transfer", "1-Hz-Fortschrittszeile beim Transfer"));
    std::printf("\n");

    U.section(L.tr("DEVICE", "GERÄT"));
    U.cmdRow("<pa> led [on|off|blink]", L.tr("drive the prog-mode LED (locate)", "Prog-Modus-LED steuern (lokalisieren)"));
    std::printf("\n");

    U.section(L.tr("CONSOLE", "KONSOLE"));
    U.cmdRow("<pa> con [N|max] [apdu M]", L.tr("remote console · N = output drain 4-246 B/answer (max = full · omit = auto from APDU) · apdu M overrides the APDU · ? = help inside",
                                               "Remote-Konsole · N = Ausgabe-Drain 4-246 B/Antwort (max = voll · leer = auto aus APDU) · apdu M überschreibt die APDU · ? = Hilfe drin"));
    U.cmdRow("  · /job add watch every <int> <cmd>", L.tr("in console: recurring auto-command · /job list · /job help",
                                                          "in der Konsole: wiederkehrender Auto-Befehl · /job list · /job help"));
    U.cmdRow("  · /stat  ·  ?", L.tr("in console: full session stats · local shortcut help",
                                     "in der Konsole: komplette Session-Statistik · lokale Shortcut-Hilfe"));
    std::printf("\n");

#ifdef OPENKNX_FTC_SECURITY
    U.section(L.tr("ACCESS", "ZUGRIFF"), L.tr("(password-protected targets)", "(passwortgeschützte Ziele)"));
    U.cmdRow("<pa> login <pw>", L.tr("unlock write actions (password -> MAC locally)", "Schreibaktionen freischalten (Passwort -> MAC lokal)"));
    U.cmdRow("<pa> logout", L.tr("lock the write actions again", "Schreibaktionen wieder sperren"));
    std::printf("\n");
#endif

    U.section(L.tr("EXAMPLES", "BEISPIELE"));
    static const char* const ex[] = {
        "ftc --discover",
        "ftc --ip 11.11.0.126 5.0.3 info",
        "ftc --ip 11.11.0.126 5.0.3 send fw.bin.gz fast",
        "ftc --ip 11.11.0.126 5.0.3 send ../build/firmware.bin.gz apply",
        "ftc --ip 11.11.0.126 5.0.3 get /firmware.bin.gz ./fw.bin.gz",
        "ftc --ip 11.11.0.126 5.0.3 led blink",
        "ftc --lang de --ip 11.11.0.126 5.0.3 info",
    };
    for (const char* e : ex)
        std::printf("  %s\n", c.dim(e).c_str());
    std::printf("\n");
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
    std::fprintf(g_logFp, "# ftc console session log — target %s — %s", pa.c_str(), std::ctime(&t)); // ctime ends in \n
    std::fflush(g_logFp);
    return path;
}

/**
 * @brief Path to the persistent console command history (<config-dir>/history, shared across sessions).
 * @details Ensures the directory exists so the first-ever session can append; empty on failure (history off).
 */
static std::string consoleHistoryPath()
{
    const std::string cfg = g_cfg.path(); // .../ftc/config.toml
    const size_t sl = cfg.find_last_of("/\\");
    if (sl == std::string::npos) return std::string("history");
    const std::string dir = cfg.substr(0, sl);
#ifdef _WIN32
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    std::error_code ec; // no shell -> a config dir with a quote/metachar can't inject
    std::filesystem::create_directories(dir, ec);
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

/**
 * @brief Ctrl+C / SIGTERM handler: request a graceful abort and re-arm the default so a 2nd Ctrl+C hard-kills.
 */
static void onAbortSignal(int)
{
    g_abort = 1;
    std::signal(SIGINT, SIG_DFL);
}

/**
 * @brief Common mid-run abort cleanup: cancel the transfer and let it + the CO teardown reach the wire.
 * @details Returns 130 (128 + SIGINT), the conventional code, so the interface frees the channel at once
 *          instead of blocking until its connection timeout.
 */
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
        p.kv(L.tr("Mode", "Modus"), t.chip(xferModeName(s.mode), mc) + "  " + c.dim(sem));
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
    if (st.window > 0) // fast: the live window (AIMD climb, or a fixed w<N>) as a first-class stat next to peak
        speed += c.dim("    win ") + c.bold(c.amber(std::to_string(st.window)));
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
    return L;
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
static int runTransferPresenter()
{
    const uint64_t QUIET_MS = 1500, ABS_CAP_MS = 1800000;
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
            return abortCleanly("user abort (Ctrl+C)");
        }
        g_knxTunnel.pump();
        openknxFileTransferClient.loop(true);
        const FtcStatus& st = openknxFileTransferClient.status();
        const FtcTransferSetup& setup = openknxFileTransferClient.transferSetup();
        const bool up = setup.kind != FtcXferKind::Download;
        const uint64_t now = nowMs();

        if (!setupDrawn && setup.valid)
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
 * @brief Drive the cooperative loop until the client reaches a terminal phase or goes QUIET; 0 done / 1 failed / 2 timeout.
 * @details Read-chain commands (info/df/scan) never set isBusy() or a terminal phase, so "finished" = no TX frame,
 *          no phase/progress change, not busy for QUIET_MS. Shared by the one-shot path and the --verbose probe.
 */
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

/**
 * @brief `ftc --ip <ip> info` — the full interface report (no PA, no tunnel); -q emits a plain key<TAB>value dump.
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
            std::printf("ipmethod\t%s\n", ipMethodName(d.ipMethod));
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
                                              : c.mut(std::string(g_term.glyph("○", "o")) + " off"));
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
        p.kv(L.tr("IP assignment", "IP-Zuweisung"), c.txt(ipMethodName(d.ipMethod)));
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
             c.bold(buf) + c.dim(L.tr("  · via device management · no bus traffic", "  · via Device-Mgmt · kein Bus-Traffic")));
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
            std::set<uint16_t> freeSet;
            if (g_term.isTty())
                runWithBusAnim(L.tr("probing tunnel slots …", "prüfe Tunnel-Slots …"), [&]() { freeSet = probeTunnelSlots(ip, port, (int)det.tunnelAddrs.size()); }, stderr);
            else
            {
                std::fprintf(stderr, "  %s\r", c.dim(L.tr("probing tunnel slots …", "prüfe Tunnel-Slots …")).c_str());
                std::fflush(stderr);
                freeSet = probeTunnelSlots(ip, port, (int)det.tunnelAddrs.size());
                std::fprintf(stderr, "\x1b[K");
                std::fflush(stderr);
            }
            size_t nFree = 0;
            for (uint16_t a : det.tunnelAddrs)
                if (freeSet.count(a)) ++nFree;
            std::snprintf(buf, sizeof(buf), "%zu", det.tunnelAddrs.size());
            std::string head = c.dim(std::string(buf) + L.tr(" additional individual address(es)", " zusätzliche Individualadresse(n)"));
            char fb[32];
            std::snprintf(fb, sizeof(fb), "%zu/%zu", nFree, det.tunnelAddrs.size());
            head += c.dim("  · ") + c.cyan(fb) + c.dim(L.tr(" free", " frei"));
            p.kv(L.tr("Tunnel addresses", "Tunnel-Adressen"), head);
            for (size_t i = 0; i < det.tunnelAddrs.size(); ++i)
            {
                const bool isFree = freeSet.count(det.tunnelAddrs[i]) != 0;
                std::snprintf(buf, sizeof(buf), "  #%zu", i + 1);
                p.kv(buf, t.chip(pa(det.tunnelAddrs[i]), isFree ? 'c' : 'a') +
                              (isFree ? std::string() : ("   " + c.amber(L.tr("busy", "belegt")))));
            }
            // The colour IS the status — one dim legend line makes it an explicit (indirect) description.
            p.kv("", c.dim(L.tr("cyan = free · amber = busy (in use)", "cyan = frei · orange = belegt (in Benutzung)")));
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
    switch (s)
    {
        case 0: return "unloaded";
        case 1: return "loaded";
        case 2: return "loading";
        case 3: return "error";
        case 4: return "unloading";
        case 5: return "load completing";
        default: return "";
    }
}

/**
 * @brief Drive the cooperative loop to quiescence, snapshotting listing() (released on finish) for ll/ls/scan.
 * @details mergeMode = scan (entries stream in, then isOpenKnx is set in place -> merge by name); else replace-on-grow.
 */
static void ftcPumpStructured(std::vector<FtcEntry>& snap, bool mergeMode, bool progress = false)
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
                if (!d) snap.push_back(e);
                else if (e.isOpenKnx)
                    d->isOpenKnx = true;
            }
        else if (lst.size() >= snap.size())
            snap = lst;
    };
    const uint64_t QUIET_MS = 1500, ABS_CAP_MS = (progress || g_pchild) ? 600000 : 60000; // a scan can run minutes
    uint64_t t0 = nowMs(), last = t0, lastRender = 0;
    uint16_t lastChildPa = 0xFFFF;
    uint32_t lastTx = knxTunnelActivity();
    bool started = false;
    for (;;)
    {
        if (g_abort)
        {
            abortCleanly("user abort (Ctrl+C)");
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
            static const char* SPU[8] = {"⣾", "⣽", "⣻", "⢿", "⡿", "⣟", "⣯", "⣷"};
            static const char* SPA[8] = {"|", "/", "-", "\\", "|", "/", "-", "\\"};
            const int si = (int)((now / 80) % 8);
            const FtcStatus& st = openknxFileTransferClient.status();
            const uint16_t pa = openknxFileTransferClient.scanCurrentPa();
            char paS[16];
            std::snprintf(paS, sizeof(paS), "%u.%u.%u", (pa >> 12) & 0x0F, (pa >> 8) & 0x0F, pa & 0xFF);
            auto& c = g_tpl.theme();
            const std::string live = std::string("  ") + c.green(g_term.glyph(SPU[si], SPA[si])) + " " +
                                     c.cyan(std::string("scanning ") + paS) + "   " +
                                     c.bold(std::to_string(openknxFileTransferClient.scanFound()) + " found") + "   " +
                                     c.dim(std::to_string(st.done) + "/" + std::to_string(st.total) + " probed");
            // resize-safe: clip to the CURRENT terminal width (re-queried every render) so the line never wraps --
            // a wrapped line leaves fragments \r cannot clear (esp. when the window is shrunk mid-scan).
            std::fprintf(stderr, "\r%s\x1b[K", g_tpl.clip(live, ftc::Tpl::cols() - 1).c_str());
            std::fflush(stderr);
        }
        if (g_pchild) // parallel-scan child: emit the P progress protocol on stdout (the parent aggregates it)
        {
            const uint16_t pa = openknxFileTransferClient.scanCurrentPa();
            if (pa != lastChildPa)
            {
                lastChildPa = pa;
                const FtcStatus& stc = openknxFileTransferClient.status();
                std::printf("P\t%u\t%u\t%u\t%u\n", openknxFileTransferClient.scanFound(), stc.done, stc.total, pa);
                std::fflush(stdout);
            }
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
        if (now - last > QUIET_MS || now - t0 > ABS_CAP_MS) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (progress)
    {
        std::fprintf(stderr, "\r\x1b[K");
        std::fflush(stderr);
    } // clear the live line before the final report
}

/**********************************************************************
 ***************************** PARALLEL SCAN ************************
 **********************************************************************/
// --tunnels N: the parent self-execs N child scans over range chunks, each child on its OWN tunnel connection
// (own process). Overlaps the per-absent-address CO timeouts N-fold. Portable: popen/_popen for the children
// + std::thread for concurrent readers. No change to the single-tunnel core.
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

namespace
{
struct PChild // one parallel-scan child (chunk + live counters, atomics for the reader thread)
{
    uint16_t cs = 0, ce = 0;
    std::atomic<uint32_t> found{0}, probed{0}, total{0}, curPa{0};
    std::atomic<bool> done{false}, failed{false};
};
} // namespace

/**
 * @brief Run one wave of parallel child scans over [start,end] split into N chunks; append devices to @p out.
 * @details Devices are deduped by PA; chunks whose child could not get a tunnel go into @p failed. @p renderProgress
 *          draws the live aggregate.
 */
static void parallelScanWave(uint16_t start, uint16_t end, int N, const std::string& ip, uint16_t port,
                             std::vector<FtcEntry>& out, std::mutex& outMtx,
                             std::vector<std::pair<uint16_t, uint16_t>>& failed, bool renderProgress)
{
    const int span = (int)end - (int)start + 1;
    if (N > span) N = span;
    if (N < 1) N = 1;
    // The ip is interpolated into a popen() shell string below -> it MUST be a bare IPv4 literal, never anything
    // a shell could act on. inet_pton mirrors the tunnel's own connect check; a non-IPv4 value fails the whole
    // wave (the caller's no-progress guard then stops) instead of ever reaching /bin/sh.
    struct in_addr _ipv4chk;
    if (inet_pton(AF_INET, ip.c_str(), &_ipv4chk) != 1)
    {
        failed.emplace_back(start, end); // runs in the calling thread (children are joined before return) -> no lock
        return;
    }
    std::vector<std::unique_ptr<PChild>> kids;
    for (int i = 0; i < N; ++i)
    {
        auto k = std::unique_ptr<PChild>(new PChild());
        k->cs = (uint16_t)(start + (int)((int64_t)span * i / N));
        k->ce = (uint16_t)(start + (int)((int64_t)span * (i + 1) / N) - 1);
        kids.push_back(std::move(k));
    }
    std::vector<std::thread> threads;
    for (int i = 0; i < N; ++i)
    {
        PChild* k = kids[i].get();
        threads.emplace_back([k, ip, port, &out, &outMtx]() {
            std::string cmd = "\"" + g_selfPath + "\" --_pchild -i " + ip + " --port " + std::to_string(port) +
                              " scan " + paToStr(k->cs) + " " + paToStr(k->ce) + " ets";
#ifdef _WIN32
            cmd += " 2>NUL"; // swallow the child's own chrome/errors -- the parent renders one clean status
#else
            cmd += " 2>/dev/null";
#endif
            FILE* f = FTC_POPEN(cmd.c_str(), "r");
            if (!f)
            {
                k->failed = true;
                k->done = true;
                return;
            }
            char line[256];
            bool gotDevice = false, gotProgress = false;
            while (std::fgets(line, sizeof(line), f))
            {
                if (line[0] == 'P' && line[1] == '\t')
                {
                    unsigned fnd = 0, prb = 0, tot = 0, pa = 0;
                    if (std::sscanf(line, "P\t%u\t%u\t%u\t%u", &fnd, &prb, &tot, &pa) == 4)
                    {
                        k->found = fnd;
                        k->probed = prb;
                        k->total = tot;
                        k->curPa = pa;
                        gotProgress = true;
                    }
                    continue;
                }
                char pa[16] = {0}, cls[80] = {0};
                unsigned mask = 0;
                int ok = 0;
                if (std::sscanf(line, "%15[^\t]\t0x%x\t%79[^\t]\t%d", pa, &mask, cls, &ok) >= 2 && pa[0])
                {
                    std::lock_guard<std::mutex> lk(outMtx);
                    bool dup = false;
                    for (const auto& e : out)
                        if (std::strcmp(e.name, pa) == 0)
                        {
                            dup = true;
                            break;
                        }
                    if (!dup)
                    {
                        FtcEntry e{};
                        std::strncpy(e.name, pa, sizeof(e.name) - 1);
                        e.crc = (uint16_t)mask;
                        e.isOpenKnx = (ok == 1);
                        e.hasInfo = true;
                        out.push_back(e);
                    }
                    gotDevice = true;
                }
            }
            const int rc = FTC_PCLOSE(f);
            if (rc != 0 && !gotDevice && !gotProgress) k->failed = true; // no output + error -> tunnel refused
            k->done = true;
        });
    }
    // aggregate live progress until every child is done
    uint64_t lastRender = 0;
    for (;;)
    {
        bool allDone = true;
        uint32_t tf = 0, tp = 0, tt = 0;
        int active = 0;
        uint16_t frontPa = 0;
        for (auto& k : kids)
        {
            tf += k->found;
            tp += k->probed;
            tt += k->total;
            if (!k->done)
            {
                allDone = false;
                active++;
                if (k->curPa > frontPa) frontPa = (uint16_t)k->curPa;
            }
        }
        if (renderProgress && nowMs() - lastRender > 70)
        {
            lastRender = nowMs();
            static const char* SPU[8] = {"⣾", "⣽", "⣻", "⢿", "⡿", "⣟", "⣯", "⣷"};
            static const char* SPA[8] = {"|", "/", "-", "\\", "|", "/", "-", "\\"};
            const int si = (int)((nowMs() / 80) % 8);
            ftc::Theme& c = g_theme;
            std::string live = std::string("  ") + c.green(g_term.glyph(SPU[si], SPA[si])) + " " +
                               c.cyan(std::to_string(active) + "\xC3\x97 tunnels") + "   " +
                               c.bold(std::to_string(tf) + " found") + "   " +
                               c.dim(std::to_string(tp) + "/" + std::to_string(tt) + " probed" +
                                     (frontPa ? ("   ~" + paToStr(frontPa)) : std::string()));
            std::fprintf(stderr, "\r%s\x1b[K", g_tpl.clip(live, ftc::Tpl::cols() - 1).c_str());
            std::fflush(stderr);
        }
        if (allDone) break;
        if (g_abort) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    for (auto& t : threads)
        if (t.joinable()) t.join();
    if (renderProgress)
    {
        std::fprintf(stderr, "\r\x1b[K");
        std::fflush(stderr);
    }
    for (auto& k : kids)
        if (k->failed) failed.push_back({k->cs, k->ce});
}

/**
 * @brief Full parallel scan with adaptive wave-retry; @p allBusy set when the interface has no free tunnel slots.
 * @details Children that could not get a tunnel have their chunks retried in later waves with fewer tunnels,
 *          until all are covered or no progress is possible.
 */
static std::vector<FtcEntry> parallelScan(uint16_t start, uint16_t end, int nReq, const std::string& ip, uint16_t port, bool& allBusy)
{
    allBusy = false;
    std::vector<FtcEntry> out;
    std::mutex outMtx;
    std::vector<std::pair<uint16_t, uint16_t>> pending = {{start, end}};
    int N = nReq > 0 ? nReq : 4; // auto -> a safe default; explicit --tunnels N overrides
    for (int wave = 0; wave < 8 && !pending.empty() && !g_abort; ++wave)
    {
        std::vector<std::pair<uint16_t, uint16_t>> failed;
        const size_t before = out.size();
        for (auto& chunk : pending)
        {
            if (g_abort) break;
            parallelScanWave(chunk.first, chunk.second, N, ip, port, out, outMtx, failed, wave == 0);
        }
        // A whole wave failed with NO progress -> the interface simply has no free tunnel slots. Stop here (do
        // not retry 8 times and flood the terminal); the caller reports it once.
        if (out.size() == before && !failed.empty() && failed.size() >= pending.size())
        {
            allBusy = true;
            break;
        }
        pending = failed;
        if (N > 1) N = (N + 1) / 2; // fewer tunnels each retry wave (slots were busy) -> converge
    }
    return out;
}

/**
 * @brief Probe which tunnel additional-addresses are currently free (union of reported PAs = free; rest = busy).
 * @details Spawns one short-lived probe child per slot; each opens a tunnel and reports the free PA it is handed,
 *          holding it briefly so siblings get *distinct* free slots.
 */
static std::set<uint16_t> probeTunnelSlots(const std::string& ip, uint16_t port, int slots)
{
    std::set<uint16_t> freeSet;
    if (slots < 1 || ip.empty()) return freeSet;
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
        threads.emplace_back([ip, port, &freeSet, &mtx]() {
            std::string cmd = "\"" + g_selfPath + "\" --_probeslot -i " + ip + " --port " + std::to_string(port);
#ifdef _WIN32
            cmd += " 2>NUL";
#else
            cmd += " 2>/dev/null";
#endif
            FILE* f = FTC_POPEN(cmd.c_str(), "r");
            if (!f) return;
            char line[128];
            while (std::fgets(line, sizeof(line), f))
            {
                unsigned a = 0, b = 0, c = 0;
                if (std::sscanf(line, "SLOT\t%u.%u.%u", &a, &b, &c) == 3)
                {
                    std::lock_guard<std::mutex> lk(mtx);
                    freeSet.insert((uint16_t)(((a & 0x0F) << 12) | ((b & 0x0F) << 8) | (c & 0xFF)));
                }
            }
            FTC_PCLOSE(f);
        });
    }
    for (auto& t : threads)
        if (t.joinable()) t.join();
    return freeSet;
}

/**
 * @brief Per-class summary footer for a scan result (total + how many of each device class).
 */
static void renderScanSummary(const std::vector<FtcEntry>& devices)
{
    ftc::Theme& c = g_theme;
    ftc::I18n& L = g_i18n;
    std::vector<std::pair<std::string, int>> byClass;
    for (const auto& e : devices)
    {
        const char* cls = ftc::knxMaskName((uint16_t)e.crc);
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
    std::string s = c.bold(std::to_string(devices.size()) + L.tr(" devices", " Geräte"));
    for (const auto& p : byClass)
        s += c.dim("   \xC2\xB7   ") + c.txt(std::to_string(p.second) + "\xC3\x97 " + p.first);
    std::printf("  %s\n", s.c_str());
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

    std::string cmd = "ftc";
    for (const auto& p : pos)
    {
        cmd += ' ';
        cmd += p;
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
        openknxFileTransferClient.processCommand(cmd, false);
        ftcPumpStructured(snap, k == K_Scan, k == K_Scan && !quiet); // live progress line for the scan (unless -q)
    }
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
            std::string feat;
            if (d.features & 0x1) feat += "Resume ";
            if (d.features & 0x2) feat += "Update ";
            if (d.features & 0x4) feat += "Fast ";
            if (d.features & 0x8) feat += "Console ";
            if (feat.empty()) feat = "(none)";
            else
                feat.pop_back();
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
                std::printf("features\t%s\n", feat.c_str());
                std::printf("progmode\t%d\n", d.progMode ? 1 : 0);
                if (d.appState != 0xFF) std::printf("app_state\t%s\n", ftcLoadNameH(d.appState));
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
            p.kv(L.tr("Prog mode", "Prog-Modus"),
                 d.progMode ? t.chip("PROG", 'o') : c.mut(std::string(g_term.glyph("○", "o")) + " off"));
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
                p.kv(L.tr("Run state", "Ausführungszustand"), c.txt(std::to_string((unsigned)d.bcuRunState)));
                std::snprintf(ib, sizeof(ib), "0x%02X", d.bcuRunError);
                p.kv(L.tr("Run error", "Ausführungsfehler"), c.txt(ib) + (d.bcuRunError >= 0xFE ? c.green("  · OK") : c.red("  · error")));
                std::snprintf(ib, sizeof(ib), "0x%02X", d.bcuPeiType);
                p.kv(L.tr("PEI type", "PEI-Typ"), c.txt(ib));
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
            rc = n > 0 ? 0 : 1;
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
            };
            std::map<uint16_t, KoRow> byKo;
            for (uint16_t i = 0; i < n; ++i)
            {
                KoRow& r = byKo[gos[i].co];
                if (gos[i].ga != 0) r.gas.push_back(gos[i].ga);
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
                    std::printf("ko\t%u\t%s\t%s\t%s\t%s\n", kv.first, gl.empty() ? "-" : gl.c_str(),
                                kv.second.cfg ? flagsActive(kv.second.flags).c_str() : "?",
                                prioName(kv.second.cfg, kv.second.prio).c_str(), sizeName(kv.second.cfg, kv.second.sizeCode).c_str());
                }
                break;
            }
            t.section(L.tr("Group communication · ", "Gruppenkommunikation · ") + target);
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
                            t.koFlags(kv.second.cfg ? flagsActive(kv.second.flags) : ""),
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
                t.tableRow({c.dim("MODE"), c.dim("SIZE"), c.dim("CRC"), c.dim("NAME")}, {5, -10, 10, 0});
                for (const auto& e : snap)
                {
                    char crc[12];
                    std::snprintf(crc, sizeof(crc), "%08X", (unsigned)e.crc);
                    std::string name = c.txt(e.name);
                    if (e.isOpenKnx) name += "  " + t.chip("OpenKNX");
                    std::string sizeCol = e.isDir ? c.dim("—") : c.txt(fmtSize(e.size));
                    std::string crcCol = e.isDir ? c.dim("—") : (e.hasCrc ? c.mut(crc) : c.dim("n/a"));
                    t.tableRow({e.isDir ? c.cyan("dir") : c.txt("file"), sizeCol, crcCol, name},
                               {5, -10, 10, 0});
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
            if (quiet)
            {
                for (const auto& e : snap)
                    std::printf("%s\t0x%04X\t%s\t%d\n", e.name, (unsigned)e.crc, ftc::knxMaskName((uint16_t)e.crc), e.isOpenKnx ? 1 : 0);
                break;
            }
            t.status(ftc::Tpl::Stat::Ok, L.tr("scan complete", "Scan fertig"),
                     {std::to_string(snap.size()) + L.tr(" device(s)", " Gerät(e)")});
            if (snap.empty()) break;
            t.tableRow({c.dim("ST"), c.dim("PA"), c.dim("CLASS"), c.dim("INFO")}, {2, 10, 26, 0});
            for (const auto& e : snap)
            {
                const char* cls = ftc::knxMaskName((uint16_t)e.crc);
                std::string info = e.isOpenKnx ? (std::string() + t.chip("OpenKNX")) : c.dim("");
                t.tableRow({t.statusDot('g'), c.txt(e.name), c.txt(cls[0] ? cls : "—"), info}, {2, 10, 26, 0});
            }
            renderScanSummary(snap); // total + per-class breakdown footer (parity with the parallel scan)
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
 * @brief The ftc-cli entry point: init, parse args, dispatch the command, run its loop, and shut down cleanly.
 */
int main(int argc, char** argv)
{
    g_selfPath = (argc > 0 && argv[0]) ? argv[0] : "ftc"; // re-invoked for parallel-scan children
    if (!socketStartup())
    {
        std::fprintf(stderr, "fatal: socket init failed\n");
        return 1;
    }
    g_color = initTerminal(); // UTF-8 + ANSI, TTY-aware, NO_COLOR-honoring
    // Config file (~/.config/ftc/config.toml) — the persisted defaults. Precedence: CLI flags > config > env.
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
    std::vector<std::string> pos; // positional tokens -> the `ftc ...` command tail

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
            verbose = true;
        else if (a == "--quiet" || a == "-q")
            quiet = true;
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
        else if ((a == "--tunnels" || a == "-T") && i + 1 < argc)
            g_tunnels = std::atoi(argv[++i]); // N parallel tunnels for the scan; 0 = as many as the interface allows
        else if (a == "--tunnels" || a == "-T")
            g_tunnels = 0; // bare --tunnels = auto (max)
        else if (a == "--_pchild")
            g_pchild = true; // internal: parallel-scan child -> emit the P/D line protocol
        else if (a == "--_probeslot")
            g_probeSlot = true; // internal: tunnel-slot probe child -> connect, emit SLOT, hold, exit
        else if (a == "--frames" && i + 1 < argc)
            monFrames = std::atoi(argv[++i]);
        else if (a == "--seconds" && i + 1 < argc)
        {
            monSeconds = std::atoi(argv[++i]);
            secondsSet = true;
        }
        else if (a == "--lang" && i + 1 < argc)
            ++i; // consumed here; already applied in the pre-scan above
        else if (a == "--theme" && i + 1 < argc)
            ++i; // consumed here; already applied in the pre-scan above
        else if (a == "--ascii")
        { /* consumed here; already applied in the pre-scan above */
        }
        else if (!a.empty() && a[0] == '-' && !(a.size() > 1 && (isdigit((unsigned char)a[1]))))
        {
            std::fprintf(stderr, "unknown option: %s\n", a.c_str());
            socketCleanup();
            return 1;
        }
        else
            pos.push_back(a);
    }

    if (g_pchild) quiet = true; // a parallel-scan child emits only the P/D line protocol (no chrome)

    // --- internal: tunnel-slot probe child --------------------------------------------------------
    // Open ONE tunnel; on success print the assigned free PA and hold it briefly so sibling probes each get a
    // *distinct* free slot, then release. The parent (renderInterfaceInfo) collects the reported PAs = free.
    if (g_probeSlot)
    {
        int rc = 3; // 3 = refused / no free slot
        if (!ip.empty() && g_knxTunnel.connect(ip, port))
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

    // `ftc ?` / `ftc help` -> the same usage as --help (the device console uses `ftc ?`, so accept it here too).
    if (pos[0] == "?" || pos[0] == "help")
    {
        usage();
        socketCleanup();
        return 0;
    }

    // `ftc progscan|ps locate` — self-discovering prog-mode localisation (no --ip). Enumerate every KNXnet/IP
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

    // `ftc gzip <in> <out>` — in-process gzip via miniz (RP firmware prep; no tunnel needed).
    if (pos[0] == "gzip")
    {
        ftc::I18n& L = g_i18n;
        if (pos.size() < 3)
        {
            std::fprintf(stderr, "%s\n", L.tr("usage: ftc gzip <in> <out>", "Aufruf: ftc gzip <ein> <aus>"));
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

    // `ftc decode <hex…>` — offline decode of a raw TP1 LPDU (no tunnel): addresses · TPCI/APCI · OpenKNX FTC /
    // console, coloured by the busmon interpreter. Paste bytes from a capture; mirrors BusmonViewer.html.
    if (pos[0] == "decode")
    {
        std::string hex;
        for (size_t i = 1; i < pos.size(); ++i)
            hex += pos[i]; // accept space-separated bytes or one blob
        if (hex.empty())
        {
            std::fprintf(stderr, "%s\n", g_i18n.tr("usage: ftc decode <hex LPDU>", "Aufruf: ftc decode <hex-LPDU>"));
            socketCleanup();
            return 1;
        }
        ftc::Monitor mon(g_term, g_theme, g_tpl, g_i18n);
        const int rc = mon.decodeOffline(hex, quiet, verbose);
        socketCleanup();
        return rc;
    }

    // `ftc config` — show the persisted config; `ftc config <key> <value>` sets + writes it.
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
            std::printf("  %s\n", c.dim(L.tr("(empty — keys: theme · lang · ascii · mode; e.g. ftc config theme amber)",
                                             "(leer — Schlüssel: theme · lang · ascii · mode; z.B. ftc config theme amber)"))
                                      .c_str());
        socketCleanup();
        return 0;
    }

    if (ip.empty())
    {
        std::fprintf(stderr, "error: --ip is required (or use --discover). See --help.\n");
        socketCleanup();
        return 1;
    }

    // `ftc --ip <ip> info` (no PA) — the full interface report. No tunnel, no bus: DESCRIPTION + device-mgmt.
    if (pos.size() == 1 && pos[0] == "info")
    {
        const int rc = renderInterfaceInfo(ip, port, quiet);
        socketCleanup();
        return rc;
    }

    // `ftc --ip <ip> groupmon|gm | busmon|bm` (no PA) — live monitors over a self-owned tunnel (own KNX layer).
    // -q gives plain tab-separated lines; -V adds raw hex; --frames/--seconds cap it for scripted runs; Ctrl+C stops.
    const bool isBus = pos[0] == "busmon" || pos[0] == "bm";
    if (pos.size() == 1 && (isBus || pos[0] == "groupmon" || pos[0] == "gm"))
    {
        ftc::Monitor mon(g_term, g_theme, g_tpl, g_i18n);
        mon.target(ip, port);
        const int rc = mon.run(isBus ? ftc::Monitor::Mode::Bus : ftc::Monitor::Mode::Group,
                               quiet, verbose, monFrames, monSeconds, &g_abort);
        socketCleanup();
        return rc;
    }

    // `ftc --ip <ip> progscan|ps [global]` (no PA) — broadcast A_IndividualAddress_Read over a self-owned tunnel
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

    // `ftc -i <ip> con|console` (no PA) — the INTERFACE'S OWN console via the OFM-Network webconsole (WebSocket
    // ws://<ip>:80/console), NOT the KNX tunnel: no APDU/drain, fast local streaming, no truncation. If the
    // device has no webconsole -> one clean error and exit. `ftc -i <ip> <pa> con` (with a PA) still tunnels.
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
                if (!ws.poll(lines)) break;
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
            if (!ws.poll(lines)) break;
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

    // --- parallel scan (--tunnels != 1): N child scans over range chunks, each on its own tunnel; the parent needs
    // no tunnel of its own. Only for the CO ("ets") scan of a parseable a.l / a.l.d range; else fall back to serial.
    {
        uint16_t pStart = 0, pEnd = 0;
        const bool isEts = std::find(pos.begin(), pos.end(), std::string("ets")) != pos.end();
        if (!g_pchild && g_tunnels != 1 && pos.size() && pos[0] == "scan" && isEts && !ip.empty() && parseScanRange(pos, pStart, pEnd))
        {
            ftc::I18n& L = g_i18n;
            ftc::Theme& c = g_theme;
            if (!quiet)
                std::printf("\n  %s %s\n", c.amber(g_term.glyph("\xE2\x9A\xA1", "!")).c_str(),
                            c.dim(L.tr("parallel scan", "Parallel-Scan") + std::string(" \xC2\xB7 ") + paToStr(pStart) + "\xE2\x80\xA6" + paToStr(pEnd) +
                                  (g_tunnels > 0 ? ("  \xC2\xB7  " + std::to_string(g_tunnels) + "\xC3\x97 tunnels") : std::string("  \xC2\xB7  auto tunnels")))
                                .c_str());
            bool allBusy = false;
            std::vector<FtcEntry> found = parallelScan(pStart, pEnd, g_tunnels, ip, port, allBusy);
            if (allBusy && found.empty()) // interface out of tunnel slots -> one clean message, not a flood
            {
                std::printf("  %s %s\n    %s\n", c.red(g_term.glyph("\xE2\x9C\x96", "x")).c_str(),
                            c.bold(L.tr("no free tunnel slots", "keine freien Tunnel-Slots")).c_str(),
                            c.dim(L.tr("all tunnels on this interface are busy -- free one, use another interface, or wait",
                                       "alle Tunnel dieses Interfaces sind belegt -- gib einen frei, nimm ein anderes Interface, oder warte"))
                                .c_str());
                socketCleanup();
                return 1;
            }
            auto paNum = [](const char* s) { unsigned a = 0, l = 0, d = 0; std::sscanf(s, "%u.%u.%u", &a, &l, &d); return (a << 12) | (l << 8) | d; };
            std::sort(found.begin(), found.end(), [&](const FtcEntry& a, const FtcEntry& b) { return paNum(a.name) < paNum(b.name); });
            if (quiet)
                for (const auto& e : found)
                    std::printf("%s\t0x%04X\t%s\t%d\n", e.name, (unsigned)e.crc, ftc::knxMaskName((uint16_t)e.crc), e.isOpenKnx ? 1 : 0);
            else
            {
                g_tpl.tableRow({c.dim("ST"), c.dim("PA"), c.dim("CLASS"), c.dim("INFO")}, {2, 10, 26, 0});
                for (const auto& e : found)
                {
                    const char* cls = ftc::knxMaskName((uint16_t)e.crc);
                    std::string info = e.isOpenKnx ? (std::string() + g_tpl.chip("OpenKNX")) : std::string();
                    g_tpl.tableRow({g_tpl.statusDot('g'), c.txt(e.name), c.txt(cls[0] ? cls : "\xE2\x80\x94"), info}, {2, 10, 26, 0});
                }
                renderScanSummary(found);
            }
            socketCleanup();
            return g_abort ? 130 : 0;
        }
    }

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
                            {line, L.tr("ftc does not support KNX IP Secure.", "ftc unterstützt kein KNX IP Secure.")},
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
                            "ftc --discover");
        }
        else
        {
            std::snprintf(line, sizeof(line),
                          L.tr("no response from %s:%u (neither CONNECT nor DESCRIPTION).",
                               "keine Antwort von %s:%u (weder CONNECT noch DESCRIPTION)."),
                          ip.c_str(), (unsigned)port);
            g_ui.errorBlock(false, L.tr("interface unreachable", "Interface nicht erreichbar"),
                            {line, L.tr("unreachable, or the IP/port is wrong.", "nicht erreichbar, oder IP/Port falsch.")},
                            "ftc --discover");
        }
        socketCleanup();
        return 1;
    }
    if (!quiet)
    {
        ftc::Theme& c = g_theme;
        ftc::I18n& L = g_i18n;
        char pa[16];
        std::snprintf(pa, sizeof(pa), "%u.%u.%u", (g_knxTunnel.assignedPA() >> 12) & 0x0F,
                      (g_knxTunnel.assignedPA() >> 8) & 0x0F, g_knxTunnel.assignedPA() & 0xFF);
        std::printf("  %s %s  %s  %s\n", c.green(g_term.glyph("●", "*")).c_str(),
                    c.green(L.tr("tunnel up", "Tunnel steht")).c_str(),
                    c.dim(ip + ":" + std::to_string((unsigned)port)).c_str(),
                    c.dim(std::string(L.tr("as ", "als ")) + pa).c_str());
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
    openknxFileTransferClient.setApduHint(ifaceApdu);

    // --verbose: read + print the full TARGET steckbrief before the actual command, reusing the device-info
    // discovery chain (mask/class · manufacturer · order · hw · version · prog mode · app program · table
    // load states · File-Transfer features). pos[0] is the target PA; all small property reads over the
    // tunnel -> fits any interface. Runs to quiescence, then the real command proceeds normally.
    // `scan` has NO target PA (pos[0] == "scan"), so an "ftc <pos[0]> info" here would run "ftc scan info" —
    // an accidental own-line scan that shadows the real command. Skip the target steckbrief for it.
    const bool isConsoleCmd = pos.size() >= 2 && (pos[1] == "console" || pos[1] == "con");
    if (verbose && !pos.empty() && pos[0] != "scan" && !isConsoleCmd) // con-open shows NO device info (clean session); one-shot keeps the panel
    {
        std::printf("\n  %s\n", g_theme.green(std::string(g_term.glyph("── ", "-- ")) + g_i18n.tr("KNX Device · ", "KNX-Gerät · ") + pos[0] + g_i18n.tr(" via ", " über ") + ip + " ──────────────────────────").c_str());
        int idrc = 0;
        ftcRenderStructured({pos[0], "info"}, quiet, idrc); // NEW structured device panel (same as `ftc <pa> info`), not the raw text
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

    // Stage 2: render read/status <pa> commands from the client's structured getters (raw text suppressed).
    // Not a structured command (or console mode) -> fall through to the normal one-shot path below.
    if (!consoleMode && ftcRenderStructured(pos, quiet, exitCode))
    {
        g_knxTunnel.disconnect();
        if (exitCode == 130)
            std::fprintf(stderr, "Aborted by user (Ctrl+C) -- transfer cancelled, tunnel closed cleanly (no interface lockout).\n");
        socketCleanup();
        return exitCode;
    }

    // Transfer commands (send/get/perf): the CLI owns the host presentation (setup + xx.xx% progress + result
    // Panels), drawn from the client's info-API. Suppress the SM's own console box so it isn't double-printed.
    // Quiet keeps the SM lines (scriptable); the embedded SM box is untouched (that path never sets suppress).
    const bool isXferCmd = !consoleMode && !quiet && pos.size() >= 2 &&
                           (pos[1] == "send" || pos[1] == "upload" || pos[1] == "get" ||
                            pos[1] == "download" || pos[1] == "receive" || pos[1] == "perf");
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
            std::string feat;
            if (d.features & 0x4) feat += "Fast ";
            if (d.features & 0x8) feat += "Console ";
            if (d.features & 0x1) feat += "Resume ";
            if (d.features & 0x2) feat += "Update ";
            if (!feat.empty()) feat.pop_back();
            tgtOrder = d.haveOrder ? d.order : std::string();
            tgtMask = mb;
            tgtCls = d.cls;
            tgtFw = fw;
            tgtFeat = feat;
            tgtValid = true;
        }
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
                        emitLine(g_theme.dim("          Leave and run it as a one-shot:  ftc <pa> login <pw>").c_str());
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
                        if (line == "quit" || line == "exit") running = false;
                    }
                }

                const uint64_t now = nowMs();
                if (now - lastBar > 250)
                {
                    lastBar = now;
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
                    exitCode = (openknxFileTransferClient.status().phase == FtcPhase::Failed) ? 1 : 0;
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
            if (!quiet)
                std::printf("[console] connected — type commands; 'quit' or 'exit' to leave.%s%s\n",
                            g_logPath.empty() ? "" : "  log: ", g_logPath.c_str());
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

                std::string line;
                if (stdinLines.poll(line))
                {
                    if (lineHasAuthKeyword(line))
                        std::printf("[console] 'login'/'logout' is not relayed here (it would send the password in\n"
                                    "          clear). Leave the console and run it as a one-shot:  ftc <pa> login <pw>\n");
                    else
                    {
                        feedConsoleLine(line);
                        if (line == "quit" || line == "exit") running = false;
                    }
                }
                else if (stdinLines.eof())
                {
                    feedConsoleLine("quit");
                    pump(64, 2);
                    running = false;
                }

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
    }

    // --- clean shutdown ---------------------------------------------------------------------------
    g_knxTunnel.disconnect();
    if (exitCode == 130)
        std::fprintf(stderr, "Aborted by user (Ctrl+C) -- transfer cancelled, tunnel closed cleanly (no interface lockout).\n");
    socketCleanup();
    return exitCode;
}

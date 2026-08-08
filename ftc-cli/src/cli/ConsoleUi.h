/**
 * @file        ConsoleUi.h
 * @brief       Interactive `ftc … console` shell with a pinned BOTTOM status bar.
 * @details     The box is pinned at the BOTTOM so the scroll region keeps top margin row 1 — that is what
 *              lets every xterm-family terminal push scrolled-off lines into native scrollback (a top bar,
 *              margin != 1, would discard them). Raw mode gives a real line editor + persistent history.
 * @date        2026-08-03
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "I18n.h"
#include "Templates.h"
#include "Term.h"
#include "Theme.h"

#ifdef _WIN32
    #include <conio.h>
    #include <io.h>
    #include <windows.h>
#else
    #include <sys/ioctl.h>
    #include <sys/select.h>
    #include <termios.h>
    #include <unistd.h>
#endif

namespace ftc
{

class ConsoleUi
{
  public:
    enum PollResult
    {
        PollNone = 0,
        PollSubmit,
        PollAbort,
        PollEof,
        PollToggleLog,
        PollHelp
    };

    ConsoleUi(Term& term, Theme& theme, I18n& i18n) : _t(term), _c(theme), _i(i18n) {}

    /**********************************************************************
     ************************** CONFIGURATION *****************************
     **********************************************************************/

    /**
     * @brief Update the log path shown in the status box (empty clears it -> logging off).
     */
    void setLogPath(const std::string& p) { _logPath = p; }

    /**
     * @brief Verbose mode adds a third status line + a wider RX graph.
     * @details Must be set BEFORE begin() — the reserved-row count (and thus the scroll region) depends on it.
     */
    void setVerbose(bool v) { _verbose = v; }

    /**
     * @brief Feed the verbose l3 figures: the console drain cap (B/answer) and the detected interface max APDU.
     */
    void setLimits(uint16_t drainCap, uint16_t apdu)
    {
        _drain = drainCap;
        _apdu = apdu;
    }

    /**
     * @brief Record a command the user just sent (starts the round-trip timer for the verbose l3 line).
     */
    void noteCommand(const std::string& cmd)
    {
        _lastCmd = cmd.size() > 24 ? cmd.substr(0, 23) + "…" : cmd; // bound the width so l3 never wraps
        _lastCmdMs = nowMs();
        _awaitRtt = true;
    }

    /**
     * @brief Interface steckbrief (the device we tunnel THROUGH) for the verbose l4 line.
     */
    void setIface(const std::string& order, const std::string& name, const std::string& mask,
                  int slotsFree = -1, int slotsTotal = 0)
    {
        _ifOrder = order;
        _ifName = name;
        _ifMask = mask;
        _slotsFree = slotsFree;
        _slotsTotal = slotsTotal;
    }

    /**
     * @brief Target steckbrief (the device we TALK TO) for the verbose l4 line.
     */
    void setTarget(const std::string& order, const std::string& mask, const std::string& cls,
                   const std::string& fw, const std::string& features)
    {
        _tgtOrder = order;
        _tgtMask = mask;
        _tgtCls = cls;
        _tgtFw = fw;
        _tgtFeat = features;
    }

    /**
     * @brief Active auto-command (/every) count -> the ⟳ badge in the status bar.
     */
    void setJobs(int n) { _jobCount = n; }

    /**
     * @brief WebSocket-console mode (ftc -i <ip> con): no KNX tunnel.
     * @details The l3/l4 tunnel/APDU/drain fields are replaced by the ws:// endpoint; l1 shows
     *          "webconsole <ip>" instead of the via/target chips.
     */
    void setWsMode(const std::string& ip)
    {
        _wsMode = true;
        _wsUrl = "ws://" + ip + "/console";
    }

    /**
     * @brief Enter raw mode, reserve the region, load history, paint the box + the input line.
     */
    void begin(const std::string& ip, const std::string& tunnelPa, const std::string& target,
               const std::string& logPath, const std::string& historyPath)
    {
        _ip = ip;
        _tpa = tunnelPa;
        _target = target;
        _logPath = logPath;
        _histFile = historyPath;
        loadHistory();
        enterRaw();
        size(_rows, _cols);           // fix the geometry once — every row calc below must agree
        std::printf("\x1b[2J\x1b[H"); // clear the screen; the console owns it now
        setRegion();                  // scroll region = rows 1..H-5 (top margin 1 -> scrollback)
        std::printf("\x1b[?25l");     // hide the real cursor — we draw our OWN blinking block
        paintBox(0, 0, 0, 0, 0);      // the OpenKNX logo is a fixed bottom-right watermark
        redrawInput();
        std::fflush(stdout);
    }

    /**********************************************************************
     ************************** INPUT POLLING *****************************
     **********************************************************************/

    /**
     * @brief Non-blocking key read: edit the line / recall history; on Enter fill @p out and return PollSubmit.
     * @details Ctrl-C -> PollAbort, closed stdin -> PollEof.
     */
    PollResult poll(std::string& out)
    {
#ifdef _WIN32
        while (_kbhit())
        {
            int c = _getch();
            if (c == 0 || c == 0xE0) // extended: arrows
            {
                int c2 = _getch();
                if (c2 == 72) histUp();
                else if (c2 == 80)
                    histDown();
                else if (c2 == 75)
                    moveLeft();
                else if (c2 == 77)
                    moveRight();
                redrawInput();
                continue;
            }
            PollResult r = feed((unsigned char)c, out);
            if (r != PollNone) return r;
        }
        return PollNone;
#else
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(STDIN_FILENO, &rf);
        struct timeval tv{0, 0};
        if (select(STDIN_FILENO + 1, &rf, nullptr, nullptr, &tv) <= 0) return PollNone;
        unsigned char buf[64];
        int n = (int)read(STDIN_FILENO, buf, sizeof(buf));
        if (n == 0) return PollEof; // stdin closed
        if (n < 0) return PollNone;
        _pend.append((const char*)buf, (size_t)n);
        return drain(out);
#endif
    }

    /**********************************************************************
     ***************************** OUTPUT *********************************
     **********************************************************************/

    /**
     * @brief Print one remote output line into the scroll region (above the input line), keeping scrollback.
     */
    void emit(const std::string& line)
    {
        if (_awaitRtt) // first device output after a command -> record the round-trip + fold into min/avg/max
        {
            _lastRttMs = (uint32_t)(nowMs() - _lastCmdMs);
            _awaitRtt = false;
            if (_rttN == 0 || _lastRttMs < _rttMin) _rttMin = _lastRttMs;
            if (_lastRttMs > _rttMax) _rttMax = _lastRttMs;
            _rttSum += _lastRttMs;
            ++_rttN;
        }
        const int regionBottom = _rows - reservedRows();
        // Move into the region, scroll one line up, write the text, then EXPLICITLY park the cursor back on the
        // input line. DECSC/DECRC over a scroll is unreliable (the restored cursor drifts), so reposition here.
        std::printf("\x1b[%d;1H\n", regionBottom); // at the region's bottom margin: LF scrolls it up 1
        std::printf("\r\x1b[2K%s", line.c_str());  // clear + write the new line at the region bottom
        drawCursor();                              // redraw our block cursor back on the input line
        std::fflush(stdout);
    }

    /**
     * @brief Print the COMPLETE local help into the scroll region — shortcuts + /job set + /stat + the model.
     * @details This is the ONE help (bare '?' AND '/help'); the user must never have to guess `/job help`.
     */
    void showHelp()
    {
        Theme& c = _c;
        emit("");
        emit(c.dim(_i.tr("── ftc console — local commands ───────────────", "── ftc-Konsole — lokale Befehle ───────────────")));
        emit("  " + c.cyan("quit") + c.dim(" / ") + c.cyan("exit") + "       " + c.dim(_i.tr("leave the session", "Session verlassen")));
        emit("  " + c.cyan("↑ ↓") + "             " + c.dim(_i.tr("command history", "Befehls-Verlauf")));
        emit("  " + c.cyan("^L") + "              " + c.dim(_i.tr("toggle session logging", "Logging an/aus")));
        emit("  " + c.cyan("?") + " " + c.dim("/") + c.cyan("help") + "         " + c.dim(_i.tr("this help", "diese Hilfe")));
        emit("  " + c.cyan("/stat") + "           " + c.dim(_i.tr("full session stats (also written to the log on exit)", "komplette Session-Statistik (beim Beenden auch ins Log)")));
        emit("");
        emit(c.dim(_i.tr("  auto-commands — /job (recurring, cached per target):", "  Auto-Befehle — /job (wiederkehrend, pro Ziel gecached):")));
        emit("    " + c.cyan("/job add watch every <int> <cmd>") + "  " + c.dim(_i.tr("e.g. /job add watch every 1s m", "z.B. /job add watch every 1s m")));
        emit("    " + c.cyan("/job") + " " + c.dim("|") + " " + c.cyan("/job list") + "             " + c.dim(_i.tr("list jobs", "Jobs auflisten")));
        emit("    " + c.cyan("/job <id> on | off | rm") + "        " + c.dim(_i.tr("manage one", "einzelnen verwalten")));
        emit("    " + c.cyan("/job on|off|rm all") + " " + c.dim("·") + " " + c.cyan("/job clear") + "  " + c.dim(_i.tr("bulk / remove all", "alle / alle entfernen")));
        emit(c.dim(_i.tr("    intervals: 500ms · 1s · 2m — fired only when ready (never floods the bus).",
                         "    Intervalle: 500ms · 1s · 2m — feuern nur wenn ready (kein Bus-Flood).")));
        emit("");
        emit(c.dim(_i.tr("everything else is sent to ", "alles andere geht an ")) + c.txt(_target) +
             c.dim(_i.tr(" — type the device's own commands (e.g. help).", " — dort die Geräte-Befehle tippen (z.B. help).")));
        emit("");
    }

    /**
     * @brief The full session stats as a block of lines — for `/stat` (colored, into the window) and the
     *        on-exit log dump (plain text).
     * @details Live byte/uptime counters are passed in; RTT/rates/tunnel/steckbrief/jobs come from members.
     */
    std::vector<std::string> statLines(uint32_t rx, uint32_t tx, uint32_t drops, uint32_t trunc, uint64_t up, bool colored)
    {
        Theme& c = _c;
        auto D = [&](const std::string& s) { return colored ? c.dim(s) : s; };
        auto T = [&](const std::string& s) { return colored ? c.txt(s) : s; };
        auto M = [&](const std::string& s) { return colored ? c.mut(s) : s; };
        auto G = [&](const std::string& s) { return colored ? c.green(s) : s; };
        std::vector<std::string> o;
        char dur[16];
        std::snprintf(dur, sizeof(dur), "%u:%02u", (unsigned)(up / 60), (unsigned)(up % 60));
        const uint32_t avg = up ? (uint32_t)(rx / up) : 0;
        const uint32_t rttAvg = _rttN ? _rttSum / _rttN : 0;
        if (_wsMode) o.push_back(D("── stats · ") + M(_i.tr("webconsole", "Webkonsole")) + " " + T(_ip) + D(" ──────────────"));
        else
            o.push_back(D("── stats · ") + T(_target) + D(" via ") + T(_tpa) + D(" ──────────────"));
        o.push_back(" " + M("uptime   ") + " " + T(dur) + "      " + M("jobs") + " " + T(std::to_string(_jobCount)));
        o.push_back(" " + M("RX       ") + " " + T(humanSize(rx)) + "   " + D("· avg " + std::to_string(avg) + " B/s · peak " + std::to_string(_sparkPeak) + " B/s"));
        o.push_back(" " + M("TX       ") + " " + T(humanSize(tx)));
        o.push_back(" " + M("\xCE\xA3 total  ") + " " + T(humanSize(rx + tx)) + "   " + D("· drops " + std::to_string(drops) + " · trunc " + std::to_string(trunc)));
        if (_rttN)
            o.push_back(" " + M("RTT      ") + " " + T("last " + std::to_string(_lastRttMs) + " ms") + "   " +
                        D("· min " + std::to_string(_rttMin) + " · avg " + std::to_string(rttAvg) + " · max " + std::to_string(_rttMax) + " (n=" + std::to_string(_rttN) + ")"));
        if (_wsMode)
            o.push_back(" " + M("transport") + " " + D(_wsUrl)); // WS: no tunnel/drain/APDU
        else
            o.push_back(" " + M("tunnel   ") + " " + D("ch " + std::to_string(_chan) + " · seq " + std::to_string(_txSeq) + "/" + std::to_string(_rxSeq) + " · drain " + std::to_string(_drain) + " B/ans · APDU " + std::to_string(_apdu)));
        if (!_wsMode)
        {
            std::string s = " " + M("target   ") + " " + T(_target);
            if (!_tgtMask.empty()) s += " " + D(_tgtMask);
            if (!_tgtCls.empty()) s += " " + D(_tgtCls);
            if (!_tgtFw.empty()) s += "  " + M("fw") + " " + T(_tgtFw);
            if (!_tgtFeat.empty()) s += "  " + G(_tgtFeat);
            o.push_back(s);
        }
        {
            std::string s = " " + M("interface") + " " + T(_wsMode ? _ip : _tpa);
            if (!_ifName.empty()) s += " " + D(_ifName);
            if (!_ifMask.empty()) s += " " + D(_ifMask);
            o.push_back(s);
        }
        o.push_back(D("────────────────────────────────────────"));
        return o;
    }

    /**********************************************************************
     *************************** STATUS BOX ******************************
     **********************************************************************/

    /**
     * @brief Repaint the status box (called on a ~500 ms timer + after activity). Cursor untouched.
     * @details SELF-HEALS the layout: a resize or an external "clear buffer" (iTerm2 ⌘K) resets the scroll
     *          region and wipes our chrome, and there is no signal an app can catch for a clear — so we
     *          re-assert the region and repaint the chrome every tick (idempotent). A resize re-fixes geometry.
     */
    void tick(uint32_t rx, uint32_t tx, uint32_t drops, uint32_t trunc, uint64_t uptimeSec,
              uint8_t chan = 0, uint8_t txSeq = 0, uint8_t rxSeq = 0)
    {
        _chan = chan;
        _txSeq = txSeq;
        _rxSeq = rxSeq; // tunnel-health snapshot for the verbose l3 line
        int r, c;
        size(r, c);
        const bool resized = (r != _rows || c != _cols);
        if (resized)
        {
            _rows = r;
            _cols = c;
            // A resize invalidates every fixed-position row: a width change strands the right-aligned
            // logo/sparkline, a shrink leaves the old box above the new bottom. Clear + repaint from scratch.
            std::printf("\x1b[2J");
        }
        setRegion();                               // (re)assert the scroll region (DECSTBM homes the cursor)
        sampleRate(rx);                            // RX B/s + sparkline level for this tick
        paintBox(rx, tx, drops, trunc, uptimeSec); // rule + status box at the (current) bottom
        // Cursor blink is time-based (~500 ms), NOT once-per-tick — so a fast 250 ms refresh does not strobe.
        const uint64_t nb = nowMs();
        if (resized)
        {
            _blinkOn = true;
            _lastBlinkMs = nb;
        }
        else if (nb - _lastBlinkMs >= 500)
        {
            _blinkOn = !_blinkOn;
            _lastBlinkMs = nb;
        }
        redrawInput(); // input line + block cursor (restores it after a clear)
        std::fflush(stdout);
    }

    /**
     * @brief Reset the region, leave raw mode, print the closing summary (below the freed screen).
     */
    void end(uint32_t rx, uint32_t tx, uint32_t drops, uint32_t trunc, uint64_t uptimeSec)
    {
        std::printf("\x1b[r");              // release the scroll region
        std::printf("\x1b[?25h");           // show the real cursor again (we hid it for our own block)
        std::printf("\x1b[%d;1H\n", _rows); // park at the very bottom
        leaveRaw();
        Theme& c = _c;
        char dur[16];
        std::snprintf(dur, sizeof(dur), "%u:%02u", (unsigned)(uptimeSec / 60), (unsigned)(uptimeSec % 60));
        std::printf("%s %s   %s %s   %s %s %s   %s %s %s\n",
                    c.dim(_t.glyph("└", "+")).c_str(), c.dim(_i.tr("console closed", "Konsole beendet")).c_str(),
                    c.cyan(_t.glyph("◷", "t")).c_str(), c.txt(std::string(dur) + " min").c_str(),
                    c.mut("·").c_str(), c.cyan("RX").c_str(), c.txt(humanSize(rx)).c_str(),
                    c.mut("·").c_str(), c.green("TX").c_str(), c.txt(humanSize(tx)).c_str());
        if (drops)
            std::printf("  %s %s %s\n", c.amber(_t.glyph("▲", "!")).c_str(),
                        c.dim(_i.tr("discarded tunnel frames:", "verworfene Tunnel-Frames:")).c_str(),
                        c.amber(std::to_string(drops)).c_str());
        if (trunc)
            std::printf("  %s %s %s\n", c.amber(_t.glyph("▲", "!")).c_str(),
                        c.dim(_i.tr("truncated device output (console-ring overflow):",
                                    "truncated Geräte-Ausgabe (Konsolen-Ring-Overflow):"))
                            .c_str(),
                        c.amber(std::to_string(trunc)).c_str());
        if (!_logPath.empty())
            std::printf("  %s %s %s\n", c.green(_t.glyph("✔", "OK")).c_str(),
                        c.dim(_i.tr("session log saved:", "Sitzungs-Log gespeichert:")).c_str(), c.txt(_logPath).c_str());
        std::fflush(stdout);
    }

  private:
    /**********************************************************************
     ************************** INPUT PARSING ****************************
     **********************************************************************/
#ifndef _WIN32
    /**
     * @brief Consume buffered stdin bytes: dispatch arrow escapes, feed ordinary bytes.
     * @details An arrow key is "\x1b[X" (or "\x1bOX"); an incomplete escape waits for more bytes.
     */
    PollResult drain(std::string& out)
    {
        while (!_pend.empty())
        {
            unsigned char c = (unsigned char)_pend[0];
            if (c == 0x1b) // an escape sequence (arrow keys): need "\x1b[X"
            {
                if (_pend.size() < 3) return PollNone; // incomplete — wait for more bytes
                if (_pend[1] == '[' || _pend[1] == 'O')
                {
                    const char a = _pend[2];
                    _pend.erase(0, 3);
                    if (a == 'A') histUp();
                    else if (a == 'B')
                        histDown();
                    else if (a == 'C')
                        moveRight();
                    else if (a == 'D')
                        moveLeft();
                    redrawInput();
                    continue;
                }
                _pend.erase(0, 1); // a bare ESC — drop it
                continue;
            }
            _pend.erase(0, 1);
            PollResult r = feed(c, out);
            if (r != PollNone) return r;
        }
        return PollNone;
    }
#endif

    /**
     * @brief Handle one ordinary (non-arrow) input byte. Returns a terminal result or PollNone.
     */
    PollResult feed(unsigned char c, std::string& out)
    {
        if (c == 0x03) return PollAbort;                  // Ctrl-C
        if (c == 0x13 || c == 0x0c) return PollToggleLog; // Ctrl-S or Ctrl-L -> toggle session logging
        if (c == 0x11) return PollNone;                   // Ctrl-Q (former XON) -> ignore
        if (c == '\r' || c == '\n')                       // submit the line
        {
            out = _buf;
            if (!_buf.empty()) pushHistory(_buf);
            _buf.clear();
            _pos = 0;
            _histIdx = (int)_hist.size();
            redrawInput();
            return PollSubmit;
        }
        if (c == 0x7f || c == 0x08) // backspace
        {
            if (_pos > 0)
            {
                _buf.erase(_pos - 1, 1);
                _pos--;
                redrawInput();
            }
            return PollNone;
        }
        if (c == '?' && _buf.empty()) // bare '?' on an empty line -> LOCAL help (not sent)
            return PollHelp;
        if (c >= 0x20 && c < 0x7f) // printable -> insert at the cursor
        {
            _buf.insert(_buf.begin() + _pos, (char)c);
            _pos++;
            redrawInput();
        }
        return PollNone;
    }

    void moveLeft()
    {
        if (_pos > 0) _pos--;
    }
    void moveRight()
    {
        if (_pos < (int)_buf.size()) _pos++;
    }

    /**********************************************************************
     ************************* COMMAND HISTORY ***************************
     **********************************************************************/
    /**
     * @brief Recall the previous history entry (stashes the half-typed draft on first recall).
     */
    void histUp()
    {
        if (_hist.empty()) return;
        if (_histIdx == (int)_hist.size()) _draft = _buf; // remember the half-typed line before recalling
        if (_histIdx > 0) _histIdx--;
        _buf = _hist[_histIdx];
        _pos = (int)_buf.size();
    }

    /**
     * @brief Recall the next history entry; back at the end restores the stashed draft.
     */
    void histDown()
    {
        if (_histIdx >= (int)_hist.size()) return;
        _histIdx++;
        _buf = (_histIdx == (int)_hist.size()) ? _draft : _hist[_histIdx];
        _pos = (int)_buf.size();
    }

    /**
     * @brief Append a submitted line to history (RAM + file), suppressing a consecutive duplicate in RAM.
     */
    void pushHistory(const std::string& line)
    {
        if (!_hist.empty() && _hist.back() == line)
        {
            appendHistory(line);
            return;
        } // no consecutive dup in RAM
        _hist.push_back(line);
        appendHistory(line);
    }

    /**
     * @brief Load history from the shared file; bound RAM to the last 1000 entries.
     */
    void loadHistory()
    {
        if (_histFile.empty()) return;
        std::ifstream f(_histFile);
        std::string line;
        while (std::getline(f, line))
            if (!line.empty()) _hist.push_back(line);
        // keep memory bounded; the file is trimmed lazily on save
        if (_hist.size() > 1000) _hist.erase(_hist.begin(), _hist.end() - 1000);
        _histIdx = (int)_hist.size();
    }

    /**
     * @brief Append one line to the history file (shared across sessions -> append, don't rewrite).
     */
    void appendHistory(const std::string& line)
    {
        if (_histFile.empty()) return;
        std::ofstream f(_histFile, std::ios::app);
        if (f) f << line << "\n";
    }

    /**********************************************************************
     **************************** RENDERING ******************************
     **********************************************************************/
    /**
     * @brief Redraw the input line ("> <buffer>") + the block cursor.
     */
    void redrawInput()
    {
        Theme& c = _c;
        const int row = inputRow();
        std::printf("\x1b[%d;1H\x1b[2K", row); // input row, clear it
        std::printf("%s%s", c.green("> ").c_str(), c.txt(_buf).c_str());
        drawCursor();
        std::fflush(stdout);
    }

    /**
     * @brief Draw OUR block cursor at the input position (the real terminal cursor is hidden).
     * @details Reverse-video on the "on" phase, the plain cell on "off" — a reliable blink + fixed position in
     *          every terminal (incl. VS Code, where DECSCUSR blink / DECSC-DECRC-over-scroll are unreliable).
     */
    void drawCursor()
    {
        const int row = inputRow();
        const int col = 3 + _pos;
        const char under = (_pos < (int)_buf.size()) ? _buf[_pos] : ' ';
        std::printf("\x1b[%d;%dH", row, col);
        if (_blinkOn) std::printf("\x1b[7m%c\x1b[27m", under); // reverse-video block = the cursor "on"
        else
            std::printf("%c", under);         // "off" phase: the plain cell
        std::printf("\x1b[%d;%dH", row, col); // leave the (hidden) cursor parked here
    }

    /**
     * @brief Paint the rule + status box (2 lines, or 4 in verbose) at the bottom + the logo watermark.
     * @details l1 = identity/via/PAs/uptime/sparkline · l2 = RX/TX/DROP/TRUNC/hints/log · l3 = live stats ·
     *          l4 = steckbriefe. Row _rows stays blank as the logo margin.
     */
    void paintBox(uint32_t rx, uint32_t tx, uint32_t drops, uint32_t trunc, uint64_t up)
    {
        const int cols = _cols;
        Theme& c = _c;
        char dur[16];
        std::snprintf(dur, sizeof(dur), "%u:%02u", (unsigned)(up / 60), (unsigned)(up % 60));
        // Verbose adds l3 (live stats) + l4 (steckbriefe) -> 4 status lines vs. 2. Row _rows stays blank.
        const int boxLines = _verbose ? 5 : 3;     // rule + status lines
        const int bottom = _rows - 1;              // last box row (margin row _rows below stays blank)
        const int rRule = bottom - (boxLines - 1); // rule (top of the box)
        const int rL1 = rRule + 1, rL2 = rL1 + 1, rL3 = rL2 + 1, rL4 = rL3 + 1;
        // Line 1: identity (+ verbose stamp) · via · PAs · uptime · RX sparkline (wider in verbose).
        const std::string mid = _wsMode
                                    ? "  " + c.green(std::string(_t.glyph("●", "*")) + " " + _i.tr("webconsole", "Webkonsole")) + "  " + c.txt(_ip)
                                    : "  " + c.green(std::string(_t.glyph("●", "*")) + " " + _i.tr("connected", "verbunden")) +
                                          "  " + c.dim(_i.tr("via", "über")) + " " + c.txt(_ip) +
                                          " " + c.dim("(as") + " " + c.chip(_tpa, 'c') + c.dim(")") +
                                          " " + c.dim(_t.glyph("→", "->")) + " " + c.chip(_target, 'c'); // PAs as UI-template chips
        std::string l1 = std::string(c.dim(_t.glyph("┌", "+"))) + " " + c.bold(c.green("ftc console").c_str()) +
                         (_verbose ? "  " + c.chip("verbose", 'a') : std::string()) + mid +
                         "   " + c.mut("·") + "   " + c.cyan(_t.glyph("◷", "t")) + " " + c.txt(dur) +
                         "   " + sparkStr(_verbose ? 24 : 14); // live RX-rate sparkline (soft dots; wider in verbose)
        // Line 2: RX · TX · (verbose: peak/avg + Σ total) · DROP/TRUNC · hints · log. Connector ├ in verbose.
        std::string drp = drops ? c.red(std::string(_t.glyph("▲", "!")) + " DROP " + std::to_string(drops))
                                : c.mut(std::string(_t.glyph("▲", "!")) + " DROP 0");
        std::string trc = trunc ? c.amber("TRUNC " + std::to_string(trunc)) : c.mut("TRUNC 0");
        const uint32_t avg = up ? (uint32_t)(rx / up) : 0; // long-run average RX B/s
        std::string l2 = std::string(c.dim(_t.glyph(_verbose ? "├" : "└", "+"))) + " " +
                         c.cyan("RX") + " " + c.txt(humanSize(rx)) + " " + c.mut(std::to_string(_rxRate) + " B/s") +
                         (_verbose ? " " + c.dim("peak " + std::to_string(_sparkPeak) + " avg " + std::to_string(avg)) : std::string()) +
                         "   " + c.green("TX") + " " + c.txt(humanSize(tx)) +
                         (_verbose ? "   " + std::string(c.mut("Σ")) + " " + c.txt(humanSize(rx + tx)) : std::string()) +
                         "   " + drp + "  " + trc +
                         "   " + c.mut("·") + "   " + c.cyan("quit") + c.dim("/") + c.cyan("exit") +
                         "  " + c.cyan("?") + " " + c.dim(_i.tr("help", "Hilfe")) +
                         "  " + c.cyan("↑↓") + " " + c.dim(_i.tr("history", "Verlauf")) +
                         "  " + c.cyan("^L") + " " + logIndicator() +
                         (_jobCount > 0 ? "   " + std::string(c.mut("·")) + "   " + c.green(_t.glyph("⟳", "~")) + " " +
                                              c.txt(std::to_string(_jobCount)) + " " + c.dim(_i.tr("jobs", "Jobs"))
                                        : std::string());
        if (!_verbose && !_logPath.empty()) // in verbose the log path lives on l3 instead
            l2 += "   " + std::string(c.mut("·")) + "   " + c.cyan(_t.glyph("✎", "log")) + " " + c.dim(_logPath);
        // Full-width horizontal rule above the box.
        std::string rule;
        for (int i = 0; i < cols; ++i)
            rule += _t.glyph("─", "-");
        std::printf("\x1b[%d;1H\x1b[2K%s", rRule, c.dim(rule).c_str());
        std::printf("\x1b[%d;1H\x1b[2K%s", rL1, l1.c_str());
        std::printf("\x1b[%d;1H\x1b[2K%s", rL2, l2.c_str());
        if (_verbose)
        {
            // Line 3: last cmd + round-trip (min/avg/max) · drain · APDU · tunnel health · busy · log.
            const uint32_t rttAvg = _rttN ? _rttSum / _rttN : 0;
            std::string rttStats = _rttN ? c.dim(" (" + std::to_string(_rttMin) + "/" + std::to_string(rttAvg) + "/" + std::to_string(_rttMax) + ")")
                                         : std::string();
            const std::string mid3 = _wsMode
                                         ? "   " + std::string(c.mut("·")) + "   " + c.dim(_wsUrl) // WS: no tunnel/APDU/drain
                                         : "   " + std::string(c.mut("·")) + "   " + c.mut("drain") + " " + c.txt(std::to_string(_drain) + " B/ans") +
                                               "   " + c.mut("APDU") + " " + c.txt(std::to_string(_apdu)) +
                                               "   " + c.mut("·") + "   " + c.mut("ch") + " " + c.txt(std::to_string(_chan)) +
                                               " " + c.mut("seq") + " " + c.txt(std::to_string(_txSeq) + "/" + std::to_string(_rxSeq));
            std::string l3 = std::string(c.dim(_t.glyph("├", "+"))) + " " +
                             c.mut(_i.tr("last", "letztes")) + " " +
                             (_lastCmd.empty() ? c.dim(_t.glyph("—", "-")) : c.txt(_lastCmd)) +
                             " " + c.green(std::to_string(_lastRttMs) + " ms") + rttStats + mid3 +
                             "  " + (_awaitRtt ? c.amber(std::string(_t.glyph("●", "*")) + " busy") : c.dim(std::string(_t.glyph("○", "o")) + " idle")) +
                             (_logPath.empty() ? std::string()
                                               : "   " + std::string(c.mut("·")) + "   " + c.cyan(_t.glyph("✎", "log")) + " " + c.dim(_logPath));
            std::printf("\x1b[%d;1H\x1b[2K%s", rL3, l3.c_str());
            // Line 4: steckbriefe — WS mode has only the interface (it IS the endpoint); tunnel mode has tgt + iface.
            std::string l4 = _wsMode
                                 ? std::string(c.dim(_t.glyph("└", "+"))) + " " + c.mut(_i.tr("interface", "Interface")) + " " + c.txt(_ip) +
                                       (_ifName.empty() ? "" : "  " + c.dim(_ifName)) + (_ifMask.empty() ? "" : " " + c.dim(_ifMask))
                                 : std::string(c.dim(_t.glyph("└", "+"))) + " " +
                                       c.mut("tgt") + " " + c.chip(_target, 'c') +
                                       (_tgtMask.empty() ? "" : " " + c.dim(_tgtMask)) +
                                       (_tgtCls.empty() ? "" : " " + c.dim(_tgtCls)) +
                                       (_tgtFw.empty() ? "" : "  " + std::string(c.mut("fw")) + " " + c.txt(_tgtFw)) +
                                       (_tgtFeat.empty() ? "" : "  " + c.green(_tgtFeat)) +
                                       "     " + c.mut("iface") + " " + c.chip(_tpa, 'c') +
                                       (_ifName.empty() ? "" : " " + c.dim(_ifName)) +
                                       (_ifMask.empty() ? "" : " " + c.dim(_ifMask)) +
                                       (_slotsFree < 0 ? "" : "  " + std::string(c.mut("slots")) + " " + c.txt(std::to_string(_slotsFree) + "/" + std::to_string(_slotsTotal)));
            std::printf("\x1b[%d;1H\x1b[2K%s", rL4, l4.c_str());
        }
        printLogo(); // OpenKNX watermark in the bottom-right corner (over the right end of the status rows)
    }

    /**
     * @brief Draw the OpenKNX logo watermark, fixed in the bottom-right corner over the status rows.
     * @details Positioned (not scrolled) + repainted each tick -> resize-safe. Skipped on a narrow terminal.
     */
    void printLogo()
    {
        if (_cols < 100) return; // too narrow -> skip (keep the status line clean)
        Theme& c = _c;
        const int col = _cols - 8; // right corner, ~7 cols for the mark
        const std::string nd = c.green(_t.glyph("■", "#"));
        std::printf("\x1b[%d;%dH%s %s", _rows - 2, col, c.txt("Open").c_str(), nd.c_str());        // l1 row (Open in white)
        std::printf("\x1b[%d;%dH%s", _rows - 1, col, c.dim(_t.glyph("┬────┴", "+----+")).c_str()); // l2 row
        std::printf("\x1b[%d;%dH%s %s", _rows, col, nd.c_str(), c.txt("KNX").c_str());             // bottom row H (KNX in white)
    }

    /**********************************************************************
     ***************************** HELPERS *******************************
     **********************************************************************/
    static uint64_t nowMs()
    {
        return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    static std::string humanSize(uint32_t v)
    {
        char b[24];
        if (v >= 1048576u) std::snprintf(b, sizeof(b), "%.2f MB", v / 1048576.0);
        else if (v >= 1024u)
            std::snprintf(b, sizeof(b), "%.1f KB", v / 1024.0);
        else
            std::snprintf(b, sizeof(b), "%u B", (unsigned)v);
        return b;
    }

    /**
     * @brief Sample the RX rate (B/s) since the last tick + push one sparkline level (0..7, scaled to peak).
     */
    void sampleRate(uint32_t rx)
    {
        const uint64_t now = nowMs();
        if (_prevRateMs && now > _prevRateMs && rx >= _prevRx)
            _rxRate = (uint32_t)((uint64_t)(rx - _prevRx) * 1000u / (now - _prevRateMs));
        _prevRx = rx;
        _prevRateMs = now;
        if (_rxRate > _sparkPeak) _sparkPeak = _rxRate; // running peak scales the bars
        uint8_t lvl = _sparkPeak ? (uint8_t)((uint64_t)_rxRate * 7u / _sparkPeak) : 0;
        if (lvl > 7) lvl = 7;
        const int CAP = (int)sizeof(_spark);
        if (_sparkN < CAP) _spark[_sparkN++] = lvl;
        else
        {
            for (int i = 1; i < CAP; ++i)
                _spark[i - 1] = _spark[i];
            _spark[CAP - 1] = lvl;
        }
    }

    /**
     * @brief Render the last @p n sparkline levels via the UI-template spark() ('d' soft-dots style).
     */
    std::string sparkStr(int n) const
    {
        int start = _sparkN > n ? _sparkN - n : 0;
        std::vector<double> v;
        for (int i = start; i < _sparkN; ++i)
            v.push_back((double)(_spark[i] & 7));
        return Tpl(_t, _c, &_i).spark(v, 7.0, 0, 'd'); // levels are 0..7 -> vmax 7; dots, not bars
    }

    /**
     * @brief Status-bar log indicator: dim "log" when off; an animated braille spinner + "logging" when on.
     */
    std::string logIndicator()
    {
        if (_logPath.empty()) return _c.dim(_i.tr("log", "Log"));
        static const char* U[10] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
        static const char* A[4] = {"|", "/", "-", "\\"};
        const std::string fr = _t.ascii() ? A[_spin % 4] : U[_spin % 10];
        ++_spin;
        return _c.green(fr + " " + _i.tr("logging", "Log an"));
    }

    // Rows reserved at the bottom (region = 1..H-reserved): 6 normally, 8 in verbose (2 extra status lines).
    int reservedRows() const { return _verbose ? 8 : 6; }
    int inputRow() const { return _rows - (_verbose ? 6 : 4); }             // input line sits just above the rule
    void setRegion() { std::printf("\x1b[1;%dr", _rows - reservedRows()); } // top margin 1 keeps scrollback; row H stays blank

    /**
     * @brief Query the terminal geometry; fall back to 80x24 when it cannot be read.
     */
    void size(int& rows, int& cols) const
    {
#ifdef _WIN32
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi))
        {
            cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
            rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
            if (rows > 8 && cols > 0) return;
        }
#else
        struct winsize ws;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 8)
        {
            cols = ws.ws_col;
            rows = ws.ws_row;
            return;
        }
#endif
        rows = 24;
        cols = 80;
    }

    /**********************************************************************
     ***************************** RAW MODE ******************************
     **********************************************************************/
    /**
     * @brief Enter raw terminal mode (no canonical buffering / echo; XON/XOFF off so Ctrl-S/Q reach us).
     */
    void enterRaw()
    {
#ifdef _WIN32
        HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
        GetConsoleMode(h, &_winMode);
        SetConsoleMode(h, _winMode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT));
#else
        tcgetattr(STDIN_FILENO, &_saved);
        struct termios raw = _saved;
        raw.c_lflag &= ~(ICANON | ECHO); // no canonical buffering, no echo (we echo ourselves)
        raw.c_iflag &= ~(IXON | IXOFF);  // disable XON/XOFF so Ctrl-S/Ctrl-Q reach us as bytes
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
#endif
        _rawOn = true;
    }

    /**
     * @brief Restore the saved terminal mode. Idempotent.
     */
    void leaveRaw()
    {
        if (!_rawOn) return;
#ifdef _WIN32
        SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), _winMode);
#else
        tcsetattr(STDIN_FILENO, TCSANOW, &_saved);
#endif
        _rawOn = false;
    }

    Term& _t;
    Theme& _c;
    I18n& _i;
    int _rows = 24, _cols = 80; // terminal geometry, fixed at begin() so every row calc agrees
    std::string _ip, _tpa, _target, _logPath, _histFile;
    std::string _buf; // current input line
    int _pos = 0;     // cursor index within _buf
    std::vector<std::string> _hist;
    int _histIdx = 0;                                            // recall cursor; == _hist.size() means "the live draft"
    std::string _draft;                                          // half-typed line stashed when recalling history
    bool _blinkOn = true;                                        // manual block-cursor blink phase (toggled ~every 500 ms, independent of tick rate)
    uint64_t _lastBlinkMs = 0;                                   // last time the cursor blink toggled (keeps a normal blink even at a 250 ms tick)
    uint8_t _spin = 0;                                           // spinner phase for the "logging" indicator (advanced per tick while logging)
    bool _verbose = false;                                       // verbose (-V): adds the l3 status line + a wider RX sparkline
    std::string _lastCmd;                                        // last command sent to the device (for the verbose l3 line)
    uint64_t _lastCmdMs = 0;                                     // when it was sent (for the round-trip estimate)
    uint32_t _lastRttMs = 0;                                     // round-trip to the first device output after the last command
    bool _awaitRtt = false;                                      // a command is in flight, waiting for the first response line
    uint16_t _drain = 0, _apdu = 0;                              // console drain cap (B/answer) + detected interface max APDU (verbose l3)
    uint32_t _rttMin = 0, _rttMax = 0, _rttSum = 0, _rttN = 0;   // round-trip stats (verbose l3)
    uint8_t _chan = 0, _txSeq = 0, _rxSeq = 0;                   // tunnel-health snapshot, fed by tick() (verbose l3)
    std::string _ifOrder, _ifName, _tgtOrder, _tgtCls, _tgtFeat; // steckbrief text (verbose l4)
    std::string _ifMask, _tgtMask, _tgtFw;
    int _slotsFree = -1, _slotsTotal = 0; // free/total tunnel slots (-1 = unknown/not probed)
    int _jobCount = 0;                    // active /every auto-commands -> ⟳ badge in the status bar
    bool _wsMode = false;                 // WebSocket console (ftc -i <ip> con): no tunnel/APDU/drain
    std::string _wsUrl;                   // ws://<ip>/console (shown in the verbose line / stats)
    uint32_t _prevRx = 0;
    uint64_t _prevRateMs = 0;
    uint32_t _rxRate = 0; // RX B/s, sampled between ticks
    uint8_t _spark[28] = {0};
    int _sparkN = 0;
    uint32_t _sparkPeak = 1; // RX-rate sparkline ring + running peak
#ifndef _WIN32
    std::string _pend; // partially-read input bytes (incomplete escape sequences)
    struct termios _saved{};
#else
    DWORD _winMode = 0;
#endif
    bool _rawOn = false;
};

} // namespace ftc

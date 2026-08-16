/**
 * @file        Templates.h
 * @brief       Reusable console output vocabulary — the phosphor-mock line templates every ftc command draws from
 * @details     One place owns HOW an ftc line looks, so every command draws from the SAME building blocks
 *              instead of ad-hoc printf's. Everything renders through Theme (colour) + Term (glyph/ascii +
 *              colour on/off), so a pipe / NO_COLOR / --ascii sink degrades cleanly and DE/EN never changes
 *              the shape. Header-only. Presentation ONLY — no I/O, no KNX logic.
 * @date        2026-08-03
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

#ifdef _WIN32
    #include <io.h>
    #include <windows.h>
#else
    #include <sys/ioctl.h>
    #include <unistd.h>
#endif

#include "I18n.h"
#include "Term.h"
#include "Theme.h"

namespace ftc
{

/**
 * @brief The reusable presentation templates. Construct once over the shared Term + Theme, then draw.
 * @details i18n is optional and only steers the decimal separator (DE comma / EN dot) in progress percentages.
 */
class Tpl
{
  public:
    Tpl(Term& term, Theme& theme, I18n* i18n = nullptr) : _t(term), _c(theme), _i(i18n) {}

    /** @brief Translate a template's own words. Falls back to English when no I18n was handed in. */
    const char* tr(const char* en, const char* de) const { return _i ? _i->tr(en, de) : en; }

    /**
     * @brief A status kind — picks the leading dot glyph + its colour. Meaning is fixed (never theme-swaps).
     */
    enum class Stat
    {
        Ok,   ///< green ● — done / up / reachable
        Warn, ///< amber ● — degraded / caution
        Err,  ///< red ●   — failed / refused
        Info, ///< cyan ●  — neutral notice
        Idle  ///< mut ○   — off / absent / not probed
    };

    /*********************************************************************
     ************************* TERMINAL GEOMETRY *************************
     ********************************************************************/

    /**
     * @brief Current terminal width in columns (default 80 when not a tty / unknown).
     */
    static int cols()
    {
#ifdef _WIN32
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi))
        {
            const int w = csbi.srWindow.Right - csbi.srWindow.Left + 1;
            if (w > 0) return w;
        }
#else
        struct winsize ws;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return (int)ws.ws_col;
#endif
        return 80;
    }

    /**
     * @brief Visible column count of a plain (un-coloured) UTF-8 string.
     * @details Counts codepoints, not bytes, so the box glyphs (┌ ─ · ●, all width-1) measure correctly.
     *          Assumes no wide/zero-width chars (we use none).
     */
    static int dispw(const std::string& s)
    {
        int n = 0;
        for (unsigned char ch : s)
            if ((ch & 0xC0) != 0x80) ++n; // count non-continuation bytes = codepoints
        return n;
    }

    /*********************************************************************
     *********************** STATUS LINE & PANELS ************************
     ********************************************************************/

    /**
     * @brief `● <label>  <seg>  <seg>` — a dotted status headline.
     * @details label takes the stat colour; segments are dimmed detail, joined by two spaces. The workhorse
     *          for "tunnel up", identity lines, ok/fail.
     */
    void status(Stat s, const std::string& label, std::initializer_list<std::string> segs = {}) const
    {
        std::printf("  %s %s", dot(s).c_str(), labelColor(s, label).c_str());
        for (const auto& seg : segs)
            if (!seg.empty()) std::printf("  %s", _c.dim(seg).c_str());
        std::printf("\n");
    }

    /**
     * @brief Open a panel: `┌─ <title> · <subtitle> ───────…` filled to the panel width.
     * @details title in amber, the frame + subtitle dimmed. Follow with kv() rows and close with panelEnd().
     */
    void panelTop(const std::string& title, const std::string& subtitle = "") const
    {
        const int w = panelWidth();
        std::string head = std::string(_t.glyph("┌─ ", "+- ")) + title;
        if (!subtitle.empty()) head += " · " + subtitle;
        head += " ";
        const int pad = w - dispw(head);
        std::string rule;
        for (int i = 0; i < (pad > 0 ? pad : 0); ++i)
            rule += _t.glyph("─", "-");
        // colour the pieces: frame + subtitle dim, title amber.
        std::string out = _c.dim(_t.glyph("┌─ ", "+- ")) + _c.amber(title);
        if (!subtitle.empty()) out += _c.dim(" · " + subtitle);
        out += " " + _c.dim(rule);
        std::printf("\n  %s\n", out.c_str());
    }

    /**
     * @brief One key/value row inside a panel: a dim, left-padded label then the value (caller pre-colours).
     */
    void kv(const std::string& label, const std::string& value) const
    {
        // Pad on DISPLAY width, not bytes: a German label with an umlaut ("Blöcke", "Identität") is one
        // byte longer than it is wide, and %-14s would short-pad it by exactly that much.
        std::string lbl = label;
        for (int n = dispw(label); n < 14; ++n)
            lbl += ' ';
        std::printf("    %s%s\n", _c.dim(lbl).c_str(), value.c_str());
    }

    /**
     * @brief `? <label>   <value>` — an input line the caller has already filled or masked.
     * @details The prompt shape knxOTA needs for a password or a typed address: amber marker, dim label in
     *          the same 14-column gutter kv() uses, then the value. Drawing it here keeps the prompt inside
     *          the template vocabulary instead of being a bare printf next to it.
     */
    void promptLine(const std::string& label, const std::string& value, const std::string& hint = "") const
    {
        std::string lbl = label;
        for (int n = dispw(label); n < 14; ++n)
            lbl += ' ';
        std::printf("  %s %s%s", _c.amber("?").c_str(), _c.dim(lbl).c_str(), value.c_str());
        if (!hint.empty()) std::printf("   %s", _c.mut(hint).c_str());
        std::printf("\n");
    }

    /**
     * @brief `◉ <label>   0:42   <detail>` — a live wait, redrawn in place while something outside happens.
     * @details For the waits knxOTA cannot shorten: a human walking to the cabinet to press the programming
     *          button, or a device rebooting into new firmware. The marker blinks off the caller's elapsed
     *          time rather than a spinner, so a frozen screen is visibly distinct from a slow one. Prints
     *          with a carriage return on a tty (one line, updated) and once per call otherwise.
     */
    void waitTick(const std::string& label, uint32_t elapsedS, const std::string& detail = "") const
    {
        const bool lit = (elapsedS % 2) == 0;
        char clock[16];
        std::snprintf(clock, sizeof(clock), "%u:%02u", (unsigned)(elapsedS / 60), (unsigned)(elapsedS % 60));
        const std::string mark = lit ? _c.amber(_t.glyph("◉", "*")) : _c.mut(_t.glyph("○", "o"));
        std::string line = "  " + mark + " " + _c.txt(label) + "   " + _c.bold(clock);
        if (!detail.empty()) line += "   " + _c.dim(detail);
        if (_t.isTty()) std::printf("\r%s\x1b[K", line.c_str());
        else
            std::printf("%s\n", line.c_str());
        std::fflush(stdout);
    }

    /**
     * @brief Close a panel: `└──────────…` matching the panel width.
     */
    void panelEnd() const
    {
        const int w = panelWidth();
        std::string rule = _t.glyph("└", "+");
        for (int i = 1; i < w; ++i)
            rule += _t.glyph("─", "-");
        std::printf("  %s\n", _c.dim(rule).c_str());
    }

    /**
     * @brief A half-open section rule: `── <title> ───────…` (green). Groups the output that follows it.
     * @param width 0 = capped auto (default) · <0 = FILL the terminal (dynamic wide) · >0 = fixed columns.
     */
    void section(const std::string& title, int width = 0) const
    {
        int w;
        if (width > 0) w = width;
        else if (width < 0)
            w = cols() - 2; // fill the terminal (dynamic wide)
        else
            w = panelWidth(); // capped auto
        std::string head = std::string(_t.glyph("── ", "-- ")) + title + " ";
        const int pad = w - dispw(head);
        std::string rule;
        for (int i = 0; i < (pad > 0 ? pad : 0); ++i)
            rule += _t.glyph("─", "-");
        std::printf("\n  %s\n", _c.green(head + rule).c_str());
    }

    /*********************************************************************
     ************************** CHIPS & COLOUR ***************************
     ********************************************************************/

    /**
     * @brief A coloured inline tag (see Theme::chip). Returned as a string so it can sit inside any row.
     */
    std::string chip(const std::string& text, char kind = 'g') const { return _c.chip(text, kind); }

    /**
     * @brief Paint a string in one of the named hues. 'g' green · 'c' cyan · 'a' amber · 'o' gold · 'r' red.
     * @details The single hue selector the transfer-mode colours ride on (mock: safe=green, fast=cyan)
     *          so bars, chips and labels of one mode all agree.
     */
    std::string paint(char color, const std::string& s) const
    {
        switch (color)
        {
            case 'c': return _c.cyan(s);
            case 'a': return _c.amber(s);
            case 'o': return _c.gold(s);
            case 'r': return _c.red(s);
            default: return _c.green(s);
        }
    }

    /**
     * @brief The fill hue for an FTC transfer mode name (mock palette). safe->g · fast->c.
     */
    static char modeColor(const std::string& mode)
    {
        if (mode == "fast") return 'c';
        return 'g'; // safe / default
    }

    /*********************************************************************
     ************************** BARS & PROGRESS **************************
     ********************************************************************/

    /**
     * @brief A block gauge `▓▓▓▓▓░░░░░` for a 0..1 fraction, `width` cells; fill green, track dim.
     * @details Returned as a string (no newline) so callers append a percentage / label. Used for df usage +
     *          transfer progress. `style` picks the glyph pair — 'b' block █/░ (bold, default) · 'l' line ━/─
     *          (slim, least dominant) · 'p' pill ▰/▱ (medium). The slimmer styles read as a gauge without
     *          dominating the line.
     */
    std::string bar(double frac, int width = 20, char color = 'g', char style = 'b') const
    {
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        const char* fu;
        const char* tu;
        const char* fa;
        const char* ta;
        barGlyphs(style, fu, tu, fa, ta);
        const int fill = (int)(frac * width + 0.5);
        std::string on, off;
        for (int i = 0; i < fill; ++i)
            on += _t.ascii() ? fa : fu;
        for (int i = fill; i < width; ++i)
            off += _t.ascii() ? ta : tu;
        return paint(color, on) + _c.mut(off); // fill hue = transfer mode; mut track = real dark colour
    }

    /**
     * @brief A live, in-place transfer line: `⠹ ↑ file  ▓▓▓▒░░  63,20%  · detail  ▂▃▅▇`.
     * @details The spinner turns on every call (so it animates even while `frac` is momentarily flat) and the
     *          bar carries a bright leading-edge cell. IMPORTANT: the percentage is CLAMPED to 99,99 % until
     *          `done` is true — a transfer only reads 100 % once it is actually confirmed (last chunk + CRC),
     *          never a moment before. On `done` the line finalises with a green ✔ and a newline.
     *          dir: '^' upload (green ↑) · 'v' download (cyan ↓) · 0 none. detail/spark are pre-built by the
     *          caller. On a non-tty sink nothing is drawn per frame (no \r spam); only the final `done` line is
     *          emitted. `color` sets the fill hue = the transfer mode (safe=g · fast=c). `label` is
     *          used verbatim, so the caller colours it (e.g. a mode chip + filename). `barStyle` picks the bar
     *          glyphs like bar(): 'b' block · 'l' line · 'p' pill.
     */
    void progress(double frac, bool done, char dir, const std::string& label, const std::string& detail = "",
                  const std::string& spark = "", char color = 'g', char barStyle = 'b') const
    {
        if (!_t.isTty() && !done) return; // a pipe / log gets only the final line, not every frame
        double f = frac < 0 ? 0 : (frac > 1 ? 1 : frac);
        const int W = 14;
        const int fill = done ? W : (int)(f * W);
        std::string head = done ? paint(color, _t.glyph("✔", "OK")) : _c.cyan(spinnerFrame());
        std::string arrow;
        if (dir == '^') arrow = paint(color, _t.glyph("↑", "^")) + " ";
        else if (dir == 'v')
            arrow = paint(color, _t.glyph("↓", "v")) + " ";
        const char* fu;
        const char* tu;
        const char* fa;
        const char* ta;
        barGlyphs(barStyle, fu, tu, fa, ta);
        const char* fg = _t.ascii() ? fa : fu; // fill glyph
        const char* tg = _t.ascii() ? ta : tu; // track glyph
        std::string b;
        for (int i = 0; i < W; ++i)
        {
            if (i < fill - 1) b += paint(color, fg);
            else if (i == fill - 1)
                b += _c.bold(paint(color, fg)); // bold leading edge
            else
                b += _c.mut(tg); // mut = real dark colour
        }
        std::string line = "  " + head + " " + arrow + label + "  " + b + "  " + _c.bold(pct(f, done));
        if (!detail.empty()) line += _c.dim("  · " + detail);
        if (!spark.empty()) line += "  " + spark;
        if (_t.isTty()) std::printf("\r%s\x1b[K", line.c_str()); // in-place: carriage-return + erase to EOL
        else
            std::printf("%s", line.c_str()); // pipe: plain final line (colours already off)
        if (done) std::printf("\n");
        std::fflush(stdout);
    }

    /*********************************************************************
     ************************ SPARKLINES & GRAPHS ************************
     ********************************************************************/

    /**
     * @brief A compact `▁▂▃▄▅▆▇█` graph of a value series (e.g. the live transfer rate), scaled to `vmax`.
     * @details vmax 0 = auto-scale to the series max. Green, returned as a string so it rides inside a progress
     *          line. `width` 0 = one cell per value (raw); >0 = resample the series to `width` cells
     *          (bucket-average) so a long history renders WIDER at a fixed size — used for the end-of-transfer
     *          history graph. `style`: 'b' bars ▁▂▃▄▅▆▇█ (default) · 'd' dots ⣀…⣿ (soft, least dominant) ·
     *          'l' line ⎽…⎺ (subtle baseline).
     */
    std::string spark(const std::vector<double>& v, double vmax = 0, int width = 0, char style = 'b') const
    {
        static const char* barsU[8] = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
        static const char* dotsU[8] = {"⣀", "⣀", "⣄", "⣤", "⣦", "⣶", "⣷", "⣿"};
        static const char* lineU[8] = {"⎽", "⎽", "⎼", "⎼", "⎻", "⎻", "⎺", "⎺"};
        static const char* aramp[8] = {".", ".", ":", "-", "=", "+", "*", "#"};
        const char** ramp = style == 'd' ? dotsU : style == 'l' ? lineU
                                                                : barsU;
        if (v.empty()) return "";
        // resample to `width` buckets (average) when asked and the series is longer than the target.
        std::vector<double> r;
        if (width > 0 && (int)v.size() > width)
        {
            r.assign(width, 0);
            for (int i = 0; i < width; ++i)
            {
                const size_t a = (size_t)i * v.size() / width;
                const size_t b = (size_t)(i + 1) * v.size() / width;
                double sum = 0;
                size_t n = 0;
                for (size_t j = a; j < b && j < v.size(); ++j)
                {
                    sum += v[j];
                    ++n;
                }
                r[i] = n ? sum / n : v[a < v.size() ? a : v.size() - 1];
            }
        }
        else
            r = v;
        double mx = vmax;
        if (mx <= 0)
            for (double x : r)
                if (x > mx) mx = x;
        if (mx <= 0) mx = 1;
        std::string s;
        for (double x : r)
        {
            int idx = (int)((x / mx) * 7 + 0.5);
            if (idx < 0) idx = 0;
            if (idx > 7) idx = 7;
            s += _t.ascii() ? aramp[idx] : ramp[idx];
        }
        return _c.green(s);
    }

    /**
     * @brief A multi-row dotted "scope" graph of a value series — `rows` lines tall, `width` cells wide.
     * @details Resampled/left-padded so the trace grows in from the right, scaled to `vmax` (0 = auto).
     *          Returns the rows top-to-bottom, green phosphor dots (⣀…⣿), ASCII fallback. The live throughput
     *          curve for a transfer.
     */
    std::vector<std::string> sparkRows(const std::vector<double>& v, int width, int rows, double vmax = 0, double vmin = 0) const
    {
        std::vector<std::string> out;
        if (rows < 1 || width < 1) return out;
        std::vector<double> r;
        if ((int)v.size() >= width) // resample to width (bucket average)
        {
            r.assign(width, 0);
            for (int i = 0; i < width; ++i)
            {
                const size_t a = (size_t)i * v.size() / width, b = (size_t)(i + 1) * v.size() / width;
                double s = 0;
                size_t n = 0;
                for (size_t j = a; j < b && j < v.size(); ++j)
                {
                    s += v[j];
                    ++n;
                }
                r[i] = n ? s / n : (a < v.size() ? v[a] : 0.0);
            }
        }
        else // left-pad with zeros so a short history sits at the right edge and the trace fills leftward
        {
            r.assign((size_t)width - v.size(), 0.0);
            for (double x : v)
                r.push_back(x);
        }
        // Auto-zoom band [lo,hi]: mapping to the data's own range (not 0..max) turns a steady signal with small
        // jitter into a visible curve instead of a flat line. vmax<=0 -> auto peak; vmin<=0 -> baseline 0.
        double hi = vmax;
        if (hi <= 0)
            for (double x : r)
                if (x > hi) hi = x;
        if (hi <= 0) hi = 1;
        double lo = vmin;
        if (lo < 0) lo = 0;
        if (lo >= hi) lo = 0;
        const double span = (hi - lo) > 1e-9 ? (hi - lo) : hi;
        static const char* dots[8] = {"⣀", "⣀", "⣄", "⣤", "⣦", "⣶", "⣷", "⣿"};
        static const char* adots[8] = {".", ".", ":", ":", "-", "=", "+", "#"};
        const char** ramp = _t.ascii() ? adots : dots;
        for (int row = 0; row < rows; ++row) // row 0 = top
        {
            const double rlo = (double)(rows - 1 - row) / rows, rhi = (double)(rows - row) / rows; // this row spans [rlo,rhi)
            std::string line;
            for (double x : r)
            {
                double f = (x - lo) / span; // normalise into the band
                if (f < 0) f = 0;
                if (f > 1) f = 1;
                if (f >= rhi) line += ramp[7];
                else if (f <= rlo)
                    line += " ";
                else
                {
                    int idx = (int)(((f - rlo) / (rhi - rlo)) * 7 + 0.5);
                    if (idx < 0) idx = 0;
                    if (idx > 7) idx = 7;
                    line += ramp[idx];
                }
            }
            out.push_back(_c.green(line));
        }
        return out;
    }

    /**
     * @brief A wide, labelled history graph line: the whole series resampled to `width` cells + an avg/peak
     *        annotation. For the recap after a transfer ("here is how the throughput looked over the whole run").
     */
    void graph(const std::string& label, const std::vector<double>& v, int width = 40, const char* unit = "KB/s",
               char style = 'b') const
    {
        if (v.empty()) return;
        double sum = 0, peak = 0;
        for (double x : v)
        {
            sum += x;
            if (x > peak) peak = x;
        }
        const double avg = sum / (double)v.size();
        char note[64];
        std::snprintf(note, sizeof(note), "avg %.0f · peak %.0f %s", avg, peak, unit);
        std::printf("    %s %s  %s\n", _c.dim(label).c_str(), spark(v, peak, width, style).c_str(), _c.dim(note).c_str());
    }

    /*********************************************************************
     ************************* STATUS INDICATORS *************************
     ********************************************************************/

    /**
     * @brief One LED: lit -> a filled ◉ in the given hue, dark -> a dim ○. Mirrors the device Info-LED.
     * @details A console view can show the same busmon / activity blink. color: 'g' green · 'a' amber ·
     *          'r' red · 'c' cyan.
     */
    std::string led(bool on, char color = 'g') const
    {
        const char* g = _t.glyph(on ? "◉" : "○", on ? "*" : "o");
        if (!on) return _c.mut(g);
        switch (color)
        {
            case 'a': return _c.amber(g);
            case 'r': return _c.red(g);
            case 'c': return _c.cyan(g);
            default: return _c.bright(g);
        }
    }

    /**
     * @brief A row of `total` LEDs with the first `lit` on — e.g. the active tunnel count `◉ ◉ ◉ ○ ○`.
     */
    std::string ledRow(int lit, int total, char color = 'g') const
    {
        std::string s;
        for (int i = 0; i < total; ++i)
        {
            s += led(i < lit, color);
            if (i + 1 < total) s += " ";
        }
        return s;
    }

    /**
     * @brief The device programming-mode badge — the same "Prog mode" indicator the interface panel shows.
     * @details Returned as a string so it works INLINE too (in a status segment, a kv value, or on its own
     *          line). off -> a dim "○ off". on -> an amber "● ON". For the blink, drive `phaseOn` from a frame
     *          counter (k % 2): the ON dot then alternates amber / dark while the "ON" label stays lit.
     */
    std::string progMode(bool on, bool phaseOn = true) const
    {
        if (!on) return _c.mut(std::string(_t.glyph("○", "o")) + " " + tr("off", "aus"));
        const std::string dot = phaseOn ? _c.amber(_t.glyph("●", "*")) : _c.mut(_t.glyph("●", "*"));
        return dot + " " + _c.amber(tr("ON", "AN"));
    }

    /**
     * @brief A KNX individual address `a.l.d` from the packed 16-bit form (area/line/device nibbles+byte).
     */
    static std::string pa(uint16_t ia)
    {
        char b[16];
        std::snprintf(b, sizeof(b), "%u.%u.%u", (ia >> 12) & 0x0F, (ia >> 8) & 0x0F, ia & 0xFF);
        return b;
    }

    /**
     * @brief A solid status dot `●` in a traffic-light hue, for the status column of a scan/result table.
     * @details 'g' green = ok/responded · 'a' amber = responded but no/partial info · 'r' red = read
     *          error/timeout.
     */
    std::string statusDot(char color) const { return paint(color, _t.glyph("●", color == 'r' ? "x" : (color == 'a' ? "!" : "*"))); }

    /**
     * @brief A KNX group-service tag chip for a monitor line.
     * @details 'W' GroupValueWrite (cyan) · 'R' GroupValueRead (gold) · 'r' GroupValueResponse (green). Keeps
     *          the service colour identical across group + bus monitor.
     */
    std::string svcTag(char s) const
    {
        switch (s)
        {
            case 'W': return chip("Write", 'c');
            case 'R': return chip("Read", 'o');
            case 'r': return chip("Resp", 'g');
            default: return chip("?", 'r');
        }
    }

    /**
     * @brief Visible width of a possibly-coloured string (ANSI stripped, codepoints counted). For alignment.
     */
    static int vis(const std::string& s) { return dispw(stripForWidth(s)); }

    /*********************************************************************
     **************************** CLIP & WRAP ****************************
     ********************************************************************/

    /**
     * @brief Truncate a possibly-coloured string to `width` VISIBLE columns, appending an ellipsis on overflow.
     * @details ANSI-safe: escape sequences are copied whole (never counted, never cut mid-sequence) and a reset
     *          is emitted before the ellipsis so colour never bleeds. The tool for "one line, must fit the width".
     */
    std::string clip(const std::string& s, int width) const
    {
        if (width <= 0) return "";
        std::string o;
        int visc = 0;
        bool truncated = false;
        for (size_t i = 0; i < s.size();)
        {
            if (s[i] == '\x1b') // copy a whole CSI escape verbatim
            {
                size_t j = i + 1;
                if (j < s.size() && s[j] == '[')
                {
                    ++j;
                    while (j < s.size() && !((unsigned char)s[j] >= 0x40 && (unsigned char)s[j] <= 0x7e))
                        ++j;
                    if (j < s.size()) ++j;
                }
                o.append(s, i, j - i);
                i = j;
                continue;
            }
            if (visc >= width - 1)
            {
                truncated = true;
                break;
            } // leave 1 col for the ellipsis
            unsigned char ch = s[i];
            size_t len = ch >= 0xF0 ? 4 : ch >= 0xE0 ? 3
                                      : ch >= 0xC0   ? 2
                                                     : 1; // one UTF-8 codepoint
            o.append(s, i, len);
            i += len;
            ++visc;
        }
        if (truncated)
        {
            if (_t.useColor()) o += _t.reset();
            o += _t.glyph("…", ">");
        }
        return o;
    }

    /**
     * @brief Reflow a PLAIN (un-coloured) string to `width` columns, breaking on spaces, with a hanging indent.
     * @details Prints one or more lines. For wide raw dumps (an extended-frame LPDU hex) that must not be lost
     *          on a narrow / resized terminal — every byte stays, it just continues on the next line.
     */
    void wrap(const std::string& text, int width, int indent = 4) const
    {
        const std::string pad(indent, ' ');
        std::string cur = pad;
        int col = indent;
        size_t i = 0;
        bool anyOnLine = false;
        while (i < text.size())
        {
            size_t j = text.find(' ', i);
            const std::string word = text.substr(i, j == std::string::npos ? std::string::npos : j - i);
            const int wl = (int)word.size();
            if (anyOnLine && col + 1 + wl > width)
            {
                std::printf("%s%s\n", pad.c_str(), _c.dim(cur.substr(indent)).c_str());
                cur = pad;
                col = indent;
                anyOnLine = false;
            }
            if (anyOnLine)
            {
                cur += ' ';
                ++col;
            }
            cur += word;
            col += wl;
            anyOnLine = true;
            if (j == std::string::npos) break;
            i = j + 1;
        }
        if (anyOnLine) std::printf("%s%s\n", pad.c_str(), _c.dim(cur.substr(indent)).c_str());
    }

    /*********************************************************************
     **************************** UI WIDGETS *****************************
     ********************************************************************/

    /**
     * @brief The bottom action bar: `F5 Copy   F9 Exec   F10 Quit`. Key in bold cyan, its label dimmed.
     */
    void keybar(std::initializer_list<std::pair<const char*, const char*>> keys) const
    {
        std::string line = "  ";
        bool first = true;
        for (const auto& k : keys)
        {
            if (!first) line += "   ";
            first = false;
            line += _c.bold(_c.cyan(k.first)) + " " + _c.dim(k.second);
        }
        std::printf("%s\n", line.c_str());
    }

    /** @brief One stat tile: a value in its hue over a dimmed caption. color 'd' = dim value (neutral). */
    struct Count
    {
        std::string value;
        std::string caption;
        char color = 'g';
    };

    /** @brief A compact row of stat tiles `1284 A-seen   1281 B-seen   …` — value bold+hued, caption dim. */
    void counters(std::initializer_list<Count> cells) const
    {
        std::string line = "  ";
        bool first = true;
        for (const auto& c : cells)
        {
            if (!first) line += "     ";
            first = false;
            line += (c.color == 'd' ? _c.dim(c.value) : _c.bold(paint(c.color, c.value))) + " " + _c.dim(c.caption);
        }
        std::printf("%s\n", line.c_str());
    }

    /**
     * @brief A breadcrumb `sd › logs › 2026` — the last part highlighted (current), the rest dimmed.
     */
    std::string crumb(std::initializer_list<std::string> parts) const
    {
        std::string s;
        size_t i = 0, n = parts.size();
        for (const auto& p : parts)
        {
            if (i) s += _c.mut(_t.glyph(" › ", " > "));
            s += (++i == n) ? _c.bright(p) : _c.dim(p);
        }
        return s;
    }

    /**
     * @brief A step row `✔ discover ── ▶ connect ── 3 program`.
     * @details Steps before `current` done (green ✔), `current` active (green ▶ + bold), after it pending
     *          (muted + number).
     */
    void stepper(std::initializer_list<const char*> steps, int current) const
    {
        std::string line = "  ";
        int i = 0;
        for (const auto& s : steps)
        {
            if (i) line += _c.mut(_t.glyph(" ── ", " -- "));
            if (i < current) line += _c.green(_t.glyph("✔", "v")) + " " + _c.dim(s);
            else if (i == current)
                line += _c.green(_t.glyph("▶", ">")) + " " + _c.bold(s);
            else
                line += _c.mut(std::to_string(i + 1) + " " + s);
            ++i;
        }
        std::printf("%s\n", line.c_str());
    }

    /**
     * @brief A settings toggle: `Sprache  [de] en` — the active option chipped, the others muted.
     */
    void seg(const std::string& label, std::initializer_list<std::string> opts, int active) const
    {
        std::string line = "  " + _c.dim(label);
        while (Tpl::vis(line) < 2 + 16)
            line += " "; // pad the label column to ~16
        int i = 0;
        for (const auto& o : opts)
        {
            if (i) line += " ";
            line += (i == active) ? _c.chip(o, 'g') : (" " + _c.mut(o) + " ");
            ++i;
        }
        std::printf("%s\n", line.c_str());
    }

    /**
     * @brief Group-object flags `C R W T U` (Comm / Read / Write / Transmit / Update).
     * @details The letters present in `active` are accented, the rest muted. `active` may list the set letters
     *          ("CRT") or a 5-char mask.
     */
    std::string koFlags(const std::string& active) const
    {
        static const char* code = "CRWTU";                    // canonical mask letters (data side)
        static const char* en[5] = {"C", "R", "W", "T", "U"}; // Comm/Read/Write/Transmit/Update
        static const char* de[5] = {"K", "L", "S", "Ü", "A"}; // Komm./Lesen/Schreiben/Übertr./Aktual.
        const bool ger = _i && _i->german();
        std::string s;
        for (int i = 0; i < 5; ++i)
        {
            const bool on = active.find(code[i]) != std::string::npos;
            // set -> the letter (localised) in accent; unset -> a dim placeholder dot, so ON flags pop.
            s += on ? _c.green(ger ? de[i] : en[i]) : _c.mut(_t.glyph("·", "."));
            if (i < 4) s += " ";
        }
        return s;
    }

    /**
     * @brief A compact legend line explaining a table's symbols/flags: `C Comm · R Read · W Write …`.
     * @details Each symbol in accent, its meaning dimmed, separated by middots. The reusable "explain this
     *          table" template — drop it under any table whose columns use flags, dots or codes.
     */
    void legend(std::initializer_list<std::pair<const char*, const char*>> items) const
    {
        std::string line = "    ";
        bool first = true;
        for (const auto& it : items)
        {
            if (!first) line += _c.mut("   ·   ");
            first = false;
            line += _c.green(it.first) + " " + _c.dim(it.second);
        }
        std::printf("%s\n", line.c_str());
    }

    /*********************************************************************
     **************************** BRAND MARK *****************************
     ********************************************************************/

    /**
     * @brief The OpenKNX console mark — the SAME animated mark `ftc mc` draws in its right pane while it scans.
     * @details Prints THREE lines: `Open ■` / `┬●──┴` / `■ KNX`. `size` sets the bus length: 's' 2 · 'm' 4 ·
     *          'l' 8. `phase` < 0 = static; >= 0 = one animation frame — a data packet `●` travels the bus and
     *          the two nodes pulse filled `■` / hollow `□` in OPPOSITE phase. Animate by looping `phase` and
     *          moving the cursor up 3 lines between frames.
     */
    void oknxLogo(char size = 'm', int phase = -1) const
    {
        const int N = size == 'l' ? 6 : size == 's' ? 2
                                                    : 4;
        const bool showText = size != 's'; // small = the bare mark (no "Open"/"KNX" words)
        const bool anim = phase >= 0;
        const bool pulse = anim && ((phase / 3) % 2 == 0);
        const int p = anim ? (phase % N) : N / 2;
        std::string n1, n2;
        if (!anim) // static: both nodes filled ■ (the About / identity form)
            n1 = n2 = _c.green(_t.glyph("■", "#"));
        else // animated: the two nodes pulse filled ■ / hollow □ in opposite phase (the scan form)
        {
            n1 = _c.green(_t.glyph(pulse ? "■" : "□", pulse ? "#" : "o"));
            n2 = _c.green(_t.glyph(pulse ? "□" : "■", pulse ? "o" : "#"));
        }
        std::string bus; // clean bus when static; the packet ● only appears while animating
        for (int i = 0; i < N; ++i)
            bus += (anim && i == p) ? _c.bright(_t.glyph("●", "*")) : _c.dim(_t.glyph("─", "-"));
        const std::string tee = _c.dim(_t.glyph("┬", "+")), toe = _c.dim(_t.glyph("┴", "+"));
        if (showText) std::printf("  %s %s\n", _c.txt("Open").c_str(), n1.c_str()); // "Open"/"KNX" in white (txt)
        else
            std::printf("  %s\n", n1.c_str());
        std::printf("  %s%s%s\n", tee.c_str(), bus.c_str(), toe.c_str());
        if (showText) std::printf("  %s %s\n", n2.c_str(), _c.txt("KNX").c_str());
        else
            std::printf("  %s\n", n2.c_str());
    }

    /**
     * @brief The OpenKNX brand / identity card — the static mark + the product line, copyright and links.
     * @details As `ftc mc`'s About overlay shows it. Reusable for an About screen / --version splash.
     *          Prints ~6 lines.
     */
    void brandCard() const
    {
        oknxLogo('m'); // the static mark (both nodes filled)
        std::printf("\n  %s   %s\n", _c.bold(_c.green("ftc")).c_str(),
                    _c.dim("OpenKNX FileTransferClient · KNXnet/IP").c_str());
        std::printf("  %s\n", _c.dim("© 2026 OpenKNX · Erkan Çolak · GPL-3.0-or-later").c_str());
        std::printf("  %s\n", _c.dim("https://openknx.de · https://wiki.openknx.de · https://forum.openknx.de").c_str());
    }

    /*********************************************************************
     *************************** NOTE & TABLE ****************************
     ********************************************************************/

    /**
     * @brief `→ <text>` — a dim follow-up hint under a status/panel (green arrow). For tips + next-steps.
     */
    void note(const std::string& text) const
    {
        std::printf("    %s %s\n", _c.green(_t.glyph("→", "->")).c_str(), _c.dim(text).c_str());
    }

    /**
     * @brief One aligned row. cells[i] is padded to widths[i] (left) unless widths[i] is negative (right).
     * @details A width of 0 leaves the cell unpadded (last column). Cells are pre-coloured by the caller;
     *          padding is computed on the VISIBLE width so colour codes never misalign. Header vs. body is the
     *          caller's colour.
     */
    void tableRow(const std::vector<std::string>& cells, const std::vector<int>& widths) const
    {
        std::string line = "  ";
        for (size_t i = 0; i < cells.size(); ++i)
        {
            const int w = i < widths.size() ? widths[i] : 0;
            const int vis = dispw(stripForWidth(cells[i]));
            const int pad = (w == 0) ? 0 : (w < 0 ? -w : w) - vis;
            std::string cell = cells[i];
            std::string sp;
            for (int k = 0; k < (pad > 0 ? pad : 0); ++k)
                sp += ' ';
            if (w < 0) line += sp + cell; // right-aligned
            else
                line += cell + sp;
            if (i + 1 < cells.size()) line += "  ";
        }
        std::printf("%s\n", line.c_str());
    }

    Theme& theme() const { return _c; }
    Term& term() const { return _t; }

  private:
    /*********************************************************************
     ************************** PRIVATE HELPERS **************************
     ********************************************************************/

    /**
     * @brief The content width a panel/section rule fills to: terminal width minus the 2-col indent, capped.
     * @details Capped so long terminals do not draw a rule across the whole screen. Floored so a narrow
     *          terminal still frames.
     */
    int panelWidth() const
    {
        int w = cols() - 2;
        if (w > 74) w = 74;
        if (w < 24) w = 24;
        return w;
    }

    /**
     * @brief The status dot glyph in its fixed hue.
     */
    std::string dot(Stat s) const
    {
        switch (s)
        {
            case Stat::Ok: return _c.green(_t.glyph("●", "*"));
            case Stat::Warn: return _c.amber(_t.glyph("●", "*"));
            case Stat::Err: return _c.red(_t.glyph("●", "x"));
            case Stat::Info: return _c.cyan(_t.glyph("●", "*"));
            case Stat::Idle: return _c.mut(_t.glyph("○", "o"));
        }
        return _c.mut(_t.glyph("○", "o"));
    }

    /**
     * @brief The status label in its fixed hue.
     */
    std::string labelColor(Stat s, const std::string& l) const
    {
        switch (s)
        {
            case Stat::Ok: return _c.green(l);
            case Stat::Warn: return _c.amber(l);
            case Stat::Err: return _c.red(l);
            case Stat::Info: return _c.bold(l);
            case Stat::Idle: return _c.dim(l);
        }
        return _c.txt(l);
    }

    /**
     * @brief The percentage token for a progress line: two decimals, CLAMPED to 99,99 until truly done.
     * @details Then a clean "100 %". Decimal separator follows the language (DE comma / EN dot). This is the
     *          99,99->100 rule.
     */
    std::string pct(double frac, bool done) const
    {
        if (done) return "100%";
        double p = frac * 100.0;
        if (p > 99.99) p = 99.99; // never show 100 before the transfer is confirmed
        if (p < 0) p = 0;
        char b[16];
        std::snprintf(b, sizeof(b), "%.2f", p);
        std::string s = b;
        if (_i && _i->german())
            for (char& ch : s)
                if (ch == '.') ch = ',';
        return s + "%";
    }

    /**
     * @brief The fill/track glyph pair for a bar style: 'l' slim line · 'p' pill · 'b' block (default). ASCII too.
     */
    static void barGlyphs(char style, const char*& fu, const char*& tu, const char*& fa, const char*& ta)
    {
        switch (style)
        {
            case 'l':
                fu = "━";
                tu = "─";
                fa = "=";
                ta = "-";
                break; // slim centred line (least dominant)
            case 'p':
                fu = "▰";
                tu = "▱";
                fa = "#";
                ta = ".";
                break; // pill
            default:
                fu = "█";
                tu = "░";
                fa = "#";
                ta = "-";
                break; // solid block
        }
    }

    /**
     * @brief One frame of the spinner; advances on every call so the line animates even at a flat fraction.
     */
    std::string spinnerFrame() const
    {
        static const char* u[10] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
        static const char* a[4] = {"|", "/", "-", "\\"};
        return _t.ascii() ? a[_spin++ % 4] : u[_spin++ % 10];
    }

    /**
     * @brief Strip ANSI so tableRow can measure the visible width of a pre-coloured cell (SGR only, our codes).
     */
    static std::string stripForWidth(const std::string& s)
    {
        std::string o;
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (s[i] == '\x1b')
            {
                size_t j = i + 1;
                if (j < s.size() && s[j] == '[')
                {
                    ++j;
                    while (j < s.size() && !((unsigned char)s[j] >= 0x40 && (unsigned char)s[j] <= 0x7e))
                        ++j;
                    i = j;
                    continue;
                }
            }
            o += s[i];
        }
        return o;
    }

    Term& _t;
    Theme& _c;
    I18n* _i;
    mutable int _spin = 0; // spinner phase (advanced by spinnerFrame)
};

/*********************************************************************
 ********************* PANEL - TITLED INFO FRAME *********************
 ********************************************************************/

/**
 * @brief The `┌─ Title · sub ───…` / rows / `└──…` info frame, buffered so its width fits the content.
 * @details Add kv()/line()/sep() rows, then render() with a width mode: render(w>0) — FIXED to w columns ·
 *          render(0) — AUTO-fit the content · render(-1) — FILL the terminal. The label column of kv rows
 *          auto-sizes to the widest label. Reuses Tpl for colour/glyph/measurement, so it degrades
 *          (no-colour / --ascii) exactly like the rest. Presentation only; the caller pre-colours values.
 */
class Panel
{
  public:
    Panel(Tpl& tpl, const std::string& title, const std::string& subtitle = "")
        : _p(tpl), _title(title), _sub(subtitle) {}

    Panel& kv(const std::string& label, const std::string& value)
    {
        _rows.push_back({Row::KV, label, value});
        return *this;
    }
    Panel& line(const std::string& text)
    {
        _rows.push_back({Row::LINE, "", text});
        return *this;
    }
    Panel& sep()
    {
        _rows.push_back({Row::SEP, "", ""});
        return *this;
    }

    /**
     * @brief Draw the frame. width: >0 fixed columns · 0 auto-fit the content · -1 fill the terminal.
     */
    void render(int width = 0) const
    {
        Theme& c = _p.theme();
        Term& T = _p.term();
        // widest kv label (visible) -> the label column.
        int labelW = 0;
        for (const auto& r : _rows)
            if (r.type == Row::KV) labelW = std::max(labelW, Tpl::vis(r.a));
        // widest content (from the content-start column): kv = label + 2 gap + value; line = text.
        int content = 0;
        for (const auto& r : _rows)
        {
            if (r.type == Row::KV) content = std::max(content, labelW + 2 + Tpl::vis(r.b));
            else if (r.type == Row::LINE)
                content = std::max(content, Tpl::vis(r.b));
        }
        const std::string opener = T.glyph("┌─ ", "+- ");
        const int titleVis = Tpl::dispw(opener) + Tpl::dispw(_title) + (_sub.empty() ? 0 : Tpl::dispw(" · " + _sub)) + 1;
        const int termW = Tpl::cols() - 2;
        int W;
        if (width > 0) W = width; // fixed
        else if (width < 0)
            W = termW; // fill terminal
        else
            W = std::max(titleVis, content + 2 + 1); // auto-fit content (2 = content indent past the frame)
        if (W > termW) W = termW;
        if (W < 12) W = 12;

        // top rule
        std::string top = c.dim(opener) + c.amber(_title);
        if (!_sub.empty()) top += c.dim(" · " + _sub);
        top += " ";
        std::string rule;
        for (int i = titleVis; i < W; ++i)
            rule += T.glyph("─", "-");
        std::printf("\n  %s%s\n", top.c_str(), c.dim(rule).c_str());

        // rows (content indented 2 past the frame opener = 4 overall)
        for (const auto& r : _rows)
        {
            if (r.type == Row::SEP)
            {
                std::string s;
                for (int i = 0; i < W - 2; ++i)
                    s += T.glyph("┈", "-");
                std::printf("    %s\n", c.mut(s).c_str());
            }
            else if (r.type == Row::KV)
            {
                std::string pad(std::max(0, labelW - Tpl::vis(r.a)), ' ');
                std::printf("    %s%s  %s\n", c.dim(r.a).c_str(), pad.c_str(), r.b.c_str());
            }
            else
                std::printf("    %s\n", r.b.c_str());
        }

        // bottom rule
        std::string brule = T.glyph("└", "+");
        for (int i = 1; i < W; ++i)
            brule += T.glyph("─", "-");
        std::printf("  %s\n", c.dim(brule).c_str());
    }

  private:
    struct Row
    {
        enum Type
        {
            KV,
            LINE,
            SEP
        } type;
        std::string a, b;
    };
    Tpl& _p;
    std::string _title, _sub;
    std::vector<Row> _rows;
};

} // namespace ftc

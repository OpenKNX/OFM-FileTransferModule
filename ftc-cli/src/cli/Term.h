/**
 * @file        Term.h
 * @brief       Terminal capability detection + low-level ANSI/SGR emission.
 * @details     One place owns "may I emit color?", "how rich?" (truecolor / 256 / none), "UTF-8 vs ASCII
 *              glyph fallback", and the raw escape strings. Everything visual (Theme, Ui) builds on top so
 *              the scattered Cg()/Cb()/… globals disappear. Honors NO_COLOR, isatty(), COLORTERM and --ascii.
 * @date        2026-08-03
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
    #include <io.h>
    #include <windows.h>
    #define FTC_ISATTY(fd) _isatty(fd)
    #define FTC_FILENO(f) _fileno(f)
#else
    #include <unistd.h>
    #define FTC_ISATTY(fd) isatty(fd)
    #define FTC_FILENO(f) fileno(f)
#endif

namespace ftc
{

/**
 * @brief Terminal output capabilities + raw SGR helpers. Construct once, call init() from main().
 */
class Term
{
  public:
    /**
     * @brief Detected color depth of the current terminal.
     */
    enum class Depth
    {
        None,    ///< no color (piped, NO_COLOR, or dumb terminal)
        Ansi256, ///< 256-color palette (also used as the fallback for basic terminals)
        True     ///< 24-bit truecolor
    };

    /**
     * @brief Probe the terminal: enable UTF-8 + VT processing (Windows), detect tty/NO_COLOR/COLORTERM.
     */
    void init()
    {
        bool tty;
#ifdef _WIN32
        SetConsoleOutputCP(CP_UTF8);
        tty = FTC_ISATTY(FTC_FILENO(stdout)) != 0;
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (tty && GetConsoleMode(h, &mode)) SetConsoleMode(h, mode | 0x0004 /*ENABLE_VIRTUAL_TERMINAL_PROCESSING*/);
#else
        tty = FTC_ISATTY(FTC_FILENO(stdout)) != 0;
#endif
        _tty = tty;
        if (std::getenv("NO_COLOR") != nullptr || !tty)
        {
            _depth = Depth::None;
            return;
        }
        const char* ct = std::getenv("COLORTERM");
        if (ct && (std::strstr(ct, "truecolor") || std::strstr(ct, "24bit")))
            _depth = Depth::True;
        else
            _depth = Depth::Ansi256; // safe default for modern terminals; 256-cube also renders on 16-colour
    }

    bool isTty() const { return _tty; }
    bool useColor() const { return _colorEnabled && _depth != Depth::None; }
    bool ascii() const { return _ascii; }

    void setColorEnabled(bool on) { _colorEnabled = on; } // force color off (e.g. -q / non-TTY sink)
    void setAscii(bool on) { _ascii = on; }               // ASCII glyph fallback (--ascii)

    /**
     * @brief Foreground color as an SGR string, degraded to the terminal's depth; "" when color is off.
     */
    std::string fg(uint8_t r, uint8_t g, uint8_t b) const
    {
        if (!useColor()) return std::string();
        char buf[32];
        if (_depth == Depth::True)
            std::snprintf(buf, sizeof(buf), "\x1b[38;2;%u;%u;%um", (unsigned)r, (unsigned)g, (unsigned)b);
        else
        {
            // map to the 6x6x6 color cube (indices 16..231)
            const int ri = (r * 5 + 127) / 255, gi = (g * 5 + 127) / 255, bi = (b * 5 + 127) / 255;
            std::snprintf(buf, sizeof(buf), "\x1b[38;5;%dm", 16 + 36 * ri + 6 * gi + bi);
        }
        return std::string(buf);
    }

    /**
     * @brief Background color as an SGR string, degraded to the terminal's depth; "" when color is off.
     */
    std::string bg(uint8_t r, uint8_t g, uint8_t b) const
    {
        if (!useColor()) return std::string();
        char buf[32];
        if (_depth == Depth::True)
            std::snprintf(buf, sizeof(buf), "\x1b[48;2;%u;%u;%um", (unsigned)r, (unsigned)g, (unsigned)b);
        else
        {
            const int ri = (r * 5 + 127) / 255, gi = (g * 5 + 127) / 255, bi = (b * 5 + 127) / 255;
            std::snprintf(buf, sizeof(buf), "\x1b[48;5;%dm", 16 + 36 * ri + 6 * gi + bi);
        }
        return std::string(buf);
    }

    const char* bold() const { return useColor() ? "\x1b[1m" : ""; }
    const char* dim() const { return useColor() ? "\x1b[2m" : ""; }
    const char* reset() const { return useColor() ? "\x1b[0m" : ""; }

    /**
     * @brief Pick the Unicode glyph or its ASCII fallback depending on --ascii.
     */
    const char* glyph(const char* unicode, const char* asciiFallback) const { return _ascii ? asciiFallback : unicode; }

    /**
     * @brief When --ascii is active, transliterate a (possibly colour-escaped) string to plain ASCII.
     * @details Decorative punctuation (· — « » → ↑ ● …), box/block glyphs, © Ç and the German umlauts are
     *          mapped to ASCII; SGR escape sequences are copied verbatim (never mangled). A no-op when not in
     *          ASCII mode, so it is safe to call on every themed fragment. Folding here (at colour-wrap time,
     *          before width measurement) keeps table/rule alignment correct. UTF-8-decoded codepoint by codepoint.
     */
    std::string foldAscii(const std::string& s) const
    {
        if (!_ascii) return s;
        std::string o;
        o.reserve(s.size());
        for (size_t i = 0; i < s.size();)
        {
            unsigned char ch = s[i];
            if (ch == '\x1b') // copy a whole CSI escape verbatim
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
            if (ch < 0x80)
            {
                o += (char)ch;
                ++i;
                continue;
            }
            uint32_t cp = ch;
            int len = 1;
            if ((ch & 0xE0) == 0xC0)
            {
                cp = ch & 0x1F;
                len = 2;
            }
            else if ((ch & 0xF0) == 0xE0)
            {
                cp = ch & 0x0F;
                len = 3;
            }
            else if ((ch & 0xF8) == 0xF0)
            {
                cp = ch & 0x07;
                len = 4;
            }
            for (int k = 1; k < len && i + k < s.size(); ++k)
                cp = (cp << 6) | (s[i + k] & 0x3F);
            o += mapCp(cp);
            i += len;
        }
        return o;
    }

  private:
    /**
     * @brief One non-ASCII codepoint -> its ASCII stand-in; unknowns become '?'.
     * @details Kept in sync with the glyphs the templates emit (drawing, blocks, arrows, dots) plus the
     *          Latin-1 punctuation / umlauts used in the chrome.
     */
    static std::string mapCp(uint32_t cp)
    {
        switch (cp)
        {
            case 0x00B7:
            case 0x2022: return "-"; // ·  •
            case 0x2013:
            case 0x2014: return "-"; // – —
            case 0x00AB:
            case 0x2039: return "<"; // «  ‹
            case 0x00BB:
            case 0x203A: return ">";  // »  ›
            case 0x2192: return "->"; // →
            case 0x2190: return "<-"; // ←
            case 0x2191: return "^";  // ↑
            case 0x2193: return "v";  // ↓
            case 0x25B6: return ">";  // ▶
            case 0x2713:
            case 0x2714: return "v"; // ✓ ✔
            case 0x25CF:
            case 0x25C9: return "*"; // ● ◉
            case 0x25AA: return "*"; // ▪
            case 0x25A0: return "#"; // ■
            case 0x25CB: return "o"; // ○
            case 0x2502:
            case 0x2503: return "|";   // │ ┃
            case 0x00A9: return "(c)"; // ©
            case 0x00C7: return "C";
            case 0x00E7: return "c"; // Ç ç
            case 0x00B0: return "";  // ° (drop)
            case 0x00E4: return "ae";
            case 0x00F6: return "oe";
            case 0x00FC: return "ue"; // ä ö ü
            case 0x00C4: return "Ae";
            case 0x00D6: return "Oe";
            case 0x00DC: return "Ue";  // Ä Ö Ü
            case 0x00DF: return "ss";  // ß
            case 0x2026: return "..."; // …
            case 0x2591: return "-";   // ░
        }
        if (cp >= 0x2500 && cp <= 0x257F) return "-";                                     // box drawing -> dash (corners/tees read fine as -)
        if ((cp >= 0x2580 && cp <= 0x259F) || (cp >= 0x2800 && cp <= 0x28FF)) return "#"; // blocks / braille
        if (cp < 0x80) return std::string(1, (char)cp);
        return "?";
    }

    bool _tty = false;
    bool _colorEnabled = true;
    bool _ascii = false;
    Depth _depth = Depth::None;
};

} // namespace ftc

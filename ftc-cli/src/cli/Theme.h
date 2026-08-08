/**
 * @file        Theme.h
 * @brief       The OpenKNX phosphor palette mapped onto ANSI colors, as a registry of NAMED palettes.
 * @details     Adding a theme = adding one Palette entry (accent swaps; the semantic colors ok/warn/err
 *              stay fixed across themes on purpose). A theme is selected by name at runtime (e.g. from
 *              ~/.config/ftc/config.toml). All colorizing goes through Term so "no color" (-q / NO_COLOR /
 *              non-TTY) collapses to plain text.
 * @date        2026-08-03
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include <cstdint>
#include <cstring>
#include <string>

#include "Term.h"

namespace ftc
{

/**
 * @brief One colour palette. Accent trio (g/gh/gd + cyan) is what a theme swaps; the rest stays fixed.
 */
struct Palette
{
    const char* name;
    uint8_t g[3], gh[3], gd[3], cyan[3]; // accent (swapped per theme)
    // fixed across themes:
    uint8_t amber[3], gold[3], red[3], blue[3], violet[3], txt[3], dim[3], mut[3];
};

class Theme
{
  public:
    explicit Theme(Term& term) : _t(term) {}

    /**
     * @brief Select a palette by name; unknown -> the default (green). Returns whether it matched.
     */
    bool select(const char* name)
    {
        for (const Palette& p : kPalettes)
            if (name && std::strcmp(name, p.name) == 0)
            {
                _p = &p;
                return true;
            }
        _p = &kPalettes[0];
        return false;
    }
    const char* activeName() const { return _p->name; }

    // colorized fragments (empty escapes when color is off)
    std::string green(const std::string& s) const { return wrap(_p->g, s); }
    std::string bright(const std::string& s) const { return wrap(_p->gh, s); }
    std::string amber(const std::string& s) const { return wrap(_p->amber, s); }
    std::string gold(const std::string& s) const { return wrap(_p->gold, s); }
    std::string red(const std::string& s) const { return wrap(_p->red, s); }
    std::string cyan(const std::string& s) const { return wrap(_p->cyan, s); }
    std::string blue(const std::string& s) const { return wrap(_p->blue, s); }
    std::string violet(const std::string& s) const { return wrap(_p->violet, s); }
    std::string txt(const std::string& s) const { return wrap(_p->txt, s); }
    std::string dim(const std::string& s) const { return std::string(_t.dim()) + _t.foldAscii(s) + _t.reset(); }
    std::string mut(const std::string& s) const { return wrap(_p->mut, s); }
    std::string bold(const std::string& s) const { return std::string(_t.bold()) + _t.foldAscii(s) + _t.reset(); }

    /**
     * @brief Full-row selection bar: the accent as BACKGROUND with near-black text (MC-style highlight).
     */
    std::string sel(const std::string& s) const
    {
        const std::string body = _t.foldAscii(s);
        const std::string b = _t.bg(_p->g[0], _p->g[1], _p->g[2]);
        if (b.empty()) return "\x1b[7m" + body + "\x1b[27m"; // no truecolor/256 -> reverse video fallback
        return b + _t.fg(4, 7, 5) + body + _t.reset();
    }

    /**
     * @brief An inline chip/tag: one padded token on a coloured background, near-black text (mock `.chip`).
     * @details kind selects the background hue: 'g' accent · 'c' cyan · 'a' amber · 'o' gold · 'r' red.
     *          No-colour -> a bracketed token so the meaning still reads on a pipe / NO_COLOR sink.
     */
    std::string chip(const std::string& s, char kind = 'g') const
    {
        const uint8_t* col = _p->g;
        switch (kind)
        {
            case 'c': col = _p->cyan; break;
            case 'a': col = _p->amber; break;
            case 'o': col = _p->gold; break;
            case 'r': col = _p->red; break;
            default: break;
        }
        const std::string body = _t.foldAscii(s);
        const std::string b = _t.bg(col[0], col[1], col[2]);
        if (b.empty()) return "[" + body + "]";
        return b + _t.fg(4, 7, 5) + _t.bold() + " " + body + " " + _t.reset();
    }

    // semantic aliases (kept separate from the accent so meaning never shifts with the theme)
    std::string ok(const std::string& s) const { return green(s); }
    std::string warn(const std::string& s) const { return amber(s); }
    std::string err(const std::string& s) const { return red(s); }

    Term& term() const { return _t; }

  private:
    std::string wrap(const uint8_t c[3], const std::string& s) const
    {
        const std::string body = _t.foldAscii(s); // ASCII-fold decorative glyphs when --ascii (no-op otherwise)
        const std::string sgr = _t.fg(c[0], c[1], c[2]);
        return sgr.empty() ? body : sgr + body + _t.reset();
    }

    // The registry. Index 0 is the default. Accent trio differs; semantics identical.
    static constexpr Palette kPalettes[3] = {
        // name        g                 gh                  gd                cyan
        // "green" (default) accent = OpenKNX brand green #449841 (logo/favicon/web), + a brighter + a dark tint.
        {"green", {68, 152, 65}, {99, 191, 92}, {40, 92, 38}, {79, 224, 208},
         /*amber*/ {255, 180, 84},
         /*gold*/ {255, 209, 102},
         /*red*/ {255, 93, 93},
         /*blue*/ {111, 176, 255},
         /*violet*/ {199, 139, 255},
         /*txt*/ {211, 228, 217},
         /*dim*/ {127, 147, 135},
         /*mut*/ {80, 96, 86}},
        {"amber", {255, 180, 84}, {255, 212, 136}, {122, 83, 32}, {255, 209, 102}, {255, 180, 84}, {255, 209, 102}, {255, 93, 93}, {111, 176, 255}, {199, 139, 255}, {211, 228, 217}, {127, 147, 135}, {80, 96, 86}},
        {"cyan", {79, 214, 255}, {154, 232, 255}, {28, 77, 107}, {111, 224, 255}, {255, 180, 84}, {255, 209, 102}, {255, 93, 93}, {111, 176, 255}, {199, 139, 255}, {211, 228, 217}, {127, 147, 135}, {80, 96, 86}},
    };

    Term& _t;
    const Palette* _p = &kPalettes[0];
};

} // namespace ftc

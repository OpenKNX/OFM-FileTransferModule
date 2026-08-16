/**
 * @file        Ui.h
 * @brief       Reusable presentation building blocks for the ftc CLI chrome.
 * @details     Banner, version, help, error blocks and small row/section helpers. Everything renders
 *              through Theme (colors) + Term (glyph/ascii + color on/off) + I18n (DE/EN), so a single
 *              switch flips the whole look. This keeps main() thin: it orchestrates; Ui draws.
 * @date        2026-08-03
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include <cstdio>
#include <string>
#include <vector>

#include "Templates.h"

#include "I18n.h"
#include "Term.h"
#include "Theme.h"

namespace ftc
{

class Ui
{
  public:
    Ui(Term& term, Theme& theme, I18n& i18n) : _t(term), _c(theme), _i(i18n) {}

    /**
     * @brief The OpenKNX console mark + one identity line (same mark the PS1 scripts + animation use).
     */
    void banner() const
    {
        const char* sq = _t.glyph("■", "#");
        std::printf("\n");
        std::printf("  %s %s\n", _c.dim("Open").c_str(), _c.green(sq).c_str());
        std::printf("  %s  %s%s  %s\n", _c.green(_t.glyph("┬────┴", "+----+")).c_str(),
                    _c.bold(_c.cyan("ftc")).c_str(), "",
                    _c.dim(_i.tr("native OpenKNX FileTransferClient over KNXnet/IP",
                                 "nativer OpenKNX-FileTransferClient über KNXnet/IP"))
                        .c_str());
        std::printf("  %s KNX   %s\n", _c.green(sq).c_str(),
                    _c.dim("© 2026 OpenKNX · Erkan Çolak https://github.com/GeminiServer · GPL-3.0").c_str());
        // Full URLs (a scheme linkifies more reliably); the banner may wrap at 80 cols -- accepted, it's not a table.
        std::printf("          %s\n\n",
                    _c.dim("https://openknx.de · https://wiki.openknx.de · https://forum.openknx.de").c_str());
    }

    /**
     * @brief The banner plus the CLI / FTC-protocol version and build date/time.
     */
    void version(const char* cliVer, const char* protoVer, const char* buildDate, const char* buildTime) const
    {
        banner();
        std::printf("  %s    %s\n", _c.dim("ftc").c_str(), _c.bold(cliVer).c_str());
        std::printf("  %s   FTC %s\n", _c.dim(_i.tr("protocol", "Protokoll")).c_str(), protoVer);
        std::printf("  %s      %s %s\n\n", _c.dim(_i.tr("built", "gebaut")).c_str(), buildDate, buildTime);
    }

    /**
     * @brief An uppercase section heading (amber), with an optional dim suffix.
     */
    void section(const char* title, const char* suffix = nullptr) const
    {
        if (suffix)
            std::printf("%s  %s\n", _c.amber(title).c_str(), _c.dim(suffix).c_str());
        else
            std::printf("%s\n", _c.amber(title).c_str());
    }
    /**
     * @brief One "  name   description" row: name in cyan, description dimmed, aligned to one column.
     * @details The description wraps at the terminal width and the continuation lines keep the column, so a
     *          long row stays readable instead of running off the screen. A name wider than the column gets
     *          its description on the next line rather than pushing the grid apart.
     */
    void cmdRow(const char* name, const char* desc) const
    {
        constexpr int COL = 34;   // where the description starts
        constexpr int LEAD = 2;   // indent of the whole block
        const int width = Tpl::cols();
        const int nameLen = visLen(name);

        // Below this there is no room for two columns: a 34-column indent would leave a ragged 40-character
        // ribbon. The description then goes under the name, indented, and stays readable.
        if (width < COL + 48)
        {
            std::printf("  %s\n", _c.cyan(name).c_str());
            for (const auto& line : wrap(desc, width - 8))
                std::printf("      %s\n", _c.dim(line.c_str()).c_str());
            return;
        }
        const int avail = width - COL - LEAD - 1;

        if (nameLen > COL - 1)
        {
            std::printf("  %s\n", _c.cyan(name).c_str());
            if (!desc || !*desc) return;
            for (const auto& line : wrap(desc, avail))
                std::printf("  %*s %s\n", COL, "", _c.dim(line.c_str()).c_str());
            return;
        }
        const std::vector<std::string> lines = wrap(desc, avail);
        bool first = true;
        for (const auto& line : lines)
        {
            if (first)
                std::printf("  %s%*s %s\n", _c.cyan(name).c_str(), COL - nameLen, "", _c.dim(line.c_str()).c_str());
            else
                std::printf("  %*s %s\n", COL, "", _c.dim(line.c_str()).c_str());
            first = false;
        }
        if (lines.empty()) std::printf("  %s\n", _c.cyan(name).c_str());
    }

    /**
     * @brief A semantic error block: a red/amber marker + title, then dimmed detail lines, then a fix hint.
     */
    void errorBlock(bool warning, const std::string& title, std::initializer_list<std::string> detail,
                    const std::string& fixHint = std::string()) const
    {
        const std::string mark = warning ? _c.amber(_t.glyph("⚠", "!")) : _c.red(_t.glyph("✖", "X"));
        std::fprintf(stderr, "  %s %s\n", mark.c_str(), (warning ? _c.amber(title) : _c.red(title)).c_str());
        for (const auto& d : detail)
            if (!d.empty()) std::fprintf(stderr, "    %s\n", _c.dim(d).c_str()); // a placeholder is not a line
        if (!fixHint.empty())
            std::fprintf(stderr, "    %s %s\n", _c.green(_t.glyph("→", "->")).c_str(), _c.dim(fixHint).c_str());
    }

    Theme& theme() const { return _c; }
    I18n& i18n() const { return _i; }
    Term& term() const { return _t; }

  private:
    /**
     * @brief Visible length of a plain (uncolored) label.
     * @details Our labels are ASCII, so byte length is fine here.
     */
    /** @brief Break a description into lines of at most `avail` characters, never mid-word. */
    static std::vector<std::string> wrap(const char* text, int avail)
    {
        std::vector<std::string> out;
        if (!text || !*text) return out;
        std::string cur;
        const char* p = text;
        while (*p)
        {
            const char* sp = p;
            while (*sp && *sp != ' ') ++sp;
            const std::string word(p, sp);
            if (!cur.empty() && (int)(cur.size() + 1 + word.size()) > avail)
            {
                out.push_back(cur);
                cur.clear();
            }
            if (!cur.empty()) cur += ' ';
            cur += word;
            p = sp;
            while (*p == ' ') ++p;
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    }

    /** @brief Printed width in columns, not bytes -- a UTF-8 glyph like "…" occupies one column, not three. */
    static int visLen(const char* s)
    {
        int n = 0;
        for (const unsigned char* p = (const unsigned char*)s; p && *p; ++p)
            if ((*p & 0xC0) != 0x80) ++n; // count lead bytes only, skip UTF-8 continuations
        return n;
    }

    Term& _t;
    Theme& _c;
    I18n& _i;
};

} // namespace ftc

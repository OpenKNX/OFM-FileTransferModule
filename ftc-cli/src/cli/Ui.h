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
                    _c.dim(_i.tr("native KNX FileTransferClient over KNXnet/IP",
                                 "nativer KNX-FileTransferClient über KNXnet/IP"))
                        .c_str());
        std::printf("  %s KNX   %s\n", _c.green(sq).c_str(),
                    _c.dim("© 2026 OpenKNX · Erkan Çolak · GPL-3.0").c_str());
        std::printf("          %s\n\n", _c.dim("wiki.openknx.de · forum.openknx.de").c_str());
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
     * @brief One "  name   description" row: name in cyan, description dimmed, aligned to col 34.
     */
    void cmdRow(const char* name, const char* desc) const
    {
        std::printf("  %s%*s %s\n", _c.cyan(name).c_str(), (int)(34 - visLen(name)), "", _c.dim(desc).c_str());
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
            std::fprintf(stderr, "    %s\n", _c.dim(d).c_str());
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
    static int visLen(const char* s)
    {
        int n = 0;
        while (s && s[n])
            ++n;
        return n;
    }

    Term& _t;
    Theme& _c;
    I18n& _i;
};

} // namespace ftc

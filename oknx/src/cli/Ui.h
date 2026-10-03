/**
 * @file        Ui.h
 * @brief       Reusable presentation building blocks for the oknx CLI chrome.
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
                    _c.bold(_c.cyan("oknx")).c_str(), "",
                    _c.dim(_i.tr("the OpenKNX desktop tool - files, firmware and diagnostics over KNXnet/IP",
                                 "das OpenKNX-Werkzeug für den PC - Dateien, Firmware und Diagnose über KNXnet/IP"))
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
        std::printf("  %s    %s\n", _c.dim("oknx").c_str(), _c.bold(cliVer).c_str());
        std::printf("  %s   FTC %s\n", _c.dim(_i.tr("protocol", "Protokoll")).c_str(), protoVer);
        std::printf("  %s      %s %s\n\n", _c.dim(_i.tr("built", "gebaut")).c_str(), buildDate, buildTime);
    }

    /**
     * @brief An uppercase section heading (amber), with an optional dim suffix.
     */
    /// @brief A heading for a block the caller prints itself (the examples page): straight out, no frame.
    void sectionPlain(const char* title, const char* suffix = nullptr) const
    {
        if (suffix) std::printf("%s  %s\n", _c.amber(title).c_str(), _c.dim(suffix).c_str());
        else std::printf("%s\n", _c.amber(title).c_str());
    }

    void section(const char* title, const char* suffix = nullptr) const
    {
        if (boxed())
        {
            // Collected, not drawn: the column grid is measured over the WHOLE help at the end, so the
            // description column starts at the same place in every frame. Measuring per frame made the
            // columns jump from section to section.
            _secs.push_back(Sec{title ? title : "", suffix ? suffix : "", {}});
            return;
        }
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
    /// @brief Is this terminal wide enough for the framed layout?
    static bool boxed() { return Tpl::cols() >= BOX_MIN; }
    /// @brief The frame width: it follows the terminal, but never below BOX_MIN and never past BOX_MAX.
    static int boxWidth()
    {
        const int c = Tpl::cols();
        return c < BOX_MIN ? BOX_MIN : (c > BOX_MAX ? BOX_MAX : c);
    }

    /**
     * @brief Draw the buffered section as one frame: command · short form · operands · text.
     * @details The three left columns are measured from THIS section's own rows, so a section of short
     *          commands does not pay for the longest line elsewhere in the help. Called by section() for
     *          the previous frame and by flushHelp() for the last one.
     */
    void flushBox() const
    {
        if (_secs.empty()) return;
        const int W = boxWidth();

        // ONE grid for the whole help: measure every row of every section, then draw.
        size_t c1 = 0, c2 = 0, c3 = 0;
        for (const Sec& sec : _secs)
            for (const auto& r : sec.rows)
            {
                if (r.first.empty()) continue;
                const Shape x = shapeOf(r.first);
                const size_t headLen = x.subject.empty()
                                           ? (size_t)visLen(x.cmd.c_str())
                                           : (size_t)visLen(x.subject.c_str()) + 1 + (size_t)visLen(x.cmd.c_str());
                if (headLen > c1) c1 = headLen;
                if ((size_t)visLen(x.alias.c_str()) > c2) c2 = (size_t)visLen(x.alias.c_str());
                if ((size_t)visLen(x.oper.c_str()) > c3) c3 = (size_t)visLen(x.oper.c_str());
            }
        // Capped, because one very long shape would otherwise push the text column of EVERY frame to the
        // right. A shape past the cap keeps its own width and pushes only its own row.
        c1 = (c1 + 2 > 22) ? 22 : c1 + 2;
        c2 = c2 ? ((c2 + 2 > 9) ? 9 : c2 + 2) : 0;
        c3 = c3 ? ((c3 + 2 > 24) ? 24 : c3 + 2) : 0;
        int dw = W - 4 - (int)(c1 + c2 + c3);
        if (dw < 40 && c3 > 10)
        {
            const size_t take = (size_t)(40 - dw);
            c3 = (c3 > take + 10) ? c3 - take : 10;
            dw = W - 4 - (int)(c1 + c2 + c3);
        }

        for (const Sec& sec : _secs)
        {
            if (sec.rows.empty())
            {
                // A section whose rows the caller prints itself keeps its plain heading.
                if (sec.suffix.empty()) std::printf("%s\n", _c.amber(sec.title).c_str());
                else std::printf("%s  %s\n", _c.amber(sec.title).c_str(), _c.dim(sec.suffix).c_str());
                continue;
            }
            std::printf("%s%s", _c.dim("╭─ ").c_str(), _c.amber(sec.title).c_str());
            int used = 3 + visLen(sec.title.c_str());
            if (!sec.suffix.empty())
            {
                std::printf("%s", _c.dim("  " + sec.suffix).c_str());
                used += 2 + visLen(sec.suffix.c_str());
            }
            std::string rule(" ");
            for (int i = used + 2; i < W; i++) rule += "─";
            std::printf("%s%s\n", _c.dim(rule).c_str(), _c.dim("╮").c_str());

            for (size_t ri = 0; ri < sec.rows.size(); ri++)
            {
                const Shape x = sec.rows[ri].first.empty() ? Shape{} : shapeOf(sec.rows[ri].first);
                // A shape wider than the capped grid pushes its own first line; the text then has less
                // room on THAT line, so it is wrapped to what is actually left - otherwise the row runs
                // past the frame.
                size_t plain0 = c1 + c2 + c3;
                if (!sec.rows[ri].first.empty())
                {
                    const size_t h = x.subject.empty()
                                         ? (size_t)visLen(x.cmd.c_str())
                                         : (size_t)visLen(x.subject.c_str()) + 1 + (size_t)visLen(x.cmd.c_str());
                    const size_t al = (size_t)visLen(x.alias.c_str()), op = (size_t)visLen(x.oper.c_str());
                    plain0 = h + (c1 > h ? c1 - h : 1);
                    if (c2) plain0 += al + (c2 > al ? c2 - al : 1);
                    if (c3) plain0 += op + (c3 > op ? c3 - op : 1);
                }
                const int roomRow = W - 4 - (int)plain0;
                const std::vector<std::string> text = wrap(sec.rows[ri].second.c_str(),
                                                           (roomRow < dw ? roomRow : dw) - 1);
                const size_t lineCount = text.empty() ? 1u : text.size();
                for (size_t k = 0; k < lineCount; k++)
                {
                    std::string left;
                    size_t plain = 0;
                    if (k == 0 && !sec.rows[ri].first.empty())
                    {
                        const size_t headLen = x.subject.empty()
                                                   ? (size_t)visLen(x.cmd.c_str())
                                                   : (size_t)visLen(x.subject.c_str()) + 1 + (size_t)visLen(x.cmd.c_str());
                        const size_t aliasLen = (size_t)visLen(x.alias.c_str());
                        const size_t operLen = (size_t)visLen(x.oper.c_str());
                        if (!x.subject.empty()) left += _c.violet(x.subject) + " ";
                        left += _c.bold(x.cmd);
                        left += pad(c1 > headLen ? c1 - headLen : 1);
                        plain = headLen + (c1 > headLen ? c1 - headLen : 1);
                        if (c2)
                        {
                            if (!x.alias.empty()) left += _c.cyan(x.alias);
                            left += pad(c2 > aliasLen ? c2 - aliasLen : 1);
                            plain += aliasLen + (c2 > aliasLen ? c2 - aliasLen : 1);
                        }
                        if (c3)
                        {
                            if (!x.oper.empty()) left += (x.oper[0] == '-') ? _c.blue(x.oper) : _c.oper(x.oper);
                            left += pad(c3 > operLen ? c3 - operLen : 1);
                            plain += operLen + (c3 > operLen ? c3 - operLen : 1);
                        }
                    }
                    else
                    {
                        left = pad(c1 + c2 + c3);
                        plain = c1 + c2 + c3;
                    }
                    const std::string body = k < text.size() ? text[k] : std::string();
                    const int fill = W - 4 - (int)plain - visLen(body.c_str());
                    std::printf("%s  %s%s%s%s\n", _c.dim("│").c_str(), left.c_str(), _c.dim(body).c_str(),
                                pad(fill > 0 ? (size_t)fill : 0).c_str(), _c.dim("│").c_str());
                }
                if (lineCount > 1 && ri + 1 < sec.rows.size())
                    std::printf("%s%s%s\n", _c.dim("│").c_str(), pad((size_t)W - 2).c_str(), _c.dim("│").c_str());
            }
            std::string bot = "╰";
            for (int i = 0; i < W - 2; i++) bot += "─";
            bot += "╯";
            std::printf("%s\n\n", _c.dim(bot).c_str());
        }
        _secs.clear();
    }

    /// @brief Close the last frame of a help page.
    void flushHelp() const { flushBox(); }

    static std::string pad(size_t n) { return std::string(n, ' '); }

    /// @brief The narrowest terminal that still gets the framed layout. Below this the rows stack, because
    ///        a 150-column frame in an 80-column terminal wraps and the box falls apart.
    static constexpr int BOX_MIN = 150;
    /// @brief And the widest it grows to. Past this the description line gets too long to read comfortably.
    static constexpr int BOX_MAX = 168;

    /// @brief One command shape split into the roles the frame gives their own column.
    struct Shape
    {
        std::string subject; ///< `<pa>` - the device the line acts on
        std::string cmd;     ///< the verb
        std::string alias;   ///< its short form, if it has one
        std::string oper;    ///< operands and option values
    };

    /**
     * @brief Split "busmon | bm" / "<pa> led [on|off|blink]" / "--ip A.B.C.D | -i" into its roles.
     * @details Driven by the characters, not by a per-row table, so a command added later lands in the
     *          right column by itself. A trailing `| -x` is the SHORT FORM, not an operand - that is what
     *          the help writes today and it is the one case the bracket rule cannot see.
     */
    static Shape shapeOf(const std::string& name)
    {
        Shape sh;
        std::string t = name;
        if (t.rfind("<pa>", 0) == 0)
        {
            sh.subject = "<pa>";
            t = trim(t.substr(4));
        }
        const size_t bar = t.rfind('|');
        if (bar != std::string::npos)
        {
            const std::string tail = trim(t.substr(bar + 1));
            if (tail.size() >= 2 && tail[0] == '-' && tail.find(' ') == std::string::npos)
            {
                sh.alias = tail;
                t = trim(t.substr(0, bar));
            }
        }
        if (!t.empty() && t[0] == '-')
        {
            const size_t sp = t.find(' ');
            sh.cmd = (sp == std::string::npos) ? t : t.substr(0, sp);
            if (sp != std::string::npos) sh.oper = trim(t.substr(sp));
            return sh;
        }
        // first token that opens an operand
        size_t cut = std::string::npos;
        for (size_t i = 0; i < t.size(); i++)
            if ((i == 0 || t[i - 1] == ' ') && (t[i] == '<' || t[i] == '[')) { cut = i; break; }
        std::string head = (cut == std::string::npos) ? t : trim(t.substr(0, cut));
        if (cut != std::string::npos) sh.oper = trim(t.substr(cut));
        if (sh.alias.empty())
        {
            const size_t b2 = head.find('|');
            if (b2 != std::string::npos)
            {
                const std::string l = trim(head.substr(0, b2)), r = trim(head.substr(b2 + 1));
                if (!l.empty() && !r.empty() && l.find(' ') == std::string::npos && r.find(' ') == std::string::npos)
                {
                    sh.cmd = l;
                    sh.alias = r;
                    return sh;
                }
            }
        }
        sh.cmd = head;
        return sh;
    }

    struct Sec
    {
        std::string title, suffix;
        std::vector<std::pair<std::string, std::string>> rows;
    };
    mutable std::vector<Sec> _secs;

    static std::string trim(const std::string& s)
    {
        size_t a = s.find_first_not_of(' ');
        if (a == std::string::npos) return "";
        size_t b = s.find_last_not_of(' ');
        return s.substr(a, b - a + 1);
    }

    /**
     * @brief Colour one command shape by the roles the USAGE block announces as a legend: subject violet,
     *        verb bold, operands teal, options blue.
     * @details Split from the characters, not a per-row list, so a command added later is coloured right
     *          by itself. `<pa>` is the subject, not an operand; a row starting with an option is an
     *          option row and its bare words are values.
     */
    std::string roleShape(const char* name) const
    {
        std::string out, tok;
        bool subjSeen = false, optRow = false, firstTok = true, sawOpt = false;
        auto flush = [&]() {
            if (tok.empty()) return;
            const bool opt = tok[0] == '-' || tok.compare(0, 2, "[-") == 0;
            if (firstTok && opt) optRow = true;
            firstTok = false;
            // Violet is reserved for <pa>, the DEVICE the line acts on - the one thing this tool has to
            // teach. Giving the command word the same colour said "ga is a device" and undid the lesson.
            if (tok == "|" || tok == "\u00b7") out += _c.dim(tok);
            else if (tok.find("<pa>") != std::string::npos) { out += _c.violet(tok); subjSeen = true; }
            else if (opt) { out += _c.blue(tok); sawOpt = true; }
            else if (tok.find('<') != std::string::npos) out += _c.oper(tok);
            else if (optRow || sawOpt) out += _c.oper(tok); // a bare word after an option is its VALUE
            else out += _c.bold(tok);
            (void)subjSeen;
            tok.clear();
        };
        for (const char* p = name; *p != 0; p++)
        {
            if (*p == ' ') { flush(); out += ' '; }
            else tok += *p;
        }
        flush();
        return out;
    }

    void cmdRow(const char* name, const char* desc) const
    {
        if (boxed() && !_secs.empty())
        {
            _secs.back().rows.push_back({name ? name : "", desc ? desc : ""});
            return;
        }
        constexpr int COL = 34;   // where the description starts
        constexpr int LEAD = 2;   // indent of the whole block
        const int width = Tpl::cols();
        // An empty name is a continuation of the row above, not a command: print the text alone instead
        // of an empty cyan line that reads as a gap.
        if (name == nullptr || *name == 0)
        {
            for (const auto& line : wrap(desc, width - 8)) std::printf("      %s\n", _c.dim(line.c_str()).c_str());
            return;
        }
        const int nameLen = visLen(name);

        // Below this there is no room for two columns: a 34-column indent would leave a ragged 40-character
        // ribbon. The description then goes under the name, indented, and stays readable.
        if (width < COL + 48)
        {
            std::printf("  %s\n", roleShape(name).c_str());
            for (const auto& line : wrap(desc, width - 8))
                std::printf("      %s\n", _c.dim(line.c_str()).c_str());
            return;
        }
        const int avail = width - COL - LEAD - 1;

        if (nameLen > COL - 1)
        {
            std::printf("  %s\n", roleShape(name).c_str());
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
                std::printf("  %s%*s %s\n", roleShape(name).c_str(), COL - nameLen, "", _c.dim(line.c_str()).c_str());
            else
                std::printf("  %*s %s\n", COL, "", _c.dim(line.c_str()).c_str());
            first = false;
        }
        if (lines.empty()) std::printf("  %s\n", roleShape(name).c_str());
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

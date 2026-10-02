#pragma once

/**
 * @file        cli/Browser.h
 * @brief       A file and folder chooser for the terminal, in the shape people already know from mc.
 *
 * Reusable on purpose: anywhere oknx has to ask "which file" or "which folder", this is the answer, so
 * the question always looks and behaves the same. Directories first with a leading slash, files after
 * them, size and time in their own columns, one solid bar on the current row.
 *
 * Arrow keys where the terminal allows it. Where it does not -- a pipe, a log, a terminal without raw
 * mode -- the same list is offered by number, so a session that cannot use arrows is not stuck.
 *
 * @date        2026-08-22
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 */

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <sys/ioctl.h>
    #include <unistd.h>
#endif

#include "I18n.h"
#include "Keys.h"
#include "Templates.h" // Tpl::vis -- display columns, which is what a table has to be measured in
#include "Term.h"
#include "Theme.h"

namespace ftc
{
    /** @brief What a caller wants picked. */
    struct BrowseSpec
    {
        std::string title;                 ///< shown above the panel
        std::vector<std::string> suffixes; ///< which files to list, e.g. {".app.bin"}; empty = every file
        bool allowDirPick = true;          ///< 'a' takes the folder the panel is showing
        bool allowFilePick = true;         ///< Enter on a file ends the chooser with that file
        std::string start;                 ///< where to open; empty = the working directory

        /// Where a path can start from, shown on "d". Left to the caller because it is the one
        /// platform-specific thing here: Windows has no common root over its drives, so the list IS
        /// the level above a path there, while elsewhere it is a shortcut to a few known places.
        std::vector<std::pair<std::string, std::string>> roots; ///< path, description

        /// Optional right-hand hint per row -- "2 release images" and the like. The chooser has no
        /// business knowing what makes a folder interesting; the caller does.
        std::function<std::string(const std::string& path, bool isDir)> note;
    };

    namespace detail
    {
        struct BrowseEntry
        {
            std::string path, name;
            bool isDir = false;
            bool isUp = false;
            bool isRoot = false;
            uint64_t size = 0;
            std::time_t mtime = 0;
            std::string note;
        };

        /** @brief Usable columns. A fixed guess would wrap the panel on a narrow terminal. */
        /** @brief The REAL terminal width. Not the layout width: this is what wraps a line. */
        inline int termCols()
        {
#ifdef _WIN32
            CONSOLE_SCREEN_BUFFER_INFO i;
            if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &i))
            {
                const int w = i.srWindow.Right - i.srWindow.Left + 1;
                if (w > 20) return w;
            }
#else
            struct winsize w;
            if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 20) return (int)w.ws_col;
#endif
            return 100;
        }

        inline int browserWidth()
        {
#ifdef _WIN32
            CONSOLE_SCREEN_BUFFER_INFO i;
            if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &i))
            {
                const int w = i.srWindow.Right - i.srWindow.Left + 1;
                if (w > 40) return (w > 160) ? 160 : w;
            }
#else
            struct winsize w;
            if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 40)
                return (w.ws_col > 160) ? 160 : (int)w.ws_col;
#endif
            return 100;
        }

        inline bool suffixMatches(const std::string& name, const std::vector<std::string>& suffixes)
        {
            if (suffixes.empty()) return true;
            for (const auto& sfx : suffixes)
            {
                if (name.size() <= sfx.size()) continue;
                if (name.compare(name.size() - sfx.size(), sfx.size(), sfx) == 0) return true;
            }
            return false;
        }

        /** @brief Cut to `w` display columns, keeping the tail of a long name -- that is the telling half. */
        /** @brief Display columns of a string. Bytes are not columns: "Größe" is 6 bytes and 5 columns. */
        inline size_t dispW(const std::string& s)
        {
            const int v = Tpl::vis(s);
            return v > 0 ? (size_t)v : 0;
        }

        /**
         * @brief Pad or shorten to exactly `w` display columns, keeping the END of a long name.
         * @details The tail is what tells two long paths apart. Cutting has to land on a character
         *          boundary -- half a UTF-8 sequence prints as a replacement glyph and shifts every
         *          column behind it, which is how one umlaut used to bend a whole table.
         */
        inline std::string fit(const std::string& s, size_t w)
        {
            const size_t vis = dispW(s);
            if (vis <= w) return s + std::string(w - vis, ' ');
            if (w <= 3) return std::string(w, '.');
            const size_t keep = w - 3;
            size_t cut = s.size(), cols = 0;
            while (cut > 0)
            {
                size_t j = cut - 1;
                while (j > 0 && ((unsigned char)s[j] & 0xC0) == 0x80) --j; // back over continuation bytes
                const size_t add = dispW(s.substr(j, cut - j));
                if (cols + add > keep) break;
                cols += add;
                cut = j;
            }
            std::string out = "..." + s.substr(cut);
            const size_t ov = dispW(out);
            return ov < w ? out + std::string(w - ov, ' ') : out;
        }

        /**
         * @brief Hard-cut a coloured line to `w` display columns, keeping every escape sequence intact.
         * @details The last line of defence for the redraw: it steps back by the number of lines PRINTED,
         *          so one line that wraps costs two and tears the picture apart. Colour codes are copied
         *          through without being counted -- they occupy no column.
         */
        inline std::string clipAnsi(const std::string& s, size_t w)
        {
            std::string out;
            size_t cols = 0, i = 0;
            while (i < s.size())
            {
                if (s[i] == '\x1b')
                {
                    const size_t start = i++;
                    if (i < s.size() && s[i] == '[')
                        while (++i < s.size() && !(s[i] >= '@' && s[i] <= '~')) {}
                    if (i < s.size()) ++i;
                    out += s.substr(start, i - start);
                    continue;
                }
                size_t j = i + 1;
                while (j < s.size() && ((unsigned char)s[j] & 0xC0) == 0x80) ++j;
                const size_t add = dispW(s.substr(i, j - i));
                if (cols + add > w) break;
                cols += add;
                out += s.substr(i, j - i);
                i = j;
            }
            if (i < s.size()) out += "\x1b[0m"; // cut mid-colour -> close it, or the rest of the screen tints
            return out;
        }

        /** @brief Shorten to `w` columns keeping the START -- for a hint, where the first words carry it. */
        inline std::string shorten(const std::string& s, size_t w)
        {
            if (dispW(s) <= w) return s;
            if (w <= 1) return std::string();
            size_t cut = 0, cols = 0;
            while (cut < s.size())
            {
                size_t j = cut + 1;
                while (j < s.size() && ((unsigned char)s[j] & 0xC0) == 0x80) ++j;
                const size_t add = dispW(s.substr(cut, j - cut));
                if (cols + add > w - 1) break;
                cols += add;
                cut = j;
            }
            return s.substr(0, cut) + ".";
        }

        /** @brief Expand a leading ~ so a typed home path behaves the way it does in a shell. */
        inline std::string expandTilde(const std::string& p)
        {
            if (p.empty() || p[0] != '~') return p;
            const char* home = std::getenv("HOME");
#ifdef _WIN32
            if (home == nullptr || *home == 0) home = std::getenv("USERPROFILE");
#endif
            if (home == nullptr || *home == 0) return p;
            if (p.size() == 1) return std::string(home);
            if (p[1] == '/' || p[1] == '\\') return std::string(home) + p.substr(1);
            return p;
        }

        inline std::string timeText(std::time_t t)
        {
            if (t == 0) return "            ";
            char b[24];
            std::tm tmv{};
#ifdef _WIN32
            localtime_s(&tmv, &t);
#else
            localtime_r(&t, &tmv);
#endif
            std::strftime(b, sizeof(b), "%d.%m %H:%M", &tmv);
            return b;
        }

        inline std::string sizeText(const BrowseEntry& e)
        {
            if (e.isUp) return "UP--DIR";
            if (e.isDir) return "DIR";
            char b[24];
            std::snprintf(b, sizeof(b), "%llu", (unsigned long long)e.size);
            return b;
        }

        /** @brief One directory's contents: folders first, then the files the caller asked for. */
        inline void readDir(const std::filesystem::path& dir, const BrowseSpec& spec,
                            std::vector<BrowseEntry>& out)
        {
            namespace fs = std::filesystem;
            std::error_code ec;
            out.clear();

            if (dir.has_parent_path() && dir.parent_path() != dir)
            {
                BrowseEntry up;
                up.path = dir.parent_path().string();
                up.name = "/..";
                up.isDir = up.isUp = true;
                out.push_back(up);
            }

            std::vector<BrowseEntry> dirs, files;
            for (const auto& e : fs::directory_iterator(dir, ec))
            {
                if (ec) { ec.clear(); break; }
                const std::string fn = e.path().filename().string();
                BrowseEntry b;
                b.path = e.path().string();
                std::error_code e2;
                const auto wt = fs::last_write_time(e.path(), e2);
                if (!e2)
                {
                    // A portable-enough conversion: file_clock and system_clock share an epoch closely
                    // enough for a column that shows minutes.
                    const auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                        wt - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
                    b.mtime = std::chrono::system_clock::to_time_t(sctp);
                }
                if (e.is_directory(e2))
                {
                    b.isDir = true;
                    b.name = "/" + fn;
                    dirs.push_back(b);
                    continue;
                }
                if (!suffixMatches(fn, spec.suffixes)) continue;
                b.name = fn;
                b.size = (uint64_t)e.file_size(e2);
                files.push_back(b);
            }
            ec.clear();
            auto byName = [](const BrowseEntry& a, const BrowseEntry& b) { return a.name < b.name; };
            std::sort(dirs.begin(), dirs.end(), byName);
            std::sort(files.begin(), files.end(), byName);
            out.insert(out.end(), dirs.begin(), dirs.end());
            out.insert(out.end(), files.begin(), files.end());
        }
    } // namespace detail

    /**
     * @brief Show the chooser until something is picked or the user gives up.
     * @param out  the chosen path -- a folder when 'a' was pressed, a file when Enter was.
     * @return false when nothing was picked.
     */
    inline bool browse(Term& t, Theme& c, I18n& L, const BrowseSpec& spec, std::string& out)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        out.clear();

        fs::path cur;
        if (!spec.start.empty() && fs::is_directory(spec.start, ec)) cur = fs::path(spec.start);
        else cur = fs::current_path(ec);
        ec.clear();

        Keys keys;
        const bool interactive = t.isTty() && keys.active();

        std::vector<detail::BrowseEntry> rows;
        size_t sel = 0, top = 0;
        int drawn = 0;

        const int W = detail::browserWidth();
        const int TERM = detail::termCols(); // what actually wraps -- the layout above may be capped
        const int COL_SIZE = 9, COL_TIME = 12;
        // "  " + "│ " + name + " │ " + size + " │ " + time == name + size + time + 10 visible columns.
        // Counting 8 made every row two columns too wide: the last field wrapped onto the next line, and
        // a wrapped line also broke the cursor-up redraw, which is why the table kept re-appearing.
        const int COL_FIXED = COL_SIZE + COL_TIME + 10;
        const size_t WINDOW = 18;

        bool rootsView = false;
        auto reload = [&]() {
            if (rootsView)
            {
                rows.clear();
                for (const auto& r : spec.roots)
                {
                    detail::BrowseEntry e;
                    e.path = r.first;
                    e.name = r.first;
                    e.isDir = true;
                    e.isRoot = true;
                    e.note = r.second;
                    rows.push_back(e);
                }
            }
            else
            {
                detail::readDir(cur, spec, rows);
                if (spec.note)
                    for (auto& e : rows)
                        if (!e.isUp) e.note = spec.note(e.path, e.isDir);
            }
            sel = 0;
            top = 0;
        };
        reload();

        auto draw = [&]() {
            if (interactive && drawn > 0) std::printf("\x1b[%dA\x1b[J", drawn);
            drawn = 0;
            // One line printed == one line on screen. Anything wider than the terminal would wrap, and the
            // redraw above counts PRINTED lines, not screen lines -- so a single wrap tears the picture.
            auto line = [&](const std::string& s) {
                std::printf("%s\n", detail::clipAnsi(s, (size_t)(TERM - 1)).c_str());
                ++drawn;
            };

            if (sel < top) top = sel;
            if (sel >= top + WINDOW) top = sel - WINDOW + 1;
            const size_t last = (rows.size() < top + WINDOW) ? rows.size() : top + WINDOW;

            // The name column follows the names that are actually in view, instead of stretching to the
            // edge of the screen: a four-character folder in a 130-character column reads as a broken
            // table, and the fixed columns end up so far right that they look unrelated to their row.
            int nameW = (int)detail::dispW(L.tr("Name", "Name"));
            for (size_t i = top; i < last; ++i)
                nameW = std::max(nameW, (int)detail::dispW(rows[i].name));
            const int nameMax = std::max(12, W - COL_FIXED);
            nameW = std::max(16, std::min(nameW, nameMax));
            const int tableW = nameW + COL_FIXED;
            const int noteRoom = TERM - 1 - tableW - 4; // what is left behind the row for a hint

            line("");
            const std::string head = rootsView ? L.tr("Drives", "Laufwerke") : cur.string();
            line("  " + c.dim("┌─ ") + c.cyan(detail::fit(head, (size_t)std::max(8, tableW - 7))) + c.dim(" ─"));
            line("  " + c.dim("│ ") + c.dim(detail::fit(L.tr("Name", "Name"), (size_t)nameW)) + c.dim(" │ ") +
                 c.dim(detail::fit(L.tr("Size", "Größe"), (size_t)COL_SIZE)) + c.dim(" │ ") +
                 c.dim(detail::fit(L.tr("Modified", "Geändert"), (size_t)COL_TIME)));

            if (rows.empty()) line("  " + c.dim("│ ") + c.dim(L.tr("nothing here", "hier ist nichts")));

            for (size_t i = top; i < last; ++i)
            {
                const auto& e = rows[i];
                const std::string name = detail::fit(e.name, (size_t)nameW);
                std::string size = detail::sizeText(e);
                const size_t sizeVis = detail::dispW(size);
                if ((int)sizeVis < COL_SIZE) size = std::string((size_t)COL_SIZE - sizeVis, ' ') + size;
                const std::string when = detail::fit(detail::timeText(e.mtime), (size_t)COL_TIME);
                // A hint only when there is room for one; clipped, never wrapped onto the next line.
                const std::string note =
                    (e.note.empty() || noteRoom < 6) ? std::string() : detail::shorten(e.note, (size_t)noteRoom);

                if (interactive && i == sel)
                {
                    // One solid bar, plain text inside it: colours layered under a reverse-video run
                    // fight each other and the row stops looking like one selection.
                    std::string flat = name + " │ " + size + " │ " + when;
                    if (!note.empty()) flat += "  " + note;
                    line("  " + c.dim("│ ") + std::string("\x1b[7m") + flat + std::string("\x1b[27m"));
                    continue;
                }
                const std::string nm = e.isDir ? c.cyan(name) : c.txt(name);
                std::string row = "  " + c.dim("│ ") + nm + c.dim(" │ ") + c.dim(size) + c.dim(" │ ") + c.dim(when);
                if (!note.empty()) row += "  " + c.green(t.glyph("●", "*")) + " " + c.dim(note);
                line(row);
            }
            if (top > 0 || last < rows.size())
            {
                char b[80];
                std::snprintf(b, sizeof(b), "%u-%u / %u", (unsigned)(top + 1), (unsigned)last, (unsigned)rows.size());
                line("  " + c.dim("└─ ") + c.dim(b));
            }
            else
                line("  " + c.dim("└─"));

            line("");
            if (interactive)
            {
                std::string k = "   " + c.bold(t.glyph("↑↓", "up/dn")) + " " + c.dim(L.tr("move", "wählen")) +
                                "   " + c.bold(t.glyph("↵", "enter")) + " " + c.dim(L.tr("open", "öffnen"));
                if (spec.allowDirPick) k += "   " + c.bold("a") + " " + c.dim(L.tr("take this folder", "diesen Ordner nehmen"));
                k += "   " + c.bold("d") + " " + c.dim(L.tr("drives", "Laufwerke")) +
                     "   " + c.bold("/") + " " + c.dim(L.tr("path", "Pfad")) +
                     "   " + c.bold("q") + " " + c.dim(L.tr("cancel", "Ende"));
                line(k);
            }
            else
            {
                std::string k = "   " + c.bold("1-99") + " " + c.dim(L.tr("open", "öffnen"));
                if (spec.allowDirPick) k += "   " + c.bold("a") + " " + c.dim(L.tr("take this folder", "diesen Ordner nehmen"));
                k += "   " + c.bold("/") + " " + c.dim(L.tr("path", "Pfad")) +
                     "   " + c.bold("q") + " " + c.dim(L.tr("cancel", "Ende"));
                line(k);
            }
            std::fflush(stdout);
        };

        auto openRow = [&](size_t i) -> int { // 1 = picked, 0 = moved, -1 = ignored
            if (i >= rows.size()) return -1;
            if (rows[i].isDir)
            {
                cur = fs::path(rows[i].path);
                rootsView = false;
                reload();
                return 0;
            }
            if (!spec.allowFilePick) return -1;
            out = rows[i].path;
            return 1;
        };

        auto readTyped = [&](std::string& dst) -> bool {
            if (interactive) keys.restore();
            char buf[512] = {0};
            const bool got = std::fgets(buf, sizeof(buf), stdin) != nullptr;
            drawn = 0;
            if (!got) return false;
            dst = buf;
            while (!dst.empty() && (dst.back() == '\n' || dst.back() == '\r' || dst.back() == ' ')) dst.pop_back();
            return true;
        };

        for (;;)
        {
            draw();

            int k = 0;
            if (interactive) k = keys.waitKey();
            else
            {
                std::printf("  %s ", c.amber("?").c_str());
                std::fflush(stdout);
                std::string in;
                if (!readTyped(in)) return false;
                if (in == "q" || in == "Q") return false;
                if (in == "a" || in == "A") { if (spec.allowDirPick) { out = cur.string(); return true; } continue; }
                if (in == "/") k = '/';
                else
                {
                    const long pick = std::strtol(in.c_str(), nullptr, 10);
                    if (pick >= 1 && (size_t)pick <= rows.size() && openRow((size_t)pick - 1) == 1) return true;
                    continue;
                }
            }

            if (k == 'q' || k == 'Q' || k == K_ESC) { if (interactive) std::printf("\n"); return false; }
            if (k == K_UP) { if (sel > 0) --sel; continue; }
            if (k == K_DOWN) { if (sel + 1 < rows.size()) ++sel; continue; }
            if (k == K_HOME) { sel = 0; continue; }
            if (k == K_END) { sel = rows.empty() ? 0 : rows.size() - 1; continue; }
            if (k == K_PGUP) { sel = (sel > WINDOW) ? sel - WINDOW : 0; continue; }
            if (k == K_PGDN) { sel = (sel + WINDOW < rows.size()) ? sel + WINDOW : (rows.empty() ? 0 : rows.size() - 1); continue; }
            if (k == 'a' || k == 'A')
            {
                if (!spec.allowDirPick) continue;
                out = cur.string();
                if (interactive) std::printf("\n");
                return true;
            }
            if (k == K_LEFT || k == K_BACK)
            {
                if (rootsView) { rootsView = false; reload(); continue; }
                if (cur.has_parent_path() && cur.parent_path() != cur) { cur = cur.parent_path(); reload(); }
                continue;
            }
            if (k == K_ENTER || k == K_RIGHT)
            {
                if (openRow(sel) == 1) { if (interactive) std::printf("\n"); return true; }
                continue;
            }
            if (k == 'd' || k == 'D')
            {
                if (spec.roots.empty()) continue;
                rootsView = !rootsView;
                reload();
                continue;
            }
            if (k == '/')
            {
                std::printf("  %s %s ", c.amber("?").c_str(), c.dim(L.tr("path", "Pfad")).c_str());
                std::fflush(stdout);
                std::string in;
                if (!readTyped(in)) return false;
                if (in.empty()) continue;
                in = detail::expandTilde(in);
                std::error_code e2;
                if (fs::is_directory(in, e2)) { cur = fs::path(in); reload(); continue; }
                if (fs::is_regular_file(in, e2) && spec.allowFilePick) { out = in; return true; }
                continue;
            }
        }
    }
} // namespace ftc

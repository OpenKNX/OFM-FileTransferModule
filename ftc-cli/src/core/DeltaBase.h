#pragma once

/**
 * @file        DeltaBase.h
 * @brief       Finding the release a device is already running, so only the difference has to be sent.
 *
 * A difference needs a starting point, and the starting point is a file on this machine -- not something
 * the device can hand over. Reading a whole image back over the bus would cost exactly as long as sending
 * the full one, so the device is only ever asked to CONFIRM a candidate, never to supply it.
 *
 * That leaves the question of where the candidate comes from, and the answer is: from what this computer
 * already knows, before anyone is asked to type a path. Two sources cover almost every real case -- the
 * release this computer installed last, and the releases lying next to the one being installed now.
 * Typing a path stays available for the rest, and so does browsing, because a release on a USB stick has
 * no reason to be somewhere a person can recite from memory.
 * @date        2026-08-22
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 */

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
    #include <windows.h>
#endif

#include "../cli/I18n.h"
#include "../cli/Templates.h"
#include "../cli/Browser.h"
#include "../cli/Keys.h"
#include "../cli/Term.h"
#include "../cli/Theme.h"
#include "Delta.h"
#include "DeltaProbe.h"

namespace ftc
{
    /** @brief One offer in the "previous release" list. */
    struct BaseCandidate
    {
        std::string appPath; ///< the raw application image a difference is computed against
        std::string label;   ///< what the user recognises: the release folder's name
        std::string origin;  ///< why it is being offered
    };

    namespace detail
    {
        /** @brief The release folder above a <release>/Firmware/<device>/<name>.app.bin. */
        inline std::string releaseFolderOf(const std::filesystem::path& appFile)
        {
            std::error_code ec;
            const std::filesystem::path dev = appFile.parent_path();      // <device>
            const std::filesystem::path fwd = dev.parent_path();          // Firmware
            const std::filesystem::path rel = fwd.parent_path();          // <release>
            if (rel.empty()) return dev.filename().string();
            return rel.filename().string();
        }

        /**
         * @brief First *.app.bin under a folder, at most three levels down.
         * @details A release keeps its images at <release>/Firmware/<variant>/, so three levels reach
         *          every one of them. The bound is not tidiness: the folder BESIDE a release can be
         *          anything at all -- a source tree, a backup drive -- and walking it whole would stall
         *          the screen this list is meant to fill.
         */
        inline std::string firstAppImage(const std::filesystem::path& dir, const std::string& preferDevice)
        {
            std::error_code ec;
            if (!std::filesystem::is_directory(dir, ec)) return "";
            std::string any;
            for (std::filesystem::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec))
            {
                if (ec) { ec.clear(); continue; }
                if (it.depth() >= 2) it.disable_recursion_pending();
                if (!it->is_regular_file(ec)) continue;
                const std::string n = it->path().filename().string();
                if (n.size() <= 8 || n.compare(n.size() - 8, 8, ".app.bin") != 0) continue;
                // A release holds one folder per hardware variant. Preferring the one whose folder name
                // matches the target keeps a KNeoPiX from being offered a REG2 image.
                if (!preferDevice.empty() && it->path().parent_path().filename().string() == preferDevice)
                    return it->path().string();
                if (any.empty()) any = it->path().string();
            }
            return any;
        }
    } // namespace detail

    // ─── the cache: what this computer installed last ────────────────────────────────────────────

    /**
     * @brief Remember which image a device was last given, as one line "<pa>\t<path>".
     * @details Only the path is stored, never a copy: an image is around a megabyte, and the release it
     *          came from is normally still on disk. A path that has since gone is simply not offered.
     */
    inline void baseCacheRemember(const std::string& cachePath, uint16_t pa, const std::string& appPath)
    {
        if (cachePath.empty() || appPath.empty()) return;
        std::vector<std::string> keep;
        if (std::FILE* f = std::fopen(cachePath.c_str(), "rb"))
        {
            char line[1024];
            while (std::fgets(line, sizeof(line), f))
            {
                std::string s(line);
                while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
                if (s.empty()) continue;
                const size_t tab = s.find('\t');
                if (tab == std::string::npos) continue;
                if ((unsigned)std::strtoul(s.substr(0, tab).c_str(), nullptr, 10) == pa) continue; // replaced
                keep.push_back(s);
            }
            std::fclose(f);
        }
        // Bounded: this is a convenience list, not an archive.
        while (keep.size() > 63) keep.erase(keep.begin());
        std::FILE* o = std::fopen(cachePath.c_str(), "wb");
        if (o == nullptr) return;
        for (const auto& k : keep) std::fprintf(o, "%s\n", k.c_str());
        std::fprintf(o, "%u\t%s\n", (unsigned)pa, appPath.c_str());
        std::fclose(o);
    }

    /** @brief The remembered image for one device, if it is still where it was. */
    inline std::string baseCacheLookup(const std::string& cachePath, uint16_t pa)
    {
        if (cachePath.empty()) return "";
        std::FILE* f = std::fopen(cachePath.c_str(), "rb");
        if (f == nullptr) return "";
        std::string hit;
        char line[1024];
        while (std::fgets(line, sizeof(line), f))
        {
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            const size_t tab = s.find('\t');
            if (tab == std::string::npos) continue;
            if ((unsigned)std::strtoul(s.substr(0, tab).c_str(), nullptr, 10) != pa) continue;
            hit = s.substr(tab + 1);
        }
        std::fclose(f);
        std::error_code ec;
        if (!hit.empty() && std::filesystem::is_regular_file(hit, ec)) return hit;
        return "";
    }

    // ─── the neighbours: other releases beside this one ──────────────────────────────────────────

    /**
     * @brief Releases lying next to the one being installed, newest folder first.
     * @details A firmware sits at <release>/Firmware/<device>/<name>.uf2, so the sibling releases are the
     *          folders beside <release>. Only folders that actually hold an application image for this
     *          same hardware are offered -- an empty offer is worse than none.
     */
    inline std::vector<BaseCandidate> siblingReleases(const std::string& firmwarePath, size_t max = 6)
    {
        std::vector<BaseCandidate> out;
        std::error_code ec;
        const std::filesystem::path fw(firmwarePath);
        const std::filesystem::path devDir = fw.parent_path();                 // <device>
        const std::string device = devDir.filename().string();
        const std::filesystem::path relDir = devDir.parent_path().parent_path(); // <release>
        const std::filesystem::path parent = relDir.parent_path();
        if (parent.empty() || !std::filesystem::is_directory(parent, ec)) return out;

        std::vector<std::filesystem::path> dirs;
        for (const auto& e : std::filesystem::directory_iterator(parent, ec))
        {
            if (ec) { ec.clear(); continue; }
            if (!e.is_directory(ec)) continue;
            if (e.path() == relDir) continue; // the release being installed is not its own predecessor
            dirs.push_back(e.path());
        }
        // Newest first: a version-sorted name usually puts the closest predecessor at the top, and the
        // closest predecessor is the one that yields the smallest difference.
        std::sort(dirs.begin(), dirs.end(), [](const std::filesystem::path& a, const std::filesystem::path& b) {
            return a.filename().string() > b.filename().string();
        });

        for (const auto& d : dirs)
        {
            if (out.size() >= max) break;
            const std::string app = detail::firstAppImage(d, device);
            if (app.empty()) continue;
            BaseCandidate c;
            c.appPath = app;
            c.label = d.filename().string();
            c.origin = d.string();
            out.push_back(c);
        }
        return out;
    }

    /** @brief Cache hit plus neighbours, the cache first and never listed twice. */
    inline std::vector<BaseCandidate> collectBaseCandidates(const std::string& cachePath, uint16_t pa,
                                                           const std::string& firmwarePath,
                                                           const std::string& cacheOriginText)
    {
        std::vector<BaseCandidate> out;
        const std::string cached = baseCacheLookup(cachePath, pa);
        if (!cached.empty())
        {
            BaseCandidate c;
            c.appPath = cached;
            c.label = detail::releaseFolderOf(cached);
            c.origin = cacheOriginText;
            out.push_back(c);
        }
        for (const auto& s : siblingReleases(firmwarePath))
        {
            bool dup = false;
            for (const auto& e : out)
                if (e.appPath == s.appPath) { dup = true; break; }
            if (!dup) out.push_back(s);
        }
        return out;
    }

    // ─── drives and volumes ──────────────────────────────────────────────────────────────────────

    /**
     * @brief Where a person's data can start from, named the way their system names it.
     * @details Windows has no common root above its drives -- C:\ and E:\ are separate trees -- so the
     *          list IS the level above a path there. macOS and Linux have one root and mount points
     *          underneath it, so the same list is a shortcut to three known places. Same key for the
     *          user, different origin underneath.
     */
    inline std::vector<std::pair<std::string, std::string>> driveRoots()
    {
        std::vector<std::pair<std::string, std::string>> out;
        std::error_code ec;
#ifdef _WIN32
        const DWORD mask = GetLogicalDrives();
        for (int i = 0; i < 26; ++i)
        {
            if ((mask & (1u << i)) == 0) continue;
            char root[4] = {(char)('A' + i), ':', '\\', 0};
            char label[MAX_PATH + 1] = {0};
            DWORD serial = 0, maxComp = 0, flags = 0;
            std::string desc;
            if (GetVolumeInformationA(root, label, MAX_PATH, &serial, &maxComp, &flags, nullptr, 0))
                desc = label;
            const UINT type = GetDriveTypeA(root);
            const char* kind = "";
            if (type == DRIVE_REMOVABLE) kind = "removable";
            else if (type == DRIVE_REMOTE) kind = "network";
            else if (type == DRIVE_CDROM) kind = "cd";
            else if (type == DRIVE_FIXED) kind = "fixed";
            if (!desc.empty() && *kind) desc += std::string("  ") + kind;
            else if (desc.empty()) desc = kind;
            out.push_back({std::string(root), desc});
        }
#else
        out.push_back({"/", ""});
        const char* home = std::getenv("HOME");
        if (home && *home) out.push_back({std::string(home), ""});
        std::vector<std::string> mountRoots;
    #ifdef __APPLE__
        mountRoots.push_back("/Volumes");
    #else
        const char* user = std::getenv("USER");
        if (user && *user) mountRoots.push_back(std::string("/media/") + user);
        mountRoots.push_back("/media");
        mountRoots.push_back("/mnt");
    #endif
        for (const auto& m : mountRoots)
        {
            if (!std::filesystem::is_directory(m, ec)) { ec.clear(); continue; }
            for (const auto& e : std::filesystem::directory_iterator(m, ec))
            {
                if (ec) { ec.clear(); break; }
                if (!e.is_directory(ec)) continue;
                bool dup = false;
                for (const auto& o : out)
                    if (o.first == e.path().string()) { dup = true; break; }
                if (!dup) out.push_back({e.path().string(), ""});
            }
        }
#endif
        return out;
    }

    // ─── input ───────────────────────────────────────────────────────────────────────────────────

    /** @brief One trimmed line from stdin; false when there is nobody to answer. */
    inline bool readLine(Term& t, std::string& out)
    {
        out.clear();
        if (!t.isTty()) return false;
        char buf[512] = {0};
        if (std::fgets(buf, sizeof(buf), stdin) == nullptr) return false;
        std::string s(buf);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t'))
            s.pop_back();
        size_t b = 0;
        while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) ++b;
        out = s.substr(b);
        return true;
    }

    /** @brief Expand a leading ~ so a typed home path behaves the way it does in a shell. */
    inline std::string expandHome(const std::string& p)
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

    // ─── browsing ────────────────────────────────────────────────────────────────────────────────

    /**
     * @brief How many application images a folder holds, looked for where a release actually keeps them.
     * @details Bounded on purpose: an unbounded walk of a home directory to decorate a list entry would
     *          stall the very screen it is meant to help. An .app.bin directly in the folder, or one
     *          under Firmware/<variant>/, is the release layout; anything else is found by opening the folder.
     */
    inline int countAppImages(const std::filesystem::path& dir)
    {
        std::error_code ec;
        int n = 0;
        auto scan = [&](const std::filesystem::path& d) {
            if (!std::filesystem::is_directory(d, ec)) { ec.clear(); return; }
            for (const auto& e : std::filesystem::directory_iterator(d, ec))
            {
                if (ec) { ec.clear(); return; }
                if (!e.is_regular_file(ec)) continue;
                const std::string fn = e.path().filename().string();
                if (fn.size() > 8 && fn.compare(fn.size() - 8, 8, ".app.bin") == 0) ++n;
            }
        };
        scan(dir);
        const std::filesystem::path fwd = dir / "Firmware";
        if (std::filesystem::is_directory(fwd, ec))
        {
            for (const auto& e : std::filesystem::directory_iterator(fwd, ec))
            {
                if (ec) { ec.clear(); break; }
                if (e.is_directory(ec)) scan(e.path());
            }
        }
        ec.clear();
        return n;
    }

    /**
     * @brief The shared chooser, set up for the one thing a difference needs: a release image.
     * @details Nothing about picking files lives here any more -- cli/Browser.h owns that, so every
     *          screen in ftc that asks "which file" looks and behaves the same. What IS specific stays
     *          specific: the suffix worth listing, where a path can start from on this system, and what
     *          makes a folder worth a second look.
     */
    inline bool browseForFolder(Term& t, Theme& c, I18n& L, const Tpl& tpl, std::string& chosen,
                                const std::string& startDir = "")
    {
        (void)tpl;
        BrowseSpec spec;
        spec.suffixes.push_back(".app.bin");
        spec.suffixes.push_back(".uf2");         // RP package -- carries the raw image inside it
        spec.suffixes.push_back(".factory.bin"); // ESP package -- likewise, behind its partition table
        spec.allowDirPick = true;  // a release folder is searched for the image inside it
        spec.allowFilePick = true; // ...or the image itself is pointed at
        spec.start = startDir;
        spec.roots = driveRoots();
        spec.note = [&L](const std::string& path, bool isDir) -> std::string {
            if (!isDir)
                return (path.size() > 4 && path.compare(path.size() - 4, 4, ".uf2") == 0)
                           ? L.tr("release image (packed)", "Release-Image (verpackt)")
                           : L.tr("release image", "Release-Image");
            const int n = countAppImages(std::filesystem::path(path));
            if (n == 1) return L.tr("1 release image", "1 Release-Image");
            if (n > 1)
            {
                static char b[48];
                std::snprintf(b, sizeof(b), L.tr("%d release images", "%d Release-Images"), n);
                return b;
            }
            return "";
        };
        return browse(t, c, L, spec, chosen);
    }

    // ─── the offer ───────────────────────────────────────────────────────────────────────────────

    /** @brief What the user decided about the starting point for a difference. */
    enum class BasePick
    {
        Chosen,   ///< a path was picked; use it
        FullImage, ///< send the whole image
        Quit      ///< abandon the update
    };

    /**
     * @brief Offer what this computer already knows, and let anything else be typed or browsed.
     * @details Every exit that is not an explicit quit lands on the full image -- the route the update
     *          would have taken anyway. Nothing here can make the update worse than not asking at all.
     */
    inline BasePick pickBase(Term& t, Theme& c, I18n& L, const Tpl& tpl,
                             const std::vector<BaseCandidate>& cands,
                             const std::string& savingText, std::string& chosen)
    {
        chosen.clear();
        if (!t.isTty()) return BasePick::FullImage;

        for (;;)
        {
            tpl.section(L.tr("Previous version", "Vorherige Version"));
            tpl.status(Tpl::Stat::Ok,
                       L.tr("this device can install a difference", "dieses Gerät kann Differenzen einspielen"),
                       {savingText});

            const std::vector<int> w = {4, 34, 0};
            if (!cands.empty())
            {
                tpl.tableRow({c.dim("#"), c.dim(L.tr("RELEASE", "RELEASE")), c.dim(L.tr("FROM", "WOHER"))}, w);
                for (size_t i = 0; i < cands.size(); ++i)
                    tpl.tableRow({c.bold(std::to_string(i + 1)), c.txt(cands[i].label), c.dim(cands[i].origin)}, w);
            }
            else
                tpl.note(L.tr("no earlier release found on this computer",
                              "auf diesem Rechner wurde kein früheres Release gefunden"));

            tpl.keybar({{"1-9", L.tr("take it", "nehmen")},
                        {"p", L.tr("type a path", "Pfad eingeben")},
                        {"b", L.tr("browse", "durchsuchen")},
                        {"n", L.tr("send the full image", "Voll-Image senden")},
                        {"q", L.tr("quit", "Ende")}});
            std::printf("  %s ", c.amber("?").c_str());
            std::fflush(stdout);
            std::string in;
            if (!readLine(t, in)) return BasePick::FullImage;

            if (in == "q" || in == "Q") return BasePick::Quit;
            if (in.empty() || in == "n" || in == "N") return BasePick::FullImage;
            if (in == "b" || in == "B")
            {
                std::string start;
                if (!cands.empty())
                {
                    std::error_code ec;
                    const std::filesystem::path p(cands[0].appPath);
                    start = p.parent_path().string();
                    ec.clear();
                }
                std::string got;
                if (browseForFolder(t, c, L, tpl, got, start) && !got.empty()) { chosen = got; return BasePick::Chosen; }
                continue;
            }
            if (in == "p" || in == "P")
            {
                std::printf("  %s %s ", c.amber("?").c_str(),
                            c.dim(L.tr("folder or .app.bin", "Ordner oder .app.bin")).c_str());
                std::fflush(stdout);
                std::string p2;
                if (!readLine(t, p2)) return BasePick::FullImage;
                p2 = expandHome(p2);
                if (p2.empty()) continue;
                chosen = p2;
                return BasePick::Chosen;
            }
            const long pick = std::strtol(in.c_str(), nullptr, 10);
            if (pick >= 1 && (size_t)pick <= cands.size())
            {
                chosen = cands[(size_t)pick - 1].appPath;
                return BasePick::Chosen;
            }
            // Anything else was not an answer; ask again rather than guess.
        }
    }
} // namespace ftc

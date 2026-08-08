/**
 * @file        Watch.h
 * @brief       Console JOB system — a `watch`/cron for the FTC console (`/job …`)
 * @details     `/job add <type> …` adds a job; today the only type is `watch` (a recurring command). Jobs
 *              fire only when the console is ready (1-outstanding lockstep -> never overlaps, bus-friendly)
 *              and are cached per target PA so they survive a reconnect. Host-only; strings are DE/EN.
 * @date        2026-08-03
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <string>
#include <vector>
#include "Theme.h"
#include "I18n.h"

namespace ftc
{

struct Job
{
    std::string type = "watch"; // extensible; only "watch" is scheduled today
    std::string cmd;
    uint32_t intervalMs = 1000;
    uint64_t lastRunMs = 0; // 0 = never -> fires on the first ready tick
    bool enabled = true;
};

class Watch
{
  public:
    Watch(Theme& theme, I18n& i18n) : _c(theme), _i(i18n) {}

    /**
     * @brief A line is a Watch meta-command iff it starts with '/'. Handled locally, never sent to the device.
     */
    bool isMeta(const std::string& line) const { return !line.empty() && line[0] == '/'; }

    /**
     * @brief Number of currently enabled jobs.
     */
    int activeCount() const
    {
        int n = 0;
        for (const auto& j : _jobs)
            if (j.enabled) ++n;
        return n;
    }

    /**
     * @brief Load the per-PA job cache. Line format: "<enabled 0/1> <type> <intervalMs> <command…>".
     */
    void load(const std::string& path)
    {
        _file = path;
        _jobs.clear();
        FILE* f = std::fopen(path.c_str(), "r");
        if (!f) return;
        char buf[512];
        while (std::fgets(buf, sizeof(buf), f))
        {
            std::string ln(buf);
            while (!ln.empty() && (ln.back() == '\n' || ln.back() == '\r'))
                ln.pop_back();
            if (ln.empty() || ln[0] == '#') continue;
            std::vector<std::string> t = split(ln);
            if (t.size() < 4) continue;
            Job j;
            j.enabled = (t[0] != "0");
            j.type = t[1];
            j.intervalMs = (uint32_t)std::strtoul(t[2].c_str(), nullptr, 10);
            for (size_t i = 3; i < t.size(); ++i)
            {
                if (i > 3) j.cmd += ' ';
                j.cmd += t[i];
            }
            if (j.intervalMs >= 100 && !j.cmd.empty()) _jobs.push_back(j);
        }
        std::fclose(f);
    }

    /**
     * @brief The one command due this tick, or "" if nothing is due / the console is not ready.
     * @details 1-outstanding: only ONE per call; the most-overdue enabled `watch` wins so no job starves.
     */
    std::string due(uint64_t nowMs, bool ready)
    {
        if (!ready) return std::string();
        int best = -1;
        uint64_t bestOver = 0;
        for (size_t i = 0; i < _jobs.size(); ++i)
        {
            if (!_jobs[i].enabled || _jobs[i].type != "watch") continue;
            if (nowMs - _jobs[i].lastRunMs < _jobs[i].intervalMs) continue;
            const uint64_t over = nowMs - _jobs[i].lastRunMs;
            if (best < 0 || over > bestOver)
            {
                best = (int)i;
                bestOver = over;
            }
        }
        if (best < 0) return std::string();
        _jobs[(size_t)best].lastRunMs = nowMs;
        return _jobs[(size_t)best].cmd;
    }

    /**
     * @brief Handle a `/job …` meta line; returns the feedback lines to emit. Persists on any change.
     */
    std::vector<std::string> handle(const std::string& line)
    {
        std::vector<std::string> t = split(line);
        std::vector<std::string> out;
        if (t.empty() || t[0] != "/job")
        {
            out.push_back(err(_i.tr("unknown command — try /job help", "unbekannter Befehl — /job help")));
            return out;
        }
        if (t.size() < 2) return listJobs();

        const std::string& sub = t[1];

        if (sub == "add")
        {
            if (t.size() >= 3 && t[2] != "watch")
            {
                out.push_back(err(_i.tr("unknown job type — only 'watch' for now", "unbekannter Job-Typ — bisher nur 'watch'")));
                return out;
            }
            size_t ai = 3;
            if (ai < t.size() && t[ai] == "every") ++ai; // optional keyword
            if (ai + 1 >= t.size())
            {
                out.push_back(err(_i.tr("usage: /job add watch every <interval> <command>   e.g. /job add watch every 1s m",
                                        "Aufruf: /job add watch every <Intervall> <Befehl>   z.B. /job add watch every 1s m")));
                return out;
            }
            const uint32_t ms = parseInterval(t[ai]);
            if (!ms)
            {
                out.push_back(err(_i.tr("bad interval — use e.g. 500ms, 1s, 2m", "ungültiges Intervall — z.B. 500ms, 1s, 2m")));
                return out;
            }
            std::string c;
            for (size_t i = ai + 1; i < t.size(); ++i)
            {
                if (i > ai + 1) c += ' ';
                c += t[i];
            }
            _jobs.push_back({"watch", c, ms, 0, true});
            save();
            out.push_back(ok(_i.tr("added", "hinzugefügt") + std::string(" #") + std::to_string(_jobs.size()) + "  " +
                             _c.dim("watch " + fmtInterval(ms)) + " " + _c.txt(c)));
            return out;
        }
        if (sub == "list" || sub == "ls") return listJobs();
        if (sub == "clear")
        {
            _jobs.clear();
            save();
            out.push_back(ok(_i.tr("all jobs cleared", "alle Jobs gelöscht")));
            return out;
        }
        if (sub == "help" || sub == "?") return helpLines();

        if ((sub == "on" || sub == "off" || sub == "enable" || sub == "disable" || sub == "rm") &&
            t.size() >= 3 && t[2] == "all")
        {
            if (sub == "rm")
            {
                _jobs.clear();
                save();
                out.push_back(ok(_i.tr("all jobs removed", "alle Jobs entfernt")));
                return out;
            }
            const bool on = (sub == "on" || sub == "enable");
            for (auto& j : _jobs)
            {
                j.enabled = on;
                if (on) j.lastRunMs = 0;
            }
            save();
            out.push_back(ok(on ? _i.tr("all enabled", "alle aktiviert") : _i.tr("all disabled", "alle deaktiviert")));
            return out;
        }

        const int id = std::atoi(sub.c_str());
        if (id >= 1 && id <= (int)_jobs.size() && t.size() >= 3)
        {
            const std::string& act = t[2];
            if (act == "on" || act == "off" || act == "enable" || act == "disable")
            {
                const bool on = (act == "on" || act == "enable");
                _jobs[(size_t)id - 1].enabled = on;
                if (on) _jobs[(size_t)id - 1].lastRunMs = 0;
                save();
                out.push_back(ok((on ? _i.tr("enabled", "aktiviert") : _i.tr("disabled", "deaktiviert")) + std::string(" #") + std::to_string(id)));
            }
            else if (act == "rm" || act == "del")
            {
                _jobs.erase(_jobs.begin() + (id - 1));
                save();
                out.push_back(ok(_i.tr("removed", "entfernt") + std::string(" #") + std::to_string(id)));
            }
            else
                out.push_back(err(_i.tr("unknown action — on | off | rm", "unbekannte Aktion — on | off | rm")));
            return out;
        }
        out.push_back(err(_i.tr("no such job / bad syntax — see /job list", "kein solcher Job / Syntax — siehe /job list")));
        return out;
    }

    /**
     * @brief Parse "500ms" / "1s" / "2m" (bare number = seconds) -> milliseconds; 0 = invalid, floored at 100 ms.
     */
    static uint32_t parseInterval(const std::string& s)
    {
        if (s.empty()) return 0;
        size_t i = 0;
        std::string num;
        while (i < s.size() && (std::isdigit((unsigned char)s[i]) || s[i] == '.'))
            num += s[i++];
        if (num.empty()) return 0;
        const double v = std::atof(num.c_str());
        const std::string u = s.substr(i);
        double ms;
        if (u == "ms") ms = v;
        else if (u == "s" || u.empty())
            ms = v * 1000.0;
        else if (u == "m")
            ms = v * 60000.0;
        else
            return 0;
        if (ms < 100.0) ms = 100.0; // floor: never flood the bus
        return (uint32_t)ms;
    }

    /**
     * @brief Format milliseconds back to the shortest "m"/"s"/"ms" unit.
     */
    static std::string fmtInterval(uint32_t ms)
    {
        if (ms % 60000 == 0) return std::to_string(ms / 60000) + "m";
        if (ms % 1000 == 0) return std::to_string(ms / 1000) + "s";
        return std::to_string(ms) + "ms";
    }

  private:
    Theme& _c;
    I18n& _i;
    std::vector<Job> _jobs;
    std::string _file;

    std::string ok(const std::string& s) const { return _c.green("job ") + s; }
    std::string err(const std::string& s) const { return _c.amber("job ") + s; }

    /**
     * @brief Persist all jobs to the per-PA cache file.
     */
    void save() const
    {
        if (_file.empty()) return;
        FILE* f = std::fopen(_file.c_str(), "w");
        if (!f) return;
        std::fprintf(f, "# ftc console jobs (watch) — one per line: <enabled> <type> <intervalMs> <command>\n");
        for (const auto& j : _jobs)
            std::fprintf(f, "%d %s %u %s\n", j.enabled ? 1 : 0, j.type.c_str(), (unsigned)j.intervalMs, j.cmd.c_str());
        std::fclose(f);
    }

    /**
     * @brief Split on runs of spaces into tokens.
     */
    static std::vector<std::string> split(const std::string& s)
    {
        std::vector<std::string> t;
        size_t i = 0;
        while (i < s.size())
        {
            while (i < s.size() && s[i] == ' ')
                ++i;
            size_t j = i;
            while (j < s.size() && s[j] != ' ')
                ++j;
            if (j > i) t.push_back(s.substr(i, j - i));
            i = j;
        }
        return t;
    }

    /**
     * @brief Render the coloured job list (or an empty-state hint).
     */
    std::vector<std::string> listJobs() const
    {
        std::vector<std::string> out;
        if (_jobs.empty())
        {
            out.push_back(_c.dim(_i.tr("no jobs — add one: /job add watch every 1s m", "keine Jobs — anlegen: /job add watch every 1s m")));
            return out;
        }
        out.push_back(_c.dim(_i.tr("── jobs ─────────────────────────────", "── Jobs ─────────────────────────────")));
        for (size_t i = 0; i < _jobs.size(); ++i)
        {
            const Job& j = _jobs[i];
            const std::string dot = j.enabled ? _c.green("●") : _c.mut("○");
            const std::string state = j.enabled ? _c.green(_i.tr("on ", "an ")) : _c.mut(_i.tr("off", "aus"));
            out.push_back(" " + _c.cyan("#" + std::to_string(i + 1)) + "  " + dot + " " + state + "  " +
                          _c.mut(j.type) + "  " + _c.txt(fmtInterval(j.intervalMs)) + "  " + _c.dim(j.cmd));
        }
        return out;
    }

    /**
     * @brief The `/job help` text (DE/EN).
     */
    std::vector<std::string> helpLines() const
    {
        std::vector<std::string> out;
        out.push_back(_c.dim(_i.tr("── /job — auto-commands (local, per target) ──", "── /job — Auto-Befehle (lokal, pro Ziel) ──")));
        out.push_back("  " + _c.cyan("/job add watch every <int> <cmd>") + "  " + _c.dim(_i.tr("e.g. /job add watch every 1s m", "z.B. /job add watch every 1s m")));
        out.push_back("  " + _c.cyan("/job list") + "                         " + _c.dim(_i.tr("list jobs", "Jobs auflisten")));
        out.push_back("  " + _c.cyan("/job <id> on | off | rm") + "           " + _c.dim(_i.tr("manage one", "einzelnen verwalten")));
        out.push_back("  " + _c.cyan("/job on|off|rm all") + "                " + _c.dim(_i.tr("bulk enable / disable / remove", "alle aktivieren / deaktivieren / entfernen")));
        out.push_back("  " + _c.cyan("/job clear") + "                        " + _c.dim(_i.tr("remove all", "alle entfernen")));
        out.push_back(_c.dim(_i.tr("intervals: 500ms · 1s · 2m — fired only when ready (never floods the bus).",
                                   "Intervalle: 500ms · 1s · 2m — feuern nur wenn ready (kein Bus-Flood).")));
        return out;
    }
};

} // namespace ftc

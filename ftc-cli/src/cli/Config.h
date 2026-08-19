/**
 * @file        Config.h
 * @brief       Flat TOML-ish user config at ~/.config/ftc/config.toml (%APPDATA%\ftc on Windows)
 * @details     Persists the user's defaults (theme, language, charset, transfer mode). Precedence:
 *              CLI flags > this file > environment/locale. Header-only, no external TOML dependency —
 *              flat `key = "value"` lines; `#` comments and `[sections]` are ignored.
 * @date        2026-08-03
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include "../core/MakeDir.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <system_error>

namespace ftc
{

class Config
{
  public:
    /**
     * @brief Absolute path to the config file (XDG_CONFIG_HOME / APPDATA aware).
     */
    std::string path() const
    {
#ifdef _WIN32
        const char* base = std::getenv("APPDATA");
        return std::string(base && *base ? base : ".") + "\\ftc\\config.toml";
#else
        const char* xdg = std::getenv("XDG_CONFIG_HOME");
        if (xdg && *xdg) return std::string(xdg) + "/ftc/config.toml";
        const char* home = std::getenv("HOME");
        return std::string(home && *home ? home : ".") + "/.config/ftc/config.toml";
#endif
    }

    /**
     * @brief Read the config file into memory; a missing file is an empty config, not an error.
     */
    void load()
    {
        _v.clear();
        std::ifstream f(path());
        if (!f) return;
        std::string line;
        while (std::getline(f, line))
        {
            const std::string t = trim(line);
            if (t.empty() || t[0] == '#' || t[0] == '[') continue;
            const size_t eq = t.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = trim(t.substr(0, eq));
            std::string val = trim(t.substr(eq + 1));
            if (val.size() >= 2 && (val.front() == '"' || val.front() == '\'') && val.back() == val.front())
                val = val.substr(1, val.size() - 2);
            if (!key.empty()) _v[key] = val;
        }
    }

    /**
     * @brief Value for @p key, or @p def if the key is absent.
     */
    std::string get(const std::string& key, const std::string& def = std::string()) const
    {
        auto it = _v.find(key);
        return it == _v.end() ? def : it->second;
    }

    /**
     * @brief Set/overwrite a key in memory (persisted on the next save()).
     */
    void set(const std::string& key, const std::string& value) { _v[key] = value; }

    /**
     * @brief The full key -> value map.
     */
    const std::map<std::string, std::string>& all() const { return _v; }

    /**
     * @brief Write the config file, creating the parent directory; false on an I/O error.
     */
    bool save() const
    {
        makeParentDir(path());
        std::ofstream f(path(), std::ios::trunc);
        if (!f) return false;
        f << "# ftc config — theme / lang / ascii / mode\n";
        for (const auto& kv : _v)
            f << kv.first << " = \"" << kv.second << "\"\n";
        return (bool)f;
    }

  private:
    /**
     * @brief Strip leading/trailing spaces, tabs and a trailing CR.
     */
    static std::string trim(const std::string& s)
    {
        size_t a = 0, b = s.size();
        while (a < b && (s[a] == ' ' || s[a] == '\t'))
            ++a;
        while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r'))
            --b;
        return s.substr(a, b - a);
    }

    /**
     * @brief Create the file's parent directory (best-effort, no shell).
     * @details std::filesystem instead of system("mkdir …") -> a $HOME/$XDG_CONFIG_HOME with a quote or
     *          shell metachar can neither break the command nor inject; also handles spaces correctly.
     */
    static void makeParentDir(const std::string& file)
    {
        const size_t sl = file.find_last_of("/\\");
        if (sl == std::string::npos) return;
        std::error_code ec;
        std::string mkErr;
        ftc::makeDirs(file.substr(0, sl), mkErr); // best-effort: failure -> the open below reports it
    }

    std::map<std::string, std::string> _v;
};

} // namespace ftc

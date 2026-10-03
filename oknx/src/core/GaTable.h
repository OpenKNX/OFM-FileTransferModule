/**
 * @file        GaTable.h
 * @brief       Group address -> datapoint type, and the decoding that follows from it.
 * @details     The DPT is on neither the bus nor the device: the device knows the object SIZE, and two
 *              bytes is `9.004 Lux` exactly as much as `7.001 pulses`. ETS reads it from the project, so
 *              this table has to be fed the same way - imported from an ETS group-address export, or
 *              given per address on the command line. Decoding itself uses the stack's own converter
 *              (`lib/knx` dptconvert), so a value cannot come out differently here than in the device.
 * @date        2026-10-02
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 */
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
    #include <direct.h>
#else
    #include <dirent.h>
    #include <sys/stat.h>
#endif
#include <cerrno>
#include <ctime>

#include "MakeDir.h" // std::filesystem::create_directories fails with EINVAL on the zig-cross Linux builds

#include "knx/dpt.h"
#include "knx/dptconvert.h"
#include "knx/knx_value.h"

namespace ftc
{

/// @brief One row of the table: what ETS knows about a group address.
struct GaInfo
{
    uint16_t main = 0; ///< DPT main group (0 = unknown)
    uint16_t sub = 0;  ///< DPT sub group
    std::string name;  ///< the group address name from the project, for the display
};

class GaTable
{
  public:
    /**
     * @brief Where the table of ONE interface lives. The export file is only needed while importing.
     * @details Bound to the interface, because that is the separation that already exists: the test rig
     *          and the productive line are reached through different interfaces, so `-i` alone decides
     *          which project's names and types apply. Without `-i` a shared default table is used.
     */
    /// @brief The stored name an interface maps to. list() returns these, so a caller comparing a raw
    ///        `--ip` against them misses every host name that carries a character the file name drops.
    static std::string key(const std::string& iface)
    {
        std::string k;
        for (char c : iface)
            if (std::isalnum((unsigned char)c) || c == '.' || c == '-') k += c;
        return k.empty() ? std::string("default") : k;
    }

    static std::string path(const std::string& iface)
    {
        const std::string key = GaTable::key(iface);
#ifdef _WIN32
        const char* base = std::getenv("APPDATA");
        return std::string(base && *base ? base : ".") + "\\oknx\\ga\\" + key + ".tsv";
#else
        const char* xdg = std::getenv("XDG_CONFIG_HOME");
        if (xdg && *xdg) return std::string(xdg) + "/oknx/ga/" + key + ".tsv";
        const char* home = std::getenv("HOME");
        return std::string(home && *home ? home : ".") + "/.config/oknx/ga/" + key + ".tsv";
#endif
    }

    /**
     * @brief Read "<ga>\t<dpt>\t<name>" lines. A missing file is an empty table, not an error.
     */
    void load(const std::string& iface)
    {
        _rows.clear();
        std::ifstream f(path(iface));
        if (!f) return;
        std::string line;
        while (std::getline(f, line))
        {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream ls(line);
            std::string ga, dpt, name;
            if (!std::getline(ls, ga, '\t')) continue;
            std::getline(ls, dpt, '\t');
            std::getline(ls, name, '\t');
            uint16_t packed = 0;
            if (!parseGa(ga, packed)) continue;
            GaInfo gi;
            parseDpt(dpt, gi.main, gi.sub);
            gi.name = name;
            _rows[packed] = gi;
        }
    }

    /**
     * @brief Replace the stored table. An import is a full picture, so a group address deleted in ETS
     *        has to disappear here too rather than live on as a stale row.
     */
    bool save(const std::string& iface, std::string& err) const
    {
        err.clear();
        const std::string p = path(iface);
        const size_t cut = p.find_last_of("/\\");
        if (cut != std::string::npos && !makeDir(p.substr(0, cut), err)) return false;
        std::ofstream f(p, std::ios::trunc);
        if (!f)
        {
            err = std::string(std::strerror(errno)) + " (" + p + ")";
            return false;
        }
        f << "# oknx group address table: <address>\\t<DPT>\\t<name>. Written by `oknx ga import`.\n";
        for (const auto& kv : _rows)
            f << gaStr(kv.first) << '\t' << dptStr(kv.second) << '\t' << kv.second.name << '\n';
        return true;
    }

    /// @brief The directory holding one table per interface.
    static std::string dir()
    {
        const std::string p = path("x");
        const size_t cut = p.find_last_of("/\\");
        return cut == std::string::npos ? std::string(".") : p.substr(0, cut);
    }

    /// @brief The interfaces a table has been imported for (the file names without their suffix).
    static std::vector<std::string> list()
    {
        // Plain readdir for the same reason as MakeDir.h: <filesystem> does not behave the same on the
        // cross-compiled Linux builds, and this is a dozen lines.
        std::vector<std::string> out;
        const std::string d = dir();
#ifdef _WIN32
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((d + "\\*.tsv").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
            {
                const std::string n = fd.cFileName;
                if (n.size() > 4) out.push_back(n.substr(0, n.size() - 4));
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
#else
        DIR* dp = opendir(d.c_str());
        if (dp != nullptr)
        {
            while (struct dirent* e = readdir(dp))
            {
                const std::string n = e->d_name;
                if (n.size() > 4 && n.compare(n.size() - 4, 4, ".tsv") == 0) out.push_back(n.substr(0, n.size() - 4));
            }
            closedir(dp);
        }
#endif
        std::sort(out.begin(), out.end());
        return out;
    }

    /// @brief Walk the rows (address, info).
    template <typename F> void each(F fn) const
    {
        for (const auto& kv : _rows) fn(kv.first, kv.second);
    }

    void clear() { _rows.clear(); }
    size_t size() const { return _rows.size(); }
    void set(uint16_t ga, const GaInfo& info) { _rows[ga] = info; }

    const GaInfo* find(uint16_t ga) const
    {
        auto it = _rows.find(ga);
        return it == _rows.end() ? nullptr : &it->second;
    }

    /// @brief "1/2/3" -> packed. Also accepts a plain number, which some exports write.
    static bool parseGa(const std::string& s, uint16_t& out)
    {
        unsigned a = 0, b = 0, c = 0;
        char tail = 0;
        if (std::sscanf(s.c_str(), "%u/%u/%u%c", &a, &b, &c, &tail) == 3 && a <= 31 && b <= 7 && c <= 255)
        {
            out = (uint16_t)((a << 11) | (b << 8) | c);
            return true;
        }
        if (std::sscanf(s.c_str(), "%u/%u%c", &a, &b, &tail) == 2 && a <= 31 && b <= 2047)
        {
            out = (uint16_t)((a << 11) | b);
            return true;
        }
        char* end = nullptr;
        long v = std::strtol(s.c_str(), &end, 10);
        if (end != nullptr && *end == 0 && v > 0 && v <= 0xFFFF)
        {
            out = (uint16_t)v;
            return true;
        }
        return false;
    }

    static std::string gaStr(uint16_t ga)
    {
        char b[16];
        std::snprintf(b, sizeof(b), "%u/%u/%u", (ga >> 11) & 0x1F, (ga >> 8) & 0x07, ga & 0xFF);
        return b;
    }

    /**
     * @brief Read a DPT in any of the spellings the exports use: "9.004", "9.4", "DPST-9-4", "DPT-9".
     *        A main group alone is legal - ETS leaves the sub group open until an object is linked.
     */
    static bool parseDpt(const std::string& in, uint16_t& main, uint16_t& sub)
    {
        main = 0;
        sub = 0;
        std::string s;
        for (char c : in)
            if (c != ' ' && c != '"') s += (char)std::tolower((unsigned char)c);
        if (s.rfind("dpst-", 0) == 0) s = s.substr(5);
        else if (s.rfind("dpt-", 0) == 0) s = s.substr(4);
        for (char& c : s)
            if (c == '-') c = '.';
        unsigned a = 0, b = 0;
        const int n = std::sscanf(s.c_str(), "%u.%u", &a, &b);
        if (n < 1 || a == 0 || a > 65535) return false;
        main = (uint16_t)a;
        sub = (n >= 2) ? (uint16_t)b : 0;
        return true;
    }

    static std::string dptStr(const GaInfo& i)
    {
        if (i.main == 0) return "";
        char b[24];
        if (i.sub == 0) std::snprintf(b, sizeof(b), "%u.*", i.main);
        else std::snprintf(b, sizeof(b), "%u.%03u", i.main, i.sub);
        return b;
    }

    /**
     * @brief Decode a group value with the stack's converter and render it as text.
     * @param small the six bits that ride in the APCI octet (used when @p len is 0)
     * @return the decoded text, or an empty string when the DPT is unknown or the converter refuses.
     *         An empty return means "show the raw octets" - never a guessed value.
     */
    static std::string decode(uint16_t main, uint16_t sub, const uint8_t* data, uint8_t len, uint8_t small)
    {
        if (main == 0) return "";
        uint8_t buf[16];
        size_t n;
        if (len == 0)
        {
            buf[0] = (uint8_t)(small & 0x3F);
            n = 1;
        }
        else
        {
            n = len > sizeof(buf) ? sizeof(buf) : len;
            std::memcpy(buf, data, n);
        }
        KNXValue v(false); // no default constructor in the stack; the decoder overwrites it
        const Dpt d((short)main, (short)sub);
        if (!KNX_Decode_Value(buf, n, d, v)) return "";

        char out[96];
        switch (main)
        {
            case 1:
                std::snprintf(out, sizeof(out), "%d", (bool)v ? 1 : 0);
                break;
            case 9:
            case 14:
                std::snprintf(out, sizeof(out), "%.2f", (double)v);
                break;
            case 16:
            {
                const char* s = (const char*)v;
                std::snprintf(out, sizeof(out), "%s", s != nullptr ? s : "");
                break;
            }
            case 10:
            case 11:
            case 19:
            {
                struct tm t = (struct tm)v;
                if (main == 10) std::snprintf(out, sizeof(out), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
                else if (main == 11) std::snprintf(out, sizeof(out), "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
                else
                    std::snprintf(out, sizeof(out), "%04d-%02d-%02d %02d:%02d:%02d", t.tm_year + 1900,
                                  t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
                break;
            }
            case 6:
            case 8:
            case 13:
                std::snprintf(out, sizeof(out), "%lld", (long long)(int64_t)v);
                break;
            default:
                std::snprintf(out, sizeof(out), "%llu", (unsigned long long)(uint64_t)v);
                break;
        }
        const char* u = unitOf(main, sub);
        if (u[0] != 0)
        {
            std::string s(out);
            return s + " " + u;
        }
        return out;
    }

    /**
     * @brief The unit a DPT carries (03_07_02). Only the ones the standard names: an unlisted DPT gets
     *        no unit rather than a plausible-looking one.
     */
    static const char* unitOf(uint16_t main, uint16_t sub)
    {
        if (main == 5 && (sub == 1 || sub == 3)) return sub == 1 ? "%" : "°";
        if (main == 5 && sub == 4) return "%";
        if (main == 7 && sub == 1) return "pulses";
        if (main == 7 && (sub == 2 || sub == 3 || sub == 4)) return "ms";
        if (main == 7 && sub == 5) return "s";
        if (main == 7 && sub == 6) return "min";
        if (main == 7 && sub == 7) return "h";
        if (main == 7 && sub == 11) return "mm";
        if (main == 7 && sub == 12) return "mA";
        if (main == 7 && sub == 13) return "lx";
        if (main == 9)
        {
            switch (sub)
            {
                case 1: return "°C";
                case 2: return "K";
                case 3: return "K/h";
                case 4: return "lx";
                case 5: return "m/s";
                case 6: return "Pa";
                case 7: return "%";
                case 8: return "ppm";
                case 9: return "m³/h";
                case 10: return "s";
                case 11: return "ms";
                case 20: return "mV";
                case 21: return "mA";
                case 22: return "W/m²";
                case 23: return "K/%";
                case 24: return "kW";
                case 25: return "l/h";
                case 26: return "l/m²";
                case 27: return "°F";
                case 28: return "km/h"; // 9.028 is wind speed in km/h - 9.005 is the m/s one
                default: return "";
            }
        }
        if (main == 12 && sub == 1) return "pulses";
        if (main == 13)
        {
            if (sub == 2) return "m³/h";
            if (sub == 10) return "Wh";
            if (sub == 11) return "VAh";
            if (sub == 12) return "VARh";
            if (sub == 13) return "kWh";
            if (sub == 100) return "s";
        }
        if (main == 14)
        {
            if (sub == 19) return "A";
            if (sub == 27) return "V";
            if (sub == 31) return "J";
            if (sub == 56) return "W";
            if (sub == 68) return "°C";
            if (sub == 76) return "m³";
        }
        return "";
    }

  private:
    static bool makeDir(const std::string& d, std::string& err) { return makeDirs(d, err); }

    std::map<uint16_t, GaInfo> _rows;
};

} // namespace ftc

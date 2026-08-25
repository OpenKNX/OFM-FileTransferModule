/**
 * @file        OtaSession.h
 * @brief       Remember unfinished knxOTA runs so a later one can pick them up. CLI.
 * @details     A LIST, keyed on (interface, target, payload CRC) -- the CRC because a rebuild is
 *              different firmware even under the same name, the target because the same firmware may
 *              be on its way to several devices. Only an unfinished run leaves an entry; a verified
 *              update removes its own, and the user can remove any of them.
 *
 *              One record used to overwrite the previous one, so a break-off at one device erased the
 *              record of another, and nothing could be thrown away at all.
 * @date        2026-08-22
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace ftc
{

/** @brief What a broken-off knxOTA run needs in order to be resumed without asking again. */
struct OtaSession
{
    bool valid = false;
    uint32_t crc = 0;      ///< CRC32 of the payload that goes on the wire -- the identity of this update
    uint32_t bytes = 0;    ///< payload size
    std::string file;      ///< firmware file as the user named it
    std::string version;   ///< version the file carries ("0.7.0")
    std::string hardware;  ///< "RP2350", "ESP32-S3", …
    std::string ip;        ///< interface it ran through
    uint16_t port = 3671;
    uint16_t pa = 0;       ///< target device
    uint32_t done = 0;     ///< bytes confirmed on the target when it broke off
    uint32_t total = 0;
    uint64_t when = 0;     ///< unix time of the break-off
};

/** @brief CRC32 (reflected, 0xEDB88320) over a buffer -- identity only, never integrity on the wire. */
inline uint32_t otaCrc32(const uint8_t* p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i)
    {
        c ^= p[i];
        for (int k = 0; k < 8; ++k)
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
    }
    return c ^ 0xFFFFFFFFu;
}

/** @brief Write the record. A failure is silent: losing the convenience must never fail an update. */
inline void otaSessionSave(const std::string& path, const OtaSession& s)
{
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "crc=%u\nbytes=%u\nfile=%s\nversion=%s\nhardware=%s\nip=%s\nport=%u\npa=%u\ndone=%u\ntotal=%u\nwhen=%llu\n",
                 (unsigned)s.crc, (unsigned)s.bytes, s.file.c_str(), s.version.c_str(), s.hardware.c_str(),
                 s.ip.c_str(), (unsigned)s.port, (unsigned)s.pa, (unsigned)s.done, (unsigned)s.total,
                 (unsigned long long)s.when);
    std::fclose(f);
}

/** @brief Read the record back; `valid` stays false when there is none or it is unreadable. */
inline OtaSession otaSessionLoad(const std::string& path)
{
    OtaSession s;
    std::FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return s;
    char line[600];
    while (std::fgets(line, sizeof(line), f))
    {
        char* eq = std::strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        std::string k(line), v(eq + 1);
        while (!v.empty() && (v.back() == '\n' || v.back() == '\r')) v.pop_back();
        if (k == "crc") s.crc = (uint32_t)std::strtoul(v.c_str(), nullptr, 10);
        else if (k == "bytes") s.bytes = (uint32_t)std::strtoul(v.c_str(), nullptr, 10);
        else if (k == "file") s.file = v;
        else if (k == "version") s.version = v;
        else if (k == "hardware") s.hardware = v;
        else if (k == "ip") s.ip = v;
        else if (k == "port") s.port = (uint16_t)std::strtoul(v.c_str(), nullptr, 10);
        else if (k == "pa") s.pa = (uint16_t)std::strtoul(v.c_str(), nullptr, 10);
        else if (k == "done") s.done = (uint32_t)std::strtoul(v.c_str(), nullptr, 10);
        else if (k == "total") s.total = (uint32_t)std::strtoul(v.c_str(), nullptr, 10);
        else if (k == "when") s.when = std::strtoull(v.c_str(), nullptr, 10);
    }
    std::fclose(f);
    s.valid = s.crc != 0 && s.pa != 0 && !s.ip.empty();
    return s;
}

/** @brief Forget the single legacy record (used once, when it has been taken over into the list). */
inline void otaSessionClear(const std::string& path) { std::remove(path.c_str()); }

/*********************************************************************
 ************************ THE LIST OF RUNS ***************************
 ********************************************************************/

static constexpr size_t OTA_RESUME_MAX = 32;          ///< entries kept; the oldest falls out
static constexpr uint64_t OTA_RESUME_MAX_AGE = 90ull * 24 * 3600; ///< dropped on read, silently

/** @brief Same run? Firmware AND target -- the same image may be on its way to several devices. */
inline bool otaSameRun(const OtaSession& a, const OtaSession& b)
{
    return a.crc == b.crc && a.pa == b.pa && a.ip == b.ip;
}

namespace detail
{
    /** @brief Tab and newline would break the one-line-per-entry file; nothing else needs escaping. */
    inline std::string otaEsc(const std::string& v)
    {
        std::string o;
        o.reserve(v.size());
        for (char ch : v)
        {
            if (ch == '\t') o += "\\t";
            else if (ch == '\n') o += "\\n";
            else if (ch == '\\') o += "\\\\";
            else o += ch;
        }
        return o;
    }
    inline std::string otaUnesc(const std::string& v)
    {
        std::string o;
        o.reserve(v.size());
        for (size_t i = 0; i < v.size(); ++i)
        {
            if (v[i] != '\\' || i + 1 >= v.size()) { o += v[i]; continue; }
            const char n = v[++i];
            o += (n == 't') ? '\t' : (n == 'n') ? '\n' : n;
        }
        return o;
    }
    inline std::vector<std::string> otaSplit(const std::string& line)
    {
        std::vector<std::string> f;
        size_t b = 0;
        for (size_t i = 0; i <= line.size(); ++i)
            if (i == line.size() || line[i] == '\t') { f.push_back(line.substr(b, i - b)); b = i + 1; }
        return f;
    }
} // namespace detail

/** @brief Write the whole list. A failure is silent: losing the convenience must never fail an update. */
inline void otaResumeSaveAll(const std::string& path, const std::vector<OtaSession>& all)
{
    if (all.empty()) { std::remove(path.c_str()); return; }
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "# ftc knxota - unfinished runs. One per line, tab separated. Edit with `ftc knxota resume`.\n");
    std::fprintf(f, "#v1\n");
    for (const OtaSession& s : all)
        std::fprintf(f, "%u\t%u\t%u\t%u\t%u\t%u\t%llu\t%s\t%s\t%s\t%s\n",
                     (unsigned)s.crc, (unsigned)s.bytes, (unsigned)s.port, (unsigned)s.pa,
                     (unsigned)s.done, (unsigned)s.total, (unsigned long long)s.when,
                     detail::otaEsc(s.ip).c_str(), detail::otaEsc(s.file).c_str(),
                     detail::otaEsc(s.version).c_str(), detail::otaEsc(s.hardware).c_str());
    std::fclose(f);
}

/**
 * @brief The list, newest first, aged entries dropped.
 * @param legacyPath  the old single-record file. Present -> taken over as one entry and removed, so
 *                    nobody loses the run they were in the middle of when this changed.
 */
inline std::vector<OtaSession> otaResumeLoad(const std::string& path, const std::string& legacyPath = std::string())
{
    std::vector<OtaSession> all;
    std::FILE* f = std::fopen(path.c_str(), "r");
    if (f)
    {
        char line[1200];
        while (std::fgets(line, sizeof(line), f))
        {
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            if (s.empty() || s[0] == '#') continue;
            const std::vector<std::string> v = detail::otaSplit(s);
            if (v.size() < 11) continue; // a short line is a damaged line, not a partial entry
            OtaSession e;
            e.crc = (uint32_t)std::strtoul(v[0].c_str(), nullptr, 10);
            e.bytes = (uint32_t)std::strtoul(v[1].c_str(), nullptr, 10);
            e.port = (uint16_t)std::strtoul(v[2].c_str(), nullptr, 10);
            e.pa = (uint16_t)std::strtoul(v[3].c_str(), nullptr, 10);
            e.done = (uint32_t)std::strtoul(v[4].c_str(), nullptr, 10);
            e.total = (uint32_t)std::strtoul(v[5].c_str(), nullptr, 10);
            e.when = std::strtoull(v[6].c_str(), nullptr, 10);
            e.ip = detail::otaUnesc(v[7]);
            e.file = detail::otaUnesc(v[8]);
            e.version = detail::otaUnesc(v[9]);
            e.hardware = detail::otaUnesc(v[10]);
            e.valid = e.crc != 0 && e.pa != 0 && !e.ip.empty();
            if (e.valid) all.push_back(e);
        }
        std::fclose(f);
    }

    if (!legacyPath.empty())
    {
        const OtaSession old = otaSessionLoad(legacyPath);
        if (old.valid)
        {
            bool known = false;
            for (const OtaSession& e : all)
                if (otaSameRun(e, old)) { known = true; break; }
            if (!known) all.push_back(old);
            otaSessionClear(legacyPath);      // taken over; never read twice
            otaResumeSaveAll(path, all);
        }
    }
    return all;
}

/** @brief Drop what is too old to still be about the firmware on anyone's disk. */
inline void otaResumeAge(std::vector<OtaSession>& all, uint64_t nowSec)
{
    std::vector<OtaSession> keep;
    keep.reserve(all.size());
    for (const OtaSession& e : all)
        if (e.when == 0 || nowSec < e.when || (nowSec - e.when) <= OTA_RESUME_MAX_AGE) keep.push_back(e);
    all.swap(keep);
}

/**
 * @brief Record a run: replace the entry for the same (interface, target, firmware), else add one.
 * @details Newest first, so the list reads the way it happened. Over the cap the oldest falls out.
 */
inline void otaResumeUpsert(const std::string& path, const OtaSession& s,
                            const std::string& legacyPath = std::string())
{
    std::vector<OtaSession> all = otaResumeLoad(path, legacyPath);
    otaResumeAge(all, s.when ? s.when : (uint64_t)std::time(nullptr));
    std::vector<OtaSession> out;
    out.reserve(all.size() + 1);
    out.push_back(s);
    for (const OtaSession& e : all)
        if (!otaSameRun(e, s)) out.push_back(e);
    if (out.size() > OTA_RESUME_MAX) out.resize(OTA_RESUME_MAX);
    otaResumeSaveAll(path, out);
}

/** @brief Remove one run. Returns whether something was removed. */
inline bool otaResumeErase(const std::string& path, const OtaSession& s)
{
    std::vector<OtaSession> all = otaResumeLoad(path);
    std::vector<OtaSession> keep;
    keep.reserve(all.size());
    for (const OtaSession& e : all)
        if (!otaSameRun(e, s)) keep.push_back(e);
    if (keep.size() == all.size()) return false;
    otaResumeSaveAll(path, keep);
    return true;
}

/** @brief Remove every run for one target, or all of them when `pa` is 0. Returns how many went. */
inline size_t otaResumeEraseWhere(const std::string& path, uint16_t pa)
{
    std::vector<OtaSession> all = otaResumeLoad(path);
    if (pa == 0) { const size_t n = all.size(); otaResumeSaveAll(path, {}); return n; }
    std::vector<OtaSession> keep;
    keep.reserve(all.size());
    for (const OtaSession& e : all)
        if (e.pa != pa) keep.push_back(e);
    const size_t gone = all.size() - keep.size();
    if (gone) otaResumeSaveAll(path, keep);
    return gone;
}

} // namespace ftc

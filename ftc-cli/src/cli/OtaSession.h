/**
 * @file        OtaSession.h
 * @brief       Remember an unfinished knxOTA run so the next one can pick it up. CLI.
 * @details     Keyed on the payload CRC (not the file name -- a rebuild is different firmware); only an
 *              unfinished run leaves a record, a verified update clears it.
 * @date        2026-08-20
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

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

/** @brief Forget the record -- called once an update is verified, so a finished job is never re-offered. */
inline void otaSessionClear(const std::string& path) { std::remove(path.c_str()); }

} // namespace ftc

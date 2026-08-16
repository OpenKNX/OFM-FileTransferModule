/**
 * @file        HostFs.h
 * @brief       Portable temp-file handling for the host CLI. CLI-agnostic CORE.
 * @details     knxOTA writes a converted, gzipped image to a temp file before uploading it, and that path
 *              has to be right on every desktop we ship: the previous ad-hoc TMPDIR-or-"/tmp" fallback in
 *              main.cpp resolves to a non-existent "/tmp" on Windows, where TMPDIR is normally unset.
 *              std::filesystem::temp_directory_path() honours TMPDIR/TMP/TEMP and GetTempPath alike.
 * @date        2026-08-11
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

namespace ftc
{

/** @brief The OS temp directory, with a "." fallback so a locked-down environment still works. */
inline std::string tempDir()
{
    std::error_code ec;
    const std::filesystem::path p = std::filesystem::temp_directory_path(ec);
    return ec ? std::string(".") : p.string();
}

/**
 * @brief A temp file that deletes itself, unless the caller keeps it.
 * @details `seq` disambiguates several files inside one run without needing a clock — the CLI has no
 *          reason to touch wall time here, and a counter cannot collide with itself.
 */
class TempFile
{
  public:
    TempFile(const std::string& stem, const std::string& ext)
    {
        static unsigned seq = 0;
        std::filesystem::path p = std::filesystem::path(tempDir());
        char name[128];
        std::snprintf(name, sizeof(name), "ftc_%s_%u%s", stem.c_str(), ++seq, ext.c_str());
        p /= name;
        _path = p.string();
    }
    ~TempFile()
    {
        if (!_keep && !_path.empty())
        {
            std::error_code ec;
            std::filesystem::remove(_path, ec); // best effort: a leftover temp file is not worth an error
        }
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    const std::string& path() const { return _path; }
    void keep() { _keep = true; } ///< --keep-temp: leave it on disk for inspection

  private:
    std::string _path;
    bool _keep = false;
};

} // namespace ftc

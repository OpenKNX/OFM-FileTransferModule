#pragma once
/**
 * @file        LittleFS.h
 * @brief       Host LittleFS shim — File over std::fstream + a host-filesystem-backed LittleFS global.
 * @details     Exactly the surface the built-in default backend uses. No FSInfo: with no ARDUINO_ARCH_*
 *              defined the client takes the #else branch, so LittleFS exposes totalBytes()/usedBytes().
 *              Contract §1.3.
 * @date        2026-07-25
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 */
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <string>

/// @brief A host file handle; copyable/movable (shared stream) so `_ftcFile = LittleFS.open(...)` works.
class File
{
  public:
    File() = default;

    // Open backing by Arduino-style mode string:
    //   "r"       -> read binary
    //   "w"/"w+"  -> create/truncate binary (write)
    //   "r+"      -> read+write existing binary, NO truncate (download resume-open)
    //   "a"/"a+"  -> append (write, positioned at end)
    File(const char* path, const char* modeStr)
    {
        _path = path ? path : "";
        const bool wr = modeStr && std::strchr(modeStr, 'w') != nullptr;
        const bool plus = modeStr && std::strchr(modeStr, '+') != nullptr;
        const bool app = modeStr && std::strchr(modeStr, 'a') != nullptr;
        const bool rplus = modeStr && modeStr[0] == 'r' && plus; // r+ : existing, read+write, no truncate

        std::ios::openmode m = std::ios::binary;
        if (wr)
            m |= std::ios::out | std::ios::trunc | (plus ? std::ios::in : std::ios::openmode(0));
        else if (app)
            m |= std::ios::out | std::ios::app | (plus ? std::ios::in : std::ios::openmode(0));
        else if (rplus)
            m |= std::ios::in | std::ios::out; // NO trunc: keep the existing bytes for a resume
        else
            m |= std::ios::in; // "r"

        _stream = std::make_shared<std::fstream>();
        _stream->open(_path, m);
        if (!_stream->is_open())
        {
            _stream.reset();
            return;
        }
        _write = wr || app || rplus;
        if (!_write) // cache size for size(); read files are seeked explicitly
        {
            _stream->seekg(0, std::ios::end);
            _size = (uint32_t)_stream->tellg();
            _stream->seekg(0, std::ios::beg);
        }
    }

    // Usable in `if (!f)` and `(bool)f`.
    explicit operator bool() const { return _stream && _stream->is_open() && _stream->good(); }

    uint32_t position()
    {
        if (!_stream) return 0;
        return (uint32_t)(_write ? _stream->tellp() : _stream->tellg());
    }

    bool seek(uint32_t offset)
    {
        if (!_stream) return false;
        _stream->clear(); // drop any prior eof/fail so the seek is honored
        if (_write)
            _stream->seekp((std::streamoff)offset, std::ios::beg);
        else
            _stream->seekg((std::streamoff)offset, std::ios::beg);
        return _stream->good();
    }

    // Sequential read; returns bytes read (0 on EOF/failure).
    int read(uint8_t* buf, size_t len)
    {
        if (!_stream || buf == nullptr || len == 0) return 0;
        _stream->read(reinterpret_cast<char*>(buf), (std::streamsize)len);
        const std::streamsize got = _stream->gcount();
        if (got < (std::streamsize)len) _stream->clear(); // partial read leaves eof set; clear for later seeks
        return (int)got;
    }

    // Append write; returns bytes written (0 on failure).
    size_t write(const uint8_t* buf, size_t len)
    {
        if (!_stream || buf == nullptr || len == 0) return 0;
        _stream->write(reinterpret_cast<const char*>(buf), (std::streamsize)len);
        _stream->flush();
        return _stream->good() ? len : 0;
    }

    void close()
    {
        if (_stream && _stream->is_open()) _stream->close();
        _stream.reset();
    }

    uint32_t size() { return _size; }

    // Cut the backing file to `newSize` (download resume-open: drop the partial last chunk). Flush first so
    // any buffered bytes land, then resize on the path; the caller re-seeks afterwards. false on failure.
    bool truncate(uint32_t newSize)
    {
        if (!_stream || _path.empty()) return false;
        _stream->flush();
        std::error_code ec;
        std::filesystem::resize_file(_path, (std::uintmax_t)newSize, ec);
        if (ec) return false;
        _stream->clear(); // drop any eof/fail from the flush so later seeks are honored
        return true;
    }

  private:
    std::shared_ptr<std::fstream> _stream;
    bool _write = false;
    uint32_t _size = 0;
    std::string _path;
};

/// @brief Host filesystem facade mirroring the Arduino LittleFS global.
class LittleFSHost
{
  public:
    // mode: "r" read · "w"/"w+" create+truncate · "r+" existing read+write (no truncate) · "a" append.
    File open(const char* path, const char* mode)
    {
        return File(path, mode ? mode : "r");
    }

    // Capacity/used of the filesystem holding the working directory (host takes the #else branch).
    uint64_t totalBytes()
    {
        std::error_code ec;
        const auto s = std::filesystem::space(std::filesystem::current_path(ec), ec);
        return ec ? 0 : (uint64_t)s.capacity;
    }
    uint64_t usedBytes()
    {
        std::error_code ec;
        const auto s = std::filesystem::space(std::filesystem::current_path(ec), ec);
        return ec ? 0 : (uint64_t)(s.capacity - s.available);
    }
};

// The Arduino-style global (defined in shim.cpp).
extern LittleFSHost LittleFS;

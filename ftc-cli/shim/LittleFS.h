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

    // Open backing; write=false -> read binary, write=true -> create/truncate binary.
    File(const char* path, bool write)
    {
        _stream = std::make_shared<std::fstream>();
        const auto mode = write ? (std::ios::out | std::ios::binary | std::ios::trunc)
                                : (std::ios::in | std::ios::binary);
        _stream->open(path ? path : "", mode);
        if (!_stream->is_open())
        {
            _stream.reset();
            return;
        }
        _write = write;
        if (!write) // cache size for size(); read files are seeked explicitly
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

  private:
    std::shared_ptr<std::fstream> _stream;
    bool _write = false;
    uint32_t _size = 0;
};

/// @brief Host filesystem facade mirroring the Arduino LittleFS global.
class LittleFSHost
{
  public:
    // mode: "r" -> read, anything containing 'w' -> create/truncate write.
    File open(const char* path, const char* mode)
    {
        const bool write = (mode != nullptr) && std::strchr(mode, 'w') != nullptr;
        return File(path, write);
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

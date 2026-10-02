/**
 * @file        MakeDir.h
 * @brief       Create a directory and its parents, without <filesystem>. CORE.
 * @details     `std::filesystem::create_directories` failed with EINVAL on a Raspberry Pi build -- the
 *              Linux targets are cross-compiled with `zig c++`, which brings its own libc++ and pins an
 *              old glibc, and its <filesystem> does not behave the same everywhere. The operation itself is
 *              fifteen lines of mkdir, so it does not need a library that varies by toolchain.
 *
 *              Three of the four callers used to pass a std::error_code and drop it, so on that Pi the
 *              config file, the console history and the monitor dumps all failed to get their directory
 *              and said nothing. This reports what went wrong and where, so a failure is diagnosable
 *              instead of silent.
 * @date        2026-08-17
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include <cerrno>
#include <cstring>
#include <string>

#ifdef _WIN32
    #include <direct.h>
    #include <windows.h>
#else
    #include <sys/stat.h>
    #include <sys/types.h>
#endif

namespace ftc
{

/**
 * @brief Create @p path and every missing parent. True when it exists afterwards.
 * @param err on failure: the component that could not be created and why.
 */
inline bool makeDirs(const std::string& path, std::string& err)
{
    err.clear();
    if (path.empty()) { err = "empty path"; return false; }

#ifdef _WIN32
    const char sep = '\\';
    const bool altSep = true;
#else
    const char sep = '/';
    const bool altSep = false;
#endif

    std::string cur;
    cur.reserve(path.size());
    size_t i = 0;

#ifdef _WIN32
    // Keep a drive prefix ("C:") or a UNC root together: mkdir on half of it is meaningless.
    if (path.size() >= 2 && path[1] == ':') { cur = path.substr(0, 2); i = 2; }
#endif
    if (i < path.size() && (path[i] == '/' || (altSep && path[i] == '\\')))
    {
        cur += path[i];
        ++i;
    }

    while (i <= path.size())
    {
        const bool end = (i == path.size());
        const bool boundary = end || path[i] == '/' || (altSep && path[i] == '\\');
        if (!boundary)
        {
            cur += path[i++];
            continue;
        }
        if (!cur.empty() && cur != "/" && !(cur.size() == 2 && cur[1] == ':'))
        {
#ifdef _WIN32
            if (_mkdir(cur.c_str()) != 0 && errno != EEXIST)
#else
            if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST)
#endif
            {
                err = "'" + cur + "': " + std::strerror(errno);
                return false;
            }
        }
        if (end) break;
        cur += sep;
        ++i;
        while (i < path.size() && (path[i] == '/' || (altSep && path[i] == '\\'))) ++i; // collapse //
    }
    return true;
}

} // namespace ftc

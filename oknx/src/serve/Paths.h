/**
 * @file        Paths.h
 * @brief       Where `oknx serve` keeps its files: key, rules, worker state, logs.
 * @details     Everything lives next to the config file (~/.config/oknx or %APPDATA%\oknx), in `serve/`.
 * @date        2026-10-19
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include "cli/Config.h"
#include "core/MakeDir.h"

#include <cstdint>
#include <string>

namespace ftc
{
namespace serve
{

#ifdef _WIN32
static const char PATH_SEP = '\\';
#else
static const char PATH_SEP = '/';
#endif

/** @brief The oknx config directory (the one holding config.toml). */
inline std::string configDir()
{
    const std::string p = Config().path();
    const size_t cut = p.find_last_of("/\\");
    return cut == std::string::npos ? std::string(".") : p.substr(0, cut);
}

/** @brief The serve directory; created on demand with @p create. */
inline std::string serveDir(bool create = false)
{
    const std::string d = configDir() + PATH_SEP + "serve";
    if (create)
    {
        std::string err;
        makeDirs(d, err);
    }
    return d;
}

/** @brief A file name part for an interface: 11.11.0.210:3671 -> 11.11.0.210_3671. */
inline std::string ifaceSlug(const std::string& ip, uint16_t port) { return ip + "_" + std::to_string((unsigned)port); }

inline std::string workerStatePath(const std::string& ip, uint16_t port)
{
    return serveDir(true) + PATH_SEP + "worker-" + ifaceSlug(ip, port) + ".json";
}

inline std::string workerLogPath(const std::string& ip, uint16_t port)
{
    return serveDir(true) + PATH_SEP + "worker-" + ifaceSlug(ip, port) + ".log";
}

inline std::string keyPath() { return configDir() + PATH_SEP + "serve.key"; }
inline std::string rulesPathDefault() { return configDir() + PATH_SEP + "rules.json"; }
inline std::string serveLogPath() { return serveDir(true) + PATH_SEP + "serve.log"; }
inline std::string opLogPath() { return serveDir(true) + PATH_SEP + "operations.log"; }

} // namespace serve
} // namespace ftc

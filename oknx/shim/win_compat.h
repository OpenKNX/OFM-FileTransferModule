/**
 * @file        win_compat.h
 * @brief       The handful of POSIX names the stack's sources use that MinGW does not provide.
 * @details     Force-included (-include) on the Windows cross builds only, so `lib/knx` stays untouched:
 *              it is a shared library and a fix there would reach every product for the sake of one tool.
 * @date        2026-10-02
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 */
#pragma once

// Also force-included into the C sources (miniz, aes), so: no C++ headers, no C++ keywords here.
#if defined(_WIN32) && defined(__cplusplus)
    #include <time.h>

/// @brief POSIX gmtime_r on top of the Windows gmtime_s (whose argument order is the other way round).
static inline struct tm* gmtime_r(const time_t* t, struct tm* out)
{
    return (gmtime_s(out, t) == 0) ? out : NULL;
}

/// @brief Same for localtime_r, which the same sources reach for.
static inline struct tm* localtime_r(const time_t* t, struct tm* out)
{
    return (localtime_s(out, t) == 0) ? out : NULL;
}
#endif

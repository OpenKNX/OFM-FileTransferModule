// ┬────┴  OFM-FileTransferModule / ftc-cli
// ■ KNX   2026 OpenKNX - Erkan Çolak
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026, Erkan Çolak
//
// Umbrella host shim. Replaces the real OGM-Common/src/OpenKNX.h (which #errors unless an
// ARDUINO_ARCH_* is set). Pulls the two sub-shims, provides the two stack macros, millis(), and the
// std headers the four FTC files assume the stack transitively includes. Contract §1.1, §6, §7.
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "knx_shim.h"     // SecurityControl/DataSecurity, HostBau, `knx` (pulls knx_ip_tunnel.h)
#include "openknx_shim.h" // OpenKNX::Module/Base, Log::Logger, Console, Facade, `openknx`

// Headline color the console/logger use (real: OGM-Common Console.h).
#ifndef CONSOLE_HEADLINE_COLOR
    #define CONSOLE_HEADLINE_COLOR 33
#endif

// Module version string. Normally injected by platformio.ini build_flags (-D, from library.json);
// this is only the fallback for a raw compile -- keep it in sync with ../../library.json.
#ifndef MODULE_FileTransferModule_Version
    #define MODULE_FileTransferModule_Version "0.1.6"
#endif

/// @brief Arduino millis() stand-in: milliseconds since the first call, via steady_clock.
inline uint32_t millis()
{
    static const auto t0 = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(now - t0).count();
}

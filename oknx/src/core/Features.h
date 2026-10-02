/**
 * @file        Features.h
 * @brief       Single CheckFeatures(102) probe — one frame, one answer. CLI-agnostic CORE.
 * @details     knxOTA has to know a target's access state before it does anything else, and it has to be
 *              able to re-ask once a second while a human hunts for the programming button. The shared
 *              client offers no bare feature probe; its cheapest public route (requestDeviceInfo) costs
 *              about twenty bus round-trips, which would flood the line for minutes. The tunnel already
 *              exposes the exact primitive, so this sends the same wire frame the client would and reads
 *              the one answer byte back.
 *
 *              Two rules the caller must keep, both enforced here: the response callback is process-global,
 *              so a probe may only run while the shared client is idle, and the callback is handed back on
 *              every exit path.
 * @date        2026-08-11
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include <cstdint>
#include <functional>

#include "../knx_ip_tunnel.h"

namespace ftc
{

/** @brief CheckFeatures(102) result bits — see FileTransferModule::cmdCheckFeatures. */
enum : uint8_t
{
    FEAT_RESUME = 0x01,          ///< the server can resume a partial upload
    FEAT_UPDATE = 0x02,          ///< the target can flash itself (RP2040/RP2350 and ESP32)
    FEAT_FAST = 0x04,            ///< windowed fast upload (cmd 44/45)
    FEAT_CONSOLE = 0x08,         ///< the obj-160 console tunnel exists
    FEAT_AUTH_REQUIRED = 0x10,   ///< access stage is Password
    FEAT_WRITES_DISABLED = 0x20, ///< writes are refused right now
    FEAT_GZIP_UPDATE = 0x40,     ///< a staged firmware may be sent compressed; the device unpacks it itself
    FEAT_DELTA = 0x80,           ///< the device rebuilds firmware from a patch; without it never send one
};

namespace detail
{

constexpr uint8_t FTC_OBJ_DATA = 159;   ///< the file-transfer command object
constexpr uint8_t FTC_CMD_FEATURES = 102;
constexpr uint32_t FEATURE_TIMEOUT_MS = 800; ///< an old server simply never answers cmd 102

inline volatile bool g_featSeen = false;
inline uint8_t g_featBits = 0;
inline uint16_t g_featPa = 0;

/** @brief Response trampoline — the tunnel callback is a plain function pointer, so it cannot capture. */
inline void featureAnswer(uint16_t pa, uint8_t objectIndex, uint8_t propertyId, uint8_t* data, uint8_t length)
{
    if (pa != g_featPa || objectIndex != FTC_OBJ_DATA || propertyId != FTC_CMD_FEATURES) return;
    if (length < 1 || data == nullptr) return;
    g_featBits = data[0];
    g_featSeen = true; // published last: the reader polls this flag
}

} // namespace detail

/**
 * @brief Ask one device which file-transfer features it offers, and whether writes are allowed right now.
 * @param pump   drives the tunnel and the shared client for one pass; called repeatedly while waiting.
 * @return false on timeout — which for this command means "an old server that does not know cmd 102",
 *         not necessarily an absent device.
 */
inline bool probeFeatures(KnxIpTunnel& tunnel, uint16_t pa, uint8_t& bitsOut,
                          const std::function<void()>& pump, const std::function<uint64_t()>& nowMs,
                          uint32_t timeoutMs = detail::FEATURE_TIMEOUT_MS)
{
    detail::g_featSeen = false;
    detail::g_featBits = 0;
    detail::g_featPa = pa;

    const FtcResponseCb prev = tunnel.responseCallback(); // hand it BACK, do not clear it
    tunnel.setResponseCallback(&detail::featureAnswer);
    const bool sent = tunnel.sendCommand(pa, detail::FTC_OBJ_DATA, detail::FTC_CMD_FEATURES, nullptr, 0);
    if (!sent)
    {
        tunnel.setResponseCallback(prev);
        return false;
    }

    const uint64_t until = nowMs() + timeoutMs;
    while (!detail::g_featSeen && nowMs() < until)
        pump();

    tunnel.setResponseCallback(prev); // restore whatever was installed before this probe
    if (!detail::g_featSeen) return false;
    bitsOut = detail::g_featBits;
    return true;
}

} // namespace ftc

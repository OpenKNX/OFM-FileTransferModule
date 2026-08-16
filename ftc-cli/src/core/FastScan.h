/**
 * @file        FastScan.h
 * @brief       Is this interface's acknowledgement worth listening to? CORE.
 * @details     On TP1 every device present answers any frame addressed to it at data-link level, whatever it
 *              thinks of the request. That acknowledgement travels back to a tunnel client in the cEMI
 *              L_Data.con, so one frame per address is a complete presence test — it finds BCU1, BCU2,
 *              System 7 and tunnel addresses alike, all of which stay invisible to an application-level
 *              read. Measured on a line with 70 devices: the connectionless DeviceDescriptor sweep found
 *              37 of them (and `deep 3` found the same 37 — the misses are device classes, not lost
 *              answers), while the connection-oriented sweep found all of them in 60 s. This finds them in
 *              a fraction of that, because it waits for one acknowledgement instead of building a
 *              connection per address.
 *
 *              The sweep itself stays where it belongs — in the shared scan machine, which now accepts the
 *              acknowledgement through `ftcScanAck()`. What is left here is the one question that machine
 *              cannot answer for itself: is this interface's acknowledgement true? Not all are. An
 *              interface that confirms on accepting the frame rather than on transmitting it reports
 *              every address as present, and that is not detectable except by asking about an address
 *              that is almost certainly empty.
 * @date        2026-08-12
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include <cstdint>
#include <functional>
#include <vector>

#include "../knx_ip_tunnel.h"

namespace ftc
{

/** @brief What the probe needs from the caller, so it stays free of CLI globals. */
struct FastScanDeps
{
    std::function<void()> pump;      ///< one tunnel pass
    std::function<uint64_t()> nowMs; ///< monotonic clock
    std::function<bool()> aborted;   ///< true once the user pressed Ctrl-C
};

namespace detail
{

// Present devices ack within one bus round trip, absent ones after the sender's repetitions (measured
// 8-35 / 35-92 ms). 400 ms leaves headroom over both and still bounds a 255-address sweep.
constexpr uint32_t FAST_CON_TMO_MS = 400;

inline volatile bool g_conSeen = false;
inline bool g_conOk = false;
inline uint16_t g_conPa = 0;

inline void fastConAnswer(uint16_t pa, bool ok)
{
    if (pa != g_conPa) return;
    g_conOk = ok;
    g_conSeen = true; // published last
}

/** @brief One address: send a frame, wait for its confirmation. `seen` is false when none arrived. */
inline bool probeOne(KnxIpTunnel& tunnel, uint16_t pa, const FastScanDeps& d, bool& seen)
{
    g_conSeen = false;
    g_conOk = false;
    g_conPa = pa;
    if (!tunnel.sendDeviceDescriptorRead(pa))
    {
        seen = false;
        return false;
    }
    const uint64_t until = d.nowMs() + FAST_CON_TMO_MS;
    while (!g_conSeen && d.nowMs() < until)
        d.pump();
    seen = g_conSeen;
    // No confirmation at all counts as absent: a present device is acknowledged well inside the window,
    // and an interface that stays silent tells us nothing we could act on either way.
    return seen && g_conOk;
}

} // namespace detail

/**
 * @brief Does this interface report the real acknowledgement, or does it confirm everything?
 * @details Asks about an address that is almost certainly empty. A truthful interface says "not
 *          acknowledged"; one that confirms on acceptance says "delivered" and is therefore useless for
 *          presence. Two candidates are tried so a genuinely occupied test address cannot mislead us.
 */
inline bool interfaceReportsAcks(KnxIpTunnel& tunnel, uint16_t lineBase, const FastScanDeps& d)
{
    const FtcConCb prev = tunnel.confirmCallback();
    tunnel.setConfirmCallback(&detail::fastConAnswer);
    bool truthful = false;
    for (uint16_t probe : {(uint16_t)(lineBase | 0xFE), (uint16_t)(lineBase | 0xFD)})
    {
        bool seen = false;
        const bool ack = detail::probeOne(tunnel, probe, d, seen);
        if (seen && !ack) // it admitted that nobody answered -> it reports the truth
        {
            truthful = true;
            break;
        }
        if (!seen) break; // no confirmation at all: nothing to build on
    }
    tunnel.setConfirmCallback(prev);
    return truthful;
}

} // namespace ftc

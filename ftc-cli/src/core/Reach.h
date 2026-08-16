/**
 * @file        Reach.h
 * @brief       Is the target actually there? One frame, asked before anything expensive. CORE.
 * @details     A transfer to an address nobody answers takes over two minutes to give up, and the message
 *              at the end blames the bus ("busy?") for what is usually a wrong address or an interface on
 *              the wrong line. One DeviceDescriptor_Read settles it in about thirty milliseconds.
 *
 *              That frame is the right question because EVERY KNX device answers it -- it is not a
 *              file-transfer feature, so the check says "this address is occupied", not "this address runs
 *              the software I want". Being wrong in that direction is harmless: the command then fails as
 *              it would have anyway, only later.
 *
 *              The interface's own line is compared too, but only as an explanation offered AFTER a failed
 *              probe. A target behind a coupler is perfectly legal, and whether the coupler forwards is not
 *              something we can know from here -- so a device that answers is never questioned, however far
 *              away it lives.
 * @date        2026-08-15
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include <cstdint>
#include <functional>

#include "../knx_ip_tunnel.h"

namespace ftc
{

/** @brief What the probe needs from the caller, so it stays free of CLI globals. */
struct ReachDeps
{
    std::function<void()> pump;
    std::function<uint64_t()> nowMs;
    std::function<bool()> aborted;
};

namespace detail
{

// One bus round trip is 10-45 ms on a healthy line; this covers a busy one plus a coupler hop.
constexpr uint32_t REACH_TMO_MS = 1200;
constexpr int REACH_TRIES = 2; // a single lost frame on a loaded bus must not condemn the device

inline volatile bool g_reachSeen = false;
inline uint16_t g_reachPa = 0;

inline void reachAnswer(uint16_t pa, uint8_t, const uint8_t*)
{
    if (pa == g_reachPa) g_reachSeen = true;
}

} // namespace detail

/**
 * @brief Ask one address whether it is there. True as soon as it answers.
 * @param triesUsed how many attempts it took (or were spent in vain) -- for an honest report.
 * @param msSpent   how long the whole check took.
 */
inline bool deviceAnswers(KnxIpTunnel& tunnel, uint16_t pa, const ReachDeps& d, int& triesUsed, uint32_t& msSpent)
{
    const FtcDdCb prev = tunnel.deviceDescriptorCallback();
    tunnel.setDeviceDescriptorCallback(&detail::reachAnswer);
    const uint64_t t0 = d.nowMs();
    bool alive = false;
    triesUsed = 0;

    for (int i = 0; i < detail::REACH_TRIES && !alive; ++i)
    {
        if (d.aborted && d.aborted()) break;
        ++triesUsed;
        detail::g_reachSeen = false;
        detail::g_reachPa = pa;
        if (!tunnel.sendDeviceDescriptorRead(pa)) break;
        const uint64_t until = d.nowMs() + detail::REACH_TMO_MS;
        while (!detail::g_reachSeen && d.nowMs() < until)
        {
            if (d.aborted && d.aborted()) break;
            d.pump();
        }
        alive = detail::g_reachSeen;
    }

    tunnel.setDeviceDescriptorCallback(prev);
    msSpent = (uint32_t)(d.nowMs() - t0);
    return alive;
}

/** @brief True when two addresses sit on different lines, i.e. only a coupler can carry between them. */
inline bool crossesLine(uint16_t a, uint16_t b)
{
    return (a & 0xFF00) != (b & 0xFF00);
}

} // namespace ftc

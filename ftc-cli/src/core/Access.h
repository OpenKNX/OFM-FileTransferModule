/**
 * @file        Access.h
 * @brief       FTC access protection: recognise the stage and resolve it inside the running command. CORE.
 * @details     A device can be locked four ways, chosen in ETS: open, needs the programming button, needs a
 *              password, or shut entirely. Telling the user to quit, run a login and start over is the one
 *              thing knxOTA must never do — a firmware update is a half-hour commitment and the wizard has
 *              already read the bus by the time this matters.
 *
 *              Two bits come back from CheckFeatures(102) and they do not separate "stage Off" from "stage
 *              ProgMode with the button not pressed": both report writes-disabled without the password bit.
 *              The programming-mode flag (PID 54) closes that gap — if the button IS held and writes are
 *              still refused, the stage can only be Off, and only ETS can change it.
 * @date        2026-08-11
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include <cstdint>
#include <functional>
#include <string>

#include "Features.h"

namespace ftc
{

/** @brief What stands between us and writing to the target. */
enum class Access
{
    Open,          ///< writes are allowed right now
    NeedPassword,  ///< stage Password, not signed in
    Blocked,       ///< writes refused: either the programming button, or stage Off — not yet distinguishable
    LockedOff,     ///< proven stage Off — nothing but an ETS download changes this
    Unknown,       ///< the target never answered CheckFeatures (a very old server)
};

/** @brief Everything Access needs from the outside, so this stays free of CLI globals. */
struct AccessDeps
{
    std::function<void()> pump;              ///< one tunnel + client pass
    std::function<uint64_t()> nowMs;         ///< monotonic clock
    std::function<bool()> aborted;           ///< true once the user pressed Ctrl-C
    std::function<bool()> clientBusy;        ///< the shared client owns the response callback while busy
    std::function<void(const char*)> login;  ///< requestLogin(pa, password) + pump to quiescence
};

namespace detail
{

inline volatile bool g_progSeen = false;
inline bool g_progOn = false;
inline uint16_t g_progPa = 0;
constexpr uint8_t PID_PROG_MODE = 54;

inline void progAnswer(uint16_t pa, uint8_t objectIndex, uint8_t propertyId, const uint8_t* data, uint8_t length)
{
    if (pa != g_progPa || objectIndex != 0 || propertyId != PID_PROG_MODE) return;
    if (length < 1 || data == nullptr) return;
    g_progOn = (data[0] & 0x01) != 0;
    g_progSeen = true;
}

} // namespace detail

/**
 * @brief Read the target's programming-mode flag (PID 54). False when it did not answer in time.
 * @details Stays readable even at stage Off: the access gate covers the file-transfer objects, not the
 *          device object, so this is exactly the independent signal the two feature bits are missing.
 */
inline bool probeProgMode(KnxIpTunnel& tunnel, uint16_t pa, bool& onOut, const AccessDeps& d,
                          uint32_t timeoutMs = 1500)
{
    detail::g_progSeen = false;
    detail::g_progOn = false;
    detail::g_progPa = pa;

    const FtcPropCb prev = tunnel.propertyCallback();
    tunnel.setPropertyCallback(&detail::progAnswer);
    if (!tunnel.sendPropertyValueRead(pa, 0, detail::PID_PROG_MODE, 1, 1))
    {
        tunnel.setPropertyCallback(prev);
        return false;
    }
    const uint64_t until = d.nowMs() + timeoutMs;
    while (!detail::g_progSeen && d.nowMs() < until)
        d.pump();
    tunnel.setPropertyCallback(prev);
    if (!detail::g_progSeen) return false;
    onOut = detail::g_progOn;
    return true;
}

/** @brief Turn the two CheckFeatures bits plus the programming-mode flag into one stage. */
inline Access classifyAccess(bool answered, uint8_t bits, bool progModeKnown, bool progModeOn)
{
    if (!answered) return Access::Unknown;
    if ((bits & FEAT_WRITES_DISABLED) == 0) return Access::Open;
    if ((bits & FEAT_AUTH_REQUIRED) != 0) return Access::NeedPassword;
    // Writes refused and no password stage: this is ProgMode with the button up, or stage Off — and the two
    // are indistinguishable from here. Only ONE direction is provable: if the button IS held and writes are
    // still refused, ProgMode would have allowed them, so it can only be Off. With the button up we know
    // nothing, and saying "press the programming button" would be a guess dressed as an instruction —
    // verified on a device whose ETS stage was Off while its button was up.
    if (progModeKnown && progModeOn) return Access::LockedOff;
    return Access::Blocked;
}

/** @brief One reading of the target's access state. */
struct AccessState
{
    Access stage = Access::Unknown;
    bool answered = false;
    uint8_t bits = 0;
    bool progModeKnown = false;
    bool progModeOn = false;
};

/** @brief Ask the target where it stands right now. */
inline AccessState readAccess(KnxIpTunnel& tunnel, uint16_t pa, const AccessDeps& d)
{
    AccessState s;
    // The callback is process-global: probing while the shared client owns it would steal its answers.
    if (d.clientBusy && d.clientBusy()) return s;
    s.answered = probeFeatures(tunnel, pa, s.bits, d.pump, d.nowMs);
    if (s.answered && (s.bits & FEAT_WRITES_DISABLED) != 0 && (s.bits & FEAT_AUTH_REQUIRED) == 0)
        s.progModeKnown = probeProgMode(tunnel, pa, s.progModeOn, d);
    s.stage = classifyAccess(s.answered, s.bits, s.progModeKnown, s.progModeOn);
    return s;
}

/**
 * @brief Wait, polling once a second, until writes become allowed or the budget runs out.
 * @details Used both for "press the programming button" and for "go and change the ETS parameter" — the
 *          command stays open and keeps everything it has already read, so nothing has to be repeated.
 *          `onTick` gets the elapsed seconds so the caller can draw a live line.
 */
inline bool waitForWrites(KnxIpTunnel& tunnel, uint16_t pa, const AccessDeps& d, uint32_t budgetS,
                          const std::function<void(uint32_t)>& onTick)
{
    const uint64_t start = d.nowMs();
    uint64_t nextProbe = start;
    uint32_t drawn = 0xFFFFFFFFu; // redraw only when the second changes — this loop runs thousands of
                                  // times a second, and a line redrawn every pass is megabytes of output
    for (;;)
    {
        if (d.aborted && d.aborted()) return false;
        const uint64_t now = d.nowMs();
        const uint32_t elapsed = (uint32_t)((now - start) / 1000);
        if (elapsed >= budgetS) return false;
        if (onTick && elapsed != drawn)
        {
            onTick(elapsed);
            drawn = elapsed;
        }
        if (now >= nextProbe)
        {
            uint8_t bits = 0;
            if (probeFeatures(tunnel, pa, bits, d.pump, d.nowMs) && (bits & FEAT_WRITES_DISABLED) == 0)
                return true;
            nextProbe = d.nowMs() + 1000; // once a second: enough to feel live, gentle on the bus
        }
        d.pump();
    }
}

} // namespace ftc

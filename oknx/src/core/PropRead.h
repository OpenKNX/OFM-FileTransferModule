/**
 * @file        PropRead.h
 * @brief       One property read, the way `oknx prop read` and `oknx busprop read` do it, for the command line
 *              and for `oknx serve` alike.
 * @details     prop: local device management on the interface itself (cEMI M_PropRead over its own device
 *              management connection). busprop: A_PropertyValue_Read to a TP device over a tunnel the caller
 *              owns; the caller's property callback is put back afterwards, so the FTC client keeps its own.
 * @date        2026-10-19
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include "core/DeviceMgmt.h"
#include "knx_ip_tunnel.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ftc
{

/** @brief What one local property read brought back. */
struct PropReadResult
{
    bool connected = false;
    std::string connectError;
    DmRead outcome = DmRead::NoAnswer;
    uint8_t propError = 0; ///< cEMI error code of a negative confirmation
    std::vector<uint8_t> data;
};

/** @brief M_PropRead of @p noe elements from @p startIdx (12 bit) on the interface at @p ip. */
inline PropReadResult propReadLocal(const std::string& ip, uint16_t port, uint16_t iot, uint8_t inst, uint8_t pid,
                                    uint16_t startIdx, uint8_t noe)
{
    PropReadResult r;
    DevMgmtSession dm;
    if (!dm.open(ip, port, &r.connectError)) return r;
    r.connected = true;
    uint8_t out[256];
    size_t outLen = 0;
    r.outcome = dm.readEx(iot, inst, pid, startIdx, noe, out, sizeof(out), outLen, &r.propError);
    r.data.assign(out, out + outLen);
    dm.close();
    return r;
}

namespace detail_busprop
{
// The tunnel property callback is a plain function pointer: the one pending request is matched against its
// own PA / object index / PID here, everything else is ignored.
inline volatile bool seen = false;
inline uint16_t wantPa = 0;
inline uint8_t wantObj = 0, wantPid = 0, len = 0;
inline uint8_t data[64] = {0};
inline void answer(uint16_t pa, uint8_t objectIndex, uint8_t propertyId, const uint8_t* d, uint8_t length)
{
    if (pa != wantPa || objectIndex != wantObj || propertyId != wantPid) return;
    len = (uint8_t)(length > sizeof(data) ? sizeof(data) : length);
    if (d != nullptr && len > 0) std::memcpy(data, d, len);
    seen = true;
}
} // namespace detail_busprop

/** @brief What one bus property read brought back. */
struct BusPropResult
{
    bool sent = false;
    bool answered = false;
    std::vector<uint8_t> data;
};

/**
 * @brief A_PropertyValue_Read on @p tun (already connected) to @p pa; waits up to @p waitMs for the answer.
 */
inline BusPropResult busPropRead(KnxIpTunnel& tun, uint16_t pa, uint8_t objIdx, uint8_t pid, uint8_t noe, uint16_t startIdx,
                                 uint32_t waitMs = 3000)
{
    namespace bp = detail_busprop;
    BusPropResult r;
    bp::wantPa = pa;
    bp::wantObj = objIdx;
    bp::wantPid = pid;
    bp::seen = false;
    bp::len = 0;
    const FtcPropCb before = tun.propertyCallback();
    tun.setPropertyCallback(&bp::answer);
    r.sent = tun.sendPropertyValueRead(pa, objIdx, pid, noe, startIdx);
    if (r.sent)
    {
        const uint64_t until = detail::nowMs() + waitMs;
        while (!bp::seen && detail::nowMs() < until) tun.pump();
        if (bp::seen)
        {
            r.answered = true;
            r.data.assign(bp::data, bp::data + bp::len);
        }
    }
    tun.setPropertyCallback(before);
    return r;
}

/** @brief The application program run states (PID_RUN_STATE_CONTROL, 03_05_01). */
inline const char* runStateName(int st)
{
    switch (st)
    {
        case 0: return "halted";
        case 1: return "running";
        case 2: return "ready";
        case 3: return "terminated";
        case 4: return "starting";
        case 5: return "shutting down";
        default: return "?";
    }
}

/** @brief What `runstate` found and did. States: >= 0 the value, -2 no answer, -3 not implemented (empty answer). */
struct RunStateResult
{
    int appIdx = -1; ///< index of the application program object (type 3), -1 = none found
    int before = -2;
    int after = -2;
    bool sendFailed = false;
};

/**
 * @brief Read (and with @p event 1 start / 2 stop, set) the run state of the application program on @p pa.
 * @details The object INDEX of the application program is device specific (measured on 5.0.3 and 5.0.8: index 3
 *          is the group object table, the program sits at 4), so it is looked up through PID 1. Object indices are
 *          contiguous: no answer or an empty one on PID 1 ends the sweep.
 */
inline RunStateResult runState(KnxIpTunnel& tun, uint16_t pa, int event)
{
    RunStateResult r;
    for (uint8_t i = 0; i <= 12; ++i)
    {
        const BusPropResult t = busPropRead(tun, pa, i, 1, 1, 1, 600);
        if (!t.sent || !t.answered || t.data.empty()) break;
        if (t.data.size() >= 2 && t.data[0] == 0x00 && t.data[1] == 0x03)
        {
            r.appIdx = (int)i;
            break;
        }
    }
    if (r.appIdx < 0) return r;
    auto readState = [&]() -> int {
        const BusPropResult t = busPropRead(tun, pa, (uint8_t)r.appIdx, 6, 1, 1, 3000);
        if (!t.sent || !t.answered) return -2;
        if (t.data.empty()) return -3; // answered, but the property carries nothing: not implemented
        return (int)t.data[0];
    };
    r.before = readState();
    if (r.before < 0 || event < 0) return r;
    const uint8_t ev = (uint8_t)event;
    if (!tun.sendPropertyValueWrite(pa, (uint8_t)r.appIdx, 6, 1, 1, &ev, 1))
    {
        r.sendFailed = true;
        return r;
    }
    r.after = readState();
    return r;
}

namespace detail_restart
{
inline int err = -1;
inline uint16_t secs = 0;
inline void answer(uint16_t, uint8_t e, uint16_t t)
{
    err = (int)e;
    secs = t;
}
} // namespace detail_restart

/** @brief What a restart request did: sent, and for a master reset the device's answer (-1 = none in 3 s). */
struct RestartResult
{
    bool sent = false;
    bool connectionOriented = false;
    int masterError = -1;
    uint16_t masterSeconds = 0;
};

/**
 * @brief A_Restart to @p pa: basic, or master reset with @p erase (knx_types.h EraseCode).
 * @details 06 Profiles 4.2 p.37 row 11: the CONNECTION-ORIENTED restart is mandatory for every profile, the
 *          connectionless one optional, so a transport connection is opened first. A master reset is answered
 *          (waited for 3 s), a basic restart is not (400 ms for the frame to go out). The session is closed
 *          afterwards: the device reboots anyway, and an open one wedges the next call.
 */
inline RestartResult deviceRestart(KnxIpTunnel& tun, uint16_t pa, bool master, uint8_t erase)
{
    RestartResult r;
    detail_restart::err = -1;
    detail_restart::secs = 0;
    if (master) tun.setRestartCallback(&detail_restart::answer);
    bool co = tun.scanConnect(pa);
    if (co)
    {
        for (uint64_t t0 = detail::nowMs(); !tun.scanConnected() && detail::nowMs() - t0 < 800;) tun.pump();
        co = tun.scanConnected();
    }
    r.connectionOriented = co;
    r.sent = tun.sendRestart(pa, master, erase, 0);
    const uint32_t window = master ? 3000u : 400u;
    for (uint64_t t0 = detail::nowMs(); detail::nowMs() - t0 < window;)
    {
        tun.pump();
        if (master && detail_restart::err >= 0) break;
    }
    tun.setRestartCallback(nullptr);
    if (co) tun.scanDisconnect();
    r.masterError = detail_restart::err;
    r.masterSeconds = detail_restart::secs;
    return r;
}

} // namespace ftc

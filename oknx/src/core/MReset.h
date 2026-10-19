/**
 * @file        MReset.h
 * @brief       Restart a KNXnet/IP interface with M_Reset.req over its device management connection, the way
 *              `oknx -i <ip> reset` and `oknx serve`'s device_mreset do it.
 * @details     03_06_03 4.1.7.5.1: the interface does the steps of a Basic Restart; every KNXnet/IP connection to it
 *              closes, also those of other clients. Afterwards it is asked with DESCRIPTION_REQUEST until it answers.
 * @date        2026-10-19
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include "core/DeviceMgmt.h"
#include "core/Describe.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace ftc
{

/** @brief What a reset did. */
struct MResetResult
{
    bool connected = false;      ///< the device management connection came up
    std::string connectError;
    DevMgmtSession::Reset outcome = DevMgmtSession::Reset::NotOpen;
    bool back = false;           ///< the interface answered a DESCRIPTION_REQUEST again
    uint32_t backAfterMs = 0;    ///< from the request to that answer
};

/**
 * @brief Send M_Reset.req to @p ip:@p port and wait up to @p waitMs for the interface to answer again.
 * @param aborted asked between the polls; true stops the wait (the reset itself is already sent then).
 */
inline MResetResult mresetInterface(const std::string& ip, uint16_t port, uint32_t waitMs, const std::function<bool()>& aborted)
{
    MResetResult r;
    DevMgmtSession dm;
    if (!dm.open(ip, port, &r.connectError)) return r;
    r.connected = true;
    const uint64_t t0 = detail::nowMs();
    r.outcome = dm.reset();
    if (r.outcome == DevMgmtSession::Reset::NoAnswer) return r;
    // an interface that reboots is silent for a while; one that restarts in software answers at once
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    while (detail::nowMs() - t0 < waitMs && !(aborted && aborted()))
    {
        IfaceDesc d;
        if (queryInterface(ip, port, d, 700))
        {
            r.back = true;
            r.backAfterMs = (uint32_t)(detail::nowMs() - t0);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    return r;
}

/** @brief Short English name of an outcome for -q lines and the API. */
inline const char* mresetOutcomeName(DevMgmtSession::Reset o)
{
    switch (o)
    {
        case DevMgmtSession::Reset::Indicated: return "indicated";
        case DevMgmtSession::Reset::Acked: return "acked";
        case DevMgmtSession::Reset::NoAnswer: return "no-answer";
        default: return "not-open";
    }
}

} // namespace ftc

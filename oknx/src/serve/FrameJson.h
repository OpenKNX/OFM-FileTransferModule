/**
 * @file        FrameJson.h
 * @brief       A monitor telegram (MonFrame) as JSON and back, for the pipe between worker and leader.
 * @details     Every field travels, so a frame rebuilt in the leader compares exactly like the one the worker
 *              decoded. `ms` is the worker's monotonic capture time; both processes run on one machine and
 *              share that clock.
 * @date        2026-10-19
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include "cli/Monitor.h"
#include "serve/JsonGet.h"
#include "third_party/json.hpp"

namespace ftc
{
namespace serve
{

inline nlohmann::json frameToJson(const MonFrame& f)
{
    return nlohmann::json{{"bus", f.bus},     {"ext", f.ext},     {"group", f.group}, {"ms", f.ms},
                          {"time", f.time},   {"src", f.src},     {"dst", f.dst},     {"body", f.body},
                          {"raw", f.raw},     {"key", f.key},     {"hasAck", f.hasAck}, {"acked", f.acked},
                          {"stat", f.stat},   {"lost", f.lost},   {"ferr", f.ferr},   {"berr", f.berr},
                          {"perr", f.perr},   {"seq", f.seq},     {"ackKind", f.ackKind}, {"reasm", f.reasm}};
}

/** @brief The worker's frame line back; a member of another type (a worker of another build) reads as missing. */
inline MonFrame frameFromJson(const nlohmann::json& j)
{
    MonFrame f;
    f.bus = jget(j, "bus", false);
    f.ext = jget(j, "ext", false);
    f.group = jget(j, "group", false);
    f.ms = jget(j, "ms", (uint64_t)0);
    f.time = jget(j, "time", "");
    f.src = jget(j, "src", (uint16_t)0);
    f.dst = jget(j, "dst", (uint16_t)0);
    f.body = jget(j, "body", "");
    f.raw = jget(j, "raw", "");
    f.key = jget(j, "key", "");
    f.hasAck = jget(j, "hasAck", false);
    f.acked = jget(j, "acked", false);
    f.stat = jget(j, "stat", false);
    f.lost = jget(j, "lost", false);
    f.ferr = jget(j, "ferr", false);
    f.berr = jget(j, "berr", false);
    f.perr = jget(j, "perr", false);
    f.seq = jget(j, "seq", (uint8_t)0);
    f.ackKind = jget(j, "ackKind", (uint8_t)0);
    f.reasm = jget(j, "reasm", (uint8_t)0);
    return f;
}

/** @brief Lower-case hex of @p n bytes, no separators. */
inline std::string hexOf(const uint8_t* d, int n)
{
    static const char* H = "0123456789abcdef";
    std::string s;
    s.reserve((size_t)n * 2);
    for (int i = 0; i < n; ++i)
    {
        s += H[d[i] >> 4];
        s += H[d[i] & 15];
    }
    return s;
}

} // namespace serve
} // namespace ftc

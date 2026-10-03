/**
 * @file        shim.cpp
 * @brief       Out-of-line definitions for the shim globals the four FTC files reference as extern.
 * @details     One translation unit owns the three host globals: openknx (Facade), knx (HostKnxFacade)
 *              and LittleFS (LittleFSHost).
 * @date        2026-07-25
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 */
#include <cstdio>

#include "LittleFS.h"
#include "OpenKNX.h"

OpenKNX::Facade openknx; // the `openknx` global (logger/console/freeLoopTime)
HostKnxFacade knx;       // the `knx` global (bau()/individualAddress())
LittleFSHost LittleFS;   // the Arduino-style filesystem global

// The stack's Dpt constructor warns through println() on an invalid "*.0" subtype. On the device that
// is the serial log; here it is the one symbol the DPT converter needs from outside, so it goes to
// stderr rather than pulling the device's logging in.
void println(const char* s)
{
    std::fprintf(stderr, "%s\n", s);
}

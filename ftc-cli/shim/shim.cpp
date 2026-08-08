/**
 * @file        shim.cpp
 * @brief       Out-of-line definitions for the shim globals the four FTC files reference as extern.
 * @details     One translation unit owns the three host globals: openknx (Facade), knx (HostKnxFacade)
 *              and LittleFS (LittleFSHost).
 * @date        2026-07-25
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 */
#include "LittleFS.h"
#include "OpenKNX.h"

OpenKNX::Facade openknx; // the `openknx` global (logger/console/freeLoopTime)
HostKnxFacade knx;       // the `knx` global (bau()/individualAddress())
LittleFSHost LittleFS;   // the Arduino-style filesystem global

// ┬────┴  OFM-FileTransferModule / ftc-cli
// ■ KNX   2026 OpenKNX - Erkan Çolak
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026, Erkan Çolak
//
// Out-of-line definitions for the shim globals the four FTC files reference as extern.
#include "LittleFS.h"
#include "OpenKNX.h"

OpenKNX::Facade openknx; // the `openknx` global (logger/console/freeLoopTime)
HostKnxFacade knx;       // the `knx` global (bau()/individualAddress())
LittleFSHost LittleFS;   // the Arduino-style filesystem global

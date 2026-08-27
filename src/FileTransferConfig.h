#pragma once
/**
 * @file        FileTransferConfig.h
 * @brief       Feature switches for the whole module -- include FIRST in every FTC translation unit
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 */
// A switch is on when it is DEFINED and off when it is not -- no values, no defaults to take away.
// Set nothing and you get the bare server core: upload, FileInfo, FilesystemInfo, Format/Rename/Delete,
// FwUpdate, CheckFeatures. Everything beyond that comes from a PROFILE, or from a single -D.
// Names starting with OPENKNX_ are yours to set; FTC_/FTM_ names are the module's own and are not.
// Measured sizes: doc/FLAGS.md -- the reasoning: doc/CONCEPT-defines.md

// --- Profiles: set ONE in your ini. MANAGER contains DEVICE. -----------------------------------
//     (nothing)                        CUSTOM -- pick the switches yourself; the core alone answers
//                                       FileUpload (safe), FileInfo, df, Format/Rename/Delete, FwUpdate
//     -D OPENKNX_FTC_PROFILE_DEVICE    a managed end device -- also needs -D OPENKNX_FTC_CONSOLE
//     -D OPENKNX_FTC_PROFILE_MANAGER   full client role -- EVERY FTC feature incl. delta; also -D OPENKNX_FTC_CLIENT
//
// MANAGER is the complete profile and turns DELTA_UPDATE on too. Delta needs the filesystem to hold the
// rebuilt image AND the patch at once; where that does not fit (e.g. a 2 MB RP2040) a delta job fails for
// lack of room -- use PROFILE_DEVICE + hand-picked switches on such a board.
//
// Why those two are not handed out here: they are read OUTSIDE this module, by libraries that never
// include this header and must not depend on it.
//   OPENKNX_FTC_CLIENT   -- lib/knx, 9 files, to compile its half of the FunctionProperty exchange
//   OPENKNX_FTC_CONSOLE  -- lib/OGM-Common, Console.h, where it gates a DATA MEMBER (_lineSink)
// A macro defined here reaches only the module's own translation units. If OGM-Common compiled Console
// without _lineSink while this module compiled it with, the two halves would disagree about the class --
// measured: Console::submitLine is 104 B without the switch and 112 B with it. Both must arrive as -D.
#if (defined(OPENKNX_FTC_PROFILE_DEVICE) + defined(OPENKNX_FTC_PROFILE_MANAGER)) > 1
    #error "FTC: set exactly one OPENKNX_FTC_PROFILE_* -- DEVICE or MANAGER"
#endif

#ifdef OPENKNX_FTC_PROFILE_MANAGER
    #define FTC_TIER 2
#elif defined(OPENKNX_FTC_PROFILE_DEVICE)
    #define FTC_TIER 1
#else
    #define FTC_TIER 0
#endif

// A profile only ADDS. Anything set in the ini is already defined here and stays untouched.
#if FTC_TIER >= 1
    #ifndef OPENKNX_FTC_SECURITY
        #define OPENKNX_FTC_SECURITY // every profile: a device that takes files and swaps firmware must know who asks
    #endif
    #ifndef OPENKNX_FTC_DOWNLOAD
        #define OPENKNX_FTC_DOWNLOAD
    #endif
    #ifndef OPENKNX_FTC_DIROPS
        #define OPENKNX_FTC_DIROPS
    #endif
    #ifndef OPENKNX_FTC_FASTUPLOAD
        #define OPENKNX_FTC_FASTUPLOAD
    #endif
    // ESP32 only -- there the inflater sits in the chip's mask ROM. RP2040 unpacks in the bootloader and
    // has no code behind this switch at all, which is why the check below refuses it there.
    #if defined(ARDUINO_ARCH_ESP32) && !defined(OPENKNX_FTC_GZIP_UPDATE)
        #define OPENKNX_FTC_GZIP_UPDATE
    #endif
#endif

// One message per profile, naming everything that is missing -- so a wrong ini is fixed in one go.
#if FTC_TIER >= 2 && (!defined(OPENKNX_FTC_CLIENT) || !defined(OPENKNX_FTC_CONSOLE))
    #error "FTC: PROFILE_MANAGER needs -D OPENKNX_FTC_CLIENT AND -D OPENKNX_FTC_CONSOLE in your ini -- lib/knx and lib/OGM-Common read them and never see this header."
#elif FTC_TIER == 1 && !defined(OPENKNX_FTC_CONSOLE)
    #error "FTC: PROFILE_DEVICE includes the console -- add -D OPENKNX_FTC_CONSOLE to your ini. lib/OGM-Common (Console.h) reads it and never sees this header."
#endif

#if FTC_TIER >= 2
    // The "knxOTA" web page: a front-end onto the embedded client (target PA, send firmware/delta from
    // this device's flash/SD/ext-flash, trigger the update, measure throughput).
    // Measured: 34732 B flash + 80 B RAM on RP2040, 40056 B + 64 B RAM on ESP32.
    // Needs a web server to be served from; OPENKNX_WEBSERVER comes from the ini and is visible here.
    #if defined(OPENKNX_WEBSERVER) && !defined(OPENKNX_FTC_KNXOTA_WEB)
        #define OPENKNX_FTC_KNXOTA_WEB
    #endif
    #ifndef OPENKNX_FTC_SCAN
        #define OPENKNX_FTC_SCAN
    #endif
    #ifndef OPENKNX_FTC_DEVICEINFO
        #define OPENKNX_FTC_DEVICEINFO
    #endif
    // MANAGER is the full profile -> delta update as well (board must hold image+patch; see the top note).
    #ifndef OPENKNX_FTC_DELTA_UPDATE
        #define OPENKNX_FTC_DELTA_UPDATE
    #endif
#endif

// --- Console take-over (obj 160) is a write/control surface -> Console always pulls in Security. There
//     is no way to have the one without the other; an unauthenticated console is not a build option. ---
#if defined(OPENKNX_FTC_CONSOLE) && !defined(OPENKNX_FTC_SECURITY)
    #define OPENKNX_FTC_SECURITY
#endif

// --- No switch may be set and do nothing. Each of these was a silent misconfiguration before. --------
#if defined(OPENKNX_FTC_GZIP_UPDATE) && !defined(ARDUINO_ARCH_ESP32)
    #error "FTC: OPENKNX_FTC_GZIP_UPDATE only exists on ESP32 -- on RP2040 the bootloader unpacks. Set it in your ESP32 env only, or let a profile do it."
#endif
#if defined(OPENKNX_FTC_KNXOTA_WEB) && !defined(OPENKNX_WEBSERVER)
    #error "FTC: OPENKNX_FTC_KNXOTA_WEB needs a web server to serve the page -- add OPENKNX_WEBSERVER or drop it."
#endif
#if defined(OPENKNX_FTC_KNXOTA_WEB) && !defined(OPENKNX_FTC_CLIENT)
    #error "FTC: OPENKNX_FTC_KNXOTA_WEB drives the client half -- add OPENKNX_FTC_CLIENT or drop it."
#endif
#if (defined(OPENKNX_FTC_SCAN) || defined(OPENKNX_FTC_DEVICEINFO)) && !defined(OPENKNX_FTC_CLIENT)
    #error "FTC: OPENKNX_FTC_SCAN / _DEVICEINFO are client features -- add OPENKNX_FTC_CLIENT or drop them."
#endif
#if defined(OPENKNX_FTC_LEGACY_STACK) && defined(OPENKNX_FTC_CLIENT)
    #error "FTC: the client needs a knx stack from 51683f5 on; release 2.4.0 has none of the 18 entry points it calls. Drop OPENKNX_FTC_LEGACY_STACK or drop the client."
#endif

// Bytes read from the staged file per inflate pass. Small on purpose: the window, not this, does the work.
#ifndef FTM_GZIP_IN_CHUNK
    #define FTM_GZIP_IN_CHUNK 1024
#endif

// Delta firmware update: send only the difference to the image the device is running and rebuild the new
// one on the device. Needs a second app slot (ESP32) or a filesystem large enough to hold the rebuilt
// image (RP2040/RP2350).
#ifdef OPENKNX_FTC_DELTA_UPDATE
    // Output produced per loop() pass. One flash sector is the smallest step the hardware cannot split,
    // and therefore the ceiling this job must never exceed.
    #ifndef FTM_DELTA_SLICE
        #define FTM_DELTA_SLICE 4096
    #endif

    // Patch file header: magic, version, flags, the lengths and CRC32s of source, target and header.
    #define FTM_DELTA_MAGIC "OKD1"
    #define FTM_DELTA_VERSION 1
    #define FTM_DELTA_HDR_SIZE 36
#endif

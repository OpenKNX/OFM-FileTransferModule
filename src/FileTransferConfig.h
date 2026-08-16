#pragma once
/**
 * @file        FileTransferConfig.h
 * @brief       Feature switches for the whole module -- include FIRST in every FTC translation unit
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 */
// Every optional feature defaults ON; strip down with -DOPENKNX_FTC_MINIMAL or pin one gate to 0 (e.g.
// -DOPENKNX_FTC_DIROPS=0). The core (upload, FileInfo, FilesystemInfo, Format/.../Delete, FwUpdate, ...) has no switch.

#ifdef OPENKNX_FTC_MINIMAL
    #define FTC_EXTRA_DEFAULT 0
#else
    #define FTC_EXTRA_DEFAULT 1
#endif

// --- Optional features (value-based: -D...=0 opts out; MINIMAL flips the default to 0) ---
#ifndef OPENKNX_FTC_DOWNLOAD
    #define OPENKNX_FTC_DOWNLOAD FTC_EXTRA_DEFAULT // FileDownload (41): read a file from the device
#endif
#ifndef OPENKNX_FTC_FASTUPLOAD
    #define OPENKNX_FTC_FASTUPLOAD FTC_EXTRA_DEFAULT // FileUploadFast+Report (44/45); OFF -> classic only
#endif
#ifndef OPENKNX_FTC_DIROPS
    #define OPENKNX_FTC_DIROPS FTC_EXTRA_DEFAULT // DirList/Create/Delete (80/81/82)
#endif
#ifndef OPENKNX_FTC_SCAN
    #define OPENKNX_FTC_SCAN FTC_EXTRA_DEFAULT // client ProgScan
#endif
#ifndef OPENKNX_FTC_DEVICEINFO
    #define OPENKNX_FTC_DEVICEINFO FTC_EXTRA_DEFAULT // client `ftc <pa> info` + device-map + GA report
#endif

// --- Info-API: render-agnostic mirror, only meaningful to a frontend. Native host always; on a device
//     only when a webserver consumes it. The device itself never reads it, so OFF costs no function. ---
#ifndef OPENKNX_FTC_INFOAPI
    #if !defined(ARDUINO) || defined(OPENKNX_WEBSERVER)
        #define OPENKNX_FTC_INFOAPI 1
    #else
        #define OPENKNX_FTC_INFOAPI 0
    #endif
#endif

// --- Console take-over (obj 160) is a write/control surface -> Console pulls in Security so it is never
//     unauthenticated. Opt out deliberately with -DOPENKNX_FTC_CONSOLE_INSECURE (dev / trusted bus). ---
#if defined(OPENKNX_FTC_CONSOLE) && !defined(OPENKNX_FTC_CONSOLE_INSECURE) && !defined(OPENKNX_FTC_SECURITY)
    #define OPENKNX_FTC_SECURITY
#endif

// ESP32 only: unpack a gzipped staged firmware into the OTA slot on the fly (inflate from mask ROM, no
// flash cost; ~44 KB heap only during the apply pass). Roughly halves the bus time of an update.
#ifndef OPENKNX_FTC_GZIP_UPDATE
    #define OPENKNX_FTC_GZIP_UPDATE 1
#endif

// Bytes read from the staged file per inflate pass. Small on purpose: the window, not this, does the work.
#ifndef FTM_GZIP_IN_CHUNK
    #define FTM_GZIP_IN_CHUNK 1024
#endif

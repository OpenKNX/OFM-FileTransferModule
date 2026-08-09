#pragma once
// Central FTC feature-switch configuration. Include FIRST in every FTC translation unit so the
// derived defaults and couplings below are visible before any feature guard is evaluated.
//
// Policy: all optional features default ON (opt-out) so nothing breaks and they work WITHOUT a
// webserver. A product strips down with -DOPENKNX_FTC_MINIMAL (extras default OFF) or by pinning an
// individual gate to 0 (e.g. -DOPENKNX_FTC_DIROPS=0). The core (FwUpdate, classic upload, FileInfo,
// FilesystemInfo, Format/Exists/Rename/Delete, ModuleVersion, CheckFeatures, Cancel) has no switch.

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

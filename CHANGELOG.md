# Changes

## ec/v0.2.0-beta.1: 2026-08-09

The complete OpenKNX FileTransferModule (FTC — file transfer, FW-update, console tunnel and access control
over cEMI/KNX). One shared client core drives **both** the on-device `ftc` console command (embedded CLI)
and the native desktop **`ftc-cli`** (mac / win / linux) — the same transfer + protocol code, compiled for
the device and for the host. Not product-specific: it runs on any OpenKNX device. This entry covers
everything since the 0.1.5 baseline (`178f186`). Every optional feature is **on by default** (opt-out); the
build switches live in `FileTransferConfig.h`. Build- and 3-agent-expert-review-verified; HW-test pending.

**File transfer (server + on-device client)**
* Feature: FTC server — fast transfer, filesystem-info, console tunnel and ESP32 self-apply; the on-device `ftc` console client (PA → PA to another OpenKNX device's FTC server).
* Feature: device client + module — SD / external-flash directory backends, fast/safe windowing, busy-patience under a congested bus, a KNX device-map.
* Feature: pluggable directory-listing backends for non-LittleFS roots (`sd/`, `efc/`).
* Feature: interface-APDU auto-framing + delivery-rate pacing feedback + OGM-Common console compatibility shim.
* Feature: tunable console drain + download package size + upload auto-degrade for small tunnels.
* Fix: raise the package ceiling to the spec-legal 254 (host guard 251) + doc sync.

**Transfer resilience**
* Feature: resilient transfers — a size-scaled stall/wedge backstop, auto-resume, download-resume (whole-chunk boundary, CRC-cooperative), a smart AUTO window (Discover & Lock), and host tunnel auto-reconnect (reconnect-then-resume). HW-verified: a mid-transfer interface reboot recovers to a CRC-verified file.
* Change: dropped the experimental "forget" mode; slimmed + optimized the client and added a RAM union for the mutually-exclusive receive buffers.

**Console tunnel**
* Feature: cooperative, non-blocking console output (info / progress / verbose diagnostics) — never blocks `loop()`.
* Feature: the console tolerates a congested bus; names the remote owner when it takes over the local console.
* Fix: release the session on a client drain-timeout and on a same-owner re-open; open a console only on a genuine OPEN-accept; mark console-open error paths as Failed so a concurrent-console rejection is reported; guard `ftcOnResponse` by source PA + arm the OUT dedup fresh (two devices sharing the console).

**Access control / security** (opt-in `OPENKNX_FTC_SECURITY`)
* Feature: server access control + password auth (login / logout, ETS-gated); brute-force back-off on the auth; the access-protection ETS block injected as a shared "Erweitert" section with context help; local serial test overrides for the config (never persisted).
* Fix: shortened + renamed the access-stage labels ("Ausgeschaltet" / "Im Prog-Modus" / "Mit Passwort").

**Device info & group communication (client)**
* Feature: `ftc <pa> info` device fingerprint (mask/class, manufacturer, order/hardware/version, FTM version, feature bits, table states, BCU extras, bus voltage) with APDU detection and `--verbose`/`--quiet`.
* Feature: read the info-ga group-address / association tables connection-oriented (the ETS path); bus scan with light/full probes and CSV save.

**Native ftc-cli (host, mac / win / linux)**
* Feature: native desktop FTC client over a KNXnet/IP tunnel — tunnel transport + protocol core + gzip + build matrix; a phosphor terminal UI + host shims; host driver with commands, result panels, discovery and monitors; delivery-rate send pacing with 1-outstanding TX; host-CLI path-length limits + `get` alias + honest probe wording.

**Minimal footprint (feature gates)** — new in 0.2.0
* Feature: central `FileTransferConfig.h` — feature defaults + couplings in one place. `OPENKNX_FTC_MINIMAL` strips all extras, or pin an individual gate with `-DOPENKNX_FTC_…=0`. Console take-over pulls in Security (`OPENKNX_FTC_CONSOLE` ⟹ `OPENKNX_FTC_SECURITY`) unless `-DOPENKNX_FTC_CONSOLE_INSECURE`. The full switch table is in the [README](README.md#build-switches-feature-gates).
* Feature: server extras gateable — `OPENKNX_FTC_DOWNLOAD` / `_FASTUPLOAD` / `_DIROPS` guard FileDownload, FileUploadFast+Report and the directory ops; `_FASTUPLOAD` also gates the CheckFeatures FAST bit so a server without fast never advertises it (the client falls back to classic). `OPENKNX_FTC_MINIMAL` → ~4.4 KB less flash.
* Feature: client extras gateable — `OPENKNX_FTC_SCAN` / `_DEVICEINFO` guard the on-device scan and the device-fingerprint + GA report (handler + dispatch + request fns + console commands + help); cross-guards keep the scan↔device-info coupling well-formed. Combined ~28 KB less flash on a 2 MB RP2040.
* Feature: the Info-API struct mirror (the render-agnostic surface a web/panel frontend draws from) is gated on `OPENKNX_WEBSERVER` (native host always on).
* Change: the core (FwUpdate, classic upload, FileInfo, FilesystemInfo, Format/Exists/Rename/Delete, ModuleVersion, CheckFeatures, Cancel) has no switch — always compiled on every device.

**Non-blocking** — new in 0.2.0
* Feature: the LittleFS FileInfo CRC is now cooperative — routed through the same `crcLoop` as SD/EFC instead of a blocking whole-file read in the KNX dispatch (never reboots on a large file). FileInfo answers `0x02` (computing) until the pass finishes, then `0x00` (size + CRC); the CRC value is byte-identical.
* Fix: cancel the cooperative CRC job before any FS-mutating command — the persistent `_crcFile` handle is dropped before format/upload/delete, so those can never run against an open handle (use-after-free / concurrent second handle on RP2040). `cmdFormat` also closes an open transfer handle first. (Found by the memory-safety review.)

**Structure / cleanup** (behaviour-preserving) — new in 0.2.0
* Change: the 2610-line `loop()` state machine was split into 7 per-feature handler methods (download / fast / dir-ops / scan / device-info / security / console); `loop()` is now a preamble + core-transfer inline + a dispatcher. Pure code motion, verified behaviour-identical.
* Fix: `cmdFileInfo` no longer puts a 1000-byte VLA on the KNX-dispatch stack (fixed 256-byte chunk; −744 B stack peak); `consoleIdle()` / `_conSub` guarded so a console-less client build compiles.
* Change: de-duplicated the classic/fast size-hint parse, dropped a dead `|| OPENKNX_EXTFLASH` token and a redundant answer byte; console dividers renamed to purpose (`RULE_SECTION` / `RULE_DFBAR` / `RULE_REPORT`) from one shared dash buffer; clang-formatted (the `_diagPad` table protected with `clang-format off`).

**Docs & tooling**
* Docs: a restructured doc set under `doc/` — the FTC engineering reference (`FTC-Reference.md`), a standalone console-tunnel doc (`FTC-Console.md`) and a standalone access-control doc (`FTC-Security.md`); a modernized README (device↔device or PC, FW-fit check, build-switch table); old pre-implementation concept notes removed.
* Tooling: FTC PowerShell test tooling + a firmware compressor; an extended `Test-FtcSuite.ps1` (help, verbose streaming, tri-state upload, path fixes); host scripts + repo meta (README, .gitignore); dropped dead constants `FTC_PKG_DEFAULT` / `FTC_RATE_MIN_MS`.
* Change: library bumped 0.1.5 → 0.2.0.

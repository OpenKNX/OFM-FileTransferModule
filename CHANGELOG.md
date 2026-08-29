# Changes


## ec/v0.2.0-beta.1 -- second batch: 2026-08-29

The tag was moved from `f489fcf` to the head of this batch, so everything below is part of it. Main additions:
firmware update as a difference instead of a whole image, a knxOTA page in the device's own web
interface, and a repaired `fast` mode.

**Delta firmware update**
* Feature: firmware can be sent as a difference to the running image (`.okd`, magic `OKD1`) instead of a full image — a 1.8 MB image takes about 78 min over the bus at 400 B/s, a typical 45 KB difference about 2 min
  * the rebuild runs in `loop()` in slices no larger than one flash sector; nothing is made bootable before the rebuilt image has been checksummed, so every abort leaves the device on the firmware it is already running
  * the client probes first (`FwProbe`, cmd 106) whether the target runs the image the patch expects, and reports the reason when it does not
  * build switches follow what is present instead of being set by hand; a failure is reported rather than silently skipped
* Feature: compressed full images — a gzipped image is unpacked straight into the ESP32 OTA slot through the inflater in the chip's mask ROM (about 88 min down to 54 min); the RP2040 bootloader already unpacks, so the switch has no code there and the build refuses it
* Feature: ESP32 OTA-slot safety guard — a single-app partition layout has no second slot, which is now checked before writing and reported through `CheckFeatures`

**knxOTA**
* Feature: knxOTA web page (`OPENKNX_FTC_KNXOTA_WEB`) — pick a target PA, read what the device is, send a firmware or a difference from this device's flash, SD or external flash over KNX, trigger the update, or measure throughput. The page is a front-end onto the embedded client, so no PC and no console are in the chain. 34732 B flash + 80 B RAM on RP2040, 40056 B + 64 B RAM on ESP32
* Feature: an unfinished knxOTA run can be resumed instead of restarted
* Fix: a refused firmware apply is reported instead of being announced as triggered — `FwUpdate` answers nothing on success but `0xA0`/`0xA2` when the security gate refuses it, and that answer was discarded
* Feature: knxOTA assistant command in `ftc-cli`, with a reachability probe and `--check`/`--force`

**Transfer**
* Fix: `fast` throughput repaired — payload degrade, window regulation and the host pacer worked against each other, so the negotiated window collapsed under load
* Feature: the negotiated mode is reported, including why `fast` was denied, instead of silently falling back to `safe`
* Feature: downloads are CRC-verified, and `FwUpdate` is refused while writes are disabled
* Feature: a device scan paces itself, so a scan no longer floods a busy bus
* Change: `perf` and the transfer commands share one option grammar (`FtcXferOptions`), so `w<N>` and the mode tokens mean the same thing everywhere

**ftc-cli (desktop client)**
* Feature: live A/B fidelity compare of two KNXnet/IP monitors (busmon or group monitor), for checking one interface against another
* Feature: self-install and self-uninstall of the binary, with no external dependencies
* Feature: host core modules — scan, describe, features, reachability, UF2 and ESP image handling, host filesystem, access
* Feature: the window regulation is visible, every run is reported, and a write is confirmed before it happens
* Fix: the tunnel ACK guard frees the in-flight slot only on a full, in-channel ACK, so a truncated or foreign frame cannot free it
* Fix: the plain monitor path honours the `L_Busmon.ind` framing rules
* Fix: the client notices when the webconsole peer stops answering, instead of waiting out the timeout
* Fix: directories are created without a shell, so a path with quotes or metacharacters cannot inject
* Fix: the real abort reason is reported, and an elevated FTC priority is confirmed
* Fix: the console link state reflects the target's answer, not just the state of the tunnel
* Change: CoreFoundation is linked on the macOS host; report directories are ignored

**ETS and documentation**
* Feature: German context help ships as an ETS baggage
* Doc: the German documents are replaced by an English set — README, ARCHITECTURE, PROTOCOL, CONSOLE, SECURITY, THROUGHPUT, DELTA, FLAGS, CONCEPT-defines
* Doc: `FLAGS.md` documents `OPENKNX_FTC_KNXOTA_WEB` with its measured flash and RAM cost
* Doc: the throughput chapter drops the 450-544 B/s crash cliff — that was an intermediate finding; the reboot needs an artificial unpaced tunnel flood, not a normal transfer
* Fix: the ETS help text calls access stage 0 "Blockiert", matching the parameter enum

**Tests**
* Test: PowerShell hardening suite over the FunctionProperty RPC surface
* Test: response-matrix and state-machine suites extended — every command against every server response per drive and async state
* Test: PowerShell tooling for the delta update path

**Device info**
* Fix: the device error-code read is gone -- the client asked for PID 24 on the device object, which is not the error-code property (property.h names PID_ERROR_CODE 28), and no device object in the stack registers PID_ERROR_CODE either
* Fix: that read spent the 800 ms optional-property probe on every device-info query and never produced a value; the row is dropped from the web status, the knxOTA page and the ftc-cli report

**Documentation**
* Feature: QUICKSTART, FIRMWARE-UPDATE, WEB, FTC-CLI, SCRIPTS and INTEGRATION added; README is the index and names the audience of every document
* Change: the superseded ftc-cli wire-protocol and host-shim documents are removed; the module doc set carries their content
* Doc: the module README is rewritten -- what the module carries besides files, knxOTA end to end including why a delta update turns half an hour into minutes, the three front ends as one state machine, the measured throughput and its cause, every build switch and both profiles
* Doc: the ftc-cli README gains knxOTA, install/uninstall, retry, prio, logging, the busmon A/B comparison and the programming LED, and points at doc/PROTOCOL.md instead of the shim documents that were removed
* Doc: the example addresses are marked as an isolated lab VLAN, with the note that 11.0.0.0/8 is publicly allocated space rather than an RFC 1918 range
* Note: `errorcodes.txt` records that 0x01..0x04 are listed as LittleFS errors while the server also uses 0x01..0x03 as per-command status bytes -- the two readings conflict and are not reconciled

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
* Feature: central `FileTransferConfig.h` — every switch is on when it is set and off when it is not, no values and no defaults to take away. Two profiles pick a sensible set: `OPENKNX_FTC_PROFILE_DEVICE` for a managed end device, `OPENKNX_FTC_PROFILE_MANAGER` for interface/router/host; setting no profile leaves the bare server core. `OPENKNX_FTC_CLIENT` and `OPENKNX_FTC_CONSOLE` stay explicit `-D` because `lib/knx` and `lib/OGM-Common` read them and never see this header. Six misconfigurations fail the build instead of compiling something else. Full table in [doc/FLAGS.md](doc/FLAGS.md).
* Feature: server extras gateable — `OPENKNX_FTC_DOWNLOAD` / `_FASTUPLOAD` / `_DIROPS` guard FileDownload, FileUploadFast+Report and the directory ops; `_FASTUPLOAD` also gates the CheckFeatures FAST bit so a server without fast never advertises it (the client falls back to classic). Together they cost ~4.6 KB on RP2040 and ~5.2 KB on ESP32; `PROFILE_DEVICE` bundles them.
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

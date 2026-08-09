![Issues](https://badgen.net/github/open-issues/OpenKNX/ofm-filetransfermodule)
![Branches](https://badgen.net/github/branches/OpenKNX/ofm-filetransfermodule)
[![CodeFactor](https://www.codefactor.io/repository/github/openknx/ofm-filetransfermodule/badge)](https://www.codefactor.io/repository/github/openknx/ofm-filetransfermodule)

# FileTransfer over the KNX Bus

Add this module to make your OpenKNX device an FTC **target**: upload/download files, manage the flash and
run a firmware update — plus, optionally, an **interactive remote console** — all **over the KNX / KNXnet-IP
tunnel**. Drive it **device → device** (the on-device `ftc` console on any OpenKNX device built with
`-D OPENKNX_FTC`) **or from a PC** with the native cross-platform `ftc` CLI — **no PC is required in the chain**.

## Step 1 — check the firmware fits for FW-update-over-KNX
The build prints a fit check: the compressed firmware image must fit in the device's filesystem (that is
where a `FwUpdate` stages the incoming image before it is applied). You want the closing `=> OK`:
```
FW-update-over-KNX fit (compressed OTA image must fit in filesystem):
  firmware:              987196 bytes
  est. gzip (x0.65):     641677 bytes
  filesystem:           2617344 bytes
  usable  (x0.90):      2355609 bytes
=> OK - fits with headroom.
```
`est. gzip` is the ~0.65× compressed size actually transferred; `usable` is the filesystem minus a safety
margin. If the check fails, grow the filesystem partition (or shrink the sketch) until the image fits.

This build-time check is only a heads-up — **it can never corrupt a device.** Before *any* real transfer,
the client also runs a **runtime space check**: it queries the target's free space and, if the file would
not fit, **refuses up front** ("not enough space on target filesystem") instead of streaming a whole
upload that could only fail near the end. So a too-small target is caught cleanly, not mid-transfer.

## Step 2
Add the Module to the OpenKnx Stack
```C++
#include <Arduino.h>
#include "OpenKNX.h"
#include "FileTransferModule.h"

void setup()
{
	const uint8_t firmwareRevision = 0;
    openknx.init(firmwareRevision);
    openknx.addModule(1, ...);
    openknx.addModule(2, FileTransferModule);
    openknx.setup();

}
```

## Step 3 — drive it (device → device or from a PC)
**Device → device** — build any OpenKNX device with `-D OPENKNX_FTC` and it gains the on-device `ftc`
console client (PA → PA, over the bus, no PC):
```
ftc <pa> ll                # list files on another device
ftc <pa> send <file>       # upload
ftc <pa> console           # interactive remote console (needs OPENKNX_FTC_CONSOLE on both)
```
**From a PC** — the native **`ftc`** host CLI (in [`ftc-cli/`](ftc-cli/), cross-built for Windows / macOS /
Linux) speaks KNXnet/IP tunnelling directly, also through third-party certified interfaces:
```
ftc --ip <interface-ip> <pa> ll
ftc --ip <interface-ip> <pa> send <file>
ftc --ip <interface-ip> <pa> console
```
Both fronts share the **same** transfer + protocol core — only the transport (cEMI on the device vs. a
KNXnet/IP tunnel on the host) differs.
> The older standalone [KnxFileTransferClient](https://github.com/OpenKNX/KnxFileTransferClient) still works too.

## Console & fast transfers — what you need
- **Plain file transfer** (upload / download / FW-update) needs no extra flag once the module is added — the
  device is a target out of the box.
- **Interactive console** — add **`-D OPENKNX_FTC_CONSOLE`** on the target; it opens the console tunnel and
  implies the log ring / web-console (and pulls in the access-control gate, see the switch table above).
- **`fast` / windowed upload** works out of the box (`OPENKNX_FTC_FASTUPLOAD`, on by default). Over a
  third-party (non-OpenKNX) interface it transparently degrades to the classic path; the biggest gains are
  over an OpenKNX interface / router.

### Documentation

| Doc | What's in it |
|---|---|
| **[`doc/FTC-Reference.md`](doc/FTC-Reference.md)** | Engineering reference: command set, wire protocol, `safe` vs `fast`, resume/recovery, throughput & the NCN bottleneck, build flags, metrics. |
| **[`doc/FTC-Console.md`](doc/FTC-Console.md)** | The interactive remote console tunnel (object 160) — how it works, non-blocking, security. |
| **[`doc/FTC-Security.md`](doc/FTC-Security.md)** | Access control: the password gate for writes & console (`OPENKNX_FTC_SECURITY`). |
| **[`ftc-cli/README.md`](ftc-cli/README.md)** | The native desktop client (`ftc --ip …`) for Windows / macOS / Linux. |

## Build switches (feature gates)

**Why:** to **save flash**. The module runs on **any OpenKNX device** — including flash-tight ones like a
2 MB RP2040 — so every optional feature can be compiled out to reclaim space you'd rather give to your own
application. Concretely: `OPENKNX_FTC_MINIMAL` frees **~4.4 KB** of server code, and dropping the client
extras (`_SCAN` / `_DEVICEINFO`) frees **~28 KB** on a 2 MB RP2040. Nothing is lost in transfer correctness
— only features you don't use.

Every optional feature is **on by default (opt-out)**, so adding the module changes nothing until you strip
it down: a minimal device drops everything except the core with `-D OPENKNX_FTC_MINIMAL`, or you pin a
single gate with `-D OPENKNX_FTC_…=0`. All defaults and couplings live in
[`FileTransferConfig.h`](src/FileTransferConfig.h).

**Core — always compiled, no switch:** FwUpdate, classic File Upload, File Info (+ cooperative CRC),
Filesystem Info, Format / Exists / Rename / Delete, Module Version, Check Features, Cancel.

| Switch | Default | Enables | Off → |
|---|---|---|---|
| `OPENKNX_FTC` | off | the on-device **`ftc`** console client (PA → PA); shares its core with the desktop `ftc-cli` | device is an FTC **target** only, no client |
| `OPENKNX_FTC_CONSOLE` | off | interactive console tunnel (obj 160) — **implies `OPENKNX_FTC_SECURITY`** | no console |
| `OPENKNX_FTC_SECURITY` | off | password access control (login / logout, ETS-gated) | writes open (unless a console pulls it in) |
| `OPENKNX_FTC_DOWNLOAD` | **on** | File Download (41) | no reading files off the device |
| `OPENKNX_FTC_FASTUPLOAD` | **on** | fast / windowed upload (44 / 45) + the CheckFeatures FAST bit | classic upload only (the client auto-falls-back) |
| `OPENKNX_FTC_DIROPS` | **on** | Dir List / Create / Delete (80 / 81 / 82) | no directory browsing |
| `OPENKNX_FTC_SCAN` | **on** (client) | on-device bus scan | — |
| `OPENKNX_FTC_DEVICEINFO` | **on** (client) | `ftc <pa> info` device fingerprint + GA report | — |
| `OPENKNX_SDCARD` / `OPENKNX_EXTFLASH` | off | `sd/` / `efc/` drive backends (+ their File Info CRC) | LittleFS only |
| `OPENKNX_WEBSERVER` | (from OFM-Network) | the render-agnostic **Info-API** struct mirror a web / panel frontend draws from | mirror not compiled |

- `OPENKNX_FTC_MINIMAL` flips every **on-by-default** extra to off → the bare FW-update + console core.
- `OPENKNX_FTC_CONSOLE` pulls in `OPENKNX_FTC_SECURITY` so a console take-over is never unauthenticated; opt
  out deliberately with `-D OPENKNX_FTC_CONSOLE_INSECURE` (dev / trusted bus).
- The client sub-gates (`_SCAN` / `_DEVICEINFO`) only apply when `OPENKNX_FTC` is set — a device without the
  client compiles none of it anyway.

## Good to know
The FileTransferModule (FTC) server uses the following FunctionProperties.  
These must not be used by any other module.
|ObjectIndex|PropertyId|Used for|
|---|---|---|
|159|0|Format|
|159|1|Exists|
|159|2|Rename|
|159|40|File Upload|
|159|41|File Download|
|159|42|File Delete|
|159|43|File Info|
|159|44|File Upload (fast / windowed)|
|159|45|File Report (gap query)|
|159|46|Filesystem Info|
|159|80|Dir List|
|159|81|Dir Create|
|159|82|Dir Delete|
|159|90|Cancel|
|159|100|Get Version|
|159|101|Firmware Update|
|159|102|Check Features|
|159|103|Auth Challenge (with `OPENKNX_FTC_SECURITY`)|
|159|104|Auth Response (with `OPENKNX_FTC_SECURITY`)|
|159|105|Auth Logout (with `OPENKNX_FTC_SECURITY`)|
|160|1 / 2|Interactive console (with `OPENKNX_FTC_CONSOLE`)|

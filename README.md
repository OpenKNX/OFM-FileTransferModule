![Issues](https://badgen.net/github/open-issues/OpenKNX/ofm-filetransfermodule)
![Branches](https://badgen.net/github/branches/OpenKNX/ofm-filetransfermodule)
[![CodeFactor](https://www.codefactor.io/repository/github/openknx/ofm-filetransfermodule/badge)](https://www.codefactor.io/repository/github/openknx/ofm-filetransfermodule)

# FileTransfer over the KNX Bus

Add this module to make your OpenKNX device an FTC **target**: upload/download files, manage the flash and
run a firmware update — plus, optionally, an **interactive remote console** — all **over the KNX / KNXnet-IP
tunnel**. Drive it **device → device** (the on-device `ftc` console on any OpenKNX device built with
`-D OPENKNX_FTC_CLIENT`) **or from a PC** with the native cross-platform `ftc` CLI — **no PC is required in the chain**.

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
**Device → device** — build any OpenKNX device with `-D OPENKNX_FTC_CLIENT` and it gains the on-device `ftc`
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
- **`fast` / windowed upload** works out of the box (`OPENKNX_FTC_FASTUPLOAD`, part of every profile). Over a
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

**Why:** to **save flash**. The module runs on any OpenKNX device, including flash-tight ones like a 2 MB
RP2040, so every optional feature can be compiled out and given back to your own application.

**A switch is on when it is set and off when it is not** -- no values, no `=0`, nothing to take away.
Set nothing and you still get the server core.

**Core -- always compiled, no switch:** FwUpdate, classic (safe) File Upload, File Info (+ cooperative
CRC), Filesystem Info, Format / Exists / Rename / Delete, Module Version, Check Features, Cancel. This
core answers **without access control** -- `SECURITY`, and with it every profile, is what turns writes
into an authenticated operation.

### Pick a profile

```ini
; a normal end device
-D OPENKNX_FTC_PROFILE_DEVICE
-D OPENKNX_FTC_CONSOLE

; interface, router, ftc-cli host
-D OPENKNX_FTC_PROFILE_MANAGER
-D OPENKNX_FTC_CLIENT
-D OPENKNX_FTC_CONSOLE
```

`DEVICE` adds `SECURITY`, `DOWNLOAD`, `DIROPS`, `FASTUPLOAD` (and `GZIP_UPDATE` on ESP32).
`MANAGER` adds `DELTA_UPDATE`, `SCAN` and `DEVICEINFO` on top.
Setting no profile at all is the CUSTOM route: pick the individual switches yourself.

**`CLIENT` and `CONSOLE` must be written out even under a profile.** They are read outside this module
-- `CLIENT` in `lib/knx` (9 files), `CONSOLE` in `lib/OGM-Common` (`Console.h`, where it gates a data
member) -- and neither library includes this module's header. A macro defined there would never reach
them. Forgetting a line is caught at build time, not left as a silent half-configuration.

`DELTA_UPDATE` is in no end-device profile on purpose: it depends on the board's free filesystem, which
no profile can know. The build prints a knxOTA report telling you whether it fits.

**The full table** -- what every switch does, what it costs on RP2040 and ESP32, and the ten
misconfigurations the build refuses -- is in **[`doc/FLAGS.md`](doc/FLAGS.md)**; the reasoning behind it
in [`doc/CONCEPT-defines.md`](doc/CONCEPT-defines.md).

Two more, from other modules: `OPENKNX_SDCARD` / `OPENKNX_EXTFLASH` add the `sd/` and `efc/` drive
backends, `OPENKNX_WEBSERVER` (OFM-Network) the struct mirror a web frontend draws from.

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

# FTC switches

**For:** developers writing a product's `ini` — what to set and what it costs. All defined in
[`../src/FileTransferConfig.h`](../src/FileTransferConfig.h). Why it is built this way:
[CONCEPT-defines.md](CONCEPT-defines.md). Wiring the module into a product at all:
[INTEGRATION.md](INTEGRATION.md).

## The rule in one sentence

**Set means on, unset means off.** No values, no `=0`, no default you have to take away. Whoever sets
nothing gets the server core.

Build switches are exclusively the names with **`OPENKNX_`** in front. Next to them the module has about
seventy `FTC_…` and `FTM_…` names (command numbers, timeouts, buffer sizes) — those are internal and are
never set by a product.

## What you write into your `ini`

**Normal end device** — two lines:

```ini
-D OPENKNX_FTC_PROFILE_DEVICE
-D OPENKNX_FTC_CONSOLE                ; why separate: see below
```

**Interface, router, `ftc-cli` host** — three lines:

```ini
-D OPENKNX_FTC_PROFILE_MANAGER
-D OPENKNX_FTC_CLIENT                 ; why separate: see below
-D OPENKNX_FTC_CONSOLE
```

**Assemble everything yourself (CUSTOM)** — set no profile, only the switches you want.

## What is in the profiles

Not copied over, but read out of the header:

| Switch | CUSTOM | `DEVICE` | `MANAGER` |
|---|:---:|:---:|:---:|
| `OPENKNX_FTC_SECURITY` | | ✅ | ✅ |
| `OPENKNX_FTC_DOWNLOAD` | | ✅ | ✅ |
| `OPENKNX_FTC_DIROPS` | | ✅ | ✅ |
| `OPENKNX_FTC_FASTUPLOAD` | | ✅ | ✅ |
| `OPENKNX_FTC_GZIP_UPDATE` | | ✅ *ESP32 only* | ✅ *ESP32 only* |
| `OPENKNX_FTC_CONSOLE` | | **you** | **you** |
| `OPENKNX_FTC_DELTA_UPDATE` | | | ✅ |
| `OPENKNX_FTC_CLIENT` | | | **you** |
| `OPENKNX_FTC_SCAN` | | | ✅ |
| `OPENKNX_FTC_DEVICEINFO` | | | ✅ |
| `OPENKNX_FTC_KNXOTA_WEB` | | | ✅ *only with `OPENKNX_WEBSERVER`* |

**you** = the profile does *not* set it, you have to add it as a `-D`. If you forget, the build aborts
and says what is missing.

## What each switch does and costs

Measured on the interface: RP2040 `release_REG2_PICO_ETH_DD`, ESP32 `release_REG1_LAN_TP_BASE`, each one
individually against a build without a profile.

| Switch | what it does | Role | RP2040 | ESP32 |
|---|---|---|---|---|
| `SECURITY` | login before every writing command ([SECURITY.md](SECURITY.md)) | both | *)* | *)* |
| `DOWNLOAD` | `FileDownload` (41) — read a file from the device | Server | 1 392 B | 1 736 B |
| `DIROPS` | `DirList/Create/Delete` (80/81/82) | Server | 1 584 B | 1 732 B |
| `FASTUPLOAD` | `FileUploadFast`/`Report` (44/45) — serve `fast` | Server | 1 640 B | 1 716 B |
| `GZIP_UPDATE` | unpack a packed full image into the OTA slot | Server | **ESP32 only** | 1 976 B |
| `CONSOLE` | console tunnel (object 160, [CONSOLE.md](CONSOLE.md)); implies `SECURITY` | both | 1 816 B · 144 RAM | 1 808 B · 136 RAM |
| `DELTA_UPDATE` | firmware as a difference to the running image ([DELTA.md](DELTA.md)) | both | 11 712 B · 360 RAM | 10 528 B · 280 RAM |
| `CLIENT` | the "I ask others" role: send `ftc …` yourself | Client | *in the profile* | *in the profile* |
| `SCAN` | search the bus | Client | *in the profile* | *in the profile* |
| `DEVICEINFO` | `ftc <pa> info`, device card, GA report | Client | *in the profile* | *in the profile* |
| `KNXOTA_WEB` | the "knxOTA" page in this device's own web interface ([WEB.md](WEB.md)) | Client | 34 732 B · 80 RAM | 40 056 B · 64 RAM |
| `LEGACY_STACK` | "I build against knx 2.4.0" — forbids the client | — | 0 B | 0 B |

*) The zero point of the measurement is not entirely switch-free: the interface `ini` already sets
`SECURITY` in `[custom]`. It could therefore not be measured in isolation here.

**Whole profiles:**

| | RP2040 | ESP32 |
|---|---|---|
| `PROFILE_DEVICE` (+ `CONSOLE`) | **+6 264 B** · 144 RAM | **+9 248 B** · 136 RAM |
| `PROFILE_MANAGER` (+ `CLIENT` + `CONSOLE`) | **+120 096 B** · 6 996 RAM | **+124 120 B** · 6 872 RAM |

The jump from `DEVICE` to `MANAGER` is, at around 102 KB, almost entirely the client role.

## Why `CLIENT` and `CONSOLE` have to be added separately

Both are read **outside** this module, by libraries that never include `FileTransferConfig.h`:

| Switch | who else reads it |
|---|---|
| `OPENKNX_FTC_CLIENT` | `lib/knx`, 9 files — that is where the counterpart of the FunctionProperty exchange lives |
| `OPENKNX_FTC_CONSOLE` | `lib/OGM-Common`, `Console.h` — a **data member** (`_lineSink`) hangs on it there |

A `#define` in the header reaches only this module's translation units; a `-D` reaches every file.
That is why these two go in the `ini`, and why the build aborts when a profile is set without them.
The failure mode this prevents:
[CONCEPT-defines.md](CONCEPT-defines.md#client-and-console-do-not-come-from-the-header).

## Without a profile: eleven commands, unprotected

With no switch at all the device answers:

```
FileUpload · FileInfo · FilesystemInfo · Exists · Cancel
Format · Rename · FileDelete
FwUpdate · ModuleVersion · CheckFeatures
```

`FileUpload` is the `safe` path. That is enough for a firmware update over the bus.

**But it runs without access protection.** `Format`, `FileDelete`, `Rename`, `FileUpload` and `FwUpdate`
are open to anyone who knows the address — only `SECURITY` turns that into a login. Whoever builds
minimal on purpose should know that, not discover it.

Missing are: download, directories, `fast`, delta, console, access protection.

## Does delta fit on my device?

The build works that out itself — `OGM-Common/scripts/pio/show_flash_partitioning.py` prints a knxOTA
report at the end of every build:

```
knxOTA  firmware update over the KNX bus
                          over bus    staged        time
  ✔ full image · gzip      1.04 MB   1.04 MB   29-38 min
  ✔ delta patch · typical   139 KB    139 KB  3.8-4.9 min   rebuilt straight into the second slot
```

Two things you can read off it:

- **On ESP32** the reconstructed image is written straight into the second OTA slot. Delta only needs
  room for the patch there — which is why it is the only practical route on tight ESP devices.
- **On RP2040** it is built up as a file in the filesystem. Patch, unpacked patch **and** the finished
  image have to fit in there at the same time. The report says whether that is enough.

`DELTA_UPDATE` is therefore in no end-device profile — it hangs on the board, not on the device class
([CONCEPT-defines.md](CONCEPT-defines.md)).

## What the build catches

Eight misconfigurations abort with a message instead of silently doing something else:

| set | message |
|---|---|
| both profiles at the same time | pick exactly one |
| `PROFILE_MANAGER` without `CLIENT` or `CONSOLE` | names both at once |
| `PROFILE_DEVICE` without `CONSOLE` | names the missing line |
| `GZIP_UPDATE` on RP2040 | the bootloader unpacks there — the switch has no byte of code |
| `SCAN` / `DEVICEINFO` without `CLIENT` | client features, dead without a client |
| `LEGACY_STACK` **and** `CLIENT` | 2.4.0 has none of the 18 entry points |
| `KNXOTA_WEB` without `OPENKNX_WEBSERVER` | there is nothing to serve the page from |
| `KNXOTA_WEB` without `CLIENT` | the page is a front-end onto the client half |

## With knx 2.4.0 you build a full TARGET

Upload · download · `safe` and `fast` · directories · console · access protection · **delta complete** —
everything the device *answers* runs on 2.4.0 (`8e4c5bc`). Only the asking role is missing: `CLIENT`,
`SCAN` and `DEVICEINFO` call 18 entry points that do not exist there — that would be a linker error.
`OPENKNX_FTC_LEGACY_STACK` turns that into a clear statement at compile time.

A side effect of 2.4.0: for every answered chunk `unhandled APDU-Type: 713` appears in the log. The
swallowing branch came later. Log noise, no malfunction.

# Adding FTC to a product

**For:** developers adding this module to an OpenKNX product firmware (OAM). One symlink, one `ini`
block, two lines in `main.cpp`, two lines in the ETS product — verified against the two working
integrations, `OAM-IP-Interface` and `OAM-IP-Router`.

## What you get, and what it costs you

| You want | Switch | Where it shows |
|---|---|---|
| **Server** — the device answers file/firmware commands (obj 159) | none, the module alone | `addModule(9, openknxFileTransferModule)` |
| **Client** — the device asks *other* devices (`ftc <pa> …`) | `OPENKNX_FTC_CLIENT` | `addModule(11, openknxFileTransferClient)` |
| **Console tunnel** — obj 160, a remote console over the bus | `OPENKNX_FTC_CONSOLE` | — |
| **Access control** — login before every write | `OPENKNX_FTC_SECURITY` | ETS page + `op:define prefix="FTM"` |
| **knxOTA web page** | `OPENKNX_FTC_KNXOTA_WEB` | `http://<device-ip>/knxota` |

Switches and their measured cost: **[FLAGS.md](FLAGS.md)** · how it works:
[ARCHITECTURE.md](ARCHITECTURE.md) · access control: [SECURITY.md](SECURITY.md) · knxOTA:
[FIRMWARE-UPDATE.md](FIRMWARE-UPDATE.md).

## 1. Link the module

Both products carry it as a symlink next to `knx` and `OGM-Common`:
`lib/OFM-FileTransferModule -> ../../OFM-FileTransferModule`. It is created by
`lib/OGM-Common/scripts/restore/Restore-Dependencies.ps1` from one line in `dependencies.txt`
(`<hash> <branch> <path> <url>`); `library.json` then pulls one external dependency,
`frankboesing/FastCRC`.

```
8ed3336 ec/v1dev lib/OFM-FileTransferModule https://github.com/OpenKNX/OFM-FileTransferModule.git
```

## 2. The build switches

Both products keep the FTC switches in one reusable `[ftc]` block and pull it per env
(`OAM-IP-Interface/platformio.custom.ini` lines 53-58, identical set in the router lines 43-48):

```ini
[ftc]
build_flags =
  -D OPENKNX_FTC_PROFILE_MANAGER
  -D OPENKNX_FTC_CLIENT
  -D OPENKNX_FTC_CONSOLE
  -D OPENKNX_FTC_DELTA_UPDATE
```

An env opts in with `${ftc.build_flags}` in its `build_flags`. `OPENKNX_FTC_SECURITY` sits in
`[custom]` instead (interface line 24, router line 22) — product-wide, because the ETS product pulls
the FTM parameter share unconditionally.

**Two of these cannot come from `FileTransferConfig.h`** — a `#define` in that header reaches only
this module's translation units. `:24-30` states it, `:65-69` enforces it:

| Switch | read outside this module by |
|---|---|
| `OPENKNX_FTC_CLIENT` | `lib/knx` — 9 files: `application_layer` · `bau_systemB` · `bau.h` · `bau07B0_ip` · `bau091A` … |
| `OPENKNX_FTC_CONSOLE` | `lib/OGM-Common` — `Console.h:42,72`, `Console.cpp:60`, `Log/Logger.h:14`; it gates a **data member** (`_lineSink`) |

## 3. Profiles

`FileTransferConfig.h:31-89`. Set **one**, or none (`CUSTOM` = the bare server core).

| Profile | `FTC_TIER` | adds |
|---|---|---|
| *(none)* | 0 | nothing — core server only |
| `OPENKNX_FTC_PROFILE_DEVICE` | 1 | `SECURITY` `DOWNLOAD` `DIROPS` `FASTUPLOAD` (+ `GZIP_UPDATE` on ESP32) |
| `OPENKNX_FTC_PROFILE_MANAGER` | 2 | tier 1 + `SCAN` `DEVICEINFO` `DELTA_UPDATE` (+ `KNXOTA_WEB` if `OPENKNX_WEBSERVER`) |

A profile only **adds** (`#ifndef` per switch, line 43), so naming `DELTA_UPDATE` next to `MANAGER`
as both products do is redundant but harmless. Eight combinations abort the build instead of doing
something else silently — the `#error` lines in order: `:32` both profiles · `:66` `MANAGER` without
`CLIENT`/`CONSOLE` · `:68` `DEVICE` without `CONSOLE` · `:99` `GZIP_UPDATE` off ESP32 · `:102`
`KNXOTA_WEB` without `OPENKNX_WEBSERVER` · `:105` `KNXOTA_WEB` without `CLIENT` · `:108`
`SCAN`/`DEVICEINFO` without `CLIENT` · `:111` `LEGACY_STACK` with `CLIENT`.

## 4. `main.cpp`

Identical in both products (`OAM-IP-Interface/src/main.cpp:5,20-22,43-46`, router `:5,19-21,42-45`):

```cpp
#include "FileTransferModule.h"
#ifdef OPENKNX_FTC_CLIENT
#include "FileTransferClient.h"
#endif
...
    openknx.addModule(9, openknxFileTransferModule);
#ifdef OPENKNX_FTC_CLIENT
    openknx.addModule(11, openknxFileTransferClient);
#endif
```

The **server is unconditional** — also in envs that do not pull `${ftc}` (the interface's
`release_REG2_PICO_W_ETH_DD`: core server + access control, no client, no console tunnel). Module
numbers in both products: `6` product module · `7` `openknxNetwork` · **`9` FTC server** ·
`10` display · **`11` FTC client** · `30` SD card. `Facade::addModule` (OGM-Common
`src/OpenKNX/Facade.cpp:70`) does **not** reject a duplicate id; `getModule(id)` (`:93`) returns the
first match, so a collision leaves the second module unreachable by number, with no diagnostic.

## 5. The ETS side

One `op:define` in the product XML pulls `src/FileTransfer.share.xml` (identical line in
`IP-Interface-Dev.xml:79`, `IP-Interface-Release.xml:54`, `IP-Router-Dev.xml:48`):

```xml
<op:define prefix="FTM" ModuleType="13" share="../lib/OFM-FileTransferModule/src/FileTransfer.share.xml" />
```

`ModuleType` must be free **inside that product** — the interface has 10 BASE · 11 NET · 12 IPI ·
99 ROUTE, the router 10 · 11 · 99; both picked 13. The share ships a
`ParameterBlock Name="ExtendedInject"` (`FileTransfer.share.xml:63`) — a fragment, not a page. Both templates rebuild the Common "OpenKNX" channel so it folds into **Erweitert**:

```xml
<!-- OAM-IP-Interface/src/TemplateInterface.xml:120-123 -->
<ParameterBlock Id="%AID%_PB-nnn" Name="Extended" Text="Erweitert" Icon="format-list-text" HelpContext="BASE-OpenKNX">
  <op:include href="../lib/OGM-Common/src/Common.share.xml" xpath="//ApplicationProgram/Dynamic/Channel/ParameterBlock[@Name='Extended']/*" IsInner="true" prefix="BASE" />
  <op:include href="../lib/OFM-FileTransferModule/src/FileTransfer.share.xml" xpath="//Dynamic/Channel/ParameterBlock/*" IsInner="true" prefix="FTM" />
</ParameterBlock>
```

`OAM-IP-Router/src/TemplateRouter.xml:83-86` is the same three lines with `Common.Router.share.xml`
as the BASE source (0 ComObjects → the router stays KO-free; the FTM share carries none either). Its
own comment: "Mirrors TemplateInterface.xml, only the source differs."

The block appears **iff** `FTM` is `op:define`'d — no `op:if` needed. The module's context help
(`src/Baggages/Help_de/FTM-Zugriff|Passwort|Abmeldung.md`) rides along on the
`<op:includetemplate href="%share%" xpath="//Manufacturer/Baggages/*" prefix="%prefix%" />` line
already present in both templates.

## 6. The knxOTA web page

Neither product sets `OPENKNX_FTC_KNXOTA_WEB` — `FileTransferConfig.h:76` defines it automatically
when `MANAGER` meets `OPENKNX_WEBSERVER` (both set that in their `[web]` block). It needs a web
server *and* the client half, and says so at `:102` / `:105`. Cost, measured
(`FileTransferConfig.h:74`): **34 732 B flash + 80 B RAM on RP2040**, **40 056 B + 64 B RAM on
ESP32**. Its assets (`web/assets/knxota.css`, `knxota.js`) are embedded by OGM-Common
`scripts/pio/prepare_webassets.py`, which runs for any product defining `OPENKNX_WEBSERVER`.

## 7. Check it worked

| Check | Expect |
|---|---|
| `pio run -e release_REG2_PICO_ETH_DD` | builds; the knxOTA report from `show_flash_partitioning.py` prints at the end |
| serial console: `ftm` | `stage: … [ETS]` · `idle window: … s` · `authorized now: …` — the command exists only with `OPENKNX_FTC_SECURITY` (`FileTransferModule.h:22-24`) |
| serial console: `help` | an `ftc` line — registered only with `OPENKNX_FTC_CLIENT` |
| from a *second* device or the PC binary: `ftc <pa> info` | FTM version + feature bits of the target |
| ETS, after `Build-knxprods.ps1` | OpenKNX ▸ Erweitert ▸ "Zugriffsschutz Service & Wartung" |

You cannot test a device against itself — `ftc <own-PA>` times out by KNX design (QUICKSTART.md).

## 8. Common mistakes

| Mistake | Symptom |
|---|---|
| profile set, `CLIENT`/`CONSOLE` not | build aborts, `FileTransferConfig.h:66` / `:68` names what is missing |
| `OPENKNX_FTC_CONSOLE` put in a header instead of `-D` | OGM-Common compiles `Console` without `_lineSink`, this module with it — two halves, one class (`Console::submitLine` 104 B vs 112 B) |
| a switch renamed on one side only | it silently never compiles — precedent in the repo: `platformio.custom.ini:63`, `[FIX: was OPENKNX_BCU_REGISTER_INFO -> never compiled on RP]` |
| `SECURITY` built, `op:define prefix="FTM"` missing | params unconfigured → the device behaves like stage `Always`, writes stay open, no ETS page ([SECURITY.md](SECURITY.md)) |
| `op:define` present, template `op:include` missing | the parameters are generated by the generic `%share%` includes but nothing references them — no page appears |
| `addModule` number reused | no error; `getModule(id)` returns the first, the second is unreachable |
| `KNXOTA_WEB` on a product without a web server | build aborts, `FileTransferConfig.h:102` |
| `MANAGER` on a tight board | ~120 KB flash for the client role, and delta needs room for patch **and** rebuilt image — use `PROFILE_DEVICE` plus hand-picked switches ([FLAGS.md](FLAGS.md)) |

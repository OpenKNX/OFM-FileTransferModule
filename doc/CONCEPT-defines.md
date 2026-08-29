# Why the FTC switches look the way they do

**For:** developers changing the switch scheme or reviewing it. The reasoning behind
[FLAGS.md](FLAGS.md) — anyone who only wants to know what to write into their `ini` does not need
this document.

## The problem that was solved

There were two kinds of switches: ones you turned on, and ones that were already on and had to be taken
away with `=0`. A name did not show you which kind it belonged to. Some could **be set and still do
nothing** — `SCAN`/`DEVICEINFO` without a client, `GZIP_UPDATE` on RP2040 — and one was checked at no
single place at all: measured effect 0 bytes.

## Five rules

**R1 — A switch is on or off.** Set means on, otherwise off. No values, no default to take away.
"Nothing set" is thereby a clearly defined state: the server core.

**R2 — The prefix `OPENKNX_` says who sets it.** Only `OPENKNX_FTC_…` belongs to the product. The about
seventy `FTC_…`/`FTM_…` names are module-internal. Without that separation, seventeen switches cannot be
found among ninety names.

**R3 — No switch may be silently ineffective.** It has an effect, or the build aborts. Eight
misconfigurations catch that today; every one of them was silent before. The list:
[FLAGS.md](FLAGS.md#what-the-build-catches).

**R4 — Profiles propose, they do not prescribe.** A profile sets a set of switches and never takes away
anything that is stated explicitly.

**R5 — One switch, one role.** A server switch takes *answering* away from the device, never *asking*.
Whoever talks to foreign devices has to be allowed to ask, independently of his own equipment.

## Three decisions you cannot see in the result

### `SECURITY` is in every profile

The core answers `Format`, `FileDelete`, `Rename`, `FileUpload` and `FwUpdate` **without a login**.
Whoever builds that way on purpose should know it; whoever takes a profile should not have to configure
it first. The console additionally pulls in access protection unconditionally — an unauthenticated
console tunnel is not a build option.

### `DELTA_UPDATE` is in no end-device profile

It hangs on the **board**, not on the device class:

- **ESP32** writes the reconstructed image straight into the second OTA slot
  (`Update.begin()`/`Update.write()`). Only the patch lies in the filesystem — a few tens of KB
  ([DELTA.md](DELTA.md)).
- **RP2040** builds it up as a file. Patch, unpacked patch **and** the finished image have to fit in
  there at the same time.

Only the concrete board knows whether that is enough. The build works it out and prints it
(`OGM-Common/scripts/pio/show_flash_partitioning.py`) instead of a profile guessing it. `MANAGER` has it
anyway: interface, router and host have the space and also need the *sending* half in order to send
patches to other devices.

### `CLIENT` and `CONSOLE` do not come from the header

That is the rule which caused the most trouble, and it is the most important one.

A `#define` in `FileTransferConfig.h` only reaches the translation units of **this module**. Both
switches, however, are read outside, by libraries that never include the header and must not depend on
this module:

| Switch | who else | what hangs on it there |
|---|---|---|
| `OPENKNX_FTC_CLIENT` | `lib/knx`, 9 files | the counterpart of the FunctionProperty exchange |
| `OPENKNX_FTC_CONSOLE` | `lib/OGM-Common`, `Console.h` | a **data member**, `_lineSink` |

If `CONSOLE` came from the header, OGM-Common would compile `Console` without `_lineSink` and the module
with it — two halves of the same program with different ideas of the same class. Measured:
`Console::submitLine` is 104 B without the switch and 112 B with it.

That is why both have to come as a `-D`, and that is why the build aborts when a profile is set without
them. The guard is not convenience, it is the only place where this error shows up.

## What was found during the changeover

**`FirmwarePatch.cpp` did not include the config header** and still checked `OPENKNX_FTC_DELTA_UPDATE` — with
profiles in the header the file compiled to nothing, the host aborted with missing `FirmwarePatch::Job` symbols.
All four module `.cpp` files now include it first, as the header demands at the top.

**`DOWNLOAD` violated R5.** It also switched off `FileTransferClient*.cpp`, `FASTUPLOAD` and `DIROPS`
did not. A management device that did not offer download itself could therefore not fetch anything from
others either. The seven client gates now hang on `CLIENT`.

**`GZIP_UPDATE` stood at "on" on RP2040 and did nothing** — all blocks lie in the ESP32 branch of
`FileTransferModule.cpp`. The build now aborts there instead of tolerating an ineffective switch.

**Two empty `case` branches in `knx/src/knx/application_layer.cpp` hung on FTC switches.** They swallow
the send confirmation of the device's own FunctionProperty frames. The second one affects **every**
device that answers FTC — including one without a console and without a client; until now that logged
one `unhandled APDU-Type: 713` line per chunk. Both are unconditional now, cost practically zero.

## What this means for an existing product

**`DOWNLOAD`, `FASTUPLOAD`, `DIROPS` and `GZIP_UPDATE` no longer switch themselves on.** A product that
has no FTC line today loses them on the changeover — silently, because missing features are not a
compile error.

That is why the matching profile plus the one or two `-D` lines belongs in every product on the
changeover. Whoever does it has more than before; whoever forgets it has the core. The changeover
belongs at the very top of the release log.

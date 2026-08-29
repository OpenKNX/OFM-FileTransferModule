<!--
  ┬────┴  OFM-FileTransferModule
  ■ KNX   2026 OpenKNX - Erkan Çolak
-->
# OFM-FileTransferModule — files, console and firmware over the KNX bus

An OpenKNX module that turns a plain KNX telegram exchange into a **transport**. It is called
FileTransferModule because that was the first thing it carried; today the same channel also carries an
**interactive device console**, **firmware updates over the bus**, **directory operations**, **device
discovery** and an **access-control layer** — all over standard KNX, with no extra port, no serial cable
and no cloud.

Three front ends speak the same protocol: a **desktop client** (`ftc`, macOS/Linux/Windows), the **device
console** on any OpenKNX device, and a **web interface** in the browser.

---

## Table of contents

- [What it carries](#what-it-carries)
- [The three front ends](#the-three-front-ends)
- [knxOTA — firmware over the KNX bus](#knxota--firmware-over-the-knx-bus)
- [How the transport works](#how-the-transport-works)
- [Access control](#access-control)
- [Throughput — what is realistic](#throughput--what-is-realistic)
- [Build switches and profiles](#build-switches-and-profiles)
- [Documentation](#documentation)
- [KNX conformance and warranty](#knx-conformance-and-warranty)
- [Author and licence](#author-and-licence)

---

## What it carries

### Files
Upload, download, delete, rename; create and remove folders; list a directory with CRC and a storage bar;
query filesystem usage; format. A transfer can be resumed: the device reports which blocks it already holds,
and only the gaps are sent again.

**Three drives**, selected by prefixing the remote path:

| Prefix | Storage |
|---|---|
| *(none)* | internal LittleFS |
| `sd/` | SD card |
| `efc/` | external flash |

### An interactive console
A second channel (`OPENKNX_FTC_CONSOLE`) carries the device's own console. Every command the device offers
on its serial line is available remotely — over the bus from another device, over a tunnel from a PC, or in
the browser. Output is drained through a bounded ring, so a chatty command cannot flood the bus.

Recurring commands are possible without a PC watching: `/job add watch every <interval> <command>` runs a
command on a schedule and is bus-friendly, one outstanding request at a time.

### Firmware
See [knxOTA](#knxota--firmware-over-the-knx-bus) below — whole image, gzip-compressed, or only the
**difference** to a release you already shipped.

### Device information and discovery
Fingerprint a target: mask version, device class, FTM version, supported features, the tunnel addresses of
an interface, the group-communication tables the way ETS shows them, bus voltage. Scan a line or an area
for devices, optionally reading each one's identity while scanning. Find devices in programming mode and
localise which line they are on. Drive the programming LED to identify a device in a cabinet.

### Live monitors
Decoded group telegrams, or the raw LPDU with the acknowledge colouring ETS uses. Two interfaces can be run
side by side and their captures diffed against each other — that is how the busmonitor fidelity of this
implementation was established against a commercial interface.

---

## The three front ends

| | Runs on | Reaches | Typical use |
|---|---|---|---|
| **`ftc`** desktop client | macOS · Linux · Windows | any device, through a KNXnet/IP tunnel — including third-party interfaces | the PC has the files, the screen and the firmware images |
| **Device console** | any OpenKNX device with `OPENKNX_FTC_CLIENT` | any other device, PA to PA over the bus | no PC on site; one device services another |
| **Web interface** | the device's own browser page | itself, and other devices via knxOTA | file manager, console, knxOTA, all without a tool |

They are not three implementations. The desktop client compiles the **same state machine** as the firmware,
byte-identical, with a host shim standing in for the Arduino stack. A protocol change lands in all three at
once, and cannot drift apart.

```bash
# from a PC, through an interface at 11.11.0.126, talking to device 5.0.3
ftc --discover                                   # which interfaces exist on the LAN
ftc -i 11.11.0.126 5.0.3 info                    # fingerprint the device
ftc -i 11.11.0.126 5.0.3 send fw.bin.gz -fa      # upload fast, then flash and reboot
ftc -i 11.11.0.126 5.0.3 con                     # open its console over the bus

# on a device console, without any PC
ftc 5.0.3 send /cfg.json /cfg.json               # this device -> 5.0.3
```

---

## knxOTA — firmware over the KNX bus

Updating a device normally means a USB cable or a network port. **knxOTA does it over the KNX line itself**,
which is often the only wire that reaches a device already mounted in a cabinet.

### How an update runs

1. **Check first.** `knxota <file> --check` compares the image against what the device reports and writes
   nothing. It tells you whether the image fits, whether it is a downgrade, and what the transfer would cost.
2. **Prepare.** The image is compressed. On ESP32 the device unpacks it itself; on RP2040 the bootloader
   does, which is why the compressed form differs per platform.
3. **Transfer.** The image goes over the tunnel or PA to PA, in blocks of at most 247 bytes — that is what a
   KNX APDU holds. Each block is verified; only gaps are resent.
4. **Apply.** The device verifies the whole image, writes it to the update partition and reboots into it.
   Silence means success — a device that refuses answers with a code instead.

### Delta updates — why they matter here

A full firmware image is roughly one megabyte. At the throughput a KNX line allows, that is **half an hour
or more**. Most updates change a fraction of that.

`--from <previous release>` sends only the **difference** to a release the device already runs. The patch is
built on the PC, transferred, and applied on the device. Typical updates drop from half an hour to a few
minutes. Without the option, `knxota` offers whichever previous releases it can find.

The device must have room for the running image **and** the patch — that is why the delta switch is stated
explicitly per product rather than assumed.

### What it will not do

It refuses a downgrade and an image without a recognisable marker, unless you insist with `--force`. It
tells you what happened rather than reporting success on trust: exit code `0` done, `1` nothing to do,
`3` the device refused, `6` no answer.

---

## How the transport works

Every operation is one `A_FunctionProperty_Command` on an interface object — the same mechanism ETS uses to
call a function on a device. There is no stream, no custom port, nothing outside standard KNX.

| Object | Carries |
|---|---|
| **159** | the file-transfer command table — upload, download, directory operations, filesystem info, firmware update, feature query, authentication |
| **160** | the console channel — one line in, the log ring out |

A payload is at most **247 bytes**, the APDU minus its header. Two upload modes exist:

- **safe** — every block is acknowledged before the next one is sent. Self-pacing: it adapts to whatever the
  bus and the target can take, and survives a congested line.
- **fast** — a window of blocks is sent, then the device is asked which ones arrived and only the gaps are
  repeated. Faster on a quiet bus, more sensitive to a busy one.

The received bit for a block is set only after its CRC verifies **and** the write succeeds, so a reported
block is a block that is really on the device.

---

## Access control

Optional (`OPENKNX_FTC_SECURITY`) and, when compiled in, it gates **every write**: uploads, deletions,
firmware updates and the console. Reads stay open.

Four stages, configured in ETS per project: **off**, **programming mode only**, **always**, **password**.
With a password, the device issues a challenge and the client answers with a MAC computed locally — the
password itself never travels on the bus. A verified session is refreshed by each accepted write and closes
on logout or when it goes idle. Repeated failures back off without blocking the device.

---

## Throughput — what is realistic

Measured, not estimated: **roughly 350 to 650 bytes per second**, depending on the interface. Two figures
from a real installation: about **430 B/s** with `safe` from a PC, about **440 B/s** with `fast` over the
device console.

The limit is neither the client nor the tunnel — it is the **target device**, writing to flash while running
the KNX stack. Consequences that are settled and not worth re-exploring:

- pushing harder does not help; past roughly 450 B/s an RP2040 target is driven into a reboot
- a second parallel tunnel makes it worse, not better: one target is one processor
- any real speed-up has to move work **onto the device** — batching flash writes, less work per block

This is why a delta update is worth so much more than a faster transfer.

---

## Build switches and profiles

Everything is opt-in. A device that only needs to receive files carries nothing else.

| Switch | Adds |
|---|---|
| `OPENKNX_FTC_CONSOLE` | the console channel (object 160) |
| `OPENKNX_FTC_SECURITY` | the access-control layer |
| `OPENKNX_FTC_DOWNLOAD` | reading files off the device |
| `OPENKNX_FTC_DIROPS` | mkdir, rmdir, rename |
| `OPENKNX_FTC_FASTUPLOAD` | the windowed upload mode |
| `OPENKNX_FTC_GZIP_UPDATE` | compressed firmware (ESP32 — the RP2040 bootloader unpacks by itself) |
| `OPENKNX_FTC_DELTA_UPDATE` | difference updates; the board must hold image **and** patch |
| `OPENKNX_FTC_SCAN`, `OPENKNX_FTC_DEVICEINFO` | scanning and fingerprinting |
| `OPENKNX_FTC_CLIENT` | the client role — this device can drive others |
| `OPENKNX_FTC_KNXOTA_WEB` | the knxOTA page in the web interface |

Two profiles bundle the sensible sets, and only ever **add** — anything already set in the ini stays:

- **`OPENKNX_FTC_PROFILE_DEVICE`** — a managed end device: security, download, directory operations, fast
  upload, and gzip where the platform supports it
- **`OPENKNX_FTC_PROFILE_MANAGER`** — the full client role, every feature including delta updates

Exactly one profile may be set; two are rejected at compile time.

---

## Documentation

| Document | For |
|---|---|
| [doc/QUICKSTART.md](doc/QUICKSTART.md) | five minutes, three front ends, one first firmware update |
| [doc/README.md](doc/README.md) | the index — every document with the audience it is written for |
| [doc/FIRMWARE-UPDATE.md](doc/FIRMWARE-UPDATE.md) | knxOTA in detail |
| [doc/DELTA.md](doc/DELTA.md) | how a difference update is built and applied |
| [doc/PROTOCOL.md](doc/PROTOCOL.md) | the wire format, command by command |
| [doc/SECURITY.md](doc/SECURITY.md) | the access-control layer |
| [doc/THROUGHPUT.md](doc/THROUGHPUT.md) | what was measured, and why the ceiling is where it is |
| [doc/CONSOLE.md](doc/CONSOLE.md) | the console channel |
| [doc/ARCHITECTURE.md](doc/ARCHITECTURE.md) | how the pieces fit together |
| [doc/FLAGS.md](doc/FLAGS.md) · [doc/CONCEPT-defines.md](doc/CONCEPT-defines.md) | every build switch |
| [doc/WEB.md](doc/WEB.md) · [doc/SCRIPTS.md](doc/SCRIPTS.md) · [doc/INTEGRATION.md](doc/INTEGRATION.md) | web interface, tooling, putting the module into your own OAM |
| [doc/errorcodes.txt](doc/errorcodes.txt) | what a result code means |
| [ftc-cli/README.md](ftc-cli/README.md) | the desktop client |

---

## KNX conformance and warranty

This is **open source**, and it is written to meet the requirements the KNX specification places on the
mechanisms it uses. The transport is not an invention: it is `A_FunctionProperty_Command` on an interface
object, the payload limit is the APDU limit, and the objects, property identifiers and result codes follow
the standard.

**How that was checked.** The behaviour was evaluated against the specification with scripted test suites
that drive a real device and name, per test case, the clause being verified — Core, Device Management,
Tunnelling, Routing, Remote Diagnosis and device-level checks. Where a measurement contradicted an
assumption, the assumption was corrected, not the measurement.

**Where it deliberately steps outside.** The `fast` and `forget` upload modes trade protocol
acknowledgement for speed. That is a conscious departure, it is documented as such, it is off by default,
and it works only between OpenKNX devices. It is not offered as standard behaviour.

**Warranty.** Everything here was implemented to the best of the author's knowledge and belief and verified
as described above. Nothing is guaranteed. This is not a KNX certification — that is a separate formal
process and is not claimed. The software is provided **as is**, without warranty of any kind, express or
implied; see the licence for the binding wording. You use it on your own installation at your own risk.

---

## Author and licence

Written by **Erkan Çolak** for OpenKNX.

Licensed under the **GNU General Public License v3** — see [LICENSE](LICENSE).

- <https://openknx.de> · <https://wiki.openknx.de> · <https://forum.openknx.de>

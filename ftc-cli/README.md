<!--
  ┬────┴  OFM-FileTransferModule / ftc-cli
  ■ KNX   2026 OpenKNX - Erkan Çolak
-->
# FTC `ftc` — native desktop FileTransferClient (macOS / Linux / Windows)

A native C++ command-line tool that talks to any KNX device on the bus **from your PC**, over a single
**KNXnet/IP tunnel** to a Router or Interface. It **reuses the embedded FTC protocol code unchanged** — one
protocol source, no drift: every future firmware FTC change is inherited here automatically.

**Zero firmware change:** the Router/Interface is just the tunnel; the target's FTC server already exists.

```bash
ftc --discover                                  # find interfaces on the LAN
ftc --ip 11.11.0.126 5.0.3 info                 # fingerprint a device
ftc --ip 11.11.0.126 5.0.3 send fw.bin.gz fast  # push a file to it
ftc --ip 11.11.0.126 5.0.3 con                  # open its remote console
```

`<pa>` is the **target device** on the bus (e.g. `5.0.3`), not the interface. `--ip` is the interface you
tunnel through. Run `ftc --help` for the full, always-current reference.

Real output of the first line above, on a live installation:

```
  suche 224.0.23.12:3671 …
  ● 11.11.0.31   OpenKNX: IP-Router REG2 - ESP     TP1 (twisted pair)
  ● 11.11.0.151  OpenKNX: IP-Router (Dev)          TP1 (twisted pair)
  ● 11.11.0.210  OpenKNX: IP-Interface - REG1 E    TP1 (twisted pair)
  ● 11.11.0.3    IP-Router N 146/02                TP1 (twisted pair)
  ● 11.11.0.5    IP Interface N148 - ALT           TP1 (twisted pair)
  5 Interface(s)
```

It finds third-party interfaces just as well as OpenKNX ones, and tunnels through them.

## Features

**Transfer & files** — over the tunnel, PA-to-PA
- `send`/`upload` a host file · modes **safe** (CRC per chunk) and **fast** (AIMD window; `fast w<N>` pins it)
- `get`/`download` a file back to the PC
- `ll`/`ls` (with CRC + a storage bar), `df` usage, `rm`, `mkdir`, `rmdir`, `mv`, `format`
- drives: **LittleFS** (default), **`sd/`** (SD card), **`efc/`** (external flash) — prefix any remote path

**Device info & discovery**
- `info` — full fingerprint (mask / class / FTM version / features / tunnel PAs), `info ga` (group-comm / GA
  table, ETS-style), `info <file>`
- `ping` round-trip, `scan <line|range>` (optional CO probe, parallel `--tunnels`)
- `progscan`/`ps` — find devices in programming mode and localise the line, `--discover` — list LAN interfaces

**Live monitors**
- `groupmon`/`gm` — decoded group telegrams · `busmon`/`bm` — raw LPDU with ETS ACK colouring
- `--frames N` / `--seconds N` to stop after N (scriptable)
- `gm|bm compare <ipB>` — run two interfaces side by side and diff what each one saw; reassembles split
  telegrams, `--raw` keeps the pieces, `--multi` reads two streams. This is how the busmonitor fidelity
  against a commercial interface was established
- `led on|off|blink` — drive the target's programming LED to find it in the cabinet

**Remote console & firmware**
- `con`/`console` — a device's console over the KNX tunnel (or an interface's own webconsole); `/job` recurring
  auto-commands, `/stat`, `--log` to file
- `fwupdate` — flash an uploaded firmware (reboots the target) · `perf` — throughput test


**Firmware over the bus — `knxota`**
- `knxota <file.uf2|.bin>` — update a device from a firmware file **on this PC**; without `--ip` and an
  address it asks which interface and which device
- `--from <folder|.app.bin>` — send only the **difference** to that release. Minutes instead of half an
  hour; without the option `knxota` offers what it finds
- `--check` — compare and report only, write nothing. Run this first
- `--no-delta` full image · `--no-compress` uncompressed · `--force` allow a downgrade or an unmarked file
- `<pa> fwupdate <remote>` — flash a firmware the device already holds, then reboot
- exit codes: `0` done · `1` nothing to do · `2` usage · `3` device refuses · `6` no answer

**Access control**
- `login`/`logout` — unlock write actions on password-protected targets (the MAC is computed locally, the
  password never goes on the bus)
- `feat` — what the target supports, and why it refuses a write

**Local utilities — no bus, no interface**
- `gzip` (firmware prep) · `decode <hex LPDU>` (decode a raw TP1 frame offline)
- `install` / `uninstall` — put this `ftc` on the PATH, version-aware and asking before a downgrade
- `config <key> <value>` — persisted defaults · `retry` — how often a transfer repeats and how long it waits
- `--theme green|amber|cyan` · `--lang de|en` · `--ascii` for terminals without box drawing
- `--quiet` — no chrome, TSV output, automatic when stdout is not a terminal, so it scripts cleanly
- `--log[=path]` — write the console session to a file
- `--prio low|normal|urgent|system` — KNX priority of the FTC frames; raising it warns and asks

## The same client also runs on the device

`ftc` on your PC and the **embedded FTC client** in the firmware are the *same state machine*. On a device
with `OPENKNX_FTC_CLIENT` the console offers `ftc` as a command, and one device transfers to another
**PA to PA over the bus** — no PC involved:

```
ftc 5.0.3 send /cfg.json /cfg.json      # on the device console: this device -> 5.0.3
ftc 5.0.3 con                           # open another device's console from this one
```

The desktop tool exists because a PC has the files, the screen and the firmware images. What it does not
have is a bus connection — that is what the tunnel is for.

## How it works — the glue-shim (for developers)

`FileTransferClient.{h,cpp}` + `FileTransferClientConsole.{h,cpp}` from `../src/` are compiled
**byte-identical**. A host-only shim replaces the OpenKNX/knx/Arduino stack with just the thin slice those
four files touch, and the `knx.bau().ftc*` calls forward to a KNXnet/IP tunnel. The wire format is the same
one the firmware speaks — see [../doc/PROTOCOL.md](../doc/PROTOCOL.md). **No `lib/knx` changes.**

```
  ../src/FileTransferClient.cpp  (UNCHANGED state machine)
        │ calls knx.bau().ftc*  +  openknx.logger/console  +  millis()  +  LittleFS
        ▼
  shim/  OpenKNX.h · knx_shim.h(HostBau) · openknx_shim.h · LittleFS.h   ← replaces the stack
        │ HostBau forwards the ftc* methods to ↓
  src/   knx_ip_tunnel.{h,cpp}   ← KNXnet/IP tunnel: cEMI/APDU/CRC, connect, tunneling, CO-scan
  src/   main.cpp                ← CLI, native file backend, loop driver (pump tunnel + client), stdin console
```

## Layout

| Path | Owner | Contents |
|------|-------|----------|
| `../doc/PROTOCOL.md` | spec | the wire format both firmware and this tool speak |
| `doc/CONCEPT-api.md` | concept | the planned local HTTP/SSE API (`ftc --api`) |
| `src/knx_ip_tunnel.h` | **fixed contract** | the transport seam (authored, do not widen casually) |
| `shim/*.h` | shim | host stand-ins; makes the 4 unchanged files compile |
| `src/knx_ip_tunnel.cpp` | transport | the real KNXnet/IP tunnel client |
| `src/main.cpp` | cli | argument parsing, backends, loop, discovery, console stdin |
| `src/cli/*.h`, `src/core/*.h` | cli | UI templates + reusable core (discovery / describe / gzip) |
| `src/third_party/miniz.*` | vendored | miniz (public-domain deflate) for `gzip` |
| `platformio.ini` | build | one env per target; `pio run` cross-builds the whole matrix |
| `scripts/pio_zig_cross.py` | build | pio pre-hook: pulls zig + sets the cross-compiler per target |

## Build (all OSes)

One `pio run` cross-builds every target — pio pulls a project-local `zig` itself for the cross OSes:

```bash
pio run                       # -> .pio/build/<env>/ftc[.exe] for all targets
pio run -e ftc-cli-linux-x64  # a single target
./.pio/build/ftc-cli-macos-arm64/ftc --help
```

**Targets (8):** `macos-arm64`, `macos-x64`, `linux-x64`, `linux-arm64`, `linux-armhf`, `windows-x86`,
`windows-x64`, `windows-arm64`. A macOS host builds the mac targets natively; Linux/Windows cross-build from
it via zig.

## Status

Feature-complete and driven against real hardware (interface + target on a live bus). The remaining work is
throughput/robustness tuning under bus congestion, not missing features.

## Author & license

Written by **Erkan Çolak** as part of the OpenKNX FileTransferModule.

Licensed under the **GNU General Public License v3** — see the module's [LICENSE](../LICENSE).

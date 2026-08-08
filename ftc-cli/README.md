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

**Remote console & firmware**
- `con`/`console` — a device's console over the KNX tunnel (or an interface's own webconsole); `/job` recurring
  auto-commands, `/stat`, `--log` to file
- `fwupdate` — flash an uploaded firmware (reboots the target) · `perf` — throughput test

**Access control & local utilities**
- `login`/`logout` — unlock write actions on password-protected targets (MAC computed locally)
- `gzip` (firmware prep), `decode` (offline raw-LPDU decode), `config` (persisted defaults), `--theme`
  green/amber/cyan, `--lang de|en`

## How it works — the glue-shim (for developers)

`FileTransferClient.{h,cpp}` + `FileTransferClientConsole.{h,cpp}` from `../src/` are compiled
**byte-identical**. A host-only shim replaces the OpenKNX/knx/Arduino stack with just the thin slice those
four files touch (see `doc/FTC-HOST-SHIM-CONTRACT.md`). The `knx.bau().ftc*` calls forward to a KNXnet/IP
tunnel (`doc/FTC-WIRE-PROTOCOL.md`). **No `lib/knx` changes.**

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
| `doc/FTC-HOST-SHIM-CONTRACT.md` | spec | exact symbols the shim must provide |
| `doc/FTC-WIRE-PROTOCOL.md` | spec | byte-exact APDU/cEMI/CRC/CO-scan |
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

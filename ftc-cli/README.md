<!--
  ┬────┴  OFM-FileTransferModule / ftc-cli
  ■ KNX   2026 OpenKNX - Erkan Çolak
-->
# FTC ftc-cli — native desktop FileTransferClient (Mac / Linux / Windows)

A native C++ CLI that **reuses the embedded FTC protocol code unchanged** and drives it over a
**KNXnet/IP tunnel** to a Router/Interface. From your PC, over one tunnel, to any device on the bus:
file transfer (`send`/`get`/`df`/`ll`/`info`), device scan, and a live remote **console**.

**Zero firmware change** on the Router/Interface (it is just the tunnel) and on the target (its FTC
server already exists). One protocol source → no drift: every future FTC change is inherited here.

## How it works — the glue-shim

`FileTransferClient.{h,cpp}` + `FileTransferClientConsole.{h,cpp}` from `../src/` are compiled
**byte-identical**. A host-only shim replaces the OpenKNX/knx/Arduino stack with just the thin slice
those four files touch (see `doc/FTC-HOST-SHIM-CONTRACT.md`). The 14 `knx.bau().ftc*` calls forward to
a KNXnet/IP tunnel (`doc/FTC-WIRE-PROTOCOL.md`). **No `lib/knx` changes.**

```
  ../src/FileTransferClient.cpp  (UNCHANGED state machine)
        │ calls knx.bau().ftc*  +  openknx.logger/console  +  millis()  +  LittleFS
        ▼
  shim/  OpenKNX.h · knx_shim.h(HostBau) · openknx_shim.h · LittleFS.h   ← replaces the stack
        │ HostBau forwards the 14 methods to ↓
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
| `platformio.ini` | build | one env per target; `pio run` cross-builds the whole matrix |
| `scripts/pio_zig_cross.py` | build | pio pre-hook: pulls zig + sets the cross-compiler per target |

## Build (all OSes)

One `pio run` cross-builds every target — pio pulls a project-local `zig` itself for the cross OSes:

```bash
pio run                       # -> .pio/build/<env>/ftc[.exe] for all targets
pio run -e ftc-cli-linux-x64  # a single target
./.pio/build/ftc-cli-macos-arm64/ftc --help
```

Targets: `macos-arm64`, `macos-x64`, `linux-x64`, `linux-arm64`, `windows-x86`, `windows-x64`,
`windows-arm64`. macOS builds need a macOS host; the Linux/Windows targets cross-build from it via zig.

## Status

Bring-up. HW round-trip (real interface IP + target PA) is the final acceptance step.

# `ftc` — the desktop client

**For:** anyone operating a device from a PC. One binary, no dependencies; it talks to any device on
the bus through a single KNXnet/IP tunnel to a Router or Interface.

It is not a rewrite: `FileTransferClient.cpp` from `src/` is compiled unchanged against a host shim
([ARCHITECTURE.md](ARCHITECTURE.md)).

Targets: macOS arm64/x64 · Linux x64/arm64/armhf · Windows x86/x64/arm64.

## The two addresses

```bash
ftc --ip 11.11.0.126 5.0.3 info
        └─ the interface       └─ the target on the bus
           you tunnel through
```

`--ip` is never the target. Forgetting that is the most common mistake.

## Commands

**Files and transfer**

| | |
|---|---|
| `send` / `upload <file> [safe\|fast] [w<N>]` | push a host file; `fast` pins its window with `w<N>` |
| `get` / `download <remote> [local]` | pull a file back to the PC |
| `ll` / `ls [path]` | listing with CRC and a storage bar |
| `df` | capacity of the target's filesystem |
| `rm`, `mkdir`, `rmdir`, `mv`, `format` | the usual, all gated by access control |
| `info <file>` | size and checksum of one remote file |

Drives: prefix the remote path with `sd/` or `efc/`; no prefix is the internal LittleFS.

**Firmware**

| | |
|---|---|
| `knxota <file>` | transfer and apply in one go, with reachability and chip checks |
| `apply` / `fwupdate <file>` | apply an already transferred image; the target reboots |
| `delta <old> <new>` | build a difference (`.okd`) from two images |
| `resume` | continue an interrupted run instead of restarting |
| `gzip <file>` | pack a full image |
| `--check` / `--force` | probe only · skip the confirmations |

**Device information**

| | |
|---|---|
| `info` | full fingerprint: mask, device class, FTM version, feature bits, tunnel addresses |
| `info ga` | group communication and the GA table, read the way ETS reads it |
| `ping` | round trip to one PA |
| `scan <line\|range>` | sweep a line; `--tunnels` runs several in parallel |
| `progscan` / `ps` | find devices in programming mode and localise the line |
| `--discover` | list the KNXnet/IP interfaces on the LAN |

**Live monitors**

| | |
|---|---|
| `groupmon` / `gm` | decoded group telegrams |
| `busmon` / `bm` | raw LPDU with ETS ACK colouring — green means *not* acknowledged |
| `compare <ipB>` | run two interfaces side by side and diff what they see |
| `--frames N` / `--seconds N` | stop after N, so it can be scripted |

**Console and access**

| | |
|---|---|
| `con` / `console` | the target's console over the tunnel ([CONSOLE.md](CONSOLE.md)), or an interface's own webconsole |
| `/job add watch every <n> <cmd>` | recurring auto-command inside a console session |
| `/stat` | session statistics, also written to the log file on exit |
| `--log <file>` | mirror the session to a file |
| `login <password>` / `logout` | unlock and lock write actions ([SECURITY.md](SECURITY.md)); the MAC is computed locally, the password never goes on the wire |

**Local**

| | |
|---|---|
| `decode` | offline decode of a raw LPDU |
| `config` | persisted defaults |
| `--theme green\|amber\|cyan`, `--lang de\|en` | appearance |
| `--verbose` / `--quiet` | more detail · facts only |

`ftc --help` is always the current and complete reference — this page is the map, not the authority.

## Transfer modes

**Rule of thumb: `fast` when the bus is quiet, `safe` when it is not, or when the target is an SD
card.** How the two differ on the wire and the measurements behind the rule:
[THROUGHPUT.md](THROUGHPUT.md).

## Install

`ftc --install` copies the binary into place and `ftc --uninstall` removes it — no package manager,
no external dependencies. On macOS and Linux the canonical location is `/usr/local/bin`.

## Build

One `pio run` cross-builds the whole matrix; PlatformIO pulls a project-local `zig` for the cross
targets by itself.

```bash
pio run                         all targets
pio run -e ftc-cli-linux-x64    one target
```

## Further reading

The shim contract and the byte-exact wire protocol live next to the client:
[`../ftc-cli/README.md`](../ftc-cli/README.md) and `doc/_alt/FTC-HOST-SHIM-CONTRACT.md`,
`doc/_alt/FTC-WIRE-PROTOCOL.md`.

# `oknx` — the desktop client

**For:** anyone operating a device from a PC. One binary, no dependencies; it talks to any device on
the bus through a single KNXnet/IP tunnel to a Router or Interface.

It is not a rewrite: `FileTransferClient.cpp` from `src/` is compiled unchanged against a host shim
([ARCHITECTURE.md](../reference/architecture.md)).

Targets: macOS arm64/x64 · Linux x64/arm64/armhf · Windows x86/x64/arm64.

## The two addresses

```bash
oknx --ip 11.11.0.126 5.0.3 info
         └─ the interface       └─ the target on the bus
            you tunnel through
```

`--ip` is never the target. Forgetting that is the most common mistake.

## Commands

Taken from `oknx --help` — that help and this table are generated from the same command set, so if they
ever disagree, the binary is right.

**Global switches** — these come *before* the command.

| | |
|---|---|
| `-i` / `--ip A.B.C.D` · `--port N` | the interface to tunnel through |
| `-D` / `--discover` | list the KNXnet/IP interfaces on the LAN and stop |
| `-W` / `--workers N` | how many identity reads run in parallel (default 5); the sweep itself stays on one tunnel |
| `--no-details` | sweep only — skip the identity read, which is on by default |
| `openknx` | read the identity of the System B candidates only |
| `-V` / `--verbose` | full interface and target profile first, and the control block during a transfer |
| `-q` / `--quiet` | no chrome, tab-separated — scriptable, and automatic when the output is not a terminal |
| `--log[=path]` | record the session; always complete, whatever the console shows |
| `--prio low\|normal\|urgent\|system` · `--prio-force` | KNX priority of the FTC frames; anything above `low` warns and asks |
| `--lang de\|en` · `--theme green\|amber\|cyan` · `--ascii` | language, accent colour, ASCII frames |
| `-VqD` | bundled, e.g. `-VD` = `-V -D` |

**Environment** — the only variable `oknx` reads itself.

| | |
|---|---|
| `OKNX_LANG` | pins the language of one shell, e.g. `OKNX_LANG=de` (anything starting with `de` is German, everything else English) |

The language is resolved in this order: `--lang` · the `lang` key of `oknx config` · `OKNX_LANG` · the
operating system's language (macOS/Windows) · `LC_ALL` / `LC_MESSAGES` / `LANG` · English. Before the
rename this variable was named FTC_LANG; that name is not read any more.

**Without a device address** — these talk to the interface itself.

| | |
|---|---|
| `info` | the full interface report (DESCRIPTION + device management) |
| `groupmon` / `gm` · `busmon` / `bm` | live group monitor (decoded) · live bus monitor (raw LPDU, ETS acknowledge colour) |
| `gm\|bm compare <ipB>` | A/B fidelity comparison of two monitors — [busmon-compare.md](../reference/busmon-compare.md) |
| `gm\|bm --frames N` / `--seconds N` | stop after N — scriptable |
| `progscan` / `ps [global\|locate]` · `ps --seconds 0` | find devices in programming mode; `--seconds 0` watches continuously |
| `con` / `console` | the interface's **own** console over its web console (WebSocket, no tunnel) |
| `scan <a.l \| a b> [ets] [deep N]` | find devices on a line or area |
| `scan … pace <ms>` / `drain <ms>` / `tmo <ms>` | spacing between questions · wait at the end for slow answers · how long one may stay unacknowledged |
| `scan … openknx` / `details` | read identities while scanning: OpenKNX candidates only, or every device |
| `install` / `uninstall` · `config <key> <value>` | put `oknx` on the PATH · persistent defaults |

**KNX properties** — the interface objects of a device, independent of the file transfer. Which objects and
PIDs exist, and how to read the answers: [knx-properties.md](../reference/knx-properties.md).

| | |
|---|---|
| `prop read\|write <iot> <inst> <pid> [start] [hex]` | the interface's **own** objects, by object type + instance |
| `busprop read\|write <pa> <objIdx> <pid> [start] [hex]` | the objects of a **remote** device over the bus, by object index |
| `busprop dump <pa>` | walk every index and known PID and print what answers |
| `<pa> runstate [start\|stop]` | read the run state machine of the application program; `stop` halts group communication |

**Reading a device** — never gated by the access control.

| | |
|---|---|
| `<pa> ping` | is the target there? round trip in ms |
| `<pa> feat` / `f` | what the target can do — and why a write would be refused |
| `<pa> exists` / `e <path>` | does this file or directory exist? |
| `<pa> info [ga\|<file>]` | device profile · group communication · file info |
| `<pa> df [sd\|efc]` | filesystem usage |
| `<pa> ll` / `ls [sd/\|efc/][dir]` | listing with CRC and a storage bar |

**Writing to a device** — the client resolves the access state first and asks for the password if the
target wants one ([unlocking-a-device.md](unlocking-a-device.md)).

| | |
|---|---|
| `<pa> send <src> [sd/\|efc/]<dst>` | upload a host file (alias `upload`) |
| `<pa> get <remote> [local]` | download (alias `download` / `receive`) |
| `<pa> rm` · `mkdir` · `rmdir` · `mv` | delete · create · remove · rename |
| `<pa> format yes` | erase the **whole** filesystem — guarded |
| `<pa> perf [kb] [pkg] [mode]` | throughput test without a file; `sd`/`efc` picks the drive |
| `<pa> login <password>` / `logout` | unlock and lock writing; the password never goes on the wire |
| `sd/` · `efc/` | prefix a **remote** path to leave LittleFS |

**Transfer options** — order does not matter, and three spellings mean the same thing.

| | |
|---|---|
| `--mode safe\|fast` | every block acknowledged · or a window |
| `--pkg <16..254>` / `auto` | APDU payload; `auto` takes the interface maximum |
| `--window <4..64>` | pin the fast window instead of letting it regulate — needs `--mode fast` |
| `--apply` / `--no-apply` | flash and reboot after a verified upload, or explicitly not |
| `--no-resume` · `--keep` | ignore a fragment on the target · keep the perf test file |
| `--progress` / `--quiet` | output level for this one call |
| `-f -a -k -n -q -v[0-2]` | the same, shorter, bundled as `-fa` |
| `fast` · `safe` · `auto` · `w16` · `apply` · `nr` · `keep` · `verbose` | the bare words the device console has always taken |

A value out of range, a window without `fast`, or an unknown word is **rejected** — never silently
ignored.

**Firmware over the bus**

| | |
|---|---|
| `knxota <file>` | the assistant: pick interface and device, compare versions, transfer, apply, verify |
| `knxota … --check` · `--force` | say what would happen · allow a downgrade or an unidentified file |
| `<pa> fwupdate <remote>` | trigger only: apply a file that is already on the target |
| `delta <old> <new>` · `gzip <file>` | build a difference image · pack one, without sending |
| `decode <file>` | read an image back: what it is, which device, which version |

An interrupted `knxota` run is offered again on the next start, recognised by the firmware's checksum:
[firmware-update.md](firmware-update.md).

## Transfer modes

**Rule of thumb: `fast` when the bus is quiet, `safe` when it is not, or when the target is an SD
card.** How the two differ on the wire and the measurements behind the rule:
[THROUGHPUT.md](throughput.md).

## What a transfer shows

Three levels of the same truth. The mode line is the **negotiated** mode, never the requested one -- a
target that cannot do `fast` says so there.

**Default.** The live line carries the window and its regulation state:

| | meaning |
|---|---|
| amber `⟦12›16⟧` | probing -- the ceiling is not known yet |
| red `⟦16›12⟧` | backing off, a window overran the receiver |
| green `⟦16⟧` | settled: the window found its size |
| blue `⟦32⟧` | pinned by `--window`, not regulated at all |

`12›16` and the blink appear only while a change is fresh; a settled window stays still, which is the
normal case for most of a transfer.

**`-V` adds the reasoning** -- a control block under the live line, and the same rows in the result:

```
Regelung  Fenster   █  16, seit 26 Fenstern unverändert
          Rahmen    pkg 254 (Ziel 254 · Interface 254) → 246 B/chunk · bewährt
          Tempo     Pacer 530 B/s · bindet nicht (die Strecke ist die Decke)
          Verlust   14 Chunks am Stück am Ende (Empfänger war voll)
          Linie     Rahmen 300 ms · Takt 519 ms → 58 % belegt · 219 ms Leerlauf
          Kosten    Report je Fenster 85 ms / 3936 B = 2 %
```

`Verlust` is the diagnosis, not the count: a **contiguous tail** means the receiver's buffer filled,
scattered gaps mean interference on the line. Opposite causes, opposite fixes.

**`-q` reports the run as data** -- one fact per line, `key<TAB>value`, no colour and no prose:

```
mode	fast
chunk_size	246
window	16
window_state	settled
resends	0
bps	470
ok	1
```

**`--log <file>` always records everything**, whatever the console was told to show. The console
filters, the log does not, and it ends with a plain-text report of the run. A log written under `-q` is
still complete months later.

## Install

`oknx install` copies the binary into place and `oknx uninstall` removes it — no package manager, no
external dependencies. The default target needs no root: `~/.local/bin` on macOS and Linux,
`%LOCALAPPDATA%\Programs\oknx` on Windows. `--system` targets `/usr/local/bin` instead and does need
root; `--dir <path>` puts it anywhere. The copy is written next to the target and renamed into place,
so a running binary is never half-overwritten.

## Build

One `pio run` cross-builds the whole matrix; PlatformIO pulls a project-local `zig` for the cross
targets by itself.

```bash
pio run                         all targets
pio run -e oknx-linux-x64       one target
```

## Further reading

The shim contract and the byte-exact wire protocol live next to the client:
[`../../oknx/README.md`](../../oknx/README.md), [HOST-SHIM.md](../reference/host-shim.md) and
[PROTOCOL-WIRE.md](../reference/protocol-wire.md).

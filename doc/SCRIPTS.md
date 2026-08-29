# Scripts

**For:** developers verifying a change. PowerShell, PS 5.1 compatible, pure ASCII, each with an
OpenKNX header and comment-based help (`Get-Help ./<script>.ps1 -Full`). They fall into three
groups.

## 1. Regression against real hardware

These drive an OpenKNX device's USB console and judge what the device actually did — no mocks.

| Script | What it proves |
|---|---|
| `Test-FtcSuite.ps1` | the console regression suite: every `ftc` sub-command, help, verbose streaming, path handling |
| `Test-FtcStress.ps1` | the full stress and regression pass — transfers under load, aborts, resumes, drive switching |
| `Test-FtcResume.ps1` | an interrupted transfer continues from the target's partial instead of restarting |
| `Test-FtcDelta.ps1` | a delta update against real hardware, judged by what the device booted afterwards |
| `Test-FtcConsoleUX.ps1` | the console over the bus: session handling, drain, truncation reporting |

Typical call:

```powershell
pwsh ./scripts/Test-FtcSuite.ps1 -Port /dev/tty.usbmodem1101 -Target 5.0.3
```

## 2. Protocol hardening

`scripts/Hardening/` exercises the FunctionProperty RPC surface itself — the wire, not the console.

```powershell
pwsh ./scripts/Hardening/Invoke-FtmHardening.ps1
```

It runs seven suites and writes into `Hardening/Reports/`:

| Suite | Covers |
|---|---|
| `1-Protocol` | frame layout, lengths, the command numbering |
| `2-ResponseMatrix` | every command against every server answer, per drive and async state ([PROTOCOL.md](PROTOCOL.md)) |
| `3-StateMachine` | the client states and every path out of them |
| `4-Security` | the access stages, login, lockout, the back-off ([SECURITY.md](SECURITY.md)) |
| `5-Console` | object 160: session, park, drain, overflow reporting ([CONSOLE.md](CONSOLE.md)) |
| `6-Limits` | the boundaries — 247-byte payload, path lengths, chunk counts |
| `7-NonBlocking` | nothing long runs in the KNX dispatch |

The important one is the **response matrix**. It exists to enforce one rule: **any change to a wire
response code or command triggers a full command × response × state audit** — not the edited file,
every consumer. What that rule cost to learn:
[PROTOCOL.md](PROTOCOL.md#when-you-change-a-response-code).

## 3. Without a device

| Script | What it does |
|---|---|
| `Invoke-DeltaSelfTest.ps1` | proves the delta encoder and the firmware interpreter against each other, on the host, no hardware |
| `New-DeltaTestPair.ps1` | builds a pair of images for that self test |
| `Compress-Firmware.ps1` | packs a firmware for the gzip path |

`Invoke-DeltaSelfTest.ps1` is the one to run after touching `FirmwarePatch.*` — it catches an encoder
and interpreter drifting apart in seconds, where hardware would take an hour ([DELTA.md](DELTA.md)).

## Preparing an image

Image preparation itself lives in OGM-Common, because it is not FTC-specific:

```powershell
pwsh Prepare-Firmware.ps1        # menu: full image, gzip or delta, with a file browser
```

See [FIRMWARE-UPDATE.md](FIRMWARE-UPDATE.md).

## Building ftc-cli

`ftc-cli/scripts/pio_zig_cross.py` is a PlatformIO pre-hook, not a user script: it pulls a
project-local `zig` and sets the cross-compiler per target, so a single `pio run` produces all eight
binaries ([FTC-CLI.md](FTC-CLI.md)). It is Python because PlatformIO requires `extra_scripts` to be
Python.

## Convention

A script that ships in this repo is PowerShell. Python is only used where a toolchain demands it. If
you add one: OpenKNX logo header, author, comment-based help, PS 5.1 compatible, pure ASCII.

# FTC — files, console and firmware over the KNX bus

A server in the device, a client next to it — both live in this module. This page is the index; every
document states its own audience in its first line.

**New here?** [guide/QUICKSTART.md](guide/quickstart.md) — five minutes, three front ends, one first
firmware update.

## How this folder is organised

```
doc/
  guide/       operating it      — you have a device in front of you
  reference/   building with it  — you are integrating, extending or debugging the module
  concept/     why it is so      — decisions, rationale, open analyses
```

A document in `guide/` answers *how do I*, one in `reference/` answers *what exactly*, one in
`concept/` answers *why this way and not another*. When the same subject appears in two of them, the
guide is the shorter one.

## guide — operating it

| Question | Document |
|---|---|
| How do I get going at all? | [quickstart.md](guide/quickstart.md) |
| How do I flash a device over the bus? | [firmware-update.md](guide/firmware-update.md) |
| How does the browser page work? | [knxota-web.md](guide/knxota-web.md) |
| What can the desktop client do? | [oknx.md](guide/oknx.md) |
| How do I reach a device's console over the bus? | [console.md](guide/console.md) |
| Why is writing locked, and how do I log in? | [unlocking-a-device.md](guide/unlocking-a-device.md) |
| How long will my transfer take, `safe` or `fast`? | [throughput.md](guide/throughput.md) |
| What does this result code mean? | [error-codes.md](guide/error-codes.md) |

## reference — building with it

| Question | Document |
|---|---|
| How is the whole thing put together? | [architecture.md](reference/architecture.md) |
| How do I add this module to a product? | [integration.md](reference/integration.md) |
| Which switches exist, what do they cost? | [flags.md](reference/flags.md) |
| Which commands exist, what do they answer? | [protocol.md](reference/protocol.md) |
| What exactly goes on the wire, byte by byte? | [protocol-wire.md](reference/protocol-wire.md) |
| How does the PC client attach to the device code? | [host-shim.md](reference/host-shim.md) |
| How does a firmware update travel as a difference? | [delta.md](reference/delta.md) |
| Why is it not faster, and what would make it faster? | [bottleneck.md](reference/bottleneck.md) |
| What is genuinely open, what is settled? | [limits.md](reference/limits.md) |
| How do I compare two bus monitors? | [busmon-compare.md](reference/busmon-compare.md) |
| Which KNX objects and PIDs can I read from a device? | [knx-properties.md](reference/knx-properties.md) |
| Which scripts exist, what do they prove? | [scripts.md](reference/scripts.md) |

## concept — why it is so

| Question | Document |
|---|---|
| Why is the access control built this way? | [access-control.md](concept/access-control.md) |
| Why are the build switches cut the way they are? | [build-defines.md](concept/build-defines.md) |
| Where are the desktop front-ends going? | [desktop-api.md](concept/desktop-api.md) |

## The four sentences that explain everything

1. **Every operation is a KNX function-property call, not a stream.** One frame out, one frame back, at
   most 247 payload bytes. There is no connection that stays open.
2. **The bus carries 350–650 bytes per second.** That is not a setting, that is the wire plus the
   interface's host link. A 1.8 MB firmware takes about an hour.
3. **The sending interface sets the pace**, not the client and not the target. The same device answers
   470 B/s through one interface and 599 through another.
4. **Nothing blocks.** Neither in the device (`loop()` stays free) nor in the client. A checksum over a
   large file is spread across many passes, not done in one.

## Where the code is

```
src/FileTransferModule.*        server in the device — files, directories, firmware, console
src/FileTransferClient*.*       client              — on the device (console) AND on the PC
src/FileTransferWebClient.*     the knxOTA web page: routes, status JSON, device-to-device update
src/FirmwarePatch.*             delta interpreter (both sides, the same source)
src/KnxDeviceMap.h              mask -> device family, and the fixed identity map of the BCU families
oknx/                           native desktop client (macOS · Linux · Windows · Raspberry Pi)
```

`src/FileTransferClient*` is compiled **unchanged** on the PC — the desktop client is not a rewrite but
the same code on a different base (`oknx/shim/`). See [architecture.md](reference/architecture.md)
and, for the exact contract, [host-shim.md](reference/host-shim.md).

The desktop client also carries its own build-and-install readme:
[`../oknx/README.md`](../oknx/README.md).

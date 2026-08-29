# FTC — File Transfer over cEMI/KNX

Files and firmware over the KNX bus, without a second wire. A server in the device, a client next to
it — both live in this module. This page is the index; every document below states its own audience
in its first line.

**New here?** [QUICKSTART.md](QUICKSTART.md) — five minutes, three front-ends, one first firmware
update.

## Using it

For anyone operating a device: transferring a file, flashing firmware over the bus, unlocking a
protected target.

| Question | Document |
|---|---|
| How do I get going at all? | [QUICKSTART.md](QUICKSTART.md) |
| How do I flash a device over the bus? | [FIRMWARE-UPDATE.md](FIRMWARE-UPDATE.md) |
| How does the browser page work? | [WEB.md](WEB.md) |
| What can the desktop client do? | [FTC-CLI.md](FTC-CLI.md) |
| How do I reach a device's console over the bus? | [CONSOLE.md](CONSOLE.md) |
| Why is writing locked, and how do I log in? | [SECURITY.md](SECURITY.md) |
| How long will my transfer take, `safe` or `fast`? | [THROUGHPUT.md](THROUGHPUT.md) |
| What does this error code mean? | [errorcodes.txt](errorcodes.txt) |

## Building with it

For anyone integrating the module into a product, changing the wire protocol, or debugging it.

| Question | Document |
|---|---|
| How do I add this module to a product? | [INTEGRATION.md](INTEGRATION.md) |
| Which switches exist, what do they cost? | [FLAGS.md](FLAGS.md) |
| Why are the switches built that way? | [CONCEPT-defines.md](CONCEPT-defines.md) |
| How is the whole thing put together? | [ARCHITECTURE.md](ARCHITECTURE.md) |
| Which commands exist, what do they answer? | [PROTOCOL.md](PROTOCOL.md) |
| How does a firmware update travel as a difference? | [DELTA.md](DELTA.md) |
| Which scripts exist, what do they prove? | [SCRIPTS.md](SCRIPTS.md) |

The native desktop client carries its own README: [`../ftc-cli/README.md`](../ftc-cli/README.md).

## The four sentences that explain everything

1. **Every operation is a KNX function-property call, not a stream.** One frame out, one frame back,
   at most 247 payload bytes. There is no connection that stays open.
2. **The bus carries 350–650 bytes per second.** That is not a setting, that is the wire. A 1.8 MB
   firmware takes close to an hour.
3. **The device sets the pace**, not the client. Sending faster does not mean arriving faster — it
   means a reboot.
4. **Nothing blocks.** Neither in the device (`loop()` stays free) nor in the client. A checksum over
   a large file is spread across many passes, not done in one.

## Where the code is

```
src/FileTransferModule.*        server in the device — files, directories, firmware, console
src/FileTransferClient*.*       client              — on the device (console) AND on the PC
src/FirmwarePatch.*             delta interpreter (both sides, the same source)
ftc-cli/                        native desktop client (macOS · Linux · Windows · Raspi)
```

`src/FileTransferClient*` is compiled **unchanged** on the PC — the desktop client is not a rewrite
but the same code on a different base (`ftc-cli/shim/`). See [ARCHITECTURE.md](ARCHITECTURE.md).

## `_alt/`

The long-form predecessors of these documents. Nothing there is deleted, and where a statement here
is short, the full version is there: `FTC-Reference.md` (the complete command reference),
`FTC-WIRE-PROTOCOL.md` (every byte on the wire), `FTC-HOST-SHIM-CONTRACT.md` (how the PC client
attaches to the device code), `BUSMON-COMPARE.md`, `FTC-SECURITY-rationale.md`.

# FTC — File Transfer over cEMI/KNX

Files and firmware over the KNX bus, without a second wire. A server in the device, a client next to
it — both live in this module.

| If you want to know … | read |
|---|---|
| how the whole thing is put together | [ARCHITECTURE.md](ARCHITECTURE.md) |
| which commands exist and what they answer | [PROTOCOL.md](PROTOCOL.md) |
| how the console works over the bus | [CONSOLE.md](CONSOLE.md) |
| how a firmware update travels as a difference | [DELTA.md](DELTA.md) |
| how the access protection works | [SECURITY.md](SECURITY.md) |
| why it is ~400 bytes per second and not more | [THROUGHPUT.md](THROUGHPUT.md) |
| which switches exist and what they cost | [FLAGS.md](FLAGS.md) |
| what an error code means | [errorcodes.txt](errorcodes.txt) |

The native desktop client has its own guide: [`../ftc-cli/README.md`](../ftc-cli/README.md).

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
but the same code on a different base (`ftc-cli/shim/`).

## `_alt/`

The long-form predecessors of these documents. Nothing there is deleted, and where a statement here
is short, the full version is there: `FTC-Reference.md` (the complete command reference),
`FTC-WIRE-PROTOCOL.md` (every byte on the wire), `FTC-HOST-SHIM-CONTRACT.md` (how the PC client
attaches to the device code), `BUSMON-COMPARE.md`, `FTC-SECURITY-rationale.md`.

# Result codes

**For:** anyone who read a code off a client and wants to know what it means and what to do next.
Every entry below was taken from the server source, not from an older list — codes that nothing emits
any more are named at the end so nobody looks for them again.

## How to read a code

Every answer to a command on object 159 begins with one status byte.

```
  0x00        the command did what it was asked            always
  0x01, 0x02  a status this COMMAND defines                per command, see below
  0x4x        the file operation failed                    global
  0x8x        the directory operation failed               global
  0xAx        access control refused                       global
```

**The low bytes are not global.** `0x02` means "format failed" after `Format` and "size known, checksum
still computing, ask again" after `FileInfo`. Reading them without knowing the command is how the
earlier version of this list came to contradict itself. From `0x40` up the meanings are the same
everywhere.

## Files — `0x4x`

| Code | Meaning | What it usually is |
|---|---|---|
| `0x41` | file already open | a previous transfer was never closed; send `Cancel` (90) and start again |
| `0x42` | file cannot be opened | the path does not exist, or the open flag is neither `0` nor `1` |
| `0x43` | file not open | a chunk arrived before the open, or after the target closed it — resume from the top |
| `0x44` | file cannot be deleted | wrong path, or the file is the one currently open |
| `0x45` | file cannot be renamed | target name exists, or the source is open |
| `0x46` | seek failed | the target could not reach the offset the sequence number implies |
| `0x47` | short write | **the filesystem is full** — the write returned fewer bytes than it was given |
| `0x4A` | too many blocks for `fast` | the file needs more than 8192 blocks; the client falls back to classic by itself |
| `0x4C` | busy | a firmware update is being applied right now; nothing else is accepted until it finishes |

`0x42` also answers the delta probe when the base image on the device does not match the one the patch
was built against — same code, different question. See [DELTA.md](../reference/delta.md).

## Directories — `0x8x`

| Code | Meaning |
|---|---|
| `0x81` | directory already open |
| `0x83` | directory not open |
| `0x84` | directory cannot be deleted (not empty, or wrong path) |
| `0x85` | directory cannot be created |

## Access control — `0xAx`

Only with `OPENKNX_FTC_SECURITY`. Reads are never gated; these answer writes and the console open.

| Code | Meaning | What to do |
|---|---|---|
| `0xA0` | authentication required | run the challenge-response: `ftc <pa> login <password>`, then repeat |
| `0xA1` | authentication failed | wrong password, an expired or missing challenge, or an empty password — an empty one always fails closed |
| `0xA2` | writes disabled | stage *Off*, or stage *ProgMode* while the device is not in programming mode |

The desktop client resolves this **before** it sends a write, so in practice you see its prompt rather
than the code. [SECURITY.md](unlocking-a-device.md).

## Per-command status bytes

These are answers, not failures. The same byte means different things after different commands.

| Command | Byte | Meaning |
|---|---|---|
| `FileInfo` (43) | `0x00` | size and CRC32 both ready |
| | `0x01` | size valid, no CRC — the default on SD and external flash |
| | `0x02` | size known, CRC still being computed; ask again (it is computed across `loop()` passes, never in one) |
| | `0x42` | no such file |
| `FilesystemInfo` (46) | `0x00` | the two numbers are in bytes |
| | `0x01` | the two numbers are in **KB** — a unit flag, not an error |
| `Format` (0) | `0x00` | formatted |
| | `0x02` | `LittleFS.format()` refused |
| `Exists` (1) | `0x00` + `0x00/0x01` | always succeeds; the **second** byte carries the answer |
| `DeltaProbe` (106) | `0x00` | the base image matches — the patch can be applied |
| | `0x02` | still reading the base image; ask again |
| | `0x03` + bytes | a rebuild is running; the four bytes are how much it has produced |
| | `0x05` + err | the last apply failed; the byte is the `FirmwarePatch::Error` |
| | `0x42` | the base image is a different one |
| | `0x4B` | the request length is not what the probe expects |

## Codes that no longer exist

Earlier lists carried these. Nothing in the server emits them; if a client ever shows one, it did not
come from this module.

`0x03` (LittleFS not initialised) · `0x04` (package larger than the answer buffer) · `0x82` (directory
cannot be opened) · `0x86` (no more files).

## Codes from other layers

A failing transfer sometimes reports something that never came from the file-transfer server at all:

- **`E_NO_MORE_CONNECTIONS` (`0x24`)** in a `CONNECT_RESPONSE` — the interface has no tunnel slot free.
  A client killed without a clean disconnect leaves its slot occupied until the interface reaps it, so
  this often means *your own* earlier run, not another user. Measurements taken while such a session is
  still open are unreliable.
- **Client abort reasons** are text, not codes — `fast: stall timeout (30s)`, `report query unanswered`,
  `source read error`. Which of them a retry can clear: [LIMITS.md](../reference/limits.md) and the retry offer
  described in [FIRMWARE-UPDATE.md](firmware-update.md).

Where each code is produced: `src/FileTransferModule.cpp`; how the client names them:
`FileTransferClient::ftcResultName()`.

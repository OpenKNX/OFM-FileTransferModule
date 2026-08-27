# Protocol

A command is an `A_FunctionProperty_Command` (APCI `0x2C7`) on object **159**, the response an
`A_FunctionPropertyState_Response` (APCI `0x2C9`). The command number is in the PID.
Payload ≤ **247 bytes** per frame.

## Commands

| PID | Name | Payload | Response |
|---|---|---|---|
| 0 | `Format` | — | Code |
| 1 | `Exists` | `path\0` | Code |
| 2 | `Rename` | `old\0new\0` | Code |
| 40 | `FileUpload` | `[seq:2][len:1][data]` | Code per block |
| 41 | `FileDownload` | open / request block | Code + data |
| 42 | `FileDelete` | `path\0` | Code |
| 43 | `FileInfo` | `path\0[flags]` | Code + size (+ CRC) |
| 44 | `FileUploadFast` | open · **data silently** · close | only open/close respond |
| 45 | `FileReport` | — | bitmask of the received blocks |
| 46 | `FilesystemInfo` | `[drive]` | Code + total + used |
| 80 | `DirList` | `path\0` | Code + type + name, one entry per call |
| 81 | `DirCreate` | `path\0` | Code |
| 82 | `DirDelete` | `path\0` | Code |
| 90 | `Cancel` | — | Code |
| 100 | `ModuleVersion` | — | Version |
| 101 | `FwUpdate` | `path\0` | **nothing** · exception: `0xA0`/`0xA2` when the write lock rejects it. What became of it is told by 106 ([DELTA.md](DELTA.md)) |
| 102 | `CheckFeatures` | — | feature byte |
| 103 | `AuthChallenge` | — | Nonce |
| 104 | `AuthResponse` | MAC | Code |
| 105 | `AuthLogout` | — | Code |
| 106 | `FwProbe` | `[len:4][crc:4]` | Code + extra · also the **job status** ([DELTA.md](DELTA.md)) |

## Response codes

| Code | Meaning |
|---|---|
| `0x00` | done · for `FileInfo`: size **and** checksum |
| `0x01` | `FileInfo`: size only, no checksum (SD/ExtFlash in the normal case) |
| `0x02` | `FileInfo`: **size is there, checksum still computing — ask again** |
| `0x03` | busy |
| `0x05` | last operation failed, reason in the extra |
| `0x41` … `0x47` | file error (open, cannot be opened, cannot be deleted, cannot be renamed …) |
| `0x81` … `0x86` | directory error |
| `0x42` | not found — the most frequent one, and the only one that means "does not exist" |
| `0x4A` | too many blocks for the fast transfer → fall back to the classic one |
| `0x4B` | range outside what is allowed |
| `0xA0` `0xA1` `0xA2` | login required · login failed · writing locked ([SECURITY.md](SECURITY.md)) |

The complete list is in [errorcodes.txt](errorcodes.txt).

## The one rule that costs the most when it is missed

**`FileInfo` returns the size at once — the checksum not.**

```
   Client                                 Device
     │   FileInfo /fw.bin                    │
     │ ─────────────────────────────────────▶│  open file, read size,
     │                                       │  queue checksum job
     │◀───────────────────────────────────── │  0x02  +  size             ◀── already here!
     │                                       │
     │            … loop() … loop() …        │  one CRC slice per pass
     │   FileInfo /fw.bin  (again)           │
     │ ─────────────────────────────────────▶│
     │◀───────────────────────────────────── │  0x02  +  size    (not done yet)
     │   FileInfo /fw.bin  (again)           │
     │ ─────────────────────────────────────▶│
     │◀───────────────────────────────────── │  0x00  +  size   +  CRC
```

Measured: a size costs **~0.35 s**, a checksum over a 500 KB file **~1.4 s**.

**Every client state that sends `FileInfo` must ask again on `0x02`** — with a deadline that is
**not** restarted. One forgotten `0x02` has already broken five commands at once: `ll` showed 0 bytes,
`info` reported "not found", `apply` ran into a race.

Whoever only wants to know **whether** a file is there treats `0x00`, `0x01` and `0x02` alike as
"there" and rejects only `0x42`.

## Two ways to send

```
  classic (40)                          fast (44/45)
  ────────────                          ────────────
  Block ──▶                             Block ──▶ ┐
      ◀── confirmation                  Block ──▶ │  one whole window,
  Block ──▶                             Block ──▶ │  without single responses
      ◀── confirmation                  Block ──▶ ┘
  Block ──▶                             FileReport ──▶
      ◀── confirmation                       ◀── bitmask: what is missing
                                        only the gaps again

  paces itself                          faster, but sensitive
  survives a full bus                   to a full bus
```

The receive bit is **set only once the block's checksum matches and the write has succeeded** — not
when the block has arrived.

Which way is the right one when: [THROUGHPUT.md](THROUGHPUT.md).

## One response belongs to one command

The client is **depth-1**: it always has at most one command in flight. `ftcSend()` records which
one that was, and every waiting state checks the response against it:

```cpp
_ftcSentProp = propertyId;                 // in ftcSend(), once
...
if (_ftcRespProp != _ftcSentProp) return;  // in the waiting state
```

**Why this is needed:** responses occasionally arrive twice, and a command can respond although
nobody is waiting for it — `FwUpdate` for instance responds only when the write lock rejects it.
Five states used to take whatever came in; a late response could pass there as their own.

**An exception that is none:** `FtcApduProbe` deliberately clears a parked `_ftcRespPending` instead
of consuming it. **No** check belongs there — a `return` would skip the state change.

## When you change a response code

**Then a command × response × state check is mandatory** — every client state that waits for this
command, not only the one you are working on. The `0x02` code was introduced on the server side and
**one** client path was adapted; five siblings stayed behind and broke. That is the most expensive
bug in the history of this module.

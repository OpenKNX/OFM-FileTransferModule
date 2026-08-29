# Firmware as a difference

**For:** developers working on the update path or the patch format. Operating side — how to build and
send one: [FIRMWARE-UPDATE.md](FIRMWARE-UPDATE.md).

A new firmware usually differs from the running one by a few percent. Instead of sending 1.8 MB over
the bus (nearly an hour) only the differences go — often under 50 KB, so minutes.

Build flag: `OPENKNX_FTC_DELTA_UPDATE` — cost and the board question in [FLAGS.md](FLAGS.md).

## The sequence

```
   Host                                                Device
  ──────                                              ────────

   new firmware   ┐
                  ├─▶ compute difference   ──▶  fw.delta.bin
   old firmware   ┘         (OKD1)                     │
        ▲                                              │
        │  from where?                                 │
        │  1. history: what this host                  │
        │     installed last                           │
        │  2. sibling folder of the release            │
        │  3. the user picks                           │
                                                       │
   ── FwProbe (106) ──────────────────────────────────▶│  "is exactly this image
      [len:4][CRC:4]                                   │   running on you?"
                                                       │
                                                       │  checksum over the
                                                       │  running image, cooperative
   ◀────────────────────────────────────────────────── │  0x00  yes, and this much space
                                                       │  0x42  no, a different one
                                                       │  0x02  still computing
                                                       │
   ── send ───────────────────────────────────────────▶│  store fw.delta.bin
   ── FwUpdate (101) ─────────────────────────────────▶│
                                                       │  FirmwarePatch: running image
                                                       │  + difference  →  new image
                                                       │  then restart
```

**Nobody selects delta.** You mark the new firmware; whether a difference is possible is decided by
the client, by asking the device for the checksum of its running image. Whoever is to choose "delta
or full" would have to know the base — which they do not know.

**The version number decides nothing.** Two builds of the same version differ; only the checksum over
the real image is a proof. Hence `FwProbe` and not a version comparison.

## What comes back after triggering

**`cmdFwUpdate` (101) does not respond.** On no path — neither on success nor on an error does the
handler set a response length. That is intentional (the device restarts right away and would not keep
up with the response), but it means: **from the command alone the client learns nothing.**

The one answer a client can still see to a `FwUpdate` comes from *before* the handler: the access
gate rejects it with `0xA0` / `0xA2` when writing is locked ([SECURITY.md](SECURITY.md)). That is a
refusal, never a result.

```
   Client                                       Device
     │  101  FwUpdate  "/fw.bin.gz"                │
     │ ───────────────────────────────────────────▶│  logInfoP("Update initiated")
     │                                             │  patch detected  ->  deltaArm()
     │            (no response)                    │
     │                                             │  ── loop() ── unpack patch,
     │                                             │     assemble image: seconds
     │                                             │     up to half a minute
     │                                             │  picoOTA.commit()  ->  restart in ~2 s
```

**For a difference there is however a status channel: `FwProbe` (106).** The same command that checks
the base also answers the question "how is the job doing":

| Response | means | Extra |
|---|---|---|
| `0x03` | **still running** — the image is being assembled right now | bytes produced so far |
| `0x05` | **failed** — reported exactly once, silent afterwards | the `FirmwarePatch::Error` |
| otherwise | no job active; the response belongs to the base check | — |

**That is the only way to learn about a failed delta install.** Without this query a client only sees
that the device does not restart — and cannot say whether the trigger never arrived, whether the
checksum did not match or whether work is still going on.

**And it is the reason why "the device has not disappeared" alone is no proof:** between the trigger
and the restart the device stays **reachable** while it computes. Measured on an 815 KB image that
was 16 seconds. Whoever only waits for the disappearance and measures too tightly declares a running
install failed.

**And it holds for both.** Not only a patch reports here — **every** error path of `FwUpdate` writes
into the same two fields, whether a difference was in play or a full image:

| Range | Meaning | Examples |
|---|---|---|
| `0x00`…`0x7F` | `FirmwarePatch::Error` — the patch could not be turned into an image | wrong base, checksum, truncated stream |
| `0x80`… | the image did not even get that far | not found · no bootable image · **wrong chip** · no second OTA slot · flash cannot be armed · write failed · gzip damaged |

**One field, one question.** The client asks "what became of my update" and gets an answer — it does
not have to know whether it sent a patch or a whole image.

## The OKD1 format

```
  ┌─────────────────────── 36 byte header, little-endian ───────────────────────┐
  │ 'O''K''D''1' │ Version │ Flags │ src length │ src CRC │ dst length │ …      │
  └─────────────────────────────────────────────────────────────────────────────┘
  ┌─── opcode stream ──┐  ┌─── literal stream ──┐
```

Two opcodes, there are no more:

| Opcode | Arguments | means |
|---|---|---|
| `ADD` | `<varint n>` | n bytes from the literal stream |
| `COPY` | `<varint n> <zigzag varint offset>` | n bytes from the running image |

`FLAG_PACKED` means: both streams are present as **one** zlib stream. The header stays readable in
any case — exactly that lets the device confirm the expected base **before** anything is unpacked.

## What the interpreter rejects

Eighteen named errors (`FirmwarePatch::Error`), each of them telling on its own — never two causes
under one code:

`ERR_MAGIC` (no patch file) · `ERR_VERSION` (newer format) · `ERR_FLAGS` · `ERR_HEADER_CRC`
(header damaged, so every length below it is worthless) · `ERR_SIZE` · `ERR_SRC_RANGE` ·
**`ERR_SRC_CRC`** (the device does not run on the image that was computed against) · `ERR_VARINT` ·
`ERR_OPCODE` · `ERR_ZERO_LEN` (would never end) · `ERR_COPY_RANGE` · `ERR_LIT_RANGE` ·
`ERR_DST_RANGE` · `ERR_TRUNCATED` · `ERR_TRAILING` · `ERR_DST_CRC` · `ERR_READ` · `ERR_WRITE`

`ERR_SRC_CRC` is the important one: it prevents a difference from being applied to a wrong image —
which would yield an unusable device.

## Why it works on an RP2040 at all

The interpreter does **no I/O of its own**. The caller passes in three callbacks (running image,
patch file, target) and a work buffer; the work is done in slices bounded by that buffer. That way it
can be driven from `loop()` without ever blocking longer than a single flash operation.

The same source runs on the host — there the round trip against the generator and the malformed
inputs are proven before a device executes it.

Cost on the device, measured: **+13 776 B flash · +568 B bss · 39 200 B peak heap demand**
(32 768 dictionary + 4096 slice + 1024 input + 1312 struct).

## Who cannot do this

An RP2040 with **2 MB** flash cannot do knxOTA at all: firmware 1.04 MB plus the stored update
1.04 MB do not fit into 2 MB. That is not a setting, that is arithmetic.

## The storage name is `/fw.delta.bin`

Not `/fw.bin`. The usual upload name and the storage name were once the same, and
`picoOTA.addFile()` takes the path named by the client — a collision that would have deleted exactly
the firmware that was being applied at that moment.

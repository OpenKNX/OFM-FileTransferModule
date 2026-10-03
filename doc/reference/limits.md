# Known limits

**For:** anyone about to investigate something that is already understood, or about to promise
something the module cannot do. Genuinely open items only -- what is merely *slow* but explained lives
in [BOTTLENECK.md](bottleneck.md).

## Open

**The interface's host link is the remaining speed lever.** The MCU pushes a whole frame to the TP
transceiver before the bus transmission starts, so host time adds to bus time. The NCN5130 offers SPI
at 500 kbps instead of the strapped UART, which would nearly remove that serial latency and push toward
the TP bus limit itself. Substantial hardware plus firmware -- new strap, new wiring, a new driver in
`lib/TPUart` -- not a configuration change.

**RP2040 reboots under an artificial tunnel flood.** Unpaced frames with incrementing sequence numbers
reboot the device; a fixed sequence or any pacing does not. Not reachable by a normal transfer, and not
the same thing as the "crash cliff" earlier documents named -- that claim was withdrawn. Root cause open.

**The console log ring is shared and small.** The console tunnel drains the common 4096 B log ring; a
single burst larger than that between two drains overwrites the head, and the client prints
`[...output truncated...]` once and continues -- honestly, but the output is gone. A dedicated, larger
device console ring is the fix; raising `OPENKNX_WEBCONSOLE_BUFSIZE` is the stopgap. See
[CONSOLE.md](../guide/console.md).

**The classic write path has no server-side sequence awareness.** Correctness comes entirely from the
client's stop-and-wait plus the absolute seek in `writeChunk`. Only the fast path tracks integrity on
the server, through its received-bitmap. A malformed or out-of-order classic write is not caught by the
device.

**`info ga` fails against an IP-Interface target.** The connection-oriented read that works against a
plain device does not complete against an interface. The lead is the single shared transport connection
left half-open. No fix attempted -- it points into the shared `knx` stack.

## Settled -- do not re-investigate

**The BIM M112 group-object table is read at a FIXED base, by convention.** Masks 0700h-0705h answer
`obj 3, PID 7` (table reference) with `nr_of_elem 0` and report no load state for the object table, so
nothing on the device points at the descriptors. They are there all the same: ETS reads them at
**`0x4400`**, and the client does likewise (`FTC_SYS7_GROT_BASE`), armed only after PID 7 came back
empty. Verified against ETS on `1.1.30` (17 of 17 rows identical in flags, priority and size) and on
`1.1.13`, `1.1.161` and `1.1.12`. Layout, derived from an ETS busmonitor of `1.1.30`:
`[count:1][RAM pointer:2]`, then 4 octets per object -- `value pointer(2) · config · type`, i.e. the
classic 3-octet descriptor of 03_05_01 4.18.3 with the pointer widened to two octets.

The base is **not** device-supplied and the standard is **silent**: 03_05_01 4.18.3 defines only
3-octet descriptors with a 1-octet pointer, and 06 Profiles 4.6.1 assigns masks 0700h-0705h no
realisation type at all. That is why the code validates the table HEADER before it takes a single flag,
and nothing else. Three per-entry plausibility rules were tried against real devices and **all three
are false** -- they are listed so nobody reinstates them:

| Rule that looked safe | Refuted by |
|---|---|
| the first value pointer sits exactly behind the flag bytes | gap 0 on `1.1.30`, gap 1 on `1.1.161` |
| value pointers rise with the object index | `1.1.30` runs up to `0x0784`, then back to `0x0748` (grouped per channel) |
| a value pointer always lies above the flag area | an unused slot is `00 00 00 00`, i.e. below it (`1.1.13` declares 33 objects and fills 17) |

**`safe` vs `fast` on a flooded bus is a trade-off, not a defect.** `safe` waits after every block and
grinds through congestion; `fast` bursts a window and only learns from the report what was lost. On a
bus a third device is saturating, `safe` is the right tool. This is delivery pattern, not filesystem.

**The 2026-08 "fast regression" is closed.** It was five client defects, not an environment: the payload
degraded on every transfer retry and never recovered, the window halved on any loss and never climbed
back, the stall guard aborted transfers that were delivering, the host pacer probed past capacity and
snapped to a rate that counted report wait as link time, and the ahead-of-link depth never bound. Fast
now measures above safe (470 vs 441 B/s). A future suspicion of the same kind should start from the
control block that `-V` prints, which names the window state and whether the pacer binds.

**Client-side rate tuning is finished.** Raising the pacing floor from 450 to 700 B/s moved the result
from 470 to 471 B/s. The client is not the brake.

**Fast is capped at 8192 blocks** (`FTM_FAST_MAX_CHUNKS`, ~2 MB at 254 B packages -- firmware fits).
Above it the server answers `0x4A` and the client falls back to classic, which has no cap. Not a defect;
the client says so in its mode line.

**RP2350 targets are flashed by UF2 or `picotool`, never OTA** -- OTA bricks the RP2350 boot path. A
target-side constraint, orthogonal to this module, but it decides what may be staged for `FwUpdate`.

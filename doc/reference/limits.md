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
plain device does not complete against an interface. Analysis, candidate root causes and the
disambiguating experiments: [ANALYSIS-infoga-co.md](../findings/2026-09-infoga-co.md). No fix attempted -- it
points into the shared `knx` stack.

## Settled -- do not re-investigate

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

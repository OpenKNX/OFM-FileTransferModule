# Throughput

**For:** anyone operating a device who needs to budget a transfer or pick a mode, and anyone designing
against the module. Every figure here is measured, not calculated. Why the numbers are what they are:
[BOTTLENECK.md](../reference/bottleneck.md).

## The number

```
        350              441 · 470        585 · 599            650
  ───────┼─────────────────┼────┼───────────┼──┼────────────────┼──────▶  bytes/s
         │                 │    │           │  │                │
     slow interface     safe  fast       safe  fast        upper bound
     (19200 host link)  OpenKNX interface  MDT interface   (fast host link)
```

Measured 2026-08-19, same target device (5.0.3), same 100 KB file, back to back:

| through | `safe` | `fast` |
|---|--:|--:|
| OpenKNX interface, ESP @38400 | 441 B/s | **470 B/s** |
| MDT SCN-IP000.03 | 585 B/s | **599 B/s** |

**The limiter is the sending interface, not the target device and not the TP wire.** The same device
answers 470 B/s through one interface and 599 B/s through another; a busmonitor on a second interface
shows the bus idle 41 % of the time because the sender cannot feed it faster. The cause is the host
link between the MCU and the TP transceiver, which transmits store-and-forward — its time adds to the
bus time instead of hiding behind it. The full derivation is in [BOTTLENECK.md](../reference/bottleneck.md).

> **Earlier versions of this document named the target device's per-block work as the limit, and before
> that a "crash cliff" at 450-544 B/s.** Neither stands. What remains of the second: an RP2040 reboots
> under an *artificial* tunnel flood with unpaced, incrementing sequence numbers; with a fixed sequence
> or with pacing it does not. That is a separate, still open finding -- not a property of a normal
> transfer. See [LIMITS.md](../reference/limits.md).

What follows from this, and what therefore does **not** need trying again:

| Idea | outcome |
|---|---|
| two tunnels in parallel to the same device | worse -- one target, one processor; overrun at *any* combined rate |
| more than one block in flight | real interfaces wedge; the spec allows one |
| FAF (silent send with a cumulative acknowledgement) | dropped -- no gain, only crash risk |
| raising the client's pacing rate | no effect: 450 -> 700 B/s changed 470 into 471 |

**Every remaining "make it faster" lever is in the interface** -- a faster host link (SPI), or a
different interface -- not in the client.

## Two modes

```
  safe                                    fast
  ────                                    ────
  block ──▶ ◀── ok ──▶ ◀── ok             ████████ window ████████ ──▶
                                          ◀── report: what is missing
  paces itself at the device              only the gaps again
  retry per block                         window backs off on an overrun, recovers on clean windows
```

`fast` sends a window of blocks and asks once what arrived, so it pays one round trip per window
instead of one per block. At the measured optimum (window 16) that round trip is ~85 ms per 3936
bytes -- about 2 % -- which is the whole of its advantage over `safe`.

**On a bus a third device is flooding, `safe` is still the right tool.** That is a property of the
delivery pattern, not a defect: `safe` waits after every block and therefore adapts to whatever the bus
currently gives, while `fast` only learns from the report that part of a window was lost. On a quiet
bus `fast` is reliably the faster of the two.

The window regulates itself and shows what it is doing; `--window <N>` pins it. What the display means:
[oknx.md](oknx.md).

## Why the web interface feels faster

The web file manager does **not** upload over the bus. It runs over HTTP/TCP in 2 KB blocks, and the
device writes each block through to the end before it answers -- stricter lockstep than `safe`, only on
a wire that carries a thousand times more.

**That cannot be carried over to the bus.** The KNX tunnel carries ~246 bytes per frame; `safe` *is*
the counterpart of the web lockstep, at the pace of the wire.

The file manager itself belongs to **OFM-Network**, not to this module — it writes through the same
drives over HTTP. What lives here is the [knxOTA page](knxota-web.md), which sends a firmware **over the
bus** to another device and is therefore bound by everything above.

## What a transfer really takes

| | at 470 B/s |
|---|---|
| 43 KB configuration file | ~1.5 min |
| 500 KB packed firmware | ~18 min |
| 1.8 MB firmware, whole | **~64 min** |
| the same as a difference, 45 KB | **~1.6 min** |

That is why [DELTA.md](../reference/delta.md) exists. And why every transfer goes into a queue instead of blocking a
user interface for an hour.

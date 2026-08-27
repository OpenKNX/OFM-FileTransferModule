# Throughput

Every figure here is measured, not calculated. They are the reason for almost every design decision
in this module — knowing them explains the rest by itself.

## The number

```
        350                  430 · 440                       650
  ───────┼────────────────────┼───┼──────────────────────────┼──────▶  bytes/s
         │                    │   │                          │
     lower bound         measured, today             upper bound
     (slow                safe from the PC ·        (fast interface)
      interface)          fast over the console
```

Measured after the rollback of 2026-08-18, ESP/NCN at 38400: **~430 B/s `safe`** from the PC,
**~440 B/s `fast`** over the device console. The limit is the **per-block work in the target device** —
LittleFS write plus KNX stack. Not the client, not the tunnel, not the acknowledgement latency (~5 ms).

> **Earlier versions of this document named a "crash cliff" at 450–544 B/s.** That was an intermediate
> finding and no longer stands. What remains: an RP2040 reboots under an **artificial tunnel flood**
> with unpaced, incrementing sequence numbers; with a fixed sequence or with pacing it does not. That
> is a separate, still open finding — **not a property of a normal transfer**.

What follows from this, and what therefore does **not** need trying again:

| Idea | outcome |
|---|---|
| two tunnels in parallel to the same device | worse — one target, one processor; overrun at *any* combined rate |
| more than one block in flight | real interfaces wedge; the spec allows one |
| FAF (silent send with a cumulative acknowledgement) | dropped — no gain, only crash risk |

**Every "make it faster" idea has to move work into the device** (batched flash writes, less work per
block) — not into the client.

## Two modes

```
  safe                                    fast
  ────                                    ────
  block ──▶ ◀── ok ──▶ ◀── ok             ████████ window ████████ ──▶
                                          ◀── report: what is missing
  paces itself at the device              only the gaps again
  retry per block                         the window grows and halves (AIMD)

  on a busy bus: gets through             on a busy bus: fragile
```

**`safe` survives a loaded bus, `fast` does not reliably.** The reason is the delivery pattern, not the
filesystem: `safe` waits after every block and therefore adapts to whatever bus and device currently
give. `fast` sends a whole window and only learns from the report that half of it was lost — on a bus
flooded by a third device that turns into a retry loop.

Rule of thumb: **`fast` when the bus is quiet, `safe` when it is not, or when the target is an SD
card.** To pin the window: `fast w<N>`.

## Why the web interface feels faster

The web file manager does **not** upload over the bus. It runs over HTTP/TCP in 2 KB blocks, and the
device writes each block through to the end before it answers — stricter lockstep than `safe`, only on
a wire that carries a thousand times more.

**That cannot be carried over to the bus.** The KNX tunnel carries ~246 bytes per frame; `safe` *is*
the counterpart of the web lockstep, at the pace of the wire.

## What a transfer really takes

| | at 400 B/s |
|---|---|
| 43 KB configuration file | ~2 min |
| 500 KB packed firmware | ~21 min |
| 1.8 MB firmware, whole | **~78 min** |
| the same as a difference, 45 KB | **~2 min** |

That is why [DELTA.md](DELTA.md) exists. And why every transfer goes into a queue instead of blocking
a user interface for an hour.

## Where the measurements come from

`ll` 1.42 s · `info` 0.345 s · throughput 350–650 B/s depending on the interface · after the rollback
of 2026-08-18: ~430 B/s `safe` from the PC, ~440 B/s `fast` over the device console, ESP/NCN at 38400.

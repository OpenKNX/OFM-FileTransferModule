# Where the time goes

**For:** anyone asking why a 9600-baud bus delivers 350-650 B/s and not more, and anyone tempted to
tune the client to change it. Operators want [THROUGHPUT.md](../guide/throughput.md); this is the engineering
answer underneath it.

The short version: **the sending interface's host link is the limiter, not the TP wire and not the
target device.** Everything below derives that, and the last section confirms it with an independent
measurement made three months after the model was written.


## TP1 is fixed 9600 baud — by the standard, not a setting

KNX TP1 is **fixed at 9600 baud** by the KNX standard (EN 50090 / ISO 14543-3). It is not a chip
register you can raise. After 11-bit characters, the layer-2 `L_ACK`, inter-frame gaps, and CSMA/CA
arbitration, the usable payload rate on a healthy line is roughly **350–870 B/s** depending on frame
size. That is the hard ceiling for *any* device on the wire.

## The real limiter is the host-UART, not the TP wire

FTC measures **349–368 B/s @19200**, well under even the low end of that TP range. The limiter is the
**NCN5130 host link** — the UART between the MCU and the NCN transceiver, strapped to **19200 baud** —
for two compounding reasons:

**(a) Store-and-forward.** The NCN transmits a frame onto TP *only after the whole frame has arrived
over the host UART* (datasheet p.49). So the host-transfer time adds **in series** with the bus time —
it does not hide behind it.

**(b) Two host bytes per KNX octet.** For every octet the host sends the NCN a `U_L_Data*` command/
position byte **and** the data byte (`Transmitter.cpp`). A 253-octet frame is therefore
~**509 host bytes** (after the sticky-offset optimization, §7.4) ≈ **292 ms @19200** — roughly *equal
to the TP time of the same frame*. Add the two in series and **per-frame time ≈ doubles**. That doubling
is exactly the `2.45 ms/octet` in the model (see the frame-time model below).

```
                 host UART (19200)            TP1 bus (9600)
   frame ready ─► [==== ~292 ms ====]──────► [==== ~292 ms ====] ─► L_ACK + gap (~66 ms)
                  (store-and-forward: the two run IN SERIES, not overlapped)
                  └───────────────── ~666 ms/frame @ pkg 253 ──────────────┘
```

**Why "~80 ms just to push a frame @19200" (and ~40 ms @38400).** The store-and-forward push is the part
of the frame time firmware people feel first — it is the delay *before the frame even starts on the bus*.
On-device timing instrumentation on a real interface, forwarding a normal-size telegram (a **57-octet**
frame, tunnel → TP), broke the per-frame time down like this (RP2040 @19200 host baud):

```
  push  ~77 ms  = octets MCU -> NCN over the host UART.  2 host bytes/octet (U_L_Data* cmd + data)
                  = ~114 bytes; at 19200, 8E1 = 11 bit-times/byte -> ~65 ms line + ~12 ms gaps.
  con   ~92 ms  = last-octet-pushed -> L_Data.con.  TP1 wire (~65 ms @9600) + the KNX ACK window + con byte.
  gap    ~2 ms  = re-arm to the next frame  (negligible -- the TX fast path works, this is NOT the limit).
  ----   ------
  per  ~170 ms  = push + con + gap  (matches the on-TP ~168 ms).  push and wire are ADDITIVE (store-and-forward).
```

**The `push` half halves at 38400** (~77 → ~38 ms), because it is pure host-UART time — so `per` drops to
~130 ms and the sustained forwarding rate climbs by ~30 % (measured, apples-to-apples: same chip, same
driver, a **19200-strapped** RP board forwarded **235 B/s**, a **38400-strapped** ESP board **303 B/s**, a
15-year-old Siemens interface **369 B/s** — the Siemens is not smarter, it just runs its TP chip at 38400).
So the concrete take-away: **~80 ms/frame push @19200, ~40 ms @38400, and it adds on top of the bus time.**
A full 245-B FTC data frame is bigger (~290 ms push @19200), but the ratio is identical — halve the host
baud time, keep the bus time. The `con` overhead above the wire (~27 ms) is the KNX ACK floor; no knob.

## Send ≠ Receive — the asymmetry that decides which baud matters

The two directions are **not** symmetric:

| | **Transmit (host → NCN → TP)** | **Receive (TP → NCN → host)** |
|---|---|---|
| Frame handling | **store-and-forward** (whole frame buffered first) | **streamed** (each byte forwarded as it arrives off TP) |
| Host bytes/octet | **2** (command + data) | ~**1** (data) |
| Host time vs bus time | **in series** — exposed | **in parallel** — hidden behind the slower bus |

On receive, the NCN forwards each byte the moment it arrives off the slow TP wire, ~1 byte per octet,
and 19200 is comfortably faster than the 9600 TP arrival rate — so the host time runs *underneath* the
bus time and never shows. **Only the sender's host baud matters.** A 19200 receiver is never the
bottleneck.

That is precisely why **a 38400-strapped sender talking to a 19200 receiver measured 478 B/s** (§6.1):
the send side halved its exposed host time; the receive side was never the limit.

## Host baud is a hardware strap — nothing to change in firmware

The NCN5130 host-UART baud is sampled **at reset from a pin strap** (datasheet p.27, pin **CSB/UC1**,
pin 26): `0 = 19200`, `1 = 38400`. **No runtime command or register changes it**, and 38400 is the
chip's max UART baud. The firmware already **auto-probes `{19200, 38400}`** at init and uses whatever
the board is strapped to (`DataLinkLayer.cpp`):

```c
uint baudrates[2] = {19200, 38400};
for (uint baudrate : baudrates) { ... if (tryInitialize(baudrate)) { setBCUState(BCU_CONNECTED, baudrate); ... } }
```

So there is **nothing to change in firmware** to go faster. Making a board faster = strap CSB/UC1 high
(a PCB rework). The measured +30 % from 19200 → 38400 is the whole available win from the host UART.

## The only way past the UART: SPI @ 500 kbps

The one higher-bandwidth host link the NCN5130 offers is **SPI at 500 kbps** — enabled by the MODE2
strap plus SCK/CSB/TREQ wiring and a new SPI host driver. It would nearly eliminate the store-and-forward
serial latency and push toward the **~800 B/s TP-bus limit** itself. It is a substantial hardware +
firmware project (new strap, new wiring, a new driver), not a config change — see LIMITS.md.

## Sticky-offset: the extractable software win (already shipped)

At a given baud, the one thing firmware *can* do is stop re-sending the `U_L_DataOffset` byte on every
octet. The NCN "stores [the offset] internally until a new offset is provided" (datasheet p.42), so it
only needs to be sent when the 6-bit position offset **changes** — 3 times per 253-octet frame instead
of ~189 (`Transmitter.cpp`; **always on** now — the resend-only-on-change is unconditional, no build flag):

```
253-octet frame, host bytes:  without sticky ≈ 698   →   with sticky ≈ 509   (saves ~189)
measured at pkg 253: +14 %   (at pkg 64 only ~7 bytes saved -> +3 %; it scales with frame length)
```

Because the whole host transfer is store-and-forward, those ~189 bytes are paid in full, in series with
the bus — so removing them is a real, measurable speed-up. **Below 2 host bytes per octet is impossible**
in the TPUART command protocol; sticky-offset is the floor at a given baud.

## Theoretical throughput limits (hardware)

First-principles math any reader can reproduce — it decomposes the frame-time model's blended `2.45 ms/octet` into its
host and bus parts, then extrapolates to the hardware ceilings.

```
   MCU ──[ host link: UART 19.2/38.4 kbps  or  SPI 500 kbps ]──► NCN5130
                                                                    │
                                    whole frame buffered, THEN ─────┘
                                                                    ▼
                                              KNX TP1 bus  (fixed 9600 baud)
```

**store-and-forward (datasheet p.49):** the bus transmission starts only after the *full* frame has
arrived over the host link, so host time and bus time are **sequential, not overlapped**.

**Derivation** (per 253-octet extended frame, 245 B payload):

- **Bus (immovable).** TP1 is fixed at 9600 baud. Each octet on the wire ≈ **1.35 ms** (~13 bit-times:
  start + 8 data + parity + stop + inter-octet spacing) → bus ≈ `253 × 1.35` ≈ **342 ms/frame**.
- **Host link.** The TPUART protocol sends **2 host bytes per KNX octet** (a command/position byte + the
  data byte) → ~**506 host bytes/frame**. UART is 8E1 = **11 bits/byte**; SPI is **8 bits/byte** (no
  parity/framing). `host_time = host_bytes × bits_per_byte / baud`.
- **Per-frame.** `frame = host_time + bus (342 ms) + overhead (~34 ms: L_ACK + inter-frame gap +
  L_Data.con)`. `throughput = 245 B / frame`.

**Validate against the two measured points** (the model reproduces both, which proves it):

- UART **19200**: host = `506×11/19200` ≈ 290 ms → frame ≈ `290+342+34` ≈ 666 ms → ≈ **368 B/s** (measured 368 ✓)
- UART **38400**: host ≈ 145 ms → frame ≈ 521 ms → ≈ **478 B/s** (measured 478 ✓)

**Extrapolate (theoretical):**

- **SPI @ 500 kbps**: host = `506×8/500000` ≈ 8 ms (~16 ms with per-byte TREQ handshaking) → frame ≈
  `16+342+34` ≈ 392 ms → ≈ **625 B/s** — the practical maximum on this chip.
- **Bus-only ceiling** (host → 0): frame → ~376 ms → ≈ **650 B/s** — the hard wall set by the 9600-baud
  TP1 bus; no host-side change can beat it.

| Interface | host/frame | + bus (fixed) | + ovh | = frame | ≈ B/s | note |
|---|---:|---:|---:|---:|---:|---|
| UART 19200 | ~290 ms | 342 ms | 34 ms | ~666 ms | **~368** | measured ✓ |
| UART 38400 | ~145 ms | 342 ms | 34 ms | ~521 ms | **~478** | strap → +30 %, measured ✓ |
| SPI 500 kbps | ~16 ms | 342 ms | 34 ms | ~392 ms | **~625** | practical max; needs a new interface + driver |
| bus-only ceiling | ~0 ms | 342 ms | 34 ms | ~376 ms | **~650** | hard TP1 wall — unbeatable host-side |

The firmware levers are already applied (both unconditional now) — sticky-offset (§7.6) and the TP TX
fast-forward path (`Transmitter.cpp` / `DataLinkLayer.cpp`). Everything beyond ~478 B/s is **hardware**:
the 38400 strap (sticky-offset, below) or the SPI link (§7.5, §12). **For real KB/s, use KNXnet/IP, not TP.**

---

## Confirmed independently (2026-08-19)

The model above was derived from on-device timing. It was later checked from the other side -- a
busmonitor on a *second* interface, capturing the bus while a 100 KB transfer ran through the first.
Neither measurement knew about the other.

| | frame airtime | measured cadence | bus in use | idle per frame |
|---|--:|--:|--:|--:|
| OpenKNX interface, ESP @38400, 246 B/chunk | 300 ms | 513 ms | 57 % | 213 ms |
| MDT SCN-IP000.03, 202 B/chunk | 250 ms | 337 ms | 74 % | 87 ms |

The prediction and the measurement meet: a 262-octet frame needs `262 x 2 x 11 / 38400` = **150 ms** of
host-UART push, the TP wire needs **300 ms**, and store-and-forward puts them in series -- **~450 ms**
plus the acknowledge floor. Observed: **513 ms**. The bus is idle 41 % of the time because the sender
cannot feed it, exactly as the store-and-forward model says.

Two consequences that settle recurring questions:

- **The target is not the limit.** The same target device answered 470 B/s through the OpenKNX
  interface and **599 B/s through the MDT** in the same session. A statement that the ceiling is the
  device's per-block work does not survive that comparison.
- **The client is not the limit either.** Raising the host client's pacing floor from 450 to 700 B/s
  changed the result by one byte per second (470 -> 471). Tuning client-side rate knobs is finished
  work; the remaining lever is the host link (SPI, above) or a faster interface.


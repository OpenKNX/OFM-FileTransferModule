# ftc-cli Monitor & Compare — reference and interpretation guide

Three related live tools in `ftc-cli`, all built on the same KNXnet/IP tunnelling monitor:

| Command | What it does |
|---|---|
| `ftc -i <ip> bm` / `busmon` | Bus monitor — raw TP1 LPDUs (incl. FCS) + the ETS ACK colour |
| `ftc -i <ip> gm` / `groupmon` | Group monitor — decoded L_Data group telegrams |
| `ftc -i <A> bm\|gm compare <B>` | **A/B fidelity diff** of two monitors on the **same** bus |
| `ftc -i <A> bm\|gm compare <B> --multi` | Two independent streams (interfaces on **different** lines; no diff) |

`compare` is a **busmonitor verification tool**: it evaluates our busmon's fidelity against another
vendor's busmon, vendor-agnostic. Two faithful busmonitors of the same bus must see byte-exact the same
telegrams; `compare` surfaces every genuine difference and **never "beautifies"** (never equates
genuinely-different telegrams, never hides a real difference, never fabricates a telegram).

---

## 1. Running a correct comparison

Three rules — get these wrong and the result is meaningless:

1. **Both interfaces on the SAME TP1 line.** That is the premise. Different lines → use `--multi`
   (no diff, just two streams side by side).
2. **Traffic must come from a THIRD device.** A busmonitored interface is **passive** (it never
   transmits), so it cannot generate the traffic it monitors. Use a third interface, an ETS action, or
   ambient bus traffic.
3. **Start `compare` BEFORE the traffic.** Otherwise the capture window opens mid-telegram and you get
   harmless `incomplete` edge fragments (see §5).

Example (rig: OpenKNX `.126` vs Siemens `.5`, traffic generated from the third interface `.210`):

```
# terminal 1 — start the comparison first (normalize is ON by default):
ftc -i 11.11.0.126 bm compare 11.11.0.5

# terminal 2 — then generate bus traffic from the THIRD interface:
ftc -i 11.11.0.210 5.0.3 perf 30 fast
```

Options: `--grace <ms>` (match window, default 750) · `--raw` (start with normalize OFF) ·
`--multi` (two-stream, no diff) · `--seconds N` / `--frames N` (stop after N, scriptable) · `-q` (TSV, no TUI).

Live keys: **`v`** layout (mix/side/stack) · **`d`** compare↔multi · **`n`** normalize on/off ·
**`f`** only-divergences · **`c`** collapse · **`m`** markers · **`t`** time-skew (see §7) ·
**`l`** save XML · **`p`** pause (display freezes, capture continues) · **`r`** reconnect both ·
**`?`** help · **`x`/`q`/Ctrl+C** quit.

---

## 2. The result

The one-word result of the whole run:

| EN (token, stable) | DE (display) | Meaning |
|---|---|---|
| `IDENTICAL` | Identisch | Both sides saw exactly the same — perfect fidelity in this window |
| `DIVERGENT` | Unterschiede | There is **at least one** difference (see the counters) |
| `MULTI` | Mehrfach | `--multi` mode — no diff was computed |

**`DIVERGENT` is strict/binary — a *single* edge fragment already makes it `DIVERGENT`.** So the word alone
does **not** mean "bad". Always read the **counters** to see *what* differs and whether it matters.

Exit codes (for scripts/CI): `0` identical · `2` divergent · `1` tunnel open failed · `130` Ctrl+C.

---

## 3. The counters — what every field means

The content axis (the raw TP1 LPDU, byte-exact incl. the control octet and FCS):

| Field | DE | Meaning | Good/bad |
|---|---|---|---|
| `seenA` / `seenB` | gesehen A/B | telegrams each side captured | context |
| `common` | gemeinsam | both saw the **identical** telegram (content **and** metadata agree) | ✅ want high |
| `onlyA` / `onlyB` | nur A / nur B | one side saw a telegram the other didn't | edge = ok · sustained = real |
| `countMismatch` | Anzahl-Abweichung | same telegram, different multiplicity (e.g. A saw it 2×, B 1×) | real if not an edge |

The metadata axes (compared **only on content-matched telegrams** — a content match with differing
metadata is **not** `common`, it is its specific class):

| Field | DE | Meaning | Why it matters |
|---|---|---|---|
| `integrityMismatch` | Integritäts-Abweichung | same bytes, but the sides disagree on the 0x03 status **F**rame/**B**it/**P**arity error flags | one receiver flagged corruption the other didn't = a real per-receiver integrity difference |
| `ackMismatch` | ACK-Abweichung | same telegram, different L2 acknowledge (A=ACK / B=NAK, etc.) | the two busmons disagree on the bus acknowledge |
| `notReported` | nicht gemeldet | a metadata dimension is **not comparable** because one side does not emit it | honesty: unknown is never counted as agreement |
| `lostA` / `lostB` | verloren A/B | a side's 0x03 status flagged **Lost** (it knows it dropped data) | a receiver admitting a loss |
| `incompleteA` / `incompleteB` | unvollständig A/B | pieces that could **not** be reassembled into a whole telegram, surfaced **tagged** | usually a capture-window edge (see §5) — never merged, never equated |

**Reading rule of thumb:**
- Only a few `onlyA/onlyB` and `incomplete*` at the window edges, everything else `common`, and
  `integrity/ack/notReported = 0` → **fidelity is fine**; the `DIVERGENT` is just the strict edges.
- Sustained `onlyA/onlyB`, or any `integrityMismatch` / `ackMismatch` / `notReported`, or `lost*` → a
  **real finding** worth investigating.

---

## 4. normalize (fragment reassembly) — ON by default in `compare`

Different vendors split a long telegram into different piece sizes based on their APDU (Siemens ~55 splits;
OpenKNX 254 delivers whole). That is **packaging**, not content. `normalize` reassembles the pieces back
into the whole telegram **before** the diff, so the comparison is of the **actual bus telegrams**, not the
vendor's fragmentation.

- **ON (default):** fragments collapse to `common`; you compare real telegrams.
- **OFF (`--raw` or key `n`):** the raw per-piece stream — Siemens' continuation pieces show as separate
  (partly garbage-decoded) frames. This is an **inspection** mode ("how does that vendor fragment?"),
  **never** the fidelity result.

Reassembly is spec-correct and conservative (see §6): it cuts only at the exact header length with a valid
FCS, and it only joins pieces while the 0x03 status **Lost flag is clear and the sequence number is
contiguous**. Anything it cannot safely complete is surfaced **tagged** (`incomplete`), never merged into a
fabricated telegram.

---

## 5. Interpreting `‹incomplete / Reassemblierung unvollständig›`

You will see `·B  STD  ‹Reassemblierung unvollständig›` lines with a run of payload bytes (e.g. a counter
ramp `50 51 52 … CF`). This is the tool being **honest**: those are continuation pieces whose **header piece
was not captured** (or a Lost/sequence break occurred), so they cannot be anchored to a whole telegram.
They are counted as `incompleteA/B` — **not** `common`, **not** a match, **not** fabricated.

Almost always the cause is a **capture-window edge**: `compare` started mid-transfer, so the head piece of a
long telegram arrived before the window. Fix: **start `compare` before the traffic** and/or run longer
(`--seconds 60`) — then those telegrams reassemble cleanly to `common`.

Only if `incompleteB` stays high **after** starting before the traffic is it a real finding (a vendor losing
head pieces).

**A lagging column is NOT a dropout.** A small-APDU vendor (e.g. Siemens) delivers a long telegram as several
pieces that reassembly holds until complete, so in the `side` layout that side's column can look empty for a
moment while the whole is still being assembled. When a side is **actively reassembling** for more than
~200 ms, a live cue appears in the footer (aligned to that column in `side` layout):
`⟳ reassembling… (N pieces)` / `⟳ reassembliert… (N Stücke)`. It clears the instant the whole completes (or is
flushed tagged-`incomplete`). It is a **display cue only** — never a frame, never counted/captured/exported —
and it distinguishes "still assembling" from a genuinely silent side (which the reconnect banner / reliability
hint covers). A fast reassembly completes in milliseconds and never flashes the cue.

---

## 6. Spec basis (why the comparison is trustworthy)

All parsing/reassembly/comparison rules are derived from the KNX Standard v3.0.0 and cross-checked against
the HW-verified `knx` stack. See the **`knx-standard` skill** section
"Busmonitor raw frame · TP1 decode · FCS · fidelity comparison" for the clause+page citations. Key points:

- Raw LPDU starts at cEMI offset `2 + AddIL` (AddIL is vendor-variable — never a fixed offset); the last
  octet is the TP1 **FCS** = `0xFF XOR (XOR of all preceding octets)`.
- Frame total length: **STD = 8 + LG**, **EXT = 9 + LG**. Reassembly cuts at the exact length, no tolerance.
- **"Frame pieces" are NOT a spec mechanism** — the spec expects whole delivery; splitting is a small-buffer
  vendor artifact. Reassembly is a heuristic anchored on header-length + FCS, **gated by the 0x03 Lost flag +
  sequence number** so a lost tail can never be glued to the next telegram.
- **MUST byte-compare (never normalize away):** repeat flag, priority, hop count, address-type, and
  **STD vs EXT** (one bus event has exactly one encoding — a STD and an EXT frame are never equated), plus
  the F/B/P error flags and the ACK/NAK. **MAY strip as packaging:** the relative timestamp and the AddIL
  framing (the sequence number is not compared but is used as the reassembly guard).

The **raw byte-exact content diff is the primary result**; normalize and the metadata classes are layered
on top and never relax it.

---

## 7. Readability toggles

Purely **display** conveniences for the live interactive view — they **never** change the comparison logic,
the counters, the result, the exit code, or the scripted `-q` TSV / the XML export (the XML always contains
**all** frames). Each has a live key and a CLI flag to preset the initial state. `compare`-only.

| Key | Flag (initial state) | Default | What it does |
|---|---|---|---|
| **`f`** | `--only-diff` | off | **Filter** the live stream to non-`common` frames only (onlyA/onlyB/count/integrity/ack/notReported/incomplete). `common` is hidden; the counters and result are unchanged. |
| **`c`** | `--collapse` | off | **Collapse** consecutive identical-content frames into one line with per-side counts, e.g. `T_Disconnect  ↻ A×2 · B×3`. Display aggregation only — the multiset diff and counts are untouched. |
| **`m`** | `--no-markers` (→ off) | **on** | **Markers** on each frame: `(Repeat)` when the TP1 CTRL repeat bit is set (bit5 = 0), `(reassembled ×N)` when normalize joined N pieces (on either side of a pair), `(edge)` for a frame inside the first/last grace window. Small tags that **add** info, hide nothing. |
| **`t`** | `--skew` | off | **Time-skew** on a content-matched pair: the A↔B host capture-time delta, e.g. `A@54.208 B@54.210 (+2ms)`. |

Two summary aids are shown automatically (not under `-q`):

- **Per-service breakdown** — when there are divergences, each divergence class is broken down by
  service/telegram type, e.g. `onlyA 758 = FTC 700 · T_Disconnect(repeat) 40 · GroupWrite 18`. The parts
  sum to the class counter.
- **Reliability hint** — one line `result may be affected by: …` when the run is fragile: a reconnect
  happened on A or B, the capture was short (< 3 s or < 20 frames), or `incomplete*` is a large fraction.

---

## 8. XML export (`l`)

`l` saves the whole run to `ftc-<mode>-<compare|multi>_<ipA>_<ipB>_<YYYYMMDD-HHMMSS>.xml` — ETS-telegram
shape plus the compare annotation, for offline analysis:

```xml
<compare mode="bm" result="DIVERGENT" divergences="…" ifaceA="…" ifaceB="…" statusA="yes" ackA="yes" …>
  <counters common="…" onlyA="…" onlyB="…" integrityMismatch="…" ackMismatch="…" notReported="…"
            lostA="…" lostB="…" incompleteA="…" incompleteB="…"/>
  <telegram time="…" frameFormat="STD|EXT" source="…" destination="…" raw="…"
            seenBy="AB|A|B" diff="common|onlyA|onlyB|integrityMismatch|ackMismatch|notReported|incomplete"
            statusA="F0B0P0" ackA="ACK" statusB="F0B0P0" ackB="ACK"/>
</compare>
```

The `<telegram>` shape stays ETS-readable; the `seenBy`/`diff`/`statusX`/`ackX` attributes are the compare
overlay. The result/field names stay stable English tokens even under `--lang de` (machine interface).

**Capture capacity:** the whole run is buffered in host RAM up to **100 000 frames** (≈ 30 MB) — enough for
a long fidelity session. When full, the oldest frames are dropped and a one-line note is logged
(`capture buffer full` / `Aufzeichnung voll`) — never a silent truncation. Plain `bm`/`gm` use the same bound.

---

## 9. Gotchas

- **Busmon is one exclusive slot per interface.** If a slot is held (`E_NO_MORE_CONNECTIONS` / 0x24), wait
  for the ~120 s KNX timeout or free it.
- **`A ≠ B`** — comparing an interface with itself is rejected.
- **Same bus** for `compare`; **different lines** → `--multi`.
- `--raw` is inspection only — never read a fidelity result from the raw per-piece stream.

# FTC Console Tunnel — remote OpenKNX console over the KNX bus

`ftc <pa> console` opens a **transparent, interactive session into another OpenKNX device's own console**,
carried over the KNX bus — no serial cable, no PC required. You type `mem`, `fs`, `info`, … locally and the
**remote** device runs them and streams its output back, until you `quit`.

It is part of the FileTransferModule (FTC) but a **separate channel** from the file-transfer command table:
file transfer is object index **159**, the console is object index **160**. They share nothing but the
transport style.

> **Opt-in.** Both devices must be built with **`-D OPENKNX_FTC_CONSOLE`** (§ *Build*). The client side also
> needs `-D OPENKNX_FTC` for the `ftc` command. Without the flag the code compiles out cleanly and `ftc ?`
> does not list `console`.

---

## 1. Using it

```
ftc 5.0.3 console      # step in -> you are "inside" the target's console
mem                    # runs on 5.0.3; its output streams back to you
info
fs
quit                   # (or `exit`) -> back to your local console
```

- **Step in / out.** `quit` and `exit` are caught **locally** and close the session; `ftc cancel` is an
  escape hatch. While you are inside, each finished input line is diverted to the target (via a `Console`
  line-sink) instead of running locally; your terminal still echoes your keystrokes normally.
- **From the host** the native `ftc-cli` (`ftc --ip <interface> <pa> console`) drives the exact same tunnel
  over a KNXnet/IP tunnel — see [`../ftc-cli/README.md`](../ftc-cli/README.md).
- **Reboot commands** (`restart` / `erase`) end the session: the client reads the ensuing silence as
  "device rebooted, session over" and steps back out.

---

## 2. How it works

### 2.1 Wire — object 160, two properties

The console rides **only** standard `A_FunctionProperty_Command` / `A_FunctionProperty_State_Response`
(exactly like the rest of FTC), so it **routes through line/area couplers** and adds **0 LOC to `lib/knx`**.

| Property | Dir | Payload | Purpose |
|---|---|---|---|
| `PID_IN` (1) | client → target | `[flags][line…]` — flags bit0 = OPEN, bit1 = CLOSE | park one input line / open / close the session |
| `PID_OUT` (2) | client → target | answer `[status][more][overflow][text…]` | drain the target's console output ring |

`OPEN` carries the client's PA (logged at the target only). Everything is **lockstep**: the client parks a
line or drains a bounded window, waits for the answer, then does the next step — never more than one request
outstanding.

### 2.2 Output capture — free, via the log ring

The server drains the shared **`OPENKNX_WEBCONSOLE` log ring** (implied by `OPENKNX_FTC_CONSOLE`, default
4096 B). Because the OpenKNX console already writes everything through the logger, capturing its output costs
**zero extra code** — background logs stream back too, not just command replies. The client writes each
drained chunk **verbatim** to its serial (under the logger mutex), *not* through `log()`, so the remote text
is not re-timestamped or reformatted.

### 2.3 Non-blocking (VORGABE)

- The **dispatch handler** only parks a line or copies a **bounded ≤247 B** window out of the ring — it never
  runs a command and never touches flash.
- The **command itself** runs in the target's `loop()` under `freeLoopTime()` + `skipLooptimeWarning()` —
  exactly like the local USB console (an accepted one-shot, not a new stall).
- The **client** drains cooperatively (one bounded chunk per `loop()` pass) and keepalive-polls every ~3 s to
  pick up asynchronous logs.

So a console session never blocks either device's `loop()`, and it **tolerates a congested bus** the same way
the `safe` transfer mode does — it just drains more slowly.

### 2.4 One session at a time

A single logical owner. A second device trying to open gets `busy`. On OPEN the target's **local USB console
is disabled** (`disableConsole(true)`) and re-enabled on CLOSE, so output goes to exactly one place. An idle
session (no poll for 60 s) is **reaped**, so the target is never left deaf if a client vanishes.

---

## 3. Truncation (honest, not silent)

The output ring is bounded (default 4096 B). A single burst **larger than the ring** between two drains
overwrites the head; the server flags it and the client prints `[...output truncated...]` **once**, then
continues cleanly. Large `mem` / `fs` dumps can trip this — raise `OPENKNX_WEBCONSOLE_BUFSIZE` if it matters
for your target. A dedicated, larger device console ring is a known TODO.

---

## 4. Security

An unauthenticated `A_FunctionProperty` carrier into a live console is **full device control** — `erase`,
`restart`, `dw`/`aw`, `flash` dumps are all reachable. Treat the console flag accordingly:

- The **first gate is the build flag** (`OPENKNX_FTC_CONSOLE`, default off) — a device without it has no
  console surface at all.
- Where `OPENKNX_FTC_CONSOLE` is set, it **pulls in `OPENKNX_FTC_SECURITY`** by default, so opening the
  console requires the same password gate as a write (see [`FTC-Security.md`](FTC-Security.md)). Opt out
  deliberately with `-D OPENKNX_FTC_CONSOLE_INSECURE` on a trusted/development bus.
- The `ftc-cli` console mode **never relays** a `login` / `logout` line (that would leak the password as
  plaintext over object 160) — run `login` as a separate one-shot before opening the console.

---

## 5. Build

```
; both the client and the target need the flag:
build_flags = -D OPENKNX_FTC          ; client: the `ftc` command
              -D OPENKNX_FTC_CONSOLE   ; console tunnel (implies the log ring + access control)

; trusted/dev bus, skip the password gate the console would otherwise pull in:
build_flags = -D OPENKNX_FTC_CONSOLE -D OPENKNX_FTC_CONSOLE_INSECURE
```

`OPENKNX_FTC_CONSOLE` on the server implies `OPENKNX_WEBCONSOLE` (the log ring, `OPENKNX_WEBCONSOLE_BUFSIZE`
= 4096 B RAM by default). See the switch table in the [README](../README.md#build-switches-feature-gates).

---

## Code anchors

- Server: `FileTransferModule::conFunctionProperty` / `conLoop`.
- Client: `FileTransferClient::requestConsole` / `consoleFeedLine` / the `FtcConsole` state.
- Reference: [`FTC-Reference.md`](FTC-Reference.md) (the full FTC protocol) · access control:
  [`FTC-Security.md`](FTC-Security.md).

# FTC access control — motivation, threat model, architecture

**For:** anyone reviewing or extending the access control, or wondering why it is only as strong as it
is. What it does and how to use it: [unlocking-a-device.md](../guide/unlocking-a-device.md).

**Status: built.** Server, ETS parameters and client (`login` / `logout`, the challenge-response, the
pre-write check in the desktop client) are all in place. This document survives implementation on
purpose -- it holds the threat model and the non-goals, which is what stops the next reader mistaking a
deterrent for a cryptographic boundary.

---

## 1. Motivation — why this exists

The file-transfer module (FTC) exposes **write** actions over KNXnet/IP: upload, format, delete, mkdir,
rmdir, rename, firmware-update, and the remote console. On a plain OpenKNX device these are ungated — anyone
who can reach the device over the tunnel can format its filesystem or take over its console.

The obvious gate is "only in programming mode". But **the programming button is not always physically
reachable** (device in a locked cabinet, on a DIN rail behind other gear, remote installation). So we need a
gate that can be **opened remotely** without walking to the device — a password.

## 2. Threat model — a coarse deterrent, NOT a hardened crypto boundary

This is deliberately a **"rough" safeguard**. The house analogy the design is built on:

> A lock on the window keeps out people who want to get in but **won't smash the glass**. There is **no
> alarm system**. Someone determined to break in can break the window — that is out of scope.

Concretely:

- **In scope:** an unauthorized user on the network who tries to use the normal FTC commands (ETS, `ftc`,
  the remote console) to write to / take over the device. They are stopped — they don't have the password.
- **Out of scope ("smashing the glass"):** an attacker who sniffs the KNXnet/IP tunnel and
  replays/brute-forces captured *challenge-response* material offline, or otherwise breaks the protocol. We
  do **not** claim protection against them. There is no KNX Secure on this product; the medium is observable.

Everything below is sized to that model: enough to deter casual/unauthorized access, cheap enough to cost
nothing on the hot path, and honest about where the glass is thin.

### Hard invariant — the password is NEVER on the wire in clear, from anywhere

Runtime authentication **never** transmits the password. From **any** entry point, the password is turned
into a MAC **at the point of entry**, and only the challenge-response (`nonce`, 4-byte `MAC`) ever crosses
the bus / tunnel:

- **ftc-cli → device:** ftc-cli computes the MAC locally; the password never leaves the PC.
- **ftc-cli remote console → device A → device B:** ftc-cli **does not relay** a `login` line into A's
  console. It **intercepts** `login`/`logout` locally and runs the 103/104 handshake to the target over its
  own tunnel. So there is no plaintext-password path even in the nested case ("egal von wo").
- **Local/serial console on a device:** that device's own client computes the MAC; only nonce+MAC leave it.

This invariant is why login/logout are handled by whoever holds the password, never forwarded as text.

### One unavoidable provisioning caveat (NOT runtime auth)

- **Plain-line ETS password download.** ETS must deliver `FTM_Password` **into** the device's parameter
  memory once; without KNX Secure that one-time download is sniffable on an unencrypted line. This is ETS
  provisioning, separate from the runtime auth above, and unavoidable on a non-Secure product. The ETS help
  text says so.

### Accepted residual risks (consequences of the coarse model)

1. **Offline brute force** of a captured `(nonce, 4-byte MAC)`. 2^-32 forgery margin per guess; mitigated by
   a non-blocking on-device back-off after repeated failures and a strong-password help text. Not claimed to
   resist a determined offline attacker ("smashing the glass").
2. **Not caller/PA-bound.** The authorized window is device-global ("egal von wem"): once open, any client on
   the network may write until it idles out. Intended (best-effort, minimal state); kept safe by the short
   challenge TTL + single-outstanding challenge + back-off.
3. **Config itself is rewritable without KNX Secure.** The gate protects the FTC command surface only. An
   attacker on the tunnel can `A_Memory_Write` the `FTM_Security` param to "Always" or blank `FTM_Password`
   via the management channel and then write freely — bypassing the gate by reprogramming, not by breaking
   it. This is the accepted non-Secure baseline (= "smashing the glass"), not a code defect; documented so
   nobody over-trusts the deterrent.
4. **`Cancel` (cmd 90) is ungated** (a read-class op that closes an open file/dir handle). An unauthorized
   on-network user can abort an authorized user's in-progress transfer — a nuisance DoS, no integrity loss
   (fast-path bits are set only after CRC + write). Accepted within the coarse model.

### Security review

The server implementation and this architecture were reviewed adversarially against the threat model
above. What holds, within that coarse model: the defaults fail closed, the write-command list is complete
(fast data, the chunked wrapper and the console are all gated), the challenge is single-use and therefore
resists an on-wire replay, the code is memory-safe and non-blocking, and the Router build is byte-identical.
What the review changed: the window now
opens **only** on a verified login (never via an accepted write — closes a stale-window fail-open across an
Always→Password stage flip); the back-off timer uses a wrap-safe elapsed compare. The client-enforced
confidentiality invariant below has **no device-side backstop** (the device cannot stop a client that relays
`login <pw>` as plaintext) — the client interception must therefore be airtight.

---

## 3. Model — a login with auto-logout

One global, best-effort **authorized window** on the device. Think *login … auto-logout*:

- `ftc <pa> login <pw>` opens the window (one challenge-response). This is the **only** thing that opens it
  — an accepted write never opens it, it only extends an already-open window (security-review MED fix).
- While open, **all** FTC writes + console take-over pass — no password re-entry, no per-write handshake
  (the client just sends; the device already knows the window is open). Every accepted action **refreshes**
  the window.
- The window **idles closed after a configurable timeout** (ETS param `FTM_AuthTimeout`, default **240 s**,
  clamped 30–3600 s, read live) — auto-logout.
- `ftc <pa> logout` closes it immediately (explicit, cmd 105).

The window lives **on the device**, not on the connection — so a `login` in one `ftc` invocation opens it for
subsequent separate invocations within the idle timeout, and for console-to-console use.

### Four stages (ETS parameter `FTM_Security`)

| Stage | Value | Meaning |
|---|---|---|
| Off | 0 | File transfer fully locked (reads **and** writes; only `CheckFeatures` answers) |
| ProgMode | 1 | Writes/console allowed only while the device is in programming mode |
| Always | 2 | No protection (legacy behaviour) — **beta default**, `TODO(secure-default)` to flip before release |
| Password | 3 | Writes/console require the login window; reads stay open |

Unconfigured device → treated as **Always** (nothing to protect; avoids a lock-out — mirrors OTA's
`handleOTA`). The stage + password are read **live** from the ETS params (not cached), so a config change
takes effect without a stale-cache window.

---

## 4. Architecture

### 4.1 Wire protocol (FunctionProperty, object 159)

Reuses the existing FTC command object (159). Three new command IDs (103/104 free; 100/101/102 taken):

| ID | Name | Direction | Payload |
|---|---|---|---|
| 103 | AuthChallenge | client→device, device→client | req: none · resp: `0x00` + 16-byte nonce |
| 104 | AuthResponse | client→device, device→client | req: 4-byte MAC · resp: `0x00` ok / `0xA1` fail |
| 105 | AuthLogout | client→device | resp: `0x00` (clears the window) |

Status bytes: `0xA0` AUTH_REQUIRED · `0xA1` AUTH_FAILED · `0xA2` WRITES_DISABLED.
CheckFeatures(102) bits: `0x10` AUTH_REQUIRED (device uses password) · `0x20` WRITES_DISABLED (writes
currently blocked — read per connect).

### 4.2 Crypto (reuses the AES already linked by knx — zero extra flash)

- **Key** = `pad16(password)` — the 16-char ETS password null-padded to 16 bytes **is** the AES-128 key.
  No KDF (a determined offline attacker is out of scope; this saves code).
- **MAC** = first **4 bytes** of `AES_ECB(key, nonce)` — a CBC-MAC over one 16-byte block collapses to a
  single ECB op. Client and device compute identically; constant-time 4-byte compare.
- **Nonce** = seeded AES-CTR, one block: `AES_ECB(seedKey, ctr‖micros())`. `seedKey` is derived once from
  device entropy (PA, `micros()`, a stack address) under a fixed public domain-separation constant and is
  never on the wire. A monotonic counter guarantees single-use; AES under an unseen seed gives
  unpredictability. **Nothing per data chunk** — the upload hot path is untouched.
- Include is `#include "knx/aes.hpp"` (the C++ `extern "C"` wrapper; `aes.h` alone mismatches C++ linkage).

### 4.3 Server (device) — `OFM-FileTransferModule/src/FileTransferModule.{h,cpp}`

All behind `#ifdef OPENKNX_FTC_SECURITY`. Products without the flag compile byte-identical (the Router does).

- Gate in `processFunctionProperty` (obj 159), before the command switch:
  - 103/104/105 handled first (no write side effect).
  - Stage Off → lock everything except CheckFeatures.
  - Write commands (Format/Rename/FileUpload/FileDelete/DirCreate/DirDelete/FileUploadFast/FwUpdate) gated
    on `secWriteAllowed()`; reads pass. Accepted write refreshes the window.
- Console OPEN gate (obj 160) → same global window (one auth mechanism, no separate console sub-flag).
- State (~55 B): `_authorized`, `_authLastMs`, `_nonce[16]`, `_challengePending`, `_challengeMs`,
  `_seedKey[16]`, `_seeded`, `_nonceCtr`, `_authFailCount`, `_authBackoffMs`.
- Challenge: single outstanding, **30 s TTL**, single-use. Verify: back-off check → challenge validity →
  empty-password fail-closed → 4-byte const-time MAC compare → open/refresh window on success.

### 4.4 Client — `OFM-FileTransferModule/src/FileTransferClient*` (shared) + `ftc-cli`

Also behind `#ifdef OPENKNX_FTC_SECURITY` (so the Router's client stays byte-identical; `ftc-cli` and the
interface define the flag). Two **standalone** console commands — the write paths are **untouched**:

- `login <pw>`: `pad16(pw)` → send 103 → receive nonce (`FtcAuthChallenge`) → compute 4-byte MAC into the
  TX buffer → send 104 → receive status (`FtcAuthResponse`) → report authorized / failed. Standard
  `FTC_TIMEOUT` on both states.
- `logout`: send 105 → report.
- Friendly surfacing on other commands: `0xA0` → "auth required — run: ftc <pa> login <pw>", `0xA1` →
  "auth failed — wrong password?", `0xA2` → "writes disabled".
- `ftc-cli` build: add `knx/aes.c` via `build_src_filter`; the password comes through the same
  `ftc <pa> login <pw>` console line (no separate `--password` flag). Password: ≤16 chars, no spaces.
- **ftc-cli console mode intercepts `login`/`logout`** (does not relay them to the remote device's console):
  it runs the 103/104 handshake locally against the target PA over its own tunnel, so the password is never
  relayed as plaintext (see §2 invariant). All other typed lines relay as before.

### 4.5 ETS — `OFM-FileTransferModule/src/FileTransfer.share.xml` (new)

The module's first own ETS XML. `FTM_Security` enum (default Always) + `FTM_Password` (TypeText, 16 chars,
shown only in stage Password via `choose/when test="3"`), on its own "Datei-Transfer" tab. Products opt in
with `op:define prefix="FTM" ModuleType="13" share="…/FileTransfer.share.xml"` + `-D OPENKNX_FTC_SECURITY`.

---

## 5. Non-goals

- Not KNX Secure. Not a replacement for it.
- Not resistant to a network sniffer / offline brute-forcer (see §2 "smashing the glass").
- Not per-user / per-PA access control (single global window by design).

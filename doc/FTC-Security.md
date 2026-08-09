# FTC Access Control — password gate for writes & console

Opt-in behind **`-D OPENKNX_FTC_SECURITY`**. A **coarse deterrent — a lock, not an alarm system** — that
gates the FTC **write** surface and the console take-over so an unauthorized user on the network cannot
write or take the console without a password.

> **What it is not.** It is **not KNX Secure.** It does not resist a tunnel sniffer or an offline
> brute-force, and that is out of scope **by design**. Its job is to stop casual/remote writes when the
> device's programming button is not physically reachable. For real confidentiality use KNX Secure.

Without the flag the module + client compile **byte-identical** — any product that does not opt in carries
none of this.

---

## 1. What is gated

| Surface | Gated? |
|---|---|
| **Writes** — upload, `format`, `rm`, `mkdir`, `rmdir`, `mv`, fw-update | **yes** |
| **Console** take-over (object 160 OPEN) | **yes** (see [`FTC-Console.md`](FTC-Console.md)) |
| **Reads** — `ll` / `ls` / `df` / `info` / download | open (except stage *Off*) |
| `CheckFeatures` (so a client can discover the gate) | always answers |

Result codes on the wire: `0xA0` auth required (run `login`), `0xA1` auth failed (wrong/empty password),
`0xA2` writes disabled (stage *Off*, or not in programming mode at stage *ProgMode*).

---

## 2. Model — login with auto-logout

One **device-global, best-effort** authorized window (not bound to a single client PA):

- `login <pw>` runs a challenge-response and **opens** the window; `logout` (cmd 105) closes it.
- While open, all writes / console pass with **no per-write handshake** — the client just sends; a closed
  window answers `0xA0`.
- Every accepted write **refreshes** the window; it idles closed after the ETS `FTM_AuthTimeout`
  (default 240 s, range 30–3600, read **live**) — auto-logout.
- The window opens **only** on a verified login, **never** via an accepted write — otherwise a stale window
  could leak across an *Always → Password* stage change.

---

## 3. Stages (ETS `FTM_Security`)

| Stage | Value | Behaviour |
|---|---|---|
| **Off** | 0 | Everything locked; only `CheckFeatures` answers. |
| **ProgMode** | 1 | Writes allowed only while the device is in KNX programming mode. |
| **Always** | 2 | Legacy / no protection (the beta default). |
| **Password** | 3 | Writes/console require a `login`. |

Parameters are read **live** each time, so a just-programmed device is never stale. **Unconfigured → treated
as *Always*** (nothing to protect yet; avoids locking yourself out of a fresh device).

---

## 4. Crypto — reuses the AES already in the build

No new algorithm, **zero extra flash** (`#include "knx/aes.hpp"`, already linked by `knx`; add
`knx/src/knx/aes.c` to the build):

- `key = pad16(password)` — up to 16 chars, taken directly as the 16-byte AES key (no KDF).
- `MAC = first 4 bytes of AES_ECB(key, nonce)` — a CBC-MAC over one block.
- `nonce` = one seeded AES-CTR block (monotonic counter → single-use; the seed never goes on the wire).

The password is turned into the MAC **at the point of entry** and **never** travels the bus in clear — only
the nonce and the 4-byte MAC do, from any client ("egal von wo").

---

## 5. Client flow (`FileTransferClient`)

1. `login` first probes `CheckFeatures` (bit4) — a target that is **not** password-protected reports it
   immediately instead of timing out.
2. `FtcAuthChallenge` — send cmd 103, receive the 16-byte nonce.
3. Compute the MAC locally, then `FtcAuthResponse` — send cmd 104, receive `0x00` (ok) / `0xA1` (fail).
4. `logout` sends cmd 105.

The `ftc-cli` console mode **never relays** a `login` / `logout` line (it would leak the password as
plaintext over object 160) — run login as a separate one-shot before opening a console.

Repeated `0xA1` triggers a **non-blocking back-off** on the server, so the gate can't be hammered.

---

## 6. Backward compatibility

Guaranteed by the additive protocol + try-and-error, in both directions:

- **new client → old server:** the old server has no gate, so a write is never answered `0xA0` → works
  unchanged.
- **old client → new server:** writes are correctly blocked with a generic rejection (no crash); reads still
  work.
- Unknown `CheckFeatures` bits are ignored by old readers.

---

## 7. Build

```
build_flags = -D OPENKNX_FTC_SECURITY   ; server gate (cmds 103/104/105) + client login handshake
```

A product that ships the gate must also ship the ETS access-protection parameter block
(`FileTransfer.share.xml`). Product-side ETS parameters, texts and threat model:
**`OAM-IP-Interface/doc/FTC-SECURITY.md`**. Protocol context: [`FTC-Reference.md`](FTC-Reference.md) §4.
Switch table: [README](../README.md#build-switches-feature-gates).

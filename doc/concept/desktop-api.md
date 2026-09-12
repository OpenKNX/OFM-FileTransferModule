# Concept — `ftc --api`: local server + offline HTML front-ends

**For:** anyone about to build a graphical front end for the desktop client.

**Status: decided, NOT built.** `ftc --api` does not exist in the binary today; this is the shape it is
to take when it does. The decision it records is the useful half: the earlier webview and ImGui
approaches are **discarded**, so a new GUI attempt should start here rather than from scratch.

## The decision

Drop the "embed a GUI inside a native binary" idea (webview / Dear ImGui). Instead give **`ftc-cli`
an API mode** (`ftc --api`): it holds the KNXnet/IP connection to the bus, exposes a small **local HTTP
server** with **Server-Sent Events (SSE)** for live streams, and drives the **same
`FileTransferClient` + tunnel code the CLI already ships**. Every UI is then a **plain, offline HTML
page** (the `BusmonViewer`, a new FTC GUI) that talks to `ftc --api` over `localhost`.

One binary. Cross-built with `zig` exactly like the CLI today. No native webview, no per-OS GUI
toolchain, no bundled browser. The browser *is* the UI.

## Architecture

```
   Offline HTML            ftc-cli --api                 OpenKNX / KNX device
   (any browser)   ──HTTP/SSE──►  local server   ──UDP (KNXnet/IP :3671)──►  interface / bus
   BusmonViewer            holds the tunnel(s)           busmon + tunnelling + FTC
   FTC GUI                 = the CLI's engine
```

**The one hard constraint (why not "just UDP in the browser"):** browsers have **no raw UDP sockets**.
So the split is fixed:
- **KNX side (ftc-cli ↔ interface):** UDP KNXnet/IP — busmon tunnel + FTC tunnel. ftc-cli already does this.
- **Browser side (HTML ↔ ftc-cli):** HTTP + SSE on `localhost` only. Never UDP, never exposed off-box.

This also makes the earlier **pwsh bridge obsolete**: `ftc --api` serves the exact same busmon SSE
stream natively (the KNX code lives in ftc-cli, not re-implemented in PowerShell). The bridge stays only
as a zero-ftc-cli fallback.

## HTTP / SSE API (one server, all front-ends)

| Method / path | Purpose |
|---|---|
| `GET /` | serve the HTML UI (embedded C string, or from disk in dev) |
| `GET /busmon/stream?ip=<knxIp>&port=3671&layer=<busmon\|link>` | **SSE** of `L_Busmon.ind` frames — **same contract as the pwsh bridge** |
| `POST /ftc/exec` `{pa, verb, args}` | run one FTC verb (`ping`/`info`/`df`/`ll`/`scan`/`send`/`get`/`perf`/`rm`/`mkdir`/`rmdir`/`mv`/`format`) → JSON result |
| `GET /ftc/console/stream?pa=<pa>` | **SSE** live remote console output (device → browser) |
| `POST /ftc/console/input` `{line}` | send a typed console line (browser → device) |
| `POST /ftc/upload` / `GET /ftc/download` | file transfer payloads |
| `GET /health` | server + tunnel status |

**SSE busmon contract (unchanged, reused from the pwsh bridge):** `text/event-stream`,
`Access-Control-Allow-Origin: *`, keep-alive; `event: hello\ndata: {"mode":..,"iface":".."}`; per frame
`data: {"hex":"2B..","ts":<epochMs>}`; `: ping` keepalive. So today's `BusmonViewer` live mode attaches
to `/busmon/stream` with **no change**.

## How the front-ends attach

- **BusmonViewer.html** — its live mode already speaks the SSE contract. Point it at
  `http://localhost:8671/busmon/stream?ip=…` instead of the pwsh bridge. Done.
- **FTC GUI (HTML)** — the phosphor mock (`ftc-gui-concept.html`) becomes real by calling `/ftc/exec`
  (file browser, transfer, df/ll/info) and `/ftc/console/*` (live terminal). Pure fetch/EventSource.
- Optional: `ftc --api` can **serve both** pages at `/` and `/busmon`, so one command → open a browser tab.

## Reuse (no forked engine)

`ftc --api` links the **same `ftc_core`** the CLI uses (the "already almost UI-free" `main.cpp` — see the
superseded integration doc's finding): tunnel lifecycle, `processCommand` dispatch, the QUIET_MS drain
loop, discovery/scan, the console session. The API layer only turns the terminal edges (stdout, stdin,
progress) into HTTP responses + SSE events — the KNX/FTC logic is untouched and HW-validated.

## Server implementation notes

- Tiny **HTTP/1.1 + SSE** server in C++ on the existing socket code (hand-rolled, or a single-header lib).
  SSE is trivial (the pwsh bridge is the reference). Static HTML embedded via `xxd -i`.
- **Non-blocking:** the KNX tunnel pump + FileTransferClient run off the HTTP thread; progress/console/
  busmon frames are pushed to SSE clients. Same discipline as the firmware's non-blocking `loop()`.
- **Cross-build:** plain sockets + std C++ → the existing **single `zig` cross-matrix** covers all OSes.
  No per-OS backend (unlike the discarded webview path).
- **platformio.ini:** one extra build flag / env `ftc-api-*` reusing `ftc_base`; no new toolchain.

## Security

`localhost`-bound only. No external listener. The busmon/console data never leaves the machine. CORS `*`
is safe because the server is not reachable off-box. (Windows may need a URL-ACL for the HTTP port, as
noted for the pwsh bridge.)

## Effort

- Mini HTTP/SSE server + wire endpoints to `ftc_core`: **~2–3 days** for v1 (busmon + exec + console).
- The busmon SSE endpoint is a **direct port of the pwsh bridge logic** into C++ (KNX side already exists).
- FTC GUI HTML against the API: incremental (the mock already exists).

## Steps

1. Extract/confirm `ftc_core` (tunnel + dispatch + console) as the shared engine (safe, CLI-preserving).
2. Add `ftc --api [--port 8671]`: HTTP server + `/busmon/stream` (port the bridge) + `/health`.
3. Point `BusmonViewer` live mode at the ftc-cli endpoint; retire the pwsh bridge to fallback.
4. Add `/ftc/exec` + `/ftc/console/*`; wire the FTC GUI HTML.
5. Embed the HTML, serve at `/`; one command opens the whole toolset in a browser.

---
© 2026 by Erkan Çolak · OpenKNX

# The knxOTA page

**For:** anyone operating a device. A browser front-end onto the embedded client: the device you are
looking at sends a firmware to **another** device over the bus. No PC in the chain, no console.

`http://<device-ip>/knxota` · build switch `OPENKNX_FTC_KNXOTA_WEB` · 34732 B flash + 80 B RAM on
RP2040, 40056 B + 64 B RAM on ESP32.

## The layout

```
  ┌──────────────────────────────────────────────────────────────┐
  │  Target 5.0.3 · access free · LittleFS 780 KB free           │  summary, always visible
  ├──────────────────────────────────────────────────────────────┤
  │  ▸ Log                                                       │  collapsed by default
  ├───────────┬───────────┬──────────────────────────────────────┤
  │ ① Target  │ ② File    │ ③ Transfer                           │  numbered steps
  └───────────┴───────────┴──────────────────────────────────────┘
```

The step number is a traffic light, and it gates the next step:

| | ① Target | ② File | ③ Transfer |
|---|---|---|---|
| **red** | no PA entered | no file picked | steps 1 and 2 not green |
| **amber** | PA answers, but writing is locked | space on the target is tight | — |
| **green** | PA answers and writing is allowed | file picked and it fits | ready |

A blocked tab stays visible — it shows what is still open instead of disappearing.

## ① Target

* **PA** — empty on entry, on purpose. Nothing is addressed until you say what.
* **Read device** — one bus run: manufacturer, order number, hardware type, mask version, FTM
  version, feature bits, and the 16 tunnel addresses if the target has them.
* **Search** — its own area and line range, so you can scan a foreign line, not only your own.
* **Prog mode** — one toggle. On, off. Some targets need it before they accept a write.
* **Group addresses** — a separate run of several minutes, never folded into "read device"; the
  result stays open once it arrives.
* **Drives** — the page probes whether the target actually has `sd/` and `efc/`, because a target
  without them silently answers with its internal filesystem instead.

## ② File

The source is **this** device's storage: internal flash, SD card or external flash. The listing shows
real sizes and marks what is a firmware (`.bin`, `.uf2`, `.gz`) and what is a difference (`.okd`).

## ③ Transfer

* A progress bar and a throughput curve appear **only while a transfer runs**, and disappear again
  afterwards.
* The throughput shown is the page's own average. The device reports a momentary rate that decays
  between two polls — measured 4913 down to 352 B/s without a byte of progress — so it is not used.
* You cannot switch tabs while a transfer runs. That is enforced, not just discouraged.
* After **apply**, the page reads the version back and compares it against the version before the
  trigger. That comparison is the only proof the new firmware is running, because `FwUpdate` answers
  nothing when it succeeds ([FIRMWARE-UPDATE.md](FIRMWARE-UPDATE.md)).

## How it talks to the device

The page never holds a request open. `start` arms a job and answers at once; the browser polls
`/knxota/status`. That is what keeps a 20-minute transfer compatible with a web server that has to
stay responsive.

| Route | Purpose |
|---|---|
| `/knxota` | the page shell; the markup is built in the browser from the embedded JS |
| `/knxota/status` | phase, busy, ok, message, progress, device, scan hits, drives, GA, result |
| `/knxota/files` | the source listing per drive |
| `/knxota/start` | arm a transfer — answers `409` when the client is already busy |
| `/knxota/ga`, `/drives`, `/progmode` | the separate bus runs of step ① |

Every exclusive endpoint answers `409` instead of queuing, so two browser tabs cannot start two
transfers.

## Not the same thing: the web file manager

`OFM-Network` serves a **file manager** (`OPENKNX_WEBFS`) for the device's own storage. It writes to
the device you are looking at; the knxOTA page writes to a device somewhere else on the bus.
Different tools, different wires — and that is why the file manager feels so much faster
([THROUGHPUT.md](THROUGHPUT.md)).

## Limits

* One transfer at a time — the client is a singleton, in the device as well as on the PC.
* Leaving the page during a transfer does not abort it: the job runs in `loop()` in the device. The
  file-manager upload in OFM-Network is the opposite — that one is a browser-side loop and dies with
  the page.
* The page is a front-end only. Everything it can do, the console ([CONSOLE.md](CONSOLE.md)) and
  `ftc-cli` ([FTC-CLI.md](FTC-CLI.md)) can do too.

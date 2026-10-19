# `oknx serve` — one process owns the connections

**For:** anyone who lets programs (Claude, the busmonitor viewer, scripts) work with KNXnet/IP interfaces
next to the command line, without the programs colliding on the interfaces.

`oknx serve` runs in the background, owns every connection it opens, and offers two ways in on
127.0.0.1 and ::1: an HTTP/SSE API for pages and scripts, and MCP for Claude. Both go through one guard
with one rules file, and run the same operations the command line runs.

A KNXnet/IP interface allows one busmonitor per line, and while it runs it refuses every other tunnel
(03_08_04 2.2.4). serve opens a monitor once and hands its telegrams to every reader; the command line
reserves what it needs at a running serve and works locally as before.

## Start, stop, look

```bash
oknx serve                      # foreground, port 8671, rules ~/.config/oknx/rules.json
oknx serve --detach             # in the background, log in ~/.config/oknx/serve/serve.log
oknx serve status               # connections, readers, reservations, bench lock
oknx serve stop
oknx serve describe             # every operation: arguments, class, what it changes
```

`--port N` and `--rules <file>` change the two defaults. One serve per port: a second one says which
program already holds it. `scripts/Test-OknxServe.ps1` checks these points on a machine (Windows, Linux, a
Raspberry Pi), with its own port and configuration folder.

Per interface serve starts a worker process (`oknx --_worker`), which owns that interface's connections.
A worker that dies is noticed at its pipe; its operations end with `INTERNAL`, and the next worker for
the interface first closes the connections the dead one left open (a state file per worker, entries
younger than 110 s; the interface itself drops older ones after 120 s, 03_08_02 5.4).

## The rules file

Without a rules file, and for every interface it does not list, serve allows only `passive`, `net` and
`device-http`: searching, and reading the state an OpenKNX interface reports over HTTP. Everything that
takes a tunnel slot or sends to the bus needs an entry.

```json
{ "version": 1, "rate_per_minute": 60,
  "sites": [
    { "name": "bench",
      "bench_config": "/path/to/OAM-IP-Interface/scripts/Test/Test-Bench-Config.json",
      "lock_file":    "/path/to/OAM-IP-Interface/scripts/Test/Reports/.bench.lock",
      "write_lines": ["1.1"], "setpa_lines": ["1.1"],
      "ga_write": { "allow": ["*"], "deny": [] },
      "suite_run": true,
      "interfaces": [
        { "ip": "11.11.0.210", "line": "1.1",
          "classes": ["passive", "net", "device-http", "bus-read", "group-monitor", "busmonitor",
                      "bus-write", "device-write"] } ] },
    { "name": "installation",
      "interfaces": [ { "ip": "11.11.0.151" } ] } ] }
```

- `bench_config` adds the suite's `safety.allowedLines` to `write_lines`, `rig.protectedTargets` to
  `protected` and `rig.powerRelayWrite` to `ga_write.deny`.
- An interface without `classes` gets everything except `busmonitor`, `bus-write` and `device-write`:
  a busmonitor on a live installation would refuse every other tunnel, the visualisation included.
- `protected` (`[{"pa": "1.1.3", "ops": []}]`) refuses every writing operation to that address unless
  `ops` names it.
- `suite_run: true` lets `suite_run` start `Run-Tests.ps1` next to `bench_config`.

The guard checks, in this order, and the first refusal decides: known operation, valid arguments, rate,
class allowed on the interface, target allowed (write lines, group address lists, protected targets,
the setpa line), bench free (the lock file), connection free.

## Claude

```bash
claude mcp add --transport http oknx http://127.0.0.1:8671/mcp --header "X-Oknx-Key: $(cat ~/.config/oknx/serve.key)"
```

serve speaks MCP 2026-07-28 and the 2025-03-26 … 2025-11-25 revisions on the same path. Long operations
answer as a stream; a caller that asks for progress gets it at least every 25 s, so a 300-s monitor is not
cut off. On 2026-07-28 closing that stream cancels the call; on the older revisions only
`notifications/cancelled` does. A result larger
than 60 000 characters comes back summarised, the full data as a resource (`oknx://results/<id>`).

`oknx mcp` is MCP over stdio for programs that speak only stdio; it passes every message on to the
running serve and opens no bus connection itself.

## Pages

```bash
oknx serve open           # prints a one-time link (60 s, once); --open hands it to the browser
oknx serve logout         # ends every browser session
```

`/ui/` has three views, in the look of the OpenKNX web interface:

- **Overview**: serve, the bench lock, the interfaces the search finds with what the rules allow on each, serve's
  connections (a monitor can be stopped there), the reservations of the command line (released there) and the
  recent operations with their result.
- **API**: every operation to try out. The form and the JSON of the request stay in step and are checked against
  the operation's schema while typing; the answer, the matching curl call and the schema are shown below. Long
  operations show their progress.
- **Rules**: `rules.json` to click together or edit as JSON, checked the way serve checks it when it loads.
  **Save** writes it only when serve's own loader accepts it, keeps the previous file as `rules.json.bak`, notes
  the change in `operations.log` and applies it to the next call, without a restart; a monitor or reservation that
  is already running goes on until it ends (stop or release it in the overview). The same is
  `GET`/`PUT /api/rules` (`{"text": "<the file>"}`) with the key; MCP has no operation for it.

`/ui/viewer` is the busmonitor viewer of the test suite, read from the folder next to the rules' `bench_config` and
connected to serve instead of the bridge; it is not part of the binary.

## The command line next to serve

A command that needs an interface asks a running serve first (300 ms) and reserves the interface for as
long as it runs; the reservation ends when the process ends, however it ends. Then it works locally as
it always does. A busmonitor and a management tunnel exclude each other: a command refused there names
what serve holds and for whom. A plain `bm` reads along a busmonitor serve holds instead of being
refused.

- `--no-serve` skips the reservation; `OKNX_SERVE_PORT` names serve's port when it is not 8671.
- `OKNX_BENCH_RUN` (set by the suite) also skips it: serve has released the bench to the run.
- `oknx serve lease rm <id>` ends a reservation by hand, e.g. of a `setpa` waiting for the button.

## The bench

A suite run holds the bench through `Reports/.bench.lock` with a heartbeat every 10 s; serve then closes
its connections to the named interfaces and refuses everything but `passive` there (`BENCH_LOCKED`).
By hand, the same way:

```bash
oknx bench lock flash-1 11.11.0.210 11.11.0.126     # Ctrl+C releases it
oknx bench status
oknx bench unlock
```

## Passwords

`ftc_login` never takes a password over MCP or the API. serve takes it from `OKNX_FTC_PW_<a>_<l>_<d>`
(`OKNX_FTC_PW_1_1_8`) of its own environment, or asks in its own terminal when it runs in the
foreground. It goes only to the worker of that interface.

## Access

Programs send the key of `~/.config/oknx/serve.key` (readable by you alone; `%APPDATA%\oknx\serve.key`
on Windows) in `X-Oknx-Key`. A browser uses the session cookie of `serve open`, and only on serve's own
pages. A request with another web page's origin is refused, also with the key. `oknx serve rekey` makes a
new key; Claude then needs it again.

## Error codes

Every result is `{"v": 1, "ok": …, "op": …, "id": …, "ms": …, "data" | "error"}`:

| code | meaning |
|---|---|
| `INVALID_ARGUMENT` | an argument is missing, of the wrong type or out of range; nothing was sent |
| `GUARD_DENIED` | refused by the rules or by the access checks; `detail.rule` names the rule where one applies |
| `RATE_LIMITED` | too many calls in one minute |
| `BENCH_LOCKED` | a suite run holds the bench |
| `BUSY` | the connection is taken (`detail.holder` says by whom), or several devices are in programming mode for `setpa` (`detail.devices`) |
| `CONNECT_FAILED` | the interface did not take the connection (`0x24`: all slots taken) |
| `NO_ANSWER` | sent, no answer in time |
| `TIMEOUT` | the operation's time limit passed |
| `CANCELLED` | cancelled by the caller, serve's stop or the bench lock |
| `INTERNAL` | a fault in serve itself |

`OKNX_SERVE_DEBUG=1` writes every MCP request's method and protocol headers (never argument values) to
serve's log.

The operations and their arguments: [serve-operations.md](../reference/serve-operations.md), generated by
`oknx serve describe --markdown`.

# The console over the bus

**For:** anyone operating a device; the build flag at the end is for integrators. The device console
from a distance: `ftc <pa> con` types lines and reads the output — no serial cable, no network at the
device.

## How it differs from the file transfer

| | files (object 159) | console (object 160) |
|---|---|---|
| Session | none | one, with OPEN and CLOSE |
| Direction | request → answer | two separate channels |
| PIDs | the command number ([PROTOCOL.md](PROTOCOL.md)) | `1` = in · `2` = out |

## The sequence

```
   Client                                        Device
     │                                             │
     │  160/PID_IN   [0x01, PA hi, PA lo]          │  OPEN -- the PA is logged, so it is
     │ ───────────────────────────────────────────▶│  visible who is typing
     │◀─────────────────────────────────────────── │  ok
     │                                             │
     │  160/PID_IN   "bcu stat\n"                  │  PARK the line (<= 247 B),
     │ ───────────────────────────────────────────▶│  do not run it
     │◀─────────────────────────────────────────── │  ok
     │                                             │
     │                                             │  -- loop(), under freeLoopTime --
     │                                             │  conLoop() runs the line, the output
     │                                             │  goes into the log ring
     │                                             │
     │  160/PID_OUT  (drain)                       │
     │ ───────────────────────────────────────────▶│
     │◀─────────────────────────────────────────── │  up to 247 B of text
     │  160/PID_OUT  (drain)                       │  … until empty
     │ ───────────────────────────────────────────▶│
     │◀─────────────────────────────────────────── │  empty
     │                                             │
     │  160/PID_IN   [0x00]                        │  CLOSE
     │ ───────────────────────────────────────────▶│
```

**The line is parked, not run in the dispatch.** A `help` in the middle of the KNX dispatch would
stall the stack. So: park, acknowledge, and let `conLoop()` run it when `freeLoopTime()` allows.

## The two limits you will notice

**The log ring is shared and 4096 bytes** (`OPENKNX_WEBCONSOLE_BUFSIZE`, OGM-Common
`Log/Logger.h`). An output larger than what the client can drain overwrites
itself — a `help` beyond 4 KB arrives truncated. The device reports that once (`_conOverflow`), so it
does not hide it. A ring of the console's own is on the list.

**You cannot reach your own PA.** `ftc <own-PA> con` runs into a timeout — a device does not process
frames it sent itself; you need a second interface ([QUICKSTART.md](QUICKSTART.md#before-you-start)).

## From the web interface

The web console passes typed lines through the same path (`Console::submitLine()` into `_lineSink`),
so the serial and the web console run side by side. The desktop client can additionally reach the
console of the **interface** over WebSocket (`ftc -i <ip> con`) — another way to the same place,
without the bus.

## Build flag

`OPENKNX_FTC_CONSOLE`. Without it object 160 disappears entirely, and `CheckFeatures` no longer
reports the console bit. It has to be set as a `-D` in the product's `ini`, not through a profile —
`lib/OGM-Common` reads it too ([FLAGS.md](FLAGS.md), [CONCEPT-defines.md](CONCEPT-defines.md)).

Opening the console is a **write** action: setting `OPENKNX_FTC_CONSOLE` pulls in
`OPENKNX_FTC_SECURITY` unconditionally, so the session is gated like every other write
([SECURITY.md](SECURITY.md)).

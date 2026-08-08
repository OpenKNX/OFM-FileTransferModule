# FTC Host-Shim Contract

**Single source of truth for the host-only glue shim** that lets the four embedded FTC files compile
**byte-identical** (no edits) on macOS/Linux/Windows, without compiling `lib/knx` or the OpenKNX Arduino
stack.

## Files in scope (compiled verbatim by `ftc-cli`)

| File | Lines | Real path |
|------|------:|-----------|
| `FileTransferClient.h` | 618 | `OFM-FileTransferModule/src/FileTransferClient.h` |
| `FileTransferClient.cpp` | 4987 | `OFM-FileTransferModule/src/FileTransferClient.cpp` |
| `FileTransferClientConsole.h` | 34 | `OFM-FileTransferModule/src/FileTransferClientConsole.h` |
| `FileTransferClientConsole.cpp` | 548 | `OFM-FileTransferModule/src/FileTransferClientConsole.cpp` |

Path note: in this repo `lib/knx -> ../../knx` and `lib/OFM-FileTransferModule -> ../../OFM-FileTransferModule`
(`ls -l lib/knx`). All `knx/...` citations below live under the resolved target
`/Users/ecolak/Entwicklung/OpenKNX-Router/knx`.

**Build gates the shim must set** (the entire body of all four files is inside `#ifdef OPENKNX_FTC`):
- `OPENKNX_FTC` — **required**, or every file compiles to nothing.
- `OPENKNX_FTC_CONSOLE` — optional; enables the console-tunnel code paths (`requestConsole`, obj-160,
  `setLineSink`). Recommended ON for a full ftc-cli.
- The shim's `OpenKNX.h` must **not** be the real one: the real one hard-`#error`s unless one of
  `ARDUINO_ARCH_SAMD/RP2040/ESP32` is defined (`OGM-Common/src/OpenKNX.h:7-9`). The shim replaces it, so
  define **none** of those arch macros. Consequence: the `#ifdef ARDUINO_ARCH_RP2040` branch in
  `littleFsFree()` (`FileTransferClient.cpp:119-125`) takes the **`#else`** path → the shim's `LittleFS`
  needs `totalBytes()`/`usedBytes()` and **not** `FSInfo` (see §1.3).

---

## 1. Includes — what each must resolve to

### 1.1 `#include "OpenKNX.h"` — the umbrella shim header
Cited at `FileTransferClient.h:10` and `FileTransferClientConsole.h:10`. This is the big one; the shim's
`OpenKNX.h` must transitively declare **everything** in §4–§7 below. Concretely it must pull in (or itself
declare):
- `knx` global + its facade type (`bau()`, `individualAddress()`) — §4
- `SecurityControl` + `DataSecurity` (from what the real stack calls `knx_types.h`) — §4
- `openknx` global (`OpenKNX::Facade`) with `logger`, `console`, `freeLoopTime()` — §5
- `OpenKNX::Module` base class — §4
- `CONSOLE_HEADLINE_COLOR` macro — §6
- `MODULE_FileTransferModule_Version` macro — §6
- `millis()` and `std::string`/`std::vector` availability — §7

Real chain being replaced (do **not** try to reproduce, just cover the leaf symbols the 4 files touch):
`OpenKNX.h` → `OpenKNX/Facade.h` → `OpenKNX/Module.h` → `OpenKNX/Base.h` → `<knx.h>` (the whole thelsing
stack). The four files reach only a thin slice of it (proven by §4–§7); the shim implements only that slice.

### 1.2 `#include "FileTransferClientConsole.h"` (`FileTransferClient.h:13`)
Own project header, compiled verbatim. No shim. (Itself includes `OpenKNX.h` → §1.1.)

### 1.3 `#include <LittleFS.h>` (`FileTransferClient.cpp:19`)
Needs a **host `LittleFS.h` shim**. Used only inside the built-in default backend
(`FileTransferClient.cpp:83-136`). Exact surface required:
- Type `File` (`FileTransferClient.cpp:86-87` `static File _ftcSrcFile;`/`_ftcSinkFile;`). Members used:
  - `explicit operator bool()` — `if (!_ftcSrcFile)` (`:91`), `if (!_ftcSinkFile)` (`:100`),
    `return (bool)_ftcSinkFile;` (`:114`), `if (!_ftcSrcFile)` (`:108`)
  - `uint32_t position()` — `:92`
  - `bool seek(uint32_t)` — `:92`
  - `int read(uint8_t* buf, size_t len)` — `:93`
  - `size_t write(const uint8_t* buf, size_t len)` — `:101`
  - `void close()` — `:96`, `:103`
  - `uint32_t size()` — `:109`
- Global object `LittleFS` with:
  - `File open(const char* path, const char* mode)` — `:107` (`"r"`), `:113` (`"w"`)
  - `uint64_t totalBytes()` / `uint64_t usedBytes()` — `:124` (the `#else`/non-RP2040 branch the host takes)
  - `void info(FSInfo&)` + type `FSInfo{ uint64_t totalBytes; uint64_t usedBytes; }` — `:120-122`.
    **RP2040-only branch; the host never compiles it.** Provide only if you choose to define
    `ARDUINO_ARCH_RP2040` (do not — see build-gates note). Safe to omit.

Back it with `std::fstream`/`std::filesystem`. This is the local file source/sink; **not** the KNX
transport. Small.

### 1.4 Plain std / libc includes — **use as-is, no shim**
- `FileTransferClient.cpp:12-15`: `<stdarg.h>`, `<string.h>`, `<string>`, `<vector>`
- `FileTransferClientConsole.cpp:13-16`: `<ctype.h>`, `<stdio.h>`, `<stdlib.h>`, `<string.h>`

All satisfied by the host libc / libstdc++.

---

## 2. The 18 `knx.bau().ftc*` methods — exact signatures

Declared in `knx/src/knx/bau_systemB.h` (class `BauSystemB`, `#ifdef OPENKNX_FTC` block lines 34-64).
Definitions in `knx/src/knx/bau_systemB.cpp:599-704`. `ftcTxQueueSize` is `virtual` in the base
(`bau_systemB.h:63`) and `override`n in `bau07B0_ip.cpp:214` (and `bau091A.cpp:269`).

`knx.bau()` returns `BauSystemB&`-compatible; on the real build the static type is `Bau07B0IP&`
(`bau07B0_ip.h:18`, `class Bau07B0IP : public BauSystemBDevice`), which inherits all 18 from `BauSystemB`.
**The shim's bau class must expose all 18 with these exact signatures** (this is the transport seam —
implementations are backed by a KNXnet/IP tunnel, out of scope here).

```cpp
// --- senders (return bool; connectionless request, reply arrives via a callback) ---
bool ftcSendCommand(uint16_t asap, const SecurityControl secCtrl, uint8_t objectIndex,
                    uint8_t propertyId, uint8_t* data, uint8_t length);           // bau_systemB.h:37
bool ftcSendDeviceDescriptorRead(uint16_t asap, const SecurityControl secCtrl);   // bau_systemB.h:41
bool ftcSendPropertyValueRead(uint16_t asap, const SecurityControl secCtrl, uint8_t objectIndex,
                    uint8_t propertyId, uint8_t count, uint16_t startIndex);       // bau_systemB.h:45-46
bool ftcSendPropertyValueWrite(uint16_t asap, const SecurityControl secCtrl, uint8_t objectIndex,
                    uint8_t propertyId, uint8_t count, uint16_t startIndex,
                    uint8_t* data, uint8_t length);                               // bau_systemB.h:50-51
bool ftcSendMemoryRead(uint16_t asap, const SecurityControl secCtrl, uint8_t number,
                    uint16_t memoryAddress);                                       // bau_systemB.h:53
bool ftcSendAdcRead(uint16_t asap, const SecurityControl secCtrl, uint8_t channelNr,
                    uint8_t readCount);                                            // A_ADC_Read (BCU bus voltage)

// --- connection-oriented (ETS-parity) scan shims ---
bool ftcScanConnect(uint16_t pa);                                                 // bau_systemB.h:57
bool ftcScanConnected();                                                          // bau_systemB.h:58
bool ftcScanReadAcked();                                                          // present once the device T_ACKed the CO read
void ftcScanReadDescriptor(const SecurityControl& sec);                          // bau_systemB.h:59
void ftcScanDisconnect();                                                        // bau_systemB.h:60

// --- callback setters (inline in the header; store a raw function pointer) ---
void ftcSetResponseCallback(void (*cb)(uint16_t pa, uint8_t objectIndex, uint8_t propertyId,
                    uint8_t* data, uint8_t length));                              // bau_systemB.h:38
void ftcSetDeviceDescriptorCallback(void (*cb)(uint16_t pa, uint8_t descriptorType,
                    const uint8_t* data));                                        // bau_systemB.h:42
void ftcSetPropertyCallback(void (*cb)(uint16_t pa, uint8_t objectIndex, uint8_t propertyId,
                    const uint8_t* data, uint8_t length));                        // bau_systemB.h:47
void ftcSetMemoryCallback(void (*cb)(uint16_t pa, uint16_t addr, const uint8_t* data,
                    uint8_t len));                                                // bau_systemB.h:54
void ftcSetAdcCallback(void (*cb)(uint16_t pa, uint8_t channel, uint8_t count, int16_t value));

// --- flow control ---
virtual uint16_t ftcTxQueueSize();  // base returns 0 (bau_systemB.h:63); Bau07B0IP overrides (bau07B0_ip.h:37)
virtual void ftcPacingRate(uint32_t deliveredBps, bool clean);  // host BBR-style pacing feedback (no-op on TP)
```

**Const/param exactness notes (do not drift — the source relies on overload identity):**
- `SecurityControl` is passed **by value** (`const SecurityControl secCtrl`) on all `ftcSend*`, but **by
  const ref** (`const SecurityControl&`) on `ftcScanReadDescriptor`.
- `data` on the two **senders** is `uint8_t*` (non-const); on the **callbacks** it is `const uint8_t*`
  for Dd/Prop/Memory but **non-const `uint8_t*`** for the Response callback (see §3).
- All five setters take a bare C function pointer (not `std::function`); the client passes `static`
  member functions (§3).

Call-site inventory (proof the shim needs no more than these 18): `knx.bau().ftc*` appears at
`FileTransferClient.cpp` lines 523, 592, 593, 735, 788, 845, 851, 1274, 1575, 1596, 1644, 1729, 1811,
1895, 1933, 1952, 2016, 2135, 2159, 2330-2332, 2363, 2379, 2388, 2415, 2535, 2561, 2582, 2966, 3001,
3032, 4094, 4158, 4183, 4186, 4192, 4203, 4583, 4590, 4613, 4652 — all resolve to the 18 above.

---

## 3. The five `ftcSet*Callback` callback types

These are **not named typedefs** in knx — they are inline function-pointer types in the setter parameter
and in the private member field. The shim must reproduce them byte-exact (member fields cited for the
canonical type):

| Setter | Member field (knx) | Exact pointer type |
|--------|--------------------|--------------------|
| `ftcSetResponseCallback` | `_ftcResponseCb` `bau_systemB.h:178` | `void (*)(uint16_t pa, uint8_t objectIndex, uint8_t propertyId, uint8_t* data, uint8_t length)` |
| `ftcSetDeviceDescriptorCallback` | `_ftcDdCb` `bau_systemB.h:179` | `void (*)(uint16_t pa, uint8_t descriptorType, const uint8_t* data)` |
| `ftcSetPropertyCallback` | `_ftcPropCb` `bau_systemB.h:180` | `void (*)(uint16_t pa, uint8_t objectIndex, uint8_t propertyId, const uint8_t* data, uint8_t length)` |
| `ftcSetMemoryCallback` | `_ftcMemCb` `bau_systemB.h:181` | `void (*)(uint16_t pa, uint16_t addr, const uint8_t* data, uint8_t len)` |
| `ftcSetAdcCallback` | `_ftcAdcCb` | `void (*)(uint16_t pa, uint8_t channel, uint8_t count, int16_t value)` |

**Where knx invokes them** (the transport seam must call these exact prototypes when a tunnel reply
arrives):
- `_ftcResponseCb(asap, objectIndex, propertyId, data, length)` — `bau_systemB.cpp:596` (from
  `functionPropertyStateResponseIndication`)
- `_ftcDdCb(asap, descriptortype, deviceDescriptor)` — `bau_systemB.cpp:634`
- `_ftcPropCb(asap, objectIndex, propertyId, data, length)` — `bau_systemB.cpp:652`
- `_ftcMemCb(asap, memoryAddress, data, number)` — `bau_systemB.cpp:676` (note: `addr`←`memoryAddress`,
  `len`←`number`)
- `_ftcAdcCb(asap, channel, count, value)` — from the A_ADC_Response decode (BCU bus-voltage read)

**Where the client defines & registers them** (the functions the shim's bau will call back):
- `ftcOnResponse` — decl `FileTransferClient.h:249`
  `static void ftcOnResponse(uint16_t pa, uint8_t objectIndex, uint8_t propertyId, uint8_t* data, uint8_t length);`
  registered `FileTransferClient.cpp:593, 788, 1644, 1729, 2016, 2135, 2159, 2331, 2966, 3001`
- `ftcOnDeviceDescriptor` — decl `FileTransferClient.h:251`
  `static void ftcOnDeviceDescriptor(uint16_t pa, uint8_t descriptorType, const uint8_t* data);`
  registered `FileTransferClient.cpp:523, 2330`
- `ftcOnPropertyValue` — decl `FileTransferClient.h:253`
  `static void ftcOnPropertyValue(uint16_t pa, uint8_t objectIndex, uint8_t propertyId, const uint8_t* data, uint8_t length);`
  registered `FileTransferClient.cpp:592, 2332`
- `ftcOnMemory` — decl `FileTransferClient.h:255`
  `static void ftcOnMemory(uint16_t pa, uint16_t addr, const uint8_t* data, uint8_t len);`
  registered `FileTransferClient.cpp:2535`
- `ftcOnAdc` — `static void ftcOnAdc(uint16_t pa, uint8_t channel, uint8_t count, int16_t value);` (bus-voltage read)

The response callback's "byte buffer + length + PA + object index" that the prompt asked about: the buffer
is `uint8_t* data`, length `uint8_t length`, PA `uint16_t pa` (the responder's source address, `asap` on
the knx side), object index `uint8_t objectIndex`, plus `uint8_t propertyId`.

---

## 4. The `knx` object surface (besides `bau()`)

`knx` is a global `KnxFacade<Platform, Bau07B0IP>` (`knx_facade.h:502` for RP2040/`KNX_TUNNELING`, `:521`
ESP32). The four files use **only two** facade members plus the two knx value-types:

| Symbol | Signature | Call sites |
|--------|-----------|-----------|
| `knx.bau()` | `Bau07B0IP& bau()` — template `B& bau()` at `knx_facade.h:119-122` | all §2 sites |
| `knx.individualAddress()` | `uint16_t individualAddress()` `knx_facade.h:215-218` | `FileTransferClient.cpp:558,1998,2050,4091,4152`; `FileTransferClientConsole.cpp:165` |

**Exact type `knx.bau()` returns:** `Bau07B0IP&` (`bau07B0_ip.h:18`). For the shim you do **not** need
`Bau07B0IP`; you need a host class (any name) exposing the 18 methods of §2, and `knx.bau()` returning a
reference to it. `knx.freeLoopTime()` is **NOT** used — every `freeLoopTime()` call is `openknx.freeLoopTime()`
(§5); the earlier-looking `knx.freeLoopTime` is the tail of `openknx.freeLoopTime` (verified:
`FileTransferClient.cpp:2270,3423,4249`).

**KNX value types used** (real home `knx/src/knx/knx_types.h`):
```cpp
enum DataSecurity { None, Auth, AuthConf };          // knx_types.h:225-230
struct SecurityControl { bool toolAccess; DataSecurity dataSecurity; };  // knx_types.h:232-236
```
The client only ever constructs `SecurityControl sec = {false, None};` (aggregate init) —
`FileTransferClient.cpp:734,842,1273,1574(secR),1595,1922,1942,2362,2377,2387,...`. The shim must define
both, with `None` visible as an unqualified enumerator.

**`OpenKNX::Module` base class** — `FileTransferClient : public OpenKNX::Module` (`FileTransferClient.h:107`).
Base chain `Module : Base` (`OpenKNX/Module.h:9`, `OpenKNX/Base.h:11`). Virtuals the client overrides (shim
must declare them `virtual`, matching signatures):
- `virtual const std::string name()` (`Base.h:35`) — override `FileTransferClient.h:113`
- `virtual const std::string version()` (`Module.h:21`) — override `:114`
- `virtual void setup(bool configured)` (`Base.h:47`) — override `:116`
- `virtual bool processCommand(const std::string cmd, bool diagnoseKo)` (`Module.h:102`) — override `:117`
- `virtual void showHelp()` (`Module.h:107`) — override `:118`
- `virtual void loop(bool configured)` (`Base.h:53`) — override `:119`

The client does **not** use `Base::log/logHex/logPrefix`, GroupObject hooks, or any ETS `Param*`/`Ko*`
macros (grep-verified: no `GroupObject`, `Param`, `Ko`, `.value(` references). So `Module`/`Base` can be
near-empty abstract shims.

---

## 5. The `openknx` object surface

`openknx` is a global `OpenKNX::Facade` (`OGM-Common/src/OpenKNX/Facade.h:44,103`). Distinct members used
(grep of all four files): `openknx.logger`, `openknx.console`, `openknx.freeLoopTime`.

### 5.1 `openknx.freeLoopTime()`
`bool freeLoopTime()` — `Facade.h:96`. Cooperative loop-budget gate. Call sites
`FileTransferClient.cpp:2270,3423,4249` (used in `while(... && openknx.freeLoopTime())` drains). Host shim:
return `true` (or a budget policy of your choosing). 5 textual hits, 3 live calls.

### 5.2 `openknx.logger` — `OpenKNX::Log::Logger` (`OGM-Common/src/OpenKNX/Log/Logger.h:118`)
Every distinct logger method the four files call (counts across both cpp files):

| Method | Exact signature (Logger.h) | Uses |
|--------|----------------------------|------|
| `logWithPrefixAndValues` | `void logWithPrefixAndValues(const char* prefix, const char* message, ...)` `:200` | ~84 |
| `logWithPrefix` | `void logWithPrefix(const char* prefix, const char* message)` `:197` | ~58 |
| `logWithValues` | `void logWithValues(const char* message, ...)` `:204` | ~9 |
| `log` | `void log(const char* message)` `:196` (also `const std::string&` `:195`) | ~4 |
| `color` | `void color(uint8_t color = 0)` `:212` | 6 |

`color()` argument values seen: `CONSOLE_HEADLINE_COLOR` (=33) `FileTransferClient.cpp:486`, `0`
(`:491,773,2271`), `32` (green) `:748`, and `e.color` (a `uint8_t` from `FtcOutLine`) `:2267`. Also
`FileTransferClientConsole.cpp:107` binds `auto &l = openknx.logger;` then calls
`l.logWithPrefixAndValues(...)`. **Not used:** `logHex*`, `logHeader`, `logDividingLine`, `indent*`,
`begin/end`, the `logInfoP`/`logErrorP` macros. The shim `Logger` needs just the 5 methods above
(printf-style variadics → forward to `vprintf`; `color()` may be a no-op or ANSI-SGR emitter).

### 5.3 `openknx.console` — `OpenKNX::Console` (`OGM-Common/src/OpenKNX/Console.h:33`)
| Method | Exact signature (Console.h) | Uses |
|--------|-----------------------------|------|
| `setLineSink` | `void setLineSink(void (*s)(const char*))` `:70` (guarded by `OPENKNX_FTC_CONSOLE`) | `FileTransferClient.cpp:2056` (`&consoleFeedLineStatic`), `:2098` (`nullptr`) |
| `printHelpLine` | `void printHelpLine(const char* command, const char* message)` `:73` | `FileTransferClient.cpp:4975` |

**Line-sink callback type:** `void (*)(const char*)` (`Console.h:41` field `_lineSink`, `:70` setter). The
client passes `FileTransferClient::consoleFeedLineStatic` — decl `FileTransferClient.h:429`
`static void consoleFeedLineStatic(const char* line);` (a trampoline to
`instance()->consoleFeedLine(line)`, `:428`). `setLineSink` is only referenced when `OPENKNX_FTC_CONSOLE`
is defined; if the shim leaves that flag off, omit it. `printHelpLine` is unconditional.

---

## 6. Constants / macros / enums / colors from the stack

**Good news:** every `FTC_*` / `CON_*` constant (object indices 159/160, all PIDs, all command ids,
timeouts, window sizes, colors) is defined **locally** in the two files — **not** pulled from knx. So the
shim owns none of them. Evidence: `FileTransferClient.cpp:143-286` (the whole constant block, e.g.
`FTC_OBJECT_INDEX = 159` `:143`, `CON_OBJECT_INDEX = 160` `FileTransferClient.h:421`, PIDs `:159-175`,
`FTC_DEV_PIDS[]` `:2369`). No APCI, property-access-level, or KNX security-level constant is referenced by
name.

Stack-owned symbols the shim **must** provide:

| Symbol | Value / type | Defined at | Used |
|--------|--------------|-----------|------|
| `CONSOLE_HEADLINE_COLOR` | `#define ... 33` | `OGM-Common/src/OpenKNX/Console.h:15` | `FileTransferClientConsole.cpp:35` (`const uint8_t H = CONSOLE_HEADLINE_COLOR`), `FileTransferClient.cpp:486` |
| `MODULE_FileTransferModule_Version` | string, e.g. `"0.1.6+2aa228e"` | generated `include/versions.h:30` | `FileTransferClient.cpp:41` (`version()` return) |
| `None` (enumerator) | `DataSecurity::None` (=0) | `knx_types.h:227` | all `SecurityControl{false, None}` inits |

Color literals `0`, `32`, `33` are passed as plain `uint8_t` to `logger.color()` — no named constants
beyond `CONSOLE_HEADLINE_COLOR`. `MODULE_FileTransferModule_Version_Major/Minor/Revision` are **not** used
by the four files (only `..._Version`), though the sibling `FileTransferModule.h` uses them.

---

## 7. Arduino / runtime symbols

| Symbol | Host stand-in? | Notes |
|--------|----------------|-------|
| `millis()` | **YES — shim** | `uint32_t millis()`, ~118 uses in `FileTransferClient.cpp`. Back with `std::chrono::steady_clock` (ms since start). Declared by the `Arduino.h` the real stack pulls; the shim must declare it (e.g. in `OpenKNX.h` or an `Arduino.h` shim). |
| `delay()` | **not needed** | **Zero** calls in both cpp files (verified). The design is non-blocking; no `delay()` stand-in required. |
| `String` (Arduino) | **not needed** | **Zero** uses. Only `std::string` (libstdc++). |
| `snprintf`, `memcpy`, `strncpy`, `strlen`, `strcmp`, `strncmp`, `sscanf`, `strstr`, `atoi`, `isdigit`, `vsnprintf`, `va_*` | **libc, as-is** | ~129 snprintf/memcpy/strncpy/strlen in the cpp; console uses `sscanf`/`strcmp`/`strstr`/`atoi`/`isdigit`. All standard. |
| `std::string`, `std::vector` | **libstdc++, as-is** | `<string>`/`<vector>` included directly (`FileTransferClient.cpp:14-15`). |
| `File`, `FSInfo`, `LittleFS` | **YES — LittleFS.h shim** | §1.3. |

No `print()`/`println()` (the knx facade console) are used — verified zero hits.

---

## 8. `FtcFileSource` / `FtcFileSink` (verbatim) + backend registration

Reproduced exactly from `FileTransferClient.h:27-52`:

```cpp
// FileTransferClient.h:27-32
struct FtcFileSource
{
    int32_t (*open)(const char *path); // returns file size, or -1 on failure
    uint8_t (*read)(uint32_t offset, uint8_t *buf, uint8_t len);
    void (*close)();
};

// FileTransferClient.h:36-41
struct FtcFileSink
{
    bool (*open)(const char *path);                 // create/truncate; false on failure
    int (*write)(const uint8_t *buf, uint16_t len); // append; bytes written, or -1 on error
    void (*close)();
};

// FileTransferClient.h:45-52
struct FtcBackend
{
    const char *prefix; // "" = default (matched when no named prefix fits); "sd" / "efc" = named
    FtcFileSource src;
    FtcFileSink sink;
    bool (*available)() = nullptr;     // null => always available; else gate open() on it (card in / mounted)
    uint64_t (*freeBytes)() = nullptr; // null => skip the pre-write space check; else local free bytes
};
```

These are **project-owned** structs (no shim), but the ftc-cli backends plug into them. Registration API:
`void registerFileBackend(const char* prefix, const FtcFileSource& src, const FtcFileSink& sink,
bool (*available)() = nullptr, uint64_t (*freeBytes)() = nullptr)` — decl `FileTransferClient.h:124-125`,
def `FileTransferClient.cpp:44-54`, bounded to 4 slots (`_backends[4]`, `FileTransferClient.h:603`).

**Built-in backend the files self-register:** the default `""` (LittleFS) in `setup()`
(`FileTransferClient.cpp:131-136`, backed by `littleFsOpen/ftcSharedRead/ftcSharedClose` +
`littleFsSinkOpen/ftcSharedSinkWrite/ftcSharedSinkClose` + `littleFsAvailable/littleFsFree`,
`:105-127`). For the host this means: with the `LittleFS.h` shim of §1.3, the default backend "just works"
against the host filesystem — no extra ftc-cli backend is strictly required to run.

**Prefixes the resolver recognizes** (`ftcResolveBackend`, `FileTransferClient.cpp:57-81`): a named prefix
matches when `path` starts with `prefix` **and** the next char is `'/'` (e.g. `"sd/x"` → backend `"sd"`,
stripped `"/x"`); `""` is the default fallback (unstripped). Named backends `sd/` (SD) and `efc/`
(ext-flash) self-register **from other modules** (`FileTransferClient.h:44-52,122-123` comments) — those
modules are **not** in the four-file scope, so on host only `/` (LittleFS/default) exists unless the
ftc-cli registers its own `sd`/`efc`. The `test` source is not a backend prefix; it is the `_ftcTestSource`
generated RAM pattern path (`FileTransferClient.h:321`), selected internally by perf, not via prefix.

`extern FileTransferClient openknxFileTransferClient;` (`FileTransferClient.h:617`) is **defined inside the
compiled set** at `FileTransferClient.cpp:4986` — the ftc-cli does **not** need to provide it.

---

## 9. Shim sizing & red flags

| Shim header | Est. size | Difficulty | Notes |
|-------------|-----------|-----------|-------|
| `OpenKNX.h` (umbrella) | ~40-80 LOC | Easy | Just `#include`s the sub-shims + the `CONSOLE_HEADLINE_COLOR` / `MODULE_FileTransferModule_Version` macros + pulls `millis()`. |
| `Arduino.h`/runtime (or folded into umbrella) | ~10 LOC | Trivial | Only `millis()`. No `delay`, no `String`. |
| `knx` facade + `SecurityControl`/`DataSecurity` + bau class | ~60-120 LOC | **Medium — the real work** | The 18 methods of §2 are the transport seam (KNXnet/IP tunnel). Signatures are flat POD/function-pointer — **no knx templates, no deep types** leak into the four files. `knx.bau()` returns a plain host class ref. |
| `OpenKNX::Module`/`Base` | ~30 LOC | Easy | 6 abstract virtuals (§4); no `GroupObject`, no `Param*`. |
| `OpenKNX::Facade` (`openknx`) — Logger + Console + `freeLoopTime` | ~60-90 LOC | Easy | 5 logger methods (variadic → vprintf), 2 console methods, 1 bool. |
| `LittleFS.h` (`File`/`FSInfo`/`LittleFS`) | ~50-80 LOC | Easy-Medium | `std::fstream` backing; watch the `explicit operator bool` on `File`. `FSInfo` branch is dead on host. |

**Overall verdict: byte-identical compile is FEASIBLE. No blocker symbol.** The four files were written
against a deliberately narrow slice of the stack (flat integer/pointer types, function-pointer callbacks,
locally-owned constants). Specifically:
- **No heavy/templated knx type crosses the boundary.** `knx.bau()` is used only to call the 18 flat
  methods; `SecurityControl`/`DataSecurity` are two trivial PODs. The real `KnxFacade<P,B>` template does
  **not** need reproducing — the shim's `knx` is a hand-written object.
- **No macro pulls in the world.** `OPENKNX_FTC`/`OPENKNX_FTC_CONSOLE` are simple gates; the logger
  `logInfoP`-style macros that *would* drag in `logPrefix()` are **not used** by these files.
- **The only "Arduino-ish" surface** is `millis()` + the `File`/`LittleFS` trio — all small.

**Minor watch-items (not blockers):**
1. `File::operator bool` must be usable in `if(!f)` and `(bool)f` contexts (`FileTransferClient.cpp:91,100,108,114`).
2. Keep the `SecurityControl` by-value vs `const&` (`ftcScanReadDescriptor`) distinction and the
   `data` const-ness split (§2) exact, or overload resolution / pointer-type match on the callbacks breaks.
3. The `_ftcMemCb` invocation maps `len`←`number` and `addr`←`memoryAddress` (`bau_systemB.cpp:676`) — the
   transport seam must feed the memory callback with those roles.
4. Do **not** define `ARDUINO_ARCH_*`; if you do, the RP2040 `FSInfo` branch (`FileTransferClient.cpp:119`)
   activates and you must then also shim `FSInfo`.
5. `MODULE_FileTransferModule_Version` must be a string literal (used as a `std::string` return,
   `FileTransferClient.cpp:41`); give it any value (e.g. mirror `include/versions.h`).

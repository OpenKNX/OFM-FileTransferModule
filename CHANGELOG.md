# Changes


## 2026-10-03 -- the scan reads every device by default; the parallel range scan is gone

* `scan` now reads each found device's identity by default. `--no-details` turns that off, `openknx` narrows it to the System B candidates, and `--workers N` / `-W N` says how many of those reads run at once (5). `details` is still accepted and does nothing
* The parallel range scan (`--tunnels N` / `scan … fast`) is removed. Measured over three interleaved blocks of three runs each, it returned a different set every time -- 47 to 60 of 49 to 68 addresses moved between runs, against 1 to 5 for the sweep -- while the serial sweep found every address the connection-oriented scan found and 29 more. Both spellings now exit 1 with a message instead of being silently ignored
* A scan names the KNXnet/IP devices on the network and the block of tunnel addresses each one owns, so an address that is a tunnel slot is shown as one instead of as a silent device. A device that answered with its own mask outranks that list: the list is configuration, the answer is evidence
* `-q` under `--no-details` printed the full decorated table. The quiet branch sat inside the `if (pool)` block, and `pool` is null exactly when no identity read runs
* The state column is filled for a device with interface objects too: it shows the load state of the application program where a device has one and the BCU run state where it has not
* Both device pickers read only the first character of the selection, so after `alle` row 12 silently resolved to row 1 -- a different device, which was then the knxOTA update target
* The silence watchdog added yesterday is confined to a monitor that drives its own loop. `gm compare` drives two monitors from its own full-screen loop, where a re-dial would have been recorded as a tunnel loss on the side under test
* The tunnel keep-alive runs only from `pump()`, which did not run while the network map was built (1.5 s plus 1.2 s per device). On an installation with many KNXnet/IP devices that window outlasted the 60 s heartbeat
* `unload`'s read-back stored an answer by the loop's current index without checking which object it came from, so a late answer for the address table was reported as the association table's state
* `ga table <verb>` without `--ip` listed the stored tables and exited 0 for every verb it cannot do offline, including `move`
* A `FtcGaPtr` read that timed out was reported as "this table does not exist" instead of "this table was not read" -- `FtcGaRef` sets `_gaTruncated` on the same path, `FtcGaPtr` did not
* `info ga` returned 0 for a table it had itself marked as a prefix; the exit code now follows the truncation flag, not only the pump cap
* `FtcGaPtr` was the one chained memory read with neither a retry nor a cleared duplicate window, so a single lost pointer answer ended the table. It now retries `FTC_GA_RETRY` times and clears `_ftcRespT` before each send, like `FtcGaMem` and `FtcGaDesc`
* `scan openknx` compared the mask as `& 0xFFF0`, which keeps the medium nibble, so it matched TP1 System B only. A device reporting `0x57B0` ("TP1/IP System B device") was never asked who it is, never got the OpenKNX mark and never reached the knxOTA picker. The test is medium-agnostic now
* `--no-details` did nothing when combined with either `details` or `openknx` -- the first set the flag before it ran, the second built the pool on its own. An explicit "off" now wins over both
* The load state travels as a number. The child process emits `app_state_id` beside the translated `app_state`, and the state column and the quiet scan field are rendered from the number, so neither depends on the child's locale
* A load state no longer counts as "this address answered", so a device that returns its load state while its order number and serial time out still gets its one retry
* `ga table` had two listings, offline and online, and only the offline one spoke the quiet protocol. The same command printed a tab record without `--ip` and a decorated line with it
* The `unload` read-back accepted a `PropertyValue_Response` from any device on the line; it now also checks the source address
* `info ga` exited 1 for a device with no group objects at all. Zero objects is a complete read -- an unloaded device, or an application without KOs -- so the exit code now follows only the truncation flag
* A tunnel slot the interface did not hand out was printed as "busy" whatever the reason. Only `E_NO_MORE_CONNECTIONS` means the interface is full; an unanswered probe now reads "unknown", and the panel says that a probe went unanswered instead of attributing the slot to somebody else

## 2026-10-02 -- "no group address" and "unknown" are decided per row

* `?` ("the address table could not be read") was a RUN-wide flag but printed on every empty row, so an object that legitimately has no group address -- TSAP 0 is the device's own address -- was reported as unreadable. It is a bit in `FtcGaEntry` now, set where the row is built, and `groupAddressesMissing()` is gone with the flag. The row stays 8 bytes: `cfgValid` and the new bit share one octet as bitfields
* `oknx` groups its rows by KO, so it folds the bit over a KO's associations: an object whose resolved addresses are complete prints them, and one with an unresolved association prints `?` behind them -- "and at least one more we could not read", instead of claiming the list is whole
* The knxOTA web page printed `0/0/0` for a row without an address -- an address that does not exist. It prints `—` or `?` now, from the new per-row `unk` field in the status JSON (the object-level `unknown` field is gone; nothing read it)
* The duplicate-answer guard stamped its timestamp before the anchor check ran, so a late or mirrored telegram for a different address could push the real answer that followed it into the 12 ms window and the read fell to a timeout. The anchor check runs first in all three memory states (`FtcGaPtr`, `FtcGaMem`, `FtcGaDesc`)
* Two chained table reads did not disarm that guard when they sent the next chunk, unlike the five other chained reads in the file. With the anchor in front of it the guard could then only ever hit a legitimate answer: on a link whose round trip is under 12 ms every second chunk was discarded and cost a 6 s timeout
* `_gaAddrRead` counted a table as fully read when the entry index reached the declared count -- but the index advances for an entry the row cap threw away too. A table whose last chunk crossed `FTC_GA_MAX` therefore reported "this object has no group address" for addresses the client itself had discarded


## 2026-10-02 -- the group-comm report no longer blocks the device loop

* The report is printed a slice at a time (8 rows per `loop()` pass) instead of queueing the whole thing at once. With `FTC_GA_MAX = 300` that burst was ~39 KB of queued lines plus ~300 `vsnprintf` in a single pass on a device with 264 KB; it is ~1 KB now. `loop()` drains the queue before it runs the state machine again, so the slices keep it short by themselves
* The row limit was deliberately NOT lowered: the knxOTA web page reads the same `_gaObjects` array and would have lost rows with it
* The bus is released BEFORE the report is printed. Splitting the report had deferred the `T_Disconnect` across the whole printing, which can take seconds on a slow console — holding a foreign device in a point-to-point connection that dies after ~6 s of silence anyway
* `oknx info ga` had a 60 s absolute cap on a walk that is hundreds of round trips. It now gets 5 minutes, and if the cap does end a running read, the result is reported as a prefix with a non-zero exit code instead of being printed as the device's complete table
* The knxOTA web page says when a read was cut short. It showed a partial table exactly like a complete one, which is the failure the incompleteness marker was built for; the console report now also prints that marker FIRST, before "no group objects"
* Chained memory reads no longer look like duplicates of each other: on a fast IP link the answer arrives inside the 12 ms duplicate window and was being dropped, costing three timeouts per chunk
* `FTC_MEM_BUF` 32 → 48. It was exactly the length of the BCU identity block, so a device answering more octets than asked for on the last chunk had the whole block silently time out

## oknx 1.0.5 -- the help reads like the job now: 2026-10-02

* Violet is reserved for `<pa>`, the device a line acts on. The command word used to get the same colour, which said "ga is a device" and undid the one thing this tool has to teach; commands are bold now, and a bare word after an option is its value, not a verb
* The USAGE block says out loud that its colours are the legend for every line below -- and only when colour is actually being printed, so a piped or `NO_COLOR` run does not claim colours it did not use
* `<pa>` is spelled out as the individual address, the tunnel is explained in one line, and both addresses stand next to each other once, labelled: `-i 192.168.1.50` is the way, `1.1.42` is the target. Confusing those two is the most common mistake with this tool
* `--discover` is the fourth call shape, named as step 1, and `oknx --examples` is pointed at from the top instead of only after 270 lines
* The firmware section is called FIRMWARE AKTUALISIEREN and sits before the transfer options; `retry` moved to the transfer options, where it belongs
* `--quiet` exists twice with two meanings 90 lines apart; the transfer-local one now says so. One word per thing throughout: Vorgabe (not Default/Standard), Laufwerk (not Drive), "keine Deko" (not "kein Chrome")
* `--ascii` says that it swaps glyphs and leaves colour alone, and names what does turn colour off
* `--examples` is built around whole jobs instead of single commands: commissioning a new device, replacing one, "why does this lamp not switch", watching the bus, files, firmware. `knxota --check` is the first firmware line, because it is the first thing a careful installer does
* Seven more commands and switches that were in the dispatch but in no help line: `knxota resume`, `delta make|show|apply` with `--pack` and `--limit`, `--ui-demo`, `--dry-run`, `--force-install`, `--browse` / `--file-browser`, and the `_conprobe` measurement aid. Audited against the source: 41 options, 0 undocumented


## oknx 1.0.4 -- programming an address, unloading a device, framed help: 2026-10-02

* `setpa <x.y.z>` is the ETS "programme the individual address": it addresses nobody, it picks the one device in PROGRAMMING MODE. Broadcast A_IndividualAddress_Read first, refuse unless exactly one device answers, write, then read the new address back as the proof. Verified on the rig: 5.0.3 -> 5.0.99 -> 5.0.3, each step confirmed on the bus
* `<pa> unload yes` writes LE_UNLOAD (4) to PID_LOAD_STATE_CONTROL (5) on the address table, the association table and the application program, then reads every load state back and reports success only when all three are UNLOADED. A terminal asks again on top of the `yes`, because only an ETS download undoes it. Verified on 5.0.3: all three states 01 -> 00, checked with a separate `busprop read`
* The tunnel can address a broadcast at all now (destination 0 with the group address type), and delivers broadcast traffic to its own callback instead of mistaking it for a group value
* `<pa> restart` and `<pa> masterreset` now run over a connection. 06 Profiles 4.2 p.37 makes the connection-oriented restart mandatory for every profile while the connectionless one is optional everywhere, so a device that implements only the mandatory form used to ignore the request while the tool reported success
* `ga write` encodes the value through the stack's own converter when the datapoint type is known, so the telegram form follows the TYPE's length as 03_03_07 3.1.3 requires, instead of following how large the typed number happened to be. `ga read|write 0/0/0` is refused: 0000h is the broadcast address, not a group address
* `<pa> led` is renamed in the help to what it does: it sets PROGRAMMING MODE (PID 54, 03_05_01 4.3.5 p.45). While it is on, any tool on the bus can overwrite that device's address. `<pa> progmode` is the same command under the name of its effect
* `--help` is drawn as one frame per section with four columns -- command, short form, operands, text. The column grid is measured once over the whole help, so the text column starts in the same place in every frame, and the frame follows the terminal between 150 and 168 columns. Below 150 the rows stack as before
* DPT 9.028 was labelled m/s; it is km/h (03_07_02). 9.009, 9.010 and 9.011 had no unit and now carry theirs
* `gm` shows the group address and its name in two columns, so the names no longer shift with the address length; the name column follows the terminal width and a longer name is cut


## oknx 1.0.3 -- the project's own names and datapoint types: 2026-10-02

* `ga import <export>` reads an ETS group-address export and remembers it, so the export file is only needed while importing. A fresh import replaces the table, because an export is the full picture and an address deleted in ETS has to disappear here too
* All four output formats of the ETS export dialog are read, verified against real exports of one project: XML, XML (ETS4), CSV (layouts 1/1, 1/3, 3/1, 3/3) and CSV (ETS3), with tab, comma or semicolon. The six that carry a datapoint type produce byte-identical tables; the two that do not -- XML (ETS4) and CSV (ETS3) -- are imported and reported as carrying none, instead of leaving a table that silently decodes nothing
* A CSV without header lines is refused and says so: without a header there is nothing to key the columns on, and reading it positionally would also swallow the first address
* The table is kept per interface, which is the separation that already exists: the test rig and the productive line are reached through different interfaces, so `-i` alone picks the right project. `ga table` lists them, `ga table move <from> <to>` re-points one after an interface changed its address, `ga table rm` drops one
* `gm` and `ga monitor` now show the group address name and the decoded value: `0/7/0 helligkeit [Write] 142.88 lx` instead of `1C 5D`. Decoding uses the stack's own converter (`lib/knx` dptconvert), so a value cannot read differently here than in the device; verified against ETS on 9.004 (`1C 5D` -> 89.36 lx)
* `gm 0/7/0:9.004` gives the type for one run without importing anything, and `gm 0/7/0,0/7/1,1/4/5` watches several addresses at once
* An address the table does not know keeps showing its raw octets. The datapoint type is on neither the bus nor the device -- two bytes is `9.004 Lux` exactly as much as `7.001 pulses` -- so it is never guessed from the payload size
* `oknx --examples` is a new page of worked examples grouped by the job. `--help` keeps the reference and points to it; its own example block is gone
* `--help` was reorganised by what a command acts on -- the interface itself, group addresses, watching the bus, finding devices, reading one device, driving one device -- instead of spreading device commands over four sections. Six commands were in the dispatch but in no section at all: `ga import`, `ga table`, `prop dump`, `busprop dump`, `browse`, `config`
* Every command row is now coloured by the roles the USAGE block announces as a legend -- subject violet, verb bold, operands teal, options blue. The split is made from the characters, so a command added later is coloured right without anyone remembering to say so
* Despite six more commands the help is shorter than before (271 lines instead of 277)
* Host only. The device firmware compiles none of this and is byte-identical (RP2040 1150800 B before and after)


## oknx 1.0.2 -- device restart and group addresses: 2026-10-02

* `<pa> restart` reboots any KNX device (A_Restart, basic). It is unacknowledged by definition, so the tool says so instead of claiming a confirmation. Verified on 5.0.3: the device's RAM-held login window is gone 14 s later, which an idle timeout cannot explain
* `<pa> masterreset <code> yes` sends a master reset. The erase code is given by NAME, because a mistyped digit is not recoverable over the bus, and `yes` is required. `--help` says what each code erases: `confirmedrestart` nothing, `resetparam`, `resetlinks`, `resetap`, `resetia`, `factoryresetwithoutia`, `factoryreset`
* The master reset is the only restart the standard has answered, so its answer is awaited for 3 s and the device's error code reported. The response carries its own bit in the APCI octet, so its opcode on the wire is 0x3A0/0x3A1 and not the request's 0x380/0x381 — matching the request opcode made the decoder unreachable and reported every master reset as unanswered. Verified on 5.0.7: error code 0, process time 10 s
* `ga read <x/y/z>` sends A_GroupValue_Read and lists every answer with its source. Flag-consistent on the test rig: an address whose object carries the read flag answers, one without it stays silent
* `ga write <x/y/z> <value>` sends A_GroupValue_Write. A plain 0-63 rides in the APCI octet, hex octets go as a payload, and a plain value above 63 is refused rather than silently meaning something else. Both forms confirmed on the wire by a second monitor
* `gm <x/y/z>` is the group monitor restricted to one address, `ga monitor <x/y/z>` the same thing spelled the other way. It is the existing monitor with a filter, not a second viewer, so the keys, the export and the decoding are unchanged
* The tunnel can now address a group at all: its cEMI builder had the destination address type nailed to "individual"

## oknx 1.0.1 -- group objects of BIM M112 devices: 2026-10-02

* `info ga` reads the group-object descriptors of mask 0x0700-0x0705 devices, so flags, priority and object size are shown there instead of `?`. The descriptor is the classic one with a 2-octet value pointer: `[count:1][ram-ptr:2]` then 4 octets per object. Derived from an ETS busmonitor of 1.1.30 and confirmed against ETS on every object of 1.1.30, 1.1.161 and 1.1.12
* Those devices publish no `PID_TABLE_REFERENCE` for that table -- they answer obj 3 PID 7 with nr_of_elem 0 -- so the walk falls back to the fixed base 0x4400 that ETS uses, but only after asking, and it checks the table header before taking a flag from it
* A table whose declared length is zero, or whose header does not hold up, is reported as incomplete instead of ending silently with a success result
* The first read of any device table fetches the header alone; a blind full chunk over-reads a short table and some devices then answer nothing at all
* Group-object flags of a device that answers more octets than were asked for are no longer reported as a truncated read -- the surplus is re-requested, nothing is lost


## ec/v1dev -- `oknx`, legacy servers, hardening: 2026-09-29

**Renaming of the PC tool -- breaking, without an alias**

* Breaking: the desktop tool `ftc` is now `oknx`. There is no alias and no migration, so run `ftc uninstall` with the OLD binary BEFORE installing the new one -- nothing removes it later
* The device console command stays `ftc`: what is typed ON a device (`ftc 5.0.3 send …`, `ftc <pa> con`) is unchanged, and so are the protocol name FTC, the `ftc::` namespace, the `FTC_*` / `FTM_*` / `OPENKNX_FTC_*` names and the `ftc*` methods of `knx.bau()`
* The source directory `ftc-cli/` is now `oknx/`, the PlatformIO environments are `oknx-<os>-<arch>` (e.g. `pio run -e oknx-linux-x64`) and the built binary is `oknx` resp. `oknx.exe`
* A product release carries the tool under `Tools/oknx/<OS>/<arch>/` instead of `Tools/ftc-cli/<OS>/<arch>/`
* Configuration is read from `~/.config/oknx/config.toml` resp. `%APPDATA%\oknx\config.toml`, monitor exports are written to `~/.local/share/oknx` resp. `%LOCALAPPDATA%\oknx`, and `oknx install` puts the binary in `~/.local/bin` resp. `%LOCALAPPDATA%\Programs\oknx` -- the directories under the old name are neither read nor deleted
* The language of a shell is pinned with `OKNX_LANG=de|en`; `FTC_LANG` is no longer read. Resolution order: `--lang` > the stored `config lang` > `OKNX_LANG` > the operating system > `LC_ALL` / `LC_MESSAGES` / `LANG` > English
* Entries below this one name `ftc-cli` and `ftc`, which is what those releases shipped

**Older FTM servers are usable instead of being refused**

* Feature: a server from 0.0.4 onwards is driven with the feature set it actually answers, instead of being reported as too old. What it cannot do is named on the line rather than failing silently
* Fix: a one-octet answer to `ModuleVersion` is a STATUS, not a version. The length guard ran first, so a reachable device whose access stage refuses writes was reported as "older than 0.0.3" instead of naming the refusal it gave
* 0.1.5 and 0.1.6 cannot be told apart by version alone, so the console tunnel is reported as unproven rather than absent
* `fast` falls back to `classic` with the reason on the line when the server does not offer it
* Measured against FTM 0.0.4 and 0.1.x on 5.0.8 and 5.0.3

**Hardening suite**

* Fix: F-N-5 abandoned `ftc <pa> ll` after 2000 ms while the pre-flight allows the same command 25000 ms, so the tail of the listing was charged to the next probe as that probe's own latency. The number tracked how many files the directory held -- 1240 ms with leftovers, 375 ms against an empty one -- not device health
* The case now lets the listing finish and judges the return to the loop; its title says so. Responsiveness DURING a listing is not measurable here at all, because probe and listing share one serial console
* `Measure-FtmResponsiveness` returns the per-probe gaps, so a failure can be read from the report instead of being inferred across twelve runs
* `Remove-FtmArtefact` also removes directories: `rm` leaves one behind and the next run fails on it

## ec/v0.2.0-beta.1 -- third batch: 2026-09-15

The tag was moved from `873ce01` to the head of this batch, so everything below is part of it.

**Documentation checks**
* Feature: `scripts/Test-DocCurrency.ps1` holds the documentation against the sources -- every `FTC_`, `FTM_`, `OPENKNX_` and `KNX_` name written in backticks must exist, and a documented value must match the constant. It reads `#define NAME 42` as well as an initialiser, without which the whole value check was inert for the build switches it targets
* Feature: `scripts/Test-DocLinks.ps1` resolves the relative links between the documentation files and reports the dead ones, plus file names that differ from the target only by case -- those resolve on macOS and Windows and 404 on GitHub
* Both exit 0 on a healthy tree with the neighbouring module roots passed, carry the OpenKNX header with working comment-based help, and a byte order mark so Windows PowerShell 5.1 does not read them as ANSI

## ec/v0.2.0-beta.1 -- second batch: 2026-08-29

The tag was moved from `f489fcf` to the head of this batch, so everything below is part of it. Main additions:
firmware update as a difference instead of a whole image, a knxOTA page in the device's own web
interface, and a repaired `fast` mode.

**Delta firmware update**
* Feature: firmware can be sent as a difference to the running image (`.okd`, magic `OKD1`) instead of a full image — a 1.8 MB image takes about 78 min over the bus at 400 B/s, a typical 45 KB difference about 2 min
  * the rebuild runs in `loop()` in slices no larger than one flash sector; nothing is made bootable before the rebuilt image has been checksummed, so every abort leaves the device on the firmware it is already running
  * the client probes first (`FwProbe`, cmd 106) whether the target runs the image the patch expects, and reports the reason when it does not
  * build switches follow what is present instead of being set by hand; a failure is reported rather than silently skipped
* Feature: compressed full images — a gzipped image is unpacked straight into the ESP32 OTA slot through the inflater in the chip's mask ROM (about 88 min down to 54 min); the RP2040 bootloader already unpacks, so the switch has no code there and the build refuses it
* Feature: ESP32 OTA-slot safety guard — a single-app partition layout has no second slot, which is now checked before writing and reported through `CheckFeatures`

**knxOTA**
* Feature: a scan names what it found -- the FULL probe keeps order number and version per hit, so the list shows the device instead of only flagging it as OpenKNX. The identity comes off the bus and is escaped before it reaches the page
* Feature: the search button starts and cancels the sweep; the separate stop button is gone, and the button stays clickable while the sweep runs on the device
* Feature: the scan status reports whether the sweep ran to its end and keeps the last progress, so a page opened or reloaded afterwards still learns the outcome
* Feature: sweep count, timeout and pace are taken from the request and clamped, so a hand-typed value cannot stall the sweep
* Fix: the scan denominator is the range times the passes -- a two-pass sweep reported "389 of 255 checked", and skipped addresses now count as probed so the bar reaches its total
* Fix: a sweep started from the console no longer appends its hits to the list of the previous one
* Feature: knxOTA web page (`OPENKNX_FTC_KNXOTA_WEB`) — pick a target PA, read what the device is, send a firmware or a difference from this device's flash, SD or external flash over KNX, trigger the update, or measure throughput. The page is a front-end onto the embedded client, so no PC and no console are in the chain. 34732 B flash + 80 B RAM on RP2040, 40056 B + 64 B RAM on ESP32
* Feature: an unfinished knxOTA run can be resumed instead of restarted
* Fix: a refused firmware apply is reported instead of being announced as triggered — `FwUpdate` answers nothing on success but `0xA0`/`0xA2` when the security gate refuses it, and that answer was discarded
* Feature: knxOTA assistant command in `ftc-cli`, with a reachability probe and `--check`/`--force`

**Transfer**
* Fix: `fast` throughput repaired — payload degrade, window regulation and the host pacer worked against each other, so the negotiated window collapsed under load
* Feature: the negotiated mode is reported, including why `fast` was denied, instead of silently falling back to `safe`
* Feature: downloads are CRC-verified, and `FwUpdate` is refused while writes are disabled
* Feature: a device scan paces itself, so a scan no longer floods a busy bus
* Change: `perf` and the transfer commands share one option grammar (`FtcXferOptions`), so `w<N>` and the mode tokens mean the same thing everywhere

**ftc-cli (desktop client)**
* Feature: live A/B fidelity compare of two KNXnet/IP monitors (busmon or group monitor), for checking one interface against another
* Feature: self-install and self-uninstall of the binary, with no external dependencies
* Feature: host core modules — scan, describe, features, reachability, UF2 and ESP image handling, host filesystem, access
* Feature: the window regulation is visible, every run is reported, and a write is confirmed before it happens
* Fix: the tunnel ACK guard frees the in-flight slot only on a full, in-channel ACK, so a truncated or foreign frame cannot free it
* Fix: the plain monitor path honours the `L_Busmon.ind` framing rules
* Fix: the client notices when the webconsole peer stops answering, instead of waiting out the timeout
* Fix: directories are created without a shell, so a path with quotes or metacharacters cannot inject
* Fix: the real abort reason is reported, and an elevated FTC priority is confirmed
* Fix: the console link state reflects the target's answer, not just the state of the tunnel
* Change: CoreFoundation is linked on the macOS host; report directories are ignored

**ETS and documentation**
* Feature: German context help ships as an ETS baggage
* Doc: the German documents are replaced by an English set — README, ARCHITECTURE, PROTOCOL, CONSOLE, SECURITY, THROUGHPUT, DELTA, FLAGS, CONCEPT-defines
* Doc: `FLAGS.md` documents `OPENKNX_FTC_KNXOTA_WEB` with its measured flash and RAM cost
* Doc: the throughput chapter drops the 450-544 B/s crash cliff — that was an intermediate finding; the reboot needs an artificial unpaced tunnel flood, not a normal transfer
* Fix: the ETS help text calls access stage 0 "Blockiert", matching the parameter enum

**Tests**
* Test: PowerShell hardening suite over the FunctionProperty RPC surface
* Test: response-matrix and state-machine suites extended — every command against every server response per drive and async state
* Test: PowerShell tooling for the delta update path

**Device info**
* Fix: the device error-code read is gone -- the client asked for PID 24 on the device object, which is not the error-code property (property.h names PID_ERROR_CODE 28), and no device object in the stack registers PID_ERROR_CODE either
* Fix: that read spent the 800 ms optional-property probe on every device-info query and never produced a value; the row is dropped from the web status, the knxOTA page and the ftc-cli report

**Documentation**
* Feature: QUICKSTART, FIRMWARE-UPDATE, WEB, FTC-CLI, SCRIPTS and INTEGRATION added; README is the index and names the audience of every document
* Change: the superseded ftc-cli wire-protocol and host-shim documents are removed; the module doc set carries their content
* Doc: the module README is rewritten -- what the module carries besides files, knxOTA end to end including why a delta update turns half an hour into minutes, the three front ends as one state machine, the measured throughput and its cause, every build switch and both profiles
* Doc: the ftc-cli README gains knxOTA, install/uninstall, retry, prio, logging, the busmon A/B comparison and the programming LED, and points at doc/PROTOCOL.md instead of the shim documents that were removed
* Doc: the example addresses are marked as an isolated lab VLAN, with the note that 11.0.0.0/8 is publicly allocated space rather than an RFC 1918 range
* Fix: a download counter of 0 no longer claims the device was never programmed -- the stack keeps PID_DOWNLOAD_COUNTER in RAM and leaves persisting it to the product, so a device without persistence reports 0 after every restart, and an ETS download ends in one
* Fix: the `[Rr]elease/` ignore rule also matched `scripts/release/`, so the release hook was absent from every clone; the hook that places the ftc-cli binaries into a product release is now versioned
* Note: `errorcodes.txt` records that 0x01..0x04 are listed as LittleFS errors while the server also uses 0x01..0x03 as per-command status bytes -- the two readings conflict and are not reconciled

## ec/v0.2.0-beta.1: 2026-08-09

The complete OpenKNX FileTransferModule (FTC — file transfer, FW-update, console tunnel and access control
over cEMI/KNX). One shared client core drives **both** the on-device `ftc` console command (embedded CLI)
and the native desktop **`ftc-cli`** (mac / win / linux) — the same transfer + protocol code, compiled for
the device and for the host. Not product-specific: it runs on any OpenKNX device. This entry covers
everything since the 0.1.5 baseline (`178f186`). Every optional feature is **on by default** (opt-out); the
build switches live in `FileTransferConfig.h`. Build- and 3-agent-expert-review-verified; HW-test pending.

**File transfer (server + on-device client)**
* Feature: FTC server — fast transfer, filesystem-info, console tunnel and ESP32 self-apply; the on-device `ftc` console client (PA → PA to another OpenKNX device's FTC server).
* Feature: device client + module — SD / external-flash directory backends, fast/safe windowing, busy-patience under a congested bus, a KNX device-map.
* Feature: pluggable directory-listing backends for non-LittleFS roots (`sd/`, `efc/`).
* Feature: interface-APDU auto-framing + delivery-rate pacing feedback + OGM-Common console compatibility shim.
* Feature: tunable console drain + download package size + upload auto-degrade for small tunnels.
* Fix: raise the package ceiling to the spec-legal 254 (host guard 251) + doc sync.

**Transfer resilience**
* Feature: resilient transfers — a size-scaled stall/wedge backstop, auto-resume, download-resume (whole-chunk boundary, CRC-cooperative), a smart AUTO window (Discover & Lock), and host tunnel auto-reconnect (reconnect-then-resume). HW-verified: a mid-transfer interface reboot recovers to a CRC-verified file.
* Change: dropped the experimental "forget" mode; slimmed + optimized the client and added a RAM union for the mutually-exclusive receive buffers.

**Console tunnel**
* Feature: cooperative, non-blocking console output (info / progress / verbose diagnostics) — never blocks `loop()`.
* Feature: the console tolerates a congested bus; names the remote owner when it takes over the local console.
* Fix: release the session on a client drain-timeout and on a same-owner re-open; open a console only on a genuine OPEN-accept; mark console-open error paths as Failed so a concurrent-console rejection is reported; guard `ftcOnResponse` by source PA + arm the OUT dedup fresh (two devices sharing the console).

**Access control / security** (opt-in `OPENKNX_FTC_SECURITY`)
* Feature: server access control + password auth (login / logout, ETS-gated); brute-force back-off on the auth; the access-protection ETS block injected as a shared "Erweitert" section with context help; local serial test overrides for the config (never persisted).
* Fix: shortened + renamed the access-stage labels ("Ausgeschaltet" / "Im Prog-Modus" / "Mit Passwort").

**Device info & group communication (client)**
* Feature: `ftc <pa> info` device fingerprint (mask/class, manufacturer, order/hardware/version, FTM version, feature bits, table states, BCU extras, bus voltage) with APDU detection and `--verbose`/`--quiet`.
* Feature: read the info-ga group-address / association tables connection-oriented (the ETS path); bus scan with light/full probes and CSV save.

**Native ftc-cli (host, mac / win / linux)**
* Feature: native desktop FTC client over a KNXnet/IP tunnel — tunnel transport + protocol core + gzip + build matrix; a phosphor terminal UI + host shims; host driver with commands, result panels, discovery and monitors; delivery-rate send pacing with 1-outstanding TX; host-CLI path-length limits + `get` alias + honest probe wording.

**Minimal footprint (feature gates)** — new in 0.2.0
* Feature: central `FileTransferConfig.h` — every switch is on when it is set and off when it is not, no values and no defaults to take away. Two profiles pick a sensible set: `OPENKNX_FTC_PROFILE_DEVICE` for a managed end device, `OPENKNX_FTC_PROFILE_MANAGER` for interface/router/host; setting no profile leaves the bare server core. `OPENKNX_FTC_CLIENT` and `OPENKNX_FTC_CONSOLE` stay explicit `-D` because `lib/knx` and `lib/OGM-Common` read them and never see this header. Six misconfigurations fail the build instead of compiling something else. Full table in [doc/reference/flags.md](doc/reference/flags.md).
* Feature: server extras gateable — `OPENKNX_FTC_DOWNLOAD` / `_FASTUPLOAD` / `_DIROPS` guard FileDownload, FileUploadFast+Report and the directory ops; `_FASTUPLOAD` also gates the CheckFeatures FAST bit so a server without fast never advertises it (the client falls back to classic). Together they cost ~4.6 KB on RP2040 and ~5.2 KB on ESP32; `PROFILE_DEVICE` bundles them.
* Feature: client extras gateable — `OPENKNX_FTC_SCAN` / `_DEVICEINFO` guard the on-device scan and the device-fingerprint + GA report (handler + dispatch + request fns + console commands + help); cross-guards keep the scan↔device-info coupling well-formed. Combined ~28 KB less flash on a 2 MB RP2040.
* Feature: the Info-API struct mirror (the render-agnostic surface a web/panel frontend draws from) is gated on `OPENKNX_WEBSERVER` (native host always on).
* Change: the core (FwUpdate, classic upload, FileInfo, FilesystemInfo, Format/Exists/Rename/Delete, ModuleVersion, CheckFeatures, Cancel) has no switch — always compiled on every device.

**Non-blocking** — new in 0.2.0
* Feature: the LittleFS FileInfo CRC is now cooperative — routed through the same `crcLoop` as SD/EFC instead of a blocking whole-file read in the KNX dispatch (never reboots on a large file). FileInfo answers `0x02` (computing) until the pass finishes, then `0x00` (size + CRC); the CRC value is byte-identical.
* Fix: cancel the cooperative CRC job before any FS-mutating command — the persistent `_crcFile` handle is dropped before format/upload/delete, so those can never run against an open handle (use-after-free / concurrent second handle on RP2040). `cmdFormat` also closes an open transfer handle first. (Found by the memory-safety review.)

**Structure / cleanup** (behaviour-preserving) — new in 0.2.0
* Change: the 2610-line `loop()` state machine was split into 7 per-feature handler methods (download / fast / dir-ops / scan / device-info / security / console); `loop()` is now a preamble + core-transfer inline + a dispatcher. Pure code motion, verified behaviour-identical.
* Fix: `cmdFileInfo` no longer puts a 1000-byte VLA on the KNX-dispatch stack (fixed 256-byte chunk; −744 B stack peak); `consoleIdle()` / `_conSub` guarded so a console-less client build compiles.
* Change: de-duplicated the classic/fast size-hint parse, dropped a dead `|| OPENKNX_EXTFLASH` token and a redundant answer byte; console dividers renamed to purpose (`RULE_SECTION` / `RULE_DFBAR` / `RULE_REPORT`) from one shared dash buffer; clang-formatted (the `_diagPad` table protected with `clang-format off`).

**Docs & tooling**
* Docs: a restructured doc set under `doc/` — the FTC engineering reference (`FTC-Reference.md`), a standalone console-tunnel doc (`FTC-Console.md`) and a standalone access-control doc (`FTC-Security.md`); a modernized README (device↔device or PC, FW-fit check, build-switch table); old pre-implementation concept notes removed.
* Tooling: FTC PowerShell test tooling + a firmware compressor; an extended `Test-FtcSuite.ps1` (help, verbose streaming, tri-state upload, path fixes); host scripts + repo meta (README, .gitignore); dropped dead constants `FTC_PKG_DEFAULT` / `FTC_RATE_MIN_MS`.
* Change: library bumped 0.1.5 → 0.2.0.

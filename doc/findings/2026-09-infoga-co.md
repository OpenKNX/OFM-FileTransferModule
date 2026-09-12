# `ftc <pa> info ga` fails against an IP-Interface target — CO / T_Connect analysis

Scope: analysis only, no code changed. Repo root `OAM-IP-Interface`; `lib/knx -> ../../knx`
(real path `/Users/ecolak/Entwicklung/OpenKNX-Router/knx`); `lib/OFM-FileTransferModule -> ../../OFM-FileTransferModule`.

## Observed

- `ftc 5.0.3 info ga` (plain `Bau07B0`, KNeoPix): WORKS — prints `Group addresses: 0/0/3`, `KO 4: 0/0/3`.
- `ftc 5.0.10 / 5.0.11 info ga` (this product, `Bau07B0IP`): FAILS — prints
  `File-Transfer:    no answer (not a KnxFileTransfer device)` and no GAs. Fails cross-interface
  (5.0.11 -> 5.0.10), so not self-addressing.
- Plain `ftc <if> info`, `ping`, `df` against the same interfaces WORK (connectionless FunctionProperty
  obj 159 + CheckFeatures answer fine; `info` even shows FTC module 0.1.6, features Resume/Update/Fast/Console).
- ETS reads the interfaces' full GA table (their CO server path works for ETS).

## The exact client sequence (`FileTransferClient.cpp`)

`info ga` -> `requestGroupComm(pa)` (FileTransferClient.cpp:2532):

1. `requestGroupComm` calls `requestDeviceInfo(pa)` (identical to plain `info`), then sets `_gaMode = true`
   and arms `ftcOnMemory` (2534-2536). The discovery chain that follows is byte-for-byte the plain-`info`
   chain until enumeration ends.
2. Discovery state machine (all **connectionless**):
   - `FtcDevDescr` -> `ftcSendDeviceDescriptorRead` (bau_systemB.cpp:620 -> app `deviceDescriptorReadRequest`,
     application_layer.cpp:463 = `individualSend`). Sets `_devHasMask`.
   - `FtcDevVer` -> `ftcSend(FTC_CMD_MODULE_VERSION)` -> `ftcSendCommand` -> `functionPropertyCommandRequest`.
     Sets `_devHasVer`. **This send is UNCONDITIONALLY connectionless** — application_layer.cpp:700-702
     deliberately calls `dataIndividualRequest`, never the `asap == _connectedTsap` CO path.
   - `FtcDevFeat` -> `ftcSend(FTC_CMD_CHECK_FEATURES)` (same unconditional-connectionless FunctionProperty).
   - `FtcDevProp` / `FtcDevEnum` -> `ftcSendPropertyValueRead` (application_layer.cpp:614 = `individualSend`,
     CO-capable).
3. At enumeration end, `_gaMode` branches (FileTransferClient.cpp:4492) to `ftcGaBeginWalk()` (2558):
   - `ftcScanConnect(target)` (bau_systemB.cpp:680) -> `applicationLayer().connectRequest` -> transport
     `connectRequest` -> `A12` (transport_layer.cpp:690) transmits a **T_Connect**, state `Connecting`,
     `_connectionAddress = target`.
   - `FtcGaConnect` (4579) waits <= `FTC_CO_CONNECT_TMO = 600 ms` (FileTransferClient.cpp:277) for
     `ftcScanConnected()` = `applicationLayer().isConnected()` = `_connectedTsap >= 0`. The originator
     becomes connected only when the T_Connect L_Data.**con** returns success (L2-ACK), i.e.
     `dataIndividualConfirm(Connect, status=true)` -> `A13` (transport_layer.cpp:702) -> `connectConfirm`
     -> `_connectedTsap = target` (application_layer.cpp:318-323).
   - Once connected, `ftcGaAdvance` reads `PID_TABLE_REFERENCE` then walks the address/assoc tables with
     `ftcSendMemoryRead` (bau_systemB.cpp:664 -> `memoryReadRequest` = `individualSend`). Because
     `asap == _connectedTsap` now, every read routes **connection-oriented** (application_layer.cpp:1457).
   - On connect-timeout it calls `ftcScanDisconnect()` (resets `_connectedTsap`) and falls back to a
     **connectionless** memory walk (FileTransferClient.cpp:4588-4591; comment 2551-2556: the permissive
     OpenKNX stack answers connectionless `A_Memory_Read`, foreign System-B devices do not).

Where the failing text comes from: `File-Transfer: no answer (not a KnxFileTransfer device)` is emitted
ONLY by `ftcDevReport()` when `_devHasVer == false` (FileTransferClient.cpp:2521-2523). In `_gaMode` the
success path uses `ftcGaReport()` and never calls `ftcDevReport()`. The only `_gaMode`-reachable
`ftcDevReport()` calls are the `FtcDevDescr`/`FtcDevVer` early-out branches (4296, 4342), and 4342 prints
the "not a KnxFileTransfer device" line only when **both** `_devHasMask == false` **and** `_devHasVer == false`.

## The core contradiction (this is the crux)

`ModuleVersion(100)` is sent **unconditionally connectionless** (application_layer.cpp:700-702), and plain
`info` proves the interface answers connectionless FunctionProperty. So in a clean single `info ga` run
`_devHasVer` should be TRUE and `ftcDevReport` should never run — yet the field reports the
"not a KnxFileTransfer device" line. For that line to appear in `_gaMode`, the connectionless
`DeviceDescriptor_Read` AND the connectionless `ModuleVersion` must BOTH have gone unanswered.

The only mechanism in the code that turns those into no-answers is that they were routed
**connection-oriented** and timed out — which for `DeviceDescriptor_Read` (`individualSend`,
application_layer.cpp:463) happens iff `_connectedTsap == target` at entry. `ModuleVersion` can't be
diverted (it is hard-connectionless), so its failure must be a genuine bus no-answer while a stale CO
session is half-open on one of the two peers. In other words: the reported symptom is a **cascade of a
wedged connection-oriented session**, not a first-order discovery failure. The single shared transport
connection slot (`TransportLayer::_currentState` / `_connectionAddress`, one instance;
`ApplicationLayer::_connectedTsap`, one instance) is the prime suspect.

## Why the interface differs from a plain `Bau07B0` — candidate root causes (ranked)

Traced statically, the TARGET-side incoming-T_Connect path on `Bau07B0IP` is structurally identical to a
plain device and looks correct:

- L2-ACK of the incoming T_Connect: `Bau07B0IP::isAckRequired` ACKs own PA (bau07B0_ip.cpp:183). OK.
- `DataLinkLayer::frameReceived` forwards a COPY to the tunnel because `_forwardToTunnel == true`
  (data_link_layer.cpp:173-174), then still delivers to `_networkLayerEntity.dataIndication`
  (data_link_layer.cpp:206). With no tunnel client connected, `IpTunnelServer::dataIndicationToTunnel`
  returns early without mutating the frame (ip_tunnel_server.cpp:363-370). OK — no corruption of the
  reference passed on to the network layer.
- `NetworkLayerDevice::dataIndication`: individual, `destination == own PA` -> `dataIndividualIndication`
  (network_layer_device.cpp:79-85). OK.
- `TransportLayer` Connect handling from `Closed`: `OpenIdle` + `A1` -> `connectIndication`
  (transport_layer.cpp:118-119, 591-598) -> `_connectedTsap = source`. Subsequent `T_Data_Connected` ->
  `A2` ACK + `dataConnectedIndication` -> app responds over CO. OK.

So no deterministic interface-specific *drop* of an incoming CO frame is visible in the console-only
(no ETS tunnel) case. The plausible differences that a plain `Bau07B0` does NOT have:

1. **Single shared transport connection can be left half-open / OpenIdle across runs (HIGH suspicion).**
   The transport state machine is single-connection. If a peer's transport is stuck in `OpenIdle`
   (`_connectionAddress` = old partner) and a NEW T_Connect arrives from the SAME source, that is event
   E0/`OpenIdle` -> `A0` = **do nothing** (transport_layer.cpp:122-127): the new connect is L2-ACKed but
   gets no transport response, so the client's `isConnected()` never turns true, it times out at 600 ms,
   disconnects, and falls back. This is order-of-events / cross-run state, not pure per-call logic — which
   is exactly why `info` (connectionless, no connection needed) keeps working while `info ga` (needs the
   connection) fails, and why the failure is sticky across attempts. The interface reaches this wedged
   state more easily than a plain device because it runs BOTH an FTC-client role and the device/server
   role on the SAME `TransportLayer`/`ApplicationLayer`, and its heavier `loop()` (bau07B0_ip.cpp:158-164:
   `_ipLayer.loop()` + `_tpLayer.loop()` + `BauSystemBDevice::loop()` + `_ipTunnelServer.loop()`) changes
   the timing of the transport connection/ACK timeouts (`TransportLayer::loop`, transport_layer.cpp:557-574).

2. **`CemiServer::_clientAddress = own PA + 1` (cemi_server.cpp:30) collides with the neighbour interface
   (MEDIUM suspicion).** With 5.0.10 and 5.0.11 both present, 5.0.10's cEMI client address is 5.0.11 (a
   real device) and vice-versa. The confirm-drop guard `if (frame.sourceAddress() == _cemiServer->clientAddress()) return;`
   (data_link_layer.cpp:137) does not fire for a device's own management TX in the traced flow (a .con is
   sourced from own PA, not own+1), so it is not a proven culprit — but the +1 default landing on a live
   neighbour is fragile and worth eliminating from the variables before deeper work.

3. **Heavier, IP-first `loop()` ordering perturbs CO transport timeouts (MEDIUM, couples with #1).** The
   connection timeout / ack-timeout are wall-clock (`millis()`), so any longer single-pass on the interface
   shifts them relative to a plain device and can abort a CO exchange mid-walk.

Not culprits (checked and cleared): `enableRoutingIndications(false)` (IP-multicast RoutingIndications only,
irrelevant to TP CO); `KNX_TUNNELING_NO_TUNNEL_PA_ON_TP` sendTelegram gate (data_link_layer.cpp:256 — would
also suppress the *connectionless* `info`, which works, so the target is not a `isTunnelingPA`); tunnel
frame mutation (early-return, no mutation, no client connected).

## What the evidence supports, and what it does not

The failure sits on the connection-oriented path: a `T_Connect` session between two `Bau07B0IP` peers does
not complete or serve reliably. The "not a KnxFileTransfer device" message is a consequence of that wedged
single shared transport connection, not a genuine absence of an FTM server. The strongest concrete lead is
the single-slot `TransportLayer` / `_connectedTsap` left half-open (hypothesis #1).

What cannot be named from code alone: a specific wrong statement on the target side. The static incoming
connection-oriented path on `Bau07B0IP` reads as correct, so the deterministic line is not identified. The
runtime trace below is what would separate the hypotheses.

## Recommended fix DIRECTION (do not implement yet — shared-lib, needs user sign-off)

- First DISAMBIGUATE with the runtime probes below; do not patch blind.
- If #1 confirmed: the fix is around the single shared transport connection lifecycle — ensure a stale/
  half-open connection on either peer is reset so a fresh incoming T_Connect from the same source is
  honoured (e.g. a self-guard so the FTC client never leaves `_connectedTsap`/transport wedged on the
  interface, and/or verify `A0` on `OpenIdle`+duplicate-Connect is the desired behaviour for a re-connect).
  This is a `lib/knx` transport-layer / application-layer change -> re-run the cross-target check in
  `knx/doc/architecture-bau-and-tunnelling.md` and the `simulate-change` skill before proposing.
- Independently, consider removing the `own PA + 1` cEMI client-address default's collision with a live
  neighbour (config, not hot-path).
- The client-side `info ga` logic itself looks sound; no change indicated there until the stack behaviour
  is understood.

## Runtime observations that would disambiguate (serial consoles; do NOT let the analysis touch the ports)

Ports: RP = `/dev/cu.usbmodem3101`, ESP = `/dev/cu.wchusbserial8430`.

1. **Reproduce cleanly after a fresh boot of BOTH interfaces**, then run ONE `ftc 5.0.10 info ga` from
   5.0.11 and capture the full output. Question: does the very first post-boot run still print
   "not a KnxFileTransfer device", or only the 2nd+ run? If only 2nd+, that confirms hypothesis #1 (a prior
   run wedges the shared connection).
2. On the **TARGET** (5.0.10) console during the client's `info ga`, watch for whether it logs an incoming
   T_Connect / a transport state transition and whether `TX Frames` increments (it must transmit T_ACK +
   the CO responses). If the target L2-ACKs (RX seen) but emits no CO response TX, that isolates the
   target's transport as the wedge (rules the client in as healthy).
3. Compare a **`ftc 5.0.10 info`** immediately followed by **`ftc 5.0.10 info ga`**: if `info` keeps
   working but `info ga` fails right after, and a subsequent bare `info` then ALSO starts failing, the
   shared `_connectedTsap` is being left set (connectionless `info` would then wrongly route CO). That is
   the decisive signature of hypothesis #1.
4. Temporarily set the two interfaces far apart in address (e.g. 5.0.10 and 5.0.40) so `own PA + 1` cannot
   land on the neighbour, and retry. If `info ga` now works, hypothesis #2 (clientAddress collision) is in
   play.

// ┬────┴  OFM-FileTransferModule / ftc-cli
// ■ KNX   2026 OpenKNX - Erkan Çolak
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026, Erkan Çolak
//
// KnxIpTunnel — the host transport seam for the FTC ftc-cli.
//
// This is the FIXED CONTRACT between two independently-built parts:
//   * the glue shim (shim/knx_shim.h) — its HostBau forwards the 14 knx.bau().ftc* calls here;
//   * the transport impl (knx_ip_tunnel.cpp) — a KNXnet/IP tunnel client that turns each call into a
//     cEMI L_Data over UDP and dispatches incoming L_Data.ind back into the four callbacks.
//
// Design notes:
//   * The unchanged embedded FileTransferClient.cpp is the STATE MACHINE; it only ever calls the 14
//     ftc* methods on knx.bau(). HostBau maps those 1:1 onto the methods below (SecurityControl is
//     dropped — no KNX Data Security on the tunnel path; `asap` is the target PA).
//   * NON-BLOCKING: pump() does one bounded pass (drain pending UDP, dispatch, ACK, heartbeat) and
//     returns. main() calls pump() then FileTransferClient::loop() every iteration. No blocking recv,
//     no delay(). This mirrors the device's cooperative loop() budget.
//   * The callback pointer types are byte-identical to the knx BAU member fields (shim contract §3), so
//     HostBau can pass the client's static callbacks straight through.
//
// Wire details (cEMI/APDU/CRC/CO-scan) live in doc/FTC-WIRE-PROTOCOL.md and are the impl's job.
#pragma once
#include <cstdint>
#include <string>

// Callback prototypes — must match knx/src/knx/bau_systemB.h:178-181 exactly (shim contract §3).
using FtcResponseCb = void (*)(uint16_t pa, uint8_t objectIndex, uint8_t propertyId, uint8_t* data, uint8_t length);
using FtcDdCb = void (*)(uint16_t pa, uint8_t descriptorType, const uint8_t* data);
using FtcPropCb = void (*)(uint16_t pa, uint8_t objectIndex, uint8_t propertyId, const uint8_t* data, uint8_t length);
using FtcMemCb = void (*)(uint16_t pa, uint16_t addr, const uint8_t* data, uint8_t len);

class KnxIpTunnel
{
  public:
    // --- lifecycle -------------------------------------------------------------------------------
    // Open a KNXnet/IP tunnel to the interface/router. Performs CONNECT_REQUEST and stores the
    // individual address the server hands back in the CONNECT_RESPONSE CRD (used as our source PA).
    // Returns false on socket/handshake failure. `port` default 3671.
    bool connect(const std::string& ip, uint16_t port = 3671);
    // Graceful DISCONNECT_REQUEST + socket close. Safe to call when not connected.
    void disconnect();
    bool connected() const { return _connected; }
    // Our assigned tunnel individual address (from CONNECT_RESPONSE). Feeds knx.individualAddress().
    uint16_t assignedPA() const { return _assignedPA; }

    // --- non-blocking pump -----------------------------------------------------------------------
    // One bounded pass: read all currently-available UDP datagrams, ACK TUNNELING_REQUESTs, parse
    // each cEMI L_Data.ind and dispatch to the matching callback, service the connection-state
    // heartbeat and any tunnel-seq bookkeeping. Never blocks. Call once per main-loop iteration.
    void pump();

    // --- the 14-method seam (mapped 1:1 from knx.bau().ftc*, SecurityControl dropped) ------------
    // Senders: connectionless T_Data_Individual, LowPriority. Return false if not connected / TX full.
    bool sendCommand(uint16_t pa, uint8_t objectIndex, uint8_t propertyId, const uint8_t* data, uint8_t length);
    bool sendDeviceDescriptorRead(uint16_t pa); // AckDontCare scan probe, descriptorType 0
    bool sendPropertyValueRead(uint16_t pa, uint8_t objectIndex, uint8_t propertyId, uint8_t count, uint16_t startIndex);
    bool sendPropertyValueWrite(uint16_t pa, uint8_t objectIndex, uint8_t propertyId, uint8_t count, uint16_t startIndex,
                                const uint8_t* data, uint8_t length);
    bool sendMemoryRead(uint16_t pa, uint8_t number, uint16_t memoryAddress);

    // Connection-oriented (ETS-parity) scan: T_Connect / T_Data_Connected(DeviceDescriptor_Read) / T_Disconnect.
    bool scanConnect(uint16_t pa);
    bool scanConnected(); // true once the T_Connect is acknowledged / the CO session is up
    void scanReadDescriptor();
    void scanDisconnect();

    // Outstanding-frame count for the client's flow control (TP-FIFO analogue on IP). 0 when idle.
    uint16_t txQueueSize() const;

    // Delivery-rate (BBR-style) pacing feedback from the FTC client's per-window reports: clean -> probe the
    // send rate higher; a lossy window -> snap it to `deliveredBps` (the measured wire ceiling); deliveredBps==0
    // && !clean = a report-timeout kick -> back off. The tunnel ACKs instantly, so the confirmed delivered
    // rate -- not a guess -- sets the TP send rate.
    void pacingRate(uint32_t deliveredBps, bool clean);

    // --- callback registration (HostBau.ftcSet*Callback forwards here) ---------------------------
    void setResponseCallback(FtcResponseCb cb) { _responseCb = cb; }
    void setDeviceDescriptorCallback(FtcDdCb cb) { _ddCb = cb; }
    void setPropertyCallback(FtcPropCb cb) { _propCb = cb; }
    void setMemoryCallback(FtcMemCb cb) { _memCb = cb; }

  private:
    // impl-owned; declared here only so the header is self-contained for the shim. The .cpp may hold
    // additional private state (socket fd, channel id, seq counters, CO seq, RX buffer) — add as needed.
    bool _connected = false;
    uint16_t _assignedPA = 0;
    FtcResponseCb _responseCb = nullptr;
    FtcDdCb _ddCb = nullptr;
    FtcPropCb _propCb = nullptr;
    FtcMemCb _memCb = nullptr;
};

// Single process-wide tunnel. main() calls connect()/pump(); the shim's knx.bau() forwards into it.
extern KnxIpTunnel g_knxTunnel;

// Monotonic count of frames transmitted. The host CLI polls it to tell when a read-chain command
// (info/df/scan — which run on their own sub-state and never set isBusy()) has gone quiet = finished.
uint32_t knxTunnelActivity();

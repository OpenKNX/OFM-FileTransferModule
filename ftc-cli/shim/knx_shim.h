// ┬────┴  OFM-FileTransferModule / ftc-cli
// ■ KNX   2026 OpenKNX - Erkan Çolak
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026, Erkan Çolak
//
// Host stand-in for the knx-lib slice the four FTC files touch (shim contract §2, §3, §4).
// HostBau reproduces the 14 knx.bau().ftc* signatures byte-exact and forwards each to g_knxTunnel
// (SecurityControl dropped; asap -> pa). No knx template or deep type crosses this boundary.
#pragma once
#include <cstdint>

#include "knx_ip_tunnel.h" // the transport seam (global g_knxTunnel)

// KNX value types (real home knx/src/knx/knx_types.h). Unqualified `None` for SecurityControl{false, None}.
enum DataSecurity
{
    None,
    Auth,
    AuthConf
};

struct SecurityControl
{
    bool toolAccess;
    DataSecurity dataSecurity;
};

/// @brief Host BAU: the 14 ftc* methods from contract §2, each forwarding to the tunnel seam.
class HostBau
{
  public:
    // --- senders (SecurityControl by value; data non-const) --------------------------------------
    bool ftcSendCommand(uint16_t asap, const SecurityControl secCtrl, uint8_t objectIndex,
                        uint8_t propertyId, uint8_t* data, uint8_t length)
    {
        (void)secCtrl;
        return g_knxTunnel.sendCommand(asap, objectIndex, propertyId, data, length);
    }
    bool ftcSendDeviceDescriptorRead(uint16_t asap, const SecurityControl secCtrl)
    {
        (void)secCtrl;
        return g_knxTunnel.sendDeviceDescriptorRead(asap);
    }
    bool ftcSendPropertyValueRead(uint16_t asap, const SecurityControl secCtrl, uint8_t objectIndex,
                                  uint8_t propertyId, uint8_t count, uint16_t startIndex)
    {
        (void)secCtrl;
        return g_knxTunnel.sendPropertyValueRead(asap, objectIndex, propertyId, count, startIndex);
    }
    bool ftcSendPropertyValueWrite(uint16_t asap, const SecurityControl secCtrl, uint8_t objectIndex,
                                   uint8_t propertyId, uint8_t count, uint16_t startIndex,
                                   uint8_t* data, uint8_t length)
    {
        (void)secCtrl;
        return g_knxTunnel.sendPropertyValueWrite(asap, objectIndex, propertyId, count, startIndex, data, length);
    }
    bool ftcSendMemoryRead(uint16_t asap, const SecurityControl secCtrl, uint8_t number,
                           uint16_t memoryAddress)
    {
        (void)secCtrl;
        return g_knxTunnel.sendMemoryRead(asap, number, memoryAddress);
    }

    // --- connection-oriented (ETS-parity) scan ---------------------------------------------------
    bool ftcScanConnect(uint16_t pa) { return g_knxTunnel.scanConnect(pa); }
    bool ftcScanConnected() { return g_knxTunnel.scanConnected(); }
    // SecurityControl by const ref here (overload identity differs from the senders).
    void ftcScanReadDescriptor(const SecurityControl& sec)
    {
        (void)sec;
        g_knxTunnel.scanReadDescriptor();
    }
    void ftcScanDisconnect() { g_knxTunnel.scanDisconnect(); }

    // --- callback setters (raw C function pointers, types per §3) ---------------------------------
    void ftcSetResponseCallback(void (*cb)(uint16_t pa, uint8_t objectIndex, uint8_t propertyId,
                                           uint8_t* data, uint8_t length))
    {
        g_knxTunnel.setResponseCallback(cb);
    }
    void ftcSetDeviceDescriptorCallback(void (*cb)(uint16_t pa, uint8_t descriptorType,
                                                   const uint8_t* data))
    {
        g_knxTunnel.setDeviceDescriptorCallback(cb);
    }
    void ftcSetPropertyCallback(void (*cb)(uint16_t pa, uint8_t objectIndex, uint8_t propertyId,
                                           const uint8_t* data, uint8_t length))
    {
        g_knxTunnel.setPropertyCallback(cb);
    }
    void ftcSetMemoryCallback(void (*cb)(uint16_t pa, uint16_t addr, const uint8_t* data,
                                         uint8_t len))
    {
        g_knxTunnel.setMemoryCallback(cb);
    }

    // --- flow control ----------------------------------------------------------------------------
    virtual uint16_t ftcTxQueueSize() { return g_knxTunnel.txQueueSize(); }
    virtual void ftcPacingRate(uint32_t deliveredBps, bool clean) { g_knxTunnel.pacingRate(deliveredBps, clean); }
};

/// @brief Host knx facade: only bau() + individualAddress() are used by the four files.
class HostKnxFacade
{
  public:
    HostBau& bau() { return _bau; }
    uint16_t individualAddress() { return g_knxTunnel.assignedPA(); }

  private:
    HostBau _bau;
};

// Global knx facade instance (defined in shim.cpp).
extern HostKnxFacade knx;

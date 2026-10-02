#pragma once
/**
 * @file        knx_shim.h
 * @brief       Host stand-in for the knx-lib slice the four FTC files touch (shim contract §2, §3, §4).
 * @details     HostBau reproduces the knx.bau().ftc* method surface (§2) byte-exact and forwards each to
 *              g_knxTunnel (SecurityControl dropped; asap -> pa). No knx template or deep type crosses
 *              this boundary. HostKnxFacade wraps it as the `knx` global (bau() + individualAddress()).
 * @date        2026-07-25
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 */
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

/// @brief Host BAU: the ftc* methods of contract §2, each forwarding to the tunnel seam.
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
    bool ftcSendAdcRead(uint16_t asap, const SecurityControl secCtrl, uint8_t channelNr, uint8_t readCount)
    {
        (void)secCtrl;
        return g_knxTunnel.sendAdcRead(asap, channelNr, readCount);
    }

    // --- connection-oriented (ETS-parity) scan ---------------------------------------------------
    bool ftcScanConnect(uint16_t pa) { return g_knxTunnel.scanConnect(pa); }
    bool ftcScanConnected() { return g_knxTunnel.scanConnected(); }
    bool ftcScanReadAcked() { return g_knxTunnel.scanReadAcked(); }
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
    void ftcSetAdcCallback(void (*cb)(uint16_t pa, uint8_t channel, uint8_t count, int16_t value))
    {
        g_knxTunnel.setAdcCallback(cb);
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

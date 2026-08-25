/**
 * @file        knx_ip_tunnel.h
 * @brief       KNXnet/IP tunnel transport seam — the fixed host<->transport contract for ftc-cli.
 * @details     The glue shim's HostBau forwards the knx.bau().ftc* calls onto this class; the impl
 *              (knx_ip_tunnel.cpp) turns each into a cEMI L_Data over UDP and dispatches each inbound
 *              L_Data.ind APDU back into the five FTC callbacks. Non-blocking: pump() does one bounded
 *              pass (drain / dispatch / ACK / heartbeat) and returns.
 * @date        2026-08-03
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include <cstdint>
#include <string>

// Callback prototypes — must match the ftc* callback prototypes in knx/src/knx/bau_systemB.h
// (under #ifdef OPENKNX_FTC_CLIENT) exactly (shim contract §3).
using FtcResponseCb = void (*)(uint16_t pa, uint8_t objectIndex, uint8_t propertyId, uint8_t* data, uint8_t length);
using FtcDdCb = void (*)(uint16_t pa, uint8_t descriptorType, const uint8_t* data);
using FtcPropCb = void (*)(uint16_t pa, uint8_t objectIndex, uint8_t propertyId, const uint8_t* data, uint8_t length);
using FtcMemCb = void (*)(uint16_t pa, uint16_t addr, const uint8_t* data, uint8_t len);
using FtcAdcCb = void (*)(uint16_t pa, uint8_t channel, uint8_t count, int16_t value);
using FtcConCb = void (*)(uint16_t pa, bool ok);

class KnxIpTunnel
{
  public:
    // --- lifecycle -------------------------------------------------------------------------------
    /**
     * @brief Open a KNXnet/IP tunnel: CONNECT_REQUEST, then store the individual address the server hands
     *        back in the CONNECT_RESPONSE CRD as our source PA.
     * @details Returns false on socket/handshake failure. `port` default 3671.
     */
    bool connect(const std::string& ip, uint16_t port = 3671);

    /**
     * @brief Graceful DISCONNECT_REQUEST + socket close. Safe to call when not connected.
     */
    void disconnect();

    bool connected() const { return _connected; }

    /**
     * @brief Our assigned tunnel individual address (from CONNECT_RESPONSE); feeds knx.individualAddress().
     */
    uint16_t assignedPA() const { return _assignedPA; }

    /**
     * @brief Live tunnel-health snapshot: KNXnet/IP channel id + current TX / last-accepted RX seq counters.
     * @details 0 when not connected.
     */
    uint8_t channelId() const;
    uint8_t txSeq() const;
    uint8_t rxSeq() const;

    /**
     * @brief Why the last connect() ended, so the caller can diagnose a failure.
     * @details -1 = no CONNECT_RESPONSE (timeout); >= 0 = the CONNECT_RESPONSE status byte (0x00 ok, else an
     *          E_* error, e.g. 0x24 no-more-connections).
     */
    int lastConnectStatus() const { return _lastConnectStatus; }

    /**
     * @brief KNX priority stamped into the CTRL octet of every FTC data frame we send (2-bit form).
     * @details system=0 · normal=1 · urgent=2 · low=3 (default). `--prio` sets it; transport control frames
     *          (T_Ack/T_Connect) keep their own fixed priority.
     */
    void setTxPriority(uint8_t p2) { _txPriority = (uint8_t)(p2 & 0x03); }
    uint8_t txPriority() const { return _txPriority; }

    // --- non-blocking pump -----------------------------------------------------------------------
    /**
     * @brief One bounded pass: read all available UDP, ACK TUNNELING_REQUESTs, dispatch each L_Data.ind,
     *        service the connection-state heartbeat and tunnel-seq bookkeeping.
     * @details Never blocks. Call once per main-loop iteration.
     */
    void pump();
    void maybeReconnect(); // re-dial the tunnel after a drop (backoff, bounded) so the FTC auto-resume can continue

    // --- the ftc* seam (mapped 1:1 from knx.bau().ftc*, SecurityControl dropped) -----------------
    // Senders: connectionless T_Data_Individual, LowPriority. Return false if not connected / TX full.
    bool sendCommand(uint16_t pa, uint8_t objectIndex, uint8_t propertyId, const uint8_t* data, uint8_t length);
    bool sendDeviceDescriptorRead(uint16_t pa); // AckDontCare scan probe, descriptorType 0
    bool sendPropertyValueRead(uint16_t pa, uint8_t objectIndex, uint8_t propertyId, uint8_t count, uint16_t startIndex);
    bool sendPropertyValueWrite(uint16_t pa, uint8_t objectIndex, uint8_t propertyId, uint8_t count, uint16_t startIndex,
                                const uint8_t* data, uint8_t length);
    bool sendMemoryRead(uint16_t pa, uint8_t number, uint16_t memoryAddress);
    bool sendAdcRead(uint16_t pa, uint8_t channelNr, uint8_t readCount); // A_ADC_Read (e.g. BCU bus voltage)

    // Connection-oriented (ETS-parity) scan: T_Connect / T_Data_Connected(DeviceDescriptor_Read) / T_Disconnect.
    bool scanConnect(uint16_t pa);
    bool scanConnected(); // true once the T_Connect is acknowledged / the CO session is up
    bool scanReadAcked(); // true once the device T_ACKed our CO read -> present (presence-on-ACK, before the DD response)
    void scanReadDescriptor();
    void scanDisconnect();

    /**
     * @brief Outstanding-frame count for the client's flow control (TP-FIFO analogue on IP). 0 when idle.
     */
    uint16_t txQueueSize() const;

    /**
     * @brief Delivery-rate (BBR-style) pacing feedback from the FTC client's per-window reports.
     * @details clean -> probe the send rate higher; a lossy window -> snap it to `deliveredBps` (the measured
     *          wire ceiling); deliveredBps==0 && !clean = a report-timeout kick -> back off. The tunnel ACKs
     *          instantly, so the confirmed delivered rate -- not a guess -- sets the TP send rate.
     */
    void pacingRate(uint32_t deliveredBps, bool clean);

    /** @brief Current paced send rate (B/s) -- so the UI can say whether WE are the brake, or the link is. */
    uint32_t paceBps() const;

    // --- callback registration (HostBau.ftcSet*Callback forwards here) ---------------------------
    void setConfirmCallback(FtcConCb cb) { _conCb = cb; }
    FtcConCb confirmCallback() const { return _conCb; }
    void setResponseCallback(FtcResponseCb cb) { _responseCb = cb; }
    FtcResponseCb responseCallback() const { return _responseCb; } // so a probe can restore, not clear
    FtcPropCb propertyCallback() const { return _propCb; }
    void setDeviceDescriptorCallback(FtcDdCb cb) { _ddCb = cb; }
    FtcDdCb deviceDescriptorCallback() const { return _ddCb; }
    void setPropertyCallback(FtcPropCb cb) { _propCb = cb; }
    void setMemoryCallback(FtcMemCb cb) { _memCb = cb; }
    void setAdcCallback(FtcAdcCb cb) { _adcCb = cb; }

  private:
    // impl-owned; declared here only so the header is self-contained for the shim. The .cpp may hold
    // additional private state (socket fd, channel id, seq counters, CO seq, RX buffer) — add as needed.
    bool _connected = false;
    std::string _reconnectIp;        // endpoint of the last successful connect() -> re-dial target after a drop
    uint16_t _reconnectPort = 3671;
    bool _reconnectEnabled = false;  // set once connected -> pump() re-dials on a drop instead of dying
    uint32_t _lastReconnectMs = 0;   // backoff clock for maybeReconnect()
    uint8_t _reconnectAttempts = 0;  // consecutive failed re-dials -> give up past RECONNECT_MAX (transfer then fails cleanly)
    uint16_t _assignedPA = 0;
    uint8_t _txPriority = 0x03;   // FTC data-frame KNX priority (2-bit): 3 = LowPriority (polite default)
    int _lastConnectStatus = -1; // see lastConnectStatus(); -1 until a CONNECT_RESPONSE arrives
    FtcResponseCb _responseCb = nullptr;
    FtcConCb _conCb = nullptr;
    FtcDdCb _ddCb = nullptr;
    FtcPropCb _propCb = nullptr;
    FtcMemCb _memCb = nullptr;
    FtcAdcCb _adcCb = nullptr;
};

/**
 * @brief Single process-wide tunnel. main() calls connect()/pump(); the shim's knx.bau() forwards into it.
 */
extern KnxIpTunnel g_knxTunnel;

/**
 * @brief Monotonic count of frames transmitted.
 * @details The host CLI polls it to tell when a read-chain command (info/df/scan — which run on their own
 *          sub-state and never set isBusy()) has gone quiet = finished.
 */
uint32_t knxTunnelActivity();

/**
 * @brief Monotonic RX (received bus indications) and drop (truncated/oversize frames discarded) counters.
 * @details Shown live in the console status bar so the loss is never silent.
 */
uint32_t knxTunnelRx();
uint32_t knxTunnelDrops();

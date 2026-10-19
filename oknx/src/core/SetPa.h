/**
 * @file        SetPa.h
 * @brief       The bus side of `oknx setpa`, for the command line and `oknx serve` alike: who is in programming
 *              mode, which address a serial number has, is an address taken, confirm and restart.
 * @details     A_IndividualAddress_Response is counted per telegram, not per sender: two devices that both still
 *              have 15.15.255 are two devices (03_05_02 2.2 p.13). A Layer-2 repetition does not count; a sender
 *              whose telegrams all carry the repetition mark still counts once, in case an interface does not pass
 *              it on. The tunnel callbacks are plain function pointers, so the state is process-wide: one SetPaBus
 *              at a time, which the command line and a worker's management thread are.
 * @date        2026-10-19
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include "core/Discovery.h"
#include "knx_ip_tunnel.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <thread>
#include <vector>

namespace ftc
{

struct SetPaProgHit
{
    uint16_t src;
    unsigned orig, rep;
};
struct SetPaSnHit
{
    uint16_t src;
    uint8_t sn[6];
};

namespace detail_setpa
{
inline std::vector<SetPaProgHit> prog;
inline std::vector<SetPaSnHit> snResp; // A_IndividualAddressSerialNumber_Response
inline std::vector<SetPaSnHit> pid11;  // PID_SERIAL_NUMBER of the device object
inline std::vector<uint16_t> asked;    // the addresses PID 11 was read from; answers from others are not ours
inline volatile bool ddSeen = false;
inline uint16_t ddPa = 0;
inline void onBroadcast(uint16_t src, uint16_t apci, const uint8_t* d, uint8_t n, bool repeated)
{
    if ((apci & 0x03C0) == APCI_INDADDR_RESP)
    {
        for (SetPaProgHit& h : prog)
            if (h.src == src)
            {
                (repeated ? h.rep : h.orig)++;
                return;
            }
        prog.push_back({src, repeated ? 0u : 1u, repeated ? 1u : 0u});
    }
    else if (apci == APCI_INDADDR_SN_RESP && n >= 6)
    {
        SetPaSnHit h{src, {}};
        std::memcpy(h.sn, d, 6);
        snResp.push_back(h);
    }
}
inline void onProperty(uint16_t pa, uint8_t obj, uint8_t pid, const uint8_t* d, uint8_t n)
{
    if (obj != 0 || pid != 11 || n < 6) return; // a refusal carries no data
    if (std::find(asked.begin(), asked.end(), pa) == asked.end()) return;
    for (const SetPaSnHit& h : pid11)
        if (h.src == pa && std::memcmp(h.sn, d, 6) == 0) return; // the same answer again
    SetPaSnHit h{pa, {}};
    std::memcpy(h.sn, d, 6);
    pid11.push_back(h);
}
inline void onDd(uint16_t p, uint8_t, const uint8_t*)
{
    if (p == ddPa) ddSeen = true;
}
} // namespace detail_setpa

class SetPaBus
{
  public:
    std::vector<SetPaProgHit>& prog = detail_setpa::prog;
    std::vector<SetPaSnHit>& snResp = detail_setpa::snResp;
    std::vector<SetPaSnHit>& pid11 = detail_setpa::pid11;
    std::vector<uint16_t>& asked = detail_setpa::asked;

    SetPaBus(KnxIpTunnel& t, std::function<bool()> aborted) : _t(t), _aborted(std::move(aborted))
    {
        prog.clear();
        asked.clear();
        snResp.clear();
        pid11.clear();
        _prevProp = _t.propertyCallback();
        _t.setBroadcastCallback(&detail_setpa::onBroadcast);
        _t.setPropertyCallback(&detail_setpa::onProperty);
    }
    ~SetPaBus() { release(); }
    void release()
    {
        if (_released) return;
        _released = true;
        _t.setBroadcastCallback(nullptr);
        _t.setPropertyCallback(_prevProp);
    }

    void pumpFor(uint32_t ms)
    {
        for (const uint64_t t0 = detail::nowMs(); detail::nowMs() - t0 < ms && !_aborted();)
        {
            _t.pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    /** @brief Devices in programming mode after the last probe (per telegram, see the file note). */
    size_t progCount() const
    {
        size_t n = 0;
        for (const SetPaProgHit& h : prog) n += h.orig != 0 ? h.orig : 1;
        return n;
    }
    /** @brief A_IndividualAddress_Read, then collect for @p ms. */
    void probe(uint32_t ms)
    {
        prog.clear();
        _t.sendIndividualAddressRead();
        pumpFor(ms);
    }
    /** @brief Is @p pa taken? A_DeviceDescriptor_Read over a connection: 1 answered, 0 connected but silent, 2 no connection. */
    int coAsk(uint16_t pa)
    {
        detail_setpa::ddPa = pa;
        detail_setpa::ddSeen = false;
        const FtcDdCb prev = _t.deviceDescriptorCallback();
        _t.setDeviceDescriptorCallback(&detail_setpa::onDd);
        int r = 0;
        if (_t.scanConnect(pa))
        {
            for (uint64_t t0 = detail::nowMs(); detail::nowMs() - t0 < 100;) _t.pump();
            _t.sendDeviceDescriptorRead(pa);
            for (uint64_t t0 = detail::nowMs(); detail::nowMs() - t0 < 2000 && !detail_setpa::ddSeen && _t.scanConnectedTo(pa);)
                _t.pump();
            r = detail_setpa::ddSeen ? 1 : (_t.scanConnectedTo(pa) ? 0 : 2);
        }
        _t.setDeviceDescriptorCallback(prev);
        return r;
    }
    /**
     * @brief Confirm over a connection, then restart: that ends programming mode (2.3 step 4 p.15) and is what makes
     *        a device with the OpenKNX stack keep the address - it holds a new one in RAM until it saves on restart.
     * @return 0 = no answer at the address, 1 = answered, restart not sent, 2 = answered and restart sent.
     */
    int confirmAt(uint16_t pa, bool restart)
    {
        const bool confirmed = coAsk(pa) == 1;
        bool sent = false;
        if (confirmed && restart)
        {
            // Until the read is T_ACKed the restart would reuse its sequence number and be dropped as a duplicate.
            for (uint64_t t0 = detail::nowMs(); detail::nowMs() - t0 < 500 && !_t.scanReadAcked();) _t.pump();
            if (_t.scanReadAcked())
            {
                _t.sendRestart(pa, false, 0, 0);
                sent = true;
                for (uint64_t t0 = detail::nowMs(); detail::nowMs() - t0 < 300;) _t.pump();
            }
        }
        _t.scanDisconnect();
        return confirmed ? (sent ? 2 : 1) : 0;
    }
    /**
     * @brief Several devices in programming mode: ask each answering address for its serial number (PID 11 of the
     *        device object, up to 2 s). Devices that share an address answer separately; the answers land in pid11.
     */
    void askSerials()
    {
        pid11.clear();
        asked.clear();
        for (const SetPaProgHit& h : prog) asked.push_back(h.src); // a copy: pump() below can grow prog
        for (uint16_t src : asked)
        {
            _t.sendPropertyValueRead(src, 0, 11, 1, 1);
            pumpFor(30);
        }
        for (const uint64_t t0 = detail::nowMs(); detail::nowMs() - t0 < 2000 && !_aborted();)
        {
            bool all = true;
            for (const SetPaProgHit& h : prog)
            {
                size_t got = 0;
                for (const SetPaSnHit& s : pid11) got += s.src == h.src;
                if (got < (h.orig != 0 ? h.orig : 1)) all = false;
            }
            if (all) break;
            _t.pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    /** @brief Which address answers to serial number @p sn (2 s). */
    bool snWhere(const uint8_t sn[6], uint16_t& at)
    {
        snResp.clear();
        _t.sendIndividualAddressSerialNumberRead(sn);
        for (const uint64_t t0 = detail::nowMs(); detail::nowMs() - t0 < 2000 && !_aborted();)
        {
            _t.pump();
            for (const SetPaSnHit& h : snResp)
                if (std::memcmp(h.sn, sn, 6) == 0)
                {
                    at = h.src;
                    return true;
                }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }

  private:
    KnxIpTunnel& _t;
    std::function<bool()> _aborted;
    FtcPropCb _prevProp = nullptr;
    bool _released = false;
};

} // namespace ftc

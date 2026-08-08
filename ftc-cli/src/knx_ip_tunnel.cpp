/**
 * @file        knx_ip_tunnel.cpp
 * @brief       KNXnet/IP tunnel transport for the FTC ftc-cli.
 * @details     Implements the fixed contract in knx_ip_tunnel.h: each ftc* call becomes a cEMI L_Data.req
 *              carried in a KNXnet/IP TUNNELING_REQUEST, and each inbound L_Data.ind APDU is decoded into
 *              one of the five FTC callbacks. Byte layout follows doc/FTC-WIRE-PROTOCOL.md (§1 APDU,
 *              §5 decode, §6 CO scan, §7 cEMI framing). Non-blocking throughout: pump() drains pending
 *              datagrams and returns. All extra transport state lives in a translation-unit-local struct;
 *              there is exactly one g_knxTunnel.
 * @date        2026-08-03 <-Initial
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/

#include "knx_ip_tunnel.h"

#include <chrono>
#include <cstring>
#include <deque>
#include <vector>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #ifdef _MSC_VER
        #pragma comment(lib, "ws2_32.lib") // MSVC auto-links ws2_32; clang/mingw link it via the build config
    #endif
using sock_t = SOCKET;
    #define SOCK_INVALID INVALID_SOCKET
    #define SOCK_CLOSE closesocket
#else
    #include <arpa/inet.h>
    #include <errno.h>
    #include <fcntl.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <unistd.h>
using sock_t = int;
    #define SOCK_INVALID (-1)
    #define SOCK_CLOSE ::close
#endif

// ---------------------------------------------------------------------------------------------------
// APCI (10-bit) — request services we build (§1). Shared by the transport and the self-test, so these
// stay outside the FTC_TUNNEL_SELFTEST guard; all transport-only constants/helpers live inside it.
// ---------------------------------------------------------------------------------------------------
static constexpr uint16_t APCI_FUNC_PROP_CMD = 0x2C7;
static constexpr uint16_t APCI_DEVDESC_READ = 0x300;
static constexpr uint16_t APCI_PROPVALUE_READ = 0x3D5;
static constexpr uint16_t APCI_PROPVALUE_WRITE = 0x3D7;
static constexpr uint16_t APCI_MEMORY_READ = 0x200;
static constexpr uint16_t APCI_ADC_READ = 0x180;

/**********************************************************************
 *************************** APDU BUILDERS ****************************
 **********************************************************************/
// Each writes the connectionless TPDU form ([byte0=TPCI|APCI9:8][byte1=APCI7:0]...) into `out` and returns
// the number of APDU octets. TPCI bits are 0 (T_Data_Individual); the CO path re-stamps byte0 with the
// numbered TPCI at send time. Free functions so the self-test can call them.

/**
 * @brief Build A_FunctionPropertyCommand (APCI 0x2C7, §1.1): [0x02][0xC7][objIdx][pid][data...].
 */
static uint8_t buildFunctionPropertyCommand(uint8_t* out, uint8_t objectIndex, uint8_t propertyId,
                                            const uint8_t* data, uint8_t length)
{
    // [0x02][0xC7][objIdx][pid][data...]   (APCI 0x2C7, §1.1)
    out[0] = (uint8_t)((APCI_FUNC_PROP_CMD >> 8) & 0x03);
    out[1] = (uint8_t)(APCI_FUNC_PROP_CMD & 0xFF);
    out[2] = objectIndex;
    out[3] = propertyId;
    if (data && length) memcpy(&out[4], data, length);
    return (uint8_t)(4 + length);
}

/**
 * @brief Build A_PropertyValue_Read (APCI 0x3D5, §1.2).
 */
static uint8_t buildPropertyValueRead(uint8_t* out, uint8_t objectIndex, uint8_t propertyId, uint8_t count,
                                      uint16_t startIndex)
{
    // [0x03][0xD5][objIdx][pid][ count<<4 | startIdx[11:8] ][ startIdx[7:0] ]   (§1.2)
    out[0] = (uint8_t)((APCI_PROPVALUE_READ >> 8) & 0x03);
    out[1] = (uint8_t)(APCI_PROPVALUE_READ & 0xFF);
    out[2] = objectIndex;
    out[3] = propertyId;
    out[4] = (uint8_t)(((count & 0x0F) << 4) | ((startIndex >> 8) & 0x0F));
    out[5] = (uint8_t)(startIndex & 0xFF);
    return 6;
}

/**
 * @brief Build A_PropertyValue_Write (APCI 0x3D7, §1.3): read header + appended data bytes.
 */
static uint8_t buildPropertyValueWrite(uint8_t* out, uint8_t objectIndex, uint8_t propertyId, uint8_t count,
                                       uint16_t startIndex, const uint8_t* data, uint8_t length)
{
    // Read header + appended data bytes (§1.3)
    out[0] = (uint8_t)((APCI_PROPVALUE_WRITE >> 8) & 0x03);
    out[1] = (uint8_t)(APCI_PROPVALUE_WRITE & 0xFF);
    out[2] = objectIndex;
    out[3] = propertyId;
    out[4] = (uint8_t)(((count & 0x0F) << 4) | ((startIndex >> 8) & 0x0F));
    out[5] = (uint8_t)(startIndex & 0xFF);
    if (data && length) memcpy(&out[6], data, length);
    return (uint8_t)(6 + length);
}

/**
 * @brief Build A_DeviceDescriptor_Read (APCI 0x300, §1.4), descriptorType 0.
 */
static uint8_t buildDeviceDescriptorRead(uint8_t* out)
{
    // [0x03][0x00]  descriptorType 0 OR-ed into byte1 low 6 bits (§1.4)
    out[0] = (uint8_t)((APCI_DEVDESC_READ >> 8) & 0x03);
    out[1] = (uint8_t)(APCI_DEVDESC_READ & 0xFF);
    return 2;
}

/**
 * @brief Build A_Memory_Read (APCI 0x200, §1.5); `number` (byte count) OR-ed into byte1 low 6 bits.
 */
static uint8_t buildMemoryRead(uint8_t* out, uint8_t number, uint16_t memoryAddress)
{
    // [0x02][0x00 | number&0x3F][addrHi][addrLo]  (§1.5)
    out[0] = (uint8_t)((APCI_MEMORY_READ >> 8) & 0x03);
    out[1] = (uint8_t)((APCI_MEMORY_READ & 0xFF) | (number & 0x3F));
    out[2] = (uint8_t)(memoryAddress >> 8);
    out[3] = (uint8_t)(memoryAddress & 0xFF);
    return 4;
}

/**
 * @brief Build A_ADC_Read (APCI 0x180): [0x01][0x80 | channel&0x3F][readCount].
 */
static uint8_t buildAdcRead(uint8_t* out, uint8_t channelNr, uint8_t readCount)
{
    // A_ADC_Read: [0x01][0x80 | channel&0x3F][readCount]  (channel in the low 6 bits, like §1.5 memory)
    out[0] = (uint8_t)((APCI_ADC_READ >> 8) & 0x03);
    out[1] = (uint8_t)((APCI_ADC_READ & 0xFF) | (channelNr & 0x3F));
    out[2] = readCount;
    return 3;
}

#ifndef FTC_TUNNEL_SELFTEST

/**********************************************************************
 *********************** KNXNET/IP CONSTANTS ************************
 **********************************************************************/
// From knx_ip_frame.h / ip_host_protocol_address_information.h.
static constexpr uint8_t KNXIP_HEADER_LEN = 0x06;
static constexpr uint8_t KNXIP_VERSION = 0x10;
static constexpr uint8_t LEN_HPAI = 0x08;
static constexpr uint8_t HPAI_IPV4_UDP = 0x01;

static constexpr uint16_t ST_CONNECT_REQUEST = 0x0205;
static constexpr uint16_t ST_CONNECT_RESPONSE = 0x0206;
static constexpr uint16_t ST_CONNECTIONSTATE_REQUEST = 0x0207;
static constexpr uint16_t ST_CONNECTIONSTATE_RESPONSE = 0x0208;
static constexpr uint16_t ST_DISCONNECT_REQUEST = 0x0209;
static constexpr uint16_t ST_DISCONNECT_RESPONSE = 0x020A;
static constexpr uint16_t ST_TUNNELING_REQUEST = 0x0420;
static constexpr uint16_t ST_TUNNELING_ACK = 0x0421;

static constexpr uint8_t CONN_TUNNEL = 0x04;      // CRI/CRD connection type: TUNNEL_CONNECTION
static constexpr uint8_t TUNNEL_LINKLAYER = 0x02; // CRI KNX layer: link layer

// cEMI message codes (knx_types.h). L_Data.con 0x2E confirmations of our own TX are simply not
// MC_LDATA_IND, so the dispatcher ignores them.
static constexpr uint8_t MC_LDATA_REQ = 0x11;
static constexpr uint8_t MC_LDATA_IND = 0x29;

// APCI (10-bit, collapsed) — response services we decode (§5)
static constexpr uint16_t APCI_FUNC_PROP_STATE_RESP = 0x2C9;
static constexpr uint16_t APCI_PROPVALUE_RESP = 0x3D6;
static constexpr uint16_t APCI_DEVDESC_RESP = 0x340;
static constexpr uint16_t APCI_MEMORY_RESP = 0x240;
static constexpr uint16_t APCI_ADC_RESP = 0x1C0;

// TPCI control TPDUs (§6.1)
static constexpr uint8_t TPCI_CONNECT = 0x80;
static constexpr uint8_t TPCI_DISCONNECT = 0x81;
static constexpr uint8_t TPCI_ACK = 0xC2;       // | (seq<<2)
static constexpr uint8_t TPCI_DATA_CONN = 0x40; // | (seq<<2)

// cEMI Ctrl priority (bits3-2)
static constexpr uint8_t PRIO_SYSTEM = 0x00;
static constexpr uint8_t PRIO_LOW = 0x03;

static constexpr uint32_t HEARTBEAT_MS = 60000; // CONNECTIONSTATE_REQUEST cadence
static constexpr int RX_BUF = 1024;
static constexpr uint32_t TX_ACK_TIMEOUT_MS = 1000; // KNXnet/IP tunneling: TUNNELING_ACK deadline before retransmit (03_08_04 §2.6)
static constexpr uint8_t TX_ACK_MAX_RETRIES = 5;    // retransmits of the same (seq) frame before the slot is freed

// NOTE ON TX PIPELINING (why there is exactly one in-flight frame): we tried sending a second TUNNELING_REQUEST
// before the first was ACKed, to overlap the tunnel round-trip with the ~290 ms TP wire time (a measured
// ~194 ms/chunk idle gap). HW test proved real interfaces enforce the KNXnet/IP 1-outstanding rule: the second
// frame is dropped, no ACK ever comes, the in-flight slot never frees and the whole transfer wedges. So the
// transport is strictly 1-outstanding (see the TxInflight slot in TunnelState). Overlapping the RTT would need
// a per-interface capability probe first -- do NOT reintroduce multi-outstanding sending blindly.

static uint32_t nowMs()
{
    using namespace std::chrono;
    static const steady_clock::time_point t0 = steady_clock::now();
    return (uint32_t)duration_cast<milliseconds>(steady_clock::now() - t0).count();
}

static inline void put16(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

/**********************************************************************
 ************************* TRANSPORT STATE **************************
 **********************************************************************/
// Translation-unit-local transport state (single tunnel; see file header).
namespace
{
struct TunnelState
{
    sock_t sock = SOCK_INVALID;
    sockaddr_in server{};   // KNXnet/IP server (data + control endpoint)
    uint32_t localIp = 0;   // our outgoing IP, host order (a<<24|b<<16|c<<8|d)
    uint16_t localPort = 0; // our local UDP port, host order

    uint8_t channelId = 0;
    uint8_t txSeq = 0; // outgoing TUNNELING_REQUEST seq
    uint8_t rxSeq = 0; // last accepted inbound tunnel seq (server-driven)
    bool rxSeqValid = false;
    std::deque<std::vector<uint8_t>> txQueue; // cEMI frames waiting to go out (fast/forget window)

    // The SINGLE in-flight TUNNELING_REQUEST: sent, awaiting its TUNNELING_ACK. KNXnet/IP tunnelling is
    // strictly 1-outstanding (03_08_04 §2.6): the client must wait for each request's ACK before sending the
    // next. This is not just advisory -- real interfaces ENFORCE it (HW-verified: a second request sent before
    // the first is ACKed is silently dropped and never ACKed, which wedges the transfer). So there is at most
    // ONE unacked frame at any moment; a std::deque would only ever hold 0 or 1 entries, so we keep a single
    // slot instead. `txInflightBusy` marks it occupied; `txInflight.body` keeps the whole frame (connection
    // header + cEMI) so a lost/late ACK can be resent verbatim -- same seq, so the server dedups the frame it
    // already processed -- until the ACK arrives or the retry budget is spent, either of which frees the slot.
    struct TxInflight
    {
        std::vector<uint8_t> body;
        uint32_t sentMs;
        uint8_t retries;
    };
    TxInflight txInflight;       // the one frame awaiting its ACK (valid only while txInflightBusy)
    bool txInflightBusy = false; // true from send until ACK / retry-budget-exhausted

    uint32_t lastHeartbeat = 0; // last CONNECTIONSTATE_REQUEST sent

    // Connection-oriented scan session (§6)
    bool coConnected = false;
    bool coAwaitingAck = false; // a numbered T_Data_Connected is out, awaiting T_ACK
    bool coReadAcked = false;   // the device T_ACKed our CO read -> present (presence-on-ACK, faster than the DD response)
    uint16_t coPa = 0;
    uint8_t coSeqSend = 0;
    uint8_t coSeqRecv = 0;
};
TunnelState s;

    #ifdef _WIN32
bool ensureWinsock()
{
    static bool done = false;
    if (done) return true;
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return false;
    done = true;
    return true;
}
    #endif

void setNonBlocking(sock_t fd)
{
    #ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(fd, FIONBIO, &mode);
    #else
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    #endif
}

/**
 * @brief Assemble + send a KNXnet/IP frame; `body` is everything after the 6-byte header.
 */
bool sendKnxIp(uint16_t serviceType, const uint8_t* body, uint16_t bodyLen)
{
    uint8_t pkt[RX_BUF];
    uint16_t total = (uint16_t)(KNXIP_HEADER_LEN + bodyLen);
    if (total > sizeof(pkt)) return false;
    pkt[0] = KNXIP_HEADER_LEN;
    pkt[1] = KNXIP_VERSION;
    put16(&pkt[2], serviceType);
    put16(&pkt[4], total);
    if (bodyLen) memcpy(&pkt[6], body, bodyLen);
    int n = (int)::send(s.sock, (const char*)pkt, total, 0);
    return n == (int)total;
}

/**
 * @brief Fill an 8-byte HPAI with our local control/data endpoint.
 */
void putLocalHpai(uint8_t* p)
{
    p[0] = LEN_HPAI;
    p[1] = HPAI_IPV4_UDP;
    p[2] = (uint8_t)(s.localIp >> 24);
    p[3] = (uint8_t)(s.localIp >> 16);
    p[4] = (uint8_t)(s.localIp >> 8);
    p[5] = (uint8_t)(s.localIp & 0xFF);
    put16(&p[6], s.localPort);
}

/**
 * @brief Build a cEMI L_Data.req into `out` and return its length.
 * @param tpdu full TPDU starting at byte0.
 */
uint16_t buildCemi(uint8_t* out, uint16_t sa, uint16_t da, const uint8_t* tpdu, uint8_t tpduLen,
                   uint8_t priority, bool ackReq)
{
    uint8_t len = (uint8_t)(tpduLen - 1); // NPDU octetCount = TPDU bytes after byte0
    uint8_t ctrl1 = 0;
    ctrl1 |= (len <= 15) ? 0x80 : 0x00; // FrameType: standard / extended
    ctrl1 |= 0x20;                      // do-not-repeat
    ctrl1 |= 0x10;                      // broadcast domain
    ctrl1 |= (uint8_t)((priority & 0x03) << 2);
    ctrl1 |= ackReq ? 0x02 : 0x00; // AckRequest
    uint8_t ctrl2 = 0x60;          // individual dest, hopcount 6, EFF 0

    out[0] = MC_LDATA_REQ;
    out[1] = 0x00; // AddIL
    out[2] = ctrl1;
    out[3] = ctrl2;
    put16(&out[4], sa);
    put16(&out[6], da);
    out[8] = len;
    memcpy(&out[9], tpdu, tpduLen);
    return (uint16_t)(9 + tpduLen);
}

// Monotonic count of frames we have transmitted. The host CLI watches it to detect "command finished"
// for read chains (info/df/scan) that run on their own sub-state and never flip isBusy().
uint32_t g_txActivity = 0;
// Monotonic RX + discard counters for the console status bar: g_rxActivity = received bus indications,
// g_dropActivity = frames dropped as truncated/oversize (surfaced so the loss is never silent).
uint32_t g_rxActivity = 0;
uint32_t g_dropActivity = 0;

// Virtual TP-FIFO (in frames) for fast/forget pacing. The KNXnet/IP tunnel ACKs every frame almost
// instantly, so the real ~350 B/s TP bottleneck is invisible to the sender -> the fast pump sees an empty
// FIFO and blasts the whole window, overrunning the interface's TP transmit FIFO (drops, "deadline
// exceeded"). This models that FIFO: +1 per transmitted FTC frame, drained at the current send rate;
// ftcTxQueueSize() then paces on FTC_TX_HIGH/LOW exactly as on the device.
double g_vfifoFrames = 0.0;
uint32_t g_vfifoLastMs = 0;
double g_vfifoFrameBytes = 245.0; // bytes of the last transmitted frame -> the Bps rate maps to a frames/s drain

// Delivery-rate (BBR-style) send pacing -- NOT a fixed ceiling. The FTC report gives the MEASURED delivered
// rate each window (bytes the target confirmed / elapsed): a clean window probes higher, a lossy window
// snaps the rate to the measured wire ceiling (no guessing), a report-timeout kick backs off. So the link,
// via what it actually delivers, sets the rate -- it finds the top, holds just under it, and self-recovers
// from a kick. Byte-based, since APDU auto-framing makes frame sizes vary. VFIFO_* are control gains.
double g_vfifoDrainBps = 450.0;          // launch (safe); the delivery-rate loop takes over immediately
constexpr double VFIFO_RATE_MIN = 120.0; // floor so a burst of losses cannot stall the transfer entirely
constexpr double VFIFO_PROBE = 1.12;     // clean window -> probe the send rate up this much
constexpr double VFIFO_MARGIN = 0.95;    // on loss, pace to 95% of the measured wire rate (small headroom)
constexpr double VFIFO_KICK = 0.8;       // report-timeout kick -> multiplicative back-off (no sample)

/**
 * @brief Drain the modeled TP-FIFO by the elapsed time at the current adaptive send rate.
 */
void vfifoDrain()
{
    uint32_t now = nowMs();
    if (g_vfifoLastMs == 0)
    {
        g_vfifoLastMs = now;
        return;
    }
    const double fps = g_vfifoDrainBps / (g_vfifoFrameBytes > 8.0 ? g_vfifoFrameBytes : 8.0);
    g_vfifoFrames -= (double)(now - g_vfifoLastMs) * (fps / 1000.0);
    g_vfifoLastMs = now;
    if (g_vfifoFrames < 0.0) g_vfifoFrames = 0.0;
}

/**
 * @brief Put ONE queued cEMI on the wire, but only when the single in-flight slot is free.
 * @details KNXnet/IP tunnelling is strictly 1-outstanding: we must wait for the previous frame's
 *          TUNNELING_ACK before sending the next. The fast/forget pump enqueues a whole window from
 *          loop(); this releases exactly one frame each time the slot frees (on ACK or retransmit
 *          exhaustion), so the interface never sees more than one unacked request.
 */
void drainTxQueue()
{
    if (s.txInflightBusy) return; // slot occupied -> wait for its ACK (strict 1-outstanding)

    // Drop any oversize frame that cannot fit the send buffer (a guard; FTC APDUs never reach this size).
    while (!s.txQueue.empty() && (uint16_t)(4 + s.txQueue.front().size()) > RX_BUF)
    {
        s.txQueue.pop_front();
        g_dropActivity++;
    }
    if (s.txQueue.empty()) return;

    std::vector<uint8_t>& cemi = s.txQueue.front();
    uint8_t body[RX_BUF];
    body[0] = 0x04; // connection header length
    body[1] = s.channelId;
    body[2] = s.txSeq;
    body[3] = 0x00;
    memcpy(&body[4], cemi.data(), cemi.size());
    const uint16_t flen = (uint16_t)(4 + cemi.size());
    if (!sendKnxIp(ST_TUNNELING_REQUEST, body, flen)) return; // socket busy -> keep it queued, retry next pump

    s.txSeq = (uint8_t)(s.txSeq + 1);
    s.txInflight = {std::vector<uint8_t>(body, body + flen), nowMs(), 0}; // occupy the slot for ACK + retransmit
    s.txInflightBusy = true;
    g_txActivity++; // completion-quiescence signal for the host CLI (commands with their own sub-state)
    vfifoDrain();   // model the TP transmit FIFO for fast/forget flow control (see txQueueSize)
    g_vfifoFrames += 1.0;
    g_vfifoFrameBytes = (double)cemi.size(); // this frame's size -> the adaptive Bps maps to the right fps
    s.txQueue.pop_front();
}

/**
 * @brief Enqueue a cEMI for tunneled transmission; drainTxQueue() sends it 1-outstanding.
 * @details Returns false only if the queue is full (backpressure to the fast/forget pump). Fires an
 *          immediate drain so single frames (safe mode, control, T_ACK) go out with no added latency.
 */
bool sendTunnel(const uint8_t* cemi, uint16_t cemiLen)
{
    static const size_t TX_QUEUE_CAP = 48;
    if (s.txQueue.size() >= TX_QUEUE_CAP) return false;
    s.txQueue.emplace_back(cemi, cemi + cemiLen);
    drainTxQueue();
    return true;
}

/**
 * @brief Send a TUNNELING_ACK echoing the server's seq.
 */
void sendTunnelAck(uint8_t seq)
{
    uint8_t body[4] = {0x04, s.channelId, seq, 0x00};
    sendKnxIp(ST_TUNNELING_ACK, body, 4);
}

/**
 * @brief Send a CONNECTIONSTATE_REQUEST (tunnel keep-alive) and stamp lastHeartbeat.
 */
void sendConnectionStateRequest()
{
    uint8_t body[2 + LEN_HPAI];
    body[0] = s.channelId;
    body[1] = 0x00;
    putLocalHpai(&body[2]);
    sendKnxIp(ST_CONNECTIONSTATE_REQUEST, body, sizeof(body));
    s.lastHeartbeat = nowMs();
}

/**
 * @brief One non-blocking receive; false on wouldblock/error. len is clamped to cap.
 */
bool recvOne(uint8_t* buf, int cap, int& len)
{
    int n = (int)::recv(s.sock, (char*)buf, cap, 0);
    if (n <= 0) return false;
    len = n;
    return true;
}

/**
 * @brief The FTC callbacks, read from the (private) class members by pump() and threaded down to the
 *        dispatcher — free functions cannot see the private fields, and the header contract is fixed.
 */
struct DispatchCbs
{
    FtcResponseCb resp;
    FtcDdCb dd;
    FtcPropCb prop;
    FtcMemCb mem;
    FtcAdcCb adc;
};

} // namespace

/**********************************************************************
 ************************* RECEIVE / DISPATCH ***********************
 **********************************************************************/

/**
 * @brief Decode one response APDU (§5) and fire the matching FTC callback.
 * @param apduData &tpdu[1] (byte after TPCI).
 * @param tpduByte0 the TPDU byte0, carrying TPCI + APCI[9:8].
 * @param apduLen NPDU octetCount.
 */
static void dispatchResponse(const DispatchCbs& cb, uint16_t sa, uint8_t tpduByte0, uint8_t* apduData,
                             uint8_t apduLen)
{
    if (apduLen < 1) return;
    // Recover the 10-bit APCI exactly as apdu.cpp:9-18 (byte0 low2 = APCI[9:8], byte1 = APCI[7:0]).
    uint16_t apci = (uint16_t)(((tpduByte0 << 8) | apduData[0]) & 0x3FF);
    uint16_t hi = (uint16_t)(apci >> 6);
    if (hi < 11 && hi != 7) apci &= 0x3C0; // collapse short APCIs (apdu.cpp)
    else if (hi == 7)
        apci &= 0x3C0; // ADCResponse (0x1C0): channel in the low 6 bits + data[0], collapse to the opcode

    switch (apci)
    {
        case APCI_FUNC_PROP_STATE_RESP: // §5.1
        {
            if (apduLen < 3) return;
            uint8_t objIdx = apduData[1];
            uint8_t pid = apduData[2];
            uint8_t* payload = &apduData[3];
            uint8_t len = (uint8_t)(apduLen - 3);
            if (cb.resp) cb.resp(sa, objIdx, pid, payload, len);
            break;
        }
        case APCI_PROPVALUE_RESP: // §5.2
        {
            if (apduLen < 5) return;
            uint8_t objIdx = apduData[1];
            uint8_t pid = apduData[2];
            const uint8_t* payload = &apduData[5];
            uint8_t len = (uint8_t)(apduLen - 5);
            if (cb.prop) cb.prop(sa, objIdx, pid, payload, len);
            break;
        }
        case APCI_DEVDESC_RESP: // §5.3
        {
            if (apduLen < 3) return; // descriptorType byte + 2 mask bytes
            uint8_t descriptorType = (uint8_t)(apduData[0] & 0x3F);
            if (cb.dd) cb.dd(sa, descriptorType, &apduData[1]);
            break;
        }
        case APCI_MEMORY_RESP: // §5.4
        {
            if (apduLen < 3) return;
            uint8_t count = (uint8_t)(apduData[0] & 0x3F);
            if (count > (uint8_t)(apduLen - 3)) return;
            uint16_t memAddr = (uint16_t)((apduData[1] << 8) | apduData[2]);
            if (cb.mem) cb.mem(sa, memAddr, &apduData[3], count);
            break;
        }
        case APCI_ADC_RESP: // A_ADC_Response: [channel in data[0]&0x3F][count][value:2 BE] (e.g. BCU bus-voltage read)
        {
            if (apduLen < 4) return;
            uint8_t channel = (uint8_t)(apduData[0] & 0x3F);
            uint8_t count = apduData[1];
            int16_t value = (int16_t)((apduData[2] << 8) | apduData[3]);
            if (cb.adc) cb.adc(sa, channel, count, value);
            break;
        }
        default:
            break; // unknown / not an FTC answer
    }
}

/**
 * @brief Parse one inbound cEMI and dispatch (connectionless, CO-numbered data, or CO control TPDUs).
 * @param selfPa our tunnel PA (source for the CO T_ACK we emit).
 */
static void handleCemi(uint16_t selfPa, const DispatchCbs& cb, uint8_t* cemi, int cemiLen)
{
    if (cemiLen < 2) return;
    uint8_t mc = cemi[0];
    uint8_t addil = cemi[1];
    int base = 2 + addil; // start of Ctrl1
    // need Ctrl1,Ctrl2,SA(2),DA(2),LEN = 7 header bytes + at least 1 TPDU byte
    if (base + 7 + 1 > cemiLen)
    {
        g_dropActivity++;
        return;
    } // malformed / too short
    uint16_t sa = (uint16_t)((cemi[base + 2] << 8) | cemi[base + 3]);
    uint8_t len = cemi[base + 6]; // NPDU octetCount
    uint8_t* tpdu = &cemi[base + 7];
    int tpduLen = len + 1;
    if (base + 7 + tpduLen > cemiLen)
    {
        g_dropActivity++;
        return;
    } // truncated — never over-read

    // Only indications carry answers. Confirmations (L_Data.con 0x2E) of our own TX are ignored here.
    if (mc != MC_LDATA_IND) return;
    g_rxActivity++; // a received bus indication (RX activity for the status bar)

    uint8_t tpci = tpdu[0];

    // CO control TPDUs (§6.1)
    if (tpci == TPCI_DISCONNECT)
    {
        s.coConnected = false;
        s.coAwaitingAck = false;
        return;
    }
    if ((tpci & 0xC3) == TPCI_ACK) // T_ACK: our numbered send was acknowledged
    {
        if (s.coAwaitingAck)
        {
            s.coSeqSend = (uint8_t)((s.coSeqSend + 1) & 0x0F);
            s.coAwaitingAck = false;
            s.coReadAcked = true; // the device acknowledged our CO read -> present (faster/surer than the DD response)
        }
        return;
    }
    if ((tpci & 0xC0) == TPCI_DATA_CONN) // numbered T_Data_Connected: CO response data
    {
        uint8_t seq = (uint8_t)((tpci >> 2) & 0x0F);
        if (seq == s.coSeqRecv)
        {
            // ACK then deliver, advance expected recv seq.
            uint8_t ackTpdu[1] = {(uint8_t)(TPCI_ACK | (seq << 2))};
            uint8_t cbuf[16];
            uint16_t cl = buildCemi(cbuf, selfPa, sa, ackTpdu, 1, PRIO_SYSTEM, false); // AckDontCare (tunnel-PA)
            sendTunnel(cbuf, cl);
            s.coSeqRecv = (uint8_t)((s.coSeqRecv + 1) & 0x0F);
            dispatchResponse(cb, sa, tpdu[0], &tpdu[1], len);
        }
        else if (seq == (uint8_t)((s.coSeqRecv - 1) & 0x0F))
        {
            // Duplicate: re-ACK, do not re-deliver.
            uint8_t ackTpdu[1] = {(uint8_t)(TPCI_ACK | (seq << 2))};
            uint8_t cbuf[16];
            uint16_t cl = buildCemi(cbuf, selfPa, sa, ackTpdu, 1, PRIO_SYSTEM, false); // AckDontCare (tunnel-PA)
            sendTunnel(cbuf, cl);
        }
        return;
    }

    // Connectionless T_Data_Individual (TPCI top bits 00): the normal FTC answer path.
    if ((tpci & 0xC0) == 0x00) dispatchResponse(cb, sa, tpdu[0], &tpdu[1], len);
}

/**********************************************************************
 ******************* PUBLIC API — CONNECTION MGMT *******************
 **********************************************************************/

/**
 * @brief Open the tunnel: CONNECT_REQUEST, then a bounded wait for CONNECT_RESPONSE.
 * @details Setup-only blocking (~3s); pump() never blocks. On success stores the channel id and the CRD
 *          individual address as our source PA. Records the CONNECT_RESPONSE status in _lastConnectStatus.
 */
bool KnxIpTunnel::connect(const std::string& ip, uint16_t port)
{
    if (_connected) return true;
    _lastConnectStatus = -1; // no CONNECT_RESPONSE seen yet (stays -1 on a pure timeout)
    #ifdef _WIN32
    if (!ensureWinsock()) return false;
    #endif
    s = TunnelState(); // reset transport state

    s.sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s.sock == SOCK_INVALID) return false;

    memset(&s.server, 0, sizeof(s.server));
    s.server.sin_family = AF_INET;
    s.server.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &s.server.sin_addr) != 1)
    {
        SOCK_CLOSE(s.sock);
        s.sock = SOCK_INVALID;
        return false;
    }

    // connect() fixes the outgoing interface so getsockname() yields our real local endpoint, and lets
    // us use send()/recv(). All server traffic (control + data) originates from serverIP:port.
    if (::connect(s.sock, (sockaddr*)&s.server, sizeof(s.server)) != 0)
    {
        SOCK_CLOSE(s.sock);
        s.sock = SOCK_INVALID;
        return false;
    }
    sockaddr_in local{};
    socklen_t ll = sizeof(local);
    if (getsockname(s.sock, (sockaddr*)&local, &ll) != 0)
    {
        SOCK_CLOSE(s.sock);
        s.sock = SOCK_INVALID;
        return false;
    }
    s.localIp = ntohl(local.sin_addr.s_addr);
    s.localPort = ntohs(local.sin_port);
    setNonBlocking(s.sock);

    // CONNECT_REQUEST: HPAI(ctrl) + HPAI(data) + CRI(tunnel/linklayer).
    uint8_t body[LEN_HPAI + LEN_HPAI + 4];
    putLocalHpai(&body[0]);
    putLocalHpai(&body[LEN_HPAI]);
    body[LEN_HPAI * 2 + 0] = 0x04;             // CRI length
    body[LEN_HPAI * 2 + 1] = CONN_TUNNEL;      // TUNNEL_CONNECTION
    body[LEN_HPAI * 2 + 2] = TUNNEL_LINKLAYER; // link layer
    body[LEN_HPAI * 2 + 3] = 0x00;             // reserved
    if (!sendKnxIp(ST_CONNECT_REQUEST, body, sizeof(body)))
    {
        SOCK_CLOSE(s.sock);
        s.sock = SOCK_INVALID;
        return false;
    }

    // Bounded wait for CONNECT_RESPONSE. Spec 03_08_02 Core §5.2: CONNECT_REQUEST_TIMEOUT = 10 s.
    // Setup-only blocking; pump() never blocks.
    uint32_t deadline = nowMs() + 10000;
    uint8_t buf[RX_BUF];
    while ((int32_t)(deadline - nowMs()) > 0)
    {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(s.sock, &rfds);
        timeval tv{0, 100000}; // 100 ms slices
        int sel = ::select((int)s.sock + 1, &rfds, nullptr, nullptr, &tv);
        if (sel <= 0) continue;
        int n = 0;
        if (!recvOne(buf, sizeof(buf), n)) continue;
        if (n < KNXIP_HEADER_LEN) continue;
        uint16_t st = (uint16_t)((buf[2] << 8) | buf[3]);
        if (st != ST_CONNECT_RESPONSE) continue;
        if (n < KNXIP_HEADER_LEN + 2) continue;
        uint8_t channelId = buf[6];
        uint8_t status = buf[7];
        _lastConnectStatus = status; // record for the caller's diagnostics (0 = ok, else E_* error)
        if (status != 0x00) break;   // E_* error -> the server refused this connection
        // CRD at offset header(6)+ch(1)+status(1)+HPAI(8) = 16: [len][type][addrHi][addrLo]
        int crd = KNXIP_HEADER_LEN + 1 + 1 + LEN_HPAI;
        if (n >= crd + 4 && buf[crd + 1] == CONN_TUNNEL)
            _assignedPA = (uint16_t)((buf[crd + 2] << 8) | buf[crd + 3]);
        s.channelId = channelId;
        s.lastHeartbeat = nowMs();
        _connected = true;
        return true;
    }

    SOCK_CLOSE(s.sock);
    s.sock = SOCK_INVALID;
    return false;
}

/**
 * @brief Send DISCONNECT_REQUEST (if connected) and close the socket.
 */
void KnxIpTunnel::disconnect()
{
    if (s.sock == SOCK_INVALID)
    {
        _connected = false;
        return;
    }
    if (_connected)
    {
        uint8_t body[2 + LEN_HPAI];
        body[0] = s.channelId;
        body[1] = 0x00;
        putLocalHpai(&body[2]);
        sendKnxIp(ST_DISCONNECT_REQUEST, body, sizeof(body));
    }
    SOCK_CLOSE(s.sock);
    s.sock = SOCK_INVALID;
    _connected = false;
    _assignedPA = 0;
}

/**
 * @brief One non-blocking pass over the tunnel: RX drain + dispatch, TX-ACK retransmit, drain queue, heartbeat.
 * @details Drains every available datagram (bounded guard), ACKs each TUNNELING_REQUEST (even duplicates),
 *          frees the 1-outstanding slot on a matching TUNNELING_ACK, retransmits a timed-out request, and
 *          sends the CONNECTIONSTATE_REQUEST keep-alive. A server DISCONNECT_REQUEST tears us down here.
 */
void KnxIpTunnel::pump()
{
    if (!_connected || s.sock == SOCK_INVALID) return;

    // Drain every currently-available datagram; stop at wouldblock. Safety cap avoids a runaway loop.
    uint8_t buf[RX_BUF];
    for (int guard = 0; guard < 256; ++guard)
    {
        int n = 0;
        if (!recvOne(buf, sizeof(buf), n)) break; // wouldblock or error
        if (n < KNXIP_HEADER_LEN) continue;
        uint16_t st = (uint16_t)((buf[2] << 8) | buf[3]);
        uint16_t total = (uint16_t)((buf[4] << 8) | buf[5]);
        if (total > n) continue; // truncated header claim — drop

        switch (st)
        {
            case ST_TUNNELING_REQUEST:
            {
                if (n < KNXIP_HEADER_LEN + 4) break;
                uint8_t connLen = buf[6];
                uint8_t chan = buf[7];
                uint8_t seq = buf[8];
                if (connLen != 0x04 || chan != s.channelId) break;
                // Spec 03_08_04 §2.6.1 (client receive rules): accept only seq == expected (last accepted + 1);
                // a retransmit of the last accepted seq is ACKed but discarded; ANY other seq is neither ACKed
                // nor processed. The old code ACKed + re-processed every non-duplicate seq, so a stale/reordered
                // frame (e.g. seq 8 after 10) got re-dispatched -> a double-delivered FTC response / console line.
                if (s.rxSeqValid)
                {
                    const uint8_t expected = (uint8_t)(s.rxSeq + 1);
                    if (seq == s.rxSeq) { sendTunnelAck(seq); break; } // retransmit of last accepted -> ACK, drop
                    if (seq != expected) break;                        // out of window -> neither ACK nor process
                }
                sendTunnelAck(seq);
                s.rxSeq = seq;
                s.rxSeqValid = true;
                int cemiOff = KNXIP_HEADER_LEN + 4;
                int cemiLen = (int)total - cemiOff;
                DispatchCbs cb{_responseCb, _ddCb, _propCb, _memCb, _adcCb};
                if (cemiLen > 0 && cemiOff + cemiLen <= n) handleCemi(_assignedPA, cb, &buf[cemiOff], cemiLen);
                break;
            }
            case ST_TUNNELING_ACK:
            {
                // Free the in-flight slot when the ACK's seq matches the frame we sent (its seq = body[2]). A
                // duplicate/stale ACK, or one arriving after the slot is already free, finds no match and is
                // ignored -- so the slot can never be freed twice or under a wrong seq. Require a FULL, in-channel
                // ACK first: a truncated frame (n < 10), a wrong connLen, or a foreign channelId is dropped, so a
                // malformed/stray ACK can never spuriously free the slot (the old `ackSeq = 0` default could have,
                // had the in-flight seq been 0). A genuinely lost ACK is still rescued by the retransmit below.
                if (n >= KNXIP_HEADER_LEN + 4 && buf[6] == 0x04 && buf[7] == s.channelId)
                {
                    const uint8_t ackSeq = buf[8];
                    const uint8_t ackStatus = buf[9]; // 03_08_04 §2.6.1: only E_NO_ERROR(0) confirms; an error
                                                      // status is NOT a valid confirmation -> leave the slot in
                                                      // flight so the retransmit/DISCONNECT path below handles it.
                    if (ackStatus == 0 && s.txInflightBusy && s.txInflight.body.size() > 2 && s.txInflight.body[2] == ackSeq)
                        s.txInflightBusy = false;
                }
                break;
            }
            case ST_CONNECTIONSTATE_RESPONSE:
                break; // heartbeat reply — connection alive
            case ST_DISCONNECT_REQUEST:
            {
                // Server tears us down: reply, then mark closed.
                uint8_t body[2] = {s.channelId, 0x00};
                sendKnxIp(ST_DISCONNECT_RESPONSE, body, 2);
                SOCK_CLOSE(s.sock);
                s.sock = SOCK_INVALID;
                _connected = false;
                return;
            }
            case ST_DISCONNECT_RESPONSE:
                break; // ack of our own disconnect
            default:
                break;
        }
    }

    // TUNNELING_ACK retransmit: a lost/late ACK must not wedge the in-flight slot forever. Once the frame is past
    // its ACK deadline, resend it verbatim (same seq -> the server dedups a frame it already processed); after the
    // retry budget is spent, free the slot so the FTC layer's own retry/abort can act instead of hanging.
    if (_connected && s.txInflightBusy && (uint32_t)(nowMs() - s.txInflight.sentMs) >= TX_ACK_TIMEOUT_MS)
    {
        if (s.txInflight.retries < TX_ACK_MAX_RETRIES)
        {
            sendKnxIp(ST_TUNNELING_REQUEST, s.txInflight.body.data(), (uint16_t)s.txInflight.body.size());
            s.txInflight.sentMs = nowMs();
            s.txInflight.retries++;
        }
        else
        {
            // Spec 03_08_04 §2.6.1: an unconfirmed request must ultimately terminate the connection. Merely
            // freeing the slot advances txSeq past the lost frame -> the server then sees a sequence gap and
            // silently drops everything (§2.6.1 receive rules) = a wedged tunnel with no recovery. DISCONNECT
            // instead, so the FTC layer detects the drop and can reconnect cleanly.
            s.txInflightBusy = false;
            uint8_t body[2] = {s.channelId, 0x00};
            sendKnxIp(ST_DISCONNECT_REQUEST, body, 2);
            SOCK_CLOSE(s.sock);
            s.sock = SOCK_INVALID;
            _connected = false;
            return;
        }
    }

    // A TUNNELING_ACK this pass may have freed the 1-outstanding slot -> push the next queued frame.
    drainTxQueue();

    // Heartbeat: CONNECTIONSTATE_REQUEST every ~60 s (non-blocking timer).
    if (_connected && (uint32_t)(nowMs() - s.lastHeartbeat) >= HEARTBEAT_MS) sendConnectionStateRequest();
}

/**********************************************************************
 ***************************** SENDERS ******************************
 **********************************************************************/

/**
 * @brief Wrap an APDU as a cEMI and send.
 * @details Auto-routes CO (numbered T_Data_Connected) when a scan session is open to `da`
 *          (application_layer.cpp individualSend: CO when asap == _connectedTsap); else connectionless.
 */
static bool txApdu(KnxIpTunnel* self, uint16_t da, uint8_t* apdu, uint8_t apduLen, bool ackReq)
{
    if (!self->connected() || s.sock == SOCK_INVALID) return false;
    // Backpressure is the txQueue cap in sendTunnel() (below); it returns false when the queue is full and that
    // propagates up through here. No separate in-flight cap is needed -- the transport is strictly 1-outstanding.

    uint8_t cemi[RX_BUF];
    if (s.coConnected && da == s.coPa)
    {
        // Re-stamp byte0 as numbered T_Data_Connected, keep APCI[9:8] (low 2 bits) + APCI[7:0].
        apdu[0] = (uint8_t)(TPCI_DATA_CONN | (s.coSeqSend << 2) | (apdu[0] & 0x03));
        // AckDontCare: an AckRequested frame sourced from the tunnel PA never gets an L2-ACK (the interface
        // does not proxy it) -> the target never processes it. The transport-layer T_ACK keeps CO reliable.
        uint16_t cl = buildCemi(cemi, self->assignedPA(), da, apdu, apduLen, PRIO_SYSTEM, false);
        if (!sendTunnel(cemi, cl)) return false;
        s.coAwaitingAck = true;
        return true;
    }
    uint16_t cl = buildCemi(cemi, self->assignedPA(), da, apdu, apduLen, PRIO_LOW, ackReq);
    return sendTunnel(cemi, cl);
}

/**
 * @brief Send an A_FunctionPropertyCommand to `pa`.
 */
bool KnxIpTunnel::sendCommand(uint16_t pa, uint8_t objectIndex, uint8_t propertyId, const uint8_t* data,
                              uint8_t length)
{
    if (length > 251) return false; // send guard: 4+length must fit apdu[256] and stay <=255 (uint8); mirrors firmware ftcSendCommand 251 (§1.1)
    uint8_t apdu[256];
    uint8_t n = buildFunctionPropertyCommand(apdu, objectIndex, propertyId, data, length);
    return txApdu(this, pa, apdu, n, false); // AckDontCare: an AckRequested frame from the tunnel PA gets no L2-ACK
}

/**
 * @brief Send a connectionless A_DeviceDescriptor_Read to `pa`.
 */
bool KnxIpTunnel::sendDeviceDescriptorRead(uint16_t pa)
{
    uint8_t apdu[2];
    uint8_t n = buildDeviceDescriptorRead(apdu);
    return txApdu(this, pa, apdu, n, false); // AckDontCare (§1.4)
}

/**
 * @brief Send an A_PropertyValue_Read to `pa`.
 */
bool KnxIpTunnel::sendPropertyValueRead(uint16_t pa, uint8_t objectIndex, uint8_t propertyId, uint8_t count,
                                        uint16_t startIndex)
{
    uint8_t apdu[6];
    uint8_t n = buildPropertyValueRead(apdu, objectIndex, propertyId, count, startIndex);
    return txApdu(this, pa, apdu, n, false); // AckDontCare: AckRequested from the tunnel PA gets no L2-ACK
}

/**
 * @brief Send an A_PropertyValue_Write to `pa`.
 */
bool KnxIpTunnel::sendPropertyValueWrite(uint16_t pa, uint8_t objectIndex, uint8_t propertyId, uint8_t count,
                                         uint16_t startIndex, const uint8_t* data, uint8_t length)
{
    uint8_t apdu[256];
    uint8_t n = buildPropertyValueWrite(apdu, objectIndex, propertyId, count, startIndex, data, length);
    return txApdu(this, pa, apdu, n, false); // AckDontCare (see sendPropertyValueRead)
}

/**
 * @brief Send an A_Memory_Read to `pa`.
 */
bool KnxIpTunnel::sendMemoryRead(uint16_t pa, uint8_t number, uint16_t memoryAddress)
{
    uint8_t apdu[4];
    uint8_t n = buildMemoryRead(apdu, number, memoryAddress);
    return txApdu(this, pa, apdu, n, false); // AckDontCare for the connectionless case (CO path forces its own)
}

/**
 * @brief Send an A_ADC_Read to `pa` (e.g. BCU bus-voltage read).
 */
bool KnxIpTunnel::sendAdcRead(uint16_t pa, uint8_t channelNr, uint8_t readCount)
{
    uint8_t apdu[3];
    uint8_t n = buildAdcRead(apdu, channelNr, readCount);
    return txApdu(this, pa, apdu, n, false);
}

/**********************************************************************
 ******************** CONNECTION-ORIENTED SCAN (§6) *****************
 **********************************************************************/

/**
 * @brief Open a CO session to `pa` (T_Connect). The connection is up on send (a device does not T_ACK it).
 */
bool KnxIpTunnel::scanConnect(uint16_t pa)
{
    if (!_connected || s.sock == SOCK_INVALID) return false;
    if (s.coConnected) return true; // self-guard (§6.2 step 1)
    uint8_t tpdu[1] = {TPCI_CONNECT};
    uint8_t cemi[16];
    uint16_t cl = buildCemi(cemi, _assignedPA, pa, tpdu, 1, PRIO_SYSTEM, false); // AckDontCare (tunnel-PA)
    if (!sendTunnel(cemi, cl)) return false;
    s.coPa = pa;
    s.coSeqSend = 0;
    s.coSeqRecv = 0;
    s.coAwaitingAck = false;
    s.coReadAcked = false;
    // A KNX device does not T_ACK a T_Connect; the connection is up on send (application_layer isConnected()).
    s.coConnected = true;
    return true;
}

bool KnxIpTunnel::scanConnected() { return s.coConnected; }
bool KnxIpTunnel::scanReadAcked() { return s.coReadAcked; }

/**
 * @brief Issue a numbered CO A_DeviceDescriptor_Read on the open session and await its T_ACK.
 */
void KnxIpTunnel::scanReadDescriptor()
{
    if (!s.coConnected) return;
    s.coReadAcked = false; // fresh read -> wait for this device's T_ACK
    uint8_t apdu[2];
    uint8_t n = buildDeviceDescriptorRead(apdu); // APCI 0x300, descriptorType 0
    // Force the CO numbered path to coPa (txApdu auto-routes since coConnected && da==coPa).
    txApdu(this, s.coPa, apdu, n, true);
}

/**
 * @brief Close the CO session (T_Disconnect) and clear its state.
 */
void KnxIpTunnel::scanDisconnect()
{
    if (s.sock == SOCK_INVALID) return;
    if (s.coConnected)
    {
        uint8_t tpdu[1] = {TPCI_DISCONNECT};
        uint8_t cemi[16];
        uint16_t cl = buildCemi(cemi, _assignedPA, s.coPa, tpdu, 1, PRIO_SYSTEM, false); // AckDontCare (tunnel-PA)
        sendTunnel(cemi, cl);
    }
    s.coConnected = false;
    s.coAwaitingAck = false;
}

/**
 * @brief Outstanding-frame count for the FTC flow control.
 * @details Reports the HIGHER of the real unacked-tunnel count and the modeled TP-FIFO depth, so the
 *          fast/forget pump paces to the TP bottleneck the tunnel would otherwise hide (see g_vfifoFrames).
 */
uint16_t KnxIpTunnel::txQueueSize() const
{
    vfifoDrain();
    uint16_t vq = (uint16_t)(g_vfifoFrames + 0.5);                                 // modeled TP-FIFO depth
    uint16_t inflight = (uint16_t)(s.txQueue.size() + (s.txInflightBusy ? 1 : 0)); // frames queued + the one unacked at the tunnel
    uint16_t hi = (inflight > vq) ? inflight : vq;
    return hi;
}

/**
 * @brief BBR-style control on the send rate from the FTC report (ground truth of what the target received).
 * @details The three branches probe up / snap to the measured ceiling / back off — it finds the wire ceiling,
 *          holds just under it, and self-recovers from a kick. Only fast mode drives this; classic is
 *          self-clocked.
 */
void KnxIpTunnel::pacingRate(uint32_t deliveredBps, bool clean)
{
    if (!clean && deliveredBps == 0)
        g_vfifoDrainBps *= VFIFO_KICK; // kick: no delivery sample -> ease off
    else if (clean)
        g_vfifoDrainBps *= VFIFO_PROBE; // link kept up -> probe higher (find the ceiling)
    else
        g_vfifoDrainBps = (double)deliveredBps * VFIFO_MARGIN; // overshot -> snap to the measured wire ceiling
    if (g_vfifoDrainBps < VFIFO_RATE_MIN) g_vfifoDrainBps = VFIFO_RATE_MIN;
    // Runaway guard: if the bottleneck is LATENCY not rate (delivered plateaus while clean), the probe would
    // climb forever harmlessly (the pacer just never binds). Cap it so it stays a sane multiple of what the
    // link actually delivers -- there is no point pacing far above the measured ceiling.
    if (deliveredBps > 0 && g_vfifoDrainBps > 2.0 * deliveredBps) g_vfifoDrainBps = 2.0 * deliveredBps;
}

/**
 * @brief Tunnel-health getters (verbose console status line): read the TU-local session state.
 */
uint8_t KnxIpTunnel::channelId() const { return _connected ? s.channelId : 0; }
uint8_t KnxIpTunnel::txSeq() const { return _connected ? s.txSeq : 0; }
uint8_t KnxIpTunnel::rxSeq() const { return _connected ? s.rxSeq : 0; }

KnxIpTunnel g_knxTunnel;

/**
 * @brief Transmitted-frame count, for the host CLI's command-quiescence completion (see header).
 */
uint32_t knxTunnelActivity() { return g_txActivity; }
uint32_t knxTunnelRx() { return g_rxActivity; }
uint32_t knxTunnelDrops() { return g_dropActivity; }

#endif // !FTC_TUNNEL_SELFTEST

/**********************************************************************
 ****************************** SELF-TEST ***************************
 **********************************************************************/
// Build the four spec APDUs and compare against doc/FTC-WIRE-PROTOCOL.md §1.6/§3 hexdumps.
//   clang++ -std=c++17 -DFTC_TUNNEL_SELFTEST -I <src> knx_ip_tunnel.cpp -o /tmp/ftc && /tmp/ftc
#ifdef FTC_TUNNEL_SELFTEST
    #include <cstdio>
    #include <cstdlib>

static int g_fail = 0;

/**
 * @brief Print and assert that `got` equals `want` byte-for-byte.
 */
static void expectBytes(const char* label, const uint8_t* got, int gotLen, const uint8_t* want, int wantLen)
{
    printf("%-28s got: ", label);
    for (int i = 0; i < gotLen; ++i)
        printf("%02X ", got[i]);
    bool ok = (gotLen == wantLen);
    if (ok)
        for (int i = 0; i < wantLen; ++i)
            if (got[i] != want[i]) ok = false;
    printf("\n%-28s exp: ", "");
    for (int i = 0; i < wantLen; ++i)
        printf("%02X ", want[i]);
    printf("  => %s\n\n", ok ? "PASS" : "FAIL");
    if (!ok) g_fail++;
}

/**
 * @brief Prefix-only check: payload bytes past `prefixLen` are arbitrary client data / CRC we do not build.
 */
static void expectPrefix(const char* label, const uint8_t* got, int gotLen, const uint8_t* want, int prefixLen,
                         int expectTotal)
{
    printf("%-28s got[0..%d]: ", label, prefixLen - 1);
    for (int i = 0; i < prefixLen; ++i)
        printf("%02X ", got[i]);
    bool ok = (gotLen == expectTotal);
    for (int i = 0; i < prefixLen; ++i)
        if (got[i] != want[i]) ok = false;
    printf("(len %d, exp %d) => %s\n\n", gotLen, expectTotal, ok ? "PASS" : "FAIL");
    if (!ok) g_fail++;
}

/**
 * @brief Run the APDU-builder self-test vectors; returns non-zero on any failure.
 */
int main()
{
    uint8_t out[300];
    uint8_t n;

    // (a) CheckFeatures(102) command on obj 159 → 02 C7 9F 66   (§1.6a)
    n = buildFunctionPropertyCommand(out, 159, 102, nullptr, 0);
    const uint8_t wa[] = {0x02, 0xC7, 0x9F, 0x66};
    expectBytes("(a) CheckFeatures(102)", out, n, wa, sizeof(wa));

    // (b) FilesystemInfo(46) command → 02 C7 9F 2E   (§1.6c)
    n = buildFunctionPropertyCommand(out, 159, 46, nullptr, 0);
    const uint8_t wb[] = {0x02, 0xC7, 0x9F, 0x2E};
    expectBytes("(b) FilesystemInfo(46)", out, n, wb, sizeof(wb));

    // (c) Fast FileUpload(44) DATA chunk, seq=5, n=245 → 02 C7 9F 2C 05 00 F5 <245 payload> <crcHi crcLo>
    //     The client hands us the full data[] (seq|n|payload|crc). We assemble only the APDU header (§1.6b).
    uint8_t data[250];
    data[0] = 0x05; // seqLo (LITTLE-endian)
    data[1] = 0x00; // seqHi
    data[2] = 0xF5; // n = 245
    for (int i = 0; i < 245; ++i)
        data[3 + i] = (uint8_t)(i & 0xFF);
    data[248] = 0xAB; // crcHi (client-supplied; transport does not compute it)
    data[249] = 0xCD; // crcLo
    n = buildFunctionPropertyCommand(out, 159, 44, data, 250);
    const uint8_t wc[] = {0x02, 0xC7, 0x9F, 0x2C, 0x05, 0x00, 0xF5};
    // Full APDU = 4 header + 250 payload = 254 bytes; NPDU LEN octet would be 3+250 = 253.
    expectPrefix("(c) FileUploadFast(44) DATA", out, n, wc, (int)sizeof(wc), 254);

    // (d) Console PID_IN write (OPEN) on obj 160, PA 5.0.7 → 02 C7 A0 01 01 50 07   (§1.6d)
    uint8_t con[3] = {0x01, 0x50, 0x07}; // flags=OPEN, paHi, paLo
    n = buildFunctionPropertyCommand(out, 160, 1, con, 3);
    const uint8_t wd[] = {0x02, 0xC7, 0xA0, 0x01, 0x01, 0x50, 0x07};
    expectBytes("(d) Console PID_IN OPEN", out, n, wd, sizeof(wd));

    // Bonus coverage of the other builders (§1.2/§1.4/§1.5) — not required, but proves the encoders.
    n = buildDeviceDescriptorRead(out);
    const uint8_t we[] = {0x03, 0x00};
    expectBytes("(e) DeviceDescriptorRead", out, n, we, sizeof(we));

    n = buildMemoryRead(out, 12, 0x1234);
    const uint8_t wf[] = {0x02, 0x0C, 0x12, 0x34}; // 0x00 | (12 & 0x3F) = 0x0C
    expectBytes("(f) MemoryRead n=12", out, n, wf, sizeof(wf));

    n = buildPropertyValueRead(out, 0, 11, 1, 1); // count=1,startIndex=1 → 0x10, 0x01
    const uint8_t wg[] = {0x03, 0xD5, 0x00, 0x0B, 0x10, 0x01};
    expectBytes("(g) PropertyValueRead", out, n, wg, sizeof(wg));

    // (h) PropertyValueWrite — prog-mode LED: obj 0, pid 54, count 1, startIndex 1, 1 data byte (§1.3)
    uint8_t led[1] = {0x01};
    n = buildPropertyValueWrite(out, 0, 54, 1, 1, led, 1);
    const uint8_t wh[] = {0x03, 0xD7, 0x00, 0x36, 0x10, 0x01, 0x01}; // 54 = 0x36
    expectBytes("(h) PropertyValueWrite", out, n, wh, sizeof(wh));

    printf("==== %s (%d failures) ====\n", g_fail ? "SELFTEST FAILED" : "SELFTEST OK", g_fail);
    return g_fail ? 1 : 0;
}
#endif // FTC_TUNNEL_SELFTEST

/**
 * @file        ProgScan.h
 * @brief       Find KNX devices in PROGRAMMING MODE and localise them to a TP line / router.
 * @details     Broadcasts an A_IndividualAddress_Read (APCI 0x0100) over a self-owned tunnel; only a device in
 *              programming mode answers (A_IndividualAddress_Response), and the cEMI source of that answer IS
 *              its individual address. The ctrl2 hop count picks the scope: LOCAL (hop 0) = this TP line only,
 *              GLOBAL (hop 6) = whole network.
 * @date        2026-08-03
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "I18n.h"
#include "Templates.h"
#include "Term.h"
#include "Theme.h"
#include "../core/Discovery.h"  // ftc::detail::socket_t · sockValid · sockClose · nowMs + platform socket headers
#include "../core/DeviceMgmt.h" // ftc::knxMaskName (mask version -> friendly device class)
#ifndef _WIN32
    #include <fcntl.h> // fcntl / O_NONBLOCK for setNonBlocking()
#endif

namespace ftc
{

/**
 * @brief One programming-mode responder: the device IA + the interface it answered through (for `locate`).
 */
struct ProgHit
{
    uint16_t ia = 0;         ///< the device's individual address (cEMI source of the response)
    std::string viaIp;       ///< the interface/router IP the LOCAL scan reached it through (locate only)
    bool haveDesc = false;   ///< a device descriptor (mask version) was read back
    uint16_t descriptor = 0; ///< the KNX mask version (== device class), via A_DeviceDescriptor_Read
};

/**
 * @brief Broadcast an A_IndividualAddress_Read over a self-owned KNXnet/IP tunnel and collect the responders.
 * @details Header-only like the rest of cli/; run() renders, scanOnce() is the reusable engine `locate` drives.
 */
class ProgScan
{
  public:
    /**
     * @brief Localisation mode = the ctrl2 hop count. Local = 0 (this TP line only) · Global = 6 (whole network).
     */
    enum class Mode
    {
        Local,
        Global
    };

    ProgScan(Term& term, Theme& theme, Tpl& tpl, I18n& i18n) : _c(theme), _p(tpl), _i(i18n) { (void)term; }

    /**
     * @brief Set the target before run()/scanOnce(): the interface IP + control port.
     */
    void target(const std::string& ip, uint16_t port)
    {
        _ip = ip;
        _port = port;
    }

    /**
     * @brief Full user-facing scan against `target()`: open, broadcast, listen, render a section + result table.
     * @details maxSeconds 0 -> a sane 3 s default. quiet -> plain `pa<TAB>line` lines. verbose -> log the TX
     *          frame bytes. maxFrames > 0 stops early after that many distinct responders. Returns 0 on a clean
     *          stop, 130 on Ctrl+C, 1 if the tunnel could not be opened.
     */
    int run(Mode mode, bool quiet, bool verbose, int maxFrames, int maxSeconds,
            const volatile std::sig_atomic_t* abort)
    {
        _quiet = quiet;
        _verbose = verbose;
        const int secs = maxSeconds > 0 ? maxSeconds : 3; // default listen window
        std::vector<ProgHit> hits;
        std::string err;
        bool aborted = false;

        // Section first, so the live "listening" line animates UNDER it while scanOnce blocks.
        if (!_quiet)
        {
            const char* what = mode == Mode::Global
                                   ? _i.tr("global · hop 6 · whole network", "global · Hop 6 · gesamtes Netz")
                                   : _i.tr("local · hop 0 · this TP line only", "lokal · Hop 0 · nur diese TP-Linie");
            _p.section(std::string("progscan · ") + what, -1);
        }
        if (maxSeconds <= 0) return runLoop(mode, abort); // --seconds 0 -> continuous prog-mode watch
        const bool anim = !_quiet && _c.term().isTty();
        std::function<void(int, int)> tick;
        if (anim) tick = [&](int elapsedMs, int found) { drawScanTick(elapsedMs, secs, found); };
        const int rc = scanOnce(mode, secs, maxFrames, hits, err, abort, &aborted, tick);
        if (anim)
        {
            std::printf("\r\x1b[K"); // wipe the live line; the result follows
            std::fflush(stdout);
        }
        if (rc != 0)
        {
            if (!_quiet)
                _p.status(Tpl::Stat::Err, _i.tr("progscan connect failed", "progscan-Verbindung fehlgeschlagen"), {err});
            else
                std::fprintf(stderr, "connect failed: %s\n", err.c_str());
            return rc; // 1
        }
        if (!_quiet)
            _p.status(Tpl::Stat::Info, _i.tr("A_IndividualAddress_Read broadcast", "A_IndividualAddress_Read-Broadcast"),
                      {std::string(_i.tr("src ", "Quelle ")) + Tpl::pa(_assignedPA)});
        render(mode, hits, secs, aborted);
        return aborted ? 130 : 0;
    }

    /**
     * @brief Continuous watch (ps --seconds 0): re-broadcast each cycle and redraw the CURRENT prog-mode set in
     *        place, so devices entering/leaving programming mode appear/disappear live. Ctrl+C stops.
     * @details Descriptors are cached per IA (a known device is not re-read). One open tunnel for the whole run.
     */
    int runLoop(Mode mode, const volatile std::sig_atomic_t* abort)
    {
        std::string err;
        if (!open(err))
        {
            if (!_quiet)
                _p.status(Tpl::Stat::Err, _i.tr("progscan connect failed", "progscan-Verbindung fehlgeschlagen"), {err});
            else
                std::fprintf(stderr, "connect failed: %s\n", err.c_str());
            return 1;
        }
        if (!_quiet)
            _p.status(Tpl::Stat::Info, _i.tr("A_IndividualAddress_Read broadcast", "A_IndividualAddress_Read-Broadcast"),
                      {std::string(_i.tr("src ", "Quelle ")) + Tpl::pa(_assignedPA),
                       _i.tr("loop · Ctrl+C to stop", "Loop · Ctrl+C zum Beenden")});
        const bool anim = !_quiet && _c.term().isTty();
        std::map<uint16_t, uint16_t> descCache; // IA -> mask, once identified
        int prevLines = 0, blink = 0;
        bool aborted = false, tunnelUp = true;
        while (!(abort && *abort))
        {
            std::vector<ProgHit> cur; // the responders THIS cycle = the current prog-mode set
            sendReadBroadcast(mode);
            for (const uint64_t winEnd = detail::nowMs() + 900; detail::nowMs() < winEnd;)
            {
                if (abort && *abort)
                {
                    aborted = true;
                    break;
                }
                if (!pump(cur))
                {
                    tunnelUp = false;
                    break;
                }
            }
            if (aborted || !tunnelUp) break;
            for (auto& h : cur) // reuse cached descriptors so a known device is not re-read
            {
                auto it = descCache.find(h.ia);
                if (it != descCache.end())
                {
                    h.descriptor = it->second;
                    h.haveDesc = true;
                }
            }
            identify(cur, abort); // reads a descriptor only for the not-yet-cached IAs
            for (const auto& h : cur)
                if (h.haveDesc) descCache[h.ia] = h.descriptor;
            if (anim)
                prevLines = redrawLoop(cur, prevLines, blink++);
            else
            {
                for (const auto& h : cur)
                    std::printf("%s\t%s\n", Tpl::pa(h.ia).c_str(), lineStr(h.ia).c_str());
                std::fflush(stdout);
            }
        }
        close();
        if (abort && *abort) aborted = true;
        if (anim && aborted) std::printf("\n"); // leave the last live view intact, drop below it
        if (!_quiet && aborted) _p.status(Tpl::Stat::Warn, _i.tr("stopped", "gestoppt"), {});
        return aborted ? 130 : (tunnelUp ? 0 : 1);
    }

    /**
     * @brief Redraw the live prog-mode set in place (cursor up over the previous block); returns lines printed.
     */
    int redrawLoop(const std::vector<ProgHit>& cur, int prevLines, int blink)
    {
        Theme& c = _c;
        Term& tm = _c.term();
        if (prevLines > 0) std::printf("\x1b[%dA", prevLines); // back to the top of the previous block
        std::printf("\r\x1b[J");                               // clear from here down
        const bool on = (blink % 2) == 0;
        const std::string dot = on ? c.amber(tm.glyph("◉", "*")) : c.dim(tm.glyph("○", "o"));
        std::printf("  %s  %s%s%s\n", dot.c_str(), c.dim(_i.tr("searching … ", "suche … ")).c_str(),
                    c.green(std::to_string(cur.size())).c_str(),
                    c.dim(_i.tr(" in programming mode", " im Programmiermodus")).c_str());
        int lines = 1;
        if (cur.empty())
        {
            std::printf("  %s\n", c.dim(_i.tr("(none right now)", "(gerade keins)")).c_str());
            ++lines;
        }
        for (const auto& h : cur)
        {
            std::string cls;
            if (h.haveDesc)
            {
                char x[8];
                std::snprintf(x, sizeof(x), "0x%04X", h.descriptor);
                cls = std::string(c.txt(x)) + c.dim(std::string("  · ") + knxMaskName(h.descriptor));
            }
            else
                cls = c.dim(_i.tr("in programming mode (no descriptor)", "im Programmiermodus (kein Deskriptor)"));
            _p.tableRow({_p.statusDot('g'), c.bold(Tpl::pa(h.ia)), c.cyan(lineStr(h.ia)), cls}, {2, 10, 8, 0});
            ++lines;
        }
        std::fflush(stdout);
        return lines;
    }

    /**
     * @brief The reusable engine: open the tunnel, send ONE broadcast, collect distinct responder IAs, DISCONNECT.
     * @details Fills @p out (deduped) + @p err on a connect failure. @p outAborted (optional) is set on Ctrl+C.
     *          Returns 0 on a clean scan, 1 on a connect failure. No rendering -> `locate` calls this per
     *          interface. `_assignedPA` holds the tunnel IA.
     */
    int scanOnce(Mode mode, int seconds, int maxFrames, std::vector<ProgHit>& out, std::string& err,
                 const volatile std::sig_atomic_t* abort, bool* outAborted = nullptr,
                 const std::function<void(int elapsedMs, int found)>& onTick = {})
    {
        if (!open(err)) return 1;
        sendReadBroadcast(mode); // the one broadcast; responses stream back as inbound TUNNELLING_REQUESTs

        const uint64_t startMs = detail::nowMs();
        bool aborted = false;
        for (;;)
        {
            if (abort && *abort)
            {
                aborted = true;
                break;
            }
            if ((int)((detail::nowMs() - startMs) / 1000) >= seconds) break;
            if (maxFrames > 0 && (int)out.size() >= maxFrames) break;
            if (!pump(out)) break;                                                 // server tore the tunnel down
            if (onTick) onTick((int)(detail::nowMs() - startMs), (int)out.size()); // live blink line (run() only)
        }
        if (!aborted) identify(out, abort); // read each responder's descriptor (mask/class) before leaving
        if (outAborted) *outAborted = aborted;
        close(); // graceful DISCONNECT_REQUEST
        return 0;
    }

    /**
     * @brief The exact cEMI L_Data.req bytes this scan transmits, for the given mode (eyeball / log without a tunnel).
     * @details src defaults to 0.0.0 (the interface fills its own IA).
     */
    static std::vector<uint8_t> frameBytes(Mode mode, uint16_t src = 0)
    {
        std::vector<uint8_t> f(11);
        buildCemi(f.data(), mode, src);
        return f;
    }

  private:
    /**********************************************************************
     *********************** KNXnet/IP CONSTANTS ************************
     **********************************************************************/
    enum : uint16_t
    {
        ST_CONNECT_REQ = 0x0205,
        ST_CONNECT_RES = 0x0206,
        ST_CONNSTATE_REQ = 0x0207,
        ST_DISCONNECT_REQ = 0x0209,
        ST_DISCONNECT_RES = 0x020A,
        ST_TUNNEL_REQ = 0x0420,
        ST_TUNNEL_ACK = 0x0421
    };
    enum : uint8_t
    {
        MC_LDATA_REQ = 0x11,
        MC_LDATA_IND = 0x29,
        MC_LDATA_CON = 0x2E
    };
    enum : uint16_t
    {
        APCI_IA_READ = 0x0100,
        APCI_IA_RESPONSE = 0x0140,
        APCI_DD_READ = 0x0300,
        APCI_DD_RESPONSE = 0x0340 // A_DeviceDescriptor_Read/Response (low 6 bits = type)
    };

    // --- socket + tunnel state ---
    Theme& _c;
    Tpl& _p;
    I18n& _i;
    detail::socket_t _sock = (detail::socket_t)-1;
    uint16_t _localPort = 0;
    uint8_t _channelId = 0;
    uint16_t _assignedPA = 0;
    uint8_t _rxSeq = 0;
    bool _rxSeqValid = false;
    uint8_t _txSeq = 0;
    uint64_t _lastKeepalive = 0;
    bool _quiet = false, _verbose = false;
    std::string _ip;
    uint16_t _port = 3671;

    /**********************************************************************
     ************************ cEMI FRAME BUILDER ***********************
     **********************************************************************/
    /**
     * @brief Build the 11-byte cEMI L_Data.req for an A_IndividualAddress_Read broadcast into @p d.
     * @details Layout [MC 0x11][AIL 0][ctrl1][ctrl2][srcHi][srcLo][00][00][len 1][TPCI/APCI 0x01][APCI 0x00].
     *          ctrl1 0xB0 = standard · no-repeat · system prio · no ack-request (broadcast has no acker).
     *          ctrl2 0x80 (LOCAL, hop 0) / 0xE0 (GLOBAL, hop 6): broadcast bit7 + hop bits 6..4.
     *          dst 0x0000 = broadcast · TPDU 01 00 = T_Data_Broadcast (TPCI 0) + APCI 0x100.
     */
    static void buildCemi(uint8_t* d, Mode mode, uint16_t src)
    {
        d[0] = MC_LDATA_REQ;                       // L_Data.req
        d[1] = 0x00;                               // additional-info length
        d[2] = 0xB0;                               // ctrl1: std · no-repeat · system prio
        d[3] = mode == Mode::Global ? 0xE0 : 0x80; // ctrl2: broadcast + hop 6 (global) / hop 0 (local)
        d[4] = (uint8_t)(src >> 8);                // source IA (0.0.0 -> interface substitutes its own)
        d[5] = (uint8_t)(src & 0xFF);
        d[6] = 0x00; // destination = 0x0000 (broadcast)
        d[7] = 0x00;
        d[8] = 0x01;  // NPDU length (octets after this byte, minus the TPCI octet)
        d[9] = 0x01;  // TPCI T_Data_Broadcast (0) | APCI[9:8] = 01
        d[10] = 0x00; // APCI[7:0] = 00  -> full APCI 0x100 (IndividualAddress_Read)
    }

    /**
     * @brief Wrap the cEMI in a TUNNELLING_REQUEST and send it. Logs the raw bytes (verbose / non-quiet).
     */
    void sendReadBroadcast(Mode mode)
    {
        uint8_t cemi[11];
        buildCemi(cemi, mode, _assignedPA);
        if (_verbose || !_quiet) logTxFrame(mode, cemi, sizeof(cemi));

        uint8_t body[4 + sizeof(cemi)];
        body[0] = 0x04;
        body[1] = _channelId;
        body[2] = _txSeq;
        body[3] = 0x00; // connection header
        std::memcpy(body + 4, cemi, sizeof(cemi));
        sendKnxIp(ST_TUNNEL_REQ, body, sizeof(body));
        _txSeq++; // wraps at 256; only one send per scan, but keep the counter honest
    }

    /**
     * @brief Send a point-to-point A_DeviceDescriptor_Read (type 0) to @p ia — asks for its mask version.
     * @details Connectionless; works even at the default 15.15.255. ctrl2 0x60 = individual + hop 6 (reaches
     *          the device on either a local or a remote line).
     */
    void sendDescriptorRead(uint16_t ia)
    {
        uint8_t cemi[11] = {MC_LDATA_REQ, 0x00, 0xB0, 0x60,
                            (uint8_t)(_assignedPA >> 8), (uint8_t)(_assignedPA & 0xFF),
                            (uint8_t)(ia >> 8), (uint8_t)(ia & 0xFF),
                            0x01, 0x03, 0x00}; // NPDU len 1 · TPDU 03 00 = A_DeviceDescriptor_Read, type 0
        uint8_t body[4 + sizeof(cemi)];
        body[0] = 0x04;
        body[1] = _channelId;
        body[2] = _txSeq;
        body[3] = 0x00;
        std::memcpy(body + 4, cemi, sizeof(cemi));
        sendKnxIp(ST_TUNNEL_REQ, body, sizeof(body));
        _txSeq++;
    }

    /**
     * @brief For each responder, read its device descriptor (mask/class) over the still-open tunnel.
     * @details Sends the P2P read then pumps until the answer lands or ~1.2 s elapse. Best-effort (a device may
     *          not answer connectionless) — an unread descriptor just stays absent.
     */
    void identify(std::vector<ProgHit>& out, const volatile std::sig_atomic_t* abort)
    {
        // Index-based: pump(out) below can push_back a late responder -> a range-for reference would dangle.
        for (size_t i = 0; i < out.size(); ++i)
        {
            if (out[i].haveDesc) continue;
            sendDescriptorRead(out[i].ia);
            const uint64_t dl = detail::nowMs() + 1200;
            while (detail::nowMs() < dl && !out[i].haveDesc)
            {
                if (abort && *abort) return;
                if (!pump(out)) return; // tunnel gone
            }
        }
    }

    /**
     * @brief Print the exact cEMI TX bytes (hop-count + APCI verifiable by eye).
     */
    void logTxFrame(Mode mode, const uint8_t* cemi, int n)
    {
        const std::string h = hex(cemi, n);
        if (_quiet)
        {
            std::fprintf(stderr, "tx cemi: %s\n", h.c_str());
            return;
        }
        _p.note(std::string(_i.tr("TX cEMI ", "TX cEMI ")) +
                (mode == Mode::Global ? "(hop 6) " : "(hop 0) ") + h);
    }

    /**********************************************************************
     ***************************** LIFECYCLE *****************************
     **********************************************************************/
    /**
     * @brief Open the UDP socket + a LINK_LAYER tunnel (CRI `04 02 02 00`). Fills @p err on failure.
     */
    bool open(std::string& err)
    {
        using namespace detail;
        _sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (!sockValid(_sock))
        {
            err = "socket()";
            return false;
        }

        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = INADDR_ANY;
        local.sin_port = 0;
        if (::bind(_sock, (sockaddr*)&local, sizeof(local)) != 0)
        {
            err = "bind()";
            close();
            return false;
        }
        socklen_t ll = sizeof(local);
        ::getsockname(_sock, (sockaddr*)&local, &ll);
        _localPort = ntohs(local.sin_port);

        sockaddr_in dst{};
        dst.sin_family = AF_INET;
        dst.sin_port = htons(_port);
        if (::inet_pton(AF_INET, _ip.c_str(), &dst.sin_addr) != 1)
        {
            err = "bad ip";
            close();
            return false;
        }
        if (::connect(_sock, (sockaddr*)&dst, sizeof(dst)) != 0)
        {
            err = "connect()";
            close();
            return false;
        }
        setNonBlocking();

        // CONNECT_REQUEST: HPAI(ctrl) + HPAI(data) + CRI(tunnel/LINK_LAYER). Route-back HPAI (0.0.0.0:localport).
        uint8_t hpai[8] = {0x08, 0x01, 0, 0, 0, 0, (uint8_t)(_localPort >> 8), (uint8_t)(_localPort & 0xFF)};
        uint8_t body[8 + 8 + 4];
        std::memcpy(body, hpai, 8);
        std::memcpy(body + 8, hpai, 8);
        body[16] = 0x04;
        body[17] = 0x04;
        body[18] = 0x02;
        body[19] = 0x00; // CRI: len, TUNNEL_CONNECTION, LINK_LAYER
        if (!sendKnxIp(ST_CONNECT_REQ, body, sizeof(body)))
        {
            err = "send CONNECT_REQUEST";
            close();
            return false;
        }

        uint8_t buf[512];
        const uint64_t deadline = nowMs() + 3000;
        while ((int64_t)(deadline - nowMs()) > 0)
        {
            int n = recvWait(buf, sizeof(buf), 200);
            if (n < 8) continue;
            if (((buf[2] << 8) | buf[3]) != ST_CONNECT_RES) continue;
            const uint8_t status = buf[7];
            if (status != 0x00)
            {
                err = connStatus(status);
                close();
                return false;
            }
            _channelId = buf[6];
            if (n >= 20 && buf[17] == 0x04) _assignedPA = (uint16_t)((buf[18] << 8) | buf[19]); // CRD IA
            _lastKeepalive = nowMs();
            return true;
        }
        err = _i.tr("no CONNECT_RESPONSE (timeout)", "keine CONNECT_RESPONSE (Timeout)");
        close();
        return false;
    }

    /**
     * @brief Graceful DISCONNECT_REQUEST + socket close. Idempotent.
     */
    void close()
    {
        if (!detail::sockValid(_sock)) return;
        if (_channelId)
        {
            uint8_t body[2 + 8] = {_channelId, 0x00, 0x08, 0x01, 0, 0, 0, 0,
                                   (uint8_t)(_localPort >> 8), (uint8_t)(_localPort & 0xFF)};
            sendKnxIp(ST_DISCONNECT_REQ, body, sizeof(body));
        }
        detail::sockClose(_sock);
        _sock = (detail::socket_t)-1;
        _channelId = 0;
    }

    /**********************************************************************
     *************************** RECEIVE LOOP ****************************
     **********************************************************************/
    /**
     * @brief One bounded pass: drain datagrams, ACK every TUNNELLING_REQUEST, collect responder IAs, keep-alive.
     * @details ALWAYS acks (even a duplicate) or the server drops us. Returns false only if the server tore the
     *          connection down.
     */
    bool pump(std::vector<ProgHit>& out)
    {
        using namespace detail;
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(_sock, &rf);
        timeval tv{0, 60000}; // 60 ms — responsive to Ctrl-C without busy-spinning
        if (::select((int)_sock + 1, &rf, nullptr, nullptr, &tv) > 0)
        {
            uint8_t buf[600];
            for (int guard = 0; guard < 256; ++guard)
            {
                int n = (int)::recv(_sock, (char*)buf, sizeof(buf), 0);
                if (n < 6) break;
                const uint16_t st = (uint16_t)((buf[2] << 8) | buf[3]);
                const uint16_t total = (uint16_t)((buf[4] << 8) | buf[5]);
                if (total > n) continue;
                if (st == ST_TUNNEL_REQ)
                {
                    if (n < 10 || buf[6] != 0x04 || buf[7] != _channelId) continue;
                    const uint8_t seq = buf[8];
                    sendTunnelAck(seq);                         // ALWAYS ack (even a duplicate) or we get dropped
                    if (_rxSeqValid && seq == _rxSeq) continue; // duplicate -> ack only
                    _rxSeq = seq;
                    _rxSeqValid = true;
                    handleCemi(&buf[10], total - 10, out);
                }
                else if (st == ST_DISCONNECT_REQ)
                {
                    uint8_t body[2] = {_channelId, 0x00};
                    sendKnxIp(ST_DISCONNECT_RES, body, 2);
                    detail::sockClose(_sock);
                    _sock = (detail::socket_t)-1;
                    _channelId = 0;
                    return false;
                }
                // CONNECTIONSTATE_RESPONSE / DISCONNECT_RESPONSE / our own TUNNELLING_ACK: nothing to do.
            }
        }
        if ((nowMs() - _lastKeepalive) >= 30000)
        {
            uint8_t body[2 + 8] = {_channelId, 0x00, 0x08, 0x01, 0, 0, 0, 0,
                                   (uint8_t)(_localPort >> 8), (uint8_t)(_localPort & 0xFF)};
            sendKnxIp(ST_CONNSTATE_REQ, body, sizeof(body));
            _lastKeepalive = nowMs();
        }
        return true;
    }

    /**********************************************************************
     *************************** cEMI DISPATCH ***************************
     **********************************************************************/
    /**
     * @brief Route one cEMI: an L_Data.ind carrying an A_IndividualAddress_Response is a prog-mode responder.
     * @details Bounds-checked throughout; a short/malformed frame is dropped silently.
     */
    void handleCemi(const uint8_t* cemi, int len, std::vector<ProgHit>& out)
    {
        if (len < 2) return;
        const uint8_t mc = cemi[0];
        if (mc != MC_LDATA_IND) return; // ignore .con echoes / busmon / anything else
        const int addil = cemi[1];
        const int off = 2 + addil; // start of ctrl1
        if (off + 8 > len) return; // need ctrl1,ctrl2,src,dst,len,>=1 tpdu octet
        const uint8_t* d = cemi + off;
        const int n = len - off;

        const uint8_t ctrl2 = d[1];
        const uint16_t src = (uint16_t)((d[2] << 8) | d[3]);
        const uint16_t dst = (uint16_t)((d[4] << 8) | d[5]);
        const uint8_t nlen = d[6]; // NPDU octet count (TPDU bytes after byte0)
        const int tpduLen = nlen + 1;
        if (7 + tpduLen > n || tpduLen < 2) return; // never over-read
        const uint8_t* tpdu = d + 7;
        if ((tpdu[0] & 0xC0) != 0x00) return; // connectionless TPCI = 0 (T_Data_Broadcast / _Individual)
        const uint16_t apci = (uint16_t)(((tpdu[0] & 0x03) << 8) | tpdu[1]);

        // A_IndividualAddress_Response: broadcast-addressed -> a device in programming mode announced its IA.
        if ((ctrl2 & 0x80) && dst == 0x0000 && apci == APCI_IA_RESPONSE)
        {
            addHit(src, out);
            return;
        }

        // A_DeviceDescriptor_Response (P2P, apci 0x340 | type): fill the descriptor of the responder we queried.
        if ((apci & 0x03C0) == APCI_DD_RESPONSE && tpduLen >= 4)
            for (auto& h : out)
                if (h.ia == src && !h.haveDesc)
                {
                    h.descriptor = (uint16_t)((tpdu[2] << 8) | tpdu[3]);
                    h.haveDesc = true;
                    return;
                }
    }

    /**
     * @brief Record a distinct responder IA (dedupe; bounded list). Tagged with the interface IP for `locate`.
     */
    void addHit(uint16_t ia, std::vector<ProgHit>& out)
    {
        for (const auto& h : out)
            if (h.ia == ia) return;    // de-dupe (a device may answer more than once)
        if (out.size() >= 256) return; // hard bound (never in practice; ETS expects one)
        ProgHit h;
        h.ia = ia;
        h.viaIp = _ip;
        out.push_back(h);
    }

    /**********************************************************************
     ****************** RENDERING (user-facing run() only) **************
     **********************************************************************/
    /**
     * @brief One live frame while scanning: a blinking prog-mode dot + countdown + live found count. CR-anchored.
     */
    void drawScanTick(int elapsedMs, int secs, int found)
    {
        Term& tm = _c.term();
        int remain = secs - elapsedMs / 1000;
        if (remain < 0) remain = 0;
        const bool on = (elapsedMs / 400) % 2 == 0; // blink ~2.5 Hz
        const std::string dot = on ? _c.amber(tm.glyph("◉", "*")) : _c.dim(tm.glyph("○", "o"));
        std::string line = std::string("\r  ") + dot + "  " + _c.dim(_i.tr("searching … ", "suche … ")) + std::to_string(remain) + "s";
        if (found > 0)
            line += _c.dim("  ·  ") + _c.green(std::to_string(found)) + _c.dim(_i.tr(" found", " gefunden"));
        std::printf("%s\x1b[K", line.c_str());
        std::fflush(stdout);
    }

    /**
     * @brief Render the result table (phosphor) or plain `pa<TAB>line` lines under -q.
     */
    void render(Mode mode, const std::vector<ProgHit>& hits, int secs, bool aborted)
    {
        (void)secs;
        if (_quiet)
        {
            for (const auto& h : hits)
            {
                char m[8] = "-";
                if (h.haveDesc) std::snprintf(m, sizeof(m), "0x%04X", h.descriptor);
                std::printf("%s\t%s\t%s\n", Tpl::pa(h.ia).c_str(), lineStr(h.ia).c_str(), m);
            }
            std::fflush(stdout);
            return;
        }
        if (hits.empty())
        {
            const char* scope = mode == Mode::Global ? _i.tr("in the network", "im Netz")
                                                     : _i.tr("on this TP line", "auf dieser TP-Linie");
            _p.status(Tpl::Stat::Idle,
                      std::string(_i.tr("no device in programming mode ", "kein Gerät im Programmiermodus ")) + scope, {});
            if (aborted)
                _p.status(Tpl::Stat::Warn, _i.tr("Ctrl+C — disconnected cleanly", "Ctrl+C — sauber abgemeldet"));
            return;
        }
        Theme& c = _c;
        _p.tableRow({c.dim("ST"), c.dim("PA"), c.dim(_i.tr("LINE", "LINIE")), c.dim(_i.tr("CLASS (mask)", "KLASSE (Maske)"))},
                    {2, 10, 8, 0});
        for (const auto& h : hits)
        {
            std::string cls;
            if (h.haveDesc)
            {
                char x[8];
                std::snprintf(x, sizeof(x), "0x%04X", h.descriptor);
                cls = std::string(c.txt(x)) + c.dim(std::string("  · ") + knxMaskName(h.descriptor));
            }
            else
                cls = c.dim(_i.tr("in programming mode (no descriptor)", "im Programmiermodus (kein Deskriptor)"));
            _p.tableRow({_p.statusDot('g'), c.bold(Tpl::pa(h.ia)), c.cyan(lineStr(h.ia)), cls}, {2, 10, 8, 0});
        }
        _p.status(Tpl::Stat::Ok,
                  std::to_string(hits.size()) + " " +
                      (hits.size() == 1 ? _i.tr("device in programming mode found", "Gerät im Programmiermodus gefunden")
                                        : _i.tr("devices in programming mode found", "Geräte im Programmiermodus gefunden")),
                  {});
        if (hits.size() > 1)
            _p.note(_i.tr("ETS expects EXACTLY ONE device in programming mode during addressing",
                          "ETS erwartet GENAU EIN Gerät im Programmiermodus während der Adressierung"));
    }

    /**********************************************************************
     ***************************** HELPERS *******************************
     **********************************************************************/
    /**
     * @brief The KNX line `area.line` an IA belongs to (the localisation answer).
     */
    static std::string lineStr(uint16_t ia)
    {
        char b[8];
        std::snprintf(b, sizeof(b), "%u.%u", (ia >> 12) & 0x0F, (ia >> 8) & 0x0F);
        return b;
    }

    /**
     * @brief Send a KNXnet/IP frame: 6-byte header + @p body. Returns true on a full write.
     */
    bool sendKnxIp(uint16_t st, const uint8_t* body, uint16_t bodyLen)
    {
        uint8_t pkt[600];
        const uint16_t total = (uint16_t)(6 + bodyLen);
        if (total > sizeof(pkt)) return false;
        pkt[0] = 0x06;
        pkt[1] = 0x10;
        pkt[2] = (uint8_t)(st >> 8);
        pkt[3] = (uint8_t)(st & 0xFF);
        pkt[4] = (uint8_t)(total >> 8);
        pkt[5] = (uint8_t)(total & 0xFF);
        if (bodyLen) std::memcpy(pkt + 6, body, bodyLen);
        return (int)::send(_sock, (const char*)pkt, total, 0) == (int)total;
    }

    /**
     * @brief Put the tunnel socket into non-blocking mode.
     */
    void setNonBlocking()
    {
#ifdef _WIN32
        u_long mode = 1;
        ioctlsocket(_sock, FIONBIO, &mode);
#else
        int fl = fcntl(_sock, F_GETFL, 0);
        fcntl(_sock, F_SETFL, fl | O_NONBLOCK);
#endif
    }

    /**
     * @brief TUNNELLING_ACK (0x0421) echoing the server's channel + seq.
     */
    void sendTunnelAck(uint8_t seq)
    {
        uint8_t body[4] = {0x04, _channelId, seq, 0x00};
        sendKnxIp(ST_TUNNEL_ACK, body, 4);
    }

    /**
     * @brief Blocking recv bounded by @p ms (setup only). Returns bytes read, or -1 on timeout/error.
     */
    int recvWait(uint8_t* buf, int cap, int ms)
    {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(_sock, &rf);
        timeval tv{ms / 1000, (ms % 1000) * 1000};
        if (::select((int)_sock + 1, &rf, nullptr, nullptr, &tv) <= 0) return -1;
        return (int)::recv(_sock, (char*)buf, cap, 0);
    }

    static std::string hex(const uint8_t* d, int n)
    {
        std::string s;
        char b[4];
        for (int i = 0; i < n; ++i)
        {
            std::snprintf(b, sizeof(b), "%02X", d[i]);
            if (i) s += ' ';
            s += b;
        }
        return s;
    }

    /**
     * @brief The connect-status hint (link-layer tunnel refusals).
     */
    std::string connStatus(uint8_t st)
    {
        char b[80];
        const char* name = st == 0x22 ? "E_CONNECTION_TYPE" : st == 0x23 ? "E_CONNECTION_OPTION"
                                                          : st == 0x24   ? "E_NO_MORE_CONNECTIONS"
                                                          : st == 0x25   ? "E_NO_MORE_UNIQUE_CONNECTIONS"
                                                          : st == 0x29   ? "E_TUNNELING_LAYER"
                                                                         : "E_*";
        std::snprintf(b, sizeof(b), "0x%02X %s", st, name);
        return b;
    }
};

} // namespace ftc

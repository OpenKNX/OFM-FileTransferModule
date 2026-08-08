/**
 * @file        Monitor.h
 * @brief       Live KNX group monitor + bus monitor over a KNXnet/IP tunnel.
 * @details     Opens its OWN tunnel so it can pick the KNX layer per mode: Group (CRI `04 02 02 00`, LINK_LAYER)
 *              decodes L_Data.ind group telegrams; Bus (CRI `04 02 80 00`, BUSMONITOR) prints the full raw LPDU
 *              coloured by the ETS ACK rule. Every TUNNELLING_REQUEST MUST be ACKed or the server disconnects;
 *              a ~30 s keep-alive + silent-death detection drive the reconnect. Rendering only; no knx-lib coupling.
 * @date        2026-08-03
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <ctime>
#include <string>
#include <thread>

#include "I18n.h"
#include "Templates.h"
#include "Term.h"
#include "Theme.h"
#include "../core/Discovery.h" // ftc::detail::socket_t · sockValid · sockClose · nowMs + platform socket headers
#ifndef _WIN32
    #include <fcntl.h> // fcntl / O_NONBLOCK for setNonBlocking()
#endif

namespace ftc
{

/**
 * @brief Live group / bus monitor over a self-owned KNXnet/IP tunnel.
 * @details Construct over the shared CLI refs, then run(Mode). Header-only like the rest of cli/.
 */
class Monitor
{
  public:
    /**
     * @brief Which monitor to run: Group = decoded L_Data.ind group telegrams · Bus = raw L_Busmon.ind LPDUs.
     */
    enum class Mode
    {
        Group,
        Bus
    };

    Monitor(Term& term, Theme& theme, Tpl& tpl, I18n& i18n) : _t(term), _c(theme), _p(tpl), _i(i18n) {}

    /**
     * @brief Open the tunnel and stream frames until @p abort, @p maxFrames or @p maxSeconds (0 = unlimited).
     * @details quiet -> plain tab-separated lines, no chrome; verbose -> extra raw/flags detail. A silent tunnel
     *          death or a server DISCONNECT triggers reconnect (never hang). Returns 0 on a clean stop, 130 on
     *          Ctrl+C, 1 if the tunnel could not be opened (e.g. a busmonitor already owns the subnet).
     */
    int run(Mode mode, bool quiet, bool verbose, int maxFrames, int maxSeconds,
            const volatile std::sig_atomic_t* abort)
    {
        _quiet = quiet;
        _verbose = verbose;
        std::string err;
        if (!open(mode, err))
        {
            if (!_quiet)
                _p.status(Tpl::Stat::Err, _i.tr("monitor connect failed", "Monitor-Verbindung fehlgeschlagen"),
                          {err});
            else
                std::fprintf(stderr, "connect failed: %s\n", err.c_str());
            return 1;
        }

        if (!_quiet)
        {
            const char* what = mode == Mode::Bus
                                   ? _i.tr("bus monitor · raw LPDU + ETS ACK colour", "Busmonitor · Roh-LPDU + ETS-ACK-Farbe")
                                   : _i.tr("group monitor · decoded telegrams", "Gruppenmonitor · dekodierte Telegramme");
            _p.section(std::string(mode == Mode::Bus ? "busmon · " : "groupmon · ") + what, -1);
            _p.status(Tpl::Stat::Ok, _i.tr("tunnel up", "Tunnel offen"),
                      {std::string("IA ") + Tpl::pa(_assignedPA), std::string("ch ") + std::to_string(_channelId),
                       _i.tr("Ctrl+C to stop", "Ctrl+C zum Beenden")});
            if (mode == Mode::Group) groupHeader();
            else
                _p.note(_i.tr("green = NOT acknowledged · grey = acknowledged · full bytes wrap (never truncate)",
                              "grün = NICHT quittiert · grau = quittiert · alle Bytes umbrochen (nie gekürzt)"));
        }

        const uint64_t startMs = detail::nowMs();
        int rc = 0;
        bool aborted = false;
        for (;;)
        {
            if (abort && *abort)
            {
                aborted = true;
                break;
            } // Ctrl+C (SIGINT/SIGTERM -> g_abort)
            if (maxSeconds > 0 && (int)((detail::nowMs() - startMs) / 1000) >= maxSeconds) break;
            if (maxFrames > 0 && _frames >= maxFrames) break;
            if (!pump(mode)) // tunnel gone (silent death or server DISCONNECT) -> reconnect, never hang
            {
                if (abort && *abort)
                {
                    aborted = true;
                    break;
                }
                if (maxSeconds > 0 && (int)((detail::nowMs() - startMs) / 1000) >= maxSeconds) break;
                if (!reconnect(mode, abort, startMs, maxSeconds))
                {
                    if (abort && *abort) aborted = true;
                    else if (!(maxSeconds > 0 && (int)((detail::nowMs() - startMs) / 1000) >= maxSeconds))
                        rc = 1;
                    break; // else: time cap reached during the outage -> clean stop (rc stays 0)
                }
            }
        }

        if (mode == Mode::Bus) flushPending(); // emit a still-pending (un-acked) busmon telegram before leaving
        // Report the graceful teardown, THEN close() sends the DISCONNECT_REQUEST (frees the tunnel / the
        // exclusive busmon slot immediately — no orphan). Ctrl+C returns the conventional 130.
        if (aborted && !_quiet)
            _p.status(Tpl::Stat::Warn, _i.tr("Ctrl+C — disconnecting monitor cleanly", "Ctrl+C — Monitor wird sauber abgemeldet"), {});
        else if (aborted)
            std::fprintf(stderr, "\n[abort] Ctrl+C -- disconnecting monitor cleanly...\n");
        close(); // sends DISCONNECT_REQUEST + closes the socket (see close())
        return aborted ? 130 : rc;
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
        ST_CONNSTATE_RES = 0x0208,
        ST_DISCONNECT_REQ = 0x0209,
        ST_DISCONNECT_RES = 0x020A,
        ST_TUNNEL_REQ = 0x0420,
        ST_TUNNEL_ACK = 0x0421
    };
    enum : uint8_t
    {
        MC_LDATA_IND = 0x29,
        MC_LBUSMON_IND = 0x2B
    };
    static constexpr uint64_t TUNNEL_DEAD_MS = 70000; // no proof-of-life this long (> 2 keep-alive rounds) -> dead

    // --- socket + tunnel state ---
    Term& _t;
    Theme& _c;
    Tpl& _p;
    I18n& _i;
    detail::socket_t _sock = (detail::socket_t)-1;
    uint16_t _localPort = 0;
    uint8_t _channelId = 0;
    uint16_t _assignedPA = 0;
    uint8_t _rxSeq = 0;
    bool _rxSeqValid = false;
    uint64_t _lastKeepalive = 0;
    uint64_t _lastAlive = 0; // last proof-of-life FROM the server (any datagram); stale -> tunnel dead -> reconnect
    bool _quiet = false, _verbose = false;
    int _frames = 0;

    // Deferred busmon telegram (for the ETS ACK colour): the L2 ACK is a SEPARATE single-octet frame that
    // follows the telegram, so we hold the last telegram one beat and colour it once we know whether an ACK came.
    struct Pending
    {
        bool active = false;
        uint64_t ms = 0;
        std::string tag, dec, raw, time;
    };
    Pending _pend;

    /**********************************************************************
     ***************************** LIFECYCLE *****************************
     **********************************************************************/
    /**
     * @brief Open the UDP socket + CONNECT with the mode's KNX layer byte. Fills @p err on failure.
     * @details Layer 0x80 = BUSMONITOR, 0x02 = LINK_LAYER. Route-back HPAI 0.0.0.0:localport; setup blocks
     *          briefly for CONNECT_RESPONSE but the run loop never blocks.
     */
    bool open(Mode mode, std::string& err)
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
        setNonBlocking(); // so pump()'s drain loop never blocks in recv() — the caps/abort stay responsive

        // CONNECT_REQUEST: HPAI(ctrl) + HPAI(data) + CRI(tunnel/<layer>). Route-back HPAI (0.0.0.0:localport).
        const uint8_t layer = mode == Mode::Bus ? 0x80 : 0x02;
        uint8_t hpai[8] = {0x08, 0x01, 0, 0, 0, 0, (uint8_t)(_localPort >> 8), (uint8_t)(_localPort & 0xFF)};
        uint8_t body[8 + 8 + 4];
        std::memcpy(body, hpai, 8);
        std::memcpy(body + 8, hpai, 8);
        body[16] = 0x04;
        body[17] = 0x04;
        body[18] = layer;
        body[19] = 0x00; // CRI: len, TUNNEL_CONNECTION, layer
        if (!sendKnxIp(ST_CONNECT_REQ, body, sizeof(body)))
        {
            err = "send CONNECT_REQUEST";
            close();
            return false;
        }

        // Bounded wait for CONNECT_RESPONSE (setup-only blocking; the run loop never blocks).
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
                err = connStatus(status, mode);
                close();
                return false;
            }
            _channelId = buf[6];
            // CRD at header(6)+ch(1)+status(1)+HPAI(8) = 16: [len][type][iaHi][iaLo]
            if (n >= 20 && buf[17] == 0x04) _assignedPA = (uint16_t)((buf[18] << 8) | buf[19]);
            _lastKeepalive = _lastAlive = nowMs();
            _rxSeqValid = false; // fresh tunnel: no prior RX seq to dedup against
            _pend.active = false;
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
     * @brief One bounded pass: drain datagrams (ACK + decode), flush a stale pending busmon telegram, keep-alive.
     * @details ALWAYS acks every TUNNELLING_REQUEST (even a duplicate) or the server drops us. Any datagram is
     *          proof of life; silence for 2+ keep-alive rounds (TUNNEL_DEAD_MS) means the tunnel died without a
     *          DISCONNECT (UDP drop / NAT expiry / reboot) -> return false so run() reconnects. Returns false
     *          only on that dead-tunnel signal or a server DISCONNECT.
     */
    bool pump(Mode mode)
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
                if (n < 6) break; // wouldblock / too short
                const uint16_t st = (uint16_t)((buf[2] << 8) | buf[3]);
                const uint16_t total = (uint16_t)((buf[4] << 8) | buf[5]);
                if (total > n) continue; // truncated claim
                if (st == ST_TUNNEL_REQ)
                {
                    if (n < 10 || buf[6] != 0x04 || buf[7] != _channelId) continue;
                    _lastAlive = nowMs(); // tunnel traffic = proof of life
                    const uint8_t seq = buf[8];
                    sendTunnelAck(seq);                         // ALWAYS ack (even a duplicate) or we get dropped
                    if (_rxSeqValid && seq == _rxSeq) continue; // duplicate -> ack only
                    _rxSeq = seq;
                    _rxSeqValid = true;
                    handleCemi(mode, &buf[10], total - 10);
                }
                else if (st == ST_DISCONNECT_REQ)
                {
                    uint8_t body[2] = {_channelId, 0x00};
                    sendKnxIp(ST_DISCONNECT_RES, body, 2);
                    detail::sockClose(_sock);
                    _sock = (detail::socket_t)-1;
                    _channelId = 0;
                    if (!_quiet) _p.status(Tpl::Stat::Warn, _i.tr("server closed the tunnel", "Server hat den Tunnel geschlossen"));
                    return false; // -> run() reconnects
                }
                else if (st == ST_CONNSTATE_RES)
                    _lastAlive = nowMs(); // keep-alive answered -> tunnel healthy
                // DISCONNECT_RESPONSE: nothing to do.
            }
        }
        // A busmon telegram with no ACK following in this batch is (per ETS) NOT acknowledged -> flush green.
        if (mode == Mode::Bus && _pend.active && (nowMs() - _pend.ms) > 30) flushPending();
        // Keep-alive every ~30 s.
        if ((nowMs() - _lastKeepalive) >= 30000)
        {
            uint8_t body[2 + 8] = {_channelId, 0x00, 0x08, 0x01, 0, 0, 0, 0,
                                   (uint8_t)(_localPort >> 8), (uint8_t)(_localPort & 0xFF)};
            sendKnxIp(ST_CONNSTATE_REQ, body, sizeof(body));
            _lastKeepalive = nowMs();
        }
        // Liveness: silent for 2+ keep-alive rounds -> the tunnel died without a DISCONNECT -> signal dead.
        if ((nowMs() - _lastAlive) >= TUNNEL_DEAD_MS) return false;
        return true;
    }

    /**
     * @brief Tunnel died -> re-open with capped backoff until it is back (or Ctrl-C / the run's time cap).
     * @details Busmon: the server may still hold the exclusive slot for a few seconds -> keep retrying; its own
     *          timeout frees it. Returns true once reconnected, false on abort / time cap.
     */
    bool reconnect(Mode mode, const volatile std::sig_atomic_t* abort, uint64_t startMs, int maxSeconds)
    {
        if (!_quiet) _p.status(Tpl::Stat::Warn, _i.tr("tunnel lost — reconnecting…", "Tunnel verloren — Reconnect…"));
        else
            std::fprintf(stderr, "[reconnect] tunnel lost -- reconnecting...\n");
        close(); // drop any stale socket (idempotent)
        uint64_t backoff = 500;
        for (int attempt = 1;; ++attempt)
        {
            if (abort && *abort) return false;
            if (maxSeconds > 0 && (int)((detail::nowMs() - startMs) / 1000) >= maxSeconds) return false;
            std::string err;
            if (open(mode, err))
            {
                if (!_quiet)
                    _p.status(Tpl::Stat::Ok, _i.tr("reconnected", "wieder verbunden"),
                              {std::string("IA ") + Tpl::pa(_assignedPA), std::string("ch ") + std::to_string(_channelId)});
                else
                    std::fprintf(stderr, "[reconnect] ok (ch %d)\n", (int)_channelId);
                return true;
            }
            if (!_quiet) _p.status(Tpl::Stat::Warn, _i.tr("retry", "Neuversuch"), {err, std::string("#") + std::to_string(attempt)});
            sleepAbortable(backoff, abort);
            if (backoff < 8000) backoff *= 2; // cap ~8 s between tries
        }
    }

    /**
     * @brief Sleep up to @p ms, in small slices so Ctrl-C stays responsive.
     */
    static void sleepAbortable(uint64_t ms, const volatile std::sig_atomic_t* abort)
    {
        const uint64_t end = detail::nowMs() + ms;
        while ((int64_t)(end - detail::nowMs()) > 0)
        {
            if (abort && *abort) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    /**********************************************************************
     *************************** cEMI DISPATCH ***************************
     **********************************************************************/
    /**
     * @brief Route one cEMI by message code. Bounds-checked; a short/malformed frame is dropped silently.
     */
    void handleCemi(Mode mode, const uint8_t* cemi, int len)
    {
        if (len < 2) return;
        const uint8_t mc = cemi[0];
        const int addil = cemi[1];
        const int off = 2 + addil; // start of ctrl1 (L_Data) or raw LPDU (L_Busmon)
        if (off > len) return;
        if (mode == Mode::Group && mc == MC_LDATA_IND) decodeGroup(cemi + off, len - off);
        else if (mode == Mode::Bus && mc == MC_LBUSMON_IND)
            decodeBusmon(cemi + off, len - off, cemi + 2, addil);
    }

    /**********************************************************************
     ******************* GROUP MONITOR (L_Data.ind) ********************
     **********************************************************************/
    /**
     * @brief Decode a group L_Data.ind body `[ctrl1][ctrl2][src2][dst2][len][tpdu...]` to one monitor line.
     */
    void decodeGroup(const uint8_t* d, int n)
    {
        if (n < 8) return; // ctrl1,ctrl2,src2,dst2,len,>=1 tpdu
        const uint8_t ctrl2 = d[1];
        const uint16_t src = (uint16_t)((d[2] << 8) | d[3]);
        const uint16_t dst = (uint16_t)((d[4] << 8) | d[5]);
        const uint8_t nlen = d[6]; // NPDU octet count (TPDU bytes after byte0)
        const int tpduLen = nlen + 1;
        if (7 + tpduLen > n || tpduLen < 1) return; // never over-read
        const bool group = (ctrl2 & 0x80) != 0;
        const uint8_t* tpdu = d + 7;

        // Group telegram (connectionless GroupValue services).
        if (group && tpduLen >= 2 && (tpdu[0] & 0xC0) == 0x00)
        {
            const uint16_t apci = (uint16_t)(((tpdu[0] & 0x03) << 8) | tpdu[1]);
            const uint16_t svc = apci & 0x03C0; // group service selector
            char s;                             // 'R' read · 'r' response · 'W' write
            if (svc == 0x0000) s = 'R';
            else if (svc == 0x0040)
                s = 'r';
            else if (svc == 0x0080)
                s = 'W';
            else
                return; // not a GroupValue service

            std::string val, rawApdu;
            if (s != 'R')
            {
                const int dataBytes = nlen - 1;                           // octets past byte1
                if (dataBytes <= 0) val = std::to_string(tpdu[1] & 0x3F); // 6-bit compact value
                else
                    val = hex(tpdu + 2, dataBytes); // longer value -> hex
            }
            if (_verbose) rawApdu = hex(tpdu, tpduLen);
            emitGroup(s, src, dst, val, rawApdu);
            _frames++;
            return;
        }

        // Point-to-point: surface OpenKNX FTC file transfer / console + any NAMED management APCI. Bare transport
        // control (T_Connect/ACK) and unrecognised P2P are dropped so the group view stays clean.
        if (tpduLen >= 2 && (tpdu[0] & 0xC0) != 0x00)
        {
            const uint16_t apci = (uint16_t)(((tpdu[0] & 0x03) << 8) | tpdu[1]);
            const bool isFtc = !interpretFtc(apci, tpdu, tpduLen).empty();
            if (!isFtc && apciName(apci) == nullptr) return; // unrecognised P2P -> skip (no flood)
            emitP2P(src, dst, interpret(src, dst, false, tpdu, tpduLen));
            _frames++;
        }
    }

    /**
     * @brief Render a point-to-point (FTC / management) line in the group monitor.
     * @details `dec` already carries the coloured "SRC → DEST  service"; we prepend the row index + timestamp.
     */
    void emitP2P(uint16_t src, uint16_t dst, const std::string& dec)
    {
        if (_quiet)
        {
            std::printf("P2P\t%s\t%s\t%s\n", Tpl::pa(src).c_str(), Tpl::pa(dst).c_str(), stripAnsi(dec).c_str());
            std::fflush(stdout);
            return;
        }
        Theme& c = _c;
        std::printf("  %s %s   %s\n", c.dim(std::to_string(_frames + 1)).c_str(), c.dim(timeStr()).c_str(), dec.c_str());
        std::fflush(stdout);
    }

    /**
     * @brief Print the group-monitor table header (one-off, non-quiet only).
     */
    void groupHeader()
    {
        Theme& c = _c;
        _p.tableRow({c.dim("#"), c.dim(_i.tr("TIME", "ZEIT")), c.dim("SRC"), c.dim("DEST"),
                     c.dim(_i.tr("SERVICE", "DIENST")), c.dim(_i.tr("VALUE", "WERT"))},
                    {5, 13, 8, 9, 9, 0});
    }

    /**
     * @brief Render one decoded group telegram (or a plain tab-separated line under -q).
     */
    void emitGroup(char s, uint16_t src, uint16_t dst, const std::string& val, const std::string& rawApdu)
    {
        const std::string ga = gaStr(dst);
        if (_quiet)
        {
            const char* svc = s == 'R' ? "Read" : s == 'r' ? "Response"
                                                           : "Write";
            std::printf("%s\t%s\t%s\t%s\t%s\n", svc, Tpl::pa(src).c_str(), ga.c_str(), val.c_str(),
                        rawApdu.c_str());
            std::fflush(stdout);
            return;
        }
        Theme& c = _c;
        std::string value = val.empty() ? c.dim("—") : c.bold(val);
        if (_verbose && !rawApdu.empty()) value += c.dim("   [" + rawApdu + "]");
        _p.tableRow({c.dim(std::to_string(_frames + 1)), c.txt(timeStr()), c.txt(Tpl::pa(src)),
                     c.cyan(ga), _p.svcTag(s), value},
                    {5, 13, 8, 9, 9, 0});
        std::fflush(stdout); // live: don't let a redirected sink block-buffer the monitor lines
    }

    /**********************************************************************
     ******************* BUS MONITOR (L_Busmon.ind) ********************
     **********************************************************************/
    /**
     * @brief Decode a raw TP1 LPDU (incl. FCS); print the full bytes (wrapped) + interpretation.
     * @details A single-octet L2 acknowledge frame colours the PRECEDING telegram (ETS rule). `addinfo`/`ail`
     *          are the cEMI add-info (status AI only — no ACK bit).
     */
    void decodeBusmon(const uint8_t* lpdu, int n, const uint8_t* addinfo, int ail)
    {
        (void)addinfo;
        (void)ail; // status AI (0x03) carries error/lost/seq only — no ACK bit (verified vs. device)
        if (n <= 0) return;
        _frames++;

        // Single-octet L2 control acknowledge: 0xCC ACK · 0xC0 BUSY · 0x0C NAK. It follows a telegram, so it
        // resolves the pending telegram's ACK colour rather than printing as its own line.
        if (n == 1)
        {
            const uint8_t o = lpdu[0];
            const bool ack = (o == 0xCC);
            if (_pend.active) flushPending(ack);
            else if (_verbose) // a stray ack with no telegram in hand — only surface it in verbose
            {
                const char* k = o == 0xCC ? "ACK" : o == 0xC0 ? "BUSY"
                                                : o == 0x0C   ? "NAK"
                                                              : "?";
                if (_quiet) std::printf("%s\t%s\n", hex(lpdu, 1).c_str(), k);
                else
                    std::printf("  %s %s\n", _c.dim(hex(lpdu, 1)).c_str(), _c.dim(k).c_str());
            }
            return;
        }

        // A new telegram arrived while one was pending -> the pending one had no ACK following -> flush green.
        if (_pend.active) flushPending(false);

        // Parse control + addresses. Standard: ctrl bit7=1. Extended: ctrl bit7=0, extra CTRLE octet.
        const bool ext = (lpdu[0] & 0x80) == 0;
        uint16_t src = 0, dst = 0;
        bool group = false;
        const uint8_t* tpdu = nullptr;
        int tpduLen = 0;
        if (!ext && n >= 8)
        {
            src = (uint16_t)((lpdu[1] << 8) | lpdu[2]);
            dst = (uint16_t)((lpdu[3] << 8) | lpdu[4]);
            group = (lpdu[5] & 0x80) != 0;
            const int L = lpdu[5] & 0x0F; // LSDU length (TPDU octets after byte0)
            tpdu = lpdu + 6;
            tpduLen = L + 1;
            if (6 + tpduLen > n - 1) tpduLen = 0; // -1 keeps the trailing FCS out of the TPDU
        }
        else if (ext && n >= 9)
        {
            group = (lpdu[1] & 0x80) != 0;
            src = (uint16_t)((lpdu[2] << 8) | lpdu[3]);
            dst = (uint16_t)((lpdu[4] << 8) | lpdu[5]);
            const int L = lpdu[6]; // full length octet
            tpdu = lpdu + 7;
            tpduLen = L + 1;
            if (7 + tpduLen > n - 1) tpduLen = 0;
        }

        std::string dec = interpret(src, dst, group, tpdu, tpduLen);
        _pend = {true, detail::nowMs(), ext ? _p.chip("EXT", 'o') : _p.chip("STD", 'c'), dec, hex(lpdu, n), timeStr()};
    }

    /**********************************************************************
     ************ APCI + OpenKNX FTC DECODE (BusmonViewer.html) *********
     **********************************************************************/
    /**
     * @brief Human name for a management APCI (exact codes + the sub-data ranges). nullptr if unassigned.
     */
    static const char* apciName(uint16_t a)
    {
        if (a <= 0x03F) return "GroupValue_Read";
        if (a >= 0x040 && a <= 0x07F) return "GroupValue_Response";
        if (a >= 0x080 && a <= 0x0BF) return "GroupValue_Write";
        if (a >= 0x180 && a <= 0x1BF) return "ADC_Read";
        if (a >= 0x1C0 && a <= 0x1C7) return "ADC_Response";
        if (a >= 0x200 && a <= 0x20F) return "Memory_Read";
        if (a >= 0x240 && a <= 0x24F) return "Memory_Response";
        if (a >= 0x280 && a <= 0x28F) return "Memory_Write";
        switch (a)
        {
            case 0x0C0: return "IndividualAddress_Write";
            case 0x100: return "IndividualAddress_Read";
            case 0x140: return "IndividualAddress_Response";
            case 0x1C8: return "SystemNetworkParameter_Read";
            case 0x1C9: return "SystemNetworkParameter_Response";
            case 0x1CA: return "SystemNetworkParameter_Write";
            case 0x1CC: return "PropertyValueExt_Read";
            case 0x1CD: return "PropertyValueExt_Response";
            case 0x2C0: return "UserMemory_Read";
            case 0x2C1: return "UserMemory_Response";
            case 0x2C2: return "UserMemory_Write";
            case 0x2C5: return "UserManufacturerInfo_Read";
            case 0x2C6: return "UserManufacturerInfo_Response";
            case 0x2C7: return "FunctionProperty_Command";
            case 0x2C8: return "FunctionProperty_State";
            case 0x2C9: return "FunctionProperty_StateResponse";
            case 0x300: return "DeviceDescriptor_Read";
            case 0x340: return "DeviceDescriptor_Response";
            case 0x380: return "Restart";
            case 0x381: return "Restart_MasterReset";
            case 0x3D1: return "Authorize_Request";
            case 0x3D2: return "Authorize_Response";
            case 0x3D3: return "Key_Write";
            case 0x3D4: return "Key_Response";
            case 0x3D5: return "PropertyValue_Read";
            case 0x3D6: return "PropertyValue_Response";
            case 0x3D7: return "PropertyValue_Write";
            case 0x3D8: return "PropertyDescription_Read";
            case 0x3D9: return "PropertyDescription_Response";
            case 0x3DC: return "IndividualAddressSerialNumber_Read";
            case 0x3DD: return "IndividualAddressSerialNumber_Response";
            case 0x3DE: return "IndividualAddressSerialNumber_Write";
            case 0x3F1: return "SecureService";
            default: return nullptr;
        }
    }

    /**
     * @brief OpenKNX FTC command name for a FunctionProperty PID on object 159 (files). nullptr if unknown.
     */
    static const char* ftmCmdName(uint8_t pid)
    {
        switch (pid)
        {
            case 0: return "Format";
            case 1: return "Exists";
            case 2: return "Rename";
            case 40: return "FileUpload";
            case 41: return "FileDownload";
            case 42: return "FileDelete";
            case 43: return "FileInfo";
            case 44: return "FileUploadFast";
            case 45: return "FileReport";
            case 46: return "FilesystemInfo";
            case 80: return "DirList";
            case 81: return "DirCreate";
            case 82: return "DirDelete";
            case 90: return "Cancel";
            case 100: return "ModuleVersion";
            case 101: return "FwUpdate";
            case 102: return "CheckFeatures";
            default: return nullptr;
        }
    }

    /**
     * @brief A human byte size (B / KB / MB) for the FS/file decode.
     */
    static std::string humanSize(uint32_t v)
    {
        char b[24];
        if (v >= 1048576u) std::snprintf(b, sizeof(b), "%.2f MB", v / 1048576.0);
        else if (v >= 1024u)
            std::snprintf(b, sizeof(b), "%.1f KB", v / 1024.0);
        else
            std::snprintf(b, sizeof(b), "%u B", v);
        return b;
    }

    /**
     * @brief Printable ASCII from a payload (file/dir path hint); non-printables collapse to a space.
     */
    static std::string asciiHint(const uint8_t* p, int n, int cap = 48)
    {
        std::string s;
        for (int i = 0; i < n && (int)s.size() < cap; ++i)
            s += (p[i] >= 32 && p[i] < 127) ? (char)p[i] : ' ';
        // trim + squeeze runs of spaces
        std::string o;
        bool sp = false;
        for (char ch : s)
        {
            if (ch == ' ')
            {
                if (!sp && !o.empty()) o += ' ';
                sp = true;
            }
            else
            {
                o += ch;
                sp = false;
            }
        }
        while (!o.empty() && o.back() == ' ')
            o.pop_back();
        return o;
    }

    /**
     * @brief Decode an OpenKNX FTC FunctionProperty TPDU (obj 159 files / 160 console) to a one-line summary.
     * @details Returns "" when it is not FTC. Mirrors BusmonViewer.html ftcDecode: FileUploadFast REQUEST seq is
     *          LITTLE-endian, RESPONSE seq is BIG-endian; console (obj 160) payload is UTF-8 text.
     */
    std::string interpretFtc(uint16_t apci, const uint8_t* tpdu, int tpduLen)
    {
        if (apci != 0x2C7 && apci != 0x2C8 && apci != 0x2C9) return "";
        if (tpduLen < 4) return "";
        const uint8_t obj = tpdu[2], pid = tpdu[3];
        if (obj != 159 && obj != 160) return "";
        const uint8_t* p = tpdu + 4;
        const int pn = tpduLen - 4;
        const bool isResp = (apci == 0x2C9), isReq = (apci == 0x2C7);
        Theme& c = _c;
        const std::string dir = isResp ? _t.glyph("◀", "<") : isReq ? _t.glyph("▶", ">")
                                                                    : _t.glyph("·", ".");
        auto be32 = [&](int o) -> uint32_t {
            return (o + 3 < pn) ? (((uint32_t)p[o] << 24) | ((uint32_t)p[o + 1] << 16) | ((uint32_t)p[o + 2] << 8) | p[o + 3]) : 0u;
        };
        std::string head = c.chip("FTC", 'g') + " " + c.dim(dir) + " ";
        char b[96];

        if (obj == 160) // console: pid 1 = typed IN, pid 2 = device OUT; payload is UTF-8 text
        {
            const char* w = pid == 1 ? "console IN" : pid == 2 ? "console OUT"
                                                               : "console";
            std::string txt;
            for (int i = 0; i < pn; ++i)
            {
                uint8_t ch = p[i];
                if (ch == '\n' || (ch >= 32 && ch != 0x7F)) txt += (char)ch;
            }
            while (!txt.empty() && (txt.back() == '\n' || txt.back() == ' '))
                txt.pop_back();
            // one line: collapse embedded newlines for the monitor row
            for (char& ch : txt)
                if (ch == '\n') ch = ' ';
            if (txt.size() > 80) txt = txt.substr(0, 80) + "…";
            head += c.txt(w);
            if (!txt.empty()) head += c.dim(" · ") + c.bold("\"" + txt + "\"");
            return head;
        }

        // obj 159 — files
        const char* cmd = ftmCmdName(pid);
        std::string cmdName = cmd ? cmd : (std::string("cmd") + std::to_string(pid));
        head += c.txt(cmdName);

        // FileUpload (40) + FileUploadFast (44) chunk. The REQUEST sequence is LITTLE-endian for BOTH commands
        // (target reads data[1]<<8|data[0]); payload = [seqLo][seqHi][len][data…] (FileTransferModule writeChunk).
        if ((pid == 40 || pid == 44) && isReq && pn >= 2)
        {
            const uint32_t seq = (uint32_t)((p[1] << 8) | p[0]);
            if (seq == 0x0000) head += c.dim(" · ") + c.cyan("open");
            else if (seq == 0xFFFF)
                head += c.dim(" · ") + c.cyan("close");
            else
            {
                const int nb = pn >= 3 ? p[2] : 0;
                std::snprintf(b, sizeof(b), " · chunk #%u · %d B", seq, nb);
                head += c.dim(b);
            }
        }
        // Upload ACK: [result][seqHi][seqLo][crcHi][crcLo] — the RESPONSE seq is BIG-endian (pushWord).
        else if ((pid == 40 || pid == 44) && isResp && pn >= 3)
        {
            std::snprintf(b, sizeof(b), " · ack #%u", (uint32_t)((p[1] << 8) | p[2]));
            head += c.dim(b);
        }
        // FileDownload chunk: [result][seqHi][seqLo][data…] — response seq BIG-endian.
        else if (pid == 41 && isResp && pn >= 3)
        {
            std::snprintf(b, sizeof(b), " · chunk #%u · %d B", (uint32_t)((p[1] << 8) | p[2]), pn - 3);
            head += c.dim(b);
        }
        else if (pid == 46 && isResp && pn >= 9) // FilesystemInfo: [status][totalBE32][usedBE32]
        {
            const uint32_t t = be32(1), u = be32(5);
            head += c.dim(" · ") + c.txt("total " + humanSize(t) + " · used " + humanSize(u) +
                                         " · free " + humanSize(t > u ? t - u : 0));
        }
        else if (pid == 43 && isResp) // FileInfo: 0x42 = not found; else [status][sizeBE32][crc32BE32]
        {
            if (pn >= 1 && p[0] == 0x42) head += c.dim(" · ") + c.amber("not found");
            else if (pn >= 9)
            {
                std::snprintf(b, sizeof(b), " · size %s · CRC32 0x%08X", humanSize(be32(1)).c_str(), be32(5));
                head += c.dim(b);
            }
        }
        else if (pid == 100 && isResp && pn) // ModuleVersion
        {
            std::string v = asciiHint(p, pn, 24);
            head += c.dim(" · ") + c.txt(v.empty() ? std::string("v?") : v);
        }
        else if (isReq && pn) // command with a path (file/dir ops carry the path as ASCII)
        {
            std::string a = asciiHint(p, pn);
            if (a.size() >= 2) head += c.dim(" · ") + c.txt(a);
        }
        return head;
    }

    /**
     * @brief Build the human interpretation `SRC → DEST  Service value` for a busmon telegram.
     * @details Handles transport control (T_Connect/ACK/NAK), GroupValue services, OpenKNX FTC, and any named
     *          management APCI (tagged P2P + seq when connection-oriented).
     */
    std::string interpret(uint16_t src, uint16_t dst, bool group, const uint8_t* tpdu, int tpduLen)
    {
        Theme& c = _c;
        std::string out = c.txt(Tpl::pa(src)) + c.dim(_t.glyph(" → ", " -> ")) +
                          (group ? c.cyan(gaStr(dst)) : c.txt(Tpl::pa(dst)));
        if (!tpdu || tpduLen < 1) return out; // control / short frame — addresses only
        const uint8_t tt = tpdu[0] & 0xC0;
        // Transport control (no APCI): T_Connect / T_Disconnect / T_ACK / T_NAK — the P2P connection framing.
        if (tt == 0x80)
        {
            const uint8_t k = tpdu[0] & 0x3F;
            out += "  " + c.dim(k == 0 ? "T_Connect" : k == 1 ? "T_Disconnect"
                                                              : "T_Ctrl");
            return out;
        }
        if (tt == 0xC0)
        {
            char b[24];
            std::snprintf(b, sizeof(b), "%s seq=%d", (tpdu[0] & 3) == 2 ? "T_ACK" : (tpdu[0] & 3) == 3 ? "T_NAK"
                                                                                                       : "T_Ctrl",
                          (tpdu[0] >> 2) & 0x0F);
            out += "  " + c.dim(b);
            return out;
        }
        if (tpduLen < 2)
        {
            out += "  " + c.dim(_i.tr("(no APCI)", "(kein APCI)"));
            return out;
        }
        const bool connected = (tt == 0x40); // T_Data_Connected -> a P2P sequence (FTC, downloads, mgmt)
        const uint16_t apci = (uint16_t)(((tpdu[0] & 0x03) << 8) | tpdu[1]);
        const uint16_t svc = apci & 0x03C0;
        if (group && (svc == 0x0000 || svc == 0x0040 || svc == 0x0080))
        {
            const char s = svc == 0x0000 ? 'R' : svc == 0x0040 ? 'r'
                                                               : 'W';
            out += "  " + _p.svcTag(s);
            if (s != 'R')
            {
                const int dataBytes = tpduLen - 2;
                std::string v = dataBytes <= 0 ? std::to_string(tpdu[1] & 0x3F) : hex(tpdu + 2, dataBytes);
                out += "  " + c.bold(v);
            }
            return out;
        }
        // OpenKNX FTC file transfer / console (FunctionProperty on obj 159/160) — decode it in full.
        std::string ftc = interpretFtc(apci, tpdu, tpduLen);
        if (!ftc.empty())
        {
            out += "  " + ftc;
            return out;
        }
        // Any other management APCI: name it (ETS-style), tag P2P + the connection seq when connection-oriented.
        const char* nm = apciName(apci);
        if (connected) out += "  " + c.dim("P2P");
        if (nm) out += "  " + c.txt(nm);
        else
        {
            char b[24];
            std::snprintf(b, sizeof(b), "APCI 0x%03X", apci);
            out += "  " + c.dim(b);
        }
        if (connected)
        {
            char b[16];
            std::snprintf(b, sizeof(b), " seq=%d", (tpdu[0] >> 2) & 0x0F);
            out += c.dim(b);
        }
        return out;
    }

    /**
     * @brief Emit the pending busmon telegram, coloured by the ETS ACK rule (green = NOT acknowledged).
     */
    void flushPending(bool acked = false)
    {
        if (!_pend.active) return;
        _pend.active = false;
        if (_quiet)
        {
            std::printf("%s\t%s\t%s\n", _pend.raw.c_str(), stripAnsi(_pend.dec).c_str(), acked ? "ACK" : "NAK");
            std::fflush(stdout);
            return;
        }
        Theme& c = _c;
        // ETS: acknowledged -> normal (grey/white) · NOT acknowledged -> green (both the line and its tag hint).
        std::string dec = acked ? _pend.dec : c.green(stripAnsi(_pend.dec));
        std::string ackTag = acked ? c.dim("ACK") : c.green(_t.glyph("— NAK", "- NAK"));
        std::printf("  %s %s   %s   %s\n", _pend.tag.c_str(), c.dim(_pend.time).c_str(), dec.c_str(), ackTag.c_str());
        _p.wrap(_pend.raw, Tpl::cols() - 4, 6); // FULL bytes, hanging indent, resize-safe (no truncation)
        std::fflush(stdout);
    }

    /**********************************************************************
     ***************************** HELPERS *******************************
     **********************************************************************/
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
     * @brief Put the tunnel socket into non-blocking mode (recv returns EWOULDBLOCK instead of parking).
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
     * @brief A KNX group address `main/mid/sub` from the packed 16-bit destination.
     */
    static std::string gaStr(uint16_t g)
    {
        char b[16];
        std::snprintf(b, sizeof(b), "%u/%u/%u", (g >> 11) & 0x1F, (g >> 8) & 0x07, g & 0xFF);
        return b;
    }

    /**
     * @brief Local wall-clock `HH:MM:SS.mmm` for the monitor line.
     */
    static std::string timeStr()
    {
        const uint64_t ms = detail::nowMs();
        std::time_t tt = std::time(nullptr);
        std::tm tmv{};
#ifdef _WIN32
        localtime_s(&tmv, &tt);
#else
        localtime_r(&tt, &tmv);
#endif
        char b[16];
        std::snprintf(b, sizeof(b), "%02d:%02d:%02d.%03u", tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
                      (unsigned)(ms % 1000));
        return b;
    }

    /**
     * @brief Strip our own ANSI SGR so a value can be re-coloured (green) or written plain under -q.
     */
    static std::string stripAnsi(const std::string& s)
    {
        std::string o;
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (s[i] == '\x1b')
            {
                size_t j = i + 1;
                if (j < s.size() && s[j] == '[')
                {
                    ++j;
                    while (j < s.size() && !(s[j] >= 0x40 && s[j] <= 0x7e))
                        ++j;
                }
                i = j;
                continue;
            }
            o += s[i];
        }
        return o;
    }

    /**
     * @brief The connect-status hint (busmonitor exclusivity is the common bus-monitor refusal).
     */
    std::string connStatus(uint8_t st, Mode mode)
    {
        char b[96];
        const char* name = st == 0x22 ? "E_CONNECTION_TYPE" : st == 0x23 ? "E_TUNNELLING_LAYER"
                                                          : st == 0x24   ? "E_NO_MORE_CONNECTIONS"
                                                          : st == 0x25   ? "E_NO_MORE_UNIQUE_CONNECTIONS"
                                                          : st == 0x26   ? "E_CONNECTION_OPTION"
                                                          : st == 0x29   ? "E_TUNNELLING_LAYER"
                                                                         : "E_*";
        if (mode == Mode::Bus)
            std::snprintf(b, sizeof(b), "0x%02X %s%s", st, name,
                          _i.tr(" — a busmonitor already owns this subnet (exclusive)",
                                " — ein Busmonitor belegt dieses Subnetz bereits (exklusiv)"));
        else
            std::snprintf(b, sizeof(b), "0x%02X %s", st, name);
        return b;
    }

  public:
    /**
     * @brief Set the target before run(): the interface IP + control port.
     */
    void target(const std::string& ip, uint16_t port)
    {
        _ip = ip;
        _port = port;
    }

    /**
     * @brief Offline decode of a raw TP1 LPDU (hex string, incl. FCS) — no tunnel.
     * @details Runs the exact busmon interpreter (addresses · TPCI/APCI · OpenKNX FTC/console) and prints one
     *          decoded line. Mirrors BusmonViewer.html for a single frame. Returns 0 on a parseable frame, 1 if
     *          the hex was empty/too short.
     */
    int decodeOffline(const std::string& hexIn, bool quiet, bool verbose)
    {
        _quiet = quiet;
        _verbose = verbose;
        uint8_t lpdu[256];
        int n = 0;
        uint8_t hi = 0;
        bool haveHi = false;
        for (char ch : hexIn) // tolerant hex parse: skip spaces / 0x / punctuation
        {
            int v;
            if (ch >= '0' && ch <= '9') v = ch - '0';
            else if (ch >= 'a' && ch <= 'f')
                v = ch - 'a' + 10;
            else if (ch >= 'A' && ch <= 'F')
                v = ch - 'A' + 10;
            else
                continue;
            if (!haveHi)
            {
                hi = (uint8_t)v;
                haveHi = true;
            }
            else
            {
                if (n < (int)sizeof(lpdu)) lpdu[n++] = (uint8_t)((hi << 4) | v);
                haveHi = false;
            }
        }
        if (n < 1)
        {
            std::fprintf(stderr, "decode: no LPDU bytes in input\n");
            return 1;
        }
        decodeBusmon(lpdu, n, nullptr, 0);
        flushPending(false); // offline: no following L2 ACK -> render as not-acknowledged (green)
        return 0;
    }

  private:
    std::string _ip;
    uint16_t _port = 3671;
};

} // namespace ftc

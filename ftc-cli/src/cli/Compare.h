/**
 * @file        Compare.h
 * @brief       Live A/B fidelity diff of two KNXnet/IP monitors (busmon or groupmon) on one TP bus.
 * @details     Owns two ftc::Monitor instances (A = the -i interface, B = the compare arg), sinks their decoded
 *              frames into per-side rolling multisets and matches them within a grace window: a frame seen on
 *              both = common, unmatched = only-A / only-B, an imbalance of the same key = count-mismatch. With
 *              the diff turned OFF it degrades to a plain two-stream "multi" viewer for two interfaces on
 *              different lines. Three layouts (mix / side / stack), a live counters + result footer, hot-keys
 *              and an ETS-telegram-shaped XML export. Header-only, presentation via Tpl/Theme/Term/I18n.
 * @date        2026-08-10
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include <csignal>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "I18n.h"
#include "Keys.h"
#include "Monitor.h"
#include "Templates.h"
#include "Term.h"
#include "Theme.h"

namespace ftc
{

/**
 * @brief The A/B compare + multi view. Construct over the shared CLI refs, then run().
 */
class Compare
{
  public:
    /** @brief The rendering layout (compare only). */
    enum class Layout
    {
        Mix,  ///< one merged stream, each frame prefixed by a source tag
        Side, ///< two aligned columns A | B
        Stack ///< per-interface lanes + a divergence marker
    };

    Compare(Term& term, Theme& theme, Tpl& tpl, I18n& i18n) : _t(term), _c(theme), _p(tpl), _i(i18n),
                                                              _A(term, theme, tpl, i18n), _B(term, theme, tpl, i18n) {}

    /**
     * @brief Run the compare/multi view until @p abort, @p maxFrames or @p maxSeconds (0 = unlimited).
     * @return 0 identical · 2 divergent · 1 a tunnel could not be opened · 130 Ctrl+C.
     */
    int run(Monitor::Mode mode, const std::string& ipA, uint16_t portA, const std::string& ipB, uint16_t portB,
            bool quiet, bool verbose, int maxFrames, int maxSeconds, int graceMs, bool diffOn, bool normalize,
            bool onlyDiff, bool collapse, bool markers, bool skew, const volatile std::sig_atomic_t* abort)
    {
        _ipA = ipA;
        _ipB = ipB;
        _graceMs = graceMs > 0 ? graceMs : 750;
        _diffOn = diffOn;
        _normalize = normalize;
        _fltDiv = onlyDiff;
        _collapse = collapse;
        _markers = markers;
        _skew = skew;
        _quiet = quiet;
        _render = _t.isTty() && !quiet;

        _A.target(ipA, portA);
        _B.target(ipB, portB);
        _A.setQuietVerbose(true, verbose);
        _B.setQuietVerbose(true, verbose);
        _A.sink([this](const MonFrame& f) { _inbox.push_back({0, f}); });
        _B.sink([this](const MonFrame& f) { _inbox.push_back({1, f}); });

        // A (the primary -i interface) opens blocking — its failure is a hard "cannot compare" (exit 1). B connects
        // COOPERATIVELY from the start (marked down + immediate retry), so an unreachable B never blocks startup or
        // the loop: A streams and stays responsive while B keeps retrying in the background.
        std::string eA;
        if (!_A.openTunnel(mode, eA))
        {
            fail(_ipA, eA);
            return 1;
        }
        _down[1] = true;
        _connecting[1] = false;
        _retryAt[1] = detail::nowMs();
        _backoff[1] = 500;

        if (!_quiet) banner(mode);

        // Keys enters raw mode in its ctor -> construct it ONLY when interactive (never touch the terminal under -q).
        std::unique_ptr<Keys> keys;
        if (_render) keys.reset(new Keys());
        const uint64_t start = detail::nowMs();
        _startMs = start;
        bool aborted = false, userStop = false;
        for (;;)
        {
            if (abort && *abort)
            {
                aborted = true;
                break;
            }
            if (_render && handleKeys(*keys, mode, abort, start, maxSeconds, userStop)) break;
            if (userStop) break;
            if (maxSeconds > 0 && (int)((detail::nowMs() - start) / 1000) >= maxSeconds) break;
            if (maxFrames > 0 && _total >= (uint64_t)maxFrames) break;

            serviceSide(0, mode, abort, start, maxSeconds);
            serviceSide(1, mode, abort, start, maxSeconds);

            std::vector<std::string> out;
            processInbox(out);
            normFlushStale(detail::nowMs(), out); // surface a lost reassembly tail after its burst ended
            expireHolding(out, detail::nowMs());
            collapseTick(out, detail::nowMs()); // flush a settled collapse run so it appears promptly

            if (_render)
            {
                const uint64_t sinceDraw = detail::nowMs() - _lastDraw;
                const bool due = !out.empty() || sinceDraw >= 500 ||
                                 ((reasmPending(0) || reasmPending(1)) && sinceDraw >= 200);
                if (due)
                {
                    footerClear();
                    for (const auto& l : out)
                        std::printf("%s\n", l.c_str());
                    drawFooter(mode);
                    _lastDraw = detail::nowMs();
                    std::fflush(stdout);
                }
            }
        }

        // Teardown: flush each side's still-pending busmon telegram INTO the inbox FIRST, then drain + grace it out,
        // so the last un-acked frame is counted / matched / rendered / captured (not dropped).
        _tearing = true; // teardown frames are late-edge (their partner may arrive after capture ends)
        _A.flushBus();
        _B.flushBus();
        std::vector<std::string> tail;
        processInbox(tail);
        normFlushAll(tail); // flush any bytes still in the reassembly accumulators (tagged, never dropped)
        expireHolding(tail, detail::nowMs() + (uint64_t)_graceMs + 1);
        collapseFlush(tail); // emit the final collapse run
        if (_render)
        {
            footerClear();
            for (const auto& l : tail)
                std::printf("%s\n", l.c_str());
        }

        const int rc = finish(mode, aborted, userStop);
        _A.closeTunnel();
        _B.closeTunnel();
        return rc;
    }

  private:
    /*********************************************************************
     ******************************* STATE *******************************
     ********************************************************************/
    static constexpr size_t HOLD_MAX = 4096;     ///< per-side unmatched-frame window
    static constexpr size_t KEYCNT_MAX = 8192;   ///< per-key cumulative-count table bound
    static constexpr size_t ANN_MAX = 100000;     ///< captured annotated frames for XML export
    static constexpr size_t NORM_ACC_MAX = 1024; ///< reassembly accumulator bound (max TP1 telegram ~263 B)
    static constexpr uint64_t NORM_STALL_MS = 400; ///< a non-empty accumulator idle this long = a lost tail -> flush

    /** @brief Per-side FCS-anchored reassembly accumulator (normalize), gated by the busmon Lost + sequence. */
    struct NormAcc
    {
        std::vector<uint8_t> acc;    ///< bytes of the telegram currently being reassembled
        uint64_t lastMs = 0;         ///< last append time (stall detection)
        uint64_t startMs = 0;        ///< when the current accumulation began (live "reassembling…" placeholder)
        bool haveSeq = false;        ///< a prior indication set an expected next sequence number
        uint8_t expSeq = 0;          ///< the sequence the next indication must carry to continue the chain
        // integrity / ack carried onto the reassembled whole (all-stat AND · F/B/P OR · completing piece's ack):
        bool accInit = false;
        bool accStat = true, accFerr = false, accBerr = false, accPerr = false;
        uint8_t accAck = 0;
        uint16_t accPieces = 0; // indications joined into the current telegram (display marker)
    };

    enum EvKind
    {
        Common,            ///< same content AND all reported metadata agrees
        OnlyA,             ///< content only A saw
        OnlyB,             ///< content only B saw
        Mismatch,          ///< same content, different capture count (content multiset imbalance)
        IntegrityMismatch, ///< same content, sides disagree on the 0x03 F/B/P error flags
        AckMismatch,       ///< same content, sides disagree on the L2 acknowledge (ACK/NAK/BUSY/none)
        NotReported        ///< same content, a metadata dimension is not comparable (a side did not report it)
    };

    struct Held
    {
        MonFrame f;
        uint64_t expiry = 0;
    };
    struct Ann
    {
        MonFrame f;         ///< the (A-side, or the sole) frame
        const char* seenBy; ///< "A" / "B" / "AB"
        const char* diff;   ///< "common" / "onlyA" / "onlyB" / "mismatch" / "integrity" / "ack" / "notReported" / "stream"
        uint32_t countA = 0, countB = 0;
        bool meta = false;  ///< carries the paired A/B metadata below (a content-matched pair)
        MonFrame fb;        ///< the B-side frame of the pair (for the XML per-telegram A/B status/ack)
    };

    Term& _t;
    Theme& _c;
    Tpl& _p;
    I18n& _i;
    Monitor _A, _B;
    std::string _ipA, _ipB;
    int _graceMs = 750;
    bool _diffOn = true;
    bool _quiet = false, _render = false, _paused = false;
    bool _normalize = true; ///< compare-only: FCS-anchored fragment reassembly (default ON, key 'n')
    Layout _layout = Layout::Mix;

    NormAcc _norm[2];                              // per-side reassembly accumulators
    uint64_t _normIncomplete[2] = {0, 0};         // tagged-incomplete emissions per side (lost tails / partials)
    bool _normCapWarned = false;

    std::vector<std::pair<int, MonFrame>> _inbox; // side (0/1) + frame, filled by the sinks
    std::deque<Held> _hold[2];                    // per-side unmatched frames awaiting a partner
    std::unordered_map<std::string, std::pair<uint32_t, uint32_t>> _keyCount; // key -> (A count, B count)
    std::vector<Ann> _ann;                        // annotated capture for XML
    bool _keyCntWarned = false, _holdWarned = false, _annWarned = false;

    uint64_t _seenA = 0, _seenB = 0, _common = 0, _onlyA = 0, _onlyB = 0, _mismatch = 0, _total = 0;
    // metadata-fidelity classes on content-matched telegrams (distinct from the content diff, never folded in):
    uint64_t _integMismatch = 0, _ackMismatch = 0, _notReported = 0;
    uint64_t _lost[2] = {0, 0};    // Lost=1 annotations per side (a receiver that knows it dropped data)
    bool _ackSeen[2] = {false, false}; // did a side EVER deliver an L2 acknowledge octet this session?
    bool _statSeen[2] = {false, false}; // did a side EVER deliver the 0x03 status add-info this session?

    // Readability pack — DISPLAY-ONLY toggles (never touch the diff logic, counters, exit code, TSV or XML):
    bool _fltDiv = false;      // f: show only non-common frames
    bool _collapse = false;    // c: collapse consecutive identical-content frames into one counted line
    bool _markers = true;      // m: per-frame (Repeat) / (reassembled ×N) / (edge) tags
    bool _skew = false;        // t: A↔B capture-time delta on matched pairs
    uint64_t _startMs = 0;     // capture start (edge window + short-capture hint)
    bool _tearing = false;     // teardown flush in progress (its frames are late-edge)
    bool _reconnected = false; // a live tunnel dropped + reconnected mid-run (reliability hint)

    /** @brief Per-service divergence tally for the summary breakdown (sums to the counter totals). */
    struct Svc { uint64_t onlyA = 0, onlyB = 0, mismatch = 0, integ = 0, ack = 0, notrep = 0; };
    std::map<std::string, Svc> _svc;

    /** @brief The collapse accumulator: one run of consecutive identical-content frames. */
    struct CRun
    {
        bool active = false;
        std::string key;
        uint32_t aCount = 0, bCount = 0, events = 0;
        EvKind kind = Common;
        int repSide = 0;
        bool hasB = false, edge = false;
        MonFrame a, b;
        uint64_t lastMs = 0;
    };
    CRun _crun;
    int _footLines = 0;
    uint64_t _lastDraw = 0;
    // per-side reconnect backoff + cooperative-connect flag (never block the shared loop for the other side / keys)
    bool _down[2] = {false, false};
    bool _connecting[2] = {false, false};
    uint64_t _retryAt[2] = {0, 0};
    uint64_t _backoff[2] = {500, 500};

    /*********************************************************************
     **************************** TUNNEL PUMP ****************************
     ********************************************************************/
    /**
     * @brief Tick one side; on a dead tunnel drop into a fully NON-BLOCKING, per-side, cooperative reconnect.
     * @details The connect wait is sliced across loop iterations (sendConnect once, pollReopen(0) each pass), so an
     *          unreachable B never freezes A's frames, the keyboard or the render — no 3 s stall.
     */
    void serviceSide(int side, Monitor::Mode mode, const volatile std::sig_atomic_t* abort, uint64_t start, int maxSeconds)
    {
        (void)start;
        (void)maxSeconds;
        Monitor& m = side == 0 ? _A : _B;
        if (_down[side])
        {
            if (abort && *abort) return;
            std::string err;
            if (!_connecting[side]) // between attempts: honour the backoff, then FIRE a request and return
            {
                if (detail::nowMs() < _retryAt[side]) return;
                if (m.beginReopen(mode, err))
                    _connecting[side] = true;
                else
                {
                    _retryAt[side] = detail::nowMs() + _backoff[side];
                    if (_backoff[side] < 8000) _backoff[side] *= 2;
                }
                return;
            }
            const int r = m.pollReopen(err); // in flight: one non-blocking poll (waitMs 0)
            if (r == 1)
            {
                _down[side] = _connecting[side] = false;
                _backoff[side] = 500;
                if (_render)
                {
                    footerClear();
                    _p.status(Tpl::Stat::Ok, _i.tr("reconnected", "wieder verbunden"), {sideName(side), _ipHost(side)});
                    _footLines = 0;
                }
            }
            else if (r == -1) // refused / timed out -> back off and try again later
            {
                _connecting[side] = false;
                _retryAt[side] = detail::nowMs() + _backoff[side];
                if (_backoff[side] < 8000) _backoff[side] *= 2;
            }
            return; // r == 0: still connecting -> yield to the loop, never block
        }
        if (!m.tick(mode))
        {
            _down[side] = true;
            _connecting[side] = false;
            _retryAt[side] = detail::nowMs() + 500;
            _backoff[side] = 500;
            _reconnected = true; // a live tunnel dropped mid-run -> flag the result reliability
            if (_render)
            {
                footerClear();
                _p.status(Tpl::Stat::Warn, _i.tr("tunnel lost — reconnecting", "Tunnel verloren — Reconnect"),
                          {sideName(side), _ipHost(side)});
                _footLines = 0;
            }
        }
    }

    /*********************************************************************
     ************************** MATCH + CLASSIFY *************************
     ********************************************************************/
    /** @brief Drain the sink inbox. Busmon frames pass through the normalizer first (when enabled). */
    void processInbox(std::vector<std::string>& out)
    {
        for (auto& pr : _inbox)
        {
            const int side = pr.first;
            MonFrame& f = pr.second;
            if (f.bus) // record the raw split-integrity signals per side (independent of content matching)
            {
                if (f.lost) _lost[side]++;
                if (f.ackKind != 0) _ackSeen[side] = true; // this side DOES deliver the L2 acknowledge
                if (f.stat) _statSeen[side] = true;        // this side DOES deliver the 0x03 status add-info
            }
            if (_normalize && f.bus)
                feedNorm(side, f, out); // reassemble fragments -> whole telegrams, then handleFrame() each
            else
                handleFrame(side, f, out);
        }
        _inbox.clear();
    }

    /** @brief Diff / display one (already whole) frame: count it, then match (compare) or stream it (multi). */
    void handleFrame(int side, const MonFrame& f, std::vector<std::string>& out)
    {
        (side == 0 ? _seenA : _seenB)++;
        _total++;

        if (!_diffOn) // multi: no correlation, colour hints the source only
        {
            render(out, side == 0 ? OnlyA : OnlyB, side, f, /*multi=*/true);
            return;
        }

        bumpKeyCount(side, f.key);
        const int other = side ^ 1;
        // try to pair with the oldest unmatched frame of the same key on the OTHER side (within grace)
        for (auto it = _hold[other].begin(); it != _hold[other].end(); ++it)
        {
            if (it->f.key == f.key)
            {
                const MonFrame partner = it->f; // the OTHER side's frame — needed to diff the metadata
                _hold[other].erase(it);
                const MonFrame& a = side == 0 ? f : partner;
                const MonFrame& b = side == 0 ? partner : f;
                classifyMatched(side, a, b, out);
                return;
            }
        }
        holdPush(side, f);
    }

    /**
     * @brief Content matched on both sides — now diff the fidelity metadata as DISTINCT classes (never folded
     *        into the content key): F/B/P integrity, the L2 acknowledge, and "not reported" (unknown != agree).
     * @param side the arriving side (only steers the render tag; the A/B frames are fixed by their sockets)
     */
    void classifyMatched(int side, const MonFrame& a, const MonFrame& b, std::vector<std::string>& out)
    {
        // Integrity F/B/P: comparable only when BOTH sides reported the 0x03 status for this telegram.
        bool integMismatch = false, integNotRep = false;
        if (a.stat && b.stat)
            integMismatch = (a.ferr != b.ferr) || (a.berr != b.berr) || (a.perr != b.perr);
        else if (a.stat != b.stat)
            integNotRep = true; // one side reported integrity, the other did not -> NOT agreement

        // L2 acknowledge: equal kinds (incl. both none) agree. A differing kind is a real ack-mismatch only when
        // BOTH sides deliver acks this session; if one side never reports acks the discrepancy is a capability
        // gap -> not comparable (never counted as agreement).
        bool ackMismatch = false, ackNotRep = false;
        if (a.ackKind != b.ackKind)
        {
            if (_ackSeen[0] && _ackSeen[1]) ackMismatch = true;
            else
                ackNotRep = true;
        }

        const bool notRep = integNotRep || ackNotRep;
        if (integMismatch) { _integMismatch++; _svc[svcBucket(a)].integ++; }
        if (ackMismatch) { _ackMismatch++; _svc[svcBucket(a)].ack++; }
        if (notRep) { _notReported++; _svc[svcBucket(a)].notrep++; }
        if (!integMismatch && !ackMismatch && !notRep) _common++;

        // Render once with the most-severe class; the XML annotation keeps the full A/B metadata.
        EvKind k = Common;
        if (integMismatch) k = IntegrityMismatch;
        else if (ackMismatch)
            k = AckMismatch;
        else if (notRep)
            k = NotReported;
        renderMatched(out, k, side, a, b);
    }

    /*********************************************************************
     ******************** FCS-ANCHORED REASSEMBLY ***********************
     ********************************************************************/
    /**
     * @brief Feed one busmon indication into a side's reassembly accumulator, emitting whole telegrams.
     * @details Spec-gated (NOT the PS heuristic): a bare 1-octet ACK/short frame passes straight through
     *          (never accumulated). Pieces are merged across indications ONLY while the busmon 0x03 status
     *          shows Lost=0, no frame-error, and a CONTIGUOUS sequence number (mod 8). Any break flushes the
     *          partial as TAGGED-incomplete and restarts — never merges across a loss. Telegrams are cut at the
     *          EXACT header length (STD 8+LG · EXT 9+LG) with a valid FCS; there is no search window.
     */
    void feedNorm(int side, const MonFrame& f, std::vector<std::string>& out)
    {
        // Rule 4: a complete 1-octet L2 acknowledge (0xCC/0x0C/0xC0/0x00) is not a fragment — pass through.
        if (byteLen(f.raw) <= 1)
        {
            handleFrame(side, f, out);
            return;
        }

        NormAcc& na = _norm[side];

        // No 0x03 status add-info -> the sequence cannot be verified, so this indication is SELF-CONTAINED:
        // never merge it across an indication boundary (a stat-less merge could fabricate a telegram). Extract
        // complete telegrams from it alone; tag any remainder.
        if (!f.stat)
        {
            if (!na.acc.empty())
            {
                emitIncomplete(side, na.acc, out);
                na.acc.clear();
                na.haveSeq = false;
            }
            accAppend(na, f);
            na.haveSeq = false;
            normExtract(side, out);
            if (!na.acc.empty()) // a stat-less partial cannot be safely continued -> surface it now
            {
                emitIncomplete(side, na.acc, out);
                na.acc.clear();
            }
            return;
        }

        if (!na.acc.empty())
        {
            // Mid-reassembly: this indication may only CONTINUE the chain when Lost=0, no frame-error, and the
            // sequence number is contiguous. Any break flushes the partial as tagged-incomplete (never merged).
            const bool broken = f.lost || f.ferr || (na.haveSeq && f.seq != na.expSeq);
            if (broken)
            {
                emitIncomplete(side, na.acc, out);
                na.acc.clear();
                na.haveSeq = false;
            }
        }
        accAppend(na, f);
        na.haveSeq = true;
        na.expSeq = (uint8_t)((f.seq + 1) & 0x07);

        normExtract(side, out);

        if (na.acc.size() > NORM_ACC_MAX) // bounded memory: a runaway front is a garbled / un-terminated telegram
        {
            emitIncomplete(side, na.acc, out);
            na.acc.clear();
            na.haveSeq = false;
            if (!_normCapWarned && _render)
            {
                _normCapWarned = true;
                footerClear();
                _p.status(Tpl::Stat::Warn, _i.tr("reassembly buffer capped", "Reassemblier-Puffer begrenzt"),
                          {sideName(side), std::to_string(NORM_ACC_MAX)});
                _footLines = 0;
            }
        }
    }

    /** @brief Append a piece: accumulate its integrity (all-stat AND · F/B/P OR · this piece's ack) + bytes. */
    void accAppend(NormAcc& na, const MonFrame& f)
    {
        if (na.acc.empty()) na.startMs = detail::nowMs(); // a fresh accumulation begins
        if (!na.accInit)
        {
            na.accInit = true;
            na.accStat = f.stat;
            na.accFerr = f.ferr;
            na.accBerr = f.berr;
            na.accPerr = f.perr;
            na.accPieces = 1;
        }
        else
        {
            na.accStat = na.accStat && f.stat;
            na.accFerr |= f.ferr;
            na.accBerr |= f.berr;
            na.accPerr |= f.perr;
            if (na.accPieces < 0xFFFF) na.accPieces++;
        }
        na.accAck = f.ackKind;
        appendHexBytes(na.acc, f.raw);
        na.lastMs = detail::nowMs();
    }

    /** @brief Pull every complete telegram from the front (EXACT header length + valid FCS, zero tolerance). */
    void normExtract(int side, std::vector<std::string>& out)
    {
        NormAcc& na = _norm[side];
        std::vector<uint8_t>& acc = na.acc;
        for (;;)
        {
            if (acc.empty()) break;
            const int exp = expectedLen(acc.data(), (int)acc.size());
            if (exp <= 0 || (int)acc.size() < exp) break; // need more header / more bytes for the exact length
            if (fcsOk(acc.data(), exp))
            {
                emitWhole(side, acc.data(), exp, na, out);
                acc.erase(acc.begin(), acc.begin() + exp); // leftover may hold the head of the next telegram
                if (acc.empty()) na.accInit = false;       // reset the integrity accumulator for the next telegram
                continue;
            }
            // FCS fails at the EXACT header length -> a real content/framing anomaly (never a packaging artifact,
            // because a wide search window could cut inside a frame and hide a genuine difference). Tag + reset.
            emitIncomplete(side, acc, out);
            acc.clear();
            na.haveSeq = false;
            break;
        }
    }

    /** @brief Flush a side's stalled accumulator (a lost tail after the burst ended) as tagged-incomplete. */
    void normFlushStale(uint64_t now, std::vector<std::string>& out)
    {
        if (!_normalize) return;
        for (int side = 0; side < 2; ++side)
        {
            NormAcc& na = _norm[side];
            if (na.acc.empty()) continue;
            if (now - na.lastMs <= NORM_STALL_MS) continue;
            emitIncomplete(side, na.acc, out);
            na.acc.clear();
            na.haveSeq = false;
        }
    }

    /** @brief Flush BOTH accumulators (teardown / toggle) so no accumulated bytes are stranded. */
    void normFlushAll(std::vector<std::string>& out)
    {
        for (int side = 0; side < 2; ++side)
        {
            NormAcc& na = _norm[side];
            if (na.acc.empty()) continue;
            emitIncomplete(side, na.acc, out);
            na.acc.clear();
            na.haveSeq = false;
        }
    }

    /** @brief Emit one reassembled whole telegram (decoded cleanly) into the diff/display pipeline. */
    void emitWhole(int side, const uint8_t* bytes, int len, const NormAcc& na, std::vector<std::string>& out)
    {
        MonFrame w;
        if (!_A.decodeWhole(bytes, len, w))
        {
            std::vector<uint8_t> b(bytes, bytes + len);
            emitIncomplete(side, b, out);
            return;
        }
        // Carry the split-integrity onto the whole: the ACK octet follows the WHOLE telegram, and F/B/P/stat
        // are OR/AND-accumulated over the constituent pieces so a mid-telegram error still surfaces.
        w.stat = na.accStat;
        w.ferr = na.accFerr;
        w.berr = na.accBerr;
        w.perr = na.accPerr;
        w.ackKind = na.accAck;
        w.hasAck = true;
        w.acked = (na.accAck == 1);
        w.reasm = na.accPieces > 255 ? 255 : (uint8_t)na.accPieces;
        handleFrame(side, w, out);
    }

    /** @brief Emit a tagged-incomplete blob (a lost/partial piece) — never silently dropped (VORGABE). */
    void emitIncomplete(int side, const std::vector<uint8_t>& bytes, std::vector<std::string>& out)
    {
        if (bytes.empty()) return;
        _normIncomplete[side]++;
        MonFrame w;
        w.bus = true;
        w.ms = detail::nowMs();
        w.time = Monitor::wallClock();
        w.raw = bytesToHex(bytes.data(), (int)bytes.size());
        w.body = _i.tr("‹reassembly incomplete›", "‹Reassemblierung unvollständig›");
        w.key = "INC:" + w.raw; // matches an identical incomplete on the other side, never a real telegram
        w.hasAck = false;
        handleFrame(side, w, out);
    }

    /**
     * @brief Header-derived total telegram length (incl. FCS). STD = 8 + LG(octet5 low nibble) · EXT = 9 + LG
     *        (octet6). Matches knx cemi_frame telegramLengthtTP (std octetCount+8, ext octetCount+9). 0 = need more.
     */
    static int expectedLen(const uint8_t* a, int n)
    {
        if (n < 1) return 0;
        if (a[0] & 0x80) // standard frame (CTRL bit7 = 1)
        {
            if (n < 6) return 0;
            return 8 + (a[5] & 0x0F);
        }
        if (n < 7) return 0; // extended frame
        return 9 + a[6];
    }

    /** @brief Byte count of a space-separated hex string (2 hex digits per byte). */
    static int byteLen(const std::string& hexStr)
    {
        int digits = 0;
        for (char ch : hexStr)
            if ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F')) ++digits;
        return digits / 2;
    }

    /** @brief TP1 frame-check: last octet == complement of the XOR of all preceding octets. */
    static bool fcsOk(const uint8_t* a, int len)
    {
        if (len < 2) return false;
        uint8_t x = 0;
        for (int i = 0; i < len - 1; ++i)
            x ^= a[i];
        return (uint8_t)(x ^ 0xFF) == a[len - 1];
    }

    /** @brief Append the bytes of a space-separated hex string to @p acc. */
    static void appendHexBytes(std::vector<uint8_t>& acc, const std::string& hexStr)
    {
        int hi = -1;
        for (char ch : hexStr)
        {
            int v;
            if (ch >= '0' && ch <= '9') v = ch - '0';
            else if (ch >= 'a' && ch <= 'f')
                v = ch - 'a' + 10;
            else if (ch >= 'A' && ch <= 'F')
                v = ch - 'A' + 10;
            else
                continue;
            if (hi < 0)
                hi = v;
            else
            {
                acc.push_back((uint8_t)((hi << 4) | v));
                hi = -1;
            }
        }
    }

    /** @brief Uppercase space-separated hex of a byte buffer. */
    static std::string bytesToHex(const uint8_t* d, int n)
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

    /** @brief Flush frames whose grace window elapsed as only-X / count-mismatch. */
    void expireHolding(std::vector<std::string>& out, uint64_t now)
    {
        for (int side = 0; side < 2; ++side)
        {
            while (!_hold[side].empty() && _hold[side].front().expiry <= now)
            {
                MonFrame f = _hold[side].front().f;
                _hold[side].pop_front();
                classifyUnmatched(out, side, f);
            }
        }
    }

    /** @brief An unmatched, expired frame: mismatch if the other side ever saw this key, else only-X. */
    void classifyUnmatched(std::vector<std::string>& out, int side, const MonFrame& f)
    {
        uint32_t otherCnt = 0;
        auto it = _keyCount.find(f.key);
        if (it != _keyCount.end()) otherCnt = side == 0 ? it->second.second : it->second.first;
        if (otherCnt > 0)
        {
            _mismatch++;
            _svc[svcBucket(f)].mismatch++;
            render(out, Mismatch, side, f, false);
        }
        else
        {
            (side == 0 ? _onlyA : _onlyB)++;
            (side == 0 ? _svc[svcBucket(f)].onlyA : _svc[svcBucket(f)].onlyB)++;
            render(out, side == 0 ? OnlyA : OnlyB, side, f, false);
        }
    }

    void holdPush(int side, const MonFrame& f)
    {
        if (_hold[side].size() >= HOLD_MAX)
        {
            // window overflow: force-expire the oldest so the bound holds; log once
            MonFrame old = _hold[side].front().f;
            _hold[side].pop_front();
            std::vector<std::string> dummy;
            classifyUnmatched(dummy, side, old);
            if (!_holdWarned && _render)
            {
                _holdWarned = true;
                footerClear();
                _p.status(Tpl::Stat::Warn, _i.tr("match window capped", "Abgleichfenster begrenzt"),
                          {std::to_string(HOLD_MAX)});
                _footLines = 0;
            }
        }
        _hold[side].push_back({f, f.ms + (uint64_t)_graceMs});
    }

    void bumpKeyCount(int side, const std::string& key)
    {
        auto it = _keyCount.find(key);
        if (it == _keyCount.end())
        {
            if (_keyCount.size() >= KEYCNT_MAX)
            {
                if (!_keyCntWarned && _render)
                {
                    _keyCntWarned = true;
                    footerClear();
                    _p.status(Tpl::Stat::Warn, _i.tr("key table capped", "Schlüsseltabelle begrenzt"),
                              {std::to_string(KEYCNT_MAX)});
                    _footLines = 0;
                }
                return;
            }
            it = _keyCount.emplace(key, std::make_pair<uint32_t, uint32_t>(0, 0)).first;
        }
        if (side == 0) it->second.first++;
        else
            it->second.second++;
    }

    /*********************************************************************
     ***************************** RENDERING *****************************
     ********************************************************************/
    /** @brief Format one single-side / streamed event into @p out (per the active layout) and capture it. */
    void render(std::vector<std::string>& out, EvKind k, int side, const MonFrame& f, bool multi)
    {
        capture(k, side, f, multi);
        if (!_render || _paused) return;
        emit(out, k, side, f, false, f, edgeOf(f));
    }

    /** @brief A content-matched pair: capture the A/B metadata, render the content line + the mismatch detail. */
    void renderMatched(std::vector<std::string>& out, EvKind k, int side, const MonFrame& a, const MonFrame& b)
    {
        captureMeta(k, a, b);
        if (!_render || _paused) return;
        emit(out, k, side, a, true, b, edgeOf(a));
    }

    /**
     * @brief The DISPLAY pipeline (capture already done): collapse -> filter -> markers/skew -> layout. Never
     *        touches the counters, the XML capture, the result or the exit code — pure interactive rendering.
     */
    void emit(std::vector<std::string>& out, EvKind k, int side, const MonFrame& a, bool hasB, const MonFrame& b, bool edge)
    {
        if (_collapse)
        {
            collapseFeed(out, k, side, a, hasB, b, edge);
            return;
        }
        if (_fltDiv && k == Common) return; // filter: show only non-common frames
        renderEvent(out, k, side, a, hasB, b, edge);
    }

    /** @brief Render one (un-collapsed) event: the content line + markers/skew suffix + the metadata detail. */
    void renderEvent(std::vector<std::string>& out, EvKind k, int side, const MonFrame& a, bool hasB, const MonFrame& b, bool edge)
    {
        const uint8_t reasm = hasB ? std::max(a.reasm, b.reasm) : a.reasm; // reassembly on EITHER side is notable
        const std::string sfx = markerSuffix(a, edge, reasm) + (hasB ? skewSuffix(a, b) : std::string());
        renderFrame(out, k, side, a, sfx);
        if (hasB && k != Common) out.push_back(metaDetailLine(k, a, b));
    }

    void renderFrame(std::vector<std::string>& out, EvKind k, int side, const MonFrame& f, const std::string& sfx = "")
    {
        switch (_layout)
        {
            case Layout::Side: renderSide(out, k, side, f, sfx); break;
            case Layout::Stack: renderStack(out, k, side, f, sfx); break;
            default: renderMix(out, k, side, f, sfx); break;
        }
    }

    /*********************************************************************
     ******************* READABILITY: MARKERS · SKEW ********************
     ********************************************************************/
    /** @brief A frame within the first (or, at teardown, last) grace window — partner may fall outside capture. */
    bool edgeOf(const MonFrame& f) const
    {
        if (_tearing) return true;
        return _startMs && f.ms >= _startMs && (f.ms - _startMs) < (uint64_t)_graceMs;
    }

    /** @brief First raw octet of a bus LPDU (the CTRL field), or -1 for a non-bus / empty frame. */
    static int firstRawByte(const MonFrame& f)
    {
        if (!f.bus) return -1;
        int hi = -1;
        for (char ch : f.raw)
        {
            int v = (ch >= '0' && ch <= '9') ? ch - '0' : (ch >= 'A' && ch <= 'F') ? ch - 'A' + 10
                                                       : (ch >= 'a' && ch <= 'f')   ? ch - 'a' + 10
                                                                                    : -1;
            if (v < 0) continue;
            if (hi < 0)
                hi = v;
            else
                return (hi << 4) | v;
        }
        return -1;
    }

    /** @brief TP1 CTRL repeat bit: bit5 = 0 means the frame is a repetition (a distinct bus event). */
    static bool isRepeat(const MonFrame& f)
    {
        const int c = firstRawByte(f);
        return c >= 0 && (c & 0x20) == 0;
    }

    /** @brief Additive `(Repeat)` / `(reassembled ×N)` / `(edge)` tags — hide nothing (key m, default on). */
    std::string markerSuffix(const MonFrame& f, bool edge, uint8_t reasm)
    {
        if (!_markers) return "";
        std::string s;
        if (isRepeat(f)) s += _c.dim(_i.tr(" (Repeat)", " (Wiederh.)"));
        if (reasm > 1) s += _c.dim(std::string(" (") + _i.tr("reassembled", "zusammengesetzt") + " \xC3\x97" + std::to_string(reasm) + ")");
        if (edge) s += _c.dim(_i.tr(" (edge)", " (Rand)"));
        return s;
    }

    /** @brief The A↔B capture-time delta on a matched pair: `A@SS.mmm B@SS.mmm (+Nms)` (key t, default off). */
    std::string skewSuffix(const MonFrame& a, const MonFrame& b)
    {
        if (!_skew) return "";
        const int64_t d = (int64_t)b.ms - (int64_t)a.ms;
        char buf[48];
        std::snprintf(buf, sizeof(buf), "  A@%s B@%s (%+lldms)", shortTime(a.time).c_str(), shortTime(b.time).c_str(),
                      (long long)d);
        return _c.dim(buf);
    }

    /** @brief The SS.mmm tail of an HH:MM:SS.mmm timestamp. */
    static std::string shortTime(const std::string& t) { return t.size() >= 12 ? t.substr(6) : t; }

    /*********************************************************************
     ****************** READABILITY: COLLAPSE RUNS **********************
     ********************************************************************/
    /** @brief Display-severity of a kind (for a collapsed run's colour): common < not-reported < divergence. */
    static int severity(EvKind k) { return k == Common ? 0 : k == NotReported ? 1 : 2; }

    /** @brief Feed one event into the collapse accumulator; a different content key flushes the held run. */
    void collapseFeed(std::vector<std::string>& out, EvKind k, int side, const MonFrame& a, bool hasB, const MonFrame& b, bool edge)
    {
        if (_crun.active && _crun.key == a.raw)
        {
            addToRun(k, side, hasB);
            return;
        }
        collapseFlush(out);
        _crun = CRun{};
        _crun.active = true;
        _crun.key = a.raw;
        _crun.repSide = side;
        _crun.hasB = hasB;
        _crun.a = a;
        _crun.b = b;
        _crun.edge = edge;
        addToRun(k, side, hasB);
    }

    void addToRun(EvKind k, int side, bool hasB)
    {
        _crun.events++;
        if (hasB)
        {
            _crun.aCount++;
            _crun.bCount++;
        }
        else if (k == OnlyB)
            _crun.bCount++;
        else
            (side == 0 ? _crun.aCount : _crun.bCount)++;
        if (severity(k) > severity(_crun.kind)) _crun.kind = k;
        _crun.lastMs = detail::nowMs();
    }

    /** @brief A settled run (idle > NORM_STALL_MS) is flushed so it appears promptly, not only at the next key. */
    void collapseTick(std::vector<std::string>& out, uint64_t now)
    {
        if (_crun.active && (now - _crun.lastMs) > NORM_STALL_MS) collapseFlush(out);
    }

    /** @brief Emit the held collapse run (filtered like a normal event); a 1-event run renders in full. */
    void collapseFlush(std::vector<std::string>& out)
    {
        if (!_crun.active) return;
        _crun.active = false;
        if (_fltDiv && _crun.kind == Common) return;
        if (_crun.events <= 1)
        {
            renderEvent(out, _crun.kind, _crun.repSide, _crun.a, _crun.hasB, _crun.b, _crun.edge);
            return;
        }
        const std::string stripe = isDiv(_crun.kind) ? _c.red(_t.glyph("▎", "|")) : std::string(" ");
        const std::string cnt = _c.dim(std::string("  ") + _t.glyph("↻", "x") + " A\xC3\x97" +
                                       std::to_string(_crun.aCount) + " \xC2\xB7 B\xC3\x97" + std::to_string(_crun.bCount));
        const uint8_t reasm = _crun.hasB ? std::max(_crun.a.reasm, _crun.b.reasm) : _crun.a.reasm;
        std::string line = "  " + stripe + " " + srcTag(_crun.kind, _crun.repSide) + "  " +
                           _c.dim(_i.tr("(collapsed)", "(gebündelt)")) + "  " + tint(_crun.kind, _crun.a.body) + cnt +
                           markerSuffix(_crun.a, _crun.edge, reasm);
        out.push_back(line);
    }

    /*********************************************************************
     **************** READABILITY: SERVICE BREAKDOWN *******************
     ********************************************************************/
    /** @brief A coarse service/telegram-type bucket for the divergence breakdown (from the decoded body). */
    std::string svcBucket(const MonFrame& f)
    {
        const std::string& b = f.body;
        auto has = [&](const char* s) { return b.find(s) != std::string::npos; };
        std::string t;
        if (has("FTC")) t = "FTC";
        else if (has("T_Disconnect"))
            t = "T_Disconnect";
        else if (has("T_Connect"))
            t = "T_Connect";
        else if (has("T_ACK") || has("T_NAK") || has("T_Ctrl"))
            t = "T_Ack";
        else if (has("Write"))
            t = "GroupWrite";
        else if (has("Read"))
            t = "GroupRead";
        else if (has("Resp"))
            t = "GroupResp";
        else if (has("incomplete") || has("unvollst"))
            t = "incomplete";
        else
            t = "other";
        if (f.bus && isRepeat(f)) t += "(repeat)";
        return t;
    }

    /** @brief One dim detail line under a matched-but-divergent pair: what metadata differs (A vs B). */
    std::string metaDetailLine(EvKind k, const MonFrame& a, const MonFrame& b)
    {
        std::string s = "        ";
        if (k == IntegrityMismatch)
            s += _c.red(_i.tr("integrity", "Integrität")) + _c.dim("  A " + fbpStr(a) + "  ·  B " + fbpStr(b));
        else if (k == AckMismatch)
            s += _c.red(_i.tr("ack", "Quittung")) +
                 _c.dim(std::string("  A ") + ackStr(a.ackKind) + "  ·  B " + ackStr(b.ackKind));
        else // NotReported
        {
            const bool integ = (a.stat != b.stat);
            const char* dim = integ ? _i.tr("integrity", "Integrität") : _i.tr("ack", "Quittung");
            const std::string aRep = integ ? (a.stat ? "ok" : _i.tr("not reported", "nicht gemeldet"))
                                           : (_ackSeen[0] ? "ok" : _i.tr("not reported", "nicht gemeldet"));
            const std::string bRep = integ ? (b.stat ? "ok" : _i.tr("not reported", "nicht gemeldet"))
                                           : (_ackSeen[1] ? "ok" : _i.tr("not reported", "nicht gemeldet"));
            s += _p.paint('o', _i.tr("not comparable", "nicht vergleichbar")) +
                 _c.dim(std::string("  ") + dim + "  A " + aRep + "  ·  B " + bRep);
        }
        return s;
    }

    /** @brief The 0x03 F/B/P integrity as `F0 B0 P0` (or `n/r` when the side did not report the status). */
    std::string fbpStr(const MonFrame& f)
    {
        if (!f.stat) return _i.tr("n/r", "n.g.");
        char b[24];
        std::snprintf(b, sizeof(b), "F%d B%d P%d", f.ferr ? 1 : 0, f.berr ? 1 : 0, f.perr ? 1 : 0);
        return b;
    }

    /** @brief The L2 acknowledge kind as a stable protocol token (verbatim in both languages). */
    static const char* ackStr(uint8_t kind)
    {
        return kind == 1 ? "ACK" : kind == 2 ? "NAK"
                               : kind == 3   ? "BUSY"
                                             : "none";
    }

    /** @brief The 0x03 F/B/P integrity for an XML attribute: `F0B0P0` or `notReported`. */
    static std::string statAttr(const MonFrame& f)
    {
        if (!f.stat) return "notReported";
        char b[16];
        std::snprintf(b, sizeof(b), "F%dB%dP%d", f.ferr ? 1 : 0, f.berr ? 1 : 0, f.perr ? 1 : 0);
        return b;
    }

    void renderMix(std::vector<std::string>& out, EvKind k, int side, const MonFrame& f, const std::string& sfx = "")
    {
        const std::string stripe = isDiv(k) ? _c.red(_t.glyph("▎", "|")) : std::string(" ");
        std::string line = "  " + stripe + " " + srcTag(k, side) + "  " + _c.dim(f.time) + "  " + fmtBadge(f);
        if (f.hasAck) line += "  " + ackTag(f);
        line += "  " + tint(k, f.body) + sfx;
        out.push_back(line);
        appendRaw(out, k, f);
    }

    void renderStack(std::vector<std::string>& out, EvKind k, int side, const MonFrame& f, const std::string& sfx = "")
    {
        const std::string lane = laneTag(k, side);
        std::string line = "  " + lane + "  " + _c.dim(f.time) + "  " + fmtBadge(f);
        if (f.hasAck) line += "  " + ackTag(f);
        line += "  " + tint(k, f.body) + sfx;
        out.push_back(line);
        if (isDiv(k))
            out.push_back("     " + _c.red(_t.glyph("‼ ", "!! ")) + _c.dim(_i.tr("divergence", "Abweichung")));
        appendRaw(out, k, f);
    }

    void renderSide(std::vector<std::string>& out, EvKind k, int side, const MonFrame& f, const std::string& sfx = "")
    {
        int half = (Tpl::cols() - 9) / 2;
        if (half < 12) half = 12;
        const std::string ph = _c.mut(_t.glyph("·", "."));
        const std::string cell = _p.clip(sideCell(k, f) + sfx, half);
        std::string L = ph, R = ph;
        const bool aSide = bothSides(k) || (k == OnlyA) || (k == Mismatch && side == 0);
        const bool bSide = bothSides(k) || (k == OnlyB) || (k == Mismatch && side == 1);
        if (aSide) L = cell;
        if (bSide) R = cell;
        const std::string sep = isDiv(k) ? _c.red(_t.glyph("│", "|")) : _c.dim(_t.glyph("│", "|"));
        std::string lpad = L;
        for (int i = Tpl::vis(L); i < half; ++i)
            lpad += ' ';
        out.push_back("  " + lpad + " " + sep + " " + R);
    }

    /** @brief The line body shared by side cells: time · badge · ack · body, tinted by kind. */
    std::string sideCell(EvKind k, const MonFrame& f)
    {
        std::string s = _c.dim(f.time) + " " + fmtBadge(f);
        if (f.hasAck) s += " " + ackTag(f);
        s += " " + tint(k, f.body);
        return s;
    }

    /** @brief Append the raw LPDU/APDU hex as wrapped follow-up line(s). */
    void appendRaw(std::vector<std::string>& out, EvKind k, const MonFrame& f)
    {
        if (f.raw.empty()) return;
        const int width = Tpl::cols() - 8;
        if (width <= 8 || (int)f.raw.size() <= width)
        {
            out.push_back("      " + (isDiv(k) ? _c.dim(f.raw) : _c.mut(f.raw)));
            return;
        }
        // reflow long extended frames on spaces, hanging indent 6
        std::string cur;
        int col = 0;
        size_t i = 0;
        while (i < f.raw.size())
        {
            size_t j = f.raw.find(' ', i);
            const std::string w = f.raw.substr(i, j == std::string::npos ? std::string::npos : j - i);
            if (!cur.empty() && col + 1 + (int)w.size() > width)
            {
                out.push_back("      " + _c.mut(cur));
                cur.clear();
                col = 0;
            }
            if (!cur.empty())
            {
                cur += ' ';
                col++;
            }
            cur += w;
            col += (int)w.size();
            if (j == std::string::npos) break;
            i = j + 1;
        }
        if (!cur.empty()) out.push_back("      " + _c.mut(cur));
    }

    /*********************************************************************
     ****************************** COLOURS ******************************
     ********************************************************************/
    static bool isDiv(EvKind k) { return k != Common; }
    /** @brief Kinds where BOTH sides saw the content (content-matched pair) — metadata is what may differ. */
    static bool bothSides(EvKind k) { return k == Common || k == IntegrityMismatch || k == AckMismatch || k == NotReported; }
    /** @brief The line hue for a kind: content-diff/integrity/ack = red · not-reported = gold · common = dim. */
    static char kindHue(EvKind k)
    {
        return k == NotReported ? 'o' : (k == OnlyA ? 'g' : k == OnlyB ? 'a'
                                                                       : 'r');
    }

    /** @brief Tint a whole line by kind: common dim · A green · B amber · not-reported gold · mismatch red. */
    std::string tint(EvKind k, const std::string& s)
    {
        if (k == Common) return _c.dim(s);
        return _p.paint(kindHue(k), s);
    }

    /** @brief The `A·B` / `A· ` / ` ·B` source tag (dim for common; both-sides-hued for a metadata mismatch). */
    std::string srcTag(EvKind k, int side)
    {
        const std::string dot = _c.dim(_t.glyph("·", "."));
        if (k == Common) return _c.dim("A") + dot + _c.dim("B");
        if (bothSides(k)) // both saw the content; the metadata differs -> both letters in the kind hue
            return _p.paint(kindHue(k), "A") + dot + _p.paint(kindHue(k), "B");
        const bool a = (k == OnlyA) || (k == Mismatch && side == 0);
        return (a ? _c.green("A") : _c.mut(" ")) + dot + (a ? _c.mut(" ") : _c.amber("B"));
    }

    std::string laneTag(EvKind k, int side)
    {
        if (k == Common) return _c.dim("AB");
        if (bothSides(k)) return _p.paint(kindHue(k), "AB");
        const bool a = (k == OnlyA) || (k == Mismatch && side == 0);
        return a ? _c.green("A ") : _c.amber(" B");
    }

    std::string fmtBadge(const MonFrame& f) { return f.ext ? _p.chip("EXT", 'o') : _p.chip("STD", 'c'); }
    std::string ackTag(const MonFrame& f)
    {
        return f.ackKind == 1 ? _c.dim("ACK") : f.ackKind == 2 ? _c.amber("NAK")
                                            : f.ackKind == 3   ? _c.amber("BUSY")
                                            : f.hasAck         ? _c.mut(_t.glyph("—", "-"))
                                                               : std::string();
    }

    /*********************************************************************
     ***************************** CAPTURE *******************************
     ********************************************************************/
    void capture(EvKind k, int side, const MonFrame& f, bool multi)
    {
        if (_ann.size() >= ANN_MAX)
        {
            if (!_annWarned && _render)
            {
                _annWarned = true;
                footerClear();
                _p.status(Tpl::Stat::Warn, _i.tr("capture buffer full", "Aufzeichnung voll"), {std::to_string(ANN_MAX)});
                _footLines = 0;
            }
            return;
        }
        Ann a;
        a.f = f;
        if (multi)
        {
            a.seenBy = side == 0 ? "A" : "B";
            a.diff = "stream";
        }
        else
        {
            switch (k)
            {
                case OnlyA: a.seenBy = "A"; a.diff = "onlyA"; break;
                case OnlyB: a.seenBy = "B"; a.diff = "onlyB"; break;
                default: // content count-mismatch
                    a.seenBy = side == 0 ? "A" : "B";
                    a.diff = "mismatch";
                    auto it = _keyCount.find(f.key);
                    if (it != _keyCount.end())
                    {
                        a.countA = it->second.first;
                        a.countB = it->second.second;
                    }
                    break;
            }
        }
        _ann.push_back(a);
    }

    /** @brief Capture a content-matched pair with its full A/B metadata (for the per-telegram XML). */
    void captureMeta(EvKind k, const MonFrame& a, const MonFrame& b)
    {
        if (_ann.size() >= ANN_MAX)
        {
            if (!_annWarned && _render)
            {
                _annWarned = true;
                footerClear();
                _p.status(Tpl::Stat::Warn, _i.tr("capture buffer full", "Aufzeichnung voll"), {std::to_string(ANN_MAX)});
                _footLines = 0;
            }
            return;
        }
        Ann an;
        an.f = a;
        an.fb = b;
        an.meta = true;
        an.seenBy = "AB";
        an.diff = k == Common ? "common" : k == IntegrityMismatch ? "integrity"
                                       : k == AckMismatch         ? "ack"
                                                                  : "notReported";
        _ann.push_back(an);
    }

    /*********************************************************************
     ****************************** FOOTER *******************************
     ********************************************************************/
    void footerClear()
    {
        if (_footLines > 0 && _t.isTty())
        {
            std::printf("\x1b[%dA\x1b[J", _footLines);
            _footLines = 0;
        }
    }

    /** @brief Total fidelity divergences: content (only-A/B + count) PLUS the metadata classes + not-comparable. */
    uint64_t divergences() const
    {
        return _onlyA + _onlyB + _mismatch + _integMismatch + _ackMismatch + _notReported;
    }

    static constexpr uint64_t REASM_HINT_MS = 200; // anti-flicker: only cue a reassembly pending this long

    /** @brief A side is actively buffering fragment pieces (whole not yet completed) for longer than the guard. */
    bool reasmPending(int side) const
    {
        const NormAcc& na = _norm[side];
        return _normalize && !na.acc.empty() && na.startMs && (detail::nowMs() - na.startMs) > REASM_HINT_MS;
    }

    /**
     * @brief Live footer cue: a side mid-reassembly shows `⟳ reassembling… (N pieces)` so a lagging column is
     *        NOT mistaken for a dropout. Pure display — never a frame, never counted/captured/exported.
     */
    std::string reasmStatusLine()
    {
        const bool a = reasmPending(0), b = reasmPending(1);
        if (!a && !b) return "";
        const std::string tag = std::string(_t.glyph("⟳ ", "~ ")) + _i.tr("reassembling…", "reassembliert…");
        auto cell = [&](int s) {
            return _c.cyan(sideName(s) + " " + tag + " (" + std::to_string(_norm[s].accPieces) + " " +
                          _i.tr("pieces", "Stücke") + ")");
        };
        if (_layout == Layout::Side) // align under the two columns
        {
            int half = (Tpl::cols() - 9) / 2;
            if (half < 12) half = 12;
            std::string L = a ? _p.clip(cell(0), half) : "";
            std::string R = b ? _p.clip(cell(1), half) : "";
            for (int i = Tpl::vis(L); i < half; ++i)
                L += ' ';
            return "  " + L + " " + _c.dim(_t.glyph("│", "|")) + " " + R;
        }
        std::string s = "  ";
        if (a) s += cell(0);
        if (a && b) s += _c.dim("   ");
        if (b) s += cell(1);
        return s;
    }

    void drawFooter(Monitor::Mode mode)
    {
        const uint64_t div = divergences();
        const std::string chip = _diffOn ? (div == 0 ? _c.chip(_i.tr("IDENTICAL", "Identisch"), 'g')
                                                      : _c.chip(std::string(_i.tr("DIVERGENT ", "Unterschiede ")) + _t.glyph("· ", "* ") + std::to_string(div), 'r'))
                                         : _c.chip(_i.tr("MULTI", "Mehrfach"), 'c');
        std::string segs = _c.dim(mode == Monitor::Mode::Bus ? "busmon" : "groupmon") + _c.dim("  ") +
                           _c.dim(_i.tr("grace ", "Toleranz ") + std::to_string(_graceMs) + "ms") + _c.dim("  ") +
                           _c.dim(_i.tr("layout ", "Layout ") + layoutName());
        if (mode == Monitor::Mode::Bus) // normalize only bears on busmon (raw LPDU fragmentation)
            segs += _c.dim("  ") + (_normalize ? _c.green(_i.tr("norm", "norm")) : _c.dim(_i.tr("raw", "roh")));
        const uint64_t inc = _normIncomplete[0] + _normIncomplete[1];
        if (inc) segs += "  " + _c.amber(_i.tr("incomplete ", "unvollst. ") + std::to_string(inc));
        const uint64_t lost = _lost[0] + _lost[1];
        if (lost) segs += "  " + _c.amber(_i.tr("lost ", "verloren ") + std::to_string(lost));
        if (_fltDiv) segs += _c.dim("  ") + _c.cyan(_i.tr("filter", "Filter"));
        if (_collapse) segs += _c.dim("  ") + _c.cyan(_i.tr("collapse", "bündeln"));
        if (_skew) segs += _c.dim("  ") + _c.cyan(_i.tr("skew", "Versatz"));
        if (!_markers) segs += _c.dim("  ") + _c.dim(_i.tr("no-markers", "ohne Marker"));
        if (_paused) segs += "  " + _c.amber(_i.tr("PAUSED", "PAUSE"));

        // Build the footer lines as strings so their REAL wrapped height can be measured (a narrow terminal wraps
        // counters/keybar to >1 row each; hardcoding a count would leave stale rows on the next footerClear()).
        const std::string rLine = reasmStatusLine(); // transient — a side mid-reassembly (may be empty)
        const std::string cLine = buildCounters();
        const std::string vLine = "  " + chip + "   " + segs;
        const std::string kLine = buildKeybar();
        int cols = Tpl::cols();
        if (cols < 1) cols = 80;
        _footLines = wrapRows(Tpl::vis(cLine), cols) + wrapRows(Tpl::vis(vLine), cols) + wrapRows(Tpl::vis(kLine), cols);
        if (!rLine.empty())
        {
            std::printf("%s\n", rLine.c_str());
            _footLines += wrapRows(Tpl::vis(rLine), cols);
        }
        std::printf("%s\n%s\n%s\n", cLine.c_str(), vLine.c_str(), kLine.c_str());
    }

    /** @brief Physical terminal rows a line of @p vis visible columns occupies at width @p cols. */
    static int wrapRows(int vis, int cols)
    {
        if (vis <= 0 || cols <= 0) return 1;
        return (vis + cols - 1) / cols;
    }

    /** @brief The counters tile row as a string (mirrors Tpl::counters, so it can be measured for wrap). */
    std::string buildCounters()
    {
        struct C
        {
            std::string v;
            const char* cap;
            char col;
        };
        const std::vector<C> cells = {
            {std::to_string(_seenA), _i.tr("A seen", "A ges."), 'g'},
            {std::to_string(_seenB), _i.tr("B seen", "B ges."), 'a'},
            {std::to_string(_common), _i.tr("common", "gemeinsam"), 'd'},
            {std::to_string(_onlyA), _i.tr("only A", "nur A"), _onlyA ? 'r' : 'd'},
            {std::to_string(_onlyB), _i.tr("only B", "nur B"), _onlyB ? 'r' : 'd'},
            {std::to_string(_mismatch), _i.tr("count≠", "Anzahl≠"), _mismatch ? 'r' : 'd'},
            {std::to_string(_integMismatch), _i.tr("integ≠", "Integr≠"), _integMismatch ? 'r' : 'd'},
            {std::to_string(_ackMismatch), _i.tr("ack≠", "Quitt≠"), _ackMismatch ? 'r' : 'd'},
            {std::to_string(_notReported), _i.tr("n/r", "n.g."), _notReported ? 'o' : 'd'},
            {std::to_string(_lost[0] + _lost[1]), _i.tr("lost", "verlor."), (_lost[0] + _lost[1]) ? 'o' : 'd'}};
        std::string s = "  ";
        for (size_t i = 0; i < cells.size(); ++i)
        {
            if (i) s += "     ";
            s += (cells[i].col == 'd' ? _c.dim(cells[i].v) : _c.bold(_p.paint(cells[i].col, cells[i].v))) + " " +
                 _c.dim(cells[i].cap);
        }
        return s;
    }

    /** @brief The keybar row as a string (mirrors Tpl::keybar, so it can be measured for wrap). */
    std::string buildKeybar()
    {
        const std::pair<const char*, std::string> keys[] = {
            {"v", _i.tr("layout", "Layout")},
            {"d", _diffOn ? _i.tr("→multi", "→Multi") : _i.tr("→diff", "→Diff")},
            {"n", _normalize ? _i.tr("→raw", "→roh") : _i.tr("→norm", "→norm")},
            {"f", _fltDiv ? _i.tr("→all", "→alle") : _i.tr("→diffs", "→Abw.")},
            {"c", _collapse ? _i.tr("uncollapse", "entbünd.") : _i.tr("collapse", "bündeln")},
            {"m", _markers ? _i.tr("markers-", "Marker-") : _i.tr("markers+", "Marker+")},
            {"t", _skew ? _i.tr("skew-", "Versatz-") : _i.tr("skew+", "Versatz+")},
            {"l", _i.tr("save", "sichern")},
            {"p", _i.tr("pause", "Pause")},
            {"r", _i.tr("reconnect", "neu")},
            {"?", _i.tr("help", "Hilfe")},
            {"x", _i.tr("quit", "beenden")}};
        std::string s = "  ";
        bool first = true;
        for (const auto& kv : keys)
        {
            if (!first) s += "   ";
            first = false;
            s += _c.bold(_c.cyan(kv.first)) + " " + _c.dim(kv.second);
        }
        return s;
    }

    std::string layoutName() const
    {
        return _layout == Layout::Mix ? "mix" : _layout == Layout::Side ? "side"
                                                                        : "stack";
    }

    /*********************************************************************
     ****************************** KEYS *********************************
     ********************************************************************/
    /** @brief Poll + act on the hot-keys. Returns true only when a forced reconnect gave up. */
    bool handleKeys(Keys& keys, Monitor::Mode mode, const volatile std::sig_atomic_t* abort, uint64_t start,
                    int maxSeconds, bool& userStop)
    {
        (void)start;
        (void)maxSeconds;
        for (char k; (k = keys.poll()) != 0;)
        {
            switch (k)
            {
                case 'x':
                case 'q':
                    userStop = true;
                    return false;
                case 'v':
                    _layout = _layout == Layout::Mix ? Layout::Side : _layout == Layout::Side ? Layout::Stack
                                                                                              : Layout::Mix;
                    forceRedraw();
                    break;
                case 'd':
                    _diffOn = !_diffOn;
                    forceRedraw();
                    break;
                case 'n':
                {
                    std::vector<std::string> flushed;
                    normFlushAll(flushed); // strand nothing across the mode switch
                    _normalize = !_normalize;
                    if (_render && !flushed.empty())
                    {
                        footerClear();
                        for (const auto& l : flushed)
                            std::printf("%s\n", l.c_str());
                        _footLines = 0;
                    }
                    forceRedraw();
                    break;
                }
                case 'f':
                    _fltDiv = !_fltDiv;
                    forceRedraw();
                    break;
                case 'c':
                {
                    std::vector<std::string> flushed;
                    collapseFlush(flushed); // emit the pending run before switching the aggregation mode
                    _collapse = !_collapse;
                    if (_render && !flushed.empty())
                    {
                        footerClear();
                        for (const auto& l : flushed)
                            std::printf("%s\n", l.c_str());
                        _footLines = 0;
                    }
                    forceRedraw();
                    break;
                }
                case 'm':
                    _markers = !_markers;
                    forceRedraw();
                    break;
                case 't':
                    _skew = !_skew;
                    forceRedraw();
                    break;
                case 'p':
                    _paused = !_paused;
                    forceRedraw();
                    break;
                case 'l':
                    saveNote(saveXml(mode));
                    break;
                case 'r':
                    footerClear();
                    _p.status(Tpl::Stat::Warn, _i.tr("reconnecting both", "beide neu verbinden"));
                    _footLines = 0;
                    reopenBoth(mode, abort);
                    break;
                case '?':
                    footerClear();
                    _p.note(_i.tr("v layout · d diff/multi · n normalize(reassemble)/raw · l save XML · p pause · r reconnect · x/q quit",
                                  "v Layout · d Diff/Multi · n Normalisieren(zusammensetzen)/roh · l XML · p Pause · r neu · x/q beenden"));
                    _p.note(_i.tr("readability: f only-divergences · c collapse identical · m markers (Repeat/reassembled/edge) · t time-skew",
                                  "Lesbarkeit: f nur Abweichungen · c Gleiche bündeln · m Marker (Wiederh./zusammengesetzt/Rand) · t Zeitversatz"));
                    _p.note(_i.tr("fidelity classes: content only-A/B + count≠ · integ≠ (F/B/P) · ack≠ (ACK/NAK/BUSY) · n/r not-comparable · lost",
                                  "Treue-Klassen: Inhalt nur-A/B + Anzahl≠ · Integr≠ (F/B/P) · Quitt≠ (ACK/NAK/BUSY) · n.g. nicht vergleichbar · verloren"));
                    _footLines = 0;
                    break;
                default: break;
            }
        }
        return false;
    }

    void forceRedraw() { _lastDraw = 0; }

    void reopenBoth(Monitor::Mode mode, const volatile std::sig_atomic_t* abort)
    {
        (void)mode;
        (void)abort;
        // Mark both sides down + drop their sockets; the cooperative serviceSide() reconnects them without blocking.
        for (int s = 0; s < 2; ++s)
        {
            (s == 0 ? _A : _B).closeTunnel();
            _down[s] = true;
            _connecting[s] = false;
            _retryAt[s] = detail::nowMs();
            _backoff[s] = 500;
        }
        _footLines = 0;
    }

    void saveNote(const std::string& path)
    {
        footerClear();
        if (path.empty())
            _p.status(Tpl::Stat::Err, _i.tr("could not write XML", "XML nicht schreibbar"));
        else
            _p.status(Tpl::Stat::Ok, _i.tr("saved run", "Lauf gesichert"),
                      {path, std::to_string(_ann.size()) + _i.tr(" frames", " Frames")});
        _footLines = 0;
    }

    /*********************************************************************
     *************************** XML EXPORT ******************************
     ********************************************************************/
    std::string saveXml(Monitor::Mode mode)
    {
        const char* mstr = mode == Monitor::Mode::Bus ? "bm" : "gm";
        const char* kind = _diffOn ? "compare" : "multi";
        const uint64_t div = divergences();
        const char* result = !_diffOn ? "MULTI" : (div == 0 ? "IDENTICAL" : "DIVERGENT");
        const std::string fname = std::string("ftc-") + mstr + "-" + kind + "_" + Monitor::safeIp(_ipA) + "_" +
                                  Monitor::safeIp(_ipB) + "_" + Monitor::stamp() + ".xml";
        const std::string path = (std::filesystem::path(Monitor::exportDir()) / fname).string();
        std::FILE* fp = std::fopen(path.c_str(), "wb");
        if (!fp) return "";
        std::fprintf(fp, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
        std::fprintf(fp, "<compare mode=\"%s\" kind=\"%s\" grace=\"%d\" normalize=\"%s\" result=\"%s\" divergences=\"%llu\"\n",
                     mstr, kind, _graceMs, (mode == Monitor::Mode::Bus && _normalize) ? "on" : "off", result,
                     (unsigned long long)div);
        std::fprintf(fp, "         ifaceA=\"%s\" paA=\"%s\" ifaceB=\"%s\" paB=\"%s\"\n",
                     Monitor::xmlEsc(_ipA).c_str(), Tpl::pa(_A.assignedPA()).c_str(),
                     Monitor::xmlEsc(_ipB).c_str(), Tpl::pa(_B.assignedPA()).c_str());
        // per-side capability discovery: does each side deliver the 0x03 status add-info and the L2 acknowledge?
        std::fprintf(fp, "         statusA=\"%s\" statusB=\"%s\" ackA=\"%s\" ackB=\"%s\">\n",
                     _statSeen[0] ? "yes" : "no", _statSeen[1] ? "yes" : "no",
                     _ackSeen[0] ? "yes" : "no", _ackSeen[1] ? "yes" : "no");
        std::fprintf(fp, "  <counters aSeen=\"%llu\" bSeen=\"%llu\" common=\"%llu\" onlyA=\"%llu\" onlyB=\"%llu\" countMismatch=\"%llu\""
                         " integrityMismatch=\"%llu\" ackMismatch=\"%llu\" notReported=\"%llu\" lostA=\"%llu\" lostB=\"%llu\""
                         " incompleteA=\"%llu\" incompleteB=\"%llu\"/>\n",
                     (unsigned long long)_seenA, (unsigned long long)_seenB, (unsigned long long)_common,
                     (unsigned long long)_onlyA, (unsigned long long)_onlyB, (unsigned long long)_mismatch,
                     (unsigned long long)_integMismatch, (unsigned long long)_ackMismatch, (unsigned long long)_notReported,
                     (unsigned long long)_lost[0], (unsigned long long)_lost[1],
                     (unsigned long long)_normIncomplete[0], (unsigned long long)_normIncomplete[1]);
        for (const auto& a : _ann)
        {
            std::string extra = std::string(" seenBy=\"") + a.seenBy + "\" diff=\"" + a.diff + "\"";
            if (std::string(a.diff) == "mismatch")
            {
                char cb[48];
                std::snprintf(cb, sizeof(cb), " countA=\"%u\" countB=\"%u\"", a.countA, a.countB);
                extra += cb;
            }
            if (a.meta) // a content-matched pair -> record each side's integrity + ack for the fidelity audit
                extra += std::string(" statusA=\"") + statAttr(a.f) + "\" ackA=\"" + ackStr(a.f.ackKind) +
                         "\" statusB=\"" + statAttr(a.fb) + "\" ackB=\"" + ackStr(a.fb.ackKind) + "\"";
            std::fprintf(fp, "%s\n", Monitor::telegramXml(a.f, extra).c_str());
        }
        std::fprintf(fp, "</compare>\n");
        std::fclose(fp);
        return path;
    }

    /*********************************************************************
     ****************************** CHROME *******************************
     ********************************************************************/
    void banner(Monitor::Mode mode)
    {
        const std::string what = std::string(mode == Monitor::Mode::Bus ? "busmon" : "groupmon") + " " +
                                 (_diffOn ? _i.tr("compare", "Vergleich") : _i.tr("multi", "Multi"));
        _p.section(what, -1);
        const std::string bState = _down[1] ? _i.tr("connecting…", "verbinde…") : "IA " + Tpl::pa(_B.assignedPA());
        _p.status(_down[1] ? Tpl::Stat::Warn : Tpl::Stat::Ok, _i.tr("tunnels", "Tunnel"),
                  {"A " + _ipA + " · IA " + Tpl::pa(_A.assignedPA()), "B " + _ipB + " · " + bState,
                   _i.tr("Ctrl+C to stop", "Ctrl+C zum Beenden")});
        if (_diffOn)
            _p.note(_i.tr("green = only A · amber = only B · dim = common · red = divergence",
                          "grün = nur A · gelb = nur B · grau = gleich · rot = Abweichung"));
        else
            _p.note(_i.tr("multi: two interfaces, no diff — green = A · amber = B",
                          "Multi: zwei Interfaces, kein Diff — grün = A · gelb = B"));
        if (mode == Monitor::Mode::Bus)
            _p.note(_normalize
                        ? _i.tr("normalize ON (n): fragments reassembled to whole telegrams (FCS-anchored)",
                                "Normalisieren AN (n): Fragmente zu ganzen Telegrammen zusammengesetzt (FCS-verankert)")
                        : _i.tr("normalize OFF (n): raw per-piece frames", "Normalisieren AUS (n): rohe Einzelstücke"));
    }

    int finish(Monitor::Mode mode, bool aborted, bool userStop)
    {
        const uint64_t div = divergences();
        if (_quiet || !_render)
        {
            // scripted: one summary line to stdout, no chrome (all fidelity dimensions + per-side capability)
            const char* result = !_diffOn ? "MULTI" : (div == 0 ? "IDENTICAL" : "DIVERGENT");
            std::printf("%s\tseenA=%llu\tseenB=%llu\tcommon=%llu\tonlyA=%llu\tonlyB=%llu\tcountMismatch=%llu"
                        "\tintegrityMismatch=%llu\tackMismatch=%llu\tnotReported=%llu\tlostA=%llu\tlostB=%llu"
                        "\tstatusA=%s\tstatusB=%s\tackA=%s\tackB=%s\tnormalize=%s\tincompleteA=%llu\tincompleteB=%llu\n",
                        result, (unsigned long long)_seenA, (unsigned long long)_seenB, (unsigned long long)_common,
                        (unsigned long long)_onlyA, (unsigned long long)_onlyB, (unsigned long long)_mismatch,
                        (unsigned long long)_integMismatch, (unsigned long long)_ackMismatch, (unsigned long long)_notReported,
                        (unsigned long long)_lost[0], (unsigned long long)_lost[1],
                        _statSeen[0] ? "yes" : "no", _statSeen[1] ? "yes" : "no",
                        _ackSeen[0] ? "yes" : "no", _ackSeen[1] ? "yes" : "no",
                        (mode == Monitor::Mode::Bus && _normalize) ? "on" : "off",
                        (unsigned long long)_normIncomplete[0], (unsigned long long)_normIncomplete[1]);
            std::fflush(stdout);
        }
        else
        {
            drawFinalResult(mode, aborted, userStop);
        }
        if (aborted) return 130;
        if (_diffOn && div > 0) return 2;
        return 0;
    }

    void drawFinalResult(Monitor::Mode mode, bool aborted, bool userStop)
    {
        (void)userStop;
        const uint64_t div = divergences();
        std::vector<std::string> tail;
        collapseFlush(tail); // flush any pending collapse run into the permanent output
        for (const auto& l : tail)
            std::printf("%s\n", l.c_str());
        drawFooter(mode);      // final counters + result + keybar snapshot (permanent — not erased)
        _footLines = 0;        // leave it on screen
        if (_diffOn && div > 0) breakdown();
        reliabilityHint();
        if (aborted)
            _p.status(Tpl::Stat::Warn, _i.tr("Ctrl+C — disconnecting cleanly", "Ctrl+C — sauber abgemeldet"));
        else if (_diffOn && div == 0)
            _p.status(Tpl::Stat::Ok, _i.tr("streams identical", "Ströme identisch"));
        else if (_diffOn)
            _p.status(Tpl::Stat::Err, _i.tr("streams diverge", "Ströme weichen ab"), {std::to_string(div)});
    }

    /** @brief Per-service breakdown of each divergence class: `onlyA 758 = FTC 700 · T_Disconnect(repeat) 40 …`. */
    void breakdown()
    {
        breakdownLine(_i.tr("only A", "nur A"), _onlyA, [](const Svc& s) { return s.onlyA; });
        breakdownLine(_i.tr("only B", "nur B"), _onlyB, [](const Svc& s) { return s.onlyB; });
        breakdownLine(_i.tr("count≠", "Anzahl≠"), _mismatch, [](const Svc& s) { return s.mismatch; });
        breakdownLine(_i.tr("integ≠", "Integr≠"), _integMismatch, [](const Svc& s) { return s.integ; });
        breakdownLine(_i.tr("ack≠", "Quitt≠"), _ackMismatch, [](const Svc& s) { return s.ack; });
        breakdownLine(_i.tr("n/r", "n.g."), _notReported, [](const Svc& s) { return s.notrep; });
    }

    template <typename Sel>
    void breakdownLine(const char* label, uint64_t total, Sel sel)
    {
        if (total == 0) return;
        std::vector<std::pair<std::string, uint64_t>> parts;
        for (const auto& kv : _svc)
            if (sel(kv.second) > 0) parts.push_back({kv.first, sel(kv.second)});
        std::sort(parts.begin(), parts.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        std::string line = "    " + _c.dim(std::string(label) + " " + std::to_string(total) + " = ");
        const size_t cap = 5;
        for (size_t i = 0; i < parts.size() && i < cap; ++i)
        {
            if (i) line += _c.mut(" · ");
            line += _c.txt(parts[i].first) + " " + _c.dim(std::to_string(parts[i].second));
        }
        if (parts.size() > cap)
            line += _c.mut(" · ") + _c.dim("+" + std::to_string(parts.size() - cap) + _i.tr(" more", " weitere"));
        std::printf("%s\n", line.c_str());
    }

    /** @brief One concise "result may be affected by …" line when the capture was fragile (never under -q). */
    void reliabilityHint()
    {
        std::vector<std::string> why;
        if (_reconnected) why.push_back(_i.tr("a reconnect during the run", "Reconnect während des Laufs"));
        const uint64_t durMs = detail::nowMs() - _startMs;
        const uint64_t seen = _seenA + _seenB;
        if (durMs < 3000 || seen < 20)
            why.push_back(_i.tr("a short capture", "kurze Aufzeichnung"));
        const uint64_t inc = _normIncomplete[0] + _normIncomplete[1];
        if (seen > 0 && inc * 10 > seen) // > 10% incomplete
            why.push_back(_i.tr("many incomplete reassemblies", "viele unvollständige Reassemblierungen"));
        if (why.empty()) return;
        std::string s;
        for (size_t i = 0; i < why.size(); ++i)
            s += (i ? _i.tr(" · ", " · ") : std::string()) + why[i];
        _p.status(Tpl::Stat::Warn, _i.tr("result may be affected by:", "Ergebnis evtl. beeinflusst durch:"), {s});
    }

    void fail(const std::string& ip, const std::string& err)
    {
        if (!_quiet)
            _p.status(Tpl::Stat::Err, _i.tr("compare connect failed", "Vergleich-Verbindung fehlgeschlagen"), {ip, err});
        else
            std::fprintf(stderr, "compare connect failed: %s (%s)\n", ip.c_str(), err.c_str());
    }

    std::string sideName(int side) const { return side == 0 ? "A" : "B"; }
    std::string _ipHost(int side) const { return side == 0 ? _ipA : _ipB; }
};

} // namespace ftc

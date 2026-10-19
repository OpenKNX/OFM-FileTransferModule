/**
 * @file        GaOps.h
 * @brief       Group address reading and writing as `oknx ga` does it, for the command line and `oknx serve`.
 * @details     GaSelect expands selectors over the imported table of one interface: x/y/z, x/y/z-n, and for a
 *              read a trailing star on x/y/, on x/ or alone; `<sel>:<DPT>` sets the type. GaBus sends one telegram at a time, the next
 *              after the L_Data.con of the one before (the interface acknowledges a tunnel request before its
 *              frame is on TP, so pacing on the tunnel ACK alone lets a list pile up in its transmit queue), and
 *              collects the answers. The tunnel callbacks are plain function pointers, so the pending state is
 *              process-wide: one GaBus at a time, which the command line and a worker's management thread are.
 * @date        2026-10-19
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include "cli/I18n.h"
#include "core/Discovery.h"
#include "core/GaTable.h"
#include "knx/dptconvert.h"
#include "knx_ip_tunnel.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace ftc
{

/** @brief One address of a `ga` call: what goes out and how answers are decoded. */
struct GaReq
{
    uint16_t ga = 0;
    std::string shown; ///< the value as typed, for the write report
    uint8_t small = 0, plen = 0, payload[14] = {0};
    uint16_t dMain = 0, dSub = 0; ///< the type the answers are decoded with (0 = unknown)
};

/** @brief One answer telegram. */
struct GaHit
{
    uint16_t src, ga;
    uint8_t small;
    std::vector<uint8_t> d;
    uint64_t t; ///< detail::nowMs() when it arrived
};

/** @brief How one telegram went: confirmed, negatively confirmed, no confirmation, not sent, stopped. */
enum GaSendState : uint8_t
{
    GA_CON_OK = 0,
    GA_CON_NEG,
    GA_CON_LOST,
    GA_NOT_SENT,
    GA_CON_STOPPED,
    GA_SKIPPED
};

/**
 * @brief Selector expansion over the imported table of one interface.
 */
class GaSelect
{
  public:
    GaSelect(const std::string& ip, I18n& L) : _L(L)
    {
        _tab.load(ip);
        _typed.clear();
    }

    /** @brief The type of @p g: an inline `:DPT` first, else the imported table; nullptr when unknown. */
    const GaInfo* infoOf(uint16_t g) const
    {
        const GaInfo* gi = _typed.find(g);
        if (gi == nullptr || gi->main == 0) gi = _tab.find(g);
        return gi != nullptr && gi->main != 0 ? gi : nullptr;
    }
    std::string nameOf(uint16_t g) const
    {
        const GaInfo* gi = _tab.find(g);
        return gi != nullptr ? gi->name : std::string();
    }
    void clearInline() { _typed.clear(); }
    const std::string& error() const { return _err; }

    /**
     * @brief One selector, comma lists already split. 0/0/0 is NOT a group address: 03_05_01 reserves 0000h for
     *        broadcast, and a frame with address type "group" and destination 0 is a T_Data_Broadcast.
     */
    bool expand(std::string tok, bool multi, std::vector<uint16_t>& out)
    {
        I18n& L = _L;
        uint16_t dMain = 0, dSub = 0;
        const size_t colon = tok.find(':');
        if (colon != std::string::npos)
        {
            if (!GaTable::parseDpt(tok.substr(colon + 1), dMain, dSub) || dMain == 0)
            {
                _err = L.tr("not a datapoint type", "kein Datenpunkttyp") + std::string(": ") + tok.substr(colon + 1);
                return false;
            }
            tok = tok.substr(0, colon);
        }
        const size_t first = out.size();
        unsigned a = 0, b = 0, c = 0, d = 0;
        char tail = 0;
        const bool star = tok.size() >= 1 && tok.back() == '*';
        if (!multi && (star || tok.find('-') != std::string::npos))
        {
            _err = L.tr("ranges and * are for read only", "Bereiche und * gibt es nur für read") + std::string(": ") + tok;
            return false;
        }
        if (multi && star)
        {
            const std::string head = tok.substr(0, tok.size() - 1);
            int depth = -1;
            if (head.empty()) depth = 0;
            else if (std::sscanf(head.c_str(), "%u/%c", &a, &tail) == 1 && head.back() == '/' && a <= 31)
                depth = 1;
            else if (std::sscanf(head.c_str(), "%u/%u/%c", &a, &b, &tail) == 2 && head.back() == '/' && a <= 31 && b <= 7)
                depth = 2;
            if (depth < 0)
            {
                _err = L.tr("not a group address", "keine Gruppenadresse") + std::string(": ") + tok;
                return false;
            }
            _tab.each([&](uint16_t g, const GaInfo&) {
                if (g == 0) return;
                if (depth >= 1 && ((g >> 11) & 0x1F) != a) return;
                if (depth >= 2 && ((g >> 8) & 0x07) != b) return;
                out.push_back(g);
            });
            if (out.size() == first)
            {
                _err = L.tr("no imported group address matches", "keine importierte Gruppenadresse passt") + std::string(": ") + tok;
                return false;
            }
        }
        else if (multi && std::sscanf(tok.c_str(), "%u/%u/%u-%u%c", &a, &b, &c, &d, &tail) == 4 && a <= 31 && b <= 7 && c <= d &&
                 d <= 255)
        {
            for (unsigned s = c; s <= d; s++)
                if ((a | b | s) != 0) out.push_back((uint16_t)((a << 11) | (b << 8) | s)); // 0/0/0-3 reads 0/0/1-3
            if (out.size() == first)
            {
                _err = L.tr("0/0/0 is not a group address", "0/0/0 ist keine Gruppenadresse");
                return false;
            }
        }
        else if (std::sscanf(tok.c_str(), "%u/%u/%u%c", &a, &b, &c, &tail) == 3 && a <= 31 && b <= 7 && c <= 255)
            out.push_back((uint16_t)((a << 11) | (b << 8) | c));
        else
        {
            _err = L.tr("not a group address", "keine Gruppenadresse") + std::string(": ") + tok;
            return false;
        }
        for (size_t i = first; i < out.size(); i++)
        {
            if (out[i] == 0)
            {
                _err = L.tr("0/0/0 is not a group address", "0/0/0 ist keine Gruppenadresse");
                return false;
            }
            if (dMain != 0)
            {
                GaInfo gi;
                gi.main = dMain;
                gi.sub = dSub;
                _typed.set(out[i], gi);
            }
        }
        return true;
    }

    bool expandList(const std::string& arg, bool multi, std::vector<uint16_t>& out)
    {
        std::string tok;
        std::istringstream ts(arg);
        while (std::getline(ts, tok, ','))
            if (!tok.empty() && !expand(tok, multi, out)) return false;
        return true;
    }

  private:
    I18n& _L;
    GaTable _tab, _typed;
    std::string _err;
};

/** @brief Why gaEncode refused a value: the error block the command line prints. */
struct GaEncodeError
{
    std::string title;
    std::vector<std::string> detail;
    std::string hint;
};

/**
 * @brief Encode the value tokens of one write for @p r (03_03_07 3.1.3: the form follows the datapoint's length).
 * @details With a known type (@p giIn) the stack's converter decides; without one a plain 0-63 is the compact
 *          form and hex octets the long one. Returns 0, or 2 with @p e filled. @p op names the verb (write, ask).
 */
inline int gaEncode(GaReq& r, const std::vector<std::string>& vals, const GaInfo* giIn, const std::string& op, I18n& L,
                    GaEncodeError& e)
{
    auto fail = [&](const std::string& title, std::initializer_list<std::string> detail, const std::string& hint) -> int {
        e.title = title;
        e.detail.assign(detail.begin(), detail.end());
        e.hint = hint;
        return 2;
    };
        if (vals.empty() || vals[0].empty())
            return fail(op + " " + L.tr("needs a value", "braucht einen Wert"),
                        {L.tr("with an imported type: the value itself (21.5, 1, 80)",
                              "mit importiertem Typ: der Wert selbst (21.5, 1, 80)"),
                         L.tr("without one: 0-63 plain, or hex octets", "ohne Typ: 0-63 blank, oder Hex-Oktette")},
                        "oknx -i <ip> ga write 1/4/5 1   ·   ga write 0/7/0:9.001 21.5");
        for (size_t i = 0; i < vals.size(); i++) r.shown += (i ? " " : "") + vals[i];
        const GaInfo* gi = giIn;
        uint16_t dMain = gi != nullptr ? gi->main : 0, dSub = gi != nullptr ? gi->sub : 0;
        r.dMain = dMain;
        r.dSub = dSub;
        if (dMain != 0)
        {
            // Sub-byte datapoints (1 bit, 2 bit, 4 bit) are the compact form; everything else is
            // carried in its own octets, as many as the type declares.
            const bool subByte = (dMain == 1 || dMain == 2 || dMain == 3 || dMain == 23);
            const Dpt d((short)dMain, (short)dSub);
            const size_t need = subByte ? 1u : (size_t)d.dataLength();
            // A typed value is one token: `21,5` must not become 21. Only a text (DPT 16) may carry spaces.
            std::string tok = vals[0];
            if (dMain == 16)
                for (size_t i = 1; i < vals.size(); i++) tok += " " + vals[i];
            else if (vals.size() > 1 || tok.find(',') != std::string::npos)
                return fail(L.tr("a value of this type is one number", "ein Wert dieses Typs ist eine Zahl"),
                            {ftc::GaTable::gaStr(r.ga) + "  DPT " + std::to_string(dMain) + "." + std::to_string(dSub) +
                                 "  " + r.shown,
                             L.tr("decimal point, not comma: 21.5", "Dezimalpunkt, kein Komma: 21.5"),
                             L.tr("nothing was sent", "es wurde nichts gesendet")},
                            "oknx -i <ip> ga write 0/7/0 21.5");
            KNXValue v(false);
            char* end = nullptr;
            if (dMain == 16) v = tok.c_str();
            else if (dMain == 9 || dMain == 14)
            {
                const double dv = std::strtod(tok.c_str(), &end);
                if (end == nullptr || *end != 0) dMain = 0;
                v = dv;
            }
            else
            {
                const long lv = std::strtol(tok.c_str(), &end, 0);
                if (end == nullptr || *end != 0) dMain = 0;
                v = (int64_t)lv;
            }
            uint8_t buf[16] = {0};
            if (dMain != 0 && need <= sizeof(buf) && KNX_Encode_Value(v, buf, need, d))
            {
                if (subByte) r.small = (uint8_t)(buf[0] & 0x3F);
                else
                {
                    r.plen = (uint8_t)(need > sizeof(r.payload) ? sizeof(r.payload) : need);
                    std::memcpy(r.payload, buf, r.plen);
                }
                return 0;
            }
            return fail(L.tr("that value does not fit the datapoint type", "dieser Wert passt nicht zum Datenpunkttyp"),
                        {ftc::GaTable::gaStr(r.ga) + "  DPT " + std::to_string(gi->main) + "." + std::to_string(dSub) +
                             "  " + r.shown,
                         L.tr("nothing was sent", "es wurde nichts gesendet")},
                        "oknx -i <ip> ga write 0/7/0 21.5");
        }
        // Untyped: octets are separate tokens, or one comma list with 0x on every octet (`0x0C,0x1A`), so a
        // decimal comma (`21,5`) is refused instead of going out as the octets 21 05.
        std::vector<std::string> toks;
        for (const std::string& t : vals)
        {
            const bool list = t.find(',') != std::string::npos;
            for (size_t a = 0;;)
            {
                const size_t b = t.find(',', a);
                toks.push_back(t.substr(a, b == std::string::npos ? std::string::npos : b - a));
                if (list && toks.back().rfind("0x", 0) != 0 && toks.back().rfind("0X", 0) != 0)
                    return fail(L.tr("a comma list needs 0x on every octet", "eine Komma-Liste braucht 0x vor jedem Oktett"),
                                {ftc::GaTable::gaStr(r.ga) + "  " + r.shown,
                                 L.tr("a number with a decimal point needs its type: ga write 0/7/0:9.001 21.5",
                                      "eine Kommazahl braucht ihren Typ: ga write 0/7/0:9.001 21.5"),
                                 L.tr("nothing was sent", "es wurde nichts gesendet")},
                                "oknx -i <ip> ga write 1/4/5=0x0C,0x1A   ·   ga write 1/4/5 0C 1A");
                if (b == std::string::npos) break;
                a = b + 1;
            }
        }
        const bool hex = toks[0].rfind("0x", 0) == 0 || toks[0].rfind("0X", 0) == 0;
        if (!hex && toks.size() == 1)
        {
            char* end = nullptr;
            long v = std::strtol(toks[0].c_str(), &end, 10);
            if (end == nullptr || *end != 0 || v < 0 || v > 63)
                return fail(L.tr("a plain value must be 0-63 without a known type",
                                 "eine blanke Zahl muss ohne bekannten Typ 0-63 sein"),
                            {ftc::GaTable::gaStr(r.ga) + "  " + r.shown,
                             L.tr("import the project, or give the type: a number 0/7/0:9.001 21.5, a text 31/0/1:16.001 uptime",
                                  "Projekt importieren, oder Typ angeben: Zahl 0/7/0:9.001 21.5, Text 31/0/1:16.001 uptime")},
                            "oknx -i <ip> ga import GA-Export.xml");
            r.small = (uint8_t)v;
            return 0;
        }
        for (const std::string& t : toks)
        {
            char* end = nullptr;
            long v = std::strtol(t.c_str(), &end, 16);
            if (t.empty() || end == nullptr || *end != 0 || v < 0 || v > 255 || r.plen >= sizeof(r.payload))
                return fail(r.plen >= sizeof(r.payload) ? L.tr("more than 14 octets", "mehr als 14 Oktette")
                                                        : L.tr("not a hex octet", "kein Hex-Oktett") + std::string(": '") + t + "'",
                            {L.tr("nothing was sent", "es wurde nichts gesendet")}, "0x00 .. 0xFF");
            r.payload[r.plen++] = (uint8_t)v;
        }
        return 0;
}

namespace detail_ga
{
inline volatile bool conSeen = false;
inline bool conOk = false;
inline uint16_t conGa = 0;
inline uint64_t conAt = 0; // detail::nowMs() of the confirmation: the telegram was on the bus
inline std::vector<GaHit> hits;
inline std::set<uint16_t> want;
inline bool takeWrite = false; // ask: a GroupValue_Write on the address is an answer too
inline uint16_t ownPa = 0;
inline void onCon(uint16_t da, bool ok)
{
    if (da != conGa) return;
    conOk = ok;
    conAt = detail::nowMs();
    conSeen = true;
}
inline void onGroup(uint16_t src, uint16_t g, uint16_t apci, const uint8_t* d, uint8_t n)
{
    constexpr uint16_t APCI_GV_WRITE = 0x0080; // A_GroupValue_Write (03_03_07 Table 1)
    const uint16_t svc = apci & 0x03C0;
    if (svc != APCI_GROUPVALUE_RESP && !(takeWrite && svc == APCI_GV_WRITE)) return;
    if (want.count(g) == 0) return;          // an answer to an address that was not asked
    if (takeWrite && src == ownPa) return;   // our own command, should the server pass it back
    hits.push_back({src, g, (uint8_t)(apci & 0x3F), std::vector<uint8_t>(d, d + n), detail::nowMs()});
}
} // namespace detail_ga

/**
 * @brief Telegrams on a tunnel for one `ga` call; restores the tunnel's callbacks when released.
 */
class GaBus
{
  public:
    GaBus(KnxIpTunnel& t, std::function<bool()> aborted) : _t(t), _aborted(std::move(aborted))
    {
        _prevCon = _t.confirmCallback();
        _t.setConfirmCallback(&detail_ga::onCon);
    }
    ~GaBus() { release(); }

    /** @brief Put the callbacks back as they were. */
    void release()
    {
        if (_released) return;
        _released = true;
        _t.setGroupCallback(nullptr);
        _t.setConfirmCallback(_prevCon);
    }

    /** @brief Send one telegram and wait (3 s at most) for its L_Data.con. */
    uint8_t sendOne(const GaReq& r, bool write)
    {
        detail_ga::conGa = r.ga;
        detail_ga::conSeen = false;
        if (!_t.sendGroupValue(r.ga, write, r.small, r.payload, r.plen)) return GA_NOT_SENT;
        for (const uint64_t t0 = detail::nowMs(); !detail_ga::conSeen && !_aborted() && detail::nowMs() - t0 < 3000;)
        {
            _t.pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (detail_ga::conSeen) return detail_ga::conOk ? GA_CON_OK : GA_CON_NEG;
        return _aborted() ? GA_CON_STOPPED : GA_CON_LOST;
    }
    uint64_t conAt() const { return detail_ga::conAt; }

    /** @brief Collect answers on @p reqs from now on; @p takeWrite also takes a GroupValue_Write (ask). */
    void listen(const std::vector<GaReq>& reqs, bool takeWrite)
    {
        detail_ga::hits.clear();
        detail_ga::want.clear();
        detail_ga::takeWrite = takeWrite;
        detail_ga::ownPa = _t.assignedPA();
        for (const GaReq& r : reqs) detail_ga::want.insert(r.ga);
        _t.setGroupCallback(&detail_ga::onGroup);
    }
    std::vector<GaHit>& hits() { return detail_ga::hits; }

    /**
     * @brief One read (or ask) cycle: send every address, then collect. A read has no single addressee, so there
     *        is no timeout to fail on - it collects for 1.5 s after the last one went out. An ask stops 300 ms
     *        after the last answer once every address that went out has one, at the latest @p waitMs after it.
     */
    void cycle(const std::vector<GaReq>& reqs, bool write, bool isAsk, uint32_t waitMs, std::vector<uint8_t>& st,
               std::vector<uint64_t>& sentAt, size_t& sentN, size_t& okN)
    {
        detail_ga::hits.clear();
        detail_ga::ownPa = _t.assignedPA(); // a reconnect can hand out another tunnel address
        sentN = okN = 0;
        for (size_t i = 0; i < reqs.size(); i++)
        {
            sentAt[i] = detail::nowMs();
            st[i] = _aborted() ? GA_SKIPPED : sendOne(reqs[i], write);
            if (st[i] == GA_CON_OK || st[i] == GA_CON_NEG) sentAt[i] = detail_ga::conAt; // "+N ms" from the telegram on the bus
            sentN += st[i] != GA_NOT_SENT && st[i] != GA_SKIPPED;
            okN += st[i] == GA_CON_OK;
        }
        const uint64_t t0 = detail::nowMs();
        for (;;)
        {
            if (_aborted() || detail::nowMs() - t0 >= (isAsk ? waitMs : 1500u)) break;
            if (isAsk)
            {
                bool all = true;
                size_t expected = 0;
                uint64_t lastHit = 0;
                for (size_t i = 0; i < reqs.size() && all; i++)
                {
                    if (st[i] == GA_NOT_SENT || st[i] == GA_SKIPPED) continue; // never handed out: no answer to wait for
                    expected++;
                    bool got = false;
                    for (const GaHit& h : detail_ga::hits)
                        if (h.ga == reqs[i].ga)
                        {
                            got = true;
                            lastHit = h.t > lastHit ? h.t : lastHit;
                        }
                    all = got;
                }
                if (expected == 0 || (all && detail::nowMs() - lastHit >= 300)) break;
            }
            _t.pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!_aborted()) _t.pump(); // what arrived during the last sleep still belongs to this cycle
    }

  private:
    KnxIpTunnel& _t;
    std::function<bool()> _aborted;
    FtcConCb _prevCon = nullptr;
    bool _released = false;
};

} // namespace ftc

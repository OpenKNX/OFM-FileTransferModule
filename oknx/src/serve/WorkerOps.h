/**
 * @file        WorkerOps.h
 * @brief       The operations a worker runs in its management thread, and what they need from the worker.
 * @details     Each operation gets its job (id, arguments, cancel mark) and returns `{ok, data}` or
 *              `{ok:false, error:{code, reason, detail}}` with a code from the fixed list (doc/guide/serve.md). Arguments
 *              arrive already checked against the operation's description in the leader; an operation still
 *              refuses what it cannot use instead of guessing.
 * @date        2026-10-19
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include "FileTransferClient.h"
#include "cli/I18n.h"
#include "cli/Templates.h"
#include "core/GaOps.h"
#include "core/Describe.h"
#include "core/DeviceMgmt.h"
#include "core/MReset.h"
#include "core/PropRead.h"
#include "core/SetPa.h"
#include "knx_ip_tunnel.h"
#include "serve/Proto.h"

#include <atomic>
#include <cctype>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace ftc
{
namespace serve
{

/** @brief One job as the worker received it. */
struct Job
{
    std::string id;
    std::string op;
    json args = json::object();
    std::shared_ptr<std::atomic<bool>> cancel;
};

/** @brief What an operation may ask of its worker (management thread only). */
class WorkerApi
{
  public:
    virtual ~WorkerApi() = default;
    virtual const std::string& ip() const = 0;
    virtual uint16_t port() const = 0;
    /** @brief One progress line for the running job. */
    virtual void progress(const Job& job, const json& data) = 0;
    /** @brief Open the management tunnel if it is not open; false with @p error filled when that fails. */
    virtual bool ensureTunnel(json& error) = 0;
    /** @brief Close the management tunnel if it is open. */
    virtual void closeTunnel() = 0;
    /** @brief Set up the FTC client once and give it the interface's max APDU (FTC operations only). */
    virtual void prepareFtc() = 0;
    /** @brief Collect what the FTC client narrates into @p v (nullptr stops it). */
    virtual void setJobLines(std::vector<std::string>* v) = 0;
    /** @brief Call from every pump loop: keeps the state file's send time current. */
    virtual void pumpTick() = 0;
};

inline json errorJson(const std::string& code, const std::string& reason, const json& detail)
{
    return json{{"code", code}, {"reason", reason}, {"detail", detail.is_null() ? json::object() : detail}};
}

inline json opFail(const std::string& code, const std::string& reason, const json& detail = json::object())
{
    return json{{"ok", false}, {"error", errorJson(code, reason, detail)}};
}

inline json opOk(const json& data) { return json{{"ok", true}, {"data", data}}; }

/** @brief Parse "a.l.d" into a packed individual address; false when it is not one. */
inline bool parsePa(const std::string& s, uint16_t& pa)
{
    unsigned a = 0, l = 0, d = 0;
    char tail = 0;
    if (std::sscanf(s.c_str(), "%u.%u.%u%c", &a, &l, &d, &tail) != 3 || a > 15 || l > 15 || d > 255) return false;
    pa = (uint16_t)((a << 12) | (l << 8) | d);
    return true;
}

inline const char* ftcPhaseName(FtcPhase p)
{
    switch (p)
    {
        case FtcPhase::Idle: return "idle";
        case FtcPhase::Ping: return "ping";
        case FtcPhase::List: return "list";
        case FtcPhase::Info: return "info";
        case FtcPhase::Delete: return "delete";
        case FtcPhase::Upload: return "upload";
        case FtcPhase::Verify: return "verify";
        case FtcPhase::Scan: return "scan";
        case FtcPhase::Done: return "done";
        case FtcPhase::Failed: return "failed";
    }
    return "unknown";
}

/**********************************************************************
 **************************** OPERATIONS ****************************
 **********************************************************************/
/** @brief `ping`: proves the job path end to end without touching the bus. */
inline json opPing(WorkerApi& w, const Job&)
{
    return opOk({{"pong", true}, {"ip", w.ip()}, {"port", w.port()}});
}

/**
 * @brief `ftc`: one FTC command on a device, as the device console takes it (`ftc <pa> <args…>`).
 * @details Runs the client to its end exactly like the command line does (same quiet rule: 1.5 s without
 *          activity ends a read chain), reports progress once a second, and on cancel goes the client's own
 *          cancel path (requestCancel, then 1.5 s of pumping so the target closes its session).
 */
inline json runFtc(WorkerApi& w, const Job& job, const std::string& cmd, uint64_t capMs);
inline json runFtcWith(WorkerApi& w, const Job& job, const std::function<void()>& start, uint64_t capMs);

inline json opFtc(WorkerApi& w, const Job& job)
{
    uint16_t pa = 0;
    const std::string paStr = job.args.value("pa", std::string());
    if (!parsePa(paStr, pa)) return opFail("INVALID_ARGUMENT", "pa is not an individual address", {{"pa", paStr}});
    if (!job.args.contains("argv") || !job.args["argv"].is_array() || job.args["argv"].empty())
        return opFail("INVALID_ARGUMENT", "argv needs the command and its arguments");
    std::string cmd = "ftc " + paStr;
    for (const auto& t : job.args["argv"])
    {
        if (!t.is_string() || t.get<std::string>().empty())
            return opFail("INVALID_ARGUMENT", "every argv entry is a non-empty string");
        const std::string v = t.get<std::string>();
        for (char c : v)
            if (std::isspace((unsigned char)c) || c == '\0')
                return opFail("INVALID_ARGUMENT", "argv entries cannot contain spaces", {{"entry", v}});
        cmd += ' ';
        cmd += v;
    }
    return runFtc(w, job, cmd, job.args.value("timeout_ms", (uint64_t)1800000));
}

/** @brief Run one FTC command to its end, the way runOneShotToQuiescence does on the command line. */
inline json runFtc(WorkerApi& w, const Job& job, const std::string& cmd, uint64_t capMs)
{
    return runFtcWith(w, job, [&cmd] { openknxFileTransferClient.processCommand(cmd, false); }, capMs);
}

/** @brief Start the client with @p start, then pump it to its end (quiet 1.5 s, or Done/Failed, or the cap). */
inline json runFtcWith(WorkerApi& w, const Job& job, const std::function<void()>& start, uint64_t capMs)
{
    json err;
    if (!w.ensureTunnel(err)) return json{{"ok", false}, {"error", err}};
    w.prepareFtc();

    std::vector<std::string> lines;
    w.setJobLines(&lines);
    start();

    const uint64_t QUIET_MS = 1500;
    const uint64_t t0 = monoMs();
    uint64_t lastActivity = t0, lastProgress = 0;
    uint32_t lastTx = knxTunnelActivity();
    FtcPhase lastPhase = openknxFileTransferClient.status().phase;
    uint32_t lastDone = openknxFileTransferClient.status().done;
    auto pumpOnce = [&] {
        g_knxTunnel.pump();
        openknxFileTransferClient.loop(true);
        w.pumpTick();
    };
    json res;
    for (;;)
    {
        if (*job.cancel)
        {
            openknxFileTransferClient.requestCancel();
            for (const uint64_t until = monoMs() + 1500; monoMs() < until;)
            {
                pumpOnce();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            res = opFail("CANCELLED", "cancelled; the client closed its session with the device");
            break;
        }
        pumpOnce();
        const FtcStatus& st = openknxFileTransferClient.status();
        const uint64_t now = monoMs();
        if (now - lastProgress >= 1000)
        {
            w.progress(job, {{"phase", ftcPhaseName(st.phase)},
                             {"done", st.done},
                             {"total", st.total},
                             {"bps", st.bps},
                             {"chunk", st.chunk},
                             {"chunks", st.chunks},
                             {"window", st.window}});
            lastProgress = now;
        }
        if (st.phase == FtcPhase::Done || st.phase == FtcPhase::Failed)
        {
            for (int i = 0; i < 64; ++i) // the final report is drained one loop() pass after Done
            {
                pumpOnce();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            const uint64_t secs = (monoMs() - t0) / 1000;
            json data = {{"phase", ftcPhaseName(st.phase)},
                         {"ok", st.phase == FtcPhase::Done && st.ok},
                         {"done", st.done},
                         {"total", st.total},
                         {"ms", monoMs() - t0},
                         {"bps_avg", secs ? (uint64_t)st.done / secs : (uint64_t)st.bps},
                         {"crc", st.crc},
                         {"verifies", st.verifies},
                         {"crc_errors", st.crcErrors},
                         {"resends", st.resends},
                         {"unconfirmed", st.unconfirmed},
                         {"lines", lines}};
            res = st.phase == FtcPhase::Done ? opOk(data)
                                             : json{{"ok", false},
                                                    {"error", errorJson("NO_ANSWER", "the FTC operation failed", data)}};
            break;
        }
        const uint32_t tx = knxTunnelActivity();
        if (tx != lastTx || st.phase != lastPhase || st.done != lastDone || openknxFileTransferClient.isBusy())
        {
            lastTx = tx;
            lastPhase = st.phase;
            lastDone = st.done;
            lastActivity = now;
        }
        if (now - lastActivity > QUIET_MS)
        {
            res = opOk({{"phase", ftcPhaseName(st.phase)}, {"ok", true}, {"ms", now - t0}, {"lines", lines}});
            break;
        }
        if (now - t0 > capMs)
        {
            openknxFileTransferClient.requestCancel();
            res = opFail("TIMEOUT", "the operation ran past its time limit", {{"timeout_ms", capMs}});
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    w.setJobLines(nullptr);
    return res;
}

inline std::string hexUpper(const std::vector<uint8_t>& v)
{
    static const char* H = "0123456789ABCDEF";
    std::string s;
    for (uint8_t b : v)
    {
        s += H[b >> 4];
        s += H[b & 15];
    }
    return s;
}

/** @brief `prop_read`: M_PropRead on the interface itself (the read of `oknx prop read`). */
inline json opPropRead(WorkerApi& w, const Job& job)
{
    const json& a = job.args;
    const PropReadResult r = propReadLocal(w.ip(), w.port(), (uint16_t)a.value("object_type", 0), (uint8_t)a.value("instance", 1),
                                           (uint8_t)a.value("pid", 0), (uint16_t)a.value("start", 1), (uint8_t)a.value("count", 1));
    if (!r.connected)
        return opFail("CONNECT_FAILED", "no device management connection: " + r.connectError, {{"outside_serve", false}});
    if (r.outcome == DmRead::Data) return opOk({{"outcome", "data"}, {"raw", hexUpper(r.data)}, {"length", r.data.size()}});
    if (r.outcome == DmRead::Negative)
    {
        char hx[8];
        std::snprintf(hx, sizeof(hx), "0x%02X", r.propError);
        return opOk({{"outcome", "negative"}, {"error_code", hx}, {"error_name", dmPropErrorName(r.propError)}, {"raw", ""}, {"length", 0}});
    }
    return opFail("NO_ANSWER", "the interface did not answer the property read");
}

/**
 * @brief `device_mreset`: M_Reset.req to the interface (the restart of `oknx -i <ip> reset`), then wait until it
 *        answers again. Every KNXnet/IP connection to it closes; this worker gives its own tunnel back first.
 */
inline json opMReset(WorkerApi& w, const Job& job)
{
    w.closeTunnel(); // closed by the restart anyway: leave it cleanly and drop it from the state file
    const uint32_t waitMs = (uint32_t)job.args.value("wait_s", 30) * 1000u;
    const auto cancel = job.cancel;
    const MResetResult r = mresetInterface(w.ip(), w.port(), waitMs, [cancel] { return cancel->load(); });
    if (!r.connected) return opFail("CONNECT_FAILED", "no device management connection: " + r.connectError);
    if (r.outcome == DevMgmtSession::Reset::NoAnswer) return opFail("NO_ANSWER", "the interface did not answer M_Reset.req");
    json d = {{"outcome", mresetOutcomeName(r.outcome)}, {"back", r.back}};
    if (r.back) d["back_after_ms"] = r.backAfterMs;
    if (!r.back && *job.cancel) return opFail("CANCELLED", "the reset was sent; the wait for the interface was cancelled", d);
    if (!r.back) return opFail("NO_ANSWER", "the reset was sent; the interface did not answer again in time", d);
    return opOk(d);
}

/** @brief `busprop_read`: A_PropertyValue_Read to a device on the bus (the read of `oknx busprop read`). */
inline json opBusPropRead(WorkerApi& w, const Job& job)
{
    const json& a = job.args;
    uint16_t pa = 0;
    if (!parsePa(a.value("pa", std::string()), pa) || pa == 0) return opFail("INVALID_ARGUMENT", "pa is not an individual address");
    json err;
    if (!w.ensureTunnel(err)) return json{{"ok", false}, {"error", err}};
    const BusPropResult r = busPropRead(g_knxTunnel, pa, (uint8_t)a.value("object_index", 0), (uint8_t)a.value("pid", 0),
                                        (uint8_t)a.value("count", 1), (uint16_t)a.value("start", 1));
    w.pumpTick();
    if (!r.sent) return opFail("INTERNAL", "the tunnel did not take the request");
    if (!r.answered) return opFail("NO_ANSWER", "no A_PropertyValue_Response within 3 s");
    return opOk({{"outcome", "data"}, {"raw", hexUpper(r.data)}, {"length", r.data.size()}});
}

inline const char* gaStateName(uint8_t st)
{
    switch (st)
    {
        case GA_CON_OK: return "confirmed";
        case GA_CON_NEG: return "not_confirmed";
        case GA_CON_LOST: return "no_confirmation";
        case GA_NOT_SENT: return "not_sent";
        case GA_CON_STOPPED: return "stopped";
        default: return "skipped";
    }
}

/** @brief `ga_read`: the read of `oknx ga read`, same selection, same telegram path, same 1.5 s window. */
inline json opGaRead(WorkerApi& w, const Job& job)
{
    I18n L;
    L.detect("en");
    GaSelect sel(w.ip(), L);
    std::vector<uint16_t> gas;
    for (const auto& g : job.args.value("ga", json::array()))
        if (!g.is_string() || !sel.expandList(g.get<std::string>(), true, gas))
            return opFail("INVALID_ARGUMENT", sel.error().empty() ? std::string("ga holds selectors") : sel.error());
    std::vector<GaReq> reqs;
    std::set<uint16_t> seen;
    for (uint16_t g : gas)
        if (seen.insert(g).second)
        {
            GaReq r;
            r.ga = g;
            if (const GaInfo* gi = sel.infoOf(g))
            {
                r.dMain = gi->main;
                r.dSub = gi->sub;
            }
            reqs.push_back(r);
        }
    if (reqs.empty()) return opFail("INVALID_ARGUMENT", "no group address selected");
    if (reqs.size() > 512) return opFail("INVALID_ARGUMENT", "more than 512 addresses in one read", {{"selected", reqs.size()}});
    json err;
    if (!w.ensureTunnel(err)) return json{{"ok", false}, {"error", err}};
    const auto cancel = job.cancel;
    GaBus bus(g_knxTunnel, [cancel] { return cancel->load(); });
    bus.listen(reqs, false);
    std::vector<uint8_t> st(reqs.size(), GA_NOT_SENT);
    std::vector<uint64_t> sentAt(reqs.size(), 0);
    size_t sentN = 0, okN = 0;
    w.progress(job, {{"addresses", reqs.size()}, {"phase", "sending"}});
    bus.cycle(reqs, false, false, 1500, st, sentAt, sentN, okN);
    w.pumpTick();
    json results = json::array();
    size_t answered = 0;
    for (size_t i = 0; i < reqs.size(); ++i)
    {
        json answers = json::array();
        for (const GaHit& h : bus.hits())
        {
            if (h.ga != reqs[i].ga) continue;
            std::string raw;
            char b[8];
            if (h.d.empty())
            {
                std::snprintf(b, sizeof(b), "%02X", h.small);
                raw = b;
            }
            else
                raw = hexUpper(h.d);
            const std::string value =
                reqs[i].dMain != 0 ? GaTable::decode(reqs[i].dMain, reqs[i].dSub, h.d.data(), (uint8_t)h.d.size(), h.small) : std::string();
            answers.push_back({{"src", Tpl::pa(h.src)}, {"raw", raw}, {"six_bit", h.d.empty()}, {"value", value},
                               {"after_ms", h.t >= sentAt[i] ? h.t - sentAt[i] : 0}});
        }
        if (!answers.empty()) answered++;
        json r = {{"ga", GaTable::gaStr(reqs[i].ga)}, {"name", sel.nameOf(reqs[i].ga)}, {"answers", answers}, {"status", gaStateName(st[i])}};
        if (reqs[i].dMain) r["dpt"] = std::to_string(reqs[i].dMain) + "." + std::to_string(reqs[i].dSub);
        results.push_back(r);
    }
    bus.release();
    if (*job.cancel) return opFail("CANCELLED", "cancelled", {{"results", results}});
    return opOk({{"results", results}, {"sent", sentN}, {"confirmed", okN}, {"answered", answered}, {"window_ms", 1500}});
}

inline std::string hex6(const uint8_t* v)
{
    char b[16];
    std::snprintf(b, sizeof(b), "%02X%02X%02X%02X%02X%02X", v[0], v[1], v[2], v[3], v[4], v[5]);
    return b;
}

/** @brief PDT_VERSION [magic:5][version:5][revision:6] as "version.revision". */
inline std::string pdtVersion(uint16_t v) { return std::to_string((v >> 6) & 0x1F) + "." + std::to_string(v & 0x3F); }

/**
 * @brief `device_info`: without pa the interface itself (DESCRIPTION + one device management session, the data
 *        of `oknx -i <ip> info`, no tunnel slot taken), with pa a bus device (the FTC info chain of `oknx <pa> info`).
 */
inline json opDeviceInfo(WorkerApi& w, const Job& job)
{
    const std::string paStr = job.args.value("pa", std::string());
    if (paStr.empty())
    {
        IfaceDesc d;
        if (!queryInterface(w.ip(), w.port(), d)) return opFail("NO_ANSWER", "no DESCRIPTION_RESPONSE from the interface");
        InterfaceDetails det;
        queryInterfaceDetails(w.ip(), w.port(), det);
        char mask[8];
        std::snprintf(mask, sizeof(mask), "0x%04X", det.haveMask ? det.mask : d.mask);
        json data = {{"target", "interface"},
                     {"name", d.name},
                     {"pa", Tpl::pa(d.ia)},
                     {"mask", (det.haveMask || d.hasExt) ? json(mask) : json()},
                     {"manufacturer", det.haveMfr ? json(det.manufacturer) : json()},
                     {"order", det.haveOrder ? json(std::string(det.order)) : json()},
                     {"version", det.haveFirmware ? json(det.firmware) : json()},
                     {"serial", hex6(det.haveSerial ? det.serial : d.serial)},
                     {"progmode", (d.status & 0x01) != 0},
                     {"max_apdu", det.haveApdu ? json(det.maxApdu) : json()},
                     {"ftm", nullptr},
                     {"device_management", det.ok}};
        return opOk(data);
    }
    uint16_t pa = 0;
    if (!parsePa(paStr, pa) || pa == 0) return opFail("INVALID_ARGUMENT", "pa is not an individual address");
    json r = runFtc(w, job, "ftc " + paStr + " info", 120000);
    const FtcDeviceInfo& di = openknxFileTransferClient.deviceInfo();
    if (!di.valid)
    {
        if (!r.value("ok", false)) return r;
        return opFail("NO_ANSWER", "the device did not answer the identity reads");
    }
    char mask[8];
    std::snprintf(mask, sizeof(mask), "0x%04X", di.mask);
    json ftm = nullptr;
    if (di.ftmVersion)
        ftm = {{"version", std::to_string(di.ftmVersion >> 8) + "." + std::to_string((di.ftmVersion >> 4) & 0x0F) + "." +
                               std::to_string(di.ftmVersion & 0x0F)},
               {"legacy", di.ftmLegacy},
               {"features", di.features}};
    return opOk({{"target", paStr},
                 {"mask", mask},
                 {"class", std::string(di.cls)},
                 {"manufacturer", di.manufacturer},
                 {"order", di.haveOrder ? json(std::string(di.order)) : json()},
                 {"hardware", di.haveHw ? json(hex6(di.hardware)) : json()},
                 {"version", di.haveVersion ? json(pdtVersion(di.version)) : json()},
                 {"serial", di.haveSerial ? json(hex6(di.serial)) : json()},
                 {"progmode", di.progMode},
                 {"max_apdu", di.maxApdu ? json(di.maxApdu) : json()},
                 {"ftm", ftm},
                 {"bus_voltage_mv", di.haveBusVolt ? json(di.busVoltmV) : json()}});
}

/** @brief The items of ga_write / ga_ask encoded as the command line encodes them; an error names the item. */
inline bool gaItems(WorkerApi& w, const Job& job, const std::string& verb, GaSelect& sel, I18n& L, std::vector<GaReq>& reqs, json& fail)
{
    (void)w;
    for (const auto& it : job.args.value("items", json::array()))
    {
        const std::string gaSel = it.value("ga", std::string());
        const std::string value = it.value("value", std::string());
        std::vector<uint16_t> gas;
        sel.clearInline(); // the type of one item does not carry over to the next
        if (!sel.expandList(gaSel, false, gas) || gas.size() != 1)
        {
            fail = opFail("INVALID_ARGUMENT", sel.error().empty() ? "one group address per item" : sel.error(), {{"ga", gaSel}});
            return false;
        }
        GaReq r;
        r.ga = gas[0];
        std::vector<std::string> vals;
        {
            std::string tok;
            for (char c : value)
            {
                if (c == ' ')
                {
                    if (!tok.empty()) vals.push_back(tok);
                    tok.clear();
                }
                else
                    tok += c;
            }
            if (!tok.empty()) vals.push_back(tok);
        }
        GaEncodeError e;
        if (gaEncode(r, vals, sel.infoOf(r.ga), verb, L, e) != 0)
        {
            json d = json::array();
            for (const auto& x : e.detail)
                if (!x.empty()) d.push_back(x);
            fail = opFail("INVALID_ARGUMENT", e.title, {{"ga", gaSel}, {"value", value}, {"detail", d}, {"hint", e.hint}});
            return false;
        }
        for (const GaReq& o : reqs)
            if (verb == "ask" && o.ga == r.ga)
            {
                fail = opFail("INVALID_ARGUMENT", "ask names each address once", {{"ga", gaSel}});
                return false;
            }
        reqs.push_back(r);
    }
    return !reqs.empty() || (fail = opFail("INVALID_ARGUMENT", "no items"), false);
}

/** @brief `ga_write`: as `oknx ga write`, one telegram after the L_Data.con of the one before. */
inline json opGaWrite(WorkerApi& w, const Job& job)
{
    I18n L;
    L.detect("en");
    GaSelect sel(w.ip(), L);
    std::vector<GaReq> reqs;
    json fail;
    if (!gaItems(w, job, "write", sel, L, reqs, fail)) return fail;
    json err;
    if (!w.ensureTunnel(err)) return json{{"ok", false}, {"error", err}};
    const auto cancel = job.cancel;
    GaBus bus(g_knxTunnel, [cancel] { return cancel->load(); });
    json results = json::array();
    size_t okN = 0;
    for (const GaReq& r : reqs)
    {
        const uint8_t st = *job.cancel ? (uint8_t)GA_SKIPPED : bus.sendOne(r, true);
        okN += st == GA_CON_OK;
        results.push_back({{"ga", GaTable::gaStr(r.ga)}, {"status", gaStateName(st)}});
        w.pumpTick();
    }
    bus.release();
    if (*job.cancel) return opFail("CANCELLED", "cancelled; the items marked sent or confirmed are on the bus", {{"results", results}});
    return opOk({{"results", results}, {"confirmed", okN}, {"note", "a group write is not acknowledged by its receivers"}});
}

/** @brief `ga_ask`: as `oknx ga ask`, write each item and collect the answers on the same addresses. */
inline json opGaAsk(WorkerApi& w, const Job& job)
{
    I18n L;
    L.detect("en");
    GaSelect sel(w.ip(), L);
    std::vector<GaReq> reqs;
    json fail;
    if (!gaItems(w, job, "ask", sel, L, reqs, fail)) return fail;
    const uint32_t waitMs = (uint32_t)job.args.value("wait_ms", 1500);
    json err;
    if (!w.ensureTunnel(err)) return json{{"ok", false}, {"error", err}};
    const auto cancel = job.cancel;
    GaBus bus(g_knxTunnel, [cancel] { return cancel->load(); });
    bus.listen(reqs, true);
    std::vector<uint8_t> st(reqs.size(), GA_NOT_SENT);
    std::vector<uint64_t> sentAt(reqs.size(), 0);
    size_t sentN = 0, okN = 0;
    w.progress(job, {{"addresses", reqs.size()}, {"phase", "sending"}});
    bus.cycle(reqs, true, true, waitMs, st, sentAt, sentN, okN);
    w.pumpTick();
    json results = json::array();
    for (size_t i = 0; i < reqs.size(); ++i)
    {
        json answers = json::array();
        for (const GaHit& h : bus.hits())
        {
            if (h.ga != reqs[i].ga) continue;
            char b[8];
            std::string raw;
            if (h.d.empty())
            {
                std::snprintf(b, sizeof(b), "%02X", h.small);
                raw = b;
            }
            else
                raw = hexUpper(h.d);
            const std::string value =
                reqs[i].dMain != 0 ? GaTable::decode(reqs[i].dMain, reqs[i].dSub, h.d.data(), (uint8_t)h.d.size(), h.small) : std::string();
            answers.push_back({{"src", Tpl::pa(h.src)}, {"raw", raw}, {"value", value}, {"after_ms", h.t >= sentAt[i] ? h.t - sentAt[i] : 0}});
        }
        results.push_back({{"ga", GaTable::gaStr(reqs[i].ga)}, {"status", gaStateName(st[i])}, {"answers", answers}});
    }
    bus.release();
    if (*job.cancel) return opFail("CANCELLED", "cancelled; the commands marked sent are on the bus", {{"results", results}});
    return opOk({{"results", results}, {"sent", sentN}, {"confirmed", okN}, {"wait_ms", waitMs}});
}

/** @brief `restart`: A_Restart over a transport connection, as `oknx <pa> restart`. */
inline json opRestart(WorkerApi& w, const Job& job)
{
    uint16_t pa = 0;
    if (!parsePa(job.args.value("pa", std::string()), pa) || pa == 0) return opFail("INVALID_ARGUMENT", "pa is not an individual address");
    json err;
    if (!w.ensureTunnel(err)) return json{{"ok", false}, {"error", err}};
    const RestartResult r = deviceRestart(g_knxTunnel, pa, false, 0);
    w.pumpTick();
    if (!r.sent) return opFail("INTERNAL", "the tunnel did not take the restart");
    return opOk({{"sent", true}, {"connection_oriented", r.connectionOriented}, {"note", "a restart is not acknowledged; the device reboots"}});
}

/** @brief `runstate`: the application program's run state, as `oknx <pa> runstate`. */
inline json opRunState(WorkerApi& w, const Job& job)
{
    uint16_t pa = 0;
    if (!parsePa(job.args.value("pa", std::string()), pa) || pa == 0) return opFail("INVALID_ARGUMENT", "pa is not an individual address");
    const std::string action = job.args.value("action", std::string("read"));
    const int event = action == "start" ? 1 : action == "stop" ? 2 : -1;
    json err;
    if (!w.ensureTunnel(err)) return json{{"ok", false}, {"error", err}};
    const RunStateResult r = runState(g_knxTunnel, pa, event);
    w.pumpTick();
    if (r.appIdx < 0) return opFail("NO_ANSWER", "no application program object (type 3) on this device");
    if (r.before == -2) return opFail("NO_ANSWER", "no answer to PID_RUN_STATE_CONTROL", {{"object_index", r.appIdx}});
    if (r.before == -3) return opFail("NO_ANSWER", "PID_RUN_STATE_CONTROL is not implemented on this device", {{"object_index", r.appIdx}});
    json d = {{"object_index", r.appIdx}, {"before", r.before}, {"before_name", runStateName(r.before)}};
    if (event >= 0)
    {
        if (r.sendFailed) return opFail("INTERNAL", "the write could not be sent", d);
        if (r.after < 0) return opFail("NO_ANSWER", action + " sent, but the state could not be read back", d);
        d["after"] = r.after;
        d["after_name"] = runStateName(r.after);
    }
    return opOk(d);
}

inline bool parseSerial(const std::string& in, uint8_t out[6])
{
    const std::string s = in.rfind("0x", 0) == 0 || in.rfind("0X", 0) == 0 ? in.substr(2) : in;
    std::string h;
    for (char ch : s)
    {
        if (ch == ':' || ch == '-') continue;
        if (!std::isxdigit((unsigned char)ch)) return false;
        h += ch;
    }
    if (h.size() != 12) return false;
    for (int i = 0; i < 6; i++) out[i] = (uint8_t)std::strtoul(h.substr((size_t)i * 2, 2).c_str(), nullptr, 16);
    return true;
}

/** @brief A serial number as the command line writes it, 00FA:01A2B3C4 (the form parseSerial() reads). */
inline std::string serialStr(const uint8_t sn[6])
{
    char b[20];
    std::snprintf(b, sizeof(b), "%02X%02X:%02X%02X%02X%02X", sn[0], sn[1], sn[2], sn[3], sn[4], sn[5]);
    return b;
}

/**
 * @brief `setpa`: the non-interactive forms of `oknx setpa` on the same bus side (SetPaBus): by serial number, or
 *        to the one device in programming mode. Several in programming mode: refused with their addresses and
 *        serial numbers (PID 11), the caller then names one (there is nobody to pick here).
 */
inline json opSetPa(WorkerApi& w, const Job& job)
{
    uint16_t want = 0;
    if (!parsePa(job.args.value("pa", std::string()), want) || want == 0) return opFail("INVALID_ARGUMENT", "pa is not an individual address");
    const std::string snIn = job.args.value("serial", std::string());
    const bool restart = job.args.value("restart", true);
    const int waitS = job.args.value("wait_s", 0);
    uint8_t sn[6] = {0};
    const bool bySn = !snIn.empty();
    if (bySn && !parseSerial(snIn, sn)) return opFail("INVALID_ARGUMENT", "serial is 12 hex digits, e.g. 00FA:01A2B3C4");
    json err;
    if (!w.ensureTunnel(err)) return json{{"ok", false}, {"error", err}};
    const auto cancel = job.cancel;
    SetPaBus spb(g_knxTunnel, [cancel] { return cancel->load(); });
    auto done = [&](json r) {
        spb.release();
        w.pumpTick();
        return r;
    };
    const std::string wantS = Tpl::pa(want);
    if (bySn)
    {
        uint16_t had = 0;
        if (!spb.snWhere(sn, had)) return done(opFail("NO_ANSWER", "no device answers to this serial number; nothing was sent", {{"serial", snIn}}));
        if (had == want) return done(opOk({{"from", Tpl::pa(had)}, {"to", wantS}, {"written", false}, {"note", "it already has this address"}}));
        const int occ = spb.coAsk(want);
        g_knxTunnel.scanDisconnect();
        if (occ != 0) return done(opFail("GUARD_DENIED", "the address is already in use: another device answered; nothing was sent", {{"pa", wantS}}));
        g_knxTunnel.sendIndividualAddressSerialNumberWrite(sn, want);
        spb.pumpFor(300);
        uint16_t now = 0;
        const bool answered = spb.snWhere(sn, now);
        const bool took = answered && now == want;
        const int conf = took && restart ? spb.confirmAt(want, true) : 0;
        if (!took)
            return done(opFail("NO_ANSWER", answered ? "the address was not taken: the device answers on " + Tpl::pa(now)
                                                     : "the address went out, the read-back by serial number got no answer",
                               {{"serial", snIn}, {"written", true}}));
        return done(opOk({{"from", Tpl::pa(had)}, {"to", wantS}, {"written", true}, {"restarted", conf == 2},
                          {"note", !restart ? "no restart: a device with the OpenKNX stack keeps it only after a restart"
                                   : conf == 2 ? "confirmed; restart sent, the device keeps the address"
                                               : "confirmed; no restart possible - restart the device so it keeps the address"}}));
    }
    // programming mode: the address must be free unless the device in programming mode is the one holding it
    const bool occupied = spb.coAsk(want) != 0;
    g_knxTunnel.scanDisconnect();
    const uint64_t t0 = monoMs();
    for (;;)
    {
        spb.probe(1000);
        if (*job.cancel) return done(opFail("CANCELLED", "cancelled; nothing was written"));
        const size_t n = spb.progCount();
        if (n == 1) break;
        if (n > 1)
        {
            // as the command line: each answering address is asked for its serial number, so the caller can name one
            spb.askSerials();
            if (*job.cancel) return done(opFail("CANCELLED", "cancelled; nothing was written"));
            json who = json::array();
            for (const auto& h : spb.prog)
            {
                json sns = json::array();
                for (const auto& s : spb.pid11)
                    if (s.src == h.src) sns.push_back(serialStr(s.sn));
                who.push_back({{"pa", Tpl::pa(h.src)}, {"answers", h.orig ? h.orig : 1}, {"serials", sns}});
            }
            json d = {{"devices", who}, {"in_prog_mode", spb.progCount()}, {"serials_answered", spb.pid11.size()}};
            if (spb.pid11.size() > spb.progCount())
                d["note"] = "more devices answered than are in programming mode: compare the serial number on the device";
            return done(opFail("BUSY", "several devices are in programming mode; name the one by its serial number (setpa with serial)", d));
        }
        if (monoMs() - t0 >= (uint64_t)waitS * 1000)
            return done(opFail("NO_ANSWER", "no device in programming mode; nothing was written", {{"waited_s", waitS}}));
        w.progress(job, {{"waiting_s", (monoMs() - t0) / 1000}, {"wait_s", waitS}});
    }
    const uint16_t had = spb.prog[0].src;
    bool occupiedNow = occupied;
    if (monoMs() - t0 > 5000 && had != want) // checked before a long wait: look again
    {
        occupiedNow = spb.coAsk(want) != 0;
        g_knxTunnel.scanDisconnect();
    }
    if (occupiedNow && had != want)
        return done(opFail("GUARD_DENIED", "the address is already in use: another device answered; nothing was sent", {{"pa", wantS}}));
    // write (unless it has it already), confirm, restart: in programming mode the restart is what ends it, so it is
    // always sent here; restart:false is for the serial-number way only
    if (had != want)
    {
        g_knxTunnel.sendIndividualAddressWrite(want);
        spb.pumpFor(600);
    }
    const int conf = spb.confirmAt(want, true);
    bool took = conf != 0;
    if (!took)
    {
        spb.probe(2500); // a device without connections: the broadcast read-back still shows the new address
        took = spb.progCount() == 1 && spb.prog[0].src == want;
    }
    if (!took)
        return done(opFail("NO_ANSWER", "the address was not taken: the read-back did not show it", {{"pa", wantS}, {"written", had != want}}));
    json d = {{"from", Tpl::pa(had)}, {"to", wantS}, {"written", had != want}, {"restarted", conf == 2},
              {"note", conf == 2 ? "confirmed; restart sent, it ends programming mode"
                                 : "confirmed; no restart possible - restart the device so it keeps the address"}};
    if (!restart) d["restart_ignored"] = "restart:false is for the serial-number way; in programming mode the restart is what ends it";
    return done(opOk(d));
}

/** @brief `ftc_login`: the challenge-response with the password the leader passed (never logged). */
inline json opFtcLogin(WorkerApi& w, const Job& job)
{
    uint16_t pa = 0;
    if (!parsePa(job.args.value("pa", std::string()), pa) || pa == 0) return opFail("INVALID_ARGUMENT", "pa is not an individual address");
    std::string pw = job.args.value("password", std::string());
    if (pw.empty()) return opFail("INVALID_ARGUMENT", "no password reached the worker");
    json last;
    FileTransferClient::FtcAuthOutcome out = FileTransferClient::FtcAuthNone;
    // A lost frame is not an answer about the password: an unanswered exchange is repeated once, a refusal never.
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        last = runFtcWith(w, job, [&] { openknxFileTransferClient.requestLogin(pa, pw.c_str()); }, 30000);
        out = openknxFileTransferClient.authOutcome();
        if (out != FileTransferClient::FtcAuthNoAnswer) break;
    }
    pw.assign(pw.size(), '\0');
    switch (out)
    {
        case FileTransferClient::FtcAuthOk: return opOk({{"signed_in", true}, {"note", "writes stay allowed until the device goes idle"}});
        case FileTransferClient::FtcAuthNotNeeded: return opOk({{"signed_in", true}, {"note", "this device does not ask for a password"}});
        case FileTransferClient::FtcAuthBadPw:
        {
            const uint16_t back = openknxFileTransferClient.authBackoffSeconds();
            return opFail("GUARD_DENIED", back ? "too many attempts: the device holds further logins off" : "the password was not accepted",
                          {{"backoff_s", back}});
        }
        default:
            return opFail("NO_ANSWER", "the device did not answer the login (tried twice); that says nothing about the password",
                          {{"stage", (int)openknxFileTransferClient.authStage()}});
    }
}

/** @brief A path FTC can carry: not empty, no whitespace (the FTC command line splits on it). */
inline bool ftcPathOk(const std::string& p)
{
    if (p.empty() || p.size() > 200) return false;
    for (char c : p)
        if (std::isspace((unsigned char)c) || c == '\0') return false;
    return true;
}

inline json opFtcSend(WorkerApi& w, const Job& job)
{
    const std::string src = job.args.value("src", std::string()), dst = job.args.value("dst", std::string());
    const std::string mode = job.args.value("mode", std::string("safe"));
    if (!ftcPathOk(src) || !ftcPathOk(dst)) return opFail("INVALID_ARGUMENT", "src and dst are paths without spaces");
    std::ifstream probe(src, std::ios::binary);
    if (!probe) return opFail("INVALID_ARGUMENT", "src is not a readable file on this machine", {{"src", src}});
    Job j = job;
    j.args = {{"pa", job.args.value("pa", std::string())}, {"argv", {"send", src, dst, mode}}};
    return opFtc(w, j);
}

inline json opFtcGet(WorkerApi& w, const Job& job)
{
    const std::string src = job.args.value("src", std::string()), dst = job.args.value("dst", std::string());
    if (!ftcPathOk(src) || !ftcPathOk(dst)) return opFail("INVALID_ARGUMENT", "src and dst are paths without spaces");
    Job j = job;
    j.args = {{"pa", job.args.value("pa", std::string())}, {"argv", {"get", src, dst}}};
    return opFtc(w, j);
}

/** @brief Dispatch a job to its operation; an unknown name is refused, never guessed. */
inline json runWorkerOp(WorkerApi& w, const Job& job)
{
    if (job.op == "ping") return opPing(w, job);
    if (job.op == "ftc") return opFtc(w, job);
    if (job.op == "prop_read") return opPropRead(w, job);
    if (job.op == "device_mreset") return opMReset(w, job);
    if (job.op == "busprop_read") return opBusPropRead(w, job);
    if (job.op == "ga_read") return opGaRead(w, job);
    if (job.op == "device_info") return opDeviceInfo(w, job);
    if (job.op == "ga_write") return opGaWrite(w, job);
    if (job.op == "ga_ask") return opGaAsk(w, job);
    if (job.op == "restart") return opRestart(w, job);
    if (job.op == "runstate") return opRunState(w, job);
    if (job.op == "setpa") return opSetPa(w, job);
    if (job.op == "ftc_login") return opFtcLogin(w, job);
    if (job.op == "ftc_send") return opFtcSend(w, job);
    if (job.op == "ftc_get") return opFtcGet(w, job);
    return opFail("INVALID_ARGUMENT", "the worker does not know this operation", {{"op", job.op}});
}

} // namespace serve
} // namespace ftc

/**
 * @file        LeaderCompare.h
 * @brief       The `compare` operation of `oknx serve`: the compare engine of the command line, fed with the
 *              telegram streams of two workers instead of two own tunnels.
 * @details     Both sides are readers on shared monitors. A dropped connection becomes a gap at the moment it is
 *              reported; telegrams lost on the way (pipe or reader queue) become a gap over their own time span,
 *              so the other side's telegrams from then count as not comparable, never as onlyA/onlyB.
 * @date        2026-10-19
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include "cli/Compare.h"
#include "serve/JsonGet.h"
#include "serve/FrameJson.h"
#include "serve/LeaderApi.h"
#include "serve/Paths.h"

#include <atomic>
#include <csignal>
#include <thread>

namespace ftc
{
namespace serve
{

/** @brief Split a `compare -q` line into JSON: word + numbers + yes/no/on/off fields. */
inline json compareLineToJson(const std::string& line)
{
    json j = json::object();
    size_t pos = 0;
    bool first = true;
    while (pos <= line.size())
    {
        size_t tab = line.find_first_of("\t\n", pos);
        if (tab == std::string::npos) tab = line.size();
        const std::string tok = line.substr(pos, tab - pos);
        pos = tab + 1;
        if (tok.empty())
        {
            if (tab >= line.size()) break;
            continue;
        }
        if (first)
        {
            j["word"] = tok;
            first = false;
            continue;
        }
        const size_t eq = tok.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = tok.substr(0, eq), v = tok.substr(eq + 1);
        if (!v.empty() && v.find_first_not_of("0123456789") == std::string::npos) j[k] = std::stoull(v);
        else
            j[k] = v;
    }
    return j;
}

/** @brief Feed one side of the engine from a reader until @p stop; ends with the reader. */
inline void feedSide(const std::shared_ptr<Reader>& r, Compare::FedSide& side, std::atomic<bool>& stop)
{
    uint64_t lastMs = monoMs();
    while (!stop)
    {
        bool closed = false;
        std::string code;
        auto evs = r->take(500, 100, closed, code);
        for (const auto& e : evs)
        try
        {
            const std::string t = jget(e, "t", "");
            Compare::FedEvent fe;
            if (t == "frame")
            {
                fe.kind = Compare::FedEvent::Frame;
                fe.f = frameFromJson(jget<json>(e, "f", json::object()));
                lastMs = fe.f.ms;
            }
            else if (t == "lost")
            {
                fe.kind = Compare::FedEvent::Lost;
                // the pipe tells the span of what it dropped; a reader overflow only that it did
                fe.t0 = jget(e, "t0", lastMs);
                fe.t1 = jget(e, "t1", monoMs());
            }
            else if (t == "mon")
            {
                const std::string ev = jget(e, "ev", "");
                if (ev == "up")
                {
                    fe.kind = Compare::FedEvent::Up;
                    fe.pa = jget(e, "pa", (uint16_t)0);
                }
                else if (ev == "down" || ev == "end" || ev == "fail")
                    fe.kind = Compare::FedEvent::Down;
                else
                    continue;
            }
            else
                continue;
            side.push(std::move(fe));
        }
        catch (const std::exception&) // an event of an unexpected shape costs that event, never serve
        {
        }
        if (closed)
        {
            Compare::FedEvent fe;
            fe.kind = Compare::FedEvent::Down;
            side.push(fe);
            return;
        }
    }
}

/** @brief Run `compare` on @p a (ip, ref, mode, seconds, grace_ms). */
inline json runCompareOp(LeaderApi& L, const json& a, OpRec& op)
{
    const std::string ip = a.value("ip", std::string()), ref = a.value("ref", std::string());
    const uint16_t port = (uint16_t)a.value("port", 3671);
    const uint16_t refPort = (uint16_t)a.value("ref_port", (int)port);
    const std::string mode = a.value("mode", std::string("gm"));
    const int secs = a.value("seconds", 30);
    const int grace = a.value("grace_ms", 750);
    if (ip == ref && port == refPort) return errorEnvelope("INVALID_ARGUMENT", "ip and ref must be two different interfaces");

    const std::string cap = serveDir(true) + PATH_SEP + "compare-" + op.id + "-" + mode + ".xml";
    json err;
    auto ra = L.openReader(ip, port, mode, false, "op:" + op.id, err);
    if (!ra) return err;
    auto rb = L.openReader(ref, refPort, mode, false, "op:" + op.id, err);
    if (!rb)
    {
        L.closeReader(ip, port, mode, ra);
        return err;
    }
    Compare::FedSide sa, sb;
    // both monitors are up (the readers would not exist otherwise); the engine still starts B as "not yet seen"
    Compare::FedEvent up;
    up.kind = Compare::FedEvent::Up;
    sa.push(up);
    sb.push(up);
    // everything the threads touch lives before them, so it outlives the joins below
    const Monitor::Mode mm = mode == "bm" ? Monitor::Mode::Bus : Monitor::Mode::Group;
    std::atomic<bool> stop{false}, running{true};
    volatile std::sig_atomic_t abortFlag = 0;
    std::thread fa([&] { feedSide(ra, sa, stop); });
    std::thread fb([&] { feedSide(rb, sb, stop); });
    std::thread prog([&] {
        const uint64_t t0 = monoMs();
        while (running)
        {
            for (int i = 0; i < 50 && running; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (*op.cancel) abortFlag = 1;
            if (running) op.addEvent({{"elapsed_s", (monoMs() - t0) / 1000}, {"seconds", secs}});
        }
    });
    int rc = 0;
    json data;
    bool failed = false;
    std::string fail;
    try
    {
        Term term;
        Theme theme(term);
        I18n i18n;
        if (L.lang() == "de") i18n.detect("de");
        Tpl tpl(term, theme, &i18n);
        Compare cmp(term, theme, tpl, i18n);
        cmp.fed(&sa, &sb);
        cmp.xmlOut(cap);
        rc = cmp.run(mm, ip, port, ref, refPort, true, false, 0, secs, grace, true, mm == Monitor::Mode::Bus, false, false, true,
                     false, &abortFlag);
        data = compareLineToJson(cmp.resultLine(mm));
    }
    catch (const std::exception& e)
    {
        failed = true;
        fail = e.what();
    }
    catch (...)
    {
        failed = true;
        fail = "unknown exception";
    }
    // every way out joins the threads (a joinable thread at return would end serve) and closes both readers
    running = false;
    prog.join();
    stop = true;
    fa.join();
    fb.join();
    L.closeReader(ip, port, mode, ra);
    L.closeReader(ref, refPort, mode, rb);
    if (failed) return errorEnvelope("INTERNAL", "compare failed: " + fail, {{"capture", cap}});
    data["capture"] = cap;
    data["rc"] = rc;
    if (rc == 130) return errorEnvelope("CANCELLED", "cancelled", data);
    return {{"ok", true}, {"data", data}};
}

} // namespace serve
} // namespace ftc

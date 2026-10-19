/**
 * @file        Rules.h
 * @brief       The rules file of `oknx serve` (~/.config/oknx/rules.json): which interface allows which classes,
 *              which targets a write may reach, and where the bench lock file lies.
 * @details     A site groups interfaces (the bench, the real installation). A bench site may name the bench
 *              config of the test suite and takes `safety.allowedLines`, `rig.protectedTargets` and
 *              `rig.powerRelayWrite` from it. Without a rules file, and for every interface it does not list,
 *              only passive, net and device-http are allowed; every writing class is refused.
 *
 *              Format (version 1):
 *              { "version": 1, "rate_per_minute": 60,
 *                "sites": [ { "name": "bench", "bench_config": "<path>", "lock_file": "<path>",
 *                             "write_lines": ["1.1"], "setpa_lines": ["1.1"],
 *                             "ga_write": { "allow": ["*"], "deny": ["0/3/100"] },
 *                             "protected": [ { "pa": "1.1.3", "ops": [] } ],
 *                             "interfaces": [ { "ip": "11.11.0.210", "port": 3671, "line": "1.1",
 *                                               "classes": ["passive", "bus-read", …] } ] } ] }
 * @date        2026-10-19
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include "serve/Ops.h"

#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace ftc
{
namespace serve
{

/** @brief Pack "x/y/z" into a 16-bit group address; false when it is not one. */
inline bool parseGa3(const std::string& s, uint16_t& ga)
{
    unsigned a = 0, b = 0, c = 0;
    char tail = 0;
    if (std::sscanf(s.c_str(), "%u/%u/%u%c", &a, &b, &c, &tail) != 3 || a > 31 || b > 7 || c > 255) return false;
    ga = (uint16_t)((a << 11) | (b << 8) | c);
    return true;
}

/** @brief A group address pattern of the rules: `x/y/z`, `x/y/z-n`, or a trailing star on `x/y/`, `x/` or alone. */
inline bool gaPatternMatches(const std::string& pat, uint16_t ga)
{
    const unsigned a = (ga >> 11) & 0x1F, b = (ga >> 8) & 0x07, c = ga & 0xFF;
    unsigned pa = 0, pb = 0, pc = 0, pd = 0;
    char tail = 0;
    if (pat == "*") return true;
    if (std::sscanf(pat.c_str(), "%u/%u/%u-%u%c", &pa, &pb, &pc, &pd, &tail) == 4) return a == pa && b == pb && c >= pc && c <= pd;
    if (std::sscanf(pat.c_str(), "%u/%u/%u%c", &pa, &pb, &pc, &tail) == 3) return a == pa && b == pb && c == pc;
    if (pat.size() >= 2 && pat.back() == '*')
    {
        const std::string head = pat.substr(0, pat.size() - 1);
        if (std::sscanf(head.c_str(), "%u/%u/%c", &pa, &pb, &tail) == 2 && head.back() == '/') return a == pa && b == pb;
        if (std::sscanf(head.c_str(), "%u/%c", &pa, &tail) == 1 && head.back() == '/') return a == pa;
    }
    return false;
}

/** @brief "a.l" of an individual address. */
inline std::string lineOf(uint16_t pa)
{
    return std::to_string((pa >> 12) & 0x0F) + "." + std::to_string((pa >> 8) & 0x0F);
}

inline std::vector<std::string> splitList(const std::string& s)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : s)
    {
        if (c == ',' || c == ' ' || c == ';')
        {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        }
        else
            cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

struct RuleIface
{
    std::string ip;
    uint16_t port = 3671;
    std::string line; ///< the TP line it couples to (setpa rule)
    std::set<OpClass> classes;
    std::string site;
};

struct RuleSite
{
    std::string name;
    std::string benchConfig;
    std::string lockFile;
    std::vector<std::string> writeLines;
    std::vector<std::string> setpaLines;
    std::vector<std::string> gaAllow, gaDeny;
    struct Protected
    {
        std::string pa;
        std::set<std::string> ops;
    };
    std::vector<Protected> protectedTargets;
    std::vector<RuleIface> ifaces;
    bool suiteRun = false; ///< `suite_run` may start Run-Tests.ps1 next to this site's bench_config
};

class Rules
{
  public:
    /** @brief Load @p path; a missing file is "no rules" (not an error), a broken one is an error. */
    bool load(const std::string& path, std::string& err)
    {
        try
        {
            if (loadFile(path, err)) return true;
        }
        catch (const json::exception& e) // a member of another type ("port": "3671"): refuse, never guess a default
        {
            fail(err, std::string("rules: a value has the wrong type (") + e.what() + ")");
        }
        _loaded = false;
        _sites.clear();
        return false;
    }

    bool loaded() const { return _loaded; }
    const std::string& path() const { return _path; }
    int ratePerMinute() const { return _ratePerMinute; }
    const std::vector<RuleSite>& sites() const { return _sites; }

  private:
    bool loadFile(const std::string& path, std::string& err)
    {
        _path = path;
        _loaded = false;
        _sites.clear();
        std::ifstream f(path, std::ios::binary);
        if (!f) return true; // no rules: defaults only
        std::stringstream ss;
        ss << f.rdbuf();
        json j = json::parse(ss.str(), nullptr, false);
        if (!j.is_object()) return fail(err, "the rules file is not valid JSON");
        if (j.value("version", 0) != 1) return fail(err, "rules: version must be 1");
        _ratePerMinute = j.value("rate_per_minute", 60);
        if (_ratePerMinute < 1 || _ratePerMinute > 6000) return fail(err, "rules: rate_per_minute must be 1..6000");
        if (!j.contains("sites") || !j["sites"].is_array()) return fail(err, "rules: sites must be a list");
        std::set<std::string> seen;
        for (const auto& sj : j["sites"])
        {
            if (!sj.is_object()) return fail(err, "rules: every site is an object");
            RuleSite s;
            s.name = sj.value("name", std::string());
            if (s.name.empty()) return fail(err, "rules: a site needs a name");
            s.benchConfig = sj.value("bench_config", std::string());
            s.lockFile = sj.value("lock_file", std::string());
            s.suiteRun = sj.value("suite_run", false);
            if (s.suiteRun && s.benchConfig.empty()) return fail(err, "rules: suite_run needs bench_config in the same site");
            if (!readStrings(sj, "write_lines", s.writeLines, err) || !readStrings(sj, "setpa_lines", s.setpaLines, err))
                return false;
            if (sj.contains("ga_write"))
            {
                const json& g = sj["ga_write"];
                if (!g.is_object() || !readStrings(g, "allow", s.gaAllow, err) || !readStrings(g, "deny", s.gaDeny, err))
                    return fail(err, "rules: ga_write needs allow and deny lists");
            }
            if (sj.contains("protected"))
            {
                if (!sj["protected"].is_array()) return fail(err, "rules: protected must be a list");
                for (const auto& p : sj["protected"])
                {
                    RuleSite::Protected pr;
                    pr.pa = p.value("pa", std::string());
                    if (!isKnxPa(pr.pa)) return fail(err, "rules: protected needs an individual address");
                    for (const auto& o : p.value("ops", json::array())) pr.ops.insert(o.get<std::string>());
                    s.protectedTargets.push_back(pr);
                }
            }
            if (!s.benchConfig.empty() && !mergeBenchConfig(s, err)) return false;
            if (!sj.contains("interfaces") || !sj["interfaces"].is_array()) return fail(err, "rules: a site needs interfaces");
            for (const auto& ij : sj["interfaces"])
            {
                if (!ij.is_object()) return fail(err, "rules: every interface is an object");
                RuleIface ri;
                ri.ip = ij.value("ip", std::string());
                const int port = ij.value("port", 3671);
                if (port < 1 || port > 65535) return fail(err, "rules: interface port must be 1..65535");
                ri.port = (uint16_t)port;
                ri.line = ij.value("line", std::string());
                ri.site = s.name;
                if (!isIpv4(ri.ip)) return fail(err, "rules: interface ip must be IPv4: " + ri.ip);
                const std::string key = ri.ip + ":" + std::to_string(ri.port);
                if (!seen.insert(key).second) return fail(err, "rules: interface listed twice: " + key);
                if (ij.contains("classes"))
                {
                    for (const auto& c : ij["classes"])
                    {
                        OpClass k;
                        if (!c.is_string() || !opClassFromName(c.get<std::string>(), k))
                            return fail(err, "rules: unknown class for " + ri.ip);
                        ri.classes.insert(k);
                    }
                }
                else // a listed interface without classes: the default for a live installation
                    ri.classes = {OpClass::Passive, OpClass::Net, OpClass::DeviceHttp, OpClass::BusRead, OpClass::GroupMon};
                s.ifaces.push_back(ri);
            }
            _sites.push_back(s);
        }
        _loaded = true;
        return true;
    }

  public:

    const RuleIface* iface(const std::string& ip, uint16_t port) const
    {
        for (const auto& s : _sites)
            for (const auto& i : s.ifaces)
                if (i.ip == ip && i.port == port) return &i;
        return nullptr;
    }
    const RuleSite* site(const std::string& name) const
    {
        for (const auto& s : _sites)
            if (s.name == name) return &s;
        return nullptr;
    }

    /** @brief Classes an interface allows; the unlisted default is passive, net and device-http. */
    std::set<OpClass> classesOf(const std::string& ip, uint16_t port) const
    {
        const RuleIface* i = iface(ip, port);
        if (i) return i->classes;
        return {OpClass::Passive, OpClass::Net, OpClass::DeviceHttp};
    }

    /**
     * @brief Guard steps 4 and 5 for one operation on one interface: is the class allowed, is the target allowed.
     * @return empty when allowed, else the rule that refused it (for GUARD_DENIED detail.rule).
     */
    std::string check(const OpDesc& d, const json& args, const std::string& ip, uint16_t port) const
    {
        const OpClass c = classFor(d, args);
        const std::set<OpClass> allowed = classesOf(ip, port);
        if (!allowed.count(c))
            return std::string("class ") + opClassName(c) + " is not allowed on " + ip +
                   (iface(ip, port) ? "" : " (not listed in the rules)");
        if (c != OpClass::BusWrite && c != OpClass::DeviceWrite) return std::string();
        const RuleIface* ri = iface(ip, port);
        if (!ri) return "no writing class without a rules entry";
        const RuleSite* s = site(ri->site);
        // Individual addresses: only in the lines of the rules, never a protected one without its own rule.
        if (args.contains("pa"))
        {
            const std::string paS = args.value("pa", std::string());
            if (!isKnxPa(paS)) return "pa is not an individual address";
            unsigned a, l, dd;
            std::sscanf(paS.c_str(), "%u.%u.%u", &a, &l, &dd);
            const std::string ln = std::to_string(a) + "." + std::to_string(l);
            if (!contains(s->writeLines, ln)) return "line " + ln + " is not a write line of site " + s->name;
            for (const auto& p : s->protectedTargets)
                if (p.pa == paS && !p.ops.count(d.name)) return "target " + paS + " is protected";
        }
        // Group addresses: in the allow list, never in the deny list.
        if (args.contains("ga") || args.contains("items"))
        {
            std::vector<std::string> gas;
            if (args.contains("ga") && args["ga"].is_array())
                for (const auto& g : args["ga"]) gas.push_back(g.get<std::string>());
            if (args.contains("items") && args["items"].is_array())
                for (const auto& it : args["items"]) gas.push_back(it.value("ga", std::string()));
            for (std::string g : gas)
            {
                const size_t colon = g.find(':');
                if (colon != std::string::npos) g = g.substr(0, colon);
                uint16_t ga = 0;
                if (!parseGa3(g, ga)) return "a write names one group address each, not " + g;
                for (const auto& p : s->gaDeny)
                    if (gaPatternMatches(p, ga)) return "group address " + g + " is denied (" + p + ")";
                bool ok = false;
                for (const auto& p : s->gaAllow)
                    if (gaPatternMatches(p, ga)) ok = true;
                if (!ok) return "group address " + g + " is not in the allow list of site " + s->name;
            }
        }
        if (d.name == "setpa" && (ri->line.empty() || !contains(s->setpaLines, ri->line)))
            return "setpa is not allowed through " + ip;
        return std::string();
    }

  private:
    static bool fail(std::string& err, const std::string& why)
    {
        err = why;
        return false;
    }
    static bool contains(const std::vector<std::string>& v, const std::string& s)
    {
        for (const auto& x : v)
            if (x == s) return true;
        return false;
    }
    static bool readStrings(const json& j, const char* key, std::vector<std::string>& out, std::string& err)
    {
        if (!j.contains(key)) return true;
        if (!j[key].is_array()) return fail(err, std::string("rules: ") + key + " must be a list");
        for (const auto& x : j[key])
        {
            if (!x.is_string()) return fail(err, std::string("rules: ") + key + " holds strings");
            out.push_back(x.get<std::string>());
        }
        return true;
    }
    /** @brief Take allowedLines, protectedTargets and powerRelayWrite from the suite's bench config. */
    static bool mergeBenchConfig(RuleSite& s, std::string& err)
    {
        std::ifstream f(s.benchConfig, std::ios::binary);
        if (!f) return fail(err, "rules: bench_config not readable: " + s.benchConfig);
        std::stringstream ss;
        ss << f.rdbuf();
        json c = json::parse(ss.str(), nullptr, false);
        if (!c.is_object()) return fail(err, "rules: bench_config is not valid JSON");
        if (c.contains("safety") && c["safety"].contains("allowedLines"))
            for (const auto& l : c["safety"]["allowedLines"])
                if (l.is_string() && !contains(s.writeLines, l.get<std::string>())) s.writeLines.push_back(l.get<std::string>());
        if (c.contains("rig"))
        {
            for (const auto& pa : splitList(c["rig"].value("protectedTargets", std::string())))
            {
                bool have = false;
                for (const auto& p : s.protectedTargets)
                    if (p.pa == pa) have = true;
                if (!have) s.protectedTargets.push_back({pa, {}});
            }
            for (const auto& g : splitList(c["rig"].value("powerRelayWrite", std::string())))
                if (!contains(s.gaDeny, g)) s.gaDeny.push_back(g);
        }
        return true;
    }

    std::string _path;
    bool _loaded = false;
    int _ratePerMinute = 60;
    std::vector<RuleSite> _sites;
};

} // namespace serve
} // namespace ftc

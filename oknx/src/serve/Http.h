/**
 * @file        Http.h
 * @brief       The access rules every request to `oknx serve` passes: host, origin, key or session.
 * @details     Only 127.0.0.1 / ::1 / localhost with serve's own port are accepted as Host. A foreign Origin and
 *              `Sec-Fetch-Site: cross-site` get 403 even with a key. Programs send the key in X-Oknx-Key; a
 *              browser carries a session cookie, accepted only with `Sec-Fetch-Site: same-origin` (and `none`
 *              for opening /ui itself). No path sends Access-Control-Allow-Origin.
 * @date        2026-10-19
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include "serve/Key.h"
#include "serve/HttpLib.h"
#include "third_party/json.hpp"

#include <cctype>
#include <exception>
#include <functional>
#include <string>

namespace ftc
{
namespace serve
{

/** @brief Who a request is, once its access was checked. */
struct Access
{
    enum Kind
    {
        None,    ///< no key, no session: health without details, /ui login page only
        Key,     ///< the program key (CLI, oknx mcp, Claude, scripts)
        Session, ///< a browser session from `oknx serve open`
    } kind = None;
    std::string id;     ///< "key" or "session:<first 8 of the id>" -- the rate limit counts per id
    std::string caller; ///< free text a client names itself with; logged only
};

inline std::string lower(std::string s)
{
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

/** @brief Host must be loopback with our port: 127.0.0.1:P, [::1]:P or localhost:P. */
inline bool hostAllowed(const std::string& host, int port)
{
    const std::string h = lower(host);
    const std::string p = ":" + std::to_string(port);
    return h == "127.0.0.1" + p || h == "[::1]" + p || h == "localhost" + p;
}

/** @brief Origin, when present, must be serve itself. */
inline bool originAllowed(const std::string& origin, int port)
{
    if (origin.empty()) return true;
    const std::string o = lower(origin);
    const std::string p = ":" + std::to_string(port);
    return o == "http://127.0.0.1" + p || o == "http://[::1]" + p || o == "http://localhost" + p;
}

/** @brief The value of cookie @p name in a Cookie header, or empty. */
inline std::string cookieValue(const std::string& header, const std::string& name)
{
    size_t pos = 0;
    while (pos < header.size())
    {
        while (pos < header.size() && (header[pos] == ' ' || header[pos] == ';')) ++pos;
        const size_t eq = header.find('=', pos);
        if (eq == std::string::npos) break;
        const size_t end = header.find(';', eq);
        const std::string k = header.substr(pos, eq - pos);
        const std::string v = header.substr(eq + 1, end == std::string::npos ? std::string::npos : end - eq - 1);
        if (k == name) return v;
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return std::string();
}

/** @brief Headers every answer carries. */
inline void baseHeaders(httplib::Response& res)
{
    res.set_header("Cache-Control", "no-store");
    res.set_header("X-Content-Type-Options", "nosniff");
    res.set_header("Referrer-Policy", "no-referrer");
}

inline void sendJson(httplib::Response& res, int status, const nlohmann::json& j)
{
    baseHeaders(res);
    res.status = status;
    res.set_content(j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), "application/json");
}

/** @brief The envelope of a refused request: {v, ok:false, error:{code, reason, detail}}. */
inline nlohmann::json errorEnvelope(const std::string& code, const std::string& reason,
                                    const nlohmann::json& detail = nlohmann::json::object())
{
    return nlohmann::json{{"v", 1}, {"ok", false}, {"error", {{"code", code}, {"reason", reason}, {"detail", detail}}}};
}

/**
 * @brief A chunked content provider that ends its stream on an exception: httplib calls it outside the catch it
 *        keeps around the handlers, where a throw would end serve.
 */
template <class F>
std::function<bool(size_t, httplib::DataSink&)> guardedProvider(F f)
{
    return [f](size_t offset, httplib::DataSink& sink) mutable -> bool {
        try
        {
            return f(offset, sink);
        }
        catch (const std::exception&)
        {
            return false;
        }
    };
}

} // namespace serve
} // namespace ftc

/**
 * @file        Ui.h
 * @brief       The pages under /ui of `oknx serve`: overview, API and rules (UiPage.h), the busmonitor viewer, and the
 *              login notices.
 * @details     Pages are served only with a session from `oknx serve open`. The viewer is the suite's
 *              BusmonViewer.html, read from disk next to the bench config; serve points its bridge field at its own
 *              origin, so /discover and /stream go to serve with the session cookie instead of to the bridge.
 * @date        2026-10-19
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include "serve/UiPage.h"

#include <fstream>
#include <sstream>
#include <string>

namespace ftc
{
namespace serve
{

/** @brief Replace every @p from in @p s with @p to. */
inline std::string uiReplaceAll(std::string s, const std::string& from, const std::string& to)
{
    for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size())) s.replace(at, from.size(), to);
    return s;
}

/** @brief A short notice page in the look of the /ui/ page: the black bar with the logo, one message. */
inline std::string uiNotice(const std::string& lang, const std::string& message, bool error)
{
    return "<!doctype html><html lang=\"" + lang +
           "\"><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><title>oknx serve</title><style>"
           "*{box-sizing:border-box;margin:0;padding:0}body{font-family:sans-serif;background:#fff;color:#111}"
           "nav{background:#000;padding:0 2em;box-shadow:0 3px 4px rgba(0,0,0,.18)}nav svg{height:44px;width:auto;padding:8px 0;display:block}"
           "main{padding:2em}h1{font-size:1.25em;font-weight:600;margin-bottom:1em}"
           "p{max-width:70ch;line-height:1.5;padding:10px 14px;border:1px solid #e6e6e6;border-left:4px solid " +
           std::string(error ? "#c62828" : "#449841") +
           "}code{font-family:monospace;background:#f3f3f3;padding:1px 4px}</style></head><body><nav>" + std::string(uiLogoSvg()) +
           "</nav><main><h1>oknx serve</h1><p>" + message + "</p></main></body></html>";
}

inline std::string uiNoSessionPage(const std::string& lang)
{
    const bool de = lang == "de";
    return uiNotice(lang,
                    de ? "Diese Seite braucht eine Sitzung. Im Terminal: <code>oknx serve open</code>, dann den ausgegebenen Link öffnen "
                         "(60 s gültig, einmal benutzbar)."
                       : "This page needs a session. In a terminal: <code>oknx serve open</code>, then open the link it prints "
                         "(valid 60 s, once).",
                    false);
}

inline std::string uiLoginFailedPage(const std::string& lang)
{
    const bool de = lang == "de";
    return uiNotice(lang,
                    de ? "Der Link ist abgelaufen oder wurde schon benutzt. <code>oknx serve open</code> gibt einen neuen."
                       : "The link expired or was used already. <code>oknx serve open</code> gives a new one.",
                    true);
}

/** @brief The /ui/ page (overview, API, rules) in @p lang. */
inline std::string uiIndexPage(const std::string& lang)
{
    return uiReplaceAll(uiReplaceAll(uiPageHtml(), "__OKNX_LANG__", lang == "de" ? "de" : "en"), "__OKNX_LOGO__", uiLogoSvg());
}

/**
 * @brief The body of a page under /ui (path after "/ui"); empty = not found. @p benchConfig locates the viewer.
 */
inline std::string uiAssetFrom(const std::string& path, const std::string& lang, const std::string& benchConfig, std::string& type)
{
    type = "text/html; charset=utf-8";
    if (path.empty() || path == "/" || path == "/index.html") return uiIndexPage(lang);
    if (path == "/viewer" || path == "/viewer/")
    {
        if (benchConfig.empty()) return std::string();
        const size_t cut = benchConfig.find_last_of("/\\");
        const std::string dir = cut == std::string::npos ? std::string(".") : benchConfig.substr(0, cut);
        std::ifstream f(dir + "/viewer/BusmonViewer.html", std::ios::binary);
        if (!f) return std::string();
        std::stringstream ss;
        ss << f.rdbuf();
        std::string html = ss.str();
        // the viewer reads its bridge field when it connects: point it at serve itself
        const std::string inject = "<script>document.addEventListener('DOMContentLoaded',function(){var b=document.getElementById("
                                   "'liveBridge');if(b){b.value=location.origin;b.readOnly=true;b.title='oknx serve';}});</script>";
        const size_t body = html.rfind("</body>");
        if (body == std::string::npos) html += inject;
        else
            html.insert(body, inject);
        return html;
    }
    return std::string();
}

} // namespace serve
} // namespace ftc

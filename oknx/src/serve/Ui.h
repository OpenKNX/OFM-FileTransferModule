/**
 * @file        Ui.h
 * @brief       The pages under /ui of `oknx serve`: overview, the busmonitor viewer, and the login pages.
 * @details     Pages are served only with a session from `oknx serve open`. The viewer is the suite's
 *              BusmonViewer.html, read from disk next to the bench config; serve points its bridge field at its own
 *              origin, so /discover and /stream go to serve with the session cookie instead of to the bridge.
 * @date        2026-10-19
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include <fstream>
#include <sstream>
#include <string>

namespace ftc
{
namespace serve
{

inline std::string uiStyle()
{
    return "<style>:root{--bg:#050805;--fg:#b8f5b0;--dim:#5f8f5a;--hi:#39ff14;--warn:#ffcc33;--err:#ff5555;--line:#1d3a1b}"
           "body{background:var(--bg);color:var(--fg);font:14px/1.45 ui-monospace,Menlo,Consolas,monospace;margin:0;padding:16px}"
           "h1{color:var(--hi);font-size:18px;margin:0 0 12px}h2{color:var(--hi);font-size:15px;margin:20px 0 8px;border-bottom:1px solid var(--line)}"
           "a{color:var(--hi)}table{border-collapse:collapse;width:100%}td,th{border-bottom:1px solid var(--line);padding:4px 8px;text-align:left;vertical-align:top}"
           "th{color:var(--dim);font-weight:normal}.dim{color:var(--dim)}.err{color:var(--err)}.ok{color:var(--hi)}"
           "select,input,button,textarea{background:#0b130a;color:var(--fg);border:1px solid var(--line);font:inherit;padding:4px 6px}"
           "button{cursor:pointer;color:var(--hi)}pre{white-space:pre-wrap;word-break:break-all;background:#0b130a;padding:8px;border:1px solid var(--line);max-height:420px;overflow:auto}"
           "@media(max-width:640px){td,th{padding:3px 4px}body{padding:12px}}</style>";
}

inline std::string uiNoSessionPage(const std::string& lang)
{
    const bool de = lang == "de";
    return "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
           "<title>oknx serve</title>" +
           uiStyle() + "</head><body><h1>oknx serve</h1><p>" +
           (de ? "Diese Seite braucht eine Sitzung. Im Terminal: <code>oknx serve open</code>, dann den ausgegebenen Link öffnen "
                 "(60 s gültig, einmal benutzbar)."
               : "This page needs a session. In a terminal: <code>oknx serve open</code>, then open the link it prints "
                 "(valid 60 s, once).") +
           "</p></body></html>";
}

inline std::string uiLoginFailedPage(const std::string& lang)
{
    const bool de = lang == "de";
    return "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
           "<title>oknx serve</title>" +
           uiStyle() + "</head><body><h1>oknx serve</h1><p class=err>" +
           (de ? "Der Link ist abgelaufen oder wurde schon benutzt. <code>oknx serve open</code> gibt einen neuen."
               : "The link expired or was used already. <code>oknx serve open</code> gives a new one.") +
           "</p></body></html>";
}

inline std::string uiIndexPage(const std::string& lang)
{
    const bool de = lang == "de";
    auto t = [&](const char* en, const char* d) { return std::string(de ? d : en); };
    return "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
           "<title>oknx serve</title>" +
           uiStyle() +
           "</head><body><h1>oknx serve <span class=dim id=ver></span></h1>"
           "<p><a href='/ui/viewer'>" +
           t("Busmonitor viewer", "Busmonitor-Viewer") +
           "</a></p>"
           "<h2>" +
           t("Interfaces", "Interfaces") +
           "</h2><button id=disc>" + t("search", "suchen") +
           "</button><table id=ifs><tr><th>IP</th><th>" + t("name", "Name") + "</th><th>PA</th><th>" + t("kind", "Art") +
           "</th><th>" + t("source", "Quelle") +
           "</th></tr></table>"
           "<h2>" +
           t("Connections and reservations", "Verbindungen und Reservierungen") +
           "</h2><pre id=conns></pre>"
           "<h2>" +
           t("Run an operation", "Operation ausführen") +
           "</h2><select id=op></select> <button id=run>" + t("run", "ausführen") +
           "</button><br><textarea id=args rows=4 style='width:100%;margin-top:6px'>{}</textarea><p class=dim id=help></p><pre id=out></pre>"
           "<h2>" +
           t("Operations", "Operationen") +
           "</h2><table id=ops><tr><th>id</th><th>op</th><th>" + t("state", "Zustand") +
           "</th><th>" + t("access", "Zugang") +
           "</th></tr></table>"
           "<script>"
           "const $=id=>document.getElementById(id);let desc=[];"
           "function esc(s){return String(s).replace(/[&<>\"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;'}[c]));}"
           "async function j(u,o){const r=await fetch(u,o);return r.json();}"
           "async function health(){const h=await j('/api/health');$('ver').textContent=h.version||'';}"
           "async function bench(){const b=await j('/api/ops/bench_state',{method:'POST',headers:{'Content-Type':'application/json'},body:'{}'});"
           "$('conns').textContent=JSON.stringify(b.ok?{connections:b.data.connections,leases:b.data.leases,bench:b.data.bench}:b.error,null,1);"
           "const t=$('ops');t.querySelectorAll('tr.r').forEach(r=>r.remove());if(b.ok)for(const o of b.data.ops.slice(-30).reverse()){const r=t.insertRow();r.className='r';"
           "r.innerHTML='<td>'+esc(o.id)+'</td><td>'+esc(o.op)+'</td><td>'+esc(o.running?'…':o.code)+'</td><td>'+esc(o.access)+'</td>';}}"
           "async function disc(){$('disc').disabled=true;const d=await j('/api/ops/discover',{method:'POST',headers:{'Content-Type':'application/json'},body:'{\"timeout_ms\":2500}'});"
           "$('disc').disabled=false;const t=$('ifs');t.querySelectorAll('tr.r').forEach(r=>r.remove());if(!d.ok){$('out').textContent=JSON.stringify(d,null,1);return;}"
           "for(const i of d.data.interfaces){const r=t.insertRow();r.className='r';r.innerHTML='<td>'+esc(i.ip)+'</td><td>'+esc(i.name)+'</td><td>'+esc(i.pa)+'</td><td>'+esc(i.kind)+'</td><td>'+esc(i.source)+'</td>';}}"
           "async function load(){const d=await j('/api/describe');desc=d.data||[];const s=$('op');for(const o of desc){if(o.changes)continue;const e=document.createElement('option');e.value=o.name;e.textContent=o.name;s.appendChild(e);}showHelp();}"
           "function showHelp(){const o=desc.find(x=>x.name===$('op').value);$('help').textContent=o?o.help+'  —  '+JSON.stringify(o.input.properties):'';}"
           "async function run(){let a;try{a=JSON.parse($('args').value||'{}');}catch(e){$('out').textContent='JSON: '+e;return;}"
           "$('out').textContent='…';const r=await j('/api/ops/'+$('op').value+'?wait=1',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(a)});"
           "$('out').textContent=JSON.stringify(r,null,1);bench();}"
           "$('op').onchange=showHelp;$('run').onclick=run;$('disc').onclick=disc;health();load();bench();setInterval(bench,5000);"
           "</script></body></html>";
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

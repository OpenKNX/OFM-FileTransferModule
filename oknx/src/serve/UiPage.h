/**
 * @file        UiPage.h
 * @brief       The pages of `oknx serve` under /ui/: overview, API and rules, as one HTML document with its style and
 *              script, in the look of the OpenKNX web interface.
 * @details     Served by uiAssetFrom(); __OKNX_LANG__ becomes de or en and __OKNX_LOGO__ the OpenKNX logo. The page
 *              talks to serve's own API with the browser session (same origin), nothing else.
 * @date        2026-10-19
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once

namespace ftc
{
namespace serve
{

/** @brief The OpenKNX logo (white type, green squares) for a black bar. */
inline const char* uiLogoSvg()
{
    return R"OKNXUI(<svg viewBox="0 0 402 242" fill="none" xmlns="http://www.w3.org/2000/svg">
    <path d="M1 50C1 34.7 5.1 22.7 13.3 14C21.6 5.3 32.2 1 45.2 1C53.7 1 61.4 3 68.3 7.1C75.1 11.2 80.3 16.9 83.9 24.2C87.5 31.4 89.3 39.7 89.3 48.9C89.3 58.3 87.4 66.7 83.6 74.1C79.9 81.5 74.5 87.1 67.6 90.9C60.6 94.7 53.2 96.6 45.2 96.6C36.5 96.6 28.7 94.5 21.8 90.3C15 86.1 9.8 80.3 6.3 73.1C2.8 65.8 1 58.1 1 50ZM13.6 50.2C13.6 61.3 16.6 70.1 22.5 76.5C28.5 82.9 36.1 86.1 45.1 86.1C54.3 86.1 61.8 82.9 67.8 76.4C73.7 69.9 76.7 60.8 76.7 48.9C76.7 41.4 75.4 34.8 72.9 29.2C70.4 23.6 66.6 19.2 61.7 16.2C56.8 13.1 51.4 11.5 45.3 11.5C36.6 11.5 29.2 14.5 22.9 20.5C16.7 26.4 13.6 36.3 13.6 50.2Z" fill="white"/>
    <path d="M103.7 121V28.1H114V36.8C116.4 33.4 119.2 30.8 122.3 29.2C125.3 27.4 129 26.6 133.4 26.6C139.1 26.6 144.2 28 148.5 31C152.9 33.9 156.2 38.1 158.4 43.5C160.6 48.8 161.7 54.6 161.7 61C161.7 67.9 160.5 74 158 79.5C155.6 85 152 89.2 147.3 92.1C142.7 95 137.7 96.5 132.6 96.5C128.8 96.5 125.4 95.7 122.4 94.1C119.4 92.5 116.9 90.5 115 88V121H103.7ZM113.9 61.9C113.9 70.5 115.7 76.9 119.2 81C122.7 85.1 126.9 87.2 131.8 87.2C136.9 87.2 141.2 85 144.7 80.8C148.4 76.5 150.2 69.9 150.2 61C150.2 52.4 148.4 46.1 144.9 41.8C141.4 37.6 137.2 35.5 132.3 35.5C127.5 35.5 123.2 37.7 119.5 42.3C115.8 46.8 113.9 53.3 113.9 61.9Z" fill="white"/>
    <path d="M221.3 73.4L233 74.9C231.1 81.7 227.7 87 222.7 90.8C217.7 94.6 211.3 96.5 203.6 96.5C193.8 96.5 186 93.5 180.3 87.5C174.6 81.4 171.7 73 171.7 62.1C171.7 50.8 174.6 42.1 180.4 35.9C186.2 29.7 193.7 26.6 202.9 26.6C211.9 26.6 219.2 29.6 224.9 35.7C230.5 41.8 233.4 50.4 233.4 61.4C233.4 62.1 233.3 63.1 233.3 64.4H183.4C183.8 71.8 185.9 77.4 189.7 81.3C193.4 85.2 198 87.2 203.6 87.2C207.8 87.2 211.3 86.1 214.3 83.9C217.2 81.7 219.5 78.2 221.3 73.4ZM184 55.1H221.4C220.9 49.5 219.5 45.3 217.1 42.4C213.5 38.1 208.8 35.9 203.1 35.9C197.9 35.9 193.5 37.6 189.9 41.1C186.4 44.6 184.4 49.3 184 55.1Z" fill="white"/>
    <path d="M247.3 95V28.1H257.5V37.6C262.4 30.2 269.5 26.6 278.8 26.6C282.8 26.6 286.5 27.3 289.9 28.8C293.3 30.2 295.8 32.1 297.5 34.4C299.2 36.8 300.3 39.6 301 42.8C301.4 44.9 301.6 48.6 301.6 53.8V95H290.3V54.3C290.3 49.7 289.9 46.2 289 44C288.1 41.6 286.5 39.8 284.3 38.5C282 37.1 279.4 36.4 276.4 36.4C271.6 36.4 267.4 37.9 263.9 41C260.4 44.1 258.6 49.9 258.6 58.4V95H247.3Z" fill="white"/>
    <path d="M103.7 242.5V138H124.8V184.4L167.4 138H195.8L156.4 178.7L197.9 242.5H170.6L141.9 193.4L124.8 210.9V242.5H103.7Z" fill="white"/>
    <path d="M209.1 242.5V138H229.6L272.4 207.8V138H292V242.5H270.8L228.7 174.3V242.5H209.1Z" fill="white"/>
    <path d="M303.8 242.5L339.5 187.9L307.1 138H331.8L352.8 171.5L373.3 138H397.7L365.2 188.7L400.9 242.5H375.5L352.3 206.3L329.1 242.5H303.8Z" fill="white"/>
    <line x1="0" y1="116" x2="97.8" y2="116" stroke="#449841" stroke-width="10"/>
    <line x1="120.8" y1="116" x2="403" y2="116" stroke="#449841" stroke-width="10"/>
    <line x1="352.8" y1="99" x2="352.8" y2="111" stroke="#449841" stroke-width="10"/>
    <rect x="318.8" y="28" width="67" height="67" fill="#449841"/>
    <line x1="45.8" y1="133" x2="45.8" y2="121" stroke="#449841" stroke-width="10"/>
    <rect x="79.8" y="205" width="67" height="67" transform="rotate(-180 79.8 205)" fill="#449841"/>
</svg>)OKNXUI";
}

/** @brief The /ui/ page. */
inline const char* uiPageHtml()
{
    return R"OKNXUI(<!doctype html>
<html lang="__OKNX_LANG__"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover"><title>oknx serve</title>
<style>
/* the OpenKNX web interface look: black top bar with the logo, white content, green #449841; light only */
:root{ --green:#449841; --green-hi:#5ab857; --nav:#000; --nav-hi:#1a1a1a; --txt:#111; --dim:#888; --mid:#555;
  --line:#e6e6e6; --line2:#d0d0d0; --soft:#f7f7f7; --ok:#2e7d32; --warn:#8a5a00; --err:#c62828; color-scheme:light }
*{box-sizing:border-box;margin:0;padding:0}
html{-webkit-text-size-adjust:100%;text-size-adjust:100%}
body{display:flex;flex-direction:column;min-height:100%;font-family:sans-serif;background:#fff;color:var(--txt);font-size:15px}
button,input,select{font-family:inherit;font-size:inherit}
nav{position:sticky;top:env(safe-area-inset-top,0px);z-index:4;background:var(--nav);display:flex;align-items:center;flex-wrap:wrap;
  gap:4px 24px;padding:0 2em;box-shadow:0 3px 4px rgba(0,0,0,.18)}
.logo{display:block;height:44px;padding:8px 0}
.logo img{height:100%;width:auto;display:block}
.menu-container{display:flex;flex-wrap:wrap;align-items:stretch;align-self:stretch}
nav a.menu{display:flex;align-items:center;padding:0 16px;color:#aaa;text-decoration:none;font-size:.9em;border-bottom:3px solid transparent;
  border-top:3px solid transparent;transition:all .15s}
nav a.menu:hover{background:var(--nav-hi);color:#fff;border-bottom-color:var(--green)}
nav a.menu.active{color:#fff;border-bottom-color:var(--green);background:#111}
.nav-footer{margin-left:auto;display:flex;align-items:center;gap:6px 18px;flex-wrap:wrap}
.nav-firm{font-weight:bold;font-size:.8em;color:#aaa}
.nav-info{display:flex;align-items:center;color:#888;font-size:.8em;gap:8px}
.prog-dot{display:inline-block;width:8px;height:8px;border-radius:50%;flex-shrink:0;background:var(--green)}
main{flex:1;padding:2em 2em 4.5em;min-width:0;display:flex;flex-direction:column}
.container{max-width:1180px;width:100%}
h1{font-size:1.25em;font-weight:600;margin-bottom:.4em;color:var(--txt)}
.meta{color:#666;font-size:.8em;margin-bottom:1.2em}
.mono{font-family:monospace}
.ota-ok{color:var(--ok)} .ota-warn{color:var(--warn)} .ota-err{color:var(--err)} .gray{color:var(--dim)}
.ctx{position:sticky;top:58px;z-index:2;display:flex;flex-wrap:wrap;gap:4px 26px;background:var(--soft);border:1px solid var(--line);
  padding:8px 12px;margin-bottom:14px;font-size:.86em}
.ctx span{display:flex;align-items:center;gap:7px} .ctx .k{color:var(--dim)} .ctx b{font-weight:600}
.draft{background:#fff8e6;border:1px solid #e8c36a;border-left:4px solid #c98a00;padding:9px 14px;margin:0 0 14px;color:#5a3d00;font-size:.88em;line-height:1.45}
.grid{display:grid;grid-template-columns:minmax(230px,300px) minmax(0,1fr);gap:18px;align-items:start}
.reg{border:1px solid var(--line);margin-bottom:18px;min-width:0}
.reg>h2{font-size:1em;margin:0;padding:9px 14px;border-bottom:1px solid #f0f0f0;display:flex;align-items:center;gap:10px;flex-wrap:wrap}
.reg>h2 .sub{margin-left:auto;font-weight:400;font-size:.84em;color:var(--dim)}
.reg .body{padding:14px}
.sec h3{font-size:.78em;letter-spacing:.08em;text-transform:uppercase;color:var(--green);margin:12px 0 6px;font-weight:600}
.sec:first-child h3{margin-top:4px}
.search{width:100%;padding:6px 8px;border:1px solid var(--line2);margin-bottom:6px}
table{width:100%;border-collapse:collapse;font-size:.88em}
td{padding:5px 8px;border-top:1px solid #eee}
tr:last-child td{border-bottom:1px solid #eee}
tr.pick{cursor:pointer} tr.pick:hover td{background:#f5f5f5}
tr.pick[aria-selected=true] td{background:#eef6ee;font-weight:600}
tr.pick td:first-child{font-family:monospace}
td.cl{color:var(--dim);text-align:right;white-space:nowrap;font-size:.92em}
td.cl.w{color:var(--warn)}
.lead{color:var(--dim);font-size:.88em;line-height:1.45;margin:0 0 12px;max-width:80ch}
.warnbox{display:flex;gap:12px;align-items:flex-start;background:#fff8e6;border:1px solid #e8c36a;border-left:4px solid #c98a00;
  padding:9px 14px;margin:0 0 12px;color:#5a3d00;font-size:.88em;line-height:1.45}
.warnbox b{color:var(--warn)}
form{display:grid;grid-template-columns:repeat(auto-fill,minmax(230px,1fr));gap:10px 16px}
form label{display:flex;flex-direction:column;gap:4px;min-width:0}
form .n{font-family:monospace;font-size:.88em;color:var(--txt)} form .n i{color:var(--err);font-style:normal}
form input,form select{padding:6px 8px;border:1px solid var(--line2);background:#fff;font-family:monospace;font-size:.9em;min-width:0}
form input:focus,form select:focus,.search:focus{outline:2px solid var(--green);outline-offset:0;border-color:var(--green)}
form .d{font-size:.78em;color:var(--dim)}
.gobar{display:flex;align-items:center;gap:14px;flex-wrap:wrap;margin-top:14px}
.go{height:36px;padding:0 22px;border:1px solid var(--green);background:var(--green);color:#fff;font-weight:600;cursor:pointer}
.go:hover{background:var(--green-hi);border-color:var(--green-hi)}
.mini{height:30px;padding:0 12px;border:1px solid var(--line2);background:#fff;cursor:pointer;font-size:.85em}
.mini:hover{border-color:var(--green)}
.why{font-size:.85em;color:var(--mid)}
.fm-tabs{display:flex;gap:4px;margin:0 0 12px;border-bottom:1px solid var(--line2)}
.fm-tab{padding:7px 16px;color:var(--mid);border:1px solid transparent;border-bottom:none;border-radius:4px 4px 0 0;background:none;cursor:pointer;margin-bottom:-1px}
.fm-tab:hover{background:#f0f0f0}
.fm-tab[aria-selected=true]{color:#222;background:#fff;border-color:var(--line2);border-bottom:1px solid #fff;font-weight:600}
.runhead{display:flex;align-items:center;gap:8px 16px;flex-wrap:wrap;margin-bottom:8px;font-size:.86em}
.runhead .t{font-weight:600}
.badge{display:inline-block;padding:1px 8px;border-radius:3px;font-size:.85em;font-weight:600;color:#fff;background:var(--ok)}
.badge.err{background:var(--err)} .badge.run{background:#c98a00}
.bar{height:6px;background:#e0e0e0;margin:4px 0 10px}
.bar i{display:block;height:100%;background:var(--green);width:0;transition:width .25s linear}
.logbox{font-family:monospace;font-size:.8em;color:var(--mid);background:#fafafa;border:1px solid #eee;padding:8px 10px;max-height:150px;overflow:auto;margin-bottom:10px}
pre{font:12.5px/1.5 monospace;background:#fafafa;border:1px solid #eee;padding:10px 12px;overflow:auto;max-height:440px;white-space:pre}
.jk{color:#1f4e8c} .js{color:var(--ok)} .jn{color:var(--warn)} .jb{color:#6a1b9a}
.foot{position:fixed;left:0;right:0;bottom:0;z-index:3;background:#fff;border-top:1px solid var(--line);color:#666;font-size:.8em;
  padding:8px 2em calc(8px + env(safe-area-inset-bottom,0px));display:flex;flex-wrap:wrap;gap:4px 18px}
.foot .r{margin-left:auto;color:var(--dim)}
main{padding-bottom:4.5em}
.view[hidden]{display:none}
.site{border:1px solid var(--line);margin-bottom:16px}
.site>h2{display:flex;align-items:center;gap:10px;padding:8px 14px;border-bottom:1px solid #f0f0f0;font-size:1em}
.site>h2 input{font-weight:600;font-size:1em;border:1px solid transparent;padding:3px 6px;font-family:inherit;min-width:0;width:14em}
.site>h2 input:hover,.site>h2 input:focus{border-color:var(--line2)}
.site>h2 .x{margin-left:auto}
.site .body{padding:12px 14px;display:grid;gap:12px}
.kv{display:grid;grid-template-columns:repeat(auto-fill,minmax(240px,1fr));gap:10px 16px}
.kv label{display:flex;flex-direction:column;gap:4px;min-width:0;font-size:.85em;color:var(--mid)}
.kv input{padding:5px 8px;border:1px solid var(--line2);font-family:monospace;font-size:.9em;min-width:0}
.kv input.bad,.ifs input.bad{border-color:var(--err)}
.chk{flex-direction:row!important;align-items:center;gap:8px!important;color:var(--txt)!important}
.chk input{accent-color:var(--green);width:auto}
.ifs{width:100%;border-collapse:collapse;font-size:.85em}
.ifs th{background:#eee;border-bottom:1px solid #bbb;padding:5px 6px;text-align:left;font-weight:600;white-space:nowrap}
.ifs th.c{text-align:center;font-weight:400;font-size:.88em}
.ifs td{padding:4px 6px;border-top:1px solid #eee}
.ifs td.c{text-align:center}
.ifs td.c input{accent-color:var(--green);width:16px;height:16px}
.ifs input[type=text]{width:100%;padding:4px 6px;border:1px solid var(--line2);font-family:monospace;font-size:.95em;min-width:7em}
.ifs td.port input{min-width:4.5em}
.tablewrap{overflow-x:auto}
.sidecol{position:sticky;top:110px}
.savebar{display:flex;gap:10px;align-items:center;flex-wrap:wrap;margin-top:10px}
.eff{font-size:.78em;color:var(--dim)}
@media (max-width:640px){.foot{padding-inline:16px}}
button:focus-visible,tr.pick:focus-visible{outline:2px solid var(--green);outline-offset:1px}
.edit{display:grid;grid-template-columns:minmax(0,1.2fr) minmax(0,1fr);gap:16px}
.colh{font-size:.78em;letter-spacing:.08em;text-transform:uppercase;color:var(--green);font-weight:600;margin-bottom:8px;display:flex;align-items:center;gap:10px}
.colh .mini{height:24px;padding:0 8px;font-size:.9em;text-transform:none;letter-spacing:0;font-weight:400;color:var(--mid)}
textarea{width:100%;min-height:210px;resize:vertical;font:12.5px/1.5 monospace;padding:8px 10px;border:1px solid var(--line2);background:#fafafa;color:var(--txt);tab-size:2}
textarea:focus{outline:2px solid var(--green);border-color:var(--green)}
textarea.bad{border-color:var(--err);outline-color:var(--err)}
.errs{font-size:.8em;color:var(--err);margin-top:6px;display:grid;gap:2px}
.errs .ok{color:var(--ok)}
form .e{font-size:.78em;color:var(--err)}
form input.bad{border-color:var(--err)}
.rows{display:grid;gap:6px}
.row{display:flex;gap:6px;align-items:center}
.row input{flex:1;min-width:0}
.x{width:26px;height:26px;border:1px solid var(--line2);background:#fff;cursor:pointer;color:var(--mid);flex:0 0 26px}
.x:hover{border-color:var(--err);color:var(--err)}
.add{align-self:start;height:26px;padding:0 10px;border:1px dashed var(--line2);background:#fff;cursor:pointer;color:var(--green);font-size:.85em}
.add:hover{border-color:var(--green)}
.full{grid-column:1/-1}
.grid2{display:grid;grid-template-columns:repeat(auto-fit,minmax(min(100%,520px),1fr));gap:0 18px}
table.list th{background:#eee;border-bottom:1px solid #bbb;padding:6px 10px;text-align:left;font-weight:600;font-size:.95em;white-space:nowrap}
table.list td{padding:6px 10px}
table.list td.mono{white-space:nowrap}
.tag{display:inline-block;font-size:.78em;padding:0 6px;margin:1px 2px 1px 0;border:1px solid #cfe3cf;background:#eef6ee;color:#2e5d2c;border-radius:3px;white-space:nowrap}
.tag.off{border-color:#e6e6e6;background:#f7f7f7;color:#999}
.tag.w{border-color:#e8c36a;background:#fff8e6;color:var(--warn)}
.sel{padding:3px 6px;border:1px solid var(--line2);background:#fff;font-size:.95em}
.go.small{height:28px;padding:0 14px}
.bmwrap{border:1px solid var(--line);max-height:58vh;overflow:auto}
table.bm{font-size:.82em} table.bm td{font-family:monospace;white-space:nowrap;padding:3px 10px}
table.bm td.raw{color:var(--dim)}
table.bm tr.p2p td{background:#f3f8ff}
table.bm tr.nak td.ack{color:var(--err);font-weight:600}
table.bm tr.new td{animation:flash 1.2s ease-out}
@keyframes flash{from{background:#e6f4e6}to{background:transparent}}
@media (prefers-reduced-motion:reduce){table.bm tr.new td{animation:none}}
@media (max-width:1100px){.edit{grid-template-columns:minmax(0,1fr)}}
@media (max-width:900px){.grid{grid-template-columns:minmax(0,1fr)}}
@media (max-width:640px){
  nav{padding:0 16px;position:static}
  .logo{height:38px}
  nav a.menu{padding:10px 12px}
  .nav-footer{margin-left:0;padding-bottom:8px}
  main{padding:1em 16px 5em}
  .ctx{top:0}
}
@media (prefers-reduced-motion:reduce){.bar i,nav a.menu{transition:none}}

.logo svg{height:100%;width:auto;display:block}
.msg{padding:9px 14px;margin:0 0 14px;font-size:.88em;line-height:1.45;border:1px solid var(--line);border-left:4px solid var(--green);background:#f3f9f3}
.msg.err{border-left-color:var(--err);background:#fdf1f1;color:#7a1b1b}
.spin{color:var(--dim);font-size:.85em}
</style></head><body>
<nav>
  <a class="logo" href="#over" aria-label="OpenKNX">__OKNX_LOGO__</a>
  <div class="menu-container">
    <a class="menu" href="#over" id="m-over" data-t="Übersicht|Overview"></a>
    <a class="menu" href="#api" id="m-api">API</a>
    <a class="menu" href="#rules" id="m-rules" data-t="Regeln|Rules"></a>
  </div>
  <div class="nav-footer">
    <span class="nav-firm" id="n-ver">oknx serve</span>
    <span class="nav-info"><span class="prog-dot" id="n-dot"></span><span id="n-state"></span></span>
  </div>
</nav>
<main>
<div class="container view" id="view-over" hidden>
  <h1 data-t="Übersicht|Overview"></h1>
  <div class="meta" data-t="Was serve gerade hält, wer es nutzt und was zuletzt lief.|What serve holds right now, who uses it and what ran last."></div>
  <div class="ctx">
    <span><span class="k">serve</span><b id="o-up"></b></span>
    <span><span class="k">Port</span><b class="mono" id="o-port"></b></span>
    <span><span class="k" data-t="Regeln|Rules"></span><b id="o-rules"></b></span>
    <span><span class="k" data-t="Prüfstand|Bench"></span><b id="o-bench"></b></span>
  </div>
  <div id="o-msg"></div>
  <div class="grid2">
    <section class="reg"><h2>Interfaces <span class="sub"><button class="mini" id="o-scan" type="button" data-t="neu suchen|search again"></button></span></h2>
      <div class="body tablewrap"><table class="list"><thead><tr><th>Interface</th><th data-t="Gerät|Device"></th><th>PA</th><th>Site</th><th data-t="erlaubt|allowed"></th></tr></thead><tbody id="o-ifs"></tbody></table></div></section>
    <section class="reg"><h2><span data-t="Verbindungen von serve|Connections of serve"></span> <span class="sub" id="o-conn-n"></span></h2>
      <div class="body tablewrap"><table class="list"><thead><tr><th>Interface</th><th data-t="Art|Kind"></th><th data-t="Leser|Readers"></th><th data-t="seit|since"></th><th></th></tr></thead><tbody id="o-conns"></tbody></table></div></section>
    <section class="reg"><h2><span data-t="Reservierungen der Kommandozeile|Reservations of the command line"></span> <span class="sub" id="o-lease-n"></span></h2>
      <div class="body tablewrap"><table class="list"><thead><tr><th data-t="Befehl|Command"></th><th>Interface</th><th data-t="Art|Kind"></th><th>PID</th><th data-t="seit|since"></th><th></th></tr></thead><tbody id="o-leases"></tbody></table></div></section>
    <section class="reg"><h2><span data-t="Letzte Operationen|Recent operations"></span> <span class="sub">operations</span></h2>
      <div class="body tablewrap"><table class="list"><thead><tr><th data-t="Zeit|Time"></th><th data-t="Operation|Operation"></th><th data-t="von|by"></th><th data-t="Ergebnis|Result"></th><th data-t="Dauer|Time taken"></th></tr></thead><tbody id="o-ops"></tbody></table></div></section>
  </div>
</div>

<div class="container view" id="view-api" hidden>
  <h1>API</h1>
  <div class="meta" data-t="Jede Operation zum Ausprobieren. Sie geht durch denselben Guard wie MCP: bekannt, gültig, Rate, Klasse, Ziel, Prüfstand, Verbindung.|Every operation to try out. It passes the same guard as MCP: known, valid, rate, class, target, bench, connection."></div>
  <div class="grid">
    <section class="reg"><h2><span data-t="Operationen|Operations"></span> <span class="sub" id="a-count"></span></h2>
      <div class="body"><input class="search" id="a-filter" type="search" autocomplete="off" data-tp="suchen|search"><div id="a-ops"></div></div></section>
    <div>
      <section class="reg"><h2><span class="mono" id="a-name"></span><span class="sub" id="a-cls"></span></h2>
        <div class="body">
          <p class="lead" id="a-help"></p>
          <div id="a-warn"></div>
          <div class="edit">
            <div class="col"><div class="colh" data-t="Formular|Form"></div><form id="a-form" autocomplete="off"></form></div>
            <div class="col"><div class="colh">JSON <button class="mini" id="a-fmt" type="button" data-t="formatieren|format"></button></div>
              <textarea id="a-json" spellcheck="false" aria-label="JSON"></textarea><div class="errs" id="a-errs"></div></div>
          </div>
          <div class="gobar"><button class="go" id="a-send" type="button" data-t="Senden|Send"></button><button class="mini" id="a-copy" type="button" data-t="curl kopieren|copy curl"></button><span class="why" id="a-hint"></span></div>
        </div></section>
      <section class="reg"><div class="body">
        <div class="fm-tabs" role="tablist">
          <button class="fm-tab" role="tab" id="t-ans" aria-selected="true" data-t="Antwort|Answer"></button>
          <button class="fm-tab" role="tab" id="t-req" aria-selected="false">curl</button>
          <button class="fm-tab" role="tab" id="t-sch" aria-selected="false" data-t="Schema|Schema"></button>
        </div>
        <div id="p-ans"><div class="runhead" id="a-status"></div><div class="bar" id="a-prog" hidden><i id="a-progbar"></i></div><div class="logbox" id="a-events" hidden></div><pre id="a-out"></pre></div>
        <div id="p-req" hidden><pre id="a-req"></pre></div>
        <div id="p-sch" hidden><pre id="a-sch"></pre></div>
      </div></section>
    </div>
  </div>
</div>

<div class="container view" id="view-rules" hidden>
  <h1 data-t="Regeln|Rules"></h1>
  <div class="meta" data-t="Was serve auf welchem Interface erlaubt. Ohne Eintrag darf ein Interface nur gesucht und über seine Firmware-Seite gelesen werden.|What serve allows on which interface. Without an entry an interface can only be searched for and read through its firmware page."></div>
  <div class="ctx">
    <span><span class="k" data-t="Datei|File"></span><b class="mono" id="r-path"></b></span>
    <span><span class="k" data-t="geladen|loaded"></span><b id="r-at"></b></span>
    <span><span class="k">Sites</span><b id="r-sites">0</b></span>
    <span><span class="k">Interfaces</span><b id="r-ifs">0</b></span>
  </div>
  <div id="r-msg"></div>
  <div class="grid" style="grid-template-columns:minmax(0,1.5fr) minmax(280px,1fr)">
    <div>
      <div class="kv" style="margin-bottom:14px"><label><span data-t="Aufrufe je Minute (rate_per_minute)|Calls per minute (rate_per_minute)"></span><input id="r-rate" type="text"></label></div>
      <div id="r-list"></div>
      <button type="button" class="add" id="r-addsite">+ Site</button>
    </div>
    <div class="sidecol">
      <section class="reg"><h2>JSON <span class="sub"><button class="mini" id="r-fmt" type="button" data-t="formatieren|format"></button></span></h2>
        <div class="body">
          <textarea id="r-json" spellcheck="false" style="min-height:420px" aria-label="rules.json"></textarea>
          <div class="errs" id="r-errs"></div>
          <div class="savebar"><button class="go" id="r-save" type="button" data-t="Speichern|Save"></button><button class="mini" id="r-reset" type="button" data-t="verwerfen|discard"></button><span class="why" id="r-hint"></span></div>
        </div></section>
    </div>
  </div>
</div>
</main>
<div class="foot"><span>© 2026 by Erkan Çolak · OpenKNX</span><span id="f-ver">oknx serve</span><span class="r mono" id="f-addr"></span></div>
<script>
"use strict";
const LANG = "__OKNX_LANG__";
const T = (de, en) => LANG === "de" ? de : en;
const $ = id => document.getElementById(id);
const esc = s => String(s).replace(/[&<>"]/g, c => ({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]));
document.querySelectorAll("[data-t]").forEach(e => { const p = e.dataset.t.split("|"); e.textContent = T(p[0], p[1]); });
document.querySelectorAll("[data-tp]").forEach(e => { const p = e.dataset.tp.split("|"); e.placeholder = T(p[0], p[1]); });
document.documentElement.lang = LANG;

// --- talking to serve: same origin, the session cookie goes along ------------------------------------------
async function api(method, path, body) {
  const o = { method, credentials: "same-origin", headers: { "X-Oknx-Caller": "ui" } };
  if (body !== undefined) { o.headers["Content-Type"] = "application/json"; o.body = JSON.stringify(body); }
  let r;
  try { r = await fetch(path, o); } catch (e) { return { status: 0, json: { ok: false, error: { code: "INTERNAL", reason: T("serve antwortet nicht", "serve does not answer") } } }; }
  let j = null;
  try { j = await r.json(); } catch (e) { j = { ok: false, error: { code: "INTERNAL", reason: "HTTP " + r.status } }; }
  if (r.status === 401) location.reload(); // the session ended: the server shows how to get a new one
  return { status: r.status, json: j };
}
function colour(j) {
  return esc(JSON.stringify(j, null, 2)).replace(/(&quot;(?:\\.|[^&\\]|&(?!quot;))*?&quot;)(\s*:)?|\b(true|false|null)\b|-?\d+(?:\.\d+)?(?:e[+-]?\d+)?/g, (m, s, colon, b) => {
    if (s) return colon ? '<span class="jk">' + s + '</span>' + colon : '<span class="js">' + s + '</span>';
    if (b) return '<span class="jb">' + m + '</span>';
    return '<span class="jn">' + m + '</span>';
  });
}
const hhmmss = ms => new Date(ms).toLocaleTimeString(LANG === "de" ? "de-DE" : "en-GB", { hour12: false });
const ago = ms => hhmmss(Date.now() - ms);

// the classes of the rules, in the guard's order
const CLASSES = [["passive", T("passiv", "passive"), T("nur serve selbst", "serve itself only")], ["net", T("Netz", "net"), T("suchen", "search")],
  ["device-http", "HTTP", T("Firmware-Seite lesen", "read the firmware page")], ["bus-read", T("lesen", "read"), T("vom Bus lesen", "read from the bus")],
  ["group-monitor", "GM", T("Gruppenmonitor", "group monitor")], ["busmonitor", "BM", T("Busmonitor (sperrt jeden anderen Tunnel)", "busmonitor (blocks every other tunnel)")],
  ["bus-write", T("schreiben", "write"), T("auf den Bus schreiben", "write to the bus")], ["device-write", T("Gerät", "device"), T("Geräteseite ändern", "change the device page")]];
const CLS_NAME = Object.fromEntries(CLASSES.map(c => [c[0], c[1]]));
const DEFAULT_CLASSES = ["passive", "net", "device-http", "bus-read", "group-monitor"];
const IPV4 = /^(25[0-5]|2[0-4]\d|1?\d?\d)(\.(25[0-5]|2[0-4]\d|1?\d?\d)){3}$/;
const PA = /^(1[0-5]|\d)\.(1[0-5]|\d)\.(25[0-5]|2[0-4]\d|1?\d?\d)$/;
const GASEL = /^(\*|\d{1,2}\/\*|\d{1,2}\/\d\/\*|\d{1,2}\/\d\/\d{1,3}(-\d{1,3})?)(:\d{1,3}(\.\d{1,3})?)?$/;
const LINE = /^(1[0-5]|\d)\.(1[0-5]|\d)$/;
const FORMATS = { "ipv4": [IPV4, T("keine IPv4-Adresse", "not an IPv4 address")], "knx-pa": [PA, T("keine physikalische Adresse (z. B. 1.1.8)", "not an individual address (e.g. 1.1.8)")],
  "knx-ga-selector": [GASEL, T("keine Gruppenadresse", "not a group address")] };

// =============================================================================================== overview
let health = null, rulesData = null, found = null;
// what serve enforces right now (its loaded rules), not what the file on disk says
function rulesIfaces() {
  const m = {};
  ((rulesData && rulesData.effective) || []).forEach(s => (s.interfaces || []).forEach(f => { m[f.ip] = { site: s.name, cls: f.classes || [] }; }));
  return m;
}
function renderOverview() {
  if (!health) return;
  const full = health.pid !== undefined; // without the details (no session) nothing below may claim a state
  $("o-up").textContent = T("läuft", "running") + (full ? " · pid " + health.pid : "");
  $("o-up").className = "ota-ok";
  $("o-port").textContent = location.host;
  const b = health.bench;
  $("o-bench").textContent = !b ? "–" : b.valid ? T("belegt von ", "held by ") + (b.run || "?") : T("frei", "free");
  $("o-bench").className = !b ? "gray" : b.valid ? "ota-warn" : "ota-ok";
  const ri = rulesIfaces();
  $("o-rules").textContent = rulesData ? (rulesData.loaded ? (rulesData.effective || []).length + " Sites, " + Object.keys(ri).length + " Interfaces" : T("keine (nur lesen)", "none (read-only)")) : "…";
  const fi = {}; (found || []).forEach(f => { fi[f.ip] = f; });
  const ips = Array.from(new Set(Object.keys(ri).concat(Object.keys(fi)))).sort((a, c) => a.localeCompare(c, undefined, { numeric: true }));
  $("o-ifs").innerHTML = ips.map(ip => {
    const f = fi[ip], r = ri[ip];
    const tags = r ? CLASSES.map(c => '<span class="tag' + (r.cls.includes(c[0]) ? (["bus-write", "device-write", "busmonitor"].includes(c[0]) ? ' w' : '') : ' off') + '" title="' + esc(c[2]) + '">' + esc(c[1]) + '</span>').join("")
                   : '<span class="gray">' + T("nur suchen und lesen", "search and read only") + '</span>';
    const dev = f ? esc(f.name || "") : (found ? '<span class="gray">' + T("nicht gefunden", "not found") + '</span>' : '<span class="spin">…</span>');
    return '<tr><td class="mono">' + esc(ip) + '</td><td>' + dev + '</td><td class="mono">' + esc(f && f.pa || "") + '</td><td>' + (r ? esc(r.site) : '<span class="gray">–</span>') + '</td><td>' + tags + '</td></tr>';
  }).join("") || '<tr><td colspan="5" class="gray">' + T("keine", "none") + '</td></tr>';
  if (!full) { ["o-conns", "o-leases"].forEach(id => { $(id).innerHTML = '<tr><td colspan="6" class="gray">–</td></tr>'; }); $("o-conn-n").textContent = $("o-lease-n").textContent = ""; return; }
  const conns = health.connections || [];
  $("o-conn-n").textContent = conns.length;
  const kindName = k => ({ gm: T("Gruppenmonitor", "group monitor"), bm: T("Busmonitor", "busmonitor"), mgmt: T("Management-Tunnel", "management tunnel") }[k] || k);
  $("o-conns").innerHTML = conns.map(c => '<tr><td class="mono">' + esc(c.iface) + '</td><td>' + esc(kindName(c.kind)) + (c.op ? ' <span class="gray mono">' + esc(c.op) + '</span>' : '') +
      '</td><td>' + (c.readers != null ? c.readers : '–') + '</td><td class="mono">' + (c.since_ms != null ? ago(c.since_ms) : '') + '</td><td>' +
      (c.kind === "gm" || c.kind === "bm" ? '<button class="mini" data-stop="' + esc(c.iface) + '|' + esc(c.kind) + '">' + T("beenden", "stop") + '</button>' : '') + '</td></tr>').join("")
    || '<tr><td colspan="5" class="gray">' + T("keine", "none") + '</td></tr>';
  const leases = health.leases || [];
  $("o-lease-n").textContent = leases.length;
  $("o-leases").innerHTML = leases.map(l => '<tr><td class="mono">' + esc(l.purpose || "") + '</td><td class="mono">' + esc((l.items || []).map(i => i.ip).join(", ")) + '</td><td>' +
      esc((l.items || []).map(i => i.kind === "bm" ? T("exklusiv", "exclusive") : kindName(i.kind)).join(", ")) + '</td><td class="mono">' + esc(l.pid) + '</td><td class="mono">' + ago(l.since_ms) + '</td><td><button class="mini" data-lease="' + esc(l.id) + '">' +
      T("freigeben", "release") + '</button></td></tr>').join("") || '<tr><td colspan="6" class="gray">' + T("keine", "none") + '</td></tr>';
}
async function loadOps() {
  const r = await api("GET", "/api/ops");
  const ops = ((r.json && r.json.data && r.json.data.ops) || []).filter(o => o.wall_ms).sort((a, b) => b.wall_ms - a.wall_ms).slice(0, 12);
  $("o-ops").innerHTML = ops.map(o => {
    const ok = o.code === "OK", run = o.running;
    return '<tr><td class="mono">' + hhmmss(o.wall_ms) + '</td><td class="mono">' + esc(o.op) + '</td><td>' + esc(o.caller || o.access || "") + '</td><td class="' +
      (run ? 'ota-warn' : ok ? 'ota-ok' : 'ota-err') + '">' + esc(run ? T("läuft", "running") : o.code || "") + '</td><td class="mono">' + (o.ms != null ? (o.ms / 1000).toFixed(1) + " s" : "") + '</td></tr>';
  }).join("") || '<tr><td colspan="5" class="gray">' + T("noch keine", "none yet") + '</td></tr>';
}
async function refresh() {
  const h = await api("GET", "/api/health");
  if (h.json && h.json.ok) {
    health = h.json;
    $("n-ver").textContent = "oknx " + health.version + " · serve"; $("f-ver").textContent = "oknx serve " + health.version;
    $("n-state").textContent = T("läuft", "running") + " · " + location.host; $("f-addr").textContent = location.host;
    $("n-dot").style.background = "";
  } else { $("n-state").textContent = T("keine Verbindung", "no connection"); $("n-dot").style.background = "#ff5555"; }
  renderOverview();
  if (!$("view-over").hidden) loadOps();
}
async function scan() {
  $("o-scan").disabled = true; found = null; renderOverview();
  const r = await api("POST", "/api/ops/discover?wait=1", {});
  found = (r.json && r.json.ok && r.json.data && r.json.data.interfaces) || [];
  $("o-scan").disabled = false; renderOverview();
}
function msg(box, text, bad) { $(box).innerHTML = text ? '<div class="msg' + (bad ? ' err' : '') + '">' + esc(text) + '</div>' : ""; }
$("o-scan").onclick = scan;
$("view-over").addEventListener("click", async e => {
  const st = e.target.dataset.stop, ls = e.target.dataset.lease;
  if (st) {
    const [iface, mode] = st.split("|"); const [ip, port] = iface.split(":");
    const r = await api("POST", "/api/ops/monitor_stop?wait=1", { ip, port: Number(port), mode });
    msg("o-msg", r.json.ok ? T("Monitor beendet: ", "monitor stopped: ") + iface : (r.json.error || {}).reason, !r.json.ok); refresh();
  }
  if (ls) {
    const r = await api("DELETE", "/api/lease/" + encodeURIComponent(ls));
    msg("o-msg", r.json && r.json.ok ? T("Reservierung freigegeben", "reservation released") : ((r.json && r.json.error) || {}).reason, !(r.json && r.json.ok)); refresh();
  }
});

// =============================================================================================== API
let OPS = [], cur = null, body = {}, es = null;
const props = () => (cur && cur.input && cur.input.properties) || {};
const req = () => (cur && cur.input && cur.input.required) || [];
function check(b) {
  const errs = [], p = props();
  if (cur.input && cur.input.additionalProperties === false) Object.keys(b).forEach(k => { if (!(k in p)) errs.push({ k, m: T("unbekanntes Argument", "unknown argument") }); });
  req().forEach(k => { if (b[k] === undefined) errs.push({ k, m: T("fehlt", "missing") }); });
  const one = (k, s, v, where) => {
    const at = where || k;
    if (s.type === "integer") {
      if (!Number.isInteger(v)) return errs.push({ k, m: at + ": " + T("ganze Zahl erwartet", "an integer") });
      if ((s.minimum != null && v < s.minimum) || (s.maximum != null && v > s.maximum)) errs.push({ k, m: at + ": " + s.minimum + ".." + s.maximum });
    } else if (s.type === "boolean") { if (typeof v !== "boolean") errs.push({ k, m: at + ": true / false" }); }
    else if (s.type === "string") {
      if (typeof v !== "string") return errs.push({ k, m: at + ": " + T("Text erwartet", "a string") });
      if (s.enum && !s.enum.includes(v)) errs.push({ k, m: at + ": " + s.enum.join(" | ") });
      if (s.format && FORMATS[s.format] && !FORMATS[s.format][0].test(v)) errs.push({ k, m: at + ": " + FORMATS[s.format][1] });
      if (s.minLength != null && v.length < s.minLength) errs.push({ k, m: at + ": " + T("zu kurz", "too short") });
      if (s.maxLength != null && v.length > s.maxLength) errs.push({ k, m: at + ": " + T("zu lang", "too long") });
    } else if (s.type === "array") {
      if (!Array.isArray(v)) return errs.push({ k, m: at + ": " + T("Liste erwartet", "a list") });
      if (s.minItems != null && v.length < s.minItems) errs.push({ k, m: at + ": " + T("mindestens ", "at least ") + s.minItems });
      if (s.maxItems != null && v.length > s.maxItems) errs.push({ k, m: at + ": " + T("höchstens ", "at most ") + s.maxItems });
      v.forEach((it, i) => one(k, s.items || {}, it, k + "[" + (i + 1) + "]"));
    } else if (s.type === "object") {
      if (typeof v !== "object" || v === null || Array.isArray(v)) return errs.push({ k, m: at + ": " + T("Objekt erwartet", "an object") });
      (s.required || []).forEach(rk => { if (v[rk] === undefined || v[rk] === "") errs.push({ k, m: at + "." + rk + ": " + T("fehlt", "missing") }); });
      Object.entries(s.properties || {}).forEach(([pk, ps]) => { if (v[pk] !== undefined && v[pk] !== "") one(k, ps, v[pk], at + "." + pk); });
    }
  };
  Object.entries(p).forEach(([k, s]) => { if (b[k] !== undefined) one(k, s, b[k]); });
  return errs;
}
function initialBody() {
  const b = {}, p = props();
  req().forEach(k => {
    const s = p[k] || {};
    if (s.type === "array") b[k] = s.items && s.items.type === "object" ? [Object.fromEntries(Object.keys(s.items.properties || {}).map(c => [c, ""]))] : [""];
    else if (s.enum) b[k] = s.enum[0];
    else if (s.type === "integer" && s.minimum != null) b[k] = s.minimum;
  });
  return b;
}
function fieldValue(s, raw) {
  if (raw === "") return undefined;
  if (s.type === "integer") return /^-?\d+$/.test(raw) ? Number(raw) : raw;
  if (s.type === "boolean") return raw === "true" ? true : raw === "false" ? false : raw;
  return raw;
}
function renderForm() {
  const form = $("a-form"); form.innerHTML = "";
  const p = props(), r = req();
  if (!Object.keys(p).length) { form.innerHTML = '<span class="gray">' + T("keine Argumente", "no arguments") + '</span>'; return; }
  Object.entries(p).sort((x, y) => (r.includes(y[0]) - r.includes(x[0]))).forEach(([k, s]) => {
    const l = document.createElement("label");
    const head = '<span class="n">' + esc(k) + (r.includes(k) ? ' <i>*</i>' : '') + '</span>';
    const tail = '<span class="d">' + esc(s.description || "") + '</span><span class="e" id="e-' + esc(k) + '"></span>';
    if (s.type === "array") {
      l.className = "full";
      const v = Array.isArray(body[k]) ? body[k] : [];
      const cols = s.items && s.items.type === "object" ? Object.keys(s.items.properties || {}) : null;
      const rows = v.map((it, i) => '<div class="row">' + (cols ? cols.map(c => '<input data-k="' + esc(k) + '" data-i="' + i + '" data-c="' + esc(c) + '" placeholder="' + esc(c) + '" value="' + esc(it && it[c] != null ? it[c] : "") + '">').join("")
        : '<input data-k="' + esc(k) + '" data-i="' + i + '" placeholder="' + esc((s.items && s.items.format) || "") + '" value="' + esc(it) + '">') +
        '<button type="button" class="x" data-del="' + esc(k) + '" data-i="' + i + '" aria-label="' + T("Zeile entfernen", "remove row") + '">×</button></div>').join("");
      l.innerHTML = head + '<div class="rows">' + rows + '<button type="button" class="add" data-add="' + esc(k) + '">+ ' + T("Zeile", "row") + '</button></div>' + tail;
    } else if (s.enum || s.type === "boolean") {
      const opts = s.enum || ["true", "false"], v = body[k] === undefined ? "" : String(body[k]);
      l.innerHTML = head + '<select data-k="' + esc(k) + '"><option value="">–</option>' + opts.map(o => '<option' + (String(o) === v ? ' selected' : '') + '>' + esc(o) + '</option>').join("") + '</select>' + tail;
    } else {
      const v = body[k] === undefined ? "" : body[k];
      const ph = s.type + (s.minimum != null ? " " + s.minimum + ".." + s.maximum : "") + (s.format ? " · " + s.format : "");
      l.innerHTML = head + '<input data-k="' + esc(k) + '" value="' + esc(v) + '" placeholder="' + esc(ph) + '">' + tail;
    }
    form.appendChild(l);
  });
}
function fromForm(ev) {
  const t = ev.target, k = t.dataset.k; if (!k) return;
  const s = props()[k] || {};
  if (s.type === "array") {
    const arr = Array.isArray(body[k]) ? body[k] : (body[k] = []), i = Number(t.dataset.i);
    if (t.dataset.c) { arr[i] = Object.assign({}, arr[i] || {}); arr[i][t.dataset.c] = t.value; }
    else arr[i] = (s.items && s.items.type === "integer") ? fieldValue(s.items, t.value) : t.value;
  } else { const v = fieldValue(s, t.value); if (v === undefined) delete body[k]; else body[k] = v; }
  writeJson(); showErrors();
}
function writeJson() { $("a-json").value = JSON.stringify(body, null, 2); $("a-json").classList.remove("bad"); $("a-req").textContent = curl(); }
function fromJson() {
  let b;
  try { b = JSON.parse($("a-json").value || "{}"); } catch (e) { $("a-json").classList.add("bad"); $("a-errs").innerHTML = '<span>JSON: ' + esc(e.message) + '</span>'; return false; }
  if (typeof b !== "object" || b === null || Array.isArray(b)) { $("a-json").classList.add("bad"); $("a-errs").innerHTML = '<span>' + T("die Anfrage ist ein Objekt { … }", "the request is an object { … }") + '</span>'; return false; }
  $("a-json").classList.remove("bad"); body = b; renderForm(); showErrors(); $("a-req").textContent = curl(); return true;
}
function showErrors() {
  const errs = check(body);
  Object.keys(props()).forEach(k => { const e = $("e-" + k); if (e) e.textContent = errs.filter(x => x.k === k).map(x => x.m).join(" · "); });
  $("a-errs").innerHTML = errs.length ? errs.map(x => '<span>' + esc(x.m.indexOf(x.k) === 0 ? x.m : x.k + ": " + x.m) + '</span>').join("") : '<span class="ok">' + T("passt zum Schema", "matches the schema") + '</span>';
  $("a-send").disabled = errs.length > 0; $("a-send").style.opacity = errs.length ? .45 : 1;
  return errs;
}
// one argument for a POSIX shell, in single quotes ('\'' for a quote inside)
const sq = v => "'" + String(v).replace(/'/g, "'\\''") + "'";
function curl() {
  const key = health && health.key_path ? sq(health.key_path) : '"$HOME/.config/oknx/serve.key"';
  return 'curl -s -H "X-Oknx-Key: $(cat ' + key + ')" -H "Content-Type: application/json" \\\n  -d ' + sq(JSON.stringify(body)) + ' \\\n  "' + location.origin + '/api/ops/' + cur.name + '?wait=1"';
}
function renderList() {
  const q = $("a-filter").value.trim().toLowerCase(), box = $("a-ops"); box.innerHTML = "";
  $("a-count").textContent = OPS.length;
  [[T("Lesen", "Reading"), o => !o.changes], [T("Schreiben", "Writing"), o => o.changes]].forEach(([g, f]) => {
    const items = OPS.filter(f).filter(o => !q || o.name.includes(q) || (o.title || "").toLowerCase().includes(q));
    if (!items.length) return;
    const sec = document.createElement("div"); sec.className = "sec";
    sec.innerHTML = '<h3>' + esc(g) + '</h3><table><tbody></tbody></table>';
    const tb = sec.querySelector("tbody");
    items.forEach(o => {
      const tr = document.createElement("tr"); tr.className = "pick"; tr.tabIndex = 0;
      tr.setAttribute("aria-selected", String(o === cur));
      tr.innerHTML = '<td>' + esc(o.name) + '</td><td class="cl' + (o.changes ? ' w' : '') + '">' + esc(CLS_NAME[o.class] || o.class) + '</td>';
      const pick = () => { cur = o; renderList(); renderOp(); };
      tr.onclick = pick; tr.onkeydown = e => { if (e.key === "Enter" || e.key === " ") { e.preventDefault(); pick(); } };
      tb.appendChild(tr);
    });
    box.appendChild(sec);
  });
}
function renderOp() {
  if (es) { es.close(); es = null; }
  $("a-name").textContent = cur.name;
  $("a-cls").textContent = (CLS_NAME[cur.class] || cur.class) + " · " + (cur.title || "");
  $("a-help").textContent = cur.help || cur.title || "";
  $("a-warn").innerHTML = cur.changes ? '<div class="warnbox"><span><b>' + T("Ändert Bus oder Gerät.", "Changes the bus or a device.") + '</b> ' +
    esc(T("Der Guard lässt die Operation nur durch, wo die Regeln die Klasse " + (CLS_NAME[cur.class] || cur.class) + " für das Interface erlauben.",
          "The guard lets it through only where the rules allow the class " + (CLS_NAME[cur.class] || cur.class) + " on the interface.")) + '</span></div>' : "";
  body = initialBody(); renderForm(); writeJson(); showErrors();
  $("a-sch").innerHTML = colour(cur.input || {});
  $("a-hint").textContent = cur.progress ? T("der Fortschritt kommt über SSE", "progress comes over SSE") : "";
  $("a-status").innerHTML = '<span class="gray">' + T("noch nichts gesendet", "nothing sent yet") + '</span>';
  $("a-out").textContent = ""; $("a-events").innerHTML = ""; $("a-events").hidden = true; $("a-prog").hidden = true;
}
function showResult(env, status) {
  const ok = env && env.ok;
  $("a-status").innerHTML = '<span class="badge' + (ok ? '' : ' err') + '">' + esc(status || (ok ? 200 : "")) + '</span><span class="t mono">' + esc(env && env.id || "") + '</span>' +
    (env && env.ms != null ? '<span class="gray">' + (env.ms / 1000).toFixed(1) + ' s</span>' : '') + (ok ? '' : '<span class="ota-err">' + esc(((env && env.error) || {}).code || "") + '</span>');
  $("a-out").innerHTML = colour(env); $("a-prog").hidden = true;
}
async function send() {
  if (!fromJson() || showErrors().length) return;
  if (es) { es.close(); es = null; }
  $("a-out").textContent = ""; $("a-events").innerHTML = ""; $("a-events").hidden = true;
  $("a-status").innerHTML = '<span class="badge run">' + T("läuft", "running") + '</span>';
  if (!cur.progress) { const r = await api("POST", "/api/ops/" + cur.name + "?wait=1", body); showResult(r.json, r.status); return; }
  const r = await api("POST", "/api/ops/" + cur.name, body);
  if (!r.json || !r.json.id || r.json.ok === false || !r.json.accepted) { showResult(r.json, r.status); return; }
  const id = r.json.id, total = Number(body.seconds) || 0;
  $("a-status").innerHTML = '<span class="badge run">' + T("läuft", "running") + '</span><span class="t mono">' + esc(id) + '</span><span class="gray mono">SSE /api/ops/' + esc(id) + '/events</span>' +
    '<button class="mini" id="a-cancel" type="button">' + T("abbrechen", "cancel") + '</button>';
  $("a-cancel").onclick = () => api("DELETE", "/api/ops/" + id);
  $("a-prog").hidden = !total; $("a-progbar").style.width = "0"; $("a-events").hidden = false;
  es = new EventSource("/api/ops/" + id + "/events");
  es.addEventListener("progress", e => {
    let d = {}; try { d = JSON.parse(e.data); } catch (x) { }
    const line = document.createElement("div"); line.textContent = e.data; $("a-events").appendChild(line); $("a-events").scrollTop = 1e9;
    if (total && d.elapsed_s != null) $("a-progbar").style.width = Math.min(100, 100 * d.elapsed_s / total) + "%";
    else if (d.total) { $("a-prog").hidden = false; $("a-progbar").style.width = Math.min(100, 100 * (d.done || 0) / d.total) + "%"; }
  });
  es.addEventListener("result", e => { es.close(); es = null; let env = {}; try { env = JSON.parse(e.data); } catch (x) { } showResult(env, env.ok ? 200 : ""); });
  es.onerror = () => { if (es) { es.close(); es = null; $("a-status").insertAdjacentHTML("beforeend", '<span class="ota-err">' + T("Verbindung zu serve verloren", "lost the connection to serve") + '</span>'); } };
}
function tab(which) { [["ans", "t-ans", "p-ans"], ["req", "t-req", "p-req"], ["sch", "t-sch", "p-sch"]].forEach(([k, t, p]) => { $(t).setAttribute("aria-selected", String(k === which)); $(p).hidden = k !== which; }); }
$("t-ans").onclick = () => tab("ans"); $("t-req").onclick = () => tab("req"); $("t-sch").onclick = () => tab("sch");
$("a-filter").addEventListener("input", renderList);
$("a-form").addEventListener("input", fromForm); $("a-form").addEventListener("change", fromForm);
$("a-form").addEventListener("click", e => {
  const add = e.target.dataset.add, del = e.target.dataset.del;
  if (add) { const s = props()[add] || {}; const arr = Array.isArray(body[add]) ? body[add] : (body[add] = []);
    arr.push(s.items && s.items.type === "object" ? Object.fromEntries(Object.keys(s.items.properties || {}).map(c => [c, ""])) : ""); renderForm(); writeJson(); showErrors(); }
  if (del) { body[del].splice(Number(e.target.dataset.i), 1); renderForm(); writeJson(); showErrors(); }
});
$("a-json").addEventListener("input", fromJson);
$("a-fmt").onclick = () => { if (fromJson()) writeJson(); };
$("a-send").onclick = send;
$("a-copy").onclick = () => { const t = curl(); (navigator.clipboard ? navigator.clipboard.writeText(t) : Promise.reject()).then(() => { $("a-hint").textContent = T("curl kopiert", "curl copied"); }, () => { tab("req"); }); };
async function loadOpsList() {
  const r = await api("GET", "/api/describe");
  OPS = (r.json && r.json.data) || [];
  if (!cur) cur = OPS.find(o => o.name === "ga_read") || OPS[0];
  if (cur) { renderList(); renderOp(); }
}

// =============================================================================================== rules
let rules = null, rulesFile = null, rulesBroken = false; // broken: the file on disk is no JSON, Save waits for an edit
const listText = a => Array.isArray(a) ? a.join(", ") : "";
const textList = t => t.split(",").map(x => x.trim()).filter(x => x);
function rcheck(r) {
  const e = [], seen = {};
  if (r.version !== 1) e.push(T("version muss 1 sein", "version must be 1"));
  const rate = r.rate_per_minute === undefined ? 60 : r.rate_per_minute;
  if (!Number.isInteger(rate) || rate < 1 || rate > 6000) e.push("rate_per_minute: 1..6000");
  if (!Array.isArray(r.sites)) { e.push(T("sites muss eine Liste sein", "sites must be a list")); return e; }
  r.sites.forEach((s, i) => {
    const n = "Site " + (i + 1) + (s && s.name ? " (" + s.name + ")" : "");
    if (typeof s !== "object" || s === null) { e.push(n + ": " + T("Objekt erwartet", "an object")); return; }
    if (!s.name) e.push(n + ": " + T("braucht einen Namen", "needs a name"));
    if (s.suite_run && !s.bench_config) e.push(n + ": " + T("suite_run braucht bench_config", "suite_run needs bench_config"));
    ["write_lines", "setpa_lines"].forEach(k => { if (s[k] !== undefined) { if (!Array.isArray(s[k])) e.push(n + ": " + k + T(" ist eine Liste", " is a list")); else s[k].forEach(l => { if (!LINE.test(l)) e.push(n + ": " + k + " " + l + T(" ist keine Linie (z. B. 1.1)", " is not a line (e.g. 1.1)")); }); } });
    if (s.ga_write !== undefined && (typeof s.ga_write !== "object" || !Array.isArray(s.ga_write.allow) || !Array.isArray(s.ga_write.deny))) e.push(n + ": " + T("ga_write braucht allow und deny", "ga_write needs allow and deny"));
    if (s.protected !== undefined && !Array.isArray(s.protected)) e.push(n + ": protected" + T(" ist eine Liste", " is a list"));
    else (s.protected || []).forEach(p => { if (!PA.test((p && p.pa) || "")) e.push(n + ": " + T("geschützt braucht eine physikalische Adresse", "protected needs an individual address")); });
    if (!Array.isArray(s.interfaces)) { e.push(n + ": " + T("braucht interfaces", "needs interfaces")); return; }
    s.interfaces.forEach((f, j) => {
      const m = n + ", Interface " + (j + 1);
      if (typeof f !== "object" || f === null || Array.isArray(f)) { e.push(m + ": " + T("Objekt erwartet", "an object")); return; }
      if (!IPV4.test(f.ip || "")) e.push(m + ": " + T("ip muss IPv4 sein", "ip must be IPv4"));
      const port = f.port === undefined ? 3671 : f.port;
      if (!Number.isInteger(port) || port < 1 || port > 65535) e.push(m + ": port 1..65535");
      const key = (f.ip || "") + ":" + port; if (seen[key]) e.push(m + ": " + key + T(" steht schon in ", " is already in ") + seen[key]); else seen[key] = s.name || n;
      if (f.line !== undefined && !LINE.test(f.line)) e.push(m + ": line " + f.line + T(" ist keine Linie", " is not a line"));
      if (f.classes !== undefined && !Array.isArray(f.classes)) e.push(m + ": classes" + T(" ist eine Liste", " is a list"));
      else (f.classes || []).forEach(c => { if (!CLASSES.some(x => x[0] === c)) e.push(m + ": " + T("unbekannte Klasse ", "unknown class ") + c); });
    });
  });
  return e;
}
function effectiveNote(i) {
  const s = rules.sites[i], eff = ((rulesData && rulesData.effective) || []).find(x => x.name === s.name);
  if (!eff || !s.bench_config) return "";
  const add = (a, b) => (a || []).filter(x => !(b || []).includes(x));
  const parts = [];
  const wl = add(eff.write_lines, s.write_lines); if (wl.length) parts.push(T("Schreiblinien ", "write lines ") + wl.join(", "));
  const pr = add(eff.protected, (s.protected || []).map(p => p.pa)); if (pr.length) parts.push(T("geschützt ", "protected ") + pr.join(", "));
  const gd = add(eff.ga_deny, s.ga_write && s.ga_write.deny); if (gd.length) parts.push(T("gesperrt ", "denied ") + gd.join(", "));
  return parts.length ? '<div class="eff">' + T("aus bench_config kommen beim Laden dazu: ", "bench_config adds when loaded: ") + esc(parts.join(" · ")) + '</div>' : "";
}
function rrender() {
  $("r-rate").value = rules.rate_per_minute === undefined ? "" : rules.rate_per_minute;
  const box = $("r-list"); box.innerHTML = "";
  (Array.isArray(rules.sites) ? rules.sites : []).forEach((s, i) => {
    if (typeof s !== "object" || s === null) return;
    const d = document.createElement("section"); d.className = "site";
    const ifs = (Array.isArray(s.interfaces) ? s.interfaces : []).map((f, j) => {
      if (typeof f !== "object" || f === null) return "";
      const cls = Array.isArray(f.classes) ? f.classes : DEFAULT_CLASSES;
      return '<tr><td><input type="text" data-s="' + i + '" data-f="' + j + '" data-k="ip" value="' + esc(f.ip || "") + '"></td>' +
        '<td class="port"><input type="text" data-s="' + i + '" data-f="' + j + '" data-k="port" placeholder="3671" value="' + esc(f.port === undefined ? "" : f.port) + '"></td>' +
        '<td><input type="text" data-s="' + i + '" data-f="' + j + '" data-k="line" placeholder="–" value="' + esc(f.line || "") + '"></td>' +
        CLASSES.map(c => '<td class="c"><input type="checkbox" title="' + esc(c[2]) + '" data-s="' + i + '" data-f="' + j + '" data-cls="' + c[0] + '"' + (cls.includes(c[0]) ? ' checked' : '') + '></td>').join("") +
        '<td><button type="button" class="x" data-delif="' + i + '" data-f="' + j + '" aria-label="' + T("Interface entfernen", "remove interface") + '">×</button></td></tr>';
    }).join("");
    const inp = (k, label, val, ph, list) => '<label>' + label + '<input data-s="' + i + '" data-k="' + k + '"' + (list ? ' data-list="1"' : '') + ' value="' + esc(val) + '" placeholder="' + esc(ph) + '"></label>';
    d.innerHTML = '<h2><input data-s="' + i + '" data-k="name" value="' + esc(s.name || "") + '" placeholder="Name"><button type="button" class="x" data-delsite="' + i + '" aria-label="' + T("Site entfernen", "remove site") + '">×</button></h2><div class="body"><div class="kv">' +
      inp("write_lines", T("Schreiblinien (write_lines)", "Write lines (write_lines)"), listText(s.write_lines), T("z. B. 1.1", "e.g. 1.1"), 1) +
      inp("setpa_lines", T("setpa-Linien (setpa_lines)", "setpa lines (setpa_lines)"), listText(s.setpa_lines), T("z. B. 1.1", "e.g. 1.1"), 1) +
      inp("ga_allow", T("Gruppenadressen erlaubt (ga_write.allow)", "Group addresses allowed (ga_write.allow)"), listText(s.ga_write && s.ga_write.allow), T("leer = keine; * = alle", "empty = none; * = all"), 1) +
      inp("ga_deny", T("Gruppenadressen gesperrt (ga_write.deny)", "Group addresses denied (ga_write.deny)"), listText(s.ga_write && s.ga_write.deny), T("z. B. 0/3/100", "e.g. 0/3/100"), 1) +
      inp("protected", T("geschützte Geräte (protected)", "Protected devices (protected)"), listText((s.protected || []).map(p => p.pa)), T("z. B. 1.1.3", "e.g. 1.1.3"), 1) +
      inp("bench_config", T("Prüfstand-Konfiguration (bench_config)", "Bench configuration (bench_config)"), s.bench_config || "", "–") +
      inp("lock_file", T("Lock-Datei (lock_file)", "Lock file (lock_file)"), s.lock_file || "", "–") +
      '<label class="chk"><input type="checkbox" data-s="' + i + '" data-k="suite_run"' + (s.suite_run ? ' checked' : '') + '>' + T("Suite-Lauf über serve erlauben (suite_run)", "Allow a suite run through serve (suite_run)") + '</label>' +
      '</div>' + effectiveNote(i) +
      '<div class="tablewrap"><table class="ifs"><thead><tr><th>Interface</th><th>Port</th><th>' + T("Linie", "Line") + '</th>' +
      CLASSES.map(c => '<th class="c" title="' + esc(c[2]) + '">' + esc(c[1]) + '</th>').join("") + '<th></th></tr></thead><tbody>' + ifs + '</tbody></table></div>' +
      '<button type="button" class="add" data-addif="' + i + '">+ Interface</button></div>';
    box.appendChild(d);
  });
  rstats();
}
function rstats() {
  const sites = Array.isArray(rules.sites) ? rules.sites : [];
  $("r-sites").textContent = sites.length;
  $("r-ifs").textContent = sites.reduce((n, s) => n + (((s && s.interfaces) || []).length), 0);
}
function rwrite() { $("r-json").value = JSON.stringify(rules, null, 2); $("r-json").classList.remove("bad"); rerrs(); rstats(); }
function rerrs() {
  const e = rcheck(rules);
  if (rulesBroken) e.unshift(T("Die Datei ist kein gültiges JSON; Speichern erst nach einer Änderung", "The file is not valid JSON; Save after a change"));
  $("r-errs").innerHTML = e.length ? e.map(x => '<span>' + esc(x) + '</span>').join("") : '<span class="ok">' + T("serve würde diese Datei laden", "serve would load this file") + '</span>';
  $("r-save").disabled = e.length > 0; $("r-save").style.opacity = e.length ? .45 : 1;
}
function rfromForm(ev) {
  const t = ev.target; if (t.dataset.s === undefined) return;
  rulesBroken = false;
  const s = rules.sites[Number(t.dataset.s)], k = t.dataset.k;
  if (t.dataset.f !== undefined) {
    const f = s.interfaces[Number(t.dataset.f)];
    if (t.dataset.cls) {
      const cls = f.classes ? f.classes.slice() : DEFAULT_CLASSES.slice(), c = t.dataset.cls, has = cls.includes(c);
      if (t.checked && !has) cls.push(c); if (!t.checked && has) cls.splice(cls.indexOf(c), 1);
      f.classes = CLASSES.map(x => x[0]).filter(x => cls.includes(x));
    } else if (k === "port") { if (t.value === "") delete f.port; else f.port = /^\d+$/.test(t.value) ? Number(t.value) : t.value; }
    else if (t.value === "") delete f[k]; else f[k] = t.value;
  } else if (k === "suite_run") { if (t.checked) s.suite_run = true; else delete s.suite_run; }
  // without ga_write serve allows no group address at all: a new one starts the same way, never with "*"
  else if (k === "ga_allow" || k === "ga_deny") { s.ga_write = s.ga_write || { allow: [], deny: [] }; s.ga_write[k === "ga_allow" ? "allow" : "deny"] = textList(t.value); }
  else if (k === "protected") { const l = textList(t.value); if (l.length) s.protected = l.map(pa => { const old = (s.protected || []).find(p => p.pa === pa); return old || { pa: pa, ops: [] }; }); else delete s.protected; }
  else if (t.dataset.list) { const l = textList(t.value); if (l.length) s[k] = l; else delete s[k]; }
  else if (t.value === "") delete s[k]; else s[k] = t.value;
  rwrite();
}
$("r-list").addEventListener("input", rfromForm); $("r-list").addEventListener("change", rfromForm);
$("r-list").addEventListener("click", e => {
  const d = e.target.dataset;
  if (d.addif !== undefined) { const s = rules.sites[Number(d.addif)]; (s.interfaces = s.interfaces || []).push({ ip: "", classes: DEFAULT_CLASSES.slice() }); rrender(); rwrite(); }
  if (d.delif !== undefined) { rules.sites[Number(d.delif)].interfaces.splice(Number(d.f), 1); rrender(); rwrite(); }
  if (d.delsite !== undefined) { rules.sites.splice(Number(d.delsite), 1); rrender(); rwrite(); }
});
$("r-addsite").onclick = () => { rulesBroken = false; (rules.sites = Array.isArray(rules.sites) ? rules.sites : []).push({ name: "", interfaces: [] }); rrender(); rwrite(); };
$("r-rate").addEventListener("input", () => { rulesBroken = false; const v = $("r-rate").value.trim(); if (v === "") delete rules.rate_per_minute; else rules.rate_per_minute = /^\d+$/.test(v) ? Number(v) : v; rwrite(); });
$("r-json").addEventListener("input", () => {
  let r; try { r = JSON.parse($("r-json").value); } catch (e) { $("r-json").classList.add("bad"); $("r-errs").innerHTML = '<span>JSON: ' + esc(e.message) + '</span>'; $("r-save").disabled = true; return; }
  if (typeof r !== "object" || r === null || Array.isArray(r)) { $("r-json").classList.add("bad"); $("r-save").disabled = true; return; }
  $("r-json").classList.remove("bad"); rules = r; rulesBroken = false; rrender(); rerrs();
});
$("r-fmt").onclick = () => { if (!$("r-json").classList.contains("bad")) rwrite(); };
$("r-reset").onclick = () => {
  rulesBroken = false;
  try { rules = JSON.parse(rulesFile); } catch (e) { rules = { version: 1, sites: [] }; rulesBroken = true; }
  if (typeof rules !== "object" || rules === null || Array.isArray(rules)) rules = { version: 1, sites: [] };
  rrender(); rwrite(); msg("r-msg", "");
};
$("r-save").onclick = async () => {
  if (rcheck(rules).length) return;
  $("r-save").disabled = true;
  const r = await api("PUT", "/api/rules", { text: JSON.stringify(rules, null, 2) + "\n" });
  if (r.json && r.json.ok) {
    applyRules(r.json.data);
    msg("r-msg", T("Gespeichert. Gilt ab dem nächsten Aufruf; die vorige Datei liegt als rules.json.bak daneben.", "Saved. It applies from the next call; the previous file is kept as rules.json.bak."));
  } else msg("r-msg", ((r.json && r.json.error) || {}).reason || ("HTTP " + r.status), true);
  rerrs();
};
function applyRules(d) {
  rulesData = d;
  $("r-path").textContent = d.path; $("r-at").textContent = d.loaded_at ? new Date(d.loaded_at).toLocaleString(LANG === "de" ? "de-DE" : "en-GB") : "–";
  rulesFile = d.exists && d.text ? d.text : '{"version": 1, "sites": []}';
  rulesBroken = false;
  try { rules = JSON.parse(rulesFile); } catch (e) { rules = { version: 1, sites: [] }; rulesBroken = true; msg("r-msg", T("Die Datei ist kein gültiges JSON: ", "The file is not valid JSON: ") + e.message, true); }
  if (typeof rules !== "object" || rules === null || Array.isArray(rules)) rules = { version: 1, sites: [] };
  rrender(); rwrite(); renderOverview();
}
async function loadRules() { const r = await api("GET", "/api/rules"); if (r.json && r.json.ok) applyRules(r.json.data); }

// =============================================================================================== views
const VIEWS = ["over", "api", "rules"];
function show(v) {
  VIEWS.forEach(x => { $("view-" + x).hidden = x !== v; $("m-" + x).classList.toggle("active", x === v); $("m-" + x).setAttribute("aria-current", x === v ? "page" : "false"); });
  if (v === "over") loadOps();
}
VIEWS.forEach(x => { $("m-" + x).onclick = e => { e.preventDefault(); history.replaceState(null, "", "#" + x); show(x); }; });
show(VIEWS.includes(location.hash.slice(1)) ? location.hash.slice(1) : "over");
refresh(); loadRules(); loadOpsList(); scan();
setInterval(refresh, 5000);
</script>
</body></html>
)OKNXUI";
}

} // namespace serve
} // namespace ftc

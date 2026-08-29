// knxOTA: firmware, delta or throughput to a target PA. The page arms a job and polls its state --
// the transfer runs in the client's loop(), so no request is ever held open.
// Every value shown comes from /knxota/status. Nothing is claimed while the answer is missing.
const _o = i => document.getElementById(i);
// "05.0.3" and "5.0.3" are the same address -- compare the numbers, not the text.
const otaNorm = p => {
  const m = /^\s*(\d+)\.(\d+)\.(\d+)\s*$/.exec(p || "");
  if (!m) return "";
  return (+m[1] > 15 || +m[2] > 15 || +m[3] > 255) ? "" : (+m[1]) + "." + (+m[2]) + "." + (+m[3]);
};
// The markup lives here, not as a C++ literal: in the gzipped asset the same text costs a third.
// The three build switches arrive as data attributes on the shell.
const OTA_HTML = {
 A: `<h1>knxOTA</h1><p class='lead'>Firmware oder Differenz von diesem Gerät über KNX auf ein anderes übertragen — PA zu PA, ohne PC.</p><div class='ctx'><div class='ctx-line'><span><span class='k'>Ziel</span> <b id='cxTarget'>—</b> <span id='cxDev' class='k'></span></span><span><span class='k'>Zugriff:</span> <b id='cxAcc'>unbekannt</b> <span class='k' id='cxAccAge'></span></span><span><span class='k'>Datei</span> <b id='cxFile'>keine</b></span><span class='cx-run'><span class='k'>Interface:</span> <b id='cxBus'>nichts läuft</b></span></div><div class='ctx-prog fm-hidden' id='cxProgWrap'><div class='ctx-bar'><i id='cxBar'></i></div><span class='k' id='cxLeft'></span><button class='mini' id='cxStop'>Abbrechen</button></div></div><details class='log' id='logBox'><summary><span id='logSum'>Protokoll</span></summary><div class='logbox' id='otaLog'><div>bereit</div></div></details><div class='fm-tabs steps' id='steps'><a class='fm-tab active' href='#' data-step='1'><span class='n'>1</span>Ziel<span class='st' id='stp1'></span></a><a class='fm-tab' href='#' data-step='2'><span class='n'>2</span>Auftrag<span class='st' id='stp2'></span></a><a class='fm-tab' href='#' data-step='3'><span class='n'>3</span>Übertragen<span class='st' id='stp3'></span></a></div><p class='ota-hint warn' id='stepNote'></p><section class='reg pane' id='pane1'><h2>Ziel<span class='sub'>wer, was, und ob es schreiben lässt</span></h2><div class='body'><div class='sec'><div class='fm-row' style='margin:0'><input type='text' id='otaPa' value='' placeholder='z. B. 5.0.3' style='flex:0 0 120px;min-width:120px'><button id='otaRead'>Gerät lesen</button><button class='mini' id='otaSearchToggle'>Andere Adresse suchen</button></div><p class='ota-hint' id='otaPaNote'></p><div class='drawer fm-hidden' id='otaDrawer'><div class='fm-row' style='margin:0'><span class='lbl'>Bereich</span><input type='text' id='otaArea' value='1' style='flex:0 0 54px;min-width:54px'><span class='lbl'>Linie</span><input type='text' id='otaLine' value='1' style='flex:0 0 54px;min-width:54px'><select id='otaScope' style='height:34px;border:1px solid #d0d0d0;padding:0 8px'><option value='line'>nur diese Linie</option><option value='area'>alle 16 Linien dieses Bereichs</option></select><button id='otaScan'>Suche starten</button><button class='mini' id='otaFromPa'>vom Ziel übernehmen</button></div><p class='ota-hint' id='otaScanRange'></p><div class='scan fm-hidden' id='otaScanBox'><div class='fm-row' style='margin:0'><b style='font-size:.9em' id='otaScanTitle'>Suche</b><span class='ota-hint' style='margin:0;flex:1' id='otaScanState'></span><button class='mini' id='otaScanStop'>Suche beenden</button></div><div class='scanbar'><i id='otaScanBar'></i></div><div class='hits fm-hidden' id='otaHitBox'><table class='tight'><thead><tr><th>Adresse</th><th>Art</th><th class='right'></th></tr></thead><tbody id='otaHits'></tbody></table></div><p class='ota-hint' id='otaScanFoot'></p></div></div></div><div class='sec'><div id='otaDevEmpty'><p class='ota-hint' style='margin:0'>Noch nicht gelesen.</p></div><div id='otaDevNone' class='fm-hidden'><p class='ota-hint err' style='margin:0'>Keine Antwort von <span id='otaDevNonePa'></span>. Prüfen Sie Adresse und Busspannung.</p></div><div id='otaDevCard' class='fm-hidden'><div class='runhead'><span class='t' id='otaDevHead'></span><span class='stale' id='otaDevAge'></span></div><div class='caps' id='otaDevCaps'></div><div class='fm-row' style='margin:10px 0 0'><span class='lbl'>Programmiermodus</span><b id='otaPmText' style='font-size:.88em'>nicht gelesen</b><button class='mini' id='otaPm'>einschalten</button><button class='mini' id='otaProps'>Eigenschaften</button><button class='mini' id='otaMore'>Weitere Angaben</button></div><p class='ota-hint' id='otaPmNote' style='margin:6px 0 0'></p><table class='attribute-table tight fm-hidden' id='otaTProps'><tbody id='otaDevRows'></tbody></table><table class='attribute-table tight fm-hidden' id='otaTMore'><tbody id='otaDevRows2'></tbody></table></div></div><div class='sec'><h3>Zugriff</h3><div class='fm-row' style='margin:0'><span id='otaAccText'>noch nicht gelesen</span><span class='stale' id='otaAccAge'></span></div>`,
 SEC: `<div class='fm-row fm-hidden' id='otaLockRow' style='margin-top:8px'><input type='password' id='otaPw' placeholder='Passwort des Ziels' autocomplete='off' style='flex:0 0 210px;min-width:170px'><button id='otaUnlock'>Entsperren</button><button id='otaLock'>Abmelden</button><button class='mini' id='otaCheck'>Zugriff prüfen</button></div>`,
 B: `<p class='ota-hint fm-hidden' id='otaAccHint'></p></div><div class='sec'><div class='fm-row' style='margin:0'><span class='lbl' style='flex:0 0 150px'>Letzte Suche</span><span style='font-size:.88em;flex:1' id='otaLastScan'>keine</span></div><div class='fm-row' style='margin:6px 0 0'><span class='lbl' style='flex:0 0 150px'>Gruppenadressen</span><span style='font-size:.88em;flex:1' id='otaGaState'>erst das Gerät lesen</span><button class='mini' id='otaGa'>Lesen</button></div><details class='ga fm-hidden' id='otaGaBox' style='margin-top:8px'><summary id='otaGaSum'></summary><div class='gabox'><table class='tight'><thead><tr><th>GA</th><th>KO</th><th>Flags</th><th>Priorität</th><th>Größe</th></tr></thead><tbody id='otaGaRows'></tbody></table></div></details></div></div></section><section class='reg pane fm-hidden' id='pane2'><h2>Auftrag<span class='sub'>was gesendet wird und wie</span></h2><div class='body'><div class='sec'><h3>Art</h3><div class='fm-tabs' style='margin-bottom:12px'><a class='fm-tab active' href='#' data-job='full'>Voll-Abbild</a><a class='fm-tab' href='#' data-job='delta'>Differenz</a><a class='fm-tab' href='#' data-job='perf'>Durchsatz</a></div><p class='ota-hint' style='margin:0' id='otaJobNote'></p></div><div class='sec'><h3>Quelle</h3><div id='otaFile'><div class='fm-tabs' style='border:none;margin-bottom:8px'><a class='fm-tab active' href='#' data-otafs='int'>/flash</a>`,
 SD: `<a class='fm-tab' href='#' data-otafs='sd'>sd/</a>`,
 C: ``,
 EFC: `<a class='fm-tab' href='#' data-otafs='efc'>efc/</a>`,
 D: `</div><table class='tight'><thead><tr><th>Datei</th><th class='right'>Größe</th></tr></thead><tbody id='otaFiles'></tbody></table><p class='ota-hint' id='otaSrcNote'></p><div class='fm-row' style='margin:10px 0 0'><span class='lbl' style='flex:0 0 150px'>Ablegen auf dem Ziel</span><select id='otaDst' style='height:32px;border:1px solid #d0d0d0;padding:0 8px'></select><span class='ota-hint' style='margin:0;flex:1' id='otaDstNote'></span><button class='mini' id='otaDrives'>Speicher abfragen</button></div><table class='attribute-table tight' style='margin-top:6px'><tbody id='otaDrvRows'></tbody></table></div><div id='otaPerf' class='fm-hidden'><div class='fm-row ota-seg' style='margin:0'><input type='text' id='otaKb' value='50' style='flex:0 0 74px;min-width:74px'><span class='lbl'>KB Muster nach</span><select id='otaDrive' style='height:32px;border:1px solid #d0d0d0;padding:0 8px'><option value=''>/flash</option><option value='sd'>sd/</option><option value='efc'>efc/</option></select><label class='ota-sw' style='margin:0'><input type='checkbox' id='otaKeep'>Mess-Datei behalten</label></div><p class='ota-hint'>Erzeugtes Muster statt einer Datei.</p></div></div><div class='sec'><h3>Verfahren</h3><div class='fm-row ota-seg' style='margin:0'><button id='otaSafe' aria-pressed='false'>safe</button><button id='otaFast' aria-pressed='true'>fast</button><select id='otaWin' style='height:32px;border:1px solid #d0d0d0;padding:0 8px'><option value=''>Fenster: automatisch</option><option value='8'>fest 8</option><option value='16'>fest 16</option><option value='24'>fest 24</option><option value='32'>fest 32</option><option value='48'>fest 48</option><option value='64'>fest 64</option></select></div><p class='ota-hint' id='otaSpeedNote'></p><div class='ota-sw' id='otaSw'><label><input type='checkbox' id='otaApply' checked>Nach dem Übertragen anwenden <span class='gray'>(das Ziel startet neu)</span></label><label><input type='checkbox' id='otaResume' checked>Abbruch fortsetzen</label></div><p class='ota-hint warn' id='otaSwNote'></p></div><div class='sec'><h3>Vorschau</h3><p class='ota-hint' style='margin:0' id='otaPreview'></p></div></div></section><section class='reg pane fm-hidden' id='pane3'><h2>Übertragen<span class='sub'>starten, zusehen, Bilanz</span></h2><div class='body'><div class='sec'><div class='ready'><span><span class='k'>Ziel</span><b id='rdTarget'>fehlt</b></span><span><span class='k'>Zugriff</span><b id='rdAcc'>unbekannt</b></span><span><span class='k'>Datei</span><b id='rdFile'>fehlt</b></span><span><span class='k'>Platz</span><b id='rdSpace'>unbekannt</b></span><span><span class='k'>Interface</span><b id='rdBus'>nichts läuft</b></span></div><div class='gobar'><button id='otaStart'>Übertragen</button><span class='why' id='otaWhy'></span><button class='mini fm-hidden' id='otaFix'></button><button class='mini fm-hidden' id='otaStop'>Abbrechen</button></div></div><div class='sec fm-hidden' id='otaRunSec'><div class='runhead'><span class='t' id='otaRunTitle'></span></div><div class='bar'><i id='otaBar'></i></div><canvas id='otaChart' width='900' height='150' class='chart'></canvas><p class='ota-hint' id='otaChartLegend'></p><div id='otaRunWrap'><table class='attribute-table tight'><tbody id='otaRunRows'></tbody></table></div><p class='ota-hint' id='otaRunFoot'></p><div class='fm-row fm-hidden' id='otaFixRow' style='margin-top:12px'><span class='ota-hint err' style='margin:0;flex:0 0 100%' id='otaFixWhy'></span><input type='password' id='otaFixPw' placeholder='Passwort des Ziels' autocomplete='off' style='flex:0 0 200px;min-width:160px'><button id='otaFixGo'>Anmelden und anwenden</button></div><div class='fm-row fm-hidden' id='otaAfter' style='margin-top:12px'><button class='mini' id='otaReread'>Gerät neu lesen</button><button class='mini' id='otaAgain'>Erneut übertragen</button><button class='mini fm-hidden' id='otaTrigger'>Update auslösen</button></div></div><p class='ota-hint' id='otaEmptyRun'>Noch nichts übertragen.</p></div></section>`,
};
function otaBuild() {
 const el = _o("ota");
 if (!el) return;
 const f = el.dataset;
 el.innerHTML = OTA_HTML.A + (f.sec === "1" ? OTA_HTML.SEC : "") + OTA_HTML.B
  + (f.sd === "1" ? OTA_HTML.SD : "") + OTA_HTML.C
  + (f.efc === "1" ? OTA_HTML.EFC : "") + OTA_HTML.D;
}

let otaStep = 1, otaJob = "full", otaFs = "int", otaSel = null, otaFast = true, otaFiles = [];
let otaBusy = false, otaPending = null, otaArmSeq = 0;
let otaDev = null, otaDevPa = "", otaDevAt = 0, otaDevMiss = false;
let otaAccAt = 0, otaPmGuess = null;
let otaDrv = null, otaDrvPa = "", otaDrvChain = [];
let otaHits = [], otaScanWhen = "", otaScanLabel = "", otaScanDone = 0, otaScanTotal = 0;
let otaGa = null, otaGaAt = "", otaMsg = "", otaLogN = 0;
let otaT0 = 0, otaD0 = 0, otaAvg = 0, otaFailCheck = false;
let otaSentFile = "", otaSentPa = ""; // what was actually transferred -- the apply hangs off this
let otaChain = [];          // step sequence waiting on the bus: login -> re-read -> apply
let otaVerBefore = "";      // application version before the trigger -- the only proof afterwards
let otaSeries = [], otaLastChunk = -1, otaResult = null, otaRunning = false, otaSawRun = false;
// The reported throughput is a momentary rate that decays between two windows (measured: 4913 -> 352 B/s
// without a byte of progress). The page measures its own average instead.
const OTA_ACT = ["otaRead", "otaScan", "otaGa", "otaDrives", "otaUnlock", "otaLock", "otaCheck",
        "otaStart", "otaTrigger", "otaAgain", "otaReread"];
function otaLog(t, c) {
  otaLogN++;
  const sum = _o("logSum");
  if (sum) sum.textContent = "Protokoll (" + otaLogN + ")";
  const d = document.createElement("div");
  const now = new Date();
  const hm = String(now.getHours()).padStart(2, "0") + ":" + String(now.getMinutes()).padStart(2, "0");
  d.innerHTML = '<span class="gray">' + hm + '</span>  ' + (c ? '<span class="' + c + '">' + t + '</span>' : t);
  _o("otaLog").appendChild(d);
  _o("otaLog").scrollTop = 1e5;
}
const otaAgo = ms => {
  if (!ms) return "";
  const s = Math.round((Date.now() - ms) / 1000);
  return s < 10 ? "gerade eben" : s < 60 ? ("vor " + s + " s") : ("vor " + Math.round(s / 60) + " min");
};
const otaKb = b => b >= 1073741824 ? (b / 1073741824).toFixed(1) + " GB"
  : b >= 1048576 ? (b / 1048576).toFixed(1) + " MB" : (b / 1024).toFixed(1) + " KB";
const otaDur = s => s >= 3600 ? (Math.floor(s / 3600) + " h " + String(Math.round(s % 3600 / 60)).padStart(2, "0") + " min")
  : s >= 60 ? (Math.round(s / 60) + " min") : (Math.round(s) + " s");
const OTA_MSG = [
 [/refused fast open/i, "Das Ziel hat die Übertragung abgelehnt"],
 [/no answer|no response/i, "Das Ziel antwortet nicht"],
 [/source read error|cannot read source/i, "Die Quelldatei ließ sich nicht lesen"],
 [/not found/i, "Auf dem Ziel nicht gefunden"],
 [/no progress/i, "Keine Blöcke kommen mehr an"],
 [/full|space/i, "Auf dem Ziel ist kein Platz mehr"],
 [/cancel/i, "Abgebrochen"],
 [/too many/i, "Zu viele Fehlversuche"],
 [/overall deadline/i, "Zeitgrenze des Laufs überschritten"],
 [/up to date/i, "Das Ziel hat die Datei bereits"]
];
const otaMsgDe = m => { for (const [re, de] of OTA_MSG) if (re.test(m || "")) return de + " (" + m + ")"; return m; };
// The device class is known from the first hit (mask from the sweep). The OpenKNX flag only appears in
// the post-sweep probe and is usually gone before a poll sees it -- so it is shown only when really there.
const OTA_MASK = { 0x0012: "BCU1", 0x0013: "BCU1", 0x0020: "BCU2", 0x0021: "BCU2", 0x0025: "BCU2",
 0x0300: "BIM M112", 0x0700: "BIM M112", 0x0701: "BIM M112", 0x0705: "BIM M112",
 0x07B0: "System B", 0x07B1: "System B", 0x091A: "System B (Koppler)", 0x57B0: "System B (IP)" };
// sizeCode is the group object's value type (03_05_01 §4.18.3), not a byte count -- 12 means 8 byte.
const OTA_SIZE = ["1 Bit", "2 Bit", "3 Bit", "4 Bit", "5 Bit", "6 Bit", "7 Bit", "1 Byte", "2 Byte",
 "3 Byte", "4 Byte", "6 Byte", "8 Byte", "10 Byte", "14 Byte"];
const otaSize = c => OTA_SIZE[c] || ("Typ " + c);
const otaClass = m => OTA_MASK[m] || (m ? "Maske 0x" + m.toString(16).toUpperCase().padStart(4, "0") : "antwortet");
const otaRow = (k, v) => '<tr><td>' + k + '</td><td>' + v + '</td></tr>';
// ── Tabs ────────────────────────────────────────────────────────────────────────────────────────
function otaGoStep(n) {
  if (otaRunning && n !== 3) return;
  otaStep = n;
  document.querySelectorAll("[data-step]").forEach(t => t.classList.toggle("active", +t.dataset.step === n));
  [1, 2, 3].forEach(i => _o("pane" + i).classList.toggle("fm-hidden", i !== n));
  window.scrollTo({ top: 0 });
  otaRender();
}
// ── Requests ────────────────────────────────────────────────────────────────────────────────────
async function otaPost(path, params, body) {
  const o = { method: "POST" };
  if (body !== undefined) { o.body = body; o.headers = { "Content-Type": "text/plain" }; }
  try {
    const r = await fetch("/knxota/" + path + "?" + new URLSearchParams(params), o);
    if (!r.ok) {
      const m = (await r.text()).trim();
      otaLog(m || ("HTTP " + r.status), "ota-err");
      return false;
    }
    return true;
  } catch (e) {
    otaLog("Gerät antwortet nicht: " + e.message, "ota-err");
    return false;
  }
}
// Arm one operation. The page gates at once, so the click is visible before the first poll answers,
// and a short operation that finishes before that poll cannot leave the page locked.
async function otaArm(id, path, params, body) {
  if (otaBusy) return;
  const b = _o(id);
  if (b) b.classList.add("busy");
  otaPending = id;
  otaArmSeq++;
  otaBusy = true;
  otaGate(true);
  if (!(await otaPost(path, params, body))) {
    if (b) b.classList.remove("busy");
    otaPending = null;
    otaBusy = false;
    otaGate(false);
  }
}
function otaGate(on) {
  OTA_ACT.forEach(id => { const e = _o(id); if (e) e.disabled = on; });
  document.querySelectorAll("[data-job],[data-otafs]").forEach(t => {
    t.style.pointerEvents = on ? "none" : "";
    t.style.opacity = on ? ".45" : "";
  });
  const s = _o("otaStop");
  if (s) s.classList.toggle("fm-hidden", !otaRunning);
}
// ── Files ───────────────────────────────────────────────────────────────────────────────────────
async function otaLoadFiles() {
  const tb = _o("otaFiles");
  tb.innerHTML = '<tr><td colspan="2" class="gray">lädt …</td></tr>';
  try { otaFiles = await (await fetch("/knxota/files?fs=" + otaFs + "&job=" + otaJob)).json(); }
  catch (e) { otaFiles = []; }
  otaSel = null;
  otaRender();
}
function otaDrawFiles() {
  const tb = _o("otaFiles");
  tb.innerHTML = "";
  if (!otaFiles.length) {
    tb.innerHTML = '<tr><td colspan="2" class="gray">Keine passende Datei auf diesem Laufwerk</td></tr>';
    return;
  }
  otaFiles.forEach((f, i) => {
    const tr = document.createElement("tr");
    tr.className = "pick"; tr.tabIndex = 0;
    tr.setAttribute("aria-selected", String(otaSel === i));
    tr.innerHTML = '<td class="mono">' + f.name + '</td><td class="right">' + f.size + '</td>';
    const pick = () => { otaSel = i; otaRender(); };
    tr.onclick = pick;
    tr.onkeydown = e => { if (e.key === "Enter" || e.key === " ") { e.preventDefault(); pick(); } };
    tb.appendChild(tr);
  });
}
function otaSetJob(j) {
  otaJob = j; otaSel = null;
  document.querySelectorAll("[data-job]").forEach(t => t.classList.toggle("active", t.dataset.job === j));
  _o("otaFile").classList.toggle("fm-hidden", j === "perf");
  _o("otaPerf").classList.toggle("fm-hidden", j !== "perf");
  _o("otaSw").style.display = j === "perf" ? "none" : "";
  _o("otaStart").textContent = j === "perf" ? "Messen" : "Übertragen";
  if (j !== "perf") otaLoadFiles(); else otaRender();
}
// ── The sweep: its own range, independent of the target ─────────────────────────────────────────
function otaScanSpec() {
  const a = Math.min(15, Math.max(0, parseInt(_o("otaArea").value) || 0));
  const l = Math.min(15, Math.max(0, parseInt(_o("otaLine").value) || 0));
  const area = _o("otaScope").value === "area";
  return { a, l, area, label: area ? (a + ".*") : (a + "." + l), count: area ? 16 * 255 : 255 };
}
// ── The target's drives: three separate commands, chained by the browser ────────────────────────
function otaProbeDrives() {
  const pa = otaNorm(_o("otaPa").value);
  if (!pa) return;
  otaDrvChain = ["", "sd", "efc"];
  otaNextDrive();
}
function otaNextDrive() {
  if (!otaDrvChain.length) { otaLog("Speicher des Ziels abgefragt", "ota-ok"); return; }
  const fs = otaDrvChain.shift();
  otaArm("otaDrives", "drives", { pa: _o("otaPa").value, fs: fs });
}
// ── Poll ────────────────────────────────────────────────────────────────────────────────────────
async function otaTick() {
  const seq = otaArmSeq;
  let s;
  try { s = await (await fetch("/knxota/status")).json(); } catch (e) { return; }
  if (seq !== otaArmSeq) return;   // armed while this poll was in flight -> its answer is from before
  otaApply(s);
  if (s.busy && !otaBusy) {
    // Already running when the page loaded: adopt it, or the run ends without a balance.
    otaBusy = true;
    if (!otaPending && otaRunning) { otaPending = "otaStart"; otaSawRun = true; }
    otaGate(true);
  } else if (!s.busy && otaBusy) {
    otaBusy = false; otaGate(false); otaFinished(s);
    if (otaChain.length) setTimeout(otaChainNext, 250);
  }
  if (otaRunning) otaSawRun = true;
}
function otaApply(s) {
  const pa = otaNorm(_o("otaPa").value);
  otaRunning = (s.phase === "upload" || s.phase === "verify");
  if (s.dev && s.dev.pa === pa) { otaDev = s.dev; otaDevPa = s.dev.pa; otaDevMiss = false; if (!otaDevAt) otaDevAt = Date.now(); }
  if (s.drives && s.drivesPa === pa) otaDrv = s.drives;
  if (s.ga) { otaGa = s.ga.lost ? null : (s.ga.pa === pa ? s.ga.rows : otaGa); otaGaLost = s.ga.lost; }
  if (s.scan) {
    otaScanDone = s.scan.done; otaScanTotal = s.scan.total;
    if (s.scan.hits && s.scan.hits.length) otaHits = s.scan.hits;
  }
  if (s.message && s.message !== otaMsg) { otaMsg = s.message; otaLog(s.message); }
  // Samples for the curve: one per poll while a transfer runs.
  if (otaRunning && s.total) {
    // Bytes over time, measured here: the reported rate decays between two windows.
    if (!otaT0) { otaT0 = Date.now(); otaD0 = s.done; }
    const dt = (Date.now() - otaT0) / 1000;
    otaAvg = dt > 1 ? Math.max(0, Math.round((s.done - otaD0) / dt)) : 0;
    const gap = otaLastChunk >= 0 && s.chunk < otaLastChunk;
    otaLastChunk = s.chunk;
    otaSeries.push({ bps: otaAvg, win: s.window || 8, gap: gap || false });
    if (otaSeries.length > 400) otaSeries.shift();
  } else if (!otaRunning) { otaT0 = 0; otaD0 = 0; }
  otaLive = s;
  // While something runs the view belongs to step 3 -- on EVERY poll, not only on the transition.
  if (otaRunning && otaStep !== 3) otaGoStep(3);
  otaRender();
}
let otaLive = null, otaGaLost = false;
// What came of the apply. The client puts it in status.message: silence means applied, 0xA0/0xA2 means
// refused -- and each needs a different remedy.
function otaApplyOutcome(msg) {
  const m = msg || "";
  if (/apply triggered/i.test(m)) return { kind: "ok" };
  if (/login/i.test(m) && /refused/i.test(m)) return { kind: "login" };
  if (/writes disabled/i.test(m)) return { kind: "writes" };
  if (/refused/i.test(m)) return { kind: "other" };
  if (/cannot self-apply/i.test(m)) return { kind: "cannot" };
  return { kind: "none" };
}

// The remedy sits where the refusal is, and matches the reason: a password helps only for 0xA0.
function otaShowApplyFix(ap) {
  const row = _o("otaFixRow"), why = _o("otaFixWhy"), pw = _o("otaFixPw"), go = _o("otaFixGo");
  if (!row) return;
  if (ap.kind !== "login" && ap.kind !== "writes") { row.classList.add("fm-hidden"); return; }
  row.classList.remove("fm-hidden");
  if (ap.kind === "login") {
    why.textContent = "Anmeldung abgelaufen — sie hält nur, solange geschrieben wird. "
      + "Die Datei liegt geprüft am Ziel; es fehlt nur das Auslösen.";
    pw.classList.remove("fm-hidden");
    go.textContent = "Anmelden und anwenden";
  } else {
    why.textContent = "Kein Passwort verlangt, trotzdem gesperrt — dann will das Ziel den Programmiermodus.";
    pw.classList.add("fm-hidden");
    go.textContent = "Programmiermodus ein und anwenden";
  }
}

// After the trigger the target reboots. Only the re-read application version proves it landed.
function otaVerifyAfterApply() {
  const pa = otaSentPa || _o("otaPa").value;
  otaLog("Prüfe in 15 s die Version am Ziel");
  setTimeout(() => {
    otaChain = [{ id: "otaRead", path: "feat", params: () => ({ pa: pa }),
      before: () => { otaVerAsk = true; return true; } }];
    otaChainNext();
  }, 15000);
}
let otaVerAsk = false;

// A chain of bus steps. Each is its own operation: the next starts when the previous is done, and a
// "before" can abort the chain when the previous did not deliver what was expected.
function otaChainNext() {
  while (otaChain.length) {
    const st = otaChain.shift();
    if (st.before && st.before() === false) { otaChain = []; return; }
    otaArm(st.id, st.path, st.params(), st.body ? st.body() : undefined);
    return;
  }
}

// A finished operation: say what came of it.
function otaFinished(s) {
  const who = otaPending;
  otaPending = null;
  OTA_ACT.forEach(id => { const e = _o(id); if (e) e.classList.remove("busy"); });
  if (who === "otaDrives") { otaNextDrive(); return; }
  if (who === "otaScan") {
    otaScanWhen = new Date().toLocaleTimeString("de-DE", { hour: "2-digit", minute: "2-digit" });
    const n = s.scan ? s.scan.hits.length : 0;
    otaLog("Suche beendet — " + n + (n === 1 ? " Gerät" : " Geräte"), n ? "ota-ok" : "ota-warn");
    return;
  }
  if (who === "otaRead" || who === "otaCheck" || who === "otaReread") {
    // An answer belonging to another address must not count as this one's success.
    const cur = otaNorm(_o("otaPa").value);
    if (s.dev && s.dev.pa !== cur) {
      otaLog("Antwort gehört zu " + s.dev.pa + " — die Adresse wurde inzwischen geändert", "ota-warn");
      return;
    }
    if (!s.dev) { otaDevMiss = true; otaDev = null; otaLog("Keine Antwort — Adresse falsch oder Gerät nicht am Bus", "ota-err"); }
    else {
      otaDevAt = Date.now(); otaAccAt = Date.now(); otaPmGuess = null; otaDevMiss = false;
      if (otaFailCheck) {
        // The client reports "target refused fast open" -- only the feature byte says why.
        otaFailCheck = false;
        if (s.dev.feat & 0x20) {
          otaLog("Ziel verweigert Schreibvorgänge — daran ist es gescheitert", "ota-err");
          if (otaResult) {
            otaResult.rows.push(["Zugriff",
              '<span class="ota-err">Das Ziel verweigert Schreibvorgänge — erst entsperren, dann erneut übertragen</span>']);
            otaRender();
          }
        } else otaLog("Zugriff offen", "ota-ok");
        return;
      }
      if (otaVerAsk) {
        otaVerAsk = false;
        const now = s.dev.appVer || "";
        if (otaVerBefore && now && now !== otaVerBefore)
          otaLog("Angewendet: Anwendungsversion " + otaVerBefore + " → " + now, "ota-ok");
        else if (otaVerBefore && now)
          otaLog("Version unverändert (" + now + ") — vermutlich nicht angewendet", "ota-warn");
        else otaLog("Version vorher unbekannt", "ota-warn");
        return;
      }
      otaLog("Gerät gelesen", "ota-ok"); otaProbeDrives();
    }
    return;
  }
  if (who === "otaGa") {
    otaGaAt = new Date().toLocaleTimeString("de-DE", { hour: "2-digit", minute: "2-digit" });
    const n = otaGa ? otaGa.length : 0;
    otaLog("Gruppenadressen gelesen: " + n + " Einträge", "ota-ok");
    if (n) _o("otaGaBox").open = true;
    return;
  }
  if (who === "otaUnlock" || who === "otaLock") {
    otaLog(who === "otaLock" ? "Abgemeldet — lese zurück"
                : "Angemeldet — lese zurück");
    if (!otaChain.length) setTimeout(() => otaArm("otaRead", "feat", { pa: _o("otaPa").value }), 300);
    return;
  }
  if (who === "otaTrigger") {
    const ap = otaApplyOutcome(s.message);
    if (ap.kind !== "none") {
      otaShowApplyFix(ap);
      if (ap.kind === "ok") {
        otaLog("Ausgelöst — das Ziel startet neu", "ota-ok");
        otaVerifyAfterApply();
      } else {
        otaLog("Abgelehnt: " + (s.message || "kein Grund"), "ota-err");
      }
      return;
    }
    otaLog("Auslösen gesendet — unbestätigt", "ota-warn");
    _o("otaRunFoot").textContent = "Auslösen gesendet — unbestätigt. „Gerät neu lesen“ zeigt die Version.";
    return;
  }
  if (who !== "otaStart" && who !== "otaAgain") return;
  if (!otaSawRun && !s.result) {
    // The client refuses some jobs silently. The status then still carries the PREVIOUS message, and a
    // balance built from it would be invented.
    otaLog("Auftrag nicht angenommen — nichts begonnen", "ota-err");
    return;
  }
  otaSawRun = false;
  otaSummaryOf(s);
  if (!(s.result && s.result.ok)) {
    otaFailCheck = true;   // warum gescheitert? Das Faehigkeitsbyte des Ziels sagt es.
    setTimeout(() => otaArm("otaRead", "feat", { pa: _o("otaPa").value }), 400);
  }
}
function otaSummaryOf(s) {
  const r = s.result, rows = [];
  if (r && r.ok) {
    const secs = (r.ms / 1000);
    if (r.kind === 1) {
      rows.push(["Ergebnis", '<span class="ota-ok">erfolgreich</span>'], ["Ziel", r.pa],
           ["Gemessen", otaKb(r.bytes)], ["Mittlerer Durchsatz", r.bps + " B/s"],
           ["Dauer", otaDur(secs)], ["Blöcke", r.chunks],
           ["Verfahren", r.mode === 1 ? "fast" : "safe"],
           ["Mess-Datei", r.cleanup === 1 ? "wieder entfernt" : r.cleanup === 2 ? "auf dem Ziel behalten"
                 : r.cleanup === 3 ? "liegen geblieben" : "—"]);
    } else {
      rows.push(["Ergebnis", '<span class="ota-ok">erfolgreich</span>'], ["Ziel", r.pa],
           ["Datei", r.file || "—"],
           ["Übertragen", otaKb(r.bytes) + " in " + r.chunks + " Blöcken"],
           ["Dauer", otaDur(secs)], ["Mittlerer Durchsatz", r.bps + " B/s"],
           ["Verfahren", r.mode === 1 ? "fast" : "safe"],
           ["Wiederholungen", (r.retries || 0)]);
      if (r.resumed) rows.push(["davon fortgesetzt", otaKb(r.resumed)]);
      rows.push(["Prüfsumme", r.verify === 1 ? '<span class="ota-ok">verglichen, stimmt überein</span>'
        : r.verify === 2 ? '<span class="ota-err">weicht ab — nicht anwenden</span>'
        : r.verify === 3 ? '<span class="ota-warn">Größe stimmt — nicht per Prüfsumme belegt (SD/externer Flash meldet keine)</span>'
        : "nicht verglichen"]);
      const ap = otaApplyOutcome(s.message);
      rows.push(["Anwenden",
        !r.apply ? '<span class="ota-warn">offen — mit „Update auslösen“ starten</span>'
        : ap.kind === "ok" ? '<span class="ota-ok">ausgelöst — das Ziel startet neu</span>'
        : ap.kind === "login" ? '<span class="ota-err">abgelehnt — die Anmeldung am Ziel ist abgelaufen</span>'
        : ap.kind === "writes" ? '<span class="ota-err">abgelehnt — das Ziel nimmt keine Schreibvorgänge an</span>'
        : ap.kind === "cannot" ? '<span class="ota-warn">das Ziel kann nicht selbst anwenden — die Datei liegt dort</span>'
        : ap.kind === "other" ? '<span class="ota-err">vom Ziel abgelehnt</span>'
        : '<span class="ota-warn">unbestätigt</span>']);
    }
    otaResult = { ok: r.verify !== 2, rows: rows,
      title: r.kind === 1 ? "Gemessen" : "Übertragen",
      foot: r.kind === 1 ? "" : "Erst die neu gelesene Version beweist, dass es sitzt." };
    _o("otaTrigger").classList.toggle("fm-hidden", !(r.kind !== 1 && !r.apply));
  } else {
    const pc = s.total ? (s.done / s.total * 100).toFixed(0) : "0";
    rows.push(["Ergebnis", '<span class="ota-err">abgebrochen bei ' + pc + ' %</span>'],
         ["Grund", '<span class="ota-err">' + (otaMsgDe(s.message) || "kein Grund gemeldet") + '</span>'],
         ["Ziel", s.target]);
    if (s.total) rows.push(["Übertragen", otaKb(s.done) + " von " + otaKb(s.total)],
               ["Blöcke", s.chunk + " von " + s.chunks]);
    rows.push(["Prüfsumme", "nicht geprüft"], ["Anwenden", "nicht ausgelöst"]);
    otaResult = { ok: false, rows: rows, title: "Abgebrochen",
      foot: "Das Ziel läuft unverändert weiter. Der nächste Versuch setzt hier an." };
    _o("otaTrigger").classList.add("fm-hidden");
  }
  _o("otaAfter").classList.remove("fm-hidden");
  otaShowApplyFix(otaApplyOutcome(s.message));
  otaRender();
}
// ── The curve: throughput, window, gaps -- the numbers the console prints, drawn ────────────────
function otaDrawChart() {
  const c = _o("otaChart");
  if (!c) return;
  const dpr = window.devicePixelRatio || 1, W = c.clientWidth || 900, H = 150;
  if (c.width !== Math.round(W * dpr)) { c.width = Math.round(W * dpr); c.height = Math.round(H * dpr); }
  const g = c.getContext("2d");
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  g.clearRect(0, 0, W, H);
  if (otaSeries.length < 2) { _o("otaChartLegend").textContent = ""; return; }
  const pad = { l: 46, r: 44, t: 12, b: 16 }, w = W - pad.l - pad.r, h = H - pad.t - pad.b;
  const bpsMax = Math.max(120, ...otaSeries.map(p => p.bps)) * 1.15, winMax = 64;
  const x = i => pad.l + (i / (otaSeries.length - 1)) * w;
  g.strokeStyle = "#e8e8e8"; g.lineWidth = 1; g.fillStyle = "#999"; g.font = "10px sans-serif";
  for (let k = 0; k <= 2; k++) {
    const v = Math.round(bpsMax / 2 * k), y = pad.t + h - (v / bpsMax) * h;
    g.beginPath(); g.moveTo(pad.l, y + .5); g.lineTo(pad.l + w, y + .5); g.stroke();
    g.textAlign = "right"; g.fillText(v + " B/s", pad.l - 6, y + 3);
  }
  g.strokeStyle = "#c8d8c8"; g.lineWidth = 1.5; g.beginPath();
  otaSeries.forEach((p, i) => { const y = pad.t + h - (p.win / winMax) * h; i ? g.lineTo(x(i), y) : g.moveTo(x(i), y); });
  g.stroke();
  g.textAlign = "left"; g.fillStyle = "#8aa88a";
  g.fillText("Fenster " + winMax, pad.l + w + 6, pad.t + 8);
  g.fillText("0", pad.l + w + 6, pad.t + h + 3);
  g.beginPath(); g.moveTo(pad.l, pad.t + h);
  otaSeries.forEach((p, i) => g.lineTo(x(i), pad.t + h - (p.bps / bpsMax) * h));
  g.lineTo(pad.l + w, pad.t + h); g.closePath();
  g.fillStyle = "rgba(68,152,65,.16)"; g.fill();
  g.beginPath();
  otaSeries.forEach((p, i) => { const y = pad.t + h - (p.bps / bpsMax) * h; i ? g.lineTo(x(i), y) : g.moveTo(x(i), y); });
  g.strokeStyle = "#449841"; g.lineWidth = 1.6; g.stroke();
  g.fillStyle = "#c62828";
  otaSeries.forEach((p, i) => { if (p.gap) g.fillRect(x(i) - 1.5, pad.t, 3, h); });
  const gaps = otaSeries.filter(p => p.gap).length;
  _o("otaChartLegend").innerHTML = '<span class="ota-ok">—</span> Durchsatz (Mittel) &nbsp; '
    + '<span style="color:#8aa88a">—</span> Fenster &nbsp; '
    + (gaps ? '<span class="ota-err">|</span> ' + gaps + ' Lücke' + (gaps > 1 ? 'n' : '') : 'keine Lücken');
}
// ── Blocking reasons: the button is never silently disabled ─────────────────────────────────────
function otaStepState() {
 const pa = otaNorm(_o("otaPa").value);
 const fresh = otaDev && otaDevPa === pa;
 // Step 1 -- the target
 let s1, w1;
 if (!pa) { s1 = "red"; w1 = "Zieladresse fehlt"; }
 else if (otaDevMiss) { s1 = "red"; w1 = "keine Antwort von " + pa; }
 else if (fresh && !otaDev.ftm) { s1 = "red"; w1 = "kein Dateitransfer"; }
 else if (!fresh) { s1 = "amber"; w1 = "noch nicht gelesen"; }
 else if (otaDev.feat & 0x20) { s1 = "amber"; w1 = "kein Schreibzugriff"; }
 else { s1 = "green"; w1 = pa; }
 // Step 2 -- the job
 let s2, w2;
 const f = otaJob === "perf" ? null : otaFiles[otaSel];
 if (otaJob === "perf") { s2 = "green"; w2 = (parseInt(_o("otaKb").value) || 50) + " KB Muster"; }
 else if (otaSel === null) { s2 = "red"; w2 = "keine Datei"; }
 else if (otaJob === "delta" && fresh && otaDev.ftm && !(otaDev.feat & 0x80)) { s2 = "red"; w2 = "Ziel kann keine Differenz"; }
 else if (otaSpaceShort(f)) { s2 = "amber"; w2 = "Platz auf dem Ziel knapp"; }
 else { s2 = "green"; w2 = f.name; }
 // Step 3 -- released only once 1 and 2 are green
 // Step 3 stays reachable while something runs: the lights describe the way TO the start, not the view
 // of a run already going.
 const open3 = (s1 === "green" && s2 === "green") || otaRunning || !!otaResult;
 let s3, w3;
 if (otaRunning) { s3 = "amber"; w3 = (otaLive && otaLive.total) ? ((otaLive.done / otaLive.total * 100).toFixed(0) + " %") : "läuft"; }
 else if (otaResult) { s3 = otaResult.ok ? "green" : "red"; w3 = otaResult.ok ? "fertig" : "abgebrochen"; }
 else if (!open3) { s3 = "red"; w3 = "gesperrt"; }
 else { s3 = "green"; w3 = "bereit"; }
 return { s1, w1, s2, w2, s3, w3, open2: s1 === "green", open3 };
}
// Kept separate: both the step lights and the blocking reason read it.
function otaSpaceShort(f) {
 if (!f || !otaDrv) return false;
 const dst = _o("otaDst").value, i = dst === "sd" ? 1 : dst === "efc" ? 2 : 0, d = otaDrv[i];
 if (!d || d.state !== 2) return false;
 const freeB = d.kb ? d.free * 1024 : d.free;
 return (f.bytes || 0) + 8192 > freeB;
}

function otaBlocker() {
  const s = otaLive;
  if (otaRunning) return ["Interface überträgt — eines auf einmal.", null, null];
  if (otaBusy) return ["Interface beschäftigt — eines auf einmal.", null, null];
  const pa = otaNorm(_o("otaPa").value);
  if (!pa) return ["Die Zieladresse ist unvollständig.", "zum Ziel", () => { otaGoStep(1); _o("otaPa").focus(); }];
  if (otaJob === "perf") return null;
  if (otaDevMiss) return ["Keine Antwort von " + pa + ".", "Erneut lesen", () => { otaGoStep(1); otaArm("otaRead", "feat", { pa: pa }); }];
  if (!otaDev || otaDevPa !== pa) return ["Das Ziel ist noch nicht gelesen.", "Gerät lesen", () => { otaGoStep(1); otaArm("otaRead", "feat", { pa: pa }); }];
  if (!otaDev.ftm) return ["Kein Dateitransfer — kein Update möglich.", null, null];
  if ((otaDev.feat & 0x10) && (otaDev.feat & 0x20))
    return ["Das Ziel verlangt ein Passwort.", "zum Entsperren", () => { otaGoStep(1); const p = _o("otaPw"); if (p) p.focus(); }];
  if (otaDev.feat & 0x20)
    return ["Das Ziel verlangt den Programmiermodus.", "einschalten", () => { otaGoStep(1); otaPmWrite(true); }];
  if (otaJob === "delta" && !(otaDev.feat & 0x80))
    return ["Das Ziel kann keine Differenz verarbeiten.", "zu Voll-Abbild wechseln", () => { otaGoStep(2); otaSetJob("full"); }];
  if (otaSel === null) return ["Es ist keine Datei gewählt.", "zur Auswahl", () => otaGoStep(2)];
  const dst = _o("otaDst").value, f = otaFiles[otaSel];
  if (f && otaDrv) {
    const i = dst === "sd" ? 1 : dst === "efc" ? 2 : 0, d = otaDrv[i];
    if (d && d.state === 2) {
      const freeB = d.kb ? d.free * 1024 : d.free, need = (f.bytes || 0) + 8192;
      if (need > 8192 && freeB < need)
        return ["Auf dem Ziel ist zu wenig Platz — " + otaKb(freeB) + " frei, gebraucht werden " + otaKb(need) + ".", null, null];
    }
  }
  return null;
}
function otaPmWrite(on) {
  otaPmGuess = on;
  otaPost("progmode", { pa: _o("otaPa").value, on: on ? 1 : 0 });
  otaLog("Programmiermodus " + (on ? "ein" : "aus") + " geschrieben — nicht bestätigt");
  _o("otaPmNote").textContent = on
    ? "Geschrieben. Die Programmier-LED leuchtet, solange er an ist."
    : "Geschrieben — unbestätigt.";
  otaRender();
}
// ── Render ──────────────────────────────────────────────────────────────────────────────────────
function otaRender() {
  const s = otaLive || {}, pa = otaNorm(_o("otaPa").value);
  const raw = _o("otaPa").value.trim();
  _o("otaPaNote").className = "ota-hint" + (!pa && raw ? " err" : "");
  _o("otaPaNote").textContent = (!pa && raw) ? "Die Zieladresse ist unvollständig."
    : "Bereich 0–15 · Linie 0–15 · Gerät 0–255";
  _o("otaRead").textContent = (otaDev || otaDevMiss) ? "Neu lesen" : "Gerät lesen";
  // Summary line
  _o("cxTarget").textContent = pa || "—";
  _o("cxDev").textContent = otaDev && otaDevPa === pa ? ("– " + (otaDev.ftm ? "OpenKNX" : "kein Dateitransfer"))
    : otaDevMiss ? "– keine Antwort" : "– nicht gelesen";
  const acc = !otaDev || otaDevPa !== pa || !otaDev.ftm ? "unbekannt"
    : (otaDev.feat & 0x20) ? "gesperrt" : "offen";
  _o("cxAcc").textContent = acc;
  _o("cxAccAge").textContent = otaAccAt ? ("(" + otaAgo(otaAccAt).replace("vor ", "vor ") + " geprüft)") : "";
  const f = otaJob === "perf" ? null : otaFiles[otaSel];
  _o("cxFile").textContent = otaJob === "perf" ? ((parseInt(_o("otaKb").value) || 50) + " KB Muster")
    : (f ? f.name : "keine");
  // What the interface is doing -- "frei" said nothing; this is an activity, with progress.
  const act = { ping: "Anmeldung läuft", list: "liest ein Verzeichnis", info: "liest das Gerät",
         delete: "schreibt", scan: "Suche " + (otaScanTotal ? otaScanDone + " von " + otaScanTotal : ""),
         upload: "", verify: "prüft die Prüfsumme" };
  let bus = "nichts läuft";
  if (otaRunning && s.total) bus = "Übertragung " + (s.done / s.total * 100).toFixed(0) + " %";
  else if (s.busy) bus = act[s.phase] || "arbeitet";
  _o("cxBus").textContent = bus;
  _o("rdBus").textContent = bus;
  // The bar up top belongs to the transfer -- the only thing long enough to need it everywhere.
  _o("cxProgWrap").classList.toggle("fm-hidden", !(otaRunning && s.total));
  if (otaRunning && s.total) {
    _o("cxBar").style.width = (s.done / s.total * 100) + "%";
    _o("cxLeft").textContent = otaAvg ? ("noch " + otaDur((s.total - s.done) / otaAvg)) : "";
  }
  // Tabs
  const st = otaStepState();
  document.querySelectorAll("[data-step]").forEach(t => {
    const n = +t.dataset.step;
    const lock = (otaRunning && n !== 3) || (n === 2 && !st.open2) || (n === 3 && !st.open3);
    t.classList.toggle("locked", lock);
    t.title = !lock ? ""
      : otaRunning ? "während einer Übertragung gesperrt"
      : n === 2 ? "erst das Ziel klären: " + st.w1
      : "erst Ziel und Auftrag klären";
    const b = t.querySelector(".n");
    if (b) b.className = "n " + (n === 1 ? st.s1 : n === 2 ? st.s2 : st.s3);
  });
  // The tab is called "Ziel", so it carries the address, not the device class.
  _o("stp1").textContent = st.s1 === "green" ? st.w1 : (pa ? pa + " · " + st.w1 : st.w1);
  _o("stp2").textContent = st.w2;
  // Device card
  const fresh = otaDev && otaDevPa === pa;
  _o("otaDevEmpty").classList.toggle("fm-hidden", !!(fresh || otaDevMiss));
  _o("otaDevCard").classList.toggle("fm-hidden", !fresh);
  _o("otaDevNone").classList.toggle("fm-hidden", !otaDevMiss);
  if (otaDevMiss) _o("otaDevNonePa").textContent = pa;
  if (fresh) {
    const d = otaDev;
    const ver = d.ftm ? ((d.ftm >> 8) + "." + ((d.ftm >> 4) & 0x0F) + "." + (d.ftm & 0x0F)) : "";
    // The identity line answers "does THIS file belong on THIS device": order number, application
    // version, FTM level. The device class lives in the properties.
_o("otaDevHead").textContent = pa
 + " · " + ((d.order && d.order !== "") ? d.order : (d.cls || "unbekannt"))
 + (d.appVer ? " · v" + d.appVer : "")
 + (d.ftm ? " · FTM " + ver : "");
    _o("otaDevAge").textContent = otaDevAt ? ("gelesen " + otaAgo(otaDevAt)) : "";
    const nm = v => (v === undefined || v === null || v === "")
      ? '<span class="gray">vom Gerät nicht gemeldet</span>' : v;
    const ld = v => ["nicht geladen", "geladen", "lädt", "Fehler", "wird entladen", "schließt ab"][v] || "—";
    _o("otaDevRows").innerHTML =
      otaRow("Geräteklasse", nm(d.cls) + (d.mask ? ' <span class="gray">(Maske ' + d.mask.toString(16).toUpperCase().padStart(4, "0") + ')</span>' : ""))
      + otaRow("Hersteller", d.mfr ? ("0x" + d.mfr.toString(16).toUpperCase().padStart(4, "0")
        + (d.mfr === 0xFA ? " (OpenKNX)" : "")) : nm(null))
      + otaRow("Bestellnummer", nm(d.order))
      + otaRow("Anwendungsversion", nm(d.appVer))
      + otaRow("Dateitransfer (FTM)", d.ftm ? ("Version " + ver)
        : '<span class="ota-err">nicht vorhanden — kein Update möglich</span>')
      + otaRow("Programmiermodus", d.prog ? "an" : "aus")
      + otaRow("ETS-Downloads", d.dl === undefined ? nm(null) : d.dl);
    _o("otaDevRows2").innerHTML =
      otaRow("Seriennummer", d.serial ? '<span class="mono">' + d.serial + '</span>' : nm(null))
      + otaRow("Hardware-Kennung", d.hw ? '<span class="mono">' + d.hw + '</span>' : nm(null))
      + otaRow("Max. Rahmengröße", d.apdu ? (d.apdu + " B") : nm(null))
      + otaRow("Busspannung", d.volt ? ((d.volt / 1000).toFixed(1).replace(".", ",") + " V") : nm(null))
      + otaRow("Koppler", d.router ? "ja" : "nein")
      + otaRow("Tabellenzustände", d.tabs
        ? ("Anwendung " + ld(d.tabs[0]) + " · Adresstabelle " + ld(d.tabs[1])
         + " · Assoziationen " + ld(d.tabs[2]) + " · Objekte " + ld(d.tabs[3]))
        : nm(null));
    if (d.ftm) {
      const names = [[0x01, "fortsetzen"], [0x02, "selbst anwenden"], [0x04, "fast"],
             [0x08, "Konsole"], [0x40, "gzip"], [0x80, "Differenz"]];
      const can = names.filter(n => d.feat & n[0]).map(n => n[1]);
      const no = names.filter(n => !(d.feat & n[0])).map(n => n[1]);
      _o("otaDevCaps").innerHTML = "Kann: " + (can.join(" · ") || "nichts davon")
        + (no.length ? '<br><span class="no">Kann nicht: ' + no.join(" · ") + '</span>' : '');
    } else _o("otaDevCaps").innerHTML = "";
    const pmOn = otaPmGuess !== null ? otaPmGuess : !!d.prog;
    _o("otaPmText").textContent = (pmOn ? "an" : "aus")
      + (otaPmGuess !== null && otaPmGuess !== !!d.prog ? " — geschrieben, noch nicht gelesen" : "");
    _o("otaPm").textContent = pmOn ? "ausschalten" : "einschalten";
    _o("otaProps").textContent = _o("otaTProps").classList.contains("fm-hidden") ? "Eigenschaften" : "Eigenschaften zu";
    _o("otaMore").textContent = _o("otaTMore").classList.contains("fm-hidden") ? "Weitere Angaben" : "Weitere Angaben zu";
  }
  // Access
  _o("otaAccText").textContent = !fresh ? "noch nicht gelesen"
    : !otaDev.ftm ? "kein Dateitransfer"
    : !(otaDev.feat & 0x20) ? ((otaDev.feat & 0x10) ? "Angemeldet."
                            : "Das Ziel verlangt kein Passwort.")
    : "Nimmt keine Schreibvorgänge an.";
  _o("otaAccAge").textContent = otaAccAt ? ("Stand: " + otaAgo(otaAccAt)) : "";
  const needPw = !!(fresh && otaDev.ftm && (otaDev.feat & 0x10));
  const progGate = !!(fresh && otaDev.ftm && !needPw && (otaDev.feat & 0x20));
  const lockRow = _o("otaLockRow");
  if (lockRow) lockRow.classList.toggle("fm-hidden", !needPw);
  _o("otaAccHint").classList.toggle("fm-hidden", !needPw && !progGate);
  _o("otaAccHint").textContent = progGate
    ? "Programmiermodus einschalten, dann neu lesen. Hilft das nicht, ist der Zugriff am Ziel aus."
    : "Das Passwort geht nie im Klartext über den Bus. Das Ziel schließt von selbst.";
  if (_o("otaUnlock")) { _o("otaUnlock").disabled = otaBusy || !(otaDev && (otaDev.feat & 0x20)); }
  if (_o("otaLock")) { _o("otaLock").disabled = otaBusy || !(otaDev && !(otaDev.feat & 0x20)); }
  // Sweep
  const sp = otaScanSpec();
  _o("otaScanRange").textContent = (sp.area
    ? (sp.a + ".0.1 – " + sp.a + ".15.255 · alle 16 Linien · " + sp.count + " Adressen")
    : (sp.a + "." + sp.l + ".1 – " + sp.a + "." + sp.l + ".255 · " + sp.count + " Adressen"))
    + " · etwa " + (sp.area ? "20–30 min" : "1–2 min");
  const scanning = s.scan && s.scan.running;
  _o("otaScanBox").classList.toggle("fm-hidden", !(scanning || otaHits.length));
  _o("otaScanTitle").textContent = "Suche " + (otaScanLabel || sp.label);
  _o("otaScanStop").classList.toggle("fm-hidden", !scanning);
  _o("otaScanBar").style.width = (otaScanTotal ? (otaScanDone / otaScanTotal * 100) : 0) + "%";
  _o("otaScanState").textContent = scanning
    ? (otaScanDone + " von " + otaScanTotal + " geprüft · " + otaHits.length + " gefunden · bei " + (s.scan.at || ""))
    : (otaScanWhen ? ("beendet " + otaScanWhen + " · " + otaHits.length + " Geräte") : "");
  _o("otaScanFoot").textContent = scanning
    ? "Eine Linie dauert 1–2 min. Läuft auf dem Interface weiter."
    : "Treffer bleiben bis zur nächsten Suche.";
  _o("otaHitBox").classList.toggle("fm-hidden", !otaHits.length);
  const hb = _o("otaHits");
  hb.innerHTML = "";
  otaHits.forEach(h => {
    const tr = document.createElement("tr");
    tr.innerHTML = '<td class="mono">' + h.pa + '</td><td>' + otaClass(h.mask)
      + (h.ok ? ' <span class="ota-ok">· OpenKNX</span>' : '') + '</td>';
    const td = document.createElement("td");
    td.className = "right";
    const b = document.createElement("button");
    b.className = "mini"; b.textContent = "Übernehmen";
    b.onclick = () => {
      _o("otaPa").value = h.pa;
      otaDev = null; otaDevMiss = false; otaDevAt = 0; otaDrv = null; otaGa = null;
      otaLog("Ziel " + h.pa + " übernommen");
      otaRender();
    };
    td.appendChild(b); tr.appendChild(td); hb.appendChild(tr);
  });
  _o("otaLastScan").textContent = otaScanWhen ? ((otaScanLabel || sp.label) + " · " + otaScanWhen + " · " + otaHits.length + " Geräte") : "keine";
  // Group addresses
  _o("otaGa").disabled = otaBusy || !(fresh && otaDev.ftm);
  _o("otaGaState").textContent = !fresh ? "erst das Gerät lesen"
    : !otaDev.ftm ? "nicht anwendbar"
    : otaGaLost ? "verworfen durch eine fast-Übertragung — erneut lesen"
    : otaGa === null ? "nicht gelesen — eigener Buslauf, 1–3 min"
    : otaGa.length === 0 ? ("gelesen " + otaGaAt + " — 0 Einträge (das Gerät führt keine Gruppenadressen)")
    : ("gelesen " + otaGaAt + " — " + otaGa.length + " Einträge");
  _o("otaGaBox").classList.toggle("fm-hidden", !(otaGa && otaGa.length));
  if (otaGa && otaGa.length) {
    _o("otaGaSum").textContent = otaGa.length + " Einträge";
    const fl = v => ["K", "L", "S", "Ü", "A"].filter((_, i) => v & (1 << i)).join(" ");
    const pr = p => p === 0 ? "System" : p === 1 ? "normal" : p === 2 ? "dringend" : p === 3 ? "niedrig" : "—";
    _o("otaGaRows").innerHTML = otaGa.map(r => '<tr><td class="mono">'
      + ((r.ga >> 11) & 0x1F) + "/" + ((r.ga >> 8) & 0x07) + "/" + (r.ga & 0xFF)
      + '</td><td>' + r.co + '</td><td class="mono">' + (r.cfg ? fl(r.flags) : "—")
      + '</td><td>' + (r.cfg ? pr(r.prio) : "—") + '</td><td>' + (r.cfg ? otaSize(r.size) : "—")
      + '</td></tr>').join("");
  }
  // Job
  otaDrawFiles();
  _o("otaJobNote").textContent = otaJob === "delta"
    ? "Nur die Differenz geht über den Bus. Das Ziel muss die passende Vorlage laufen."
    : otaJob === "perf" ? "Misst den Durchsatz — gleicher Weg, erzeugtes Muster."
    : "Das ganze Abbild geht über den Bus und braucht Platz am Ziel.";
  _o("otaSrcNote").textContent = otaJob === "delta" ? "Angezeigt werden .okd." : "Angezeigt werden .bin, .uf2 und .gz.";
  _o("otaSpeedNote").textContent = otaFast
    ? "Mehrere Blöcke, dann eine Lückenmeldung. Fenster 8–64, sucht die Obergrenze."
    : "Ein Block, eine Bestätigung. Langsamer, aber busfest.";
  // Target drives -- only what was asked for, never a guess
  const dd = _o("otaDst"), keep = dd.value;
  const opts = [["", "intern"]];
  if (otaDrv) {
    if (otaDrv[1] && otaDrv[1].state === 2) opts.push(["sd", "SD-Karte des Ziels"]);
    if (otaDrv[2] && otaDrv[2].state === 2) opts.push(["efc", "externer Flash des Ziels"]);
  }
  const sig = opts.map(o => o[0]).join(",");
  if (dd.dataset.sig !== sig) {
    dd.dataset.sig = sig;
    dd.innerHTML = opts.map(o => '<option value="' + o[0] + '">' + o[1] + '</option>').join("");
    dd.value = opts.some(o => o[0] === keep) ? keep : "";
  }
  const dst = dd.value;
  _o("otaDstNote").textContent = !otaDrv ? "Noch nicht abgefragt."
    : dst ? "Von hier kann das Ziel nicht anwenden — Datei wird nur abgelegt." : "";
  _o("otaDstNote").className = "ota-hint" + (dst ? " warn" : "");
  if (dst && _o("otaApply").checked) _o("otaApply").checked = false;
  _o("otaApply").disabled = !!dst;
  _o("otaDrvRows").innerHTML = !otaDrv ? "" :
    [[0, "interner Speicher"], [1, "SD-Karte"], [2, "externer Flash"]].map(([i, lbl]) => {
      const v = otaDrv[i];
      const f2 = v.kb ? v.free * 1024 : v.free, t2 = v.kb ? v.total * 1024 : v.total;
      return otaRow(lbl, v.state === 0 ? '<span class="gray">nicht abgefragt</span>'
        : v.state === 1 ? '<span class="gray">nicht vorhanden</span>'
        : v.state === 3 ? '<span class="ota-warn">unterstützt, nichts eingelegt</span>'
        : otaKb(f2) + " frei von " + otaKb(t2));
    }).join("");
  let swn = "";
  if (otaJob !== "perf" && fresh && otaDev.ftm && _o("otaApply").checked && !(otaDev.feat & 0x02))
    swn = "Ziel kann nicht selbst anwenden — Datei wird nur abgelegt.";
  if (otaFast && fresh && otaDev.ftm && !(otaDev.feat & 0x04))
    swn = (swn ? swn + " " : "") + "fast nicht verfügbar — es wird safe verwendet.";
  _o("otaSwNote").textContent = swn;
  _o("otaPreview").textContent = otaJob === "perf"
    ? ((parseInt(_o("otaKb").value) || 50) + " KB nach " + (pa || "—") + " · geschätzt "
     + otaDur((parseInt(_o("otaKb").value) || 50) * 1024 / 400))
    : (!f ? "Datei fehlt — Dauer nicht berechenbar"
      : (f.name + " → " + (pa || "—") + " · " + f.size + " · geschätzt " + otaDur((f.bytes || 0) / 400) + " bei rund 400 B/s"));
  // Readiness + blocking reason
  _o("rdTarget").textContent = !pa ? "fehlt" : (fresh && otaDev.ftm) ? "bereit"
    : otaDevMiss ? "keine Antwort" : fresh ? "kein Dateitransfer" : "nicht gelesen";
  _o("rdAcc").textContent = otaJob === "perf" ? "—" : acc;
  _o("rdFile").textContent = otaJob === "perf" ? "Muster" : (f ? "bereit" : "fehlt");
  _o("rdSpace").textContent = (() => {
    if (!otaDrv) return "unbekannt";
    const i = dst === "sd" ? 1 : dst === "efc" ? 2 : 0, v = otaDrv[i];
    return v && v.state === 2 ? otaKb(v.kb ? v.free * 1024 : v.free) + " frei" : "unbekannt";
  })();
  const b = otaBlocker();
  _o("otaStart").disabled = !!b;
  _o("otaStart").textContent = otaJob === "perf" ? "Messen" : "Übertragen";
  _o("otaWhy").textContent = b ? ("Nicht möglich: " + b[0]) : "Bereit.";
  _o("otaWhy").className = "why" + (b ? " ota-warn" : "");
  const fx = _o("otaFix");
  if (b && b[1]) { fx.classList.remove("fm-hidden"); fx.textContent = b[1]; fx.onclick = b[2]; }
  else fx.classList.add("fm-hidden");
  _o("otaStop").classList.toggle("fm-hidden", !otaRunning);
  // Live values, then the balance -- the same table: the values BECOME the balance.
  otaDrawChart();
  if (otaRunning) {
    _o("otaEmptyRun").classList.add("fm-hidden");
    _o("otaRunSec").classList.remove("fm-hidden");
    _o("otaRunTitle").textContent = "Überträgt " + (f ? f.name + " → " : "") + (s.target || pa);
    _o("otaBar").style.width = (s.total ? (s.done / s.total * 100) : 0) + "%";
    const win = s.window ? (s.window + (s.windowState === 2 ? " · fest" : s.windowState === 3 ? " · bremst"
      : s.windowState === 1 ? " · eingependelt" : " · sucht noch das Maximum")) : "safe — ein Block je Bestätigung";
    _o("otaRunRows").innerHTML =
      otaRow("Übertragen", otaKb(s.done) + " von " + otaKb(s.total))
      + otaRow("Blöcke", s.chunk + " von " + s.chunks)
      + otaRow("Durchsatz", otaAvg ? (otaAvg + ' B/s <span class="gray">(Mittel, hier gemessen)</span>') : "—")
      + otaRow("Fenster", win)
      + otaRow("Wiederholungen", (s.resends || 0) + (s.crcErrors ? " · Prüfsummenfehler " + s.crcErrors : ""))
      + otaRow("Verbleibend", otaAvg ? otaDur((s.total - s.done) / otaAvg) : "—");
    _o("otaRunWrap").className = "";
    _o("otaRunFoot").textContent = "Läuft auf dem Interface. Abbrechen nur hier.";
  } else if (otaResult) {
    _o("otaEmptyRun").classList.add("fm-hidden");
    _o("otaRunSec").classList.remove("fm-hidden");
    _o("otaRunTitle").innerHTML = otaResult.ok ? '<span class="ota-ok">' + otaResult.title + '</span>'
                         : '<span class="ota-err">' + otaResult.title + '</span>';
    _o("otaRunRows").innerHTML = otaResult.rows.map(([k, v]) => otaRow(k, v)).join("");
    _o("otaRunWrap").className = "balance" + (otaResult.ok ? "" : " bad");
    if (otaResult.foot) _o("otaRunFoot").textContent = otaResult.foot;
  }
  _o("stp3").textContent = st.w3;
}
// ── Arming ──────────────────────────────────────────────────────────────────────────────────────
function otaStartJob() {
  if (otaBlocker()) return;
  const pa = _o("otaPa").value.trim();
  const p = { pa: pa, job: otaJob, mode: otaFast ? "fast" : "safe", win: _o("otaWin").value };
  if (otaJob === "perf") {
    p.kb = _o("otaKb").value;
    p.drive = _o("otaDrive").value;
    p.keep = _o("otaKeep").checked ? 1 : 0;
  } else {
    p.fs = otaFs;
    p.path = otaFiles[otaSel].name;
    otaSentFile = otaFiles[otaSel].name;
    otaSentPa = pa;
    p.apply = _o("otaApply").checked ? 1 : 0;
    p.resume = _o("otaResume").checked ? 1 : 0;
    const d = _o("otaDst").value;
    if (d) p.dst = d;
  }
  otaSeries = []; otaLastChunk = -1; otaResult = null; otaSawRun = false; otaT0 = 0; otaD0 = 0; otaAvg = 0;
  _o("otaAfter").classList.add("fm-hidden");
  otaGoStep(3);
  otaArm("otaStart", "start", p);
}
function otaInit() {
 otaBuild();
  document.querySelectorAll("[data-step]").forEach(t => t.onclick = e => {
    e.preventDefault();
    const n = +t.dataset.step, ss = otaStepState();
    let why = "";
    if (otaRunning && n !== 3)
      why = "Übertragung aktiv. Wechsel gesperrt.";
    else if (n === 2 && !ss.open2)
      why = "Erst das Ziel: " + ss.w1 + ".";
    else if (n === 3 && !ss.open3)
      why = "Erst Ziel und Auftrag: " + (ss.s1 !== "green" ? ss.w1 : ss.w2) + ".";
    if (why) {
      _o("stepNote").textContent = why;
      setTimeout(() => { _o("stepNote").textContent = ""; }, 6000);
      return;
    }
    otaGoStep(+t.dataset.step);
  });
  document.querySelectorAll("[data-job]").forEach(t => t.onclick = e => { e.preventDefault(); otaSetJob(t.dataset.job); });
  document.querySelectorAll("[data-otafs]").forEach(t => t.onclick = e => {
    e.preventDefault();
    document.querySelectorAll("[data-otafs]").forEach(x => x.classList.remove("active"));
    t.classList.add("active");
    otaFs = t.dataset.otafs;
    otaLoadFiles();
  });
  _o("otaSafe").onclick = () => { otaFast = false; _o("otaSafe").setAttribute("aria-pressed", "true");
    _o("otaFast").setAttribute("aria-pressed", "false"); _o("otaWin").disabled = true; otaRender(); };
  _o("otaFast").onclick = () => { otaFast = true; _o("otaSafe").setAttribute("aria-pressed", "false");
    _o("otaFast").setAttribute("aria-pressed", "true"); _o("otaWin").disabled = false; otaRender(); };
  _o("otaPa").oninput = () => { otaDev = null; otaDevMiss = false; otaDevAt = 0; otaAccAt = 0;
    otaDrv = null; otaGa = null; otaPmGuess = null; otaRender(); };
  _o("otaRead").onclick = () => otaArm("otaRead", "feat", { pa: _o("otaPa").value });
  _o("otaSearchToggle").onclick = () => {
    const d = _o("otaDrawer");
    d.classList.toggle("fm-hidden");
    _o("otaSearchToggle").textContent = d.classList.contains("fm-hidden") ? "Andere Adresse suchen" : "Suche schließen";
  };
  _o("otaFromPa").onclick = () => {
    const n = otaNorm(_o("otaPa").value);
    if (!n) return;
    const p = n.split(".");
    _o("otaArea").value = p[0]; _o("otaLine").value = p[1];
    otaRender();
  };
  _o("otaScan").onclick = () => {
    const sp = otaScanSpec();
    otaHits = []; otaScanLabel = sp.label; otaScanWhen = "";
    otaArm("otaScan", "scan", { pa: _o("otaPa").value, area: sp.a, line: sp.l, scope: sp.area ? "area" : "line" });
  };
  _o("otaScanStop").onclick = () => otaPost("cancel", {});
  _o("otaPm").onclick = () => {
    const on = otaPmGuess !== null ? otaPmGuess : !!(otaDev && otaDev.prog);
    otaPmWrite(!on);
  };
  _o("otaProps").onclick = () => { _o("otaTProps").classList.toggle("fm-hidden"); otaRender(); };
  _o("otaMore").onclick = () => { _o("otaTMore").classList.toggle("fm-hidden"); otaRender(); };
  _o("otaGa").onclick = () => otaArm("otaGa", "ga", { pa: _o("otaPa").value });
  _o("otaDrives").onclick = otaProbeDrives;
  _o("otaStart").onclick = otaStartJob;
  _o("otaAgain").onclick = otaStartJob;
  _o("otaReread").onclick = () => { otaGoStep(1); otaArm("otaRead", "feat", { pa: _o("otaPa").value }); };
  _o("otaStop").onclick = () => otaPost("cancel", {});
  _o("cxStop").onclick = () => otaPost("cancel", {});
  // The file that WAS transferred -- not the one currently selected.
  _o("otaTrigger").onclick = () => {
    otaVerBefore = (otaDev && otaDev.appVer) ? otaDev.appVer : "";
    otaArm("otaTrigger", "trigger", { pa: otaSentPa || _o("otaPa").value, path: otaSentFile || "" });
  };
  // The chain: log in (or programming mode) -> read the state back -> apply. It stops and says so when
  // the target still refuses afterwards, instead of sending a second time blind.
  _o("otaFixGo").onclick = () => {
    const pa = otaSentPa || _o("otaPa").value;
    const pwEl = _o("otaFixPw");
    const viaPw = !pwEl.classList.contains("fm-hidden");
    if (viaPw && !pwEl.value) { otaLog("Erst das Passwort eintragen", "ota-warn"); return; }
    const pw = pwEl.value;
    pwEl.value = "";                                  // einmal benutzt, nirgends behalten
    otaVerBefore = (otaDev && otaDev.appVer) ? otaDev.appVer : "";
    otaChain = [
      viaPw ? { id: "otaUnlock", path: "auth", params: () => ({ pa: pa }), body: () => pw }
            : { id: "otaPm", path: "progmode", params: () => ({ pa: pa, on: 1 }) },
      { id: "otaRead", path: "feat", params: () => ({ pa: pa }) },
      { id: "otaTrigger", path: "trigger", params: () => ({ pa: pa, path: otaSentFile || "" }),
        before: () => {
          if (otaDev && (otaDev.feat & 0x20)) {
            otaLog(viaPw ? "Das Ziel sperrt weiterhin — Passwort falsch?"
                         : "Das Ziel sperrt weiterhin — der Zugriff ist dort ganz abgeschaltet", "ota-err");
            return false;
          }
          return true;
        } }
    ];
    otaChainNext();
  };
  const unlock = _o("otaUnlock");
  if (unlock) {
    unlock.onclick = () => {
      const v = _o("otaPw").value;
      if (!v) { otaLog("Erst das Passwort eintragen", "ota-warn"); return; }
      _o("otaPw").value = "";                   // never keep it around
      otaArm("otaUnlock", "auth", { pa: _o("otaPa").value }, v);
    };
    _o("otaLock").onclick = () => otaArm("otaLock", "auth", { pa: _o("otaPa").value }, "");
    _o("otaCheck").onclick = () => otaArm("otaRead", "feat", { pa: _o("otaPa").value });
    _o("otaPw").onkeydown = e => { if (e.key === "Enter") unlock.click(); };
  }
  ["otaArea", "otaLine", "otaScope", "otaKb", "otaDst", "otaWin", "otaApply", "otaResume", "otaKeep", "otaDrive"]
    .forEach(id => { const e = _o(id); if (!e) return; e.onchange = otaRender; if (e.tagName === "INPUT") e.oninput = otaRender; });
  _o("logBox").open = false;
  otaSetJob("full");
  otaTick();
  setInterval(otaTick, 900);
  setInterval(() => { if (otaDevAt || otaAccAt) otaRender(); }, 5000);
}
document.addEventListener("DOMContentLoaded", otaInit);

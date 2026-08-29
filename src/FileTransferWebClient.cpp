#include "FileTransferConfig.h" // switches first -- every guard below depends on it

#ifdef OPENKNX_FTC_KNXOTA_WEB
    #include "FileTransferWebClient.h"
    #include "FileTransferClient.h"
    #include "webassets.h"
    #include <LittleFS.h>
    #ifdef OPENKNX_SDCARD
        #include "SdFileStore.h"
    #endif
    #ifdef OPENKNX_EXTFLASH
        #include "EfcFileStore.h"
    #endif

FileTransferWebClient openknxFtcWeb;

using OpenKNX::Network::WebRequest;
using OpenKNX::Network::WebResponse;
using OpenKNX::Network::Webserver;
using OpenKNX::Network::WEB_GET;
using OpenKNX::Network::WEB_POST;

// "5.0.3" -> 0x5003. Returns 0 for anything that is not three numbers, so a typo cannot become 0.0.0.
uint16_t FileTransferWebClient::parsePa(const std::string &s)
{
    unsigned a = 0, l = 0, d = 0;
    if (sscanf(s.c_str(), "%u.%u.%u", &a, &l, &d) != 3) return 0;
    if (a > 15 || l > 15 || d > 255) return 0;
    return (uint16_t)((a << 12) | (l << 8) | d);
}

std::string FileTransferWebClient::paText(uint16_t pa)
{
    char b[16];
    snprintf(b, sizeof(b), "%u.%u.%u", (pa >> 12) & 0x0F, (pa >> 8) & 0x0F, pa & 0xFF);
    return b;
}

// Device text goes into JSON: one stray quote or backslash from a path would break the whole answer.
static std::string jsonEsc(const char *v)
{
    std::string o;
    for (const char *p = v; p && *p; p++)
    {
        if (*p == '"' || *p == '\\') o += '\\';
        else if ((uint8_t)*p < 0x20) continue;
        o += *p;
    }
    return o;
}

// Sizes the way the file manager shows them -- a raw byte count says little about a 1 MB image.
static std::string fmtSize(uint32_t b)
{
    char t[24];
    if (b >= 1048576u) snprintf(t, sizeof(t), "%.1f MB", b / 1048576.0);
    else if (b >= 1024u) snprintf(t, sizeof(t), "%.1f KB", b / 1024.0);
    else snprintf(t, sizeof(t), "%u B", (unsigned)b);
    return t;
}

// ── Page ───────────────────────────────────────────────────────────────────────────────────────
void FileTransferWebClient::handlePage(WebRequest &req, WebResponse &res)
{
    // Only the shell. The page's markup lives in knxota.js, which sits gzipped in flash -- the same
    // text costs a third of what a string literal here costs (measured: 9836 B -> 2641 B). The build
    // switches travel as data attributes, because the script builds the page before the first poll.
    std::string h = "<div id='ota' data-sec='";
#ifdef OPENKNX_FTC_SECURITY
    h += "1";
#else
    h += "0";
#endif
    h += "' data-sd='";
#ifdef OPENKNX_SDCARD
    h += "1";
#else
    h += "0";
#endif
    h += "' data-efc='";
#ifdef OPENKNX_EXTFLASH
    h += "1";
#else
    h += "0";
#endif
    h += "'></div>";

    res.setLayout(true);              // the shared frame: sidebar, menu, stylesheets
    res.setActiveMenu("/knxota");
    res.send(h.c_str());
}


// ── Scan hits: kept here, because the client releases its listing when the sweep ends ──────────
void FileTransferWebClient::collectScanHits()
{
    auto *c = FileTransferClient::instance();
    if (c->status().phase != FtcPhase::Scan) return;
    for (const FtcEntry &e : c->listing())
    {
        const uint16_t pa = parsePa(e.name); // the sweep stores the PA as text in name[]
        if (!pa) continue;
        bool seen = false;
        for (ScanHit &h : _scanHits)
            if (h.pa == pa)
            {
                h.openKnx = h.openKnx || e.isOpenKnx; // the flag arrives in the post-sweep probe
                seen = true;
                break;
            }
        if (!seen && _scanHits.size() < SCAN_HIT_MAX)
            _scanHits.push_back({pa, (uint16_t)e.crc, e.isOpenKnx}); // the sweep parks the mask in crc
    }
}


// ── Drives: three separate FilesystemInfo commands, one per drive ──────────────────────────────
// A target without that drive ignores the prefix and answers with its internal filesystem, so the
// status byte is the only discriminator: 0x01 = a provider answered (KB), 0x00 = the internal one.
void FileTransferWebClient::collectDriveAnswer()
{
    if (_dfPending < 0) return;
    auto *c = FileTransferClient::instance();
    if (c->isBusy()) return; // still in flight
    const FtcFsInfo &f = c->fsInfo();
    DriveInfo &d = _drv[_dfPending];
    if (!f.valid) d.state = 1;                       // no usable answer -> treat as absent
    else if (_dfPending != 0 && !f.kb) d.state = 1;  // asked for sd//efc/, got the internal FS
    else
    {
        d.total = f.total;
        d.free = f.free;
        d.kb = f.kb;
        d.state = (f.total == 0) ? 3 : 2;
    }
    _dfPending = -1;
}


// ── Status: what the client really holds -- no field is served that is not backed by its state ─
void FileTransferWebClient::handleStatus(WebRequest &req, WebResponse &res)
{
    auto *c = FileTransferClient::instance();
    const FtcStatus &s = c->status();
    collectScanHits();
    collectDriveAnswer();

    const char *ph = "idle";
    switch (s.phase)
    {
        case FtcPhase::Ping: ph = "ping"; break;
        case FtcPhase::List: ph = "list"; break;
        case FtcPhase::Info: ph = "info"; break;
        case FtcPhase::Delete: ph = "delete"; break;
        case FtcPhase::Upload: ph = "upload"; break;
        case FtcPhase::Verify: ph = "verify"; break;
        case FtcPhase::Scan: ph = "scan"; break;
        case FtcPhase::Done: ph = "done"; break;
        case FtcPhase::Failed: ph = "failed"; break;
        default: break;
    }

    char buf[512];
    std::string out = "{";
    snprintf(buf, sizeof(buf),
             "\"phase\":\"%s\",\"busy\":%s,\"ok\":%s,\"message\":\"%s\","
             "\"done\":%u,\"total\":%u,\"bps\":%u,\"chunk\":%u,\"chunks\":%u,"
             "\"window\":%u,\"windowState\":%u,\"target\":\"%s\","
             "\"resends\":%u,\"verifies\":%u,\"crcErrors\":%u",
             ph, c->isBusy() ? "true" : "false", s.ok ? "true" : "false", jsonEsc(s.message).c_str(),
             (unsigned)s.done, (unsigned)s.total, (unsigned)s.bps, (unsigned)s.chunk,
             (unsigned)s.chunks, (unsigned)s.window, (unsigned)s.windowState,
             paText(s.target).c_str(), (unsigned)s.resends, (unsigned)s.verifies,
             (unsigned)s.crcErrors);
    out += buf;

    // What the target answered about itself. Only reported while it still belongs to the PA the page
    // asked for -- any other operation in between makes it stale, and a stale feature byte would put a
    // wrong answer on the password question.
    const FtcDeviceInfo &d = c->deviceInfo();
    const bool devFresh = d.valid && _featPa && s.target == _featPa;
    if (devFresh)
    {
        snprintf(buf, sizeof(buf),
                 ",\"dev\":{\"pa\":\"%s\",\"cls\":\"%s\",\"mask\":%u,\"ftm\":%u,\"feat\":%u,"
                 "\"prog\":%s,\"mfr\":%u,\"apdu\":%u,\"router\":%s",
                 paText(_featPa).c_str(), jsonEsc(d.cls).c_str(), (unsigned)d.mask,
                 (unsigned)d.ftmVersion, (unsigned)d.features, d.progMode ? "true" : "false",
                 (unsigned)d.manufacturer, (unsigned)d.maxApdu, d.isRouter ? "true" : "false");
        out += buf;
        if (d.haveOrder) { out += ",\"order\":\""; out += jsonEsc(d.order); out += "\""; }
        if (d.haveVersion)
        {
            snprintf(buf, sizeof(buf), ",\"appVer\":\"%u.%u\"", (unsigned)((d.version >> 6) & 0x1F),
                     (unsigned)(d.version & 0x3F));
            out += buf;
        }
        if (d.haveSerial)
        {
            out += ",\"serial\":\"";
            for (int i = 0; i < 6; i++) { snprintf(buf, sizeof(buf), "%s%02X", i ? " " : "", d.serial[i]); out += buf; }
            out += "\"";
        }
        if (d.haveHw)
        {
            out += ",\"hw\":\"";
            for (int i = 0; i < 6; i++) { snprintf(buf, sizeof(buf), "%s%02X", i ? " " : "", d.hardware[i]); out += buf; }
            out += "\"";
        }
        if (d.haveDownloads) { snprintf(buf, sizeof(buf), ",\"dl\":%u", (unsigned)d.downloads); out += buf; }
        if (d.haveBusVolt) { snprintf(buf, sizeof(buf), ",\"volt\":%u", (unsigned)d.busVoltmV); out += buf; }
        snprintf(buf, sizeof(buf), ",\"tabs\":[%u,%u,%u,%u]}", (unsigned)d.appState,
                 (unsigned)d.addrTableState, (unsigned)d.assocTableState, (unsigned)d.goTableState);
        out += buf;
    }

    snprintf(buf, sizeof(buf),
             ",\"scan\":{\"running\":%s,\"found\":%u,\"at\":\"%s\",\"done\":%u,\"total\":%u,\"hits\":[",
             s.phase == FtcPhase::Scan ? "true" : "false", (unsigned)c->scanFound(),
             paText(c->scanCurrentPa()).c_str(),
             (unsigned)(s.phase == FtcPhase::Scan ? s.done : 0),
             (unsigned)(s.phase == FtcPhase::Scan ? s.total : 0));
    out += buf;
    for (size_t i = 0; i < _scanHits.size(); i++)
    {
        snprintf(buf, sizeof(buf), "%s{\"pa\":\"%s\",\"mask\":%u,\"ok\":%s}", i ? "," : "",
                 paText(_scanHits[i].pa).c_str(), (unsigned)_scanHits[i].mask,
                 _scanHits[i].openKnx ? "true" : "false");
        out += buf;
    }
    out += "]}";

    // The target's drives, as far as they were asked for. Never a guess: state 0 means "not asked".
    out += ",\"drives\":[";
    for (int i = 0; i < 3; i++)
    {
        snprintf(buf, sizeof(buf), "%s{\"state\":%u,\"total\":%u,\"free\":%u,\"kb\":%s}",
                 i ? "," : "", (unsigned)_drv[i].state, (unsigned)_drv[i].total,
                 (unsigned)_drv[i].free, _drv[i].kb ? "true" : "false");
        out += buf;
    }
    snprintf(buf, sizeof(buf), "],\"drivesPa\":\"%s\",\"drivesBusy\":%s",
             paText(_drvPa).c_str(), _dfPending >= 0 ? "true" : "false");
    out += buf;

    // Group addresses: served straight from the client, because it keeps them -- but its storage is
    // shared with the fast-upload bitmap, so a transfer invalidates them and the page says so.
    uint16_t gaN = 0;
    const FtcGaEntry *ga = c->groupObjects(gaN);
    snprintf(buf, sizeof(buf), ",\"ga\":{\"pa\":\"%s\",\"lost\":%s,\"rows\":[",
             paText(_gaPa).c_str(), (_gaLost && _gaPa) ? "true" : "false");
    out += buf;
    if (!(_gaLost && _gaPa))
        for (uint16_t i = 0; i < gaN; i++)
        {
            snprintf(buf, sizeof(buf), "%s{\"ga\":%u,\"co\":%u,\"flags\":%u,\"prio\":%u,\"size\":%u,\"cfg\":%s}",
                     i ? "," : "", (unsigned)ga[i].ga, (unsigned)ga[i].co, (unsigned)ga[i].flags,
                     (unsigned)ga[i].prio, (unsigned)ga[i].sizeCode, ga[i].cfgValid ? "true" : "false");
            out += buf;
        }
    out += "]}";

    // The finished job, for the summary. Set once at the end of a transfer and kept until the next one.
    const FtcTransferResult &r = c->transferResult();
    const FtcTransferSetup &u = c->transferSetup();
    if (r.valid)
    {
        snprintf(buf, sizeof(buf),
                 ",\"result\":{\"kind\":%u,\"ok\":%s,\"pa\":\"%s\",\"bytes\":%u,\"chunks\":%u,"
                 "\"ms\":%u,\"bps\":%u,\"verify\":%u,\"mode\":%u,\"resumed\":%u,\"retries\":%u,"
                 "\"cleanup\":%u,\"file\":\"%s\",\"apply\":%s}",
                 (unsigned)r.kind, r.ok ? "true" : "false", paText(r.target).c_str(),
                 (unsigned)r.bytes, (unsigned)r.chunks, (unsigned)r.elapsedMs, (unsigned)r.avgBps,
                 (unsigned)r.verify, (unsigned)r.mode, (unsigned)r.resumedBytes, (unsigned)r.retries,
                 (unsigned)r.cleanup, jsonEsc(u.valid ? u.local : "").c_str(), u.willApply ? "true" : "false");
        out += buf;
    }

    out += "}";
    res.setContentType("application/json");
    res.send(out.c_str());
}


// ── Local file list: own walk, so the KNX-side directory cursor is never disturbed ─────────────
void FileTransferWebClient::handleFiles(WebRequest &req, WebResponse &res)
{
    const std::string fs = req.getQueryParam("fs");
    const std::string job = req.getQueryParam("job");
    const bool wantDelta = (job == "delta");
    std::string out = "[";
    bool first = true;
    // The tab decides what belongs in the list: .okd is a patch, everything else must at least look
    // like an image. The accepted endings are named on the page, so nothing vanishes without a word.
    auto endsWith = [](const char *name, const char *ext) {
        const size_t n = strlen(name), e = strlen(ext);
        return n > e && strcasecmp(name + n - e, ext) == 0;
    };
    auto add = [&](const char *name, uint32_t size) {
        const bool okd = endsWith(name, ".okd");
        if (okd != wantDelta) return;
        if (!wantDelta && !(endsWith(name, ".bin") || endsWith(name, ".uf2") || endsWith(name, ".gz")))
            return;
        if (!first) out += ",";
        first = false;
        out += "{\"name\":\"";
        out += name;
        out += "\",\"size\":\"" + fmtSize(size) + "\",\"bytes\":" + std::to_string(size) + "}";
    };
    if (fs == "sd")
    {
#ifdef OPENKNX_SDCARD
        char nm[64];
        if (sd::fileStore.dirOpen("/"))
        {
            int t;
            uint32_t sz = 0;
            while ((t = sd::fileStore.dirNext(nm, sizeof(nm), &sz)) > 0)
                if (t == 1) add(nm, sz);
            sd::fileStore.dirClose();
        }
#endif
    }
    else if (fs == "efc")
    {
#ifdef OPENKNX_EXTFLASH
        char nm[64];
        if (efc::fileStore.dirOpen("/"))
        {
            int t;
            uint32_t sz = 0;
            while ((t = efc::fileStore.dirNext(nm, sizeof(nm), &sz)) > 0)
                if (t == 1) add(nm, sz);
            efc::fileStore.dirClose();
        }
#endif
    }
    else
    {
        File d = LittleFS.open("/", "r");
        for (File f = d.openNextFile(); f; f = d.openNextFile())
            if (!f.isDirectory()) add(f.name(), (uint32_t)f.size());
    }
    out += "]";
    res.setContentType("application/json");
    res.send(out.c_str());
}


// ── Arming: hand the job to the client and answer at once ──────────────────────────────────────
void FileTransferWebClient::handleStart(WebRequest &req, WebResponse &res)
{
    auto *c = FileTransferClient::instance();
    if (c->isBusy())
    {
        res.setStatus(409);
        res.send("Es läuft bereits ein Auftrag");
        return;
    }
    const uint16_t pa = parsePa(req.getQueryParam("pa"));
    if (!pa)
    {
        res.setStatus(400);
        res.send("Physikalische Adresse nicht lesbar");
        return;
    }
    const std::string job = req.getQueryParam("job");
    const uint8_t mode = req.getQueryParam("mode") == "fast" ? 1 : 0;
    const uint16_t win = (uint16_t)atoi(req.getQueryParam("win").c_str()); // 0 = let it regulate

    if (job == "perf")
    {
        const uint32_t kb = (uint32_t)atoi(req.getQueryParam("kb").c_str());
        // The drive the measurement writes to is a real argument -- the dropdown must reach it.
        const std::string drive = req.getQueryParam("drive");
        if (mode == 1) _gaLost = true;
        c->requestPerf(pa, (kb ? kb : 50) * 1024, 0, mode, req.getQueryParam("keep") == "1", win,
                       drive.c_str());
    }
    else
    {
        std::string path = req.getQueryParam("path");
        const std::string fs = req.getQueryParam("fs");
        if (path.empty())
        {
            res.setStatus(400);
            res.send("Keine Datei gewählt");
            return;
        }
        if (fs == "sd") path = "sd/" + path;
        else if (fs == "efc") path = "efc/" + path;
        else if (path[0] != '/') path.insert(0, "/");
#ifdef OPENKNX_FTC_DELTA_UPDATE
        if (job == "delta")
        {
            c->requestDeltaSend(pa, path.c_str());
            if (!c->isBusy())
            {
                res.setStatus(409);
                res.send("Die Differenz wurde nicht angenommen — kein gültiger OKD1-Kopf?");
                return;
            }
            res.setContentType("text/plain");
            res.send("OK");
            return;
        }
#endif
        if (mode == 1) _gaLost = true; // the fast bitmap and the group-address table share their storage
        // Where it lands ON THE TARGET. Empty = the target's internal filesystem, which is the only
        // place a firmware can be applied from.
        const std::string dst = req.getQueryParam("dst");
        std::string remote;
        if (dst == "sd" || dst == "efc")
        {
            const size_t sl = path.find_last_of('/');
            remote = dst + "/" + (sl == std::string::npos ? path : path.substr(sl + 1));
        }
        c->requestUpload(pa, path.c_str(), 0, req.getQueryParam("resume") != "1", mode,
                         req.getQueryParam("apply") == "1", remote.c_str(), win);
    }
    // The client refuses some jobs without ever entering a state (source missing, drive gone, path too
    // long) -- it only logs. Answering OK there would make the page report a run that never began, and
    // its balance would carry the PREVIOUS operation's message.
    if (!c->isBusy())
    {
        res.setStatus(409);
        res.send("Der Auftrag wurde nicht angenommen — Quelle nicht lesbar? Siehe Protokoll des Geräts.");
        return;
    }
    res.setContentType("text/plain");
    res.send("OK");
}


void FileTransferWebClient::handleCancel(WebRequest &req, WebResponse &res)
{
    FileTransferClient::instance()->requestCancel();
    res.setContentType("text/plain");
    res.send("OK");
}


void FileTransferWebClient::handleTrigger(WebRequest &req, WebResponse &res)
{
    if (FileTransferClient::instance()->isBusy())
    {
        res.setStatus(409);
        res.send("Es läuft bereits ein Auftrag");
        return;
    }
    const uint16_t pa = parsePa(req.getQueryParam("pa"));
    std::string path = req.getQueryParam("path");
    if (!pa || path.empty())
    {
        res.setStatus(400);
        res.send("Adresse oder Datei fehlt");
        return;
    }
    if (path[0] != '/') path.insert(0, "/"); // the target always keeps it in its root
    FileTransferClient::instance()->requestFwUpdate(pa, path.c_str());
    res.setContentType("text/plain");
    res.send("OK");
}


#ifdef OPENKNX_FTC_SECURITY
// The password arrives in the BODY, never in the request line: a query parameter would travel through
// every proxy log on the way. It is used once and stored nowhere.
void FileTransferWebClient::handleAuth(WebRequest &req, WebResponse &res)
{
    if (FileTransferClient::instance()->isBusy())
    {
        res.setStatus(409);
        res.send("Es läuft bereits ein Auftrag");
        return;
    }
    const uint16_t pa = parsePa(req.getQueryParam("pa"));
    if (!pa)
    {
        res.setStatus(400);
        res.send("Physikalische Adresse nicht lesbar");
        return;
    }
    std::string pw;
    if (req.body() && req.bodyLength()) pw.assign((const char *)req.body(), req.bodyLength());
    if (pw.empty()) FileTransferClient::instance()->requestLogout(pa);
    else FileTransferClient::instance()->requestLogin(pa, pw.c_str());
    pw.assign(pw.size(), '\0'); // do not leave it in the heap block this string hands back
    res.setContentType("text/plain");
    res.send("OK");
}
#endif


// scan / led / feat -- one entry point, the path says which. Each arms one client operation, so a
// running job must block them: the client serves one at a time and a second request would end the first.
void FileTransferWebClient::handleAction(WebRequest &req, WebResponse &res)
{
    auto *c = FileTransferClient::instance();
    const std::string p = req.getUri();
    const uint16_t pa = parsePa(req.getQueryParam("pa"));
    if (c->isBusy())
    {
        res.setStatus(409);
        res.send("Es läuft bereits ein Auftrag");
        return;
    }
    if (p.find("/feat") != std::string::npos)
    {
        if (!pa)
        {
            res.setStatus(400);
            res.send("Physikalische Adresse nicht lesbar");
            return;
        }
        _featPa = pa; // the answer is only reported back while it still belongs to this PA
        c->requestDeviceInfo(pa);
    }
    else if (p.find("/scan") != std::string::npos)
    {
        // The range is the page's, not the target's: an installer often sweeps a line he is not on.
        const std::string as = req.getQueryParam("area"), ls = req.getQueryParam("line");
        const uint16_t own = knx.individualAddress();
        unsigned a = as.empty() ? ((own >> 12) & 0x0F) : (unsigned)atoi(as.c_str());
        unsigned l = ls.empty() ? ((own >> 8) & 0x0F) : (unsigned)atoi(ls.c_str());
        if (a > 15) a = 15;
        if (l > 15) l = 15;
        const bool area = req.getQueryParam("scope") == "area";
        const uint16_t start = area ? (uint16_t)((a << 12) | 0x0001) : (uint16_t)((a << 12) | (l << 8) | 0x01);
        const uint16_t end = area ? (uint16_t)((a << 12) | 0x0FFF) : (uint16_t)((a << 12) | (l << 8) | 0xFF);
        _scanHits.clear();
        c->requestScan(start, end, area ? "Bereich" : "Linie", 1, false, true);
    }
    res.setContentType("text/plain");
    res.send("OK");
}


// One drive per call -- the client serves one command at a time, so the browser chains them.
void FileTransferWebClient::handleDrives(WebRequest &req, WebResponse &res)
{
    auto *c = FileTransferClient::instance();
    if (c->isBusy())
    {
        res.setStatus(409);
        res.send("Es läuft bereits ein Auftrag");
        return;
    }
    const uint16_t pa = parsePa(req.getQueryParam("pa"));
    if (!pa)
    {
        res.setStatus(400);
        res.send("Physikalische Adresse nicht lesbar");
        return;
    }
    const std::string fs = req.getQueryParam("fs");
    const int idx = (fs == "sd") ? 1 : (fs == "efc") ? 2 : 0;
    if (_drvPa != pa)
    {
        for (int i = 0; i < 3; i++) _drv[i] = DriveInfo();
        _drvPa = pa;
    }
    _dfPending = (int8_t)idx;
    c->requestFsInfo(pa, idx == 1 ? "sd/" : idx == 2 ? "efc/" : "");
    res.setContentType("text/plain");
    res.send("OK");
}


// The group-address table is its own bus run of minutes -- never folded into "Gerät lesen".
void FileTransferWebClient::handleGa(WebRequest &req, WebResponse &res)
{
    auto *c = FileTransferClient::instance();
    if (c->isBusy())
    {
        res.setStatus(409);
        res.send("Es läuft bereits ein Auftrag");
        return;
    }
    const uint16_t pa = parsePa(req.getQueryParam("pa"));
    if (!pa)
    {
        res.setStatus(400);
        res.send("Physikalische Adresse nicht lesbar");
        return;
    }
    _gaPa = pa;
    _gaLost = false;
    c->requestGroupComm(pa);
    res.setContentType("text/plain");
    res.send("OK");
}


// The target's programming mode (PID 54). One write, no answer -- the state is read back with the device.
void FileTransferWebClient::handleProgMode(WebRequest &req, WebResponse &res)
{
    auto *c = FileTransferClient::instance();
    const uint16_t pa = parsePa(req.getQueryParam("pa"));
    if (!pa)
    {
        res.setStatus(400);
        res.send("Physikalische Adresse nicht lesbar");
        return;
    }
    c->requestLed(pa, req.getQueryParam("on") == "1" ? 1 : 0);
    res.setContentType("text/plain");
    res.send("OK");
}


void FileTransferWebClient::setup()
{
    openknxNetwork.webserver.addMenuItem("knxOTA", "/knxota", 55);
    openknxNetwork.webserver.addRoute(WEB_GET, "/assets/knxota.css",
                                      Webserver::Asset(WebAssets::knxota_css_mime, WebAssets::knxota_css_gz,
                                                       sizeof(WebAssets::knxota_css_gz)));
    openknxNetwork.webserver.addRoute(WEB_GET, "/assets/knxota.js",
                                      Webserver::Asset(WebAssets::knxota_js_mime, WebAssets::knxota_js_gz,
                                                       sizeof(WebAssets::knxota_js_gz)));
    openknxNetwork.webserver.addStylesheet("/assets/knxota.css");
    openknxNetwork.webserver.addJavaScript("/assets/knxota.js");

    openknxNetwork.webserver.addRoute(WEB_GET, "/knxota", [this](WebRequest &q, WebResponse &r) { handlePage(q, r); });
    openknxNetwork.webserver.addRoute(WEB_GET, "/knxota/status", [this](WebRequest &q, WebResponse &r) { handleStatus(q, r); });
    openknxNetwork.webserver.addRoute(WEB_GET, "/knxota/files", [this](WebRequest &q, WebResponse &r) { handleFiles(q, r); });
    openknxNetwork.webserver.addRoute(WEB_POST, "/knxota/start", [this](WebRequest &q, WebResponse &r) { handleStart(q, r); });
    openknxNetwork.webserver.addRoute(WEB_POST, "/knxota/cancel", [this](WebRequest &q, WebResponse &r) { handleCancel(q, r); });
    openknxNetwork.webserver.addRoute(WEB_POST, "/knxota/trigger", [this](WebRequest &q, WebResponse &r) { handleTrigger(q, r); });
#ifdef OPENKNX_FTC_SECURITY
    openknxNetwork.webserver.addRoute(WEB_POST, "/knxota/auth", [this](WebRequest &q, WebResponse &r) { handleAuth(q, r); });
#endif
    openknxNetwork.webserver.addRoute(WEB_POST, "/knxota/scan", [this](WebRequest &q, WebResponse &r) { handleAction(q, r); });
    openknxNetwork.webserver.addRoute(WEB_POST, "/knxota/feat", [this](WebRequest &q, WebResponse &r) { handleAction(q, r); });
    openknxNetwork.webserver.addRoute(WEB_POST, "/knxota/ga", [this](WebRequest &q, WebResponse &r) { handleGa(q, r); });
    openknxNetwork.webserver.addRoute(WEB_POST, "/knxota/drives", [this](WebRequest &q, WebResponse &r) { handleDrives(q, r); });
    openknxNetwork.webserver.addRoute(WEB_POST, "/knxota/progmode", [this](WebRequest &q, WebResponse &r) { handleProgMode(q, r); });
}
#endif

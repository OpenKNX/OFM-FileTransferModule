/**
 * @file        FileTransferClientConsole.cpp
 * @brief       "ftc" console: command parsing, validation and the colored help
 * @date        2026-07-16
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 */
#include "FileTransferClientConsole.h"

#ifdef OPENKNX_FTC
    #include "FileTransferClient.h"
    #include <ctype.h>
    #include <stdio.h>
    #include <stdlib.h>
    #include <string.h>

/**
 * @brief Transfer-mode keyword -> 0 safe/classic, 1 fast/windowed.
 *
 * Aliases: win/windowed -> fast; any unrecognised token -> safe.
 */
static uint8_t ftcParseModeWord(const char *t)
{
    if (strcmp(t, "fast") == 0 || strcmp(t, "win") == 0 || strcmp(t, "windowed") == 0) return 1;
    return 0;
}

/** @brief The categorized `ftc ?` help, built via the client's cooperative ftcOut queue (drained in loop()). */
void FileTransferClientConsole::showUsage()
{
    // Queued via the client's cooperative ftcOut (drained one budget/loop): a ~40-line block never blocks
    // on USB-CDC. The command column is wide (33) so nothing truncates (old printHelpLine cut off at 23).
    const uint8_t H = CONSOLE_HEADLINE_COLOR;
    static const char RULE[] = "================================================================================";
    auto &c = _client;
    c.ftcOut(H, "%s", RULE);
    c.ftcOut(H, "OpenKNX Embedded FileTransferClient - ftc commands");
    c.ftcOut(H, "%s", RULE);
    c.ftcOut(H, "Per-target:  ftc <pa> <cmd> [args]");
    c.ftcOut(0, "");

    c.ftcOut(H, "Presence & info");
    c.ftcOut(0, "  %-33s  %s", "ping", "Is the target there? (module-version round trip)");
#if OPENKNX_FTC_DEVICEINFO
    c.ftcOut(0, "  %-33s  %s", "info | i", "Device fingerprint: mask/class, FTM version, features");
    c.ftcOut(0, "  %-33s  %s", "info ga", "Group communication: GA table + com-object links (not on BCU1 / mask 0x0012)");
#endif
    c.ftcOut(0, "  %-33s  %s", "info <file>", "Size + CRC32 of one file");
    c.ftcOut(0, "  %-33s  %s", "df [sd|efc]", "Filesystem: total / used / free + usage bar (drive optional)");
    c.ftcOut(0, "  %-33s  %s", "ll [dir]", "List a directory: name, size, CRC32 (+ usage bar)");
    c.ftcOut(0, "  %-33s  %s", "ls [dir]", "List a directory: names only");
    c.ftcOut(0, "");

    c.ftcOut(H, "File & folder");
    c.ftcOut(0, "  %-33s  %s", "rm <file>", "Delete a file");
    c.ftcOut(0, "  %-33s  %s", "mkdir <dir>", "Create a directory");
    c.ftcOut(0, "  %-33s  %s", "rmdir <dir>", "Remove a directory");
    c.ftcOut(0, "  %-33s  %s", "mv <old> <new>", "Rename / move a file or directory");
    c.ftcOut(0, "  %-33s  %s", "format yes", "Erase the WHOLE filesystem (gated)");
    c.ftcOut(0, "");

#ifdef OPENKNX_FTC_SECURITY
    c.ftcOut(H, "Access (password-protected targets)");
    c.ftcOut(0, "  %-33s  %s", "login <pw>", "Unlock write actions on the target (password never sent in clear; stays open until the target's idle timeout)");
    c.ftcOut(0, "  %-33s  %s", "logout", "Lock the target's write actions again now");
    c.ftcOut(0, "");
#endif

    c.ftcOut(H, "Transfer");
    c.ftcOut(0, "  %-33s  %s", "send | upload <src> [pkg] [mode] [flags]", "Upload - auto-resume a partial; mode safe|fast, fast w<N> pins the window; flags below");
#if OPENKNX_FTC_DOWNLOAD
    c.ftcOut(0, "  %-33s  %s", "get | receive | download <rem> [local]", "Download a file (always fresh)");
#endif
    c.ftcOut(0, "  %-33s  %s", "perf [kb] [pkg] [mode] [sd|efc] [w<N>]", "Speed test: push a pattern, report B/s (sd|efc = target drive; fast w<N> = fixed window; keep = leave file)");
    c.ftcOut(0, "");

    c.ftcOut(H, "Locate & firmware");
    c.ftcOut(0, "  %-33s  %s", "led on|off|blink", "Drive the target's prog-mode LED (locate)");
    c.ftcOut(0, "  %-33s  %s", "fwupdate <file>", "Trigger the target to apply an uploaded firmware (reboots it)");
#ifdef OPENKNX_FTC_CONSOLE
    c.ftcOut(0, "  %-33s  %s", "console | con [maxbytes]", "Interactive console tunnel ('quit'/'exit' to leave; maxbytes caps per-drain output for small tunnels)");
#endif
    c.ftcOut(0, "");

    c.ftcOut(H, "Global:  ftc <cmd>");
#if OPENKNX_FTC_SCAN
    c.ftcOut(0, "  %-33s  %s", "scan [a.l | a b] [deep] [ets]", "Discover devices (ets = connection-oriented, finds more)");
#endif
    c.ftcOut(0, "  %-33s  %s", "     [openknx | info] [save <path>]", "openknx=flag OpenKNX (mfr 0x00FA)  info=full per-device info  save=write CSV to /|sd/|efc/");
    c.ftcOut(0, "  %-33s  %s", "retry [max|transfer|backoff [v]]", "Show / set retry tuning");
    c.ftcOut(0, "  %-33s  %s", "s | status, c | cancel", "Status / cancel of the running job");
    c.ftcOut(0, "  %-33s  %s", "?", "This help");
    c.ftcOut(0, "");

    c.ftcOut(0, "  [mode]  = safe (default) | fast      perf only: fast w<N> pins the AIMD window (no end-burst overrun)   [pkg] = auto (default, 254 + degrades) or 16..254");
    c.ftcOut(0, "  [flags] = apply . verbose . no-resume (send) . keep (default) . verbose (perf | receive)");
    c.ftcOut(0, "  remote drive prefix (routed at the target): / (LittleFS) | sd/ (SD) | efc/ (ExtFlash) - df ll ls rm mkdir rmdir mv info get perf");
    c.ftcOut(0, "  send <src> / get <rem> [local]: the SOURCE / local-sink path takes the same prefix; send's remote name is always /<basename>");
    c.ftcOut(0, "");
    c.ftcOut(H, "Examples:");
    c.ftcOut(0, "  %-42s  %s", "ftc 5.0.3 send sd/app.bin.gz apply", "Upload from SD + auto-apply (RP target)");
    c.ftcOut(0, "  %-42s  %s", "ftc 5.0.3 upload /fw.bin auto fast verbose", "Upload from LittleFS, fast, log 1s progress");
    c.ftcOut(0, "  %-42s  %s", "ftc 5.0.3 receive /cfg.json sd/cfg.json", "Download a target file to SD");
    c.ftcOut(0, "  %-42s  %s", "ftc 5.0.3 perf 50 fast", "50 KB speed test, fast mode (no file needed)");
#ifdef OPENKNX_FTC_CONSOLE
    c.ftcOut(0, "  %-42s  %s", "ftc 5.0.3 console", "Interactive console tunnel over KNX");
#endif
#if OPENKNX_FTC_SCAN
    c.ftcOut(0, "  %-42s  %s", "ftc scan 1.1 ets openknx", "Scan and discover OpenKNX devices on line 1.1 (connection-oriented)");
    c.ftcOut(0, "  %-42s  %s", "ftc scan 1.1 ets save /scan.csv", "Discover devices on line 1.1 and write CSV to LittleFS");
#endif
    c.ftcOut(H, "%s", RULE);
}

void FileTransferClientConsole::showStatus()
{
    static const char *const kPhase[] = {"idle", "ping", "list", "info", "delete",
                                         "upload", "verify", "scan", "done", "failed"};
    const FtcStatus &s = _client.status();
    auto &l = openknx.logger;
    l.logWithPrefixAndValues("FTC", "status: %s  target %u.%u.%u  \"%s\"", kPhase[(uint8_t)s.phase],
                             FTC_PA_ARGS(s.target), s.path);
    if (s.total)
    {
        const uint16_t p = s.percentX100();
        l.logWithPrefixAndValues("FTC", "  %u.%02u %%  (%u / %u B)  %u B/s  chunk %u/%u", p / 100, p % 100,
                                 (unsigned)s.done, (unsigned)s.total, s.bps, s.chunk, s.chunks);
    }
    if (s.phase == FtcPhase::Done || s.phase == FtcPhase::Failed)
        l.logWithPrefixAndValues("FTC", "  result: %s  crc=0x%08X  %s", s.ok ? "OK" : "FAILED", (unsigned)s.crc,
                                 s.message);
    else if (s.message[0])
        l.logWithPrefixAndValues("FTC", "  %s", s.message);
}

/**
 * @brief `ftc scan ...` -- the only command with no PA in front (it discovers PAs).
 *
 * Forms: bare (own line), <a.l>, <a.l.d>, <from> <to> range, `full yes` (gated once),
 * `area <a> yes ...` (gated twice).
 */
#if OPENKNX_FTC_SCAN
void FileTransferClientConsole::showScan(const std::string &cmd)
{
    if (_client.isBusy())
    {
        openknx.logger.logWithPrefix("FTC", "busy -- a transfer/console/scan is already running (ftc cancel to stop)");
        return;
    }

    char a1[24] = {0}, a2[24] = {0};
    const int ntok = sscanf(cmd.c_str(), "ftc scan %23s %23s", a1, a2);

    // "deep [N]" anywhere = deep scan: N passes (default 5), union across passes -- robust over IP.
    const char *dp = strstr(cmd.c_str(), " deep");
    uint8_t sw = 1;
    if (dp != nullptr)
    {
        unsigned n = 0;
        sw = (sscanf(dp, " deep %u", &n) == 1 && n >= 2 && n <= 20) ? (uint8_t)n : 5;
    }
    // "ets" anywhere = connection-oriented (ETS-parity) scan: T_Connect per address, finds BCU1/BCU2.
    const bool co = strstr(cmd.c_str(), " ets") != nullptr;
    // "info" anywhere = full per-device info probe (implies openknx); "openknx" = light mfr-only probe.
    const bool infoFlag = strstr(cmd.c_str(), " info") != nullptr;
    const bool okFlag = infoFlag || strstr(cmd.c_str(), " openknx") != nullptr;
    // "save <path>" anywhere = write the listing to a backend CSV (path stops at the next space).
    char savePath[80] = {0};
    const char *svp = strstr(cmd.c_str(), " save ");
    if (svp != nullptr) sscanf(svp, " save %79s", savePath);
    // "deep"/"ets"/"openknx"/"info"/"save" as a 2nd token are keywords, not a range end.
    const bool a2IsEnd = (ntok >= 2 && strcmp(a2, "deep") != 0 && strcmp(a2, "ets") != 0 &&
                          strcmp(a2, "openknx") != 0 && strcmp(a2, "info") != 0 && strcmp(a2, "save") != 0);

    // ftc scan [deep] [ets] [openknx|info] [save <path>]  -> own line
    if (ntok <= 0 || strcmp(a1, "deep") == 0 || strcmp(a1, "ets") == 0 ||
        strcmp(a1, "openknx") == 0 || strcmp(a1, "info") == 0 || strcmp(a1, "save") == 0)
    {
        const uint16_t own = knx.individualAddress();
        const uint16_t base = own & 0xFF00;
        _client.requestScan((uint16_t)(base | 0x01), (uint16_t)(base | 0xFF), "own line", sw, co, okFlag, infoFlag, savePath);
        return;
    }

    // ftc scan full [yes]
    if (strcmp(a1, "full") == 0)
    {
        if (strcmp(a2, "yes") != 0)
        {
            openknx.logger.logWithPrefix("FTC", "full scan sweeps the WHOLE bus (65535 addresses, can take ~40+ min).");
            openknx.logger.logWithPrefix("FTC", "confirm with:  ftc scan full yes");
            return;
        }
        _client.requestScan(0x0001, 0xFFFF, "full bus", sw, co, okFlag, infoFlag, savePath);
        return;
    }

    if (strcmp(a1, "area") == 0)
    {
        unsigned ar = 0;
        char g1[24] = {0}, grest[80] = {0};
        const int m = sscanf(cmd.c_str(), "ftc scan area %u %23s %79[^\n]", &ar, g1, grest);
        if (m < 1 || ar > 15)
        {
            openknx.logger.logWithPrefix("FTC", "usage: ftc scan area <0..15> yes i really know what i am doing");
            return;
        }
        if (m < 2 || strcmp(g1, "yes") != 0)
        {
            openknx.logger.logWithPrefixAndValues("FTC", "area %u scan = all 16 lines (~4080 addresses).", ar);
            openknx.logger.logWithPrefixAndValues("FTC", "first confirm:  ftc scan area %u yes", ar);
            return;
        }
        if (strcmp(grest, "i really know what i am doing") != 0)
        {
            openknx.logger.logWithPrefixAndValues("FTC", "second confirm:  ftc scan area %u yes i really know what i am doing", ar);
            return;
        }
        char lbl[24];
        snprintf(lbl, sizeof(lbl), "area %u", ar);
        const uint16_t base = (uint16_t)(ar << 12);
        _client.requestScan((uint16_t)(base | 0x0001), (uint16_t)(base | 0x0FFF), lbl, sw, co, okFlag, infoFlag, savePath);
        return;
    }

    unsigned a = 0, l = 0, d = 0;
    const int f = sscanf(a1, "%u.%u.%u", &a, &l, &d);
    if (f == 2 && a <= 15 && l <= 15)
    {
        char lbl[24];
        snprintf(lbl, sizeof(lbl), "line %u.%u", a, l);
        const uint16_t base = (uint16_t)((a << 12) | (l << 8));
        _client.requestScan((uint16_t)(base | 0x01), (uint16_t)(base | 0xFF), lbl, sw, co, okFlag, infoFlag, savePath);
        return;
    }
    if (f == 3 && a <= 15 && l <= 15 && d <= 255)
    {
        const uint16_t start = (uint16_t)((a << 12) | (l << 8) | d);
        uint16_t end = start;
        if (a2IsEnd)
        {
            unsigned a2a = 0, l2 = 0, d2 = 0;
            if (sscanf(a2, "%u.%u.%u", &a2a, &l2, &d2) != 3 || a2a > 15 || l2 > 15 || d2 > 255)
            {
                openknx.logger.logWithPrefix("FTC", "bad end address -- expected a.l.d, e.g. ftc scan 5.0.1 5.0.50");
                return;
            }
            end = (uint16_t)((a2a << 12) | (l2 << 8) | d2);
        }
        _client.requestScan(start, end, a2IsEnd ? "range" : "single", sw, co, okFlag, infoFlag, savePath);
        return;
    }

    openknx.logger.logWithPrefix("FTC", "usage: ftc scan [a.l | from to] [deep] [ets] [openknx|info] [save <path>] | ftc scan full yes");
}
#endif

/** @brief Parse/dispatch one `ftc ...` command; PA first (it changes less often than the cmd), `cancel`/`?` take no PA. */
bool FileTransferClientConsole::processCommand(const std::string &cmd)
{
    if (cmd.compare(0, 4, "ftc ") != 0 && cmd != "ftc" && cmd != "ftc ?")
        return false;

    if (cmd == "ftc" || cmd == "ftc ?")
    {
        showUsage();
        return true;
    }

    char paStr[24] = {0};
    char sub[16] = {0};
    char arg[80] = {0};
    const int argc = sscanf(cmd.c_str(), "ftc %23s %15s %79s", paStr, sub, arg);

    // "ftc" with nothing but whitespace after it (e.g. "ftc " or "ftc   ") -> full help, like `ftc ?`.
    if (argc < 1 || strcmp(paStr, "?") == 0 || strcmp(paStr, "help") == 0)
    {
        showUsage();
        return true;
    }

    // "ftc cancel" / "ftc c" -- no PA, and it must bypass the busy check (it is the busy state it ends).
    if (argc >= 1 && (strcmp(paStr, "cancel") == 0 || strcmp(paStr, "c") == 0))
    {
        _client.requestCancel();
        return true;
    }
    if (argc >= 1 && (strcmp(paStr, "status") == 0 || strcmp(paStr, "s") == 0))
    {
        showStatus();
        return true;
    }
    // "ftc scan ..." -- no PA first; sweeps a range of addresses. Own gates for the wide sweeps.
#if OPENKNX_FTC_SCAN
    if (argc >= 1 && strcmp(paStr, "scan") == 0)
    {
        showScan(cmd);
        return true;
    }
#endif
    // "ftc retry [max|transfer|backoff] [value]" -- no PA; global retry tuning (get/set/help).
    if (argc >= 1 && strcmp(paStr, "retry") == 0)
    {
        _client.ftcRetryCmd(argc >= 2 ? sub : nullptr, argc >= 3 ? arg : nullptr);
        return true;
    }

    unsigned a = 0, l = 0, d = 0;
    if (sscanf(paStr, "%u.%u.%u", &a, &l, &d) != 3 || a > 15 || l > 15 || d > 255)
    {
        openknx.logger.logWithPrefixAndValues("FTC", "bad PA '%s' -- expected a.l.d, e.g. 5.0.3", paStr);
        return true;
    }
    const uint16_t pa = (uint16_t)((a << 12) | (l << 8) | d);

    if (argc < 2)
    {
        openknx.logger.logWithPrefix("FTC", "missing command -- try 'ftc ?'");
        return true;
    }

    // "ftc <pa> cancel|c" / "ftc <pa> status|s" also work, and must run even while busy.
    if (strcmp(sub, "cancel") == 0 || strcmp(sub, "c") == 0)
    {
        _client.requestCancel();
        return true;
    }
    if (strcmp(sub, "status") == 0 || strcmp(sub, "s") == 0)
    {
        showStatus();
        return true;
    }

    if (_client.isBusy())
    {
        openknx.logger.logWithPrefix("FTC", "busy -- a transfer/console/scan is already running (ftc cancel to stop)");
        return true;
    }

    if (strcmp(sub, "ping") == 0)
    {
        _client.requestPing(pa);
        return true;
    }
#ifdef OPENKNX_FTC_SECURITY
    if (strcmp(sub, "login") == 0) // open the target's write window; password -> MAC locally, never on the wire
    {
        if (argc < 3 || arg[0] == 0)
        {
            openknx.logger.logWithPrefix("FTC", "usage: ftc <pa> login <password>  (max 16 chars, no spaces)");
            return true;
        }
        _client.requestLogin(pa, arg);
        return true;
    }
    if (strcmp(sub, "logout") == 0) // close the target's write window now
    {
        _client.requestLogout(pa);
        return true;
    }
#endif
    if (strcmp(sub, "led") == 0)
    {
        uint8_t m = strcmp(arg, "on") == 0 ? 1 : strcmp(arg, "blink") == 0 ? 2
                                                                           : 0;
        _client.requestLed(pa, m);
        return true;
    }
#ifdef OPENKNX_FTC_CONSOLE
    if (strcmp(sub, "console") == 0 || strcmp(sub, "con") == 0) // interactive console tunnel (obj 160)
    {
        // Optional [maxbytes]: cap the per-drain output so the answer fits a constrained tunnel's max frame
        // (e.g. 16 for a small IP interface). Omitted/0 -> the full window; requestConsole() range-checks it.
        unsigned mb = 0;
        sscanf(cmd.c_str(), "ftc %*s %*s %u", &mb);
        _client.requestConsole(pa, (uint8_t)mb);
        return true;
    }
#endif
    if (strcmp(sub, "ll") == 0) // long list: name, size, CRC32
    {
        _client.requestList(pa, argc >= 3 ? arg : "/", true);
        return true;
    }
    if (strcmp(sub, "ls") == 0)
    {
        _client.requestList(pa, argc >= 3 ? arg : "/", false);
        return true;
    }
    if (strcmp(sub, "df") == 0)
    {
        _client.requestFsInfo(pa, argc >= 3 ? arg : ""); // "sd/" / "efc/" -> that provider; else LittleFS
        return true;
    }
    if (strcmp(sub, "info") == 0 || strcmp(sub, "i") == 0)
    {
#if OPENKNX_FTC_DEVICEINFO
        if (argc >= 3 && strcmp(arg, "ga") == 0) // group communication: GA table + com-object links
        {
            _client.requestGroupComm(pa);
            return true;
        }
        if (argc < 3) // no file -> device fingerprint (mask/class + FTM version + features)
            _client.requestDeviceInfo(pa);
        else
#endif
            _client.requestInfo(pa, arg);
        return true;
    }
    if (strcmp(sub, "rm") == 0)
    {
        if (argc < 3)
        {
            openknx.logger.logWithPrefix("FTC", "usage: ftc <pa> rm <remotepath>   e.g. ftc 5.0.3 rm /ftctest.bin");
            return true;
        }
        _client.requestDelete(pa, arg);
        return true;
    }
#if OPENKNX_FTC_DOWNLOAD
    if (strcmp(sub, "receive") == 0 || strcmp(sub, "download") == 0 || strcmp(sub, "get") == 0)
    {
        // ftc <pa> receive|download|get <remote> [local] [pkg] [verbose]  -- pull a file onto the local sink (SD).
        char rem[FTC_PATH_MAX] = {0}, a2[FTC_PATH_MAX] = {0}, a3[80] = {0}, a4[80] = {0};
        const int n = sscanf(cmd.c_str(), "ftc %*s %*s %" FTC_PATH_SCAN "s %" FTC_PATH_SCAN "s %79s %79s", rem, a2, a3, a4);
        if (n < 1)
        {
            openknx.logger.logWithPrefix("FTC", "usage: ftc <pa> receive <remotepath> [localpath] [pkg] [verbose]   e.g. ftc 5.0.3 receive fw.bin sd/fw.bin 16");
            return true;
        }
        // Order-tolerant trailing tokens: verbose/v is a flag anywhere; a pure number 16..240 is the download
        // chunk size (pkg, helps constrained tunnels); the first remaining non-flag is the local dest (default
        // = same path as remote). So `receive f l 16 v`, `receive f 16 l` and `receive f l` all work.
        bool verbose = false;
        bool noResume = false; // default: auto-resume a matching local partial
        uint8_t pkg = 0;
        const char *local = rem;
        const char *toks[3] = {a2, a3, a4};
        for (int i = 0; i < n - 1; i++)
        {
            const char *t = toks[i];
            if (strcmp(t, "verbose") == 0 || strcmp(t, "v") == 0) { verbose = true; continue; }
            if (strcmp(t, "no-resume") == 0 || strcmp(t, "noresume") == 0 || strcmp(t, "nr") == 0 ||
                strcmp(t, "fresh") == 0) { noResume = true; continue; } // force a fresh (truncating) download
            bool num = t[0] != 0;
            for (const char *p = t; *p; p++)
                if (!isdigit((unsigned char)*p)) num = false;
            if (num) { const int v = atoi(t); if (v >= 16 && v <= 240) pkg = (uint8_t)v; continue; } // [pkg]
            if (local == rem) local = t;
        }
        _client.setVerbose(verbose);
        _client.requestDownload(pa, rem, local, pkg, noResume);
        return true;
    }
#endif
    if (strcmp(sub, "format") == 0)
    {
        // Wipes the WHOLE filesystem (all files + folders). Cannot brick the device (app is in program
        // flash, not LittleFS), but it clears everything on the FS -> gate it with an explicit "yes".
        if (strcmp(arg, "yes") != 0)
        {
            openknx.logger.logWithPrefix("FTC", "format erases ALL files and folders on the target's filesystem.");
            openknx.logger.logWithPrefixAndValues("FTC", "confirm with:  ftc %u.%u.%u format yes", FTC_PA_ARGS(pa));
            return true;
        }
        _client.requestFormat(pa);
        return true;
    }
    if (strcmp(sub, "mkdir") == 0)
    {
        if (argc < 3)
        {
            openknx.logger.logWithPrefix("FTC", "usage: ftc <pa> mkdir <dir>   e.g. ftc 5.0.3 mkdir /logs");
            return true;
        }
        _client.requestMkdir(pa, arg);
        return true;
    }
    if (strcmp(sub, "rmdir") == 0)
    {
        if (argc < 3)
        {
            openknx.logger.logWithPrefix("FTC", "usage: ftc <pa> rmdir <dir>   e.g. ftc 5.0.3 rmdir /logs");
            return true;
        }
        _client.requestRmdir(pa, arg);
        return true;
    }
    if (strcmp(sub, "mv") == 0)
    {
        char newp[80] = {0};
        if (sscanf(cmd.c_str(), "ftc %*s %*s %79s %79s", arg, newp) != 2)
        {
            openknx.logger.logWithPrefix("FTC", "usage: ftc <pa> mv <old> <new>   e.g. ftc 5.0.3 mv /a.bin /b.bin");
            return true;
        }
        _client.requestRename(pa, arg, newp);
        return true;
    }

    if (strcmp(sub, "fwupdate") == 0 || strcmp(sub, "apply") == 0)
    {
        if (argc < 3)
        {
            openknx.logger.logWithPrefix("FTC", "usage: ftc <pa> fwupdate <remotepath>   e.g. ftc 5.0.3 fwupdate /fw.bin.gz");
            return true;
        }
        _client.requestFwUpdate(pa, arg);
        return true;
    }

    if (strcmp(sub, "perf") == 0)
    {
        // ftc <pa> perf [kb] [pkg] [mode] [flags] -- upload a generated pattern, report throughput. Trailing
        // tokens are order-tolerant: numbers fill kb then pkg, `auto` = pkg auto, a keyword sets the mode,
        // "keep" leaves the file (/ftcperf_<crc>.bin), "verbose"/"v" = 1 Hz progress, `w<N>` pins the fast window.
        char t[6][24] = {{0}};
        const int nt = sscanf(cmd.c_str(), "ftc %*s %*s %23s %23s %23s %23s %23s %23s", t[0], t[1], t[2], t[3], t[4], t[5]);
        unsigned kb = 0, pk = 0, seenNum = 0, fixedWnd = 0;
        uint8_t mode = 0;
        bool keep = false, verbose = false;
        char pdrive[8] = {0}; // "sd" / "efc" target drive (empty = LittleFS)
        for (int i = 0; i < nt; i++)
        {
            if (isdigit((unsigned char)t[i][0]))
            {
                if (seenNum++ == 0)
                    kb = (unsigned)atoi(t[i]);
                else
                    pk = (unsigned)atoi(t[i]);
            }
            else if ((t[i][0] == 'w' || t[i][0] == 'W') && isdigit((unsigned char)t[i][1]))
                fixedWnd = (unsigned)atoi(t[i] + 1); // w<N>: pin the fast window (no AIMD probe-up; loss still ratchets down)
            else if (strcmp(t[i], "keep") == 0)
                keep = true;
            else if (strcmp(t[i], "verbose") == 0 || strcmp(t[i], "v") == 0)
                verbose = true;
            else if (strcmp(t[i], "auto") == 0)
                pk = 0; // pkg auto (default) -- explicit token, leave pk at 0
            else if (strcmp(t[i], "nr") == 0 || strcmp(t[i], "no-resume") == 0 ||
                     strcmp(t[i], "noresume") == 0 || strcmp(t[i], "fresh") == 0)
                ; // no-resume: perf regenerates the pattern (fresh by default) -- accept the token, don't let it reset the mode
            else if (strcmp(t[i], "sd") == 0 || strcmp(t[i], "sd/") == 0 || strcmp(t[i], "efc") == 0 || strcmp(t[i], "efc/") == 0)
            {
                strncpy(pdrive, t[i], sizeof(pdrive) - 1); // perf target drive -> sd/ftcperf.bin etc. (accept "sd" or "sd/")
                char *sl = strchr(pdrive, '/');
                if (sl) *sl = '\0'; // "sd/" -> "sd" (ftcPerfCrcDone re-adds the slash)
            }
            else
            {
                // Only a RECOGNISED mode word (fast) or an explicit "safe" sets the mode -- otherwise a
                // stray trailing token (previously "nr") would silently reset a just-parsed mode to 0 (=safe).
                const uint8_t m = ftcParseModeWord(t[i]);
                if (m != 0 || strcmp(t[i], "safe") == 0) mode = m;
            }
        }
        if (kb == 0) kb = 16;
        _client.setVerbose(verbose);
        _client.requestPerf(pa, kb * 1024u, pk, mode, keep, (uint16_t)fixedWnd, pdrive); // pk 0 -> auto; w<N> pins the fast window; pdrive = sd/efc
        return true;
    }

    if (strcmp(sub, "send") != 0 && strcmp(sub, "upload") != 0)
    {
        openknx.logger.logWithPrefixAndValues("FTC", "unknown command '%s' -- try 'ftc ?'", sub);
        return true;
    }
    // ftc <pa> send|upload <src> [pkg] [mode] [flags]. src is mandatory (first token after sub); the
    // trailing tokens are order-tolerant: a number (or `auto`) is pkg, apply|on|yes / no|off|noapply
    // toggles the opt-in self-apply, no-resume|nr|fresh forces a fresh upload, verbose|v = 1 Hz progress,
    // and anything else is the mode (safe|fast).
    char src[FTC_PATH_MAX] = {0}, t1[24] = {0}, t2[24] = {0}, t3[24] = {0}, t4[24] = {0}, t5[24] = {0};
    const int nt = sscanf(cmd.c_str(), "ftc %*s %*s %" FTC_PATH_SCAN "s %23s %23s %23s %23s %23s", src, t1, t2, t3, t4, t5);
    if (nt < 1)
    {
        openknx.logger.logWithPrefix("FTC", "usage: ftc <pa> send <src> [pkg] [mode] [flags]   e.g. ftc 5.0.3 send sd/fw.bin");
        return true;
    }
    unsigned pk = 0;
    uint8_t mode = 0;
    bool apply = false;    // opt-in (default off): never reboot a target unless explicitly asked
    bool noResume = false; // default: auto-resume a matching partial
    bool verbose = false;
    unsigned fixedWnd = 0; // fast w<N>: pin the fast window (0 = adaptive AIMD)
    char target[32] = {0}; // explicit remote target ("sd/foo", "efc/foo", "/foo"); empty -> /<basename>
    const char *rest[5] = {t1, t2, t3, t4, t5};
    for (int i = 0; i < nt - 1; i++)
    {
        const char *tk = rest[i];
        if (strchr(tk, '/') != nullptr && target[0] == '\0')
        {
            strncpy(target, tk, sizeof(target) - 1);
            target[sizeof(target) - 1] = '\0';
        }
        else if (isdigit((unsigned char)tk[0]))
            pk = (unsigned)atoi(tk);
        else if (strcmp(tk, "auto") == 0)
            pk = 0; // pkg auto (default)
        else if (strcmp(tk, "apply") == 0 || strcmp(tk, "on") == 0 || strcmp(tk, "yes") == 0)
            apply = true;
        else if (strcmp(tk, "no") == 0 || strcmp(tk, "off") == 0 || strcmp(tk, "noapply") == 0)
            apply = false;
        else if (strcmp(tk, "no-resume") == 0 || strcmp(tk, "noresume") == 0 || strcmp(tk, "nr") == 0 || strcmp(tk, "fresh") == 0)
            noResume = true;
        else if (strcmp(tk, "verbose") == 0 || strcmp(tk, "v") == 0)
            verbose = true;
        else if ((tk[0] == 'w' || tk[0] == 'W') && isdigit((unsigned char)tk[1]))
            fixedWnd = (unsigned)atoi(tk + 1); // fast w<N>: pin the fast window (no AIMD probe-up; loss still ratchets down)
        else
        {
            const uint8_t m = ftcParseModeWord(tk);
            if (m != 0 || strcmp(tk, "safe") == 0) mode = m;
        }
    }
    // mode + apply + resume are echoed by the CONFIG box (Mode / Options rows) that requestUpload prints.
    // The client range-checks pkg and the path length against its own constants/buffers.
    _client.setVerbose(verbose);
    _client.requestUpload(pa, src, pk, noResume, mode, apply, target, (uint16_t)fixedWnd); // fast w<N> pins the window
    return true;
}

#endif

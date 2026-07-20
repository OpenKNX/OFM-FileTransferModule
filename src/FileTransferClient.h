#pragma once
/**
 * @file        FileTransferClient.h
 * @brief       KNX file transfer client -- pushes a file to another device's server, PA to PA
 * @version     0.0.1
 * @date        2026-07-16
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 */
#include "OpenKNX.h"

#ifdef OPENKNX_FTC
    #include "FileTransferClientConsole.h"

// KNX file transfer CLIENT: push a file to another OpenKNX device's KnxFileTransfer server, PA -> PA,
// no PC. Mirror of FileTransferModule (the server), same OFM. The source is read locally (a registered
// backend -- LittleFS/SD/efc -- or a built-in test pattern); the target writes its own LittleFS. `ftc ?`.

// Shared max length for every local/remote PATH buffer (client-side convention; the frame itself carries up
// to FTC_PKG_MAX). 96 = headroom for nested sd//efc/ paths over the ~22-char flat firmware names, without the
// vector-multiplied cost of FtcEntry::name (a single filename, kept at 64). Every path buffer sizes off this;
// all writes stay snprintf/strncpy-bounded, so a longer input truncates safely (never overflows).
static constexpr size_t FTC_PATH_MAX = 96;

// The source is reached through this callback triple so the client needs no storage header: the router
// registers implementations via registerFileBackend(); everything storage-specific stays router-side.
struct FtcFileSource
{
    int32_t (*open)(const char *path); // returns file size, or -1 on failure
    uint8_t (*read)(uint32_t offset, uint8_t *buf, uint8_t len);
    void (*close)();
};

// The mirror of FtcFileSource for a download: where the pulled bytes are written (a local backend).
// The router registers implementations via registerFileBackend(); download aborts if none resolves.
struct FtcFileSink
{
    bool (*open)(const char *path);                 // create/truncate; false on failure
    int (*write)(const uint8_t *buf, uint16_t len); // append; bytes written, or -1 on error
    void (*close)();
};

// A named local-storage backend. The client resolves a local path's prefix (e.g. "sd/x", "efc/x", or
// none) to one of these; the router registers only what it has. Prefix "" = the default backend.
struct FtcBackend
{
    const char *prefix; // "" = default (matched when no named prefix fits); "sd" / "efc" = named
    FtcFileSource src;
    FtcFileSink sink;
    bool (*available)() = nullptr;     // null => always available; else gate open() on it (card in / mounted)
    uint64_t (*freeBytes)() = nullptr; // null => skip the pre-write space check; else local free bytes
};

// --- Read-only surface for other front-ends (WebConfig, the display / DDC) ---------------------
// The console is just one caller of the request* API; a UI polls status() for progress + result and
// listing() for the last `ll`/`ls`. Everything here mirrors what the console logs, as plain data.
enum class FtcPhase : uint8_t
{
    Idle,
    Ping,
    List,
    Info,
    Delete,
    Upload,
    Verify,
    Scan,   // sweeping a PA range for present devices (DeviceDescriptor_Read)
    Done,   // last operation finished successfully (see status().ok / crc)
    Failed, // last operation aborted (see status().message)
};

struct FtcStatus
{
    FtcPhase phase = FtcPhase::Idle;
    uint16_t target = 0;    // target PA of the current/last operation
    uint32_t done = 0;      // bytes transferred so far (0..total) -> progress bar
    uint32_t total = 0;     // bytes to transfer (0 if not applicable)
    uint16_t bps = 0;       // measured throughput
    uint16_t chunk = 0;     // current chunk (1-based)
    uint16_t chunks = 0;    // total chunks
    bool ok = false;        // did the last finished operation verify / succeed?
    uint32_t crc = 0;       // last verify / info CRC-32
    char path[FTC_PATH_MAX] = {0};    // current / last file
    char message[40] = {0}; // short human status ("resuming", "up to date", "fs full?" ...)

    // Progress as hundredths of a percent, e.g. 4567 = 45.67 %  -> printf("%u.%02u %%", p/100, p%100).
    uint16_t percentX100() const { return total ? (uint16_t)((uint64_t)done * 10000ULL / total) : 0; }
};

struct FtcEntry
{
    char name[64] = {0};
    uint32_t size = 0;
    uint32_t crc = 0;
    bool isDir = false;
    bool hasInfo = false;    // ll fills size+crc; ls leaves them 0
    bool isOpenKnx = false;  // scan `openknx`/`info` probe: this device's manufacturer id == 0x00FA
};

// One queued console-output line for the cooperative drain (ftcOut / ftcDrainOut), so no single loop()
// pass blocks on USB-CDC long enough to trip the loop-time warning.
struct FtcOutLine
{
    uint8_t color = 0;
    char text[128] = {0};
};

class FileTransferClient : public OpenKNX::Module
{

  public:
    FileTransferClient();

    const std::string name() override;
    const std::string version() override;
    static FileTransferClient *instance();
    void setup(bool configured) override; // auto-registers the built-in local-storage backends (once)
    bool processCommand(const std::string cmd, bool diagnoseKo) override;
    void showHelp() override;
    void loop(bool configured) override;

    // Register a local-storage backend under a path prefix ("" = default). Bounded to 4; overflow ignored.
    // available()=null -> always available; freeBytes()=null -> no space pre-check. Built-ins self-register:
    // "/"=LittleFS here in setup(), "sd/"=SD and "efc/"=ext flash from their own module setups (OPENKNX_FTC).
    void registerFileBackend(const char *prefix, const FtcFileSource &src, const FtcFileSink &sink,
                             bool (*available)() = nullptr, uint64_t (*freeBytes)() = nullptr);

    // --- Action API: any front-end (console, WebConfig, DDC) calls these -----------------------
    // Callers must ensure !isBusy() first (cancel excepted); a rejected request logs the reason.
    bool isBusy() const { return _ftcState != FtcIdle; }
    void requestCancel();
    void requestPing(uint16_t pa);
    void requestList(uint16_t pa, const char *dir, bool detailed); // ll = detailed, ls = names only
    void requestInfo(uint16_t pa, const char *remotePath);
    void requestFsInfo(uint16_t pa);                                           // `ftc df` -- target LittleFS total/used/free with a usage bar
    void ftcRetryCmd(const char *sub, const char *val);                        // `ftc retry ...` -- show/set retry tuning (help/get/set)
    void requestDeviceInfo(uint16_t pa);                                       // `ftc <pa> info` with no file: mask/class + FTM version + features
    void requestGroupComm(uint16_t pa);                                        // `ftc <pa> info ga`: GA table + com-object links (ETS-style), via A_Memory_Read
    void requestLed(uint16_t pa, uint8_t mode);                                // `ftc <pa> led on|off|blink`: locate via the prog-mode LED (0=off 1=on 2=blink)
#ifdef OPENKNX_FTC_CONSOLE
    void requestConsole(uint16_t pa); // `ftc <pa> console`: open the interactive console tunnel (obj 160)
#endif
    void requestDelete(uint16_t pa, const char *remotePath);                   // rm  (file)
    void requestFormat(uint16_t pa);                                           // format: wipe the whole filesystem
    void requestMkdir(uint16_t pa, const char *dir);                           // mkdir
    void requestRmdir(uint16_t pa, const char *dir);                           // rmdir (directory)
    void requestRename(uint16_t pa, const char *oldPath, const char *newPath); // mv / rename
    // mode: 0 = safe/classic (default), 1 = fast/windowed, 2 = forget. A non-zero mode negotiates the
    // server's FAST capability first and silently falls back to classic if missing (ftcBeginFeatureProbe).
    // apply (opt-in, default false): after a verified upload, trigger the target to self-apply the image
    // (fwupdate -> reboot). Forced false for the perf/test sources so /ftctest.bin is never flashed.
    void requestUpload(uint16_t pa, const char *src, unsigned pkg, bool noResume, uint8_t mode = 0, bool apply = false);
    void requestPerf(uint16_t pa, uint32_t sizeBytes, unsigned pkg, uint8_t mode = 0, bool keep = false); // upload a test pattern, report B/s (keep = leave it as /ftcperf_<crc>.bin)
    void requestDownload(uint16_t pa, const char *remotePath, const char *localPath);  // pull to a local backend
    // `ftc <pa> fwupdate <remote>`: trigger the target to apply an already-uploaded firmware (reboots it).
    // FileInfo existence pre-check first, so a mistyped path cannot reboot a live coupler.
    void requestFwUpdate(uint16_t pa, const char *remotePath);
    // Sweep the inclusive PA range [startPa..endPa] with connectionless DeviceDescriptor_Read and list
    // who answers (mask -> device class). `sweeps` > 1 = deep scan: repeat and union the answers (IP
    // drops different ones each pass, so several passes catch nearly all). openknx/info/savePath are the
    // opt-in post-sweep options: openknx = flag OpenKNX devices (mfr 0x00FA), info = full per-device info,
    // savePath = write the listing to a backend CSV (all cooperative, never block loop()).
    void requestScan(uint16_t startPa, uint16_t endPa, const char *label, uint8_t sweeps = 1, bool co = false,
                     bool probeOpenKnx = false, bool info = false, const char *savePath = nullptr);

    // --- Read API: poll these from a UI (progress bar, result, file list) -----------------------
    const FtcStatus &status() const { return _status; }
    const std::vector<FtcEntry> &listing() const { return _ftcListing; } // ll/ls/scan result -- valid only WHILE the op runs; ftcFinish() releases it (poll from a live UI, not after completion)

    // Cooperative console-output primitive (drained one budget/loop in ftcDrainOut): also used by the
    // console for the framed `ftc ?` help so a ~40-line block never blocks the loop. Formats + enqueues one line.
    void ftcOut(uint8_t color, const char *fmt, ...);

    // Console `verbose`/`v` flag: progress every second instead of per decile (default off). Set by the
    // parser just before a send/perf/receive request; ftcStart deliberately does not clear it.
    void setVerbose(bool v) { _ftcVerbose = v; }

  private:
    void ftcRetryOrAbort(const char *why);
    void ftcSendInfo(); // FileInfo(43): ask the target for size + CRC32
    // rm / mkdir / rmdir / mv share one path: send a command with a filename payload and report the
    // 1-byte result. `verb` names the action in the log; `shownPath` is what to print.
    void ftcSimpleCmd(uint16_t pa, uint8_t cmd, const char *verb, const uint8_t *payload, uint8_t len,
                      const char *shownPath);
    const char *_ftcVerb = "rm"; // action word for the FtcDelete result line
    void ftcSendDirList();
    void ftcListAdvance(); // print dirs, FileInfo the next file, or finish with the footer
    static uint32_t ftcCrc32Posix(uint32_t crc, const uint8_t *data, size_t len, bool first);
    // Cooperative whole-prefix CRC-32 (VORGABE: never block loop()): CRC `target` source bytes a bounded
    // chunk per FtcCrcPrefix pass, optionally snapshotting the running CRC at `snapAt` (the resume chunk
    // boundary) as the fold seed. next: 0 = resume/up-to-date decision, 1 = perf precompute -> box + start.
    void ftcStartCrc(uint32_t target, uint32_t snapAt, uint32_t targetCrc, uint8_t next);
    void ftcResumeCrcDone(); // FtcCrcPrefix done, next 0: compare CRC vs target -> up-to-date / resume / restart
    void ftcPerfCrcDone();   // FtcCrcPrefix done, next 1: name the perf file + config box + ftcStart
    static const char *ftcResultName(uint8_t result);
    bool ftcDropDup(); // true if the just-arrived response is a mirrored duplicate (drop it), else stamp+accept

    // Every step is an async request/response round trip over TP, so this is a loop()-driven state
    // machine: the router must keep routing (and feed the 16s watchdog) while it waits. Nothing here
    // may block or delay().
    enum FtcState
    {
        FtcIdle,
        FtcSent,             // ping: ModuleVersion sent, waiting for the answer
        FtcInfo,             // FileInfo(43) sent by "ftc info" -- just report what comes back
        FtcVerify,           // FileInfo(43) sent right after an upload -- compare against _ftcSrcCrc
        FtcFeatureProbe,     // fast/forget only: CheckFeatures(102) sent, short 800ms gate before start
        FtcFastOpen,         // fast: cmd44 open sent, waiting for the open ack (0x00 / 0x42 / 0x4A)
        FtcFastStream,       // fast: the pump -- burst DATA frames (silent), gated by the TP FIFO + window
        FtcFastReport,       // fast: cmd45 report sent, waiting for the received-bitmap
        FtcFastResend,       // fast: re-send only the unset seqs of the current window, then re-report
        FtcFastClose,        // fast: cmd44 close sent, waiting for the 1-byte ack -> then verify
        FtcResumeInfo,       // FileInfo(43) sent BEFORE an upload -- work out where to continue
        FtcCrcPrefix,        // cooperative prefix CRC-32 (resume seed / perf precompute) -- a chunk per loop() pass
        FtcDirList,          // DirList(80) iterating -- collect one directory's entries
        FtcDirInfo,          // FileInfo(43) per collected file -- fill in its size + CRC32 (ll only)
        FtcFsInfo,           // FilesystemInfo(46) sent -- LittleFS total/used for `df`, the ll footer, or the pre-upload space check
        FtcDelete,           // FileDelete(42) sent -- waiting for the result byte
        FtcUploadOpen,       // FileUpload sequence 0 sent, waiting for the "file opened" result
        FtcUploadChunk,      // FileUpload sequence n sent, waiting for the echoed sequence + CRC
        FtcUploadClose,      // FileUpload 0xFFFF sent, waiting for the (empty) final answer
        FtcCancel,           // Cancel sent, letting the frame drain before disconnecting
        FtcUploadChunkRetry, // the chunk in _ftcTx failed (timeout/seq/CRC) -- send it again
        FtcScan,             // sweeping a PA range: pace probes, collect answers by their PA
        FtcScanCo,           // ETS-parity scan (`ets`): T_Connect per address, CO read -- finds BCU1/BCU2
        FtcDevDescr,         // device info: DeviceDescriptor_Read sent, waiting for the mask
        FtcDevVer,           // device info: ModuleVersion(100) sent, waiting for the FTM version
        FtcDevFeat,          // device info: CheckFeatures(102) sent, waiting for the feature flags
        FtcDevProp,          // device info: reading Device-Object properties (serial/order/hardware)
        FtcDevEnum,          // device info: probing object indices for their type (find app/tables)
        FtcDevLoad,          // device info: reading load states + app version from the found objects
        FtcGaConnect,        // group comm: T_Connect opening -- wait for the CO link before the memory walk (ETS reads memory connection-oriented)
        FtcGaRef,            // group comm: PropertyValue_Read PID_TABLE_REFERENCE of the current table
        FtcGaMem,            // group comm: A_Memory_Read walking the current table's blob
        FtcDownloadOpen,     // download: FileDownload open sent, waiting for the file size
        FtcDownloadChunk,    // download: FileDownload chunk sent, waiting for the data + CRC16
        FtcDownloadVerify,   // download: FileInfo(43) sent after the last chunk -- compare target CRC32 vs the received-stream CRC32
        FtcPerfCleanup,      // perf: FileDelete of the throwaway /ftcperf.bin sent, waiting for the result
        FtcApplyCheck,       // fwupdate (standalone): FileInfo(43) existence pre-check before triggering apply
        FtcApplyProbe,       // apply: CheckFeatures(102) sent, short gate to learn if the target can self-apply
        FtcScanPost,         // post-sweep OpenKNX/info probe + cooperative CSV write (Feature B/C); NOT the FtcScanCo SM
#ifdef OPENKNX_FTC_CONSOLE
        FtcConsoleProbe,     // console: CheckFeatures(102) sent, short gate -- does the target even have the console feature?
        FtcConsole           // console tunnel: finished lines go to the target (obj 160), its log ring streams back
#endif
    };

    // Fires in the KNX stack's dispatch context, so it only parks the bytes and lets loop() act -- a
    // send or file I/O from here would re-enter the application layer from inside its own dispatch.
    static void ftcOnResponse(uint8_t objectIndex, uint8_t propertyId, uint8_t *data, uint8_t length);
    // Same rule for a DeviceDescriptor_Response (scan): park the responder's PA + mask, act in loop().
    static void ftcOnDeviceDescriptor(uint16_t pa, uint8_t descriptorType, const uint8_t *data);
    // Same rule for a PropertyValue_Response (device info): park it, act in loop().
    static void ftcOnPropertyValue(uint16_t pa, uint8_t objectIndex, uint8_t propertyId, const uint8_t *data, uint8_t length);
    // Same rule for a MemoryResponse (group comm): place the table chunk at its true offset, act in loop().
    static void ftcOnMemory(uint16_t pa, uint16_t addr, const uint8_t *data, uint8_t len);
    void ftcDevSendProp();                         // send the property read for the current _devPropStep
    void scanRecord(uint16_t pa, uint16_t mask);   // one responder -> log line + bounded listing
    void scanReport();                             // footer with counts when the sweep is done
    static const char *ftcMaskName(uint16_t mask); // 0x07B0 -> "System B", 0x091A -> "TP/IP coupler" ...
    // --- post-sweep OpenKNX/info probe + CSV save (Feature B/C): all cooperative, run from FtcScanPost ---
    void ftcScanPostBegin();                          // enter FtcScanPost: count candidates, arm callbacks, open the CSV sink
    bool ftcScanProbeStart(const FtcEntry &e);        // launch one device probe (LIGHT PID-11 read or FULL info chain); false = unparseable PA
    void ftcScanPostConsume();                        // an in-flight probe finished: mark OpenKNX + log (green when 0x00FA) + stream its CSV row
    void ftcScanInfoBail();                           // FULL probe: an early device-info exit -> hand back to FtcScanPost (never aborts the scan)
    void ftcScanSaveOpen();                           // resolve + open the CSV sink and write the header row
    void ftcScanWriteRow(const FtcEntry &e, bool probed); // append one bounded CSV row via the sink (probed = use the _dev* identity)
    void ftcScanSaveFail();                           // a sink write failed: log once, close, stop saving (never wedges the scan)
    void ftcDevSendObjType();                      // probe the current object index's type (FtcDevEnum)
    void ftcDevBuildLoadQueue();                   // after enum: assemble the load-state / app-version reads
    void ftcDevSendLoad();                         // send the current queued load/version read (FtcDevLoad)
    static const char *ftcLoadName(uint8_t state); // 1 -> "loaded", 0 -> "unloaded" ...

    void ftcStart(uint16_t pa, bool upload, uint8_t mode = 0);
    bool ftcSend(uint8_t propertyId, uint8_t length);

    // --- fast-transfer negotiation (phase 1): capability probe + classic fallback ---------------
    // Read the TP transmit FIFO depth via the bau -- the fast-transfer pump's flow control (send while
    // below the high-water mark, drain below the low-water mark before a report/close).
    uint16_t ftcTxQueueSize();
    // Start (or resolve from cache) the CheckFeatures(102) probe for a fast/forget upload. Returns true
    // if a probe is in flight (-> FtcFeatureProbe), false if resolved synchronously -> caller runs classic.
    bool ftcBeginFeatureProbe();
    // Decide fast vs classic from the probed feature byte and gate on the chunk cap. Downgrades
    // _ftcMode to 0 (and logs once) on any miss; otherwise keeps the mode and logs the negotiation.
    void ftcGateFast(uint8_t features, bool answered);
    static const char *ftcModeName(uint8_t mode); // 0 -> "safe", 1 -> "fast", 2 -> "forget"
    void ftcSendUploadOpen();
    // After the resume decision: open the transfer via the fast path (mode 1/2) or the classic path
    // (mode 0). A single dispatcher so the classic call sites in FtcResumeInfo stay byte-for-byte.
    void ftcProceedToUpload();
    void ftcSendFastOpen();                            // cmd44 open (flags: resume?, keepBitmap for recovery; expectedChunks) -> FtcFastOpen
    void ftcFastOpenWindow();                          // freeze the current window's high edge _ftcWndEnd = min(base+wnd, chunks+1)
    void ftcSendFastData(uint16_t seq);                // SILENT cmd44 DATA frame (does NOT arm _ftcRespPending/_ftcSince)
    void ftcSendReport(uint16_t base, uint16_t count); // cmd45 report query (fresh nonce) -> FtcFastReport
    void ftcSendFastClose();                           // cmd44 close (FF FF) -> FtcFastClose
    void ftcSendNextChunk();
    void ftcSendClose();
    void ftcAbort(const char *reason);
    void ftcFinish();
    // --- firmware apply (fwupdate) : trigger the target to apply an uploaded image (opt-in) ---
    void ftcEnterApplyGate();  // use the cached feature byte now, else probe (CheckFeatures) -> FtcApplyProbe
    void ftcApplyDecide();     // feature byte known -> trigger self-apply (Update bit set) or log a skip; finishes
    void ftcTriggerFwUpdate(); // send FwUpdate(101) with the remote path (fire-and-forget) + loud log; finishes
    void ftcCloseSource();
    uint8_t ftcReadSource(uint32_t offset, uint8_t *buffer, uint8_t maxLength);

    FtcState _ftcState = FtcIdle;
    uint16_t _ftcTarget = 0;
    uint32_t _ftcSince = 0;

    // --- transfer mode + fast-capability negotiation (phase 1) ---
    uint8_t _ftcMode = 0; // 0 = safe/classic, 1 = fast/windowed, 2 = forget (set by ftcStart)
    // Per-PA feature cache: skip the re-probe for a known target (single slot -- one PA at a time). Only
    // a real answer is cached, never a timeout (transient, must not pin a device to classic forever).
    bool _ftcFeatValid = false; // is the cache below populated?
    uint16_t _ftcFeatPa = 0;    // PA the cached feature byte belongs to
    uint8_t _ftcFeatBits = 0;   // CheckFeatures(102) result byte for _ftcFeatPa

    // --- upload job ---
    bool _ftcUpload = false;      // false = ping (ModuleVersion round trip only)
    bool _ftcTestSource = false;  // true = generated RAM pattern instead of the SD file
    bool _ftcResume = false;      // FtcResumeInfo decided to pick up where a previous run left off
    bool _ftcNoResume = false;    // send no-resume: discard any target partial, upload fresh (truncate)
    bool _ftcPkgAuto = false;     // pkg auto: start at FTC_PKG_MAX (degrade one notch per fresh link-retry)
    bool _ftcIsPerf = false;      // perf test -> delete /ftcperf.bin from the target after verify
    bool _ftcPerfRemoved = false; // perf: did the cleanup delete succeed (for the summary box)
    bool _ftcPerfKeep = false;    // perf `keep`: skip the cleanup delete; file stays as /ftcperf_<crc>.bin
    bool _ftcApply = false;       // opt-in: after a verified upload, trigger the target to self-apply (fwupdate)
    uint16_t _ftcStartSeq = 1;    // first chunk to send (>1 only when resuming)
    uint32_t _ftcResumeBase = 0;  // bytes the target already had; not sent by this run
    uint32_t _ftcDupes = 0;       // late second copies of answers the target sends for every request
    char _ftcPath[FTC_PATH_MAX] = {0};      // remote path; also the local SD path unless _ftcTestSource
    uint8_t _ftcPayloadSize = 0;  // = pkg - 6, mirrored to the target as its _size
    uint16_t _ftcPayloadBase = 0; // original payload for this request -> pkg-auto degrade recomputes from this (survives retries)
    uint16_t _ftcSequence = 0;    // 1-based; 0 and 0xFFFF are the open/close markers
    uint16_t _ftcChunks = 0;
    uint32_t _ftcSize = 0;
    uint32_t _ftcDone = 0;
    uint32_t _ftcSrcCrc = 0; // CRC-32/POSIX of what we sent -- compared against the target's

    // --- fast transfer (phase 2 windowed + phase 3 forget) -- STATIC, ~1 KB ---------------------
    // Block-synchronous windows: stream [base,wndEnd), report, resend gaps, advance. _ftcWnd (AIMD) sizes
    // the NEXT window; _ftcWndEnd freezes the CURRENT high edge so halving on loss can't orphan tail seqs.
    static constexpr uint16_t FTC_BMP_BYTES = 1024; // = FTM_FAST_MAX_CHUNKS/8; mirror of the server bitmap
    uint16_t _ftcWnd = 8;                           // AIMD window size (grows +8 clean, halves on loss), bounded [MIN,MAX]
    uint16_t _ftcWndEnd = 0;                        // frozen high edge (exclusive) of the current window: [base, wndEnd)
    uint16_t _ftcNextSeq = 0;                       // next seq the stream pump will send
    uint16_t _ftcReportBase = 0;                    // low edge (1-based, inclusive) of the current window
    uint16_t _ftcResendCur = 0;                     // cursor while re-sending the unset seqs of a window
    uint8_t _ftcRecvBmp[FTC_BMP_BYTES];             // client mirror of the server's received-bitmap (absolute seq-1)
    uint32_t _ftcFoldWatermark = 0;                 // highest seq already folded into _ftcSrcCrc (fold-once, immune to resends)
    uint16_t _ftcPrevMissing = 0;                   // missing count of the previous report (union no-progress guard)
    uint16_t _ftcNoProgress = 0;                    // consecutive reports without the missing count shrinking
    uint16_t _ftcReportRetries = 0;                 // report-query timeouts spent (abort past FTC_REPORT_RETRIES)
    uint32_t _ftcRepWaitStart = 0;                  // [dbg] millis() of the first query of the current report round (retry re-sends keep it) -> per-window answer latency
    uint8_t _ftcReportNonce = 0;                    // bumped on every report send; the answer must echo it (staleness)
    uint32_t _ftcDeadline = 0;                      // wall-clock overall guard (millis()) -- backstop behind no-progress
    uint32_t _ftcPaceNext = 0;                      // forget (mode 2): millis() gate for the next send burst. forget has no
                                                    // per-chunk report and no backpressure over IP -> without pacing it blasts ~94 KB/s, target drops chunks.
    uint8_t _ftcTransferRetries = 0;                // whole-transfer auto-retries spent (bounded by _cfgTransferRetries); reset only on a fresh request
    bool _ftcRetryPending = false;                  // a transient upload abort armed a resume-retry -> FtcCancel re-runs the transfer after a backoff
    // Retry tuning -- runtime-settable via `ftc retry <max|transfer|backoff> [value]` (defaults below).
    static constexpr uint8_t FTC_MAX_RETRIES_DEF = 3;          // per-chunk retries (CRC/timeout) before that chunk fails
    static constexpr uint8_t FTC_TRANSFER_RETRIES_DEF = 8;     // whole-transfer auto-retries (transient abort -> resume)
    static constexpr uint32_t FTC_RETRY_BACKOFF_MS_DEF = 3000; // ms between transfer retries (let a busy/rebooting target settle)
    uint8_t _cfgMaxRetries = FTC_MAX_RETRIES_DEF;
    uint8_t _cfgTransferRetries = FTC_TRANSFER_RETRIES_DEF;
    uint32_t _cfgBackoffMs = FTC_RETRY_BACKOFF_MS_DEF;
    bool _ftcRecovering = false;    // forget -> report-based gap recovery in progress (re-open kept the bitmap)
    bool _ftcClassicRescue = false; // forget recovery also failed -> the one classic full resend is running

    // --- throughput measurement (measured from the wire, never estimated) ---
    uint32_t _ftcStartMs = 0;         // set when the open frame goes out, i.e. when bytes start flowing (RE-armed each retry attempt)
    uint32_t _ftcElapsedMs = 0;       // upload wall time, frozen at completion for the summary box
    uint32_t _ftcGrandStartMs = 0;    // true end-to-end start; survives auto-retries (0 until first open) -> "with retry" wall time
    uint32_t _ftcGrandElapsedMs = 0;  // end-to-end wall time (all attempts), frozen at completion; == _ftcElapsedMs when no retry
    uint32_t _ftcGrandResumeBase = 0; // _ftcResumeBase at the FIRST open -> end-to-end bytes = _ftcDone - this (survives retries)
    uint32_t _ftcData100Ms = 0;       // millis() when the CLOSE is sent = all payload delivered; the "pure transfer" end,
                                      // EXCLUDING the close-ack round-trip and the whole-file CRC verify (those vary 10..1000ms)
    uint32_t _ftcRetryLostMs = 0;     // cumulative dead window (stall + Cancel drain + backoff) charged to auto-retries -> subtracted for the transfer-only rate
    uint8_t _ftcNextPct = 10;         // next decile that gets a progress line
    uint32_t _ftcRateMarkMs = 0;      // timestamp of the last decile line -> lets the progress print an interval (instantaneous) B/s
    uint32_t _ftcRateMarkDone = 0;    // _ftcDone at the last decile line (pairs with _ftcRateMarkMs)
    // Variant-D progress state, shared by the fast/classic/download emit sites via ftcMaybeProgress:
    uint32_t _ftcLastProgMs = 0;      // millis() of the last SHOWN progress line -> interval rate + 1 Hz verbose gate
    uint32_t _ftcLastProgDone = 0;    // bytes done at the last shown line (pairs with _ftcLastProgMs)
    uint32_t _ftcPeakBps = 0;         // highest interval rate seen this transfer -> the result panel
    bool _ftcVerbose = false;         // `verbose`/`v`: emit progress every second (default off = per decile)
    uint32_t _ftcLastProgressMs = 0;  // millis() of the last forward byte-progress -> the dead window (stall+recovery) charged to a retry
    void ftcLogRate(const char *what, uint32_t elapsedMs);
    void ftcPrintSummary();  // one clean, prefix-less framed result block (upload/perf)
    // "Variant D" progress. ftcMaybeProgress computes cur/avg/peak (and _status.bps) EVERY call, then gates
    // an emit (initial line, then 1 Hz verbose / 10 s otherwise); ftcProgress renders it -- two aligned lines
    // when verbose, else one compact line -- via ftcOut.
    void ftcMaybeProgress(bool up, uint16_t seq, uint16_t chunks, uint32_t done, uint32_t size, uint32_t startMs);
    void ftcProgress(bool up, uint16_t pa, uint16_t seq, uint16_t chunks, uint32_t done, uint32_t size, uint32_t cur, uint32_t avg, bool verbose);
    void ftcBoxRule(char ch);      // result-panel divider: "+<ch * FTC_BOX_IW>+" (headline color)
    void ftcBoxRow(const char *s); // result-panel row: "|<s padded AND truncated to FTC_BOX_IW>|"
    // One framed, prefix-less CONFIG box at the start of an upload/perf/download (queued via ftcOut). A
    // null mode/framing/options skips that row (download); hasCrc adds "- CRC32 0x.." to Size (perf precompute).
    void ftcConfigBox(uint16_t pa, const char *title, const char *source, const char *target,
                      uint32_t size, bool hasCrc, uint32_t crc,
                      const char *mode, const char *framing, const char *options);
    uint16_t _ftcTxCrc = 0;  // CRC16/Modbus of the frame in flight, compared against the echo
    uint8_t _ftcRetries = 0; // attempts spent on the CURRENT chunk (see FTC_MAX_RETRIES)
    uint8_t _ftcTxLen = 0;
    uint8_t _ftcTx[256] = {0}; // holds [seq:2][len:1][payload:<=247] and the longer open frame

    // --- response handoff (written in stack context, read in loop) ---
    volatile bool _ftcRespPending = false;
    // Sized for the longest answer we parse: a FileDownload chunk [result:1][seq:2][readed:1][data:<=247]
    // [crc:2] ~253 bytes. Also covers DirList names and the short FileInfo/version answers.
    uint8_t _ftcResp[253] = {0};
    uint8_t _ftcRespLen = 0;
    uint8_t _ftcRespObj = 0;
    uint8_t _ftcRespProp = 0;
    uint32_t _ftcRespT = 0; // millis() of the last response we accepted -- to drop mirrored duplicates

#ifdef OPENKNX_FTC_CONSOLE
    // --- console tunnel (ftc <pa> console): logical session over obj 160, no knx-lib change ---
    static constexpr uint8_t CON_OBJECT_INDEX = 160;
    static constexpr uint8_t CON_PID_IN = 1;         // input / control (OPEN/CLOSE/line)
    static constexpr uint8_t CON_PID_OUT = 2;        // output drain (poll the target's log ring)
    static constexpr uint32_t CON_KEEP_MS = 3000;    // idle: poll OUT to fetch async logs + keep the session fresh
    uint8_t _conSub = 0;                             // 0 = idle in-session, 1 = await IN ack, 2 = draining OUT
    uint32_t _conKeepNext = 0;                       // millis() of the next idle keepalive poll
    uint32_t _conStartMs = 0;                        // millis() at conOpen() -> session duration on close (0 = never opened)
    void consoleFeedLine(const char *line);          // a finished local line -> remote; quit/exit/`ftc cancel` end the session
    static void consoleFeedLineStatic(const char *line); // Console line-sink trampoline -> instance()->consoleFeedLine
    void conSend(uint8_t pid, const uint8_t *payload, uint8_t len); // A_FunctionProperty_Command on obj 160 (arms _ftcRespPending)
    void conAfterProbe(uint8_t features, bool answered); // capability pre-flight result -> open the session or bail with a clear note
    void conOpen();                                      // send OPEN (obj 160), arm the sink + banner, enter FtcConsole
    void conClose(const char *reason, bool sendClose);   // detach the sink, optionally CLOSE the remote, banner + finish
#endif

    // --- scan (ftc scan): pace DeviceDescriptor_Read across a PA range, collect answers by PA ---
    // Answers carry the responder's own PA -> matched by address, not order, so probes pipeline. Parked
    // in a small SPSC ring (not a single slot: two devices can answer between two loop() ticks).
    struct FtcDdMsg
    {
        uint16_t pa;
        uint16_t mask;
    };
    static constexpr uint8_t FTC_DD_Q = 16; // power of two -> mask with (FTC_DD_Q - 1)
    FtcDdMsg _ftcDdQ[FTC_DD_Q];
    volatile uint8_t _ftcDdHead = 0; // written in stack context
    volatile uint8_t _ftcDdTail = 0; // written in loop
    uint32_t _scanNext = 0;          // next PA to probe (32-bit so the top of the range cannot wrap)
    uint32_t _scanEnd = 0;           // last PA to probe, inclusive
    uint32_t _scanProbed = 0;
    uint16_t _scanFound = 0;
    uint32_t _scanLastSend = 0;   // spacing timer between probes
    uint32_t _scanSpacingMs = 40; // gap between probes; larger cross-line so the IP path is not flooded
    uint32_t _scanDrainMs = 2500; // quiet time before finishing; longer for cross-line (IP-routed) scans
    uint8_t _scanSweeps = 1;      // total passes (deep scan > 1)
    uint8_t _scanSweep = 1;       // current pass
    uint32_t _scanStart = 0;      // first PA of the range (to restart each pass)
    char _scanLabel[24] = {0};    // "line 5.0", "full bus", "area 3" ... for the log/status
    // CO scan (`ets`): strictly serial, one address in flight (see FtcScanCo in loop()).
    bool _scanCo = false;      // this sweep is connection-oriented
    uint8_t _scanCoPhase = 0;  // 0 Connect, 1 WaitConnect, 2 WaitResp
    uint32_t _scanCoT0 = 0;    // per-phase deadline (connect ack / read answer)
    bool _scanCoGot = false;   // the address in flight answered -> present
    uint32_t _scanCoBusyT0 = 0; // phase-0 first "TP busy" instant -> abort the CO scan if the link stays owned (FTC_CO_BUSY_TMO)

    // --- post-sweep probe + CSV save (`openknx` / `info` / `save` flags): cooperative in FtcScanPost --
    // ONE PID read and at most a few file rows per loop() pass -> never blocks (Feature B/C). Attaches at
    // scanReport() (deferral) and in FtcScanPost only -- the FtcScanCo SM above is left untouched.
    bool _scanOpenKnx = false;      // openknx OR info flag -> run the post-probe (adds the CSV `openknx` column)
    bool _scanInfo = false;         // info flag -> FULL per-device info (reuses the device-info chain) + info CSV columns
    bool _scanPostDone = false;     // post phase finished -> scanReport() prints the summary + ftcFinish() may run
    bool _scanInfoActive = false;   // a FULL info chain is running as a scan probe (gates the FtcDevProp short-circuit)
    uint16_t _scanOkIdx = 0;        // cursor over _ftcListing during the post phase (one entry per pass)
    uint16_t _scanOkNum = 0;        // 1-based count of System B devices probed so far (progress "k/N")
    uint16_t _scanOkTotal = 0;      // total System B candidates (the "k/N" denominator)
    uint8_t _scanProbeInFlight = 0; // 0 = none, 1 = LIGHT (single PID-11 read), 2 = FULL (device-info chain)
    bool _scanProbeDone = false;    // the in-flight probe has completed -> FtcScanPost consumes it
    bool _scanProbeAnswered = false; // the current device answered (else empty CSV columns / "no answer")
    char _scanSavePath[FTC_PATH_MAX] = {0};   // `save <path>`: empty = do not save the listing
    bool _scanSinkOpen = false;     // the CSV sink is open (streamed, one row per device / per pass)
    bool _scanSaveErr = false;      // a sink write failed -> stop writing (logged once)
    uint16_t _scanSaveN = 0;        // CSV data rows written so far
    const FtcBackend *_scanSaveBe = nullptr; // resolved backend for the streamed CSV (sink.write / sink.close)

    // --- device info (ftc <pa> info, no file): chained DeviceDescriptor -> ModuleVersion -> CheckFeatures ---
    uint16_t _devMask = 0; // 0 = no DeviceDescriptor answer
    uint16_t _devVerMaj = 0, _devVerMin = 0, _devVerRev = 0;
    uint8_t _devFeat = 0; // bit0 Resume, bit1 Update
    bool _devHasMask = false, _devHasVer = false;
    // Shared device-info kickoff. fromScan = quiet (no standalone log/status) + the chain hands the
    // identity back to FtcScanPost after the 5 Device-Object PIDs instead of running the phase-2 enum.
    void ftcDevInfoBegin(uint16_t pa, bool fromScan);
    void ftcDevReport(); // print the assembled device-info block
    // --- group communication (ftc <pa> info ga): reuse the device-info discovery, then A_Memory_Read walk ---
    void ftcGaBeginWalk(); // open a T_Connect for the walk (ETS reads memory CO); connectionless fallback if it can't
    void ftcGaAdvance();   // send the next present table's PID_TABLE_REFERENCE read, or emit the report + finish
    void ftcGaParse();     // extract the just-walked table from _memBuf (address -> _gaList; assoc stays in _memBuf)
    void ftcGaReport();    // header + resolved GA list + com-object links, via ftcOut (cooperative)
    bool _gaConnected = false; // we opened a T_Connect for the memory walk -> ftcFinish() closes it (ours only, never an ETS session)

    // Device-Object (index 0) properties, read one after another in FtcDevProp:
    uint8_t _devPropStep = 0;    // which property in the read sequence we are on
    uint16_t _devMfr = 0;        // manufacturer id (serial number bytes 0..1)
    uint8_t _devSerial[6] = {0}; // full KNX serial number (mfr[2] + serial[4])
    uint8_t _devOrder[10] = {0}; // order number (ASCII device name)
    uint8_t _devHw[6] = {0};     // hardware type (0000 + app number + version)
    uint8_t _devFw[4] = {0};     // PID_VERSION raw
    uint8_t _devFwLen = 0;
    uint8_t _devProgMode = 0; // PID_PROG_MODE (0 = off, 1 = on)
    bool _devHasSerial = false, _devHasOrder = false, _devHasHw = false, _devHasFw = false, _devHasProg = false;

    // device-info phase 2: object enumeration + load states
    uint8_t _devObjProbe = 1;                                                     // object index being probed for its type
    int8_t _devIdxAddr = -1, _devIdxAssoc = -1, _devIdxApp = -1, _devIdxGrp = -1; // found indices (-1 = none)
    uint16_t _devAppMfr = 0, _devAppNum = 0;                                      // application program: manufacturer + number
    uint8_t _devAppVer = 0;                                                       // application program version byte
    bool _devHasApp = false;
    uint8_t _devLoad[4] = {0}; // load state: [0]addr [1]assoc [2]app [3]grp
    bool _devLoadHas[4] = {false, false, false, false};
    uint8_t _loadQObj[8] = {0}, _loadQPid[8] = {0}, _loadQN = 0, _loadQi = 0; // queued (obj,pid) reads
    // Property-Response handoff (single outstanding read, written in stack context, read in loop):
    volatile bool _propPending = false;
    uint8_t _propObj = 0, _propPid = 0, _propLen = 0;
    uint8_t _propData[16] = {0};

    // --- group communication (ftc <pa> info ga): walk the GA + association tables via A_Memory_Read (ETS path) ---
    static constexpr uint16_t FTC_GA_MAX_BYTES = 256; // bounded table-memory accumulator (per table)
    static constexpr uint8_t FTC_GA_MAX = 127;        // max GAs / association entries parsed (bounds every buffer write)
    bool _gaMode = false;              // requestGroupComm (true) vs requestDeviceInfo (false): shares the discovery chain
    uint8_t _gaWhich = 0;              // table being walked: 0 = address, 1 = association
    uint16_t _gaRef = 0;               // current table's memory base (PID_TABLE_REFERENCE, low word)
    uint16_t _gaGot = 0;               // bytes accumulated for the current table
    uint16_t _gaExpect = 0;            // total bytes to read (from the leading count), clamped to FTC_GA_MAX_BYTES
    bool _gaAssocValid = false;        // the association walk left valid bytes in _memBuf -> gate the report's KO section
    volatile bool _memPending = false; // a MemoryResponse is parked (stack ctx) -> consumed in loop()
    uint8_t _memBuf[FTC_GA_MAX_BYTES]; // current table's raw blob; ftcOnMemory writes each chunk at its true offset
    uint8_t _memLen = 0;               // length of the chunk ftcOnMemory just parked
    uint16_t _gaList[FTC_GA_MAX];      // resolved GAs from the address table (0-based: _gaList[t-1] = TSAP t, TSAP is 1-based)
    uint16_t _gaN = 0;                 // count in _gaList

    // --- locate (ftc <pa> led): drive the target's prog-mode LED; blink toggles from loop() while idle ---
    static constexpr uint32_t FTC_LED_BLINK_MS = 700; // half-period of the locate blink
    uint16_t _ledBlinkPa = 0;                         // 0 = no blink running; else the PA whose LED is toggling
    uint32_t _ledBlinkNext = 0;                       // millis() of the next toggle
    uint8_t _ledBlinkOn = 0;                          // LED state written on the last toggle

    // --- directory listing (ftc ll/ls): DirList collects entries, then FileInfo fills size+CRC ---
    // Cleared at ll/ls/scan start, filled during the walk, then RELEASED in ftcFinish() (swap-to-empty, not
    // just clear -> the grown capacity is returned to the heap; nothing reads it once the op completes).
    std::vector<FtcEntry> _ftcListing;
    char _ftcListDir[FTC_PATH_MAX] = {0}; // the directory being listed (for full-path FileInfo)
    uint16_t _ftcDirIdx = 0;    // cursor while filling in per-file details
    uint32_t _ftcListBytes = 0; // running total for the footer
    uint16_t _ftcListFiles = 0, _ftcListDirs = 0;
    // FilesystemInfo(46): target LittleFS capacity for `df`, the `ll` footer, and the pre-upload space gate.
    uint32_t _ftcFsTotal = 0, _ftcFsUsed = 0;                            // last FilesystemInfo answer (total, used bytes)
    uint8_t _ftcFsPurpose = 0;                                           // 0 = df, 1 = pre-upload space check, 2 = ll footer
    uint32_t _ftcTargetHave = 0;                                         // existing target file's size (FtcResumeInfo) -> overwrite credit for the space check
    // Cooperative prefix-CRC (FtcCrcPrefix) state -- CRC a chunk per loop() pass, never one blocking read:
    uint32_t _crcOff = 0;             // bytes CRC'd so far
    uint32_t _crcTarget = 0;          // total bytes to CRC (resume: `have`; perf: _ftcSize)
    uint32_t _crcSnapAt = 0xFFFFFFFF; // snapshot _ftcSrcCrc when _crcOff reaches here (resume chunk boundary); 0xFFFFFFFF = none
    uint32_t _ftcResumeSeedCrc = 0;   // CRC-32 at the chunk boundary -> the running fold's seed when resuming
    uint32_t _ftcTargetCrc = 0;       // the target's reported partial/file CRC, compared once the coop CRC finishes
    uint8_t _crcNext = 0;             // FtcCrcPrefix continuation: 0 = resume decision, 1 = perf start
    uint16_t _crcPerfPa = 0;          // perf continuation context (captured before the coop CRC runs)
    uint8_t _crcPerfMode = 0;
    bool _crcPerfKeep = false;
    uint16_t _crcPerfPkg = 0;
    bool _ftcSpaceChecked = false;                                       // the pre-upload space check has run for THIS transfer (once; survives a retry)
    void ftcSendFsInfo(uint8_t purpose);                                 // send FilesystemInfo(46) -> FtcFsInfo, remembering why
    void ftcPrintFsBar(uint32_t total, uint32_t used, bool full = true); // df=full block; ll footer=just the two bars
    bool _ftcListDetailed = true;                                        // ll -> per-file size+CRC; ls -> names only

    // Cooperative console output: multi-line ll/ls/df blocks are queued here and drained one line per
    // loop() pass (gated on freeLoopTime()) so no pass blocks on USB-CDC; the state machine waits meanwhile.
    std::vector<FtcOutLine> _ftcOut;
    size_t _ftcOutPos = 0;
    bool ftcDrainOut(); // emit a budget's worth; true = emitted this pass -> caller yields (ftcOut is public above)

    // --- read surface for UIs (mirrors the console logs as data) ---
    FtcStatus _status;
    void ftcStatusReset(FtcPhase phase, uint16_t pa, const char *path); // start of an operation
    void ftcStatusMsg(const char *msg);                                 // set a short human note

    // --- download job (ftc <pa> receive|download): pull a remote file to the local sink (SD) ---
    void ftcDlSendOpen();  // FileDownload open: [00][00][pkg][path]
    void ftcDlSendChunk(); // FileDownload chunk request for _dlSeq
    void ftcCloseSink();
    void ftcDownloadPanel(bool ok, bool statOk, uint32_t tcrc); // framed DOWNLOAD result box + Verify line (FtcDownloadVerify)
    uint32_t _dlSize = 0;    // remote file size (from the open answer)
    uint16_t _dlChunks = 0;  // total chunks
    uint16_t _dlSeq = 0;     // current chunk (1-based)
    uint32_t _dlWritten = 0; // bytes written to the sink so far
    uint32_t _dlStartMs = 0; // for throughput
    uint32_t _dlEndMs = 0;   // transfer end (last chunk) -- frozen before the post-download verify round-trip
    uint32_t _dlCrc = 0;     // running CRC32/POSIX folded over the received stream -> compared to the target FileInfo CRC32
    bool _dlSinkOpen = false;
    char _dlLocal[FTC_PATH_MAX] = {0}; // local (resolved) destination path, prefix already stripped

    // Local-storage backends self-registered in setup() (LittleFS default + optional sd/efc). One transfer
    // at a time -> a single active source/sink pair remembered for this transfer's read/write/close.
    FtcBackend _backends[4]{};
    uint8_t _backendN = 0;
    const FtcFileSource *_activeSrc = nullptr; // resolved source for the current upload
    const FtcFileSink *_activeSink = nullptr;  // resolved sink for the current download
    const FtcBackend *_dlBackend = nullptr;    // resolved backend for the active download (space gate + messages)
    // Resolve a local path's prefix to a backend; *stripped points past a named prefix (at the '/'), or
    // at the whole path for the default. Returns nullptr only if no backend (not even a default) matches.
    const FtcBackend *ftcResolveBackend(const char *path, const char **stripped);

    FileTransferClientConsole _console; // parsing + colored help; constructed with *this

    static FileTransferClient *_instance;
};

extern FileTransferClient openknxFileTransferClient;
#endif

#pragma once
/**
 * @file        FileTransferClient.h
 * @brief       KNX file transfer client -- transfers files (push/pull) and manages/queries another device's FTC server, PA to PA
 * @date        2026-07-16
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 */
#include "OpenKNX.h"
#include "FileTransferConfig.h"

#ifdef OPENKNX_FTC
    #include "FileTransferClientConsole.h"

    // KNX file transfer CLIENT: push a file to another OpenKNX device's KnxFileTransfer server, PA -> PA,
    // no PC. Mirror of FileTransferModule (the server), same OFM. The source is read locally (a registered
    // backend -- LittleFS/SD/efc -- or a built-in test pattern); the target writes its own LittleFS. `ftc ?`.

    // Shared max length for every local/remote PATH buffer (client-side convention; the frame itself carries up
    // to FTC_PKG_MAX). 96 = headroom for nested sd//efc/ paths over the ~22-char flat firmware names, without the
    // vector-multiplied cost of FtcEntry::name (a single filename, kept at 64). Every path buffer sizes off this;
    // all writes stay snprintf/strncpy-bounded, so a longer input truncates safely (never overflows).
    #if defined(ARDUINO_ARCH_RP2040) || defined(ARDUINO_ARCH_ESP32) || defined(ARDUINO_ARCH_ESP8266)
static constexpr size_t FTC_PATH_MAX = 96; // embedded target: RAM-bounded path buffers
        #define FTC_PATH_SCAN "95"         // sscanf width (FTC_PATH_MAX-1), literal for the format string
    #else
static constexpr size_t FTC_PATH_MAX = 512; // native ftc-cli host: long absolute host paths (no RAM constraint)
        #define FTC_PATH_SCAN "511"
    #endif

// Longest REMOTE (on-the-wire) path that may enter _ftcPath/_ftcListDir. Those buffers are memcpy'd whole
// into the 256-B _ftcTx frame -- the host's 512-B FTC_PATH_MAX buffer (needed only for the LOCAL source path)
// would overrun _ftcTx and wrap the uint8_t _ftcTxLen. The worst frame (fast-open: 6-B header + path + NUL +
// 4-B pre-alloc size) must fit BOTH the 247-B KNX APDU payload and _ftcTx, so a remote path caps at 235.
static constexpr size_t FTC_REMOTE_PATH_MAX = 235;
// Effective remote-path cap = the smaller of the path buffer and the wire limit. On embedded this is
// FTC_PATH_MAX-1 (95) -> identical to the previous `>= sizeof(_ftcPath)` guards (no firmware behaviour change);
// on the host it clamps the wire path to 235 instead of the 511-B local-path buffer.
static constexpr size_t FTC_REMOTE_PATH_LIMIT =
    (FTC_PATH_MAX - 1 < FTC_REMOTE_PATH_MAX) ? (FTC_PATH_MAX - 1) : FTC_REMOTE_PATH_MAX;

// Expand a 16-bit KNX individual address to the three "%u.%u.%u" printf args (area.line.device). One
// definition for the ~30 log/format sites; identical expansion to the previous inline shift/mask triplets.
#define FTC_PA_ARGS(pa) (unsigned)(((pa) >> 12) & 0x0F), (unsigned)(((pa) >> 8) & 0x0F), (unsigned)((pa) & 0xFF)

// The source is reached through this callback triple so the client needs no storage header: ftcResolveBackend()
// maps a path prefix ("" LittleFS · "sd/" · "efc/") to an implementation; storage-specifics live in the
// each provider's own store class (sd::IFileStore / efc::IFileStore) -- self-contained, no bridge.
struct FtcFileSource
{
    int32_t (*open)(const char *path);
    uint8_t (*read)(uint32_t offset, uint8_t *buf, uint8_t len);
    void (*close)();
};

// The mirror of FtcFileSource for a download: where the pulled bytes are written (a local backend).
// Resolved by ftcResolveBackend() (see above); download aborts if the prefix has no provider.
struct FtcFileSink
{
    bool (*open)(const char *path);                 // create/truncate; false on failure
    int (*write)(const uint8_t *buf, uint16_t len); // append; bytes written, or -1 on error
    void (*close)();
    // Resume-open (download resume): truncate `path` to exactly keepBytes (a whole-chunk boundary) and
    // position for append; returns keepBytes on success, <0 on failure. Subsequent writes use `write`
    // (append). A provider that cannot resume leaves this nullptr -> resume is not offered (fresh fallback).
    int32_t (*resumeOpen)(const char *path, uint32_t keepBytes) = nullptr;
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
    Failed,
};

struct FtcStatus
{
    FtcPhase phase = FtcPhase::Idle;
    uint16_t target = 0;
    uint32_t done = 0;             // bytes transferred so far (0..total) -> progress bar
    uint32_t total = 0;            // bytes to transfer (0 if not applicable)
    uint16_t bps = 0;
    uint16_t chunk = 0;            // current chunk (1-based)
    uint16_t chunks = 0;
    uint16_t window = 0;           // fast: current AIMD window size (grows on clean, halves on loss); 0 = safe/n.a.
    // Live event counters (monotonic within an op) — a frontend samples the deltas to mark the throughput curve.
    uint16_t verifies = 0;         // delivery confirmations: safe = per chunk (CRC) · fast = per window (bitmap report)
    uint16_t crcErrors = 0;        // CRC mismatches encountered (a retry is made for each)
    uint16_t resends = 0;          // chunk/window retransmits fired
    bool ok = false;
    uint32_t crc = 0;              // last verify / info CRC-32
    char path[FTC_PATH_MAX] = {0};
    char message[40] = {0};        // short human status ("resuming", "up to date", "fs full?" ...)

    // Progress as hundredths of a percent, e.g. 4567 = 45.67 %  -> printf("%u.%02u %%", p/100, p%100).
    uint16_t percentX100() const { return total ? (uint16_t)((uint64_t)done * 10000ULL / total) : 0; }
};

struct FtcEntry
{
    char name[64] = {0};
    uint32_t size = 0;
    uint32_t crc = 0;
    bool isDir = false;
    bool hasInfo = false;   // ll fills size (+crc); ls leaves them 0
    bool hasCrc = false;    // 0x00 answer had a CRC; 0x01 (SD/EFC default) is size-only -> CRC shown as n/a
    bool isOpenKnx = false; // scan `openknx`/`info` probe: this device's manufacturer id == 0x00FA
};

// One queued console-output line for the cooperative drain (ftcOut / ftcDrainOut), so no single loop()
// pass blocks on USB-CDC long enough to trip the loop-time warning.
struct FtcOutLine
{
    uint8_t color = 0;
    char text[128] = {0};
};

// --- Structured result surface (the "info API"): every frontend renders from these, no text parsing.
// Fixed-size, filled cooperatively by the request* handlers; `valid` set false at op start, left valid at
// finish for a poll (same lifetime as status()/listing()).
struct FtcFsInfo
{
    uint32_t total = 0, used = 0, free = 0;
    bool kb = false; // values are KB (sd/efc, >4 GB) vs bytes (LittleFS)
    bool valid = false;
};
struct FtcDeviceInfo
{
    uint16_t mask = 0;
    char cls[24] = {0};        // friendly class from ftcMaskName()
    uint16_t manufacturer = 0;
    uint8_t serial[6] = {0};   bool haveSerial = false;
    char order[12] = {0};      bool haveOrder = false;
    uint8_t hardware[6] = {0}; bool haveHw = false;
    uint16_t version = 0;      bool haveVersion = false; // PDT_VERSION [magic:5][ver:5][rev:6]
    uint16_t ftmVersion = 0;
    uint32_t features = 0;     // feature bits: 1 Resume, 2 Update, 4 Fast, 8 Console
    bool progMode = false;
    uint8_t appState = 0, addrTableState = 0, assocTableState = 0, goTableState = 0;
    // BCU1 (mask 0012) memory-map extras (ETS: Ausführungszustand / PEI-Typ / Ausführungsfehler); 0xFF = not read
    uint8_t bcuRunState = 0xFF, bcuPeiType = 0xFF, bcuRunError = 0xFF;
    uint16_t busVoltmV = 0;    // bus voltage in mV (A_ADC_Read), 0 = not read
    bool haveBusVolt = false;
    uint8_t bcuMfr = 0;        // BCU manufacturer code (0x01 = Siemens)
    uint8_t bcuApp[3] = {0};   // BCU application id, packed BCD -> the 6-digit ETS number (last pair = version)
    bool haveBcuApp = false;
    bool haveBcu1 = false;
    bool valid = false;
};
struct FtcGaEntry // `info ga` row; union-overlaid, so ftcExtractGaTable sets EVERY field at creation -> no default member initialisers (they would make the union non-trivial)
{
    uint16_t ga;
    uint16_t co;
    uint8_t flags;    // bit0 C(K) bit1 R(L) bit2 W(S) bit3 T(Ü) bit4 U(A); valid only if cfgValid
    uint8_t prio;     // KNX config-octet coding: 0 system, 1 normal, 2 urgent, 3 low; 0xFF = unread
    uint8_t sizeCode; // group-object value-type 0..12 (03_05_01 §4.18.3) -> object SIZE (not semantic DPT); 0xFF = unread
    bool cfgValid;    // the group-object descriptor (flags/prio/size) was read for this KO
};

// --- Transfer "info API": upload / perf / download setup + result. Same lifetime + pattern as FtcFsInfo /
// FtcDeviceInfo (valid=false at op start, filled cooperatively, left valid at finish for a poll). This is the
// canonical, render-agnostic surface every frontend draws from: the ftc-cli Panel today, the embedded WebConfig
// tomorrow, the SM console box on-device -- all from these structs, no text parsing. Keep it structured (codes,
// not pre-formatted strings) so a UI can label/format as it likes.
enum class FtcXferKind : uint8_t { Upload = 0, Perf = 1, Download = 2 };

struct FtcTransferSetup
{
    bool valid = false;
    FtcXferKind kind = FtcXferKind::Upload;
    uint16_t target = 0;
    char local[FTC_PATH_MAX] = {0};
    char remote[FTC_PATH_MAX] = {0};
    uint32_t size = 0;                   // total bytes to transfer (0 = unknown yet, e.g. download pre-open)
    bool hasCrc = false; uint32_t crc = 0; // source CRC32 known up-front (perf deterministic pattern)
    uint8_t mode = 0;                    // FtcMode: 0 safe · 1 fast
    uint16_t chunkSize = 0;              // payload bytes per chunk
    uint16_t chunks = 0;
    bool noResume = false;
    bool willApply = false;              // upload: flash + reboot after the transfer
    bool keep = false;
    uint32_t resumedBytes = 0;           // upload: bytes already on the target that a resume skips (0 = fresh); set once FtcResumeInfo decided
};

struct FtcTransferResult
{
    bool valid = false;
    FtcXferKind kind = FtcXferKind::Upload;
    bool ok = false;
    uint16_t target = 0;
    uint32_t bytes = 0;                  // net bytes delivered this run (resume-deduped)
    uint16_t chunks = 0; uint16_t chunkSize = 0;
    uint32_t elapsedMs = 0;
    uint32_t avgBps = 0; uint32_t peakBps = 0;
    uint32_t crc = 0;                    // CRC32 of the transferred data (miniz macro-safe name)
    uint8_t verify = 0;                  // 0 = n/a / no answer · 1 = verified OK · 2 = mismatch
    uint16_t retries = 0; uint32_t retryLostMs = 0; // upload auto-retry recovery
    uint32_t resumedBytes = 0;           // bytes already on the target (skipped this run)
    uint16_t dupes = 0;                  // IP-mirror duplicate answers discarded
    uint8_t mode = 0;                    // FtcMode actually used (a fast-probe miss downgrades to safe)
    uint8_t cleanup = 0;                 // perf only: 0 n/a · 1 removed · 2 kept · 3 left behind
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
    void loopDownload();   // download state group (extracted from loop())
    void loopFast();       // fast-upload state group
    void loopDirOps();     // dir list/info state group
    void loopScan();       // scan state group
    void loopDeviceInfo(); // device-info + GA state group
#ifdef OPENKNX_FTC_SECURITY
    void loopSecurity(); // auth state group
#endif
#ifdef OPENKNX_FTC_CONSOLE
    void loopConsole(); // console-client state group
#endif


    // --- Action API: any front-end (console, WebConfig, DDC) calls these -----------------------
    // Callers must ensure !isBusy() first (cancel excepted); a rejected request logs the reason.
    bool isBusy() const { return _ftcState != FtcIdle; }
    // Console lockstep state: true when no console command/drain/OPEN is in flight (_conSub == 0). The host
    // auto-command scheduler gates on this so a scheduled line is sent ONLY when the previous round-trip is done
    // (never overlaps -> 1-outstanding, bus-friendly). Read-only; no behaviour change.
#ifdef OPENKNX_FTC_CONSOLE
    bool consoleIdle() const { return _conSub == 0; }
#endif
    void requestCancel();
    void requestPing(uint16_t pa);
    void requestList(uint16_t pa, const char *dir, bool detailed);
    void requestInfo(uint16_t pa, const char *remotePath);
    void requestFsInfo(uint16_t pa, const char *path = "");                    // `ftc df` -- target LittleFS total/used/free with a usage bar
    void ftcRetryCmd(const char *sub, const char *val); // `ftc retry ...` -- show/set retry tuning (help/get/set)
    void requestDeviceInfo(uint16_t pa);                // `ftc <pa> info` with no file: mask/class + FTM version + features
    void requestGroupComm(uint16_t pa);                 // `ftc <pa> info ga`: GA table + com-object links (ETS-style), via A_Memory_Read
    void requestLed(uint16_t pa, uint8_t mode);         // `ftc <pa> led on|off|blink`: locate via the prog-mode LED (0=off 1=on 2=blink)
    #ifdef OPENKNX_FTC_CONSOLE
    void requestConsole(uint16_t pa, uint8_t maxDrain = 0); // `ftc <pa> console [maxbytes]`: open the console tunnel (obj 160); maxDrain 0 = default 247
    #endif
    void setApduHint(uint16_t apdu) { _apduHint = apdu; } // detected interface max APDU -> seeds auto-framing so a constrained link starts at the right frame instead of degrading from the max (used by requestUpload/Perf/Download, so NOT console-gated)
    void requestDelete(uint16_t pa, const char *remotePath);
    void requestFormat(uint16_t pa);
    void requestMkdir(uint16_t pa, const char *dir);
    void requestRmdir(uint16_t pa, const char *dir);
    void requestRename(uint16_t pa, const char *oldPath, const char *newPath);
    #ifdef OPENKNX_FTC_SECURITY
    // `ftc <pa> login <pw>`: open the target's write window via a challenge-response. The password is turned
    // into the AES key HERE and never leaves this process on the wire (only nonce + 4-byte MAC travel).
    void requestLogin(uint16_t pa, const char *pw);
    void requestLogout(uint16_t pa); // `ftc <pa> logout`: close the target's write window immediately
    #endif
    // mode: 0 = safe/classic (default), 1 = fast/windowed. A non-zero mode negotiates the
    // server's FAST capability first and silently falls back to classic if missing (ftcBeginFeatureProbe).
    // apply (opt-in, default false): after a verified upload, trigger the target to self-apply the image
    // (fwupdate -> reboot). Forced false for the perf/test sources so /ftctest.bin is never flashed.
    void requestUpload(uint16_t pa, const char *src, unsigned pkg, bool noResume, uint8_t mode = 0, bool apply = false, const char *target = "", uint16_t fixedWnd = 0);
    void requestPerf(uint16_t pa, uint32_t sizeBytes, unsigned pkg, uint8_t mode = 0, bool keep = false, uint16_t fixedWnd = 0, const char *drive = ""); // upload a test pattern, report B/s (keep = leave it as /ftcperf_<crc>.bin; fixedWnd>0 pins the fast window)
    void requestDownload(uint16_t pa, const char *remotePath, const char *localPath, uint8_t pkg = 0, bool noResume = false); // pull to a local backend; pkg 0 = default chunk size; noResume forces a fresh (truncating) download
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

    const FtcStatus &status() const { return _status; }
    uint16_t scanCurrentPa() const { return (uint16_t)_scanNext; } // live: the PA currently being probed (for a progress line)
    uint16_t scanFound() const { return _scanFound; }
    const std::vector<FtcEntry> &listing() const { return _ftcListing; }
    const FtcFsInfo &fsInfo() const { return _fsInfo; }
    const FtcDeviceInfo &deviceInfo() const { return _deviceInfo; }
    const FtcTransferSetup &transferSetup() const { return _xferSetup; }
    const FtcTransferResult &transferResult() const { return _xferResult; }
    uint8_t transferRetries() const { return _ftcTransferRetries; }        // live transfer-level auto-retry count (survives a retry-restart)
    uint8_t transferRetryMax() const { return _cfgTransferRetries; }       // configured auto-retry budget
    const FtcGaEntry *groupObjects(uint16_t &count) const { count = _gaObjectsN; return _gaObjects; }

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
    const char *_ftcVerb = "rm";
    void ftcSendDirList();
    void ftcListAdvance();
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
        FtcSent,
        FtcInfo,
        FtcVerify,           // FileInfo(43) sent right after an upload -- compare against _ftcSrcCrc
        FtcInfoSend,         // verify FileInfo(43) could not be queued (TX congested) -- retry the send until the queue drains
        FtcFeatureProbe,     // fast only: CheckFeatures(102) sent, short 800ms gate before start
        FtcFastOpen,         // fast: cmd44 open sent, waiting for the open ack (0x00 / 0x42 / 0x4A)
        FtcFastStream,       // fast: the pump -- burst DATA frames (silent), gated by the TP FIFO + window
        FtcFastReport,       // fast: cmd45 report sent, waiting for the received-bitmap
        FtcFastResend,       // fast: re-send only the unset seqs of the current window, then re-report
        FtcFastClose,        // fast: cmd44 close sent, waiting for the 1-byte ack -> then verify
        FtcResumeInfo,
        FtcCrcPrefix,        // cooperative prefix CRC-32 (resume seed / perf precompute) -- a chunk per loop() pass
        FtcDirList,
        FtcDirInfo,          // FileInfo(43) per collected file -- fill in its size + CRC32 (ll only)
        FtcFsInfoReq,        // FilesystemInfo(46) sent -- LittleFS total/used for `df`, the ll footer, or the pre-upload space check
        FtcDelete,           // FileDelete(42) sent -- waiting for the result byte
        FtcUploadOpen,
        FtcUploadChunk,      // FileUpload sequence n sent, waiting for the echoed sequence + CRC
        FtcUploadClose,      // FileUpload 0xFFFF sent, waiting for the (empty) final answer
        FtcCancel,           // Cancel sent, letting the frame drain before disconnecting
        FtcUploadChunkRetry, // the chunk in _ftcTx failed (timeout/seq/CRC) -- send it again
        FtcScan,             // sweeping a PA range: pace probes, collect answers by their PA
        FtcScanCo,           // ETS-parity scan (`ets`): T_Connect per address, CO read -- finds BCU1/BCU2
        FtcDevDescr,         // device info: DeviceDescriptor_Read sent, waiting for the mask
        FtcDevCoConn,        // device info: connectionless DD got no answer -> T_Connect opening for a CO retry (reaches BCU1/old masks)
        FtcDevCoDescr,       // device info: CO DeviceDescriptor_Read sent over the T_Connect, waiting for the mask
        FtcBcuConn,          // device info (BCU2 memory-mapped): T_Connect opening before the fixed identity block read
        FtcBcu1Mem,          // device info (BCU1/BCU2): A_Memory_Read of the fixed identity block (0x0100..) -> Bcu*::decode
        FtcBcuAdc,           // device info (BCU1/BCU2): A_ADC_Read of the bus-voltage channel, waiting for the response
        FtcDevVer,
        FtcDevFeat,
        FtcDevProp,
        FtcDevEnum,          // device info: probing object indices for their type (find app/tables)
        FtcDevLoad,
        FtcGaConnect,        // group comm: T_Connect opening -- wait for the CO link before the memory walk (ETS reads memory connection-oriented)
        FtcGaRef,            // group comm: PropertyValue_Read PID_TABLE_REFERENCE of the current table
        FtcGaPtr,            // group comm (BCU1): A_Memory_Read of the 1-byte table pointer (0x0111/0x0112) -> table at 0x100 + pointer
        FtcGaMem,            // group comm: A_Memory_Read walking the current table's blob
        FtcDownloadOpen,
        FtcDownloadCrcPrefix, // download resume: cooperatively CRC the on-disk prefix [0,keepBytes) before appending
        FtcDownloadChunk,    // download: FileDownload chunk sent, waiting for the data + CRC16
        FtcDownloadVerify,   // download: FileInfo(43) sent after the last chunk -- compare target CRC32 vs the received-stream CRC32
        FtcPerfCleanup,
        FtcApplyCheck,
        FtcApplyProbe,       // apply: CheckFeatures(102) sent, short gate to learn if the target can self-apply
        FtcScanPost,         // post-sweep OpenKNX/info probe + cooperative CSV write (Feature B/C); NOT the FtcScanCo SM
    #ifdef OPENKNX_FTC_SECURITY
        FtcAuthProbe,     // login: CheckFeatures(102) sent -> is the target password-protected at all?
        FtcAuthChallenge, // login: AuthChallenge(103) sent -> waiting for the 16-byte nonce
        FtcAuthResponse,  // login/logout: AuthResponse(104)/AuthLogout(105) sent -> waiting for the status byte
    #endif
    #ifdef OPENKNX_FTC_CONSOLE
        FtcConsoleProbe, // console: CheckFeatures(102) sent, short gate -- does the target even have the console feature?
        FtcConsole       // console tunnel: finished lines go to the target (obj 160), its log ring streams back
    #endif
    };

    // Fires in the KNX stack's dispatch context, so it only parks the bytes and lets loop() act -- a
    // send or file I/O from here would re-enter the application layer from inside its own dispatch.
    static void ftcOnResponse(uint16_t pa, uint8_t objectIndex, uint8_t propertyId, uint8_t *data, uint8_t length);
    // Same rule for a DeviceDescriptor_Response (scan): park the responder's PA + mask, act in loop().
    static void ftcOnDeviceDescriptor(uint16_t pa, uint8_t descriptorType, const uint8_t *data);
    // Same rule for a PropertyValue_Response (device info): park it, act in loop().
    static void ftcOnPropertyValue(uint16_t pa, uint8_t objectIndex, uint8_t propertyId, const uint8_t *data, uint8_t length);
    // Same rule for a MemoryResponse (group comm): place the table chunk at its true offset, act in loop().
    static void ftcOnMemory(uint16_t pa, uint16_t addr, const uint8_t *data, uint8_t len);
    static void ftcOnAdc(uint16_t pa, uint8_t channel, uint8_t count, int16_t value); // A_ADC_Response (bus voltage)
    void ftcDevSendProp();
    void scanRecord(uint16_t pa, uint16_t mask);   // one responder -> log line + bounded listing
    void scanReport();
    static const char *ftcMaskName(uint16_t mask); // 0x07B0 -> "System B", 0x091A -> "TP/IP coupler" ...
    // --- post-sweep OpenKNX/info probe + CSV save (Feature B/C): all cooperative, run from FtcScanPost ---
    void ftcScanPostBegin();                              // enter FtcScanPost: count candidates, arm callbacks, open the CSV sink
    bool ftcScanProbeStart(const FtcEntry &e);            // launch one device probe (LIGHT PID-11 read or FULL info chain); false = unparseable PA
    void ftcScanPostConsume();                            // an in-flight probe finished: mark OpenKNX + log (green when 0x00FA) + stream its CSV row
    void ftcScanInfoBail();                               // FULL probe: an early device-info exit -> hand back to FtcScanPost (never aborts the scan)
    void ftcScanSaveOpen();
    void ftcScanWriteRow(const FtcEntry &e, bool probed); // append one bounded CSV row via the sink (probed = use the _dev* identity)
    void ftcScanSaveFail();                               // a sink write failed: log once, close, stop saving (never wedges the scan)
    void ftcDevSendObjType();                             // probe the current object index's type (FtcDevEnum)
    void ftcDevBuildLoadQueue();
    void ftcDevSendLoad();
    static const char *ftcLoadName(uint8_t state);

    void ftcStart(uint16_t pa, bool upload, uint8_t mode = 0);
    bool ftcSend(uint8_t propertyId, uint8_t length);

    void ftcSetFixedWindow(uint16_t fixedWnd); // perf/upload `w<N>`: pin the fast window, clamped to [MIN,MAX] (0 = adaptive AIMD)
    bool ftcResolvePkg(unsigned &pkg);         // resolve pkg auto/apduHint + range-check; false (logged) if out of range
    void ftcResetTransferJob(unsigned pkg, uint8_t mode); // clear the per-request transfer state + set payload size/base
    void ftcArmHardDeadline(uint32_t size);              // arm the size-scaled whole-transfer wedge backstop at up/download start

    // --- fast-transfer negotiation (phase 1): capability probe + classic fallback ---------------
    // Read the TP transmit FIFO depth via the bau -- the fast-transfer pump's flow control (send while
    // below the high-water mark, drain below the low-water mark before a report/close).
    uint16_t ftcTxQueueSize();
    // Start (or resolve from cache) the CheckFeatures(102) probe for a fast upload. Returns true
    // if a probe is in flight (-> FtcFeatureProbe), false if resolved synchronously -> caller runs classic.
    bool ftcBeginFeatureProbe();
    // Decide fast vs classic from the probed feature byte and gate on the chunk cap. Downgrades
    // _ftcMode to 0 (and logs once) on any miss; otherwise keeps the mode and logs the negotiation.
    void ftcGateFast(uint8_t features, bool answered);
    static const char *ftcModeName(uint8_t mode);
    void ftcSendUploadOpen();
    // After the resume decision: open the transfer via the fast path (mode 1) or the classic path
    // (mode 0). A single dispatcher so the classic call sites in FtcResumeInfo stay byte-for-byte.
    void ftcProceedToUpload();
    void ftcAutoDegradePayload();                      // auto-pkg: drop the payload toward FTC_PKG_MIN on a link retry (fresh OR resume)
    void ftcSendFastOpen();                            // cmd44 open (flags: resume?, keepBitmap for recovery; expectedChunks) -> FtcFastOpen
    void ftcFastOpenWindow();                          // freeze the current window's high edge _ftcWndEnd = min(base+wnd, chunks+1)
    void ftcSendFastData(uint16_t seq);                // SILENT cmd44 DATA frame (does NOT arm _ftcRespPending/_ftcSince)
    void ftcSendReport(uint16_t base, uint16_t count); // cmd45 report query (fresh nonce) -> FtcFastReport
    void ftcSendFastClose();
    void ftcSendNextChunk();
    void ftcSendClose();
    void ftcAbort(const char *reason);
    void ftcFinish();
    void ftcEnterApplyGate();  // use the cached feature byte now, else probe (CheckFeatures) -> FtcApplyProbe
    void ftcApplyDecide();     // feature byte known -> trigger self-apply (Update bit set) or log a skip; finishes
    void ftcTriggerFwUpdate(); // send FwUpdate(101) with the remote path (fire-and-forget) + loud log; finishes
    void ftcCloseSource();
    // Reads up to maxLength source bytes at offset. Returns: >0 = bytes read, 0 = clean EOF (offset at/after
    // the source size), <0 = a real backend READ error (distinct from EOF -> the caller retries transiently
    // instead of closing the transfer as if it were complete).
    int ftcReadSource(uint32_t offset, uint8_t *buffer, uint8_t maxLength);

    FtcState _ftcState = FtcIdle;
    uint16_t _ftcTarget = 0;
    uint32_t _ftcSince = 0;

    #ifdef OPENKNX_FTC_SECURITY
    // login: pad16(password) == the AES key, held only between requestLogin() and the AuthResponse send.
    // The password itself is never stored beyond this and never leaves the process on the wire.
    uint8_t _ftcAuthKey[16] = {};
    bool _ftcLogout = false; // distinguishes the logout flow (cmd 105) from login (103/104) in FtcAuthResponse
    // CheckFeatures result for a login: only send the challenge if the target is password-protected (0x10);
    // otherwise report clearly instead of timing out against an old / non-auth device.
    void authAfterProbe(uint8_t features, bool answered);
    #endif

    uint8_t _ftcMode = 0; // 0 = safe/classic, 1 = fast/windowed (set by ftcStart)
    // Per-PA feature cache: skip the re-probe for a known target (single slot -- one PA at a time). Only
    // a real answer is cached, never a timeout (transient, must not pin a device to classic forever).
    bool _ftcFeatValid = false;
    uint16_t _ftcFeatPa = 0;    // PA the cached feature byte belongs to
    uint8_t _ftcFeatBits = 0;   // CheckFeatures(102) result byte for _ftcFeatPa

    bool _ftcUpload = false;           // false = ping (ModuleVersion round trip only)
    bool _ftcTestSource = false;
    bool _ftcResume = false;           // FtcResumeInfo decided to pick up where a previous run left off
    bool _ftcNoResume = false;         // send no-resume: discard any target partial, upload fresh (truncate)
    bool _ftcNoTargetCrc = false;      // SD/EFC target answered size-only (0x01) -> summary shows "not verified" not "verified OK"
    bool _ftcPkgAuto = false;          // pkg auto: start at FTC_PKG_MAX (degrade one notch per fresh link-retry)
    bool _ftcIsPerf = false;
    bool _ftcPerfRemoved = false;
    bool _ftcPerfKeep = false;         // perf `keep`: skip the cleanup delete; file stays as /ftcperf_<crc>.bin
    bool _ftcApply = false;            // opt-in: after a verified upload, trigger the target to self-apply (fwupdate)
    uint16_t _ftcStartSeq = 1;         // first chunk to send (>1 only when resuming)
    uint32_t _ftcResumeBase = 0;       // bytes the target already had; not sent by this run
    uint32_t _ftcDupes = 0;            // late second copies of answers the target sends for every request
    char _ftcPath[FTC_PATH_MAX] = {0};
    uint8_t _ftcPayloadSize = 0;       // = pkg - 6, mirrored to the target as its _size
    uint16_t _ftcPayloadBase = 0;      // original payload for this request -> pkg-auto degrade recomputes from this (survives retries)
    uint16_t _ftcSequence = 0;         // 1-based; 0 and 0xFFFF are the open/close markers
    uint16_t _ftcChunks = 0;
    uint32_t _ftcSize = 0;
    uint32_t _ftcDone = 0;
    uint32_t _ftcSrcCrc = 0; // CRC-32/POSIX of what we sent -- compared against the target's

    // --- fast transfer (windowed AIMD) -- STATIC, ~1 KB -----------------------------------------
    // Block-synchronous windows: stream [base,wndEnd), report, resend gaps, advance. _ftcWnd (AIMD) sizes
    // the NEXT window; _ftcWndEnd freezes the CURRENT high edge so halving on loss can't orphan tail seqs.
    static constexpr uint16_t FTC_BMP_BYTES = 1024; // = FTM_FAST_MAX_CHUNKS/8; mirror of the server bitmap
    uint16_t _ftcWnd = 16;                          // AUTO window: +8 climb from 16 until the first loss LOCKS it at the last clean size (Discover&Lock); bounded [MIN,MAX]
    bool _ftcWndLocked = false;                     // set on the first lossy window -> _ftcWnd never probes up again (stable, no edge oscillation)
    uint16_t _ftcWndClean = 16;                     // last window proven clean (0 loss) -> the lock target when the next x2 probe overruns
    uint16_t _ftcWndFix = 0;                        // fast: fixed window (0 = adaptive AIMD). >0 pins the ceiling: NO grow (no
                                                    // "feel-its-way-up" that floods at the end); loss still ratchets it down one
                                                    // step and it STAYS there. Set per-request (perf `w<N>`); 0 for uploads.
    uint16_t _ftcWndEnd = 0;                        // frozen high edge (exclusive) of the current window: [base, wndEnd)
    uint16_t _ftcNextSeq = 0;
    uint16_t _ftcReportBase = 0;                    // low edge (1-based, inclusive) of the current window
    uint16_t _ftcResendCur = 0;                     // cursor while re-sending the unset seqs of a window
    // _ftcRecvBmp[FTC_BMP_BYTES] is overlaid with the info-ga buffers -> declared in the union near _gaObjects
    uint32_t _ftcFoldWatermark = 0;                 // highest seq already folded into _ftcSrcCrc (fold-once, immune to resends)
    uint16_t _ftcPrevMissing = 0;
    uint16_t _ftcNoProgress = 0;                    // consecutive reports without the missing count shrinking
    uint32_t _ftcLastReportMs = 0;                  // millis() of the previous report -> delivery-rate (BBR) pacing interval
    uint16_t _ftcReportRetries = 0;                 // report-query timeouts spent (abort past FTC_REPORT_RETRIES)
    uint32_t _ftcRepWaitStart = 0;                  // [dbg] millis() of the first query of the current report round (retry re-sends keep it) -> per-window answer latency
    uint8_t _ftcReportNonce = 0;                    // bumped on every report send; the answer must echo it (staleness)
    uint32_t _ftcDeadline = 0;
    uint32_t _ftcHardDeadline = 0;                  // absolute wall-clock wedge backstop for the whole transfer (0 = unarmed). Armed per attempt at up/download start, cleared at ftcFinish + retry-arm -> only ever fires inside a transfer
    uint32_t _ftcInfoDeadline = 0;                  // patience window for the pre-transfer FileInfo on a busy target
    uint8_t _ftcInfoLen = 0;                         // FileInfo(43) payload length -- kept so a congested send can be re-tried
    uint8_t _ftcTransferRetries = 0;                // whole-transfer auto-retries spent (bounded by _cfgTransferRetries); reset only on a fresh request
    bool _ftcRetryPending = false;                  // a transient upload abort armed a resume-retry -> FtcCancel re-runs the transfer after a backoff
    // Retry tuning -- runtime-settable via `ftc retry <max|transfer|backoff> [value]` (defaults below).
    static constexpr uint8_t FTC_MAX_RETRIES_DEF = 3;          // per-chunk retries (CRC/timeout) before that chunk fails
    static constexpr uint8_t FTC_TRANSFER_RETRIES_DEF = 3;     // auto-resume attempts (transient abort -> re-trigger the action + resume); runtime-settable via `ftc retry transfer <n>`
    static constexpr uint32_t FTC_RETRY_BACKOFF_MS_DEF = 3000; // ms between transfer retries (let a busy/rebooting target settle)
    uint8_t _cfgMaxRetries = FTC_MAX_RETRIES_DEF;
    uint8_t _cfgTransferRetries = FTC_TRANSFER_RETRIES_DEF;
    uint32_t _cfgBackoffMs = FTC_RETRY_BACKOFF_MS_DEF;
    bool _cfgAutoResume = true;                     // master on/off for the mid-action auto-resume (re-trigger the aborted up/download); off = a transient abort fails immediately

    // --- throughput measurement (measured from the wire, never estimated) ---
    uint32_t _ftcStartMs = 0;         // set when the open frame goes out, i.e. when bytes start flowing (RE-armed each retry attempt)
    uint32_t _ftcElapsedMs = 0;
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
    uint32_t _ftcLastProgMs = 0;     // millis() of the last SHOWN progress line -> interval rate + 1 Hz verbose gate
    uint32_t _ftcLastProgDone = 0;   // bytes done at the last shown line (pairs with _ftcLastProgMs)
    uint32_t _ftcPeakBps = 0;        // highest interval rate seen this transfer -> the result panel
    bool _ftcVerbose = false;        // `verbose`/`v`: emit progress every second (default off = per decile)
    uint32_t _ftcLastProgressMs = 0; // millis() of the last forward byte-progress -> the dead window (stall+recovery) charged to a retry
    void ftcLogRate(const char *what, uint32_t elapsedMs);
    void ftcPrintSummary(); // one clean, prefix-less framed result block (upload/perf)
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
    uint8_t _ftcRetries = 0;         // attempts spent on the CURRENT chunk (see FTC_MAX_RETRIES)
    bool _ftcRereadChunk = false;    // classic: the last attempt failed to READ the source -> FtcUploadChunkRetry re-reads (not re-sends stale _ftcTx)
    bool _ftcChunkFolded = false;    // classic: this chunk's payload is already folded into _ftcSrcCrc (fold-once; survives a read-error re-read / resend)
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

    // Detected interface max APDU (0 = none); see setApduHint(). Read by requestUpload/requestPerf/
    // requestDownload to seed auto-framing, so it must stay OUTSIDE the console #ifdef (else FTC-without-CONSOLE
    // fails to compile).
    uint16_t _apduHint = 0;

    #ifdef OPENKNX_FTC_CONSOLE
    // --- console tunnel (ftc <pa> console): logical session over obj 160, no knx-lib change ---
    static constexpr uint8_t CON_OBJECT_INDEX = 160;
    static constexpr uint8_t CON_PID_IN = 1;                        // input / control (OPEN/CLOSE/line)
    static constexpr uint8_t CON_PID_OUT = 2;                       // output drain (poll the target's log ring)
    static constexpr uint8_t CON_DRAIN_MIN = 4;                     // smallest drain cap to request (keep in sync with the server; 4 keeps margin under a 15-octet-APDU interface)
    static constexpr uint8_t CON_DRAIN_MAX = 247;                   // max console text per PID_OUT answer (one APDU minus the 7 B header)
    static constexpr uint32_t CON_KEEP_MS = 3000;                   // idle: poll OUT to fetch async logs + keep the session fresh
    uint8_t _conSub = 0;                                            // 0 = idle in-session, 1 = await command ack, 2 = draining OUT, 3 = await OPEN ack
    uint8_t _conProbeTries = 0;                                     // console pre-flight: CheckFeatures resends used (congested-bus tolerance)
    uint8_t _conMaxDrain = CON_DRAIN_MAX;                           // cap on PID_OUT drain bytes/answer (small = fits constrained tunnels)
    uint32_t _conKeepNext = 0;
    uint32_t _conLastInputMs = 0;                                   // millis() of the last local line -> suspend the idle poll while typing (TP: keeps _conSub==0 free for the command instead of colliding with an in-flight drain)
    uint32_t _conStartMs = 0;                                       // millis() at conOpen() -> session duration on close (0 = never opened)
    void consoleFeedLine(const char *line);
    static void consoleFeedLineStatic(const char *line);            // Console line-sink trampoline -> instance()->consoleFeedLine
    void conSend(uint8_t pid, const uint8_t *payload, uint8_t len); // A_FunctionProperty_Command on obj 160 (arms _ftcRespPending)
    void conAfterProbe(uint8_t features, bool answered);
    void conOpen();
    void conRefuse(const char *reason);                             // OPEN refused (auth/locked/busy) -> one-line note, no session
    void conClose(const char *reason, bool sendClose);
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
    volatile uint8_t _ftcDdTail = 0;
    uint32_t _scanNext = 0;          // next PA to probe (32-bit so the top of the range cannot wrap)
    uint32_t _scanEnd = 0;           // last PA to probe, inclusive
    uint32_t _scanProbed = 0;
    uint16_t _scanFound = 0;
    uint32_t _scanLastSend = 0;   // spacing timer between probes
    uint32_t _scanSpacingMs = 40; // gap between probes; larger cross-line so the IP path is not flooded
    uint32_t _scanDrainMs = 2500;
    uint8_t _scanSweeps = 1;
    uint8_t _scanSweep = 1;
    uint32_t _scanStart = 0;
    char _scanLabel[24] = {0};
    // CO scan (`ets`): strictly serial, one address in flight (see FtcScanCo in loop()).
    bool _scanCo = false;       // this sweep is connection-oriented
    uint8_t _scanCoPhase = 0;
    uint32_t _scanCoT0 = 0;     // per-phase deadline (connect ack / read answer)
    uint32_t _scanCoAckT0 = 0;  // instant the device T_ACKed our CO read (presence-on-ACK) -> short grace for the DeviceDescriptor (class), then advance
    static constexpr uint32_t FTC_CO_ACK_GRACE = 140; // ms to wait after the presence T_ACK for the DD response (class); then record + advance
    bool _scanCoGot = false;
    uint32_t _scanCoBusyT0 = 0; // phase-0 first "TP busy" instant -> abort the CO scan if the link stays owned (FTC_CO_BUSY_TMO)

    // --- post-sweep probe + CSV save (`openknx` / `info` / `save` flags): cooperative in FtcScanPost --
    // ONE PID read and at most a few file rows per loop() pass -> never blocks (Feature B/C). Attaches at
    // scanReport() (deferral) and in FtcScanPost only -- the FtcScanCo SM above is left untouched.
    bool _scanOpenKnx = false;               // openknx OR info flag -> run the post-probe (adds the CSV `openknx` column)
    bool _scanInfo = false;
    bool _scanPostDone = false;
    bool _scanInfoActive = false;            // a FULL info chain is running as a scan probe (gates the FtcDevProp short-circuit)
    uint16_t _scanOkIdx = 0;
    uint16_t _scanOkNum = 0;                 // 1-based count of System B devices probed so far (progress "k/N")
    uint16_t _scanOkTotal = 0;               // total System B candidates (the "k/N" denominator)
    uint8_t _scanProbeInFlight = 0;          // 0 = none, 1 = LIGHT (single PID-11 read), 2 = FULL (device-info chain)
    bool _scanProbeDone = false;             // the in-flight probe has completed -> FtcScanPost consumes it
    bool _scanProbeAnswered = false;         // the current device answered (else empty CSV columns / "no answer")
    char _scanSavePath[FTC_PATH_MAX] = {0};  // `save <path>`: empty = do not save the listing
    bool _scanSinkOpen = false;
    bool _scanSaveErr = false;               // a sink write failed -> stop writing (logged once)
    uint16_t _scanSaveN = 0;
    const FtcBackend *_scanSaveBe = nullptr;

    // --- device info (ftc <pa> info, no file): chained DeviceDescriptor -> ModuleVersion -> CheckFeatures ---
    uint16_t _devMask = 0; // 0 = no DeviceDescriptor answer
    uint16_t _devVerMaj = 0, _devVerMin = 0, _devVerRev = 0;
    uint8_t _devFeat = 0; // bit0 Resume, bit1 Update
    bool _devHasMask = false, _devHasVer = false;
    void ftcDevInfoBegin(uint16_t pa, bool fromScan);
    void ftcDevReport();        // print the assembled device-info block
    void ftcBcuStartBlockRead(); // BCU1/BCU2: A_Memory_Read the fixed identity block (0x0100..) over the open CO link -> FtcBcu1Mem
    // --- group communication (ftc <pa> info ga): reuse the device-info discovery, then A_Memory_Read walk ---
    void ftcGaBeginWalk();     // open a T_Connect for the walk (ETS reads memory CO); connectionless fallback if it can't
    void ftcGaAdvance();       // send the next present table's PID_TABLE_REFERENCE read, or emit the report + finish
    void ftcGaParse();         // extract the just-walked table from _memBuf (address -> _gaList; assoc stays in _memBuf)
    void ftcGaReport();        // header + resolved GA list + com-object links, via ftcOut (cooperative)
    bool _gaConnected = false; // we opened a T_Connect for the memory walk -> ftcFinish() closes it (ours only, never an ETS session)

    uint8_t _devPropStep = 0;
    uint16_t _devMfr = 0;        // manufacturer id (serial number bytes 0..1)
    uint8_t _devSerial[6] = {0}; // full KNX serial number (mfr[2] + serial[4])
    uint8_t _devOrder[10] = {0};
    uint8_t _devHw[6] = {0};
    uint8_t _devFw[4] = {0};     // PID_VERSION raw
    uint8_t _devFwLen = 0;
    uint8_t _devProgMode = 0; // PID_PROG_MODE (0 = off, 1 = on)
    uint8_t _devBcuRunState = 0xFF, _devBcuPei = 0xFF, _devBcuRunError = 0xFF; // BCU1/BCU2 memory-map identity
    uint8_t _devBcuMfr = 0, _devBcuApp[3] = {0};                               // BCU manufacturer + application id (BCD)
    bool _devHasBcuApp = false;
    bool _devHasBcu1 = false;                                                  // the BCU identity block was decoded
    bool _devHasSerial = false, _devHasOrder = false, _devHasHw = false, _devHasFw = false, _devHasProg = false;

    // device-info: connection-oriented DeviceDescriptor fallback (BCU1/old masks answer only CO)
    bool _devCoTried = false; // the CO DeviceDescriptor retry has been attempted (once)
    bool _devCoConn = false;  // a T_Connect we opened for the CO fallback is up -> ours to close in ftcFinish()

    uint8_t _devObjProbe = 1;                                                     // object index being probed for its type
    int8_t _devIdxAddr = -1, _devIdxAssoc = -1, _devIdxApp = -1, _devIdxGrp = -1; // found indices (-1 = none)
    uint16_t _devAppMfr = 0, _devAppNum = 0;
    uint8_t _devAppVer = 0;                                                       // application program version byte
    bool _devHasApp = false;
    uint8_t _devLoad[4] = {0}; // load state: [0]addr [1]assoc [2]app [3]grp
    bool _devLoadHas[4] = {false, false, false, false};
    uint8_t _loadQObj[8] = {0}, _loadQPid[8] = {0}, _loadQN = 0, _loadQi = 0;
    // Property-Response handoff (single outstanding read, written in stack context, read in loop):
    volatile bool _propPending = false;
    uint8_t _propObj = 0, _propPid = 0, _propLen = 0;
    uint8_t _propData[16] = {0};

    // --- group communication (ftc <pa> info ga): walk the GA + association tables via A_Memory_Read (ETS path) ---
    static constexpr uint16_t FTC_GA_MAX_BYTES = 640; // bounded table-memory accumulator (per table); 640 covers a big System-2 assoc/GrOT (e.g. 1.1.20: ~250 assoc, ~148-obj GrOT)
    static constexpr uint16_t FTC_GA_MAX = 300;       // max GAs / association entries parsed (bounds every buffer write); large enough for a fully-loaded System-2 device
    bool _gaMode = false;
    bool _gaClassic = false;                          // table family: true = BCU realisation type 1/2 (1B count, 2B entries, 8b ASAP); false = System B (2B count, 4B assoc entries, 16b ASAP)
    uint8_t _gaWhich = 0;                             // table being walked: 0 = address, 1 = association, 2 = group-object descriptors (flags/prio/size)
    uint16_t _gaRef = 0;                              // current table's memory base (PID_TABLE_REFERENCE, low word)
    uint16_t _gaGot = 0;                              // bytes accumulated for the current table
    uint16_t _gaExpect = 0;                           // total bytes to read (from the leading count), clamped to FTC_GA_MAX_BYTES
    bool _gaAssocValid = false;                       // the association walk left valid bytes in _memBuf -> gate the report's KO section
    volatile bool _memPending = false;                // a MemoryResponse is parked (stack ctx) -> consumed in loop()
    volatile bool _adcPending = false;                // an ADCResponse is parked (stack ctx) -> consumed in loop()
    int16_t _adcVal = 0; uint8_t _adcCnt = 0;         // parked A_ADC_Response: raw sum + sample count
    uint16_t _devBusVoltmV = 0; bool _devHasBusVolt = false;
    uint8_t _memBuf[FTC_GA_MAX_BYTES];                // current table's raw blob; ftcOnMemory writes each chunk at its true offset
    uint8_t _memLen = 0;                              // length of the chunk ftcOnMemory just parked
    // _ftcRecvBmp (fast upload) and {_gaList,_gaObjects} (info-ga) are never live at once -- one FtcState
    // machine + isBusy() serialises the ops -- so they share storage (~1 KB saved). Each arm is fully
    // (re)initialised before any read: the bitmap is memset at fast-open; every GA row has ALL fields set
    // at creation and reads are bounded by _gaN/_gaObjectsN. _memBuf is NOT here: its writer ftcOnMemory
    // is an async callback (stale/duplicate frames) and must never land on the bitmap.
    union
    {
        uint8_t _ftcRecvBmp[FTC_BMP_BYTES]; // client mirror of the server's received-bitmap (absolute seq-1)
        struct { uint16_t _gaList[FTC_GA_MAX]; FtcGaEntry _gaObjects[FTC_GA_MAX]; };
    };
    uint16_t _gaN = 0;                                // resolved GAs in _gaList (_gaList[t-1] = TSAP t, 1-based)
    uint16_t _gaObjectsN = 0;                         // association rows in _gaObjects

    FtcFsInfo _fsInfo;
    FtcDeviceInfo _deviceInfo;
    FtcTransferSetup _xferSetup;
    FtcTransferResult _xferResult;

    // --- locate (ftc <pa> led): drive the target's prog-mode LED; blink toggles from loop() while idle ---
    static constexpr uint32_t FTC_LED_BLINK_MS = 700; // half-period of the locate blink
    uint16_t _ledBlinkPa = 0;                         // 0 = no blink running; else the PA whose LED is toggling
    uint32_t _ledBlinkNext = 0;
    uint8_t _ledBlinkOn = 0;

    // --- directory listing (ftc ll/ls): DirList collects entries, then FileInfo fills size+CRC ---
    // Cleared at ll/ls/scan start, filled during the walk, then RELEASED in ftcFinish() (swap-to-empty, not
    // just clear -> the grown capacity is returned to the heap; nothing reads it once the op completes).
    std::vector<FtcEntry> _ftcListing;
    char _ftcListDir[FTC_PATH_MAX] = {0};
    uint16_t _ftcDirIdx = 0;
    uint32_t _ftcListBytes = 0;
    uint16_t _ftcListFiles = 0, _ftcListDirs = 0;
    // FilesystemInfo(46): target LittleFS capacity for `df`, the `ll` footer, and the pre-upload space gate.
    uint32_t _ftcFsTotal = 0, _ftcFsUsed = 0; // last FilesystemInfo answer (total, used bytes)
    uint8_t _ftcFsPurpose = 0;                // 0 = df, 1 = pre-upload space check, 2 = ll footer
    bool _ftcFsKb = false;                    // last answer was in KB (provider, status 0x01) vs bytes (LittleFS)
    uint32_t _ftcTargetHave = 0;              // existing target file's size (FtcResumeInfo) -> overwrite credit for the space check
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
    char _crcPerfDrive[8] = {0};      // perf target drive: "sd" / "efc" (empty = LittleFS)
    bool _ftcSpaceChecked = false;                                       // the pre-upload space check has run for THIS transfer (once; survives a retry)
    void ftcSendFsInfo(uint8_t purpose, const char *path = "");                                 // send FilesystemInfo(46) -> FtcFsInfo, remembering why
    void ftcPrintFsBar(uint32_t total, uint32_t used, bool full = true); // df=full block; ll footer=just the two bars
    bool _ftcListDetailed = true;                                        // ll -> per-file size+CRC; ls -> names only

    // Cooperative console output: multi-line ll/ls/df blocks are queued here and drained one line per
    // loop() pass (gated on freeLoopTime()) so no pass blocks on USB-CDC; the state machine waits meanwhile.
    std::vector<FtcOutLine> _ftcOut;
    size_t _ftcOutPos = 0;
    bool ftcDrainOut(); // emit a budget's worth; true = emitted this pass -> caller yields (ftcOut is public above)

    // --- read surface for UIs (mirrors the console logs as data) ---
    FtcStatus _status;
    void ftcStatusReset(FtcPhase phase, uint16_t pa, const char *path);
    void ftcStatusMsg(const char *msg);

    void ftcDlSendOpen();  // FileDownload open: [00][00][pkg][path]
    void ftcDlSendChunk();
    void ftcCloseSink();
    // FtcDownloadCrcPrefix finished: resume-open the sink at keepBytes and start chunks, or fall back to fresh.
    void ftcDlAfterPrefixFold();
    void ftcDownloadPanel(bool ok, bool statOk, uint32_t tcrc); // framed DOWNLOAD result box + Verify line (FtcDownloadVerify)
    uint32_t _dlSize = 0;
    uint16_t _dlChunks = 0;
    uint8_t _dlPayload = 240;                                   // data bytes/chunk requested on download (FTC_DL_PAYLOAD default; settable via [pkg])
    uint16_t _dlSeq = 0;                                        // current chunk (1-based)
    uint32_t _dlWritten = 0;                                    // bytes written to the sink so far
    uint32_t _dlStartMs = 0;
    uint32_t _dlEndMs = 0;                                      // transfer end (last chunk) -- frozen before the post-download verify round-trip
    uint32_t _dlCrc = 0;                                        // running CRC32/POSIX folded over the received stream -> compared to the target FileInfo CRC32
    bool _dlSinkOpen = false;
    char _dlLocal[FTC_PATH_MAX] = {0}; // local (resolved) destination path, prefix already stripped
    // --- download resume + auto-resume (additive; fresh download stays byte-identical) ---
    bool _ftcDownload = false;         // this op is a download -> ftcAbort auto-resume + FtcCancel re-trigger pick the download path
    bool _dlNoResume = false;          // no-resume: discard any local partial, fetch fresh (truncate) -- mirrors _ftcNoResume
    uint32_t _dlResumeBase = 0;        // bytes already on the local sink kept by a resume (0 = fresh); whole-chunk boundary
    uint32_t _dlCrcOff = 0;            // cooperative prefix-CRC cursor: on-disk bytes already folded into _dlCrc
    // Re-trigger params (auto-resume re-runs requestDownload with these). _dlLocalArg keeps the ORIGINAL
    // prefixed local arg so the backend re-resolves (sd/efc), unlike _dlLocal which is already stripped.
    uint16_t _dlTarget = 0;
    uint8_t _dlPkg = 0;
    char _dlRemote[FTC_REMOTE_PATH_LIMIT + 1] = {0};
    char _dlLocalArg[FTC_PATH_MAX] = {0};

    // Backends are resolved directly (ftcResolveBackend): LittleFS default + optional sd/efc bridges. One
    // transfer at a time -> a single active source/sink pair remembered for this transfer's read/write/close.
    const FtcFileSource *_activeSrc = nullptr;
    const FtcFileSink *_activeSink = nullptr;
    const FtcBackend *_dlBackend = nullptr;    // resolved backend for the active download (space gate + messages)
    // Resolve a local path's prefix to a backend; *stripped points past a named prefix (at the '/'), or
    // at the whole path for the default. Returns nullptr only if no backend (not even a default) matches.
    const FtcBackend *ftcResolveBackend(const char *path, const char **stripped);

    FileTransferClientConsole _console;

    static FileTransferClient *_instance;
};

extern FileTransferClient openknxFileTransferClient;
#endif

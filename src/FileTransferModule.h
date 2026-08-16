/**
 * @file        FileTransferModule.h
 * @brief       KNX file transfer SERVER: serves file, directory, firmware-update and console commands
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 */
#if defined(ARDUINO_ARCH_RP2040) || defined(ARDUINO_ARCH_ESP32)
#include "FileTransferConfig.h"
#include "FastCRC.h"
#include "OpenKNX.h"
#include <Arduino.h>
#include <LittleFS.h>

#define HEARTBEAT_INTERVAL 30000 // ms without a client heartbeat -> close the open file/dir

class FileTransferModule : public OpenKNX::Module
{
  public:
    const std::string name() override;
    const std::string version() override;
#ifdef OPENKNX_FTC_SECURITY
    bool processCommand(const std::string cmd, bool diagnoseKo) override; // "ftm sec/pw/secwin" test overrides
    void showHelp() override;
#endif
    const uint8_t _major = MODULE_FileTransferModule_Version_Major; // also update library.json
    const uint8_t _minor = MODULE_FileTransferModule_Version_Minor;
    const uint8_t _revision = MODULE_FileTransferModule_Version_Revision;
    void loop(bool configured) override;

  private:
    // Remote `ll sd/…` / `ll efc/…` are served directly from sd::/efc::fileStore (guarded); LittleFS uses
    // the default _dir path. True while iterating that provider's listing instead of the LittleFS _dir.
    bool _sdDirActive = false;
    bool _efcDirActive = false;

    uint32_t _rebootRequested = 0;
    uint32_t _heartbeat = 0;
    uint32_t _lastAccess = 0;
    File _file;
    File _dir;
    uint8_t _size = 0;
    bool _fileOpen = false;
    bool _dirOpen = false;
    uint16_t _lastSequence = 0;

    // Active transfer target: LittleFS (_file) by default, or the SD / ext-flash provider store for a
    // "sd/…" / "efc/…" path. The store's src/sink is stateful (one transfer at a time), like _file.
    uint8_t _xferDrive = 0;  // 0 LittleFS · 1 sd · 2 efc
    uint32_t _xferSize = 0;  // active download size (drives have no _file.available())
    bool _xferWrite = false; // active transfer writes a sink (upload) vs reads a src (download)
    // Open the transfer source/sink on the right drive (path prefix); read/write/close route by _xferDrive.
    int32_t ftmXferOpenRead(const char *path);         // -> size, or -1
    bool ftmXferOpenWrite(const char *path, bool resume, uint32_t sizeHint = 0);
    void ftmXferClose();

#ifdef OPENKNX_FTC_CONSOLE
    // --- Console-tunnel server (obj 160, separate from the FTC-159 command table). Single logical
    // session: IN parks a line / OUT drains the log ring, both bounded; the command runs in loop(). ---
    static constexpr uint8_t CON_OBJECT_INDEX = 160; // separate from FTC-159
    static constexpr uint8_t CON_PID_IN = 1;         // A_FunctionProperty_Command: input / control
    static constexpr uint8_t CON_PID_OUT = 2;        // A_FunctionProperty_Command: output drain
    static constexpr uint8_t CON_DRAIN_MIN = 4; // smaller requests are ignored; keeps margin under a 15-octet APDU
    static constexpr uint8_t CON_DRAIN_MAX = 247;    // max console text per PID_OUT answer (one APDU minus the 7 B header)
    static constexpr uint32_t CON_IDLE_TMO = 60000;  // reap an orphaned session, re-enable the local console
    bool _conActive = false;
    uint16_t _conOwnerPa = 0;                    // from the OPEN payload, logging only (the hook carries no PA)
    char _conOwnerStr[20] = {};                  // persistent "remote a.b.c" -> handed to disableConsole() so local input gets a reason
    uint32_t _conCursor = 0;                     // read position (ringWritePos scale, monotonic)
    bool _conCmdPending = false;                 // a parked line waits to run in conLoop()
    char _conLine[CONSOLE_INPUT_SIZE + 1] = {};  // one parked command line (Console.h: 100 + NUL)
    uint32_t _conLastAccess = 0;                 // last IN/OUT touch -> idle reap
    bool _conOverflow = false;                   // ring wrapped past the cursor -> report truncation once
    // FunctionProperty branch for obj 160: OPEN/CLOSE/line on IN, bounded ring drain on OUT. Only parks +
    // copies (<=247 B) -> never blocks the main loop; the command itself runs in conLoop().
    bool conFunctionProperty(uint8_t pid, uint8_t len, uint8_t *data, uint8_t *res, uint8_t &resLen);
    void conLoop(); // run a parked command under freeLoopTime() + reap idle sessions
#endif

    // Cooperative (non-blocking) SD/EFC whole-file CRC for FileInfo with the CRC-request flag. Reads a bounded
    // slice per loop() pass under freeLoopTime(); the client polls FileInfo until the answer flips from
    // 0x02 (computing) to 0x00 + size + crc. Enables SD/EFC resume + a real verify without a blocking read.
    // MUST live OUTSIDE the OPENKNX_FTC_CONSOLE guard above: the .cpp references these under the independent
    // OPENKNX_SDCARD/OPENKNX_EXTFLASH guard, so a console-less build would otherwise drop the declarations.
    // Also left unguarded here (the macros are defined by a header included after this one); the crcLoop/
    // crcCancel bodies + call sites stay guarded, so a non-SD/EFC build never defines nor references them.
    bool _crcActive = false;
    uint8_t _crcDrive = 0;       // FD_SD / FD_EFC
    char _crcPath[64] = {};      // rel path the job CRCs (identifies a re-poll vs a new request)
    uint32_t _crcSize = 0;
    uint32_t _crcOff = 0;
    uint32_t _crcVal = 0;        // running CRC32; final once _crcOff >= _crcSize
    bool _crcFirst = true;
    uint32_t _crcLastAccess = 0; // last FileInfo(flag) poll -> idle-cancel a stranded job
    FastCRC32 _crc32;
    File _crcFile;               // LittleFS read handle for the cooperative CRC job (FD_INT)
    void crcLoop();              // advance the CRC by a bounded slice; called from loop() (SD/EFC builds)
    void crcCancel();            // close the read handle + clear the job state

    // Max result payload: octetCount = 3 + resultLength must stay <= 254. Tighter than the 255 B bau buffer.
    static constexpr uint8_t FTM_RESULT_MAX = 251;

    // Fast-transfer server state, STATIC (no malloc). Received-bitmap indexed by (seq-1); a bit is set
    // only after the per-chunk CRC verifies AND the write succeeds. Over FTM_FAST_MAX_CHUNKS -> 0x4A -> classic.
    static constexpr uint16_t FTM_FAST_MAX_CHUNKS = 8192;
    uint8_t _fastBitmap[FTM_FAST_MAX_CHUNKS / 8]; // 1024 B, absolute, seq-1 indexed (global => zero-init)
    uint16_t _fastExpectedChunks = 0;             // chunk count from the fast open; sizes the report + bounds
    uint32_t _fastRateBytes = 0;                  // new (non-resend) bytes written since the last cmd45 -> ingest rate
    uint32_t _fastRateMs = 0;                     // millis() of the last cmd45; the interval for the reported rate

#ifdef OPENKNX_FTC_SECURITY
    // --- FTC access control (opt-in via -D OPENKNX_FTC_SECURITY + FileTransfer.share.xml). Gates every FTC
    // WRITE command (obj 159) and the console OPEN (obj 160). Reading stays open (except stage "Off").
    // One global, best-effort window (NOT PA-bound, "egal von wem"), refreshed by each accepted write;
    // the initial authorization is a single challenge-response over the knx AES. Nothing runs per data
    // chunk -> the upload hot path is unchanged. Everything here compiles out without the flag. ---
    static constexpr uint8_t FTM_SEC_OFF = 0, FTM_SEC_PROG = 1, FTM_SEC_ALWAYS = 2, FTM_SEC_PW = 3;
    static constexpr uint32_t SEC_WINDOW_MIN = 30;       // clamp for the ETS idle timeout (s)
    static constexpr uint32_t SEC_WINDOW_MAX = 3600;     // clamp for the ETS idle timeout (s)
    static constexpr uint32_t SEC_CHALLENGE_TTL = 30000; // one outstanding challenge lifetime (ms)
    static constexpr uint8_t SEC_MAC_LEN = 4;            // MAC bytes compared (forgery margin 2^-32)
    static constexpr uint8_t ST_AUTH_REQUIRED = 0xA0, ST_AUTH_FAILED = 0xA1, ST_WRITES_DISABLED = 0xA2;
    // ParamFTM_Security / ParamFTM_Password are read LIVE (not cached): a device may boot unconfigured and
    // then be programmed without a setup() re-run, so a cached stage would go stale. paramByte/paramData are
    // cheap RAM reads. Precedent: NetworkModule reads ParamNET_OTAUpdate live for the same reason.
    bool _authorized = false;           // global window open
    uint32_t _authLastMs = 0;           // last accepted write / auth -> idle timeout
    uint8_t _nonce[16] = {};            // current outstanding challenge
    bool _challengePending = false;
    uint32_t _challengeMs = 0;          // challenge issue time -> 30 s TTL
    uint8_t _seedKey[16] = {};          // AES-derived nonce seed (bus observer never sees it)
    bool _seeded = false;
    uint32_t _nonceCtr = 0;             // monotonic -> nonce never repeats
    uint8_t _authFailCount = 0;         // consecutive 0xA1 -> non-blocking back-off
    uint32_t _authBackoffStart = 0;     // back-off window start (wrap-safe: compare elapsed, not absolute)
    uint32_t _authBackoffDur = 0;       // back-off duration (ms); 0 = no back-off active
    uint32_t secWindowMs();                        // live ETS idle timeout (ParamFTM_AuthTimeout, clamped) in ms
    uint8_t secStage();                            // live ParamFTM_Security (unconfigured -> ALWAYS)
    bool secWriteAllowed();                        // stage + progMode() + window (unconfigured -> allow)
    static bool secIsWriteCommand(uint8_t propertyId);
    void secMakeNonce();                           // fill _nonce (seeded AES CTR, one block)
    static void secComputeMac(const uint8_t *key, const uint8_t *nonce, uint8_t *out16); // CBC-MAC(1 block) = AES_ECB(key, nonce)
    void cmdAuthChallenge(uint8_t *resultData, uint8_t &resultLength);            // cmd 103
    void cmdAuthResponse(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength); // cmd 104
    // Extend the idle window. Does NOT open it: _authorized is set ONLY on a verified login (cmdAuthResponse).
    // (Security review: setting it here would leak a stale-open window across an Always->Password stage flip.)
    inline void secRefreshWindow() { _authLastMs = millis(); }
    // Auth-failed result payload: [ST_AUTH_FAILED, remainSecHi, remainSecLo]. The 2 back-off seconds let the
    // client report "next try in N min". Old clients read only byte 0 (backward-compatible). len set to 3.
    inline void secAuthFail(uint8_t *res, uint8_t &len, uint32_t sec)
    { if (sec > 0xFFFF) sec = 0xFFFF; res[0] = ST_AUTH_FAILED; res[1] = (uint8_t)(sec >> 8); res[2] = (uint8_t)sec; len = 3; }
    // TEST/diagnostic runtime overrides of the ETS security config, set via the local serial console
    // ("ftm sec/pw/secwin"). Never persisted -> a reboot restores the ETS config. Let a tester flip all four
    // stages + a known password + a short idle window without an ETS download (which would reboot the device).
    int8_t _secStageOvr = -1;    // -1 = none (ETS param wins); else an FTM_SEC_* value
    uint32_t _secWinOvrS = 0;    // 0 = none; else idle-window seconds (bypasses the [30,3600] clamp for testing)
    uint8_t _secPwOvr[16] = {};  // runtime test key (null-padded password == AES key), used iff _secPwOvrSet
    bool _secPwOvrSet = false;
#endif

    bool processFunctionProperty(uint8_t objectIndex, uint8_t propertyId, uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength) override;
    void readFile(uint16_t sequence, uint8_t *resultData, uint8_t &resultLength);
    // Position-write core shared by the classic per-chunk path and the fast handler: seek to
    // (sequence-1)*_size and write n payload bytes; true only if the seek AND full write succeeded.
    bool writeChunk(uint16_t sequence, const uint8_t *payload, uint8_t n);
    void writeFile(uint16_t sequence, uint8_t *data, uint8_t length, uint8_t *resultData, uint8_t &resultLength);
#if OPENKNX_FTC_FASTUPLOAD
    // FAST upload (cmd44): open(00 00, answered) / close(FF FF, answered) / data(SILENT). Returns the
    // "handled" flag so a DATA frame produces NO L7 answer (the dispatcher only responds if handled).
    bool cmdFileUploadFast(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength);
    // FAST gap report (cmd45): answer the received-bitmap for a window so the client resends only gaps.
    void cmdFileReport(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength);
#endif

    bool checkOpenedFile(uint8_t *resultData, uint8_t &resultLength);
    bool checkOpenedDir(uint8_t *resultData, uint8_t &resultLength);
    bool checkOpenFile(uint8_t *resultData, uint8_t &resultLength);
    bool checkOpenDir(uint8_t *resultData, uint8_t &resultLength);
    void cmdModuleVersion(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength);
    void cmdFormat(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength);
    void cmdRename(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength);
    void cmdExists(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength);
#if defined(ARDUINO_ARCH_RP2040) || defined(ARDUINO_ARCH_ESP32)
    void cmdFwUpdate(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength);
    // Is there a slot to write an update into? RP2040 always has one (PicoOTA); on ESP32 a single-app layout
    // has none (IDF returns the RUNNING partition). Checked before writing + before cmdCheckFeatures.
    static bool otaSlotAvailable();
#ifdef ARDUINO_ARCH_ESP32
    static bool espImageFitsThisChip(const uint8_t *hdr24, uint16_t &imgChip, uint16_t &runChip);
    #if OPENKNX_FTC_GZIP_UPDATE
    size_t inflateToOta(File &img, size_t dataStart, size_t outSize); // gzipped staged image -> OTA slot
    #endif
#endif
#endif
    void cmdFileInfo(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength);
    void cmdFilesystemInfo(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength);
    void cmdCancel(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength);
#if OPENKNX_FTC_DIROPS
    void cmdDirList(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength);
    void cmdDirCreate(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength);
    void cmdDirDelete(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength);
#endif
    void cmdFileDelete(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength);
    void cmdFileUpload(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength);
#if OPENKNX_FTC_DOWNLOAD
    void cmdFileDownload(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength);
#endif
    void cmdCheckFeatures(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength);
};

extern FileTransferModule openknxFileTransferModule;
#endif

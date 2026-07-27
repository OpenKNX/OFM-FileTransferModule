#if defined(ARDUINO_ARCH_RP2040) || defined(ARDUINO_ARCH_ESP32)
#include "FastCRC.h"
#include "OpenKNX.h"
#include <Arduino.h>
#include <LittleFS.h>

#define HEARTBEAT_INTERVAL 30000

class FileTransferModule : public OpenKNX::Module
{
  public:
    const std::string name() override;
    const std::string version() override;
    const uint8_t _major = MODULE_FileTransferModule_Version_Major; // also update library.json
    const uint8_t _minor = MODULE_FileTransferModule_Version_Minor;
    const uint8_t _revision = MODULE_FileTransferModule_Version_Revision;
    void loop(bool configured) override;

  private:
    uint32_t _rebootRequested = 0;
    uint32_t _heartbeat = 0;
    uint32_t _lastAccess = 0;
    File _file;
    File _dir;
    uint8_t _size = 0;
    bool _fileOpen = false;
    bool _dirOpen = false;
    uint16_t _lastSequence = 0;

#ifdef OPENKNX_FTC_CONSOLE
    // --- Console-tunnel server (obj 160, separate from the FTC-159 command table). Single logical
    // session: IN parks a line / OUT drains the log ring, both bounded; the command runs in loop(). ---
    static constexpr uint8_t CON_OBJECT_INDEX = 160; // separate from FTC-159
    static constexpr uint8_t CON_PID_IN = 1;         // A_FunctionProperty_Command: input / control
    static constexpr uint8_t CON_PID_OUT = 2;        // A_FunctionProperty_Command: output drain
    static constexpr uint8_t CON_DRAIN_MIN = 8;      // smallest sensible drain cap a client may request
    static constexpr uint8_t CON_DRAIN_MAX = 247;    // max console text per PID_OUT answer (one APDU minus the 7 B header)
    static constexpr uint32_t CON_IDLE_TMO = 60000;  // reap an orphaned session, re-enable the local console
    bool _conActive = false;
    uint16_t _conOwnerPa = 0;                    // from the OPEN payload, logging only (the hook carries no PA)
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

    // Fast-transfer server state, STATIC (no malloc). Received-bitmap indexed by (seq-1); a bit is set
    // only after the per-chunk CRC verifies AND the write succeeds. Over FTM_FAST_MAX_CHUNKS -> 0x4A -> classic.
    static constexpr uint16_t FTM_FAST_MAX_CHUNKS = 8192;
    uint8_t _fastBitmap[FTM_FAST_MAX_CHUNKS / 8]; // 1024 B, absolute, seq-1 indexed (global => zero-init)
    uint16_t _fastExpectedChunks = 0;             // chunk count from the fast open; sizes the report + bounds

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
#endif

    bool processFunctionProperty(uint8_t objectIndex, uint8_t propertyId, uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength) override;
    void readFile(uint16_t sequence, uint8_t *resultData, uint8_t &resultLength);
    // Position-write core shared by the classic per-chunk path and the fast handler: seek to
    // (sequence-1)*_size and write n payload bytes; true only if the seek AND full write succeeded.
    bool writeChunk(uint16_t sequence, const uint8_t *payload, uint8_t n);
    void writeFile(uint16_t sequence, uint8_t *data, uint8_t length, uint8_t *resultData, uint8_t &resultLength);
    // FAST upload (cmd44): open(00 00, answered) / close(FF FF, answered) / data(SILENT). Returns the
    // "handled" flag so a DATA frame produces NO L7 answer (the dispatcher only responds if handled).
    bool cmdFileUploadFast(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength);
    // FAST gap report (cmd45): answer the received-bitmap for a window so the client resends only gaps.
    void cmdFileReport(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength);
    // bool processFunctionPropertyState(uint8_t objectIndex, uint8_t propertyId, uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength) override;

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
#endif
    void cmdFileInfo(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength);
    void cmdFilesystemInfo(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength);
    void cmdCancel(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength);
    void cmdDirList(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength);
    void cmdDirCreate(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength);
    void cmdDirDelete(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength);
    void cmdFileDelete(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength);
    void cmdFileUpload(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength);
    void cmdFileDownload(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength);
    void cmdCheckFeatures(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength);
};

extern FileTransferModule openknxFileTransferModule;
#endif

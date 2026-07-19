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

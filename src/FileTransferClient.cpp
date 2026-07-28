/**
 * @file        FileTransferClient.cpp
 * @brief       KNX file transfer client -- pushes a file to another device's server, PA to PA
 * @version     0.0.1
 * @date        2026-07-16
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 */
#include "FileTransferClient.h"

#ifdef OPENKNX_FTC
    #include <stdarg.h>
    #include <string.h>
    #include <string>
    #include <vector>

    // Only the LittleFS default backend lives here; SD / ext-flash self-register from their own modules
    // (they include FileTransferClient.h and call registerFileBackend), so the client needs no storage header.
    #include <LittleFS.h>
    #ifdef OPENKNX_FTC_SECURITY
        #include "knx/aes.hpp" // login MAC (client side); reuses the AES already linked by knx (extern "C" wrapper)
    #endif

FileTransferClient *FileTransferClient::_instance = nullptr;

FileTransferClient::FileTransferClient() : _console(*this)
{
    FileTransferClient::_instance = this;
}

FileTransferClient *FileTransferClient::instance()
{
    return FileTransferClient::_instance;
}

const std::string FileTransferClient::name()
{
    return "FileTransferClient";
}

const std::string FileTransferClient::version()
{
    // From library.json (versions.h), the same source the server module uses -> one bump versions both.
    return MODULE_FileTransferModule_Version;
}

void FileTransferClient::registerFileBackend(const char *prefix, const FtcFileSource &src, const FtcFileSink &sink,
                                             bool (*available)(), uint64_t (*freeBytes)())
{
    if (_backendN >= 4) return; // bounded (see _backends[4]); silently ignore overflow
    _backends[_backendN].prefix = prefix ? prefix : "";
    _backends[_backendN].src = src;
    _backends[_backendN].sink = sink;
    _backends[_backendN].available = available;
    _backends[_backendN].freeBytes = freeBytes;
    _backendN++;
}

/** @brief Map a local path's prefix to a backend; *stripped -> past a named prefix (at the '/'), else the whole path. */
const FtcBackend *FileTransferClient::ftcResolveBackend(const char *path, const char **stripped)
{
    const FtcBackend *def = nullptr;
    for (uint8_t i = 0; i < _backendN; i++)
    {
        const char *p = _backends[i].prefix;
        if (p == nullptr || p[0] == '\0')
        {
            def = &_backends[i];
            continue;
        } // remember the default
        const size_t L = strlen(p);
        if (strncmp(path, p, L) == 0 && path[L] == '/') // "sd/x" -> match "sd", stripped = "/x"
        {
            *stripped = path + L;
            return &_backends[i];
        }
    }
    if (def != nullptr)
    {
        *stripped = path;
        return def;
    } // no named match -> the default, unstripped
    return nullptr;
}

/** @brief Built-in LittleFS ("") backend -- the default, always-present core FS (Arduino fs::File:
 *  size()/position()/seek()). SD ("sd") and ext-flash ("efc") self-register from their own modules with
 *  their own file-type families, so nothing storage-specific lives here anymore. */
static File _ftcSrcFile;  // LittleFS source (read) handle
static File _ftcSinkFile; // LittleFS sink (write) handle

static uint8_t ftcSharedRead(uint32_t offset, uint8_t *buf, uint8_t len) // reads are sequential; seek is a no-op
{
    if (!_ftcSrcFile || buf == nullptr || len == 0) return 0;
    if (_ftcSrcFile.position() != offset && !_ftcSrcFile.seek(offset)) return 0;
    const int r = _ftcSrcFile.read(buf, len);
    return (r > 0) ? (uint8_t)r : 0;
}
static void ftcSharedClose() { _ftcSrcFile.close(); }

static int ftcSharedSinkWrite(const uint8_t *buf, uint16_t len)
{
    if (!_ftcSinkFile || buf == nullptr || len == 0) return -1;
    return (int)_ftcSinkFile.write(buf, len);
}
static void ftcSharedSinkClose() { _ftcSinkFile.close(); }

static int32_t littleFsOpen(const char *path)
{
    _ftcSrcFile = LittleFS.open(path, "r");
    if (!_ftcSrcFile) return -1;
    return (int32_t)_ftcSrcFile.size();
}
static bool littleFsSinkOpen(const char *path)
{
    _ftcSinkFile = LittleFS.open(path, "w"); // create / truncate
    return (bool)_ftcSinkFile;
}
static bool littleFsAvailable() { return true; } // core FS -> always present
static uint64_t littleFsFree()
{
    #ifdef ARDUINO_ARCH_RP2040
    FSInfo fsinfo = {0}; // arduino-pico: fs::FS has no totalBytes()/usedBytes() -> FSInfo (as `df` does)
    LittleFS.info(fsinfo);
    const uint64_t total = fsinfo.totalBytes, used = fsinfo.usedBytes;
    #else
    const uint64_t total = (uint64_t)LittleFS.totalBytes(), used = (uint64_t)LittleFS.usedBytes();
    #endif
    return total >= used ? total - used : 0; // clamp: a bogus used > total must not underflow to a huge free
}

/** @brief Register the default LittleFS backend. SD ("sd/") and ext-flash ("efc/") self-register from
 *  their own module setups (OPENKNX_FTC-gated); order is irrelevant -- the resolver finds "" regardless. */
void FileTransferClient::setup(bool configured)
{
    registerFileBackend("", {littleFsOpen, ftcSharedRead, ftcSharedClose},
                        {littleFsSinkOpen, ftcSharedSinkWrite, ftcSharedSinkClose},
                        littleFsAvailable, littleFsFree);
}

// KNX file transfer CLIENT: this device drives another device's KnxFileTransfer server (PA -> PA, no
// PC). The server side is FileTransferModule (same OFM) -- the truth for every constant below.

// Command ids are sparse (FileTransferModule.cpp:48-64). A wrong id is silently ignored: the server
// only answers `if (handled)`, so it produces no response at all rather than an error.
static constexpr uint8_t FTC_OBJECT_INDEX = 159; // FileTransferModule.cpp:195 hard-rejects anything else
static constexpr uint8_t FTC_CMD_FILE_UPLOAD = 40;
static constexpr uint8_t FTC_CMD_FILE_DOWNLOAD = 41;
static constexpr uint8_t FTC_DL_PAYLOAD = 240; // data bytes/chunk requested on a download (answer <= 246)
static constexpr uint8_t FTC_CMD_CANCEL = 90;
static constexpr uint8_t FTC_CMD_MODULE_VERSION = 100;
static constexpr uint8_t FTC_CMD_FW_UPDATE = 101;      // arm PicoOTA + reboot ~2s (RP2040 target only); fire-and-forget, no L7 reply
static constexpr uint8_t FTC_CMD_CHECK_FEATURES = 102; // 1-byte flags: bit0 Resume, bit1 Update, bit2 FAST
#ifdef OPENKNX_FTC_SECURITY
static constexpr uint8_t FTC_CMD_AUTH_CHALLENGE = 103; // login: request a nonce
static constexpr uint8_t FTC_CMD_AUTH_RESPONSE = 104;  // login: submit the 4-byte MAC over the nonce
static constexpr uint8_t FTC_CMD_AUTH_LOGOUT = 105;    // logout: close the target's window now
static constexpr uint8_t SEC_MAC_LEN = 4;              // MAC bytes sent (must match the server)
#endif

static constexpr uint8_t FTC_CMD_FORMAT = 0; // Format: LittleFS.format() -- wipes ALL files + folders

// OpenKNX manufacturer id -- the knx stack hardcodes it; the scan `openknx`/`info` probe flags a device
// as OpenKNX when the first 2 bytes of PID_SERIAL match this.
static constexpr uint16_t FTC_MFR_OPENKNX = 0x00FA;

// Device-Object (interface object index 0) property IDs -- the standard KNX identity fields ETS reads.
static constexpr uint8_t FTC_PID_SERIAL = 11;    // 6 bytes: manufacturer id (2) + serial (4)
static constexpr uint8_t FTC_PID_ORDER = 15;     // order number (up to 10 bytes, ASCII device name)
static constexpr uint8_t FTC_PID_VERSION = 25;   // device version
static constexpr uint8_t FTC_PID_PROGMODE = 54;  // programming mode (1 byte, 0/1)
static constexpr uint8_t FTC_PID_HARDWARE = 78;  // hardware type (6 bytes: 0000 + app number + version)
static constexpr uint8_t FTC_DEV_PROP_COUNT = 5; // serial, order, version, progmode, hardware

// Object enumeration (device-info phase 2): find the app-program + table objects, read their load
// states + the app version -- ETS's "Applikationsprogramm" / "Gruppenkommunikation" blocks.
static constexpr uint8_t FTC_PID_OBJECT_TYPE = 1;   // on every interface object -> discover what it is
static constexpr uint8_t FTC_PID_LOAD_STATE = 5;    // PID_LOAD_STATE_CONTROL: 1 byte load state
static constexpr uint8_t FTC_PID_PROG_VERSION = 13; // manufacturer(2) + app number(2) + version(1)
static constexpr uint16_t FTC_OT_ADDR = 1, FTC_OT_ASSOC = 2, FTC_OT_APP = 3, FTC_OT_GRP = 9;
static constexpr uint8_t FTC_ENUM_MAX = 8; // probe object indices 1..8 for their type
// Group communication (ftc <pa> info ga): ETS reads the GA + association tables via A_Memory_Read.
static constexpr uint8_t FTC_PID_TABLE_REFERENCE = 7; // 4-byte value; its low word is the table's memory address
static constexpr uint8_t FTC_GA_STEP = 12;            // bytes per A_Memory_Read while walking a table (classic devices accept 12)
static constexpr uint8_t FTC_CMD_RENAME = 2;
static constexpr uint8_t FTC_CMD_FILE_DELETE = 42;
static constexpr uint8_t FTC_CMD_FILE_INFO = 43; // NOT 4 -- the enum is 40/41/42/43/... (FileTransferModule.cpp:48-64)
static constexpr uint8_t FTC_CMD_DIR_LIST = 80;  // stateful iterator: one entry per round trip
static constexpr uint8_t FTC_CMD_DIR_CREATE = 81;
static constexpr uint8_t FTC_CMD_DIR_DELETE = 82;

// Two retry layers: per-chunk (the frame still sits in _ftcTx -> just re-send) and whole-transfer on a
// transient failure (resume continues from the target's partial). Counts runtime-settable via `ftc retry`.

/** @brief CRC-32/POSIX (poly 0x04C11DB7, init 0, no reflect, xorout 0xFFFFFFFF) -- must match the target's FastCRC32::cksum, NOT zlib. */
uint32_t FileTransferClient::ftcCrc32Posix(uint32_t crc, const uint8_t *data, size_t len, bool first)
{
    if (first) crc = 0;
    for (size_t i = 0; i < len; i++)
    {
        crc ^= (uint32_t)data[i] << 24;
        for (uint8_t b = 0; b < 8; b++)
            crc = (crc & 0x80000000u) ? ((crc << 1) ^ 0x04C11DB7u) : (crc << 1);
    }
    return crc;
}

// Chunk overhead = 6 B: request [seq:2][len:1] + answer [seq:2][crc:2] -> payload = pkg - 6.
static constexpr uint8_t FTC_PKG_OVERHEAD = 6;
static constexpr uint8_t FTC_PKG_MIN = 16;
// 254 is the true max: pkg 254 -> FunctionProperty payload 251 (both modes) -> octetCount 254, the last valid
// value (255=0xFF is the reserved escape per 03_03_02 2.5). Requires the paired fixes: NPDU::length() as uint16
// (octetCount 254 -> 256, no uint8 wrap) and the ftcSendCommand guard raised to 251. 255 would hit the escape.
static constexpr uint8_t FTC_PKG_MAX = 254;
static constexpr uint32_t FTC_TIMEOUT = 6000;
static constexpr uint32_t FTC_TEST_SIZE = 2048;

// --- fast-transfer negotiation (phase 1) --------------------------------------------------------
// CheckFeatures(102) result bit that says the server understands the fast protocol (cmd44/cmd45).
// bit0 Resume, bit1 Update, bit2 FAST, bit3 Console -- see FileTransferModule::cmdCheckFeatures.
static constexpr uint8_t FTC_FEAT_FAST = 0x04;
    #ifdef OPENKNX_FTC_CONSOLE
static constexpr uint8_t FTC_FEAT_CONSOLE = 0x08; // server compiled with OPENKNX_FTC_CONSOLE (obj-160 tunnel)
    #endif
    #ifdef OPENKNX_FTC_SECURITY
static constexpr uint8_t FTC_FEAT_AUTH = 0x10; // server requires a password (stage 3) -> login makes sense
    #endif
// Dedicated SHORT probe window -- NOT the 6 s FTC_TIMEOUT. An old server never answers cmd102, so a
// fast/forget request must give up fast and fall back to classic instead of stalling for seconds.
static constexpr uint32_t FTC_FEATURE_TIMEOUT = 800;
// Console robustness on a CONGESTED bus: all FTC frames are low priority, so a bulk transfer starves the small
// console control frames (verified by busmon: console 114 frames vs bulk 1325 over the same window). The
// pre-flight probe is an idempotent read -> resend it a few times before declaring "no answer"; the in-session
// round-trips just need a longer window (the probe already proved the target present) -> congestion DELAYS the
// console, it must not KILL it. Only the (idempotent) probe is resent; OPEN/command/drain merely wait longer
// (no resend -> no double-exec, no re-OPEN BUSY, no drain cursor gap).
static constexpr uint8_t  FTC_CON_PROBE_RETRIES = 5;  // resend CheckFeatures up to N times on timeout (~N*800 ms budget)
static constexpr uint32_t FTC_CON_TIMEOUT = 10000;    // console OPEN/command/drain round-trip window (vs 6 s FTC_TIMEOUT)
// Chunk cap above which fast is refused and we fall back to classic (mirrors the server's static
// bitmap size FTM_FAST_MAX_CHUNKS = 8192, ~2 MB @ pkg253). Classic has no such cap.
static constexpr uint16_t FTC_FAST_MAX_CHUNKS = 8192;
// A coupler echoes each answer twice (TP + a late IP mirror). The mirror lands within a few ms -- far
// faster than a real TP round trip -- so an answer this soon after the last accepted one is a duplicate.
static constexpr uint32_t FTC_DUP_WINDOW_MS = 12;

// --- fast-transfer commands + pacing (phase 2 windowed / phase 3 forget) ------------------------
static constexpr uint8_t FTC_CMD_FILE_UPLOAD_FAST = 44; // open(answered) / data(SILENT) / close(answered)
static constexpr uint8_t FTC_CMD_FILE_REPORT = 45;      // received-bitmap gap query (answered)
static constexpr uint8_t FTC_CMD_FILESYSTEM_INFO = 46;  // LittleFS total+used bytes (df / pre-upload space check)
static constexpr uint32_t FTC_FS_MARGIN = 8192;         // headroom demanded above the payload (LittleFS block rounding + metadata)
// AIMD window bounds (plan sec.3.3, B's smaller bounds -> limits erase-stall exposure): start at 8,
// grow +8 after a clean window, halve on loss, clamp to [4,64].
static constexpr uint16_t FTC_WND_INIT = 8;
static constexpr uint16_t FTC_WND_MIN = 4;
static constexpr uint16_t FTC_WND_MAX = 64;
// TP transmit-FIFO water marks (via ftcTxQueueSize()): the pump sends only below HIGH, and drains below
// LOW before a report/close so the report reflects what the SERVER received, not still-queued frames.
// LOW=1 (empty) is deliberate: a higher LOW would fire the report behind a small window's undrained tail.
static constexpr uint16_t FTC_TX_HIGH = 30;
static constexpr uint16_t FTC_TX_LOW = 1;
// Per-loop() send cap: a real SD read costs loop time, so cap bursts hard; the RAM pattern is cheap, so
// a bigger burst is fine. Keeps the FTC pump off the "loop took >50 ms" list.
static constexpr uint8_t FTC_FAST_BURST_SD = 4;
static constexpr uint8_t FTC_FAST_BURST_RAM = 16;
// forget (mode 2) pacing: no per-chunk report and, over IP, no TP-FIFO backpressure -> it would blast
// ~94 KB/s and the target drops most chunks. Pace small bursts (BURST*~245B < the ~2 KB RX socket) via a
// millis() gate; rate ~= BURST/PACE.
static constexpr uint8_t FTC_FORGET_BURST = 4;       // 4*~245B ~= 1 KB per burst (fits a 2 KB RX socket)
static constexpr uint16_t FTC_FORGET_PACE_MS = 25;   // >= this many ms between bursts -> ~160 chunks/s ~= 39 KB/s
static constexpr uint32_t FTC_REPORT_TIMEOUT = 4000; // per report-query answer (covers the in-flight frame + round-trip)
static constexpr uint8_t FTC_REPORT_RETRIES = 3;     // report-query timeouts before abort
static constexpr uint16_t FTC_NOPROGRESS_MAX = 4;    // reports whose (union) missing count won't shrink -> abort (4: one paced recovery cycle of slack vs a coincidental target flash-erase stall)
// Forget-recovery report page: how many seqs one report covers when paging the whole file (well under
// the server's <=1928 report cap; the normal windowed path uses the tiny AIMD window instead).
static constexpr uint16_t FTC_FAST_PAGE = 1024;
// Progress-based stall backstop: abort only if NO chunk makes forward progress for this long (not a fixed
// wall clock -- a slow-but-steady TP upload must not false-abort). 30s covers the worst legit no-progress
// gap (a report round-trip + its retries ~12s, plus a server flash-erase stall).
static constexpr uint32_t FTC_FAST_STALL_MS = 30000; // no forward progress for this long -> genuinely wedged

// Scan pacing: one DeviceDescriptor_Read every SPACING ms (paced, not blasted), then wait DRAIN ms after
// the last probe for the slowest answer. The bounded listing caps RAM on a wide sweep; the log is not.
static constexpr uint32_t FTC_SCAN_SPACING_MS = 40;
static constexpr uint32_t FTC_SCAN_DRAIN_MS = 2500; // quiet time after the LAST answer, not the last probe
static constexpr uint16_t FTC_SCAN_MAX_LIST = 128;
// Console-drain buffer (_ftcOut): keep this many FtcOutLine slots (~132 B each) as steady state so frequent
// 1-line output (upload progress) does not realloc; a bigger burst (ll/ls/help ~12 lines) is released on drain.
static constexpr size_t FTC_OUT_KEEP = 4;
// CO scan (`ets`) per-address deadlines. Both exceed the TP 3-retry window (~300 ms), so a present device
// is never mis-timed as absent. Serial, so these bound the worst-case time-per-address, not a burst.
static constexpr uint32_t FTC_CO_CONNECT_TMO = 600; // no T_Connect ack in this window -> ABSENT
static constexpr uint32_t FTC_CO_READ_TMO = 800;    // connected but no DeviceDescriptor answer -> give up
static constexpr uint32_t FTC_CO_SETTLE_MS = 40;    // quiet gap after a disconnect before the next connect
static constexpr uint32_t FTC_CO_BUSY_TMO = 4000;   // phase-0 CO scan: abort if the TP link stays owned (ETS/mgmt) this long

// --- Variant-D two-line progress + result panel -------------------------------------------------
static constexpr uint32_t FTC_VERBOSE_MS = 1000;   // verbose progress cadence (1 Hz) when _ftcVerbose is on
static constexpr uint32_t FTC_PROGRESS_MS = 10000; // non-verbose progress cadence (one compact line every 10 s)
static constexpr uint8_t FTC_PROG_BAR_W = 20;      // progress-bar inner width ('#' fill / '-' track)
static constexpr uint8_t FTC_BOX_IW = 54;          // result-panel inner width (between the '|' borders)

void FileTransferClient::ftcCloseSource()
{
    if (_activeSrc && _activeSrc->close) _activeSrc->close();
}

/** @brief Reset the UI status snapshot at the start of a new operation; fields fill in as it progresses. */
void FileTransferClient::ftcStatusReset(FtcPhase phase, uint16_t pa, const char *path)
{
    _status = FtcStatus{}; // all zero / Idle defaults
    _status.phase = phase;
    _status.target = pa;
    strncpy(_status.path, path ? path : "", sizeof(_status.path) - 1);
}

void FileTransferClient::ftcStatusMsg(const char *msg)
{
    strncpy(_status.message, msg ? msg : "", sizeof(_status.message) - 1);
    _status.message[sizeof(_status.message) - 1] = 0;
}

/** @brief CRC-16/MODBUS (init 0xFFFF, reflected poly 0xA001) -- must match the target's FastCRC16::modbus(), which echoes it back to compare. */
static uint16_t ftcCrc16Modbus(const uint8_t *data, uint8_t length)
{
    uint16_t crc = 0xFFFF;
    for (uint8_t i = 0; i < length; i++)
    {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; bit++)
            crc = (crc & 1) ? ((crc >> 1) ^ 0xA001) : (crc >> 1);
    }
    return crc;
}

void FileTransferClient::ftcOnResponse(uint16_t pa, uint8_t objectIndex, uint8_t propertyId, uint8_t *data, uint8_t length)
{
    FileTransferClient *self = FileTransferClient::instance();
    if (self == nullptr) return;
    // Only accept the reply from the device we are actually talking to. In R<->R (both peers are
    // couplers that are client AND server on the same IP multicast, and the IP data-link layer does
    // not filter looped-back multicast) a foreign/mirrored FunctionPropertyStateResponse would
    // otherwise be mistaken for the target's answer and could surface a spurious 0x01 BUSY. Same
    // source-PA guard the other FTC callbacks (ftcOnPropertyValue/ftcOnMemory) already use.
    if (pa != self->_ftcTarget) return;

    // Runs inside the KNX stack's dispatch. Only park the bytes -- doing the file read and the
    // follow-up send from here would re-enter the application layer from within its own callback.
    uint8_t n = length;
    if (n > sizeof(self->_ftcResp)) n = sizeof(self->_ftcResp);
    if (n > 0 && data != nullptr) memcpy(self->_ftcResp, data, n);
    self->_ftcRespLen = n;
    self->_ftcRespObj = objectIndex;
    self->_ftcRespProp = propertyId;
    self->_ftcRespPending = true; // set last: loop() keys off this flag
}

void FileTransferClient::ftcOnDeviceDescriptor(uint16_t pa, uint8_t descriptorType, const uint8_t *data)
{
    FileTransferClient *self = FileTransferClient::instance();
    if (self == nullptr) return;
    // Stack dispatch context: only enqueue. descriptorType 0 -> the 2-byte mask version.
    const uint8_t next = (uint8_t)((self->_ftcDdHead + 1) & (FTC_DD_Q - 1));
    if (next == self->_ftcDdTail) return; // ring full -> drop (loop is behind; extremely unlikely)
    self->_ftcDdQ[self->_ftcDdHead].pa = pa;
    self->_ftcDdQ[self->_ftcDdHead].mask = (data != nullptr) ? (uint16_t)((data[0] << 8) | data[1]) : 0;
    self->_ftcDdHead = next; // publish last
}

void FileTransferClient::ftcOnPropertyValue(uint16_t pa, uint8_t objectIndex, uint8_t propertyId, const uint8_t *data, uint8_t length)
{
    FileTransferClient *self = FileTransferClient::instance();
    if (self == nullptr) return;
    if (pa != self->_ftcTarget) return; // only the device we are querying
    uint8_t n = length;
    if (n > sizeof(self->_propData)) n = sizeof(self->_propData);
    if (n > 0 && data != nullptr) memcpy(self->_propData, data, n);
    self->_propObj = objectIndex;
    self->_propPid = propertyId;
    self->_propLen = n;
    self->_propPending = true; // publish last
}

void FileTransferClient::ftcOnMemory(uint16_t pa, uint16_t addr, const uint8_t *data, uint8_t len)
{
    FileTransferClient *self = FileTransferClient::instance();
    if (self == nullptr || pa != self->_ftcTarget) return; // only the device we are walking
    // Stack dispatch context: only park. Place the chunk at its TRUE table offset (addr - base) so an
    // IP-mirror duplicate lands on the SAME bytes (idempotent), never on the next slot. loop() advances
    // the cursor and drops the time-duplicate. wrote=0 (out-of-range/empty) -> loop lets the timeout end it.
    uint8_t wrote = 0;
    if (addr >= self->_gaRef)
    {
        const uint16_t off = (uint16_t)(addr - self->_gaRef);
        if ((uint32_t)off + len <= FTC_GA_MAX_BYTES && len > 0 && data != nullptr)
        {
            memcpy(self->_memBuf + off, data, len);
            wrote = len;
        }
    }
    self->_memLen = wrote;
    self->_memPending = true; // publish last
}

/** @brief Drop a mirrored duplicate: true if it arrived within FTC_DUP_WINDOW_MS of the last accepted answer (a real TP round trip is far slower), else stamp + accept. */
bool FileTransferClient::ftcDropDup()
{
    if (_ftcRespT && millis() - _ftcRespT < FTC_DUP_WINDOW_MS) return true;
    _ftcRespT = millis();
    return false;
}

/** @brief Mask version -> short human device class. Not exhaustive; the exact model needs the order number (ftc <pa> info), not the mask. */
const char *FileTransferClient::ftcMaskName(uint16_t mask)
{
    // Mask version = medium (top nibble: 0 TP1, 1 PL110, 2 RF, 5 IP) + system generation (low 12 bits).
    static char buf[28]; // single-threaded console -> one live result at a time
    const char *cls;
    switch (mask & 0x0FFF)
    {
        case 0x012:
        case 0x013: cls = "BCU1"; break;
        case 0x020:
        case 0x021:
        case 0x025: cls = "BCU2"; break;
        case 0x300:
        case 0x700:
        case 0x701:
        case 0x705: cls = "BIM M112"; break;
        case 0x7B0:
        case 0x7B1: cls = "System B"; break;         // OpenKNX runs on System B
        case 0x91A: cls = "System-B coupler"; break; // e.g. this router
        default: cls = nullptr; break;
    }
    if (!cls)
    {
        snprintf(buf, sizeof(buf), "? (0x%04X)", mask);
        return buf;
    }
    const char *med;
    switch ((mask >> 12) & 0x0F)
    {
        case 0x1: med = " (PL)"; break;
        case 0x2: med = " (RF)"; break;
        case 0x5: med = " (IP)"; break;
        default: med = ""; break; // TP1 (0) or unknown -> no suffix
    }
    snprintf(buf, sizeof(buf), "%s%s", cls, med);
    return buf;
}

/** @brief Bounded formatted append at buf[*pos] within cap. Advances *pos; on overflow clamps to cap-1 and returns false (caller may stop). */
static bool ftcCat(char *buf, size_t cap, size_t &pos, const char *fmt, ...)
{
    if (pos >= cap) return false;
    va_list ap;
    va_start(ap, fmt);
    const int w = vsnprintf(buf + pos, cap - pos, fmt, ap);
    va_end(ap);
    if (w < 0) return false;
    if ((size_t)w >= cap - pos)
    {
        pos = cap - 1;
        return false;
    } // truncated -> buffer full
    pos += (size_t)w;
    return true;
}

void FileTransferClient::scanRecord(uint16_t pa, uint16_t mask)
{
    char paStr[16];
    snprintf(paStr, sizeof(paStr), "%u.%u.%u", (pa >> 12) & 0x0F, (pa >> 8) & 0x0F, pa & 0xFF);
    // A late second copy of the same answer (the router mirrors TP -> IP) must not be counted twice.
    for (const FtcEntry &e : _ftcListing)
        if (strcmp(e.name, paStr) == 0) return;

    _scanFound++;
    openknx.logger.logWithPrefixAndValues("FTC", "  %s   mask 0x%04X   %s", paStr, mask, ftcMaskName(mask));
    // Bounded mirror for the read API / display; the console log above is never capped.
    if (_ftcListing.size() < FTC_SCAN_MAX_LIST)
    {
        FtcEntry e{};
        strncpy(e.name, paStr, sizeof(e.name) - 1);
        e.crc = mask; // reuse: mask version of the responder
        e.hasInfo = true;
        _ftcListing.push_back(e);
    }
}

void FileTransferClient::scanReport()
{
    // Feature B/C: the sweep is done -- first run the cooperative post-sweep probe / CSV write (once), then
    // re-enter here to print. Attaches HERE (shared by FtcScan and FtcScanCo), never inside the FtcScanCo SM.
    if ((_scanOpenKnx || _scanSavePath[0]) && !_scanPostDone)
    {
        ftcScanPostBegin();
        return; // summary deferred until FtcScanPost finishes (which calls scanReport() again)
    }

    openknx.logger.color(CONSOLE_HEADLINE_COLOR);
    openknx.logger.log("------------------------------------------------------------------------------");
    openknx.logger.logWithValues("Scan %s:  %u device(s) found  /  %u address(es) probed",
                                 _scanLabel, _scanFound, (unsigned)_scanProbed);
    openknx.logger.log("------------------------------------------------------------------------------");
    openknx.logger.color(0);

    // Feature B: one-line summary of the devices the probe flagged as OpenKNX (bounded RAM string, no I/O).
    if (_scanOpenKnx && _scanPostDone)
    {
        char line[200] = {0};
        size_t p = 0;
        uint16_t n = 0;
        for (const FtcEntry &e : _ftcListing)
            if (e.isOpenKnx)
            {
                n++;
                ftcCat(line, sizeof(line), p, "%s%s", p ? ", " : "", e.name);
            }
        openknx.logger.logWithPrefixAndValues("FTC", "OpenKNX devices (mfr 0x%04X):  %s", FTC_MFR_OPENKNX,
                                              n ? line : "none");
    }

    _status.ok = true;
    _status.done = _scanProbed;
    ftcStatusMsg(_scanFound ? "scan complete" : "scan complete -- nothing answered");
    // _ftcListing is released centrally in ftcFinish() (the terminal all scan/ll/ls paths reach).
}

void FileTransferClient::requestScan(uint16_t startPa, uint16_t endPa, const char *label, uint8_t sweeps, bool co,
                                     bool probeOpenKnx, bool info, const char *savePath)
{
    if (startPa > endPa)
    {
        openknx.logger.logWithPrefix("FTC", "empty scan range");
        return;
    }
    knx.bau().ftcSetDeviceDescriptorCallback(ftcOnDeviceDescriptor);
    _ftcListing.clear();

    // Opt-in post-sweep options (Feature B/C). info implies openknx (the mfr is part of the full info).
    _scanOpenKnx = probeOpenKnx || info;
    _scanInfo = info;
    strncpy(_scanSavePath, savePath ? savePath : "", sizeof(_scanSavePath) - 1);
    _scanSavePath[sizeof(_scanSavePath) - 1] = 0;
    _scanPostDone = false;
    _scanInfoActive = false;
    _scanOkIdx = _scanOkNum = _scanOkTotal = 0;
    _scanProbeInFlight = 0;
    _scanProbeDone = _scanProbeAnswered = false;
    _scanSinkOpen = _scanSaveErr = false;
    _scanSaveN = 0;
    _scanSaveBe = nullptr;
    _scanStart = startPa;
    _scanNext = startPa;
    _scanEnd = endPa;
    _scanCo = co;
    _scanCoPhase = 0; // Connect
    _scanCoGot = false;
    _scanCoBusyT0 = 0;                            // phase-0 "TP busy" deadline (E1) -- 0 = not waiting yet
    _scanSweeps = co ? 1 : (sweeps ? sweeps : 1); // CO is reliable -> one pass, no union needed
    _scanSweep = 1;
    _scanProbed = 0;
    _scanFound = 0;
    _ftcDdTail = 0; // empty the answer ring (two statements: chained assign to a volatile is C++20-deprecated)
    _ftcDdHead = 0;
    _scanLastSend = 0; // 0 = send the first probe immediately
    strncpy(_scanLabel, label ? label : "", sizeof(_scanLabel) - 1);
    _scanLabel[sizeof(_scanLabel) - 1] = 0;

    // A cross-line range is KNXnet/IP-routed with higher, variable latency -- give it a longer drain so
    // the sweep does not finish before slow responders report.
    const uint16_t ownLine = knx.individualAddress() & 0xFF00;
    const bool crossLine = (startPa & 0xFF00) != ownLine || (endPa & 0xFF00) != ownLine;
    _scanDrainMs = crossLine ? 6000 : FTC_SCAN_DRAIN_MS;
    // Cross-line probes are KNXnet/IP-routed and lose answers to multicast/coupler drops. A slightly
    // slower probe helps a little; the real robustness comes from `deep` (several passes -> union).
    _scanSpacingMs = crossLine ? 60 : FTC_SCAN_SPACING_MS;

    ftcStatusReset(FtcPhase::Scan, 0, "");
    _status.total = (uint32_t)(endPa - startPa) + 1;
    ftcStatusMsg("scanning...");

    openknx.logger.logWithPrefixAndValues("FTC", "scan %s: %u.%u.%u .. %u.%u.%u  (%u addresses, %u sweep(s), LowPriority)",
                                          _scanLabel,
                                          (startPa >> 12) & 0x0F, (startPa >> 8) & 0x0F, startPa & 0xFF,
                                          (endPa >> 12) & 0x0F, (endPa >> 8) & 0x0F, endPa & 0xFF,
                                          (unsigned)_status.total, _scanSweeps);
    _ftcState = _scanCo ? FtcScanCo : FtcScan;
    _ftcSince = millis();
}

/** @brief Enter the cooperative post-sweep phase: count System B candidates, arm the probe callbacks, open the CSV sink. */
void FileTransferClient::ftcScanPostBegin()
{
    _scanOkIdx = _scanOkNum = _scanOkTotal = 0;
    _scanProbeInFlight = 0;
    _scanProbeDone = _scanProbeAnswered = false;
    if (_scanOpenKnx)
    {
        // Count System B candidates (bounded RAM scan, no I/O) for the "k/N" progress denominator.
        for (const FtcEntry &e : _ftcListing)
        {
            const uint16_t m = (uint16_t)e.crc;
            if ((m & 0x0FFF) == 0x07B0 || (m & 0x0FFF) == 0x07B1) _scanOkTotal++;
        }
        knx.bau().ftcSetPropertyCallback(ftcOnPropertyValue); // LIGHT + FULL property reads
        knx.bau().ftcSetResponseCallback(ftcOnResponse);      // FULL ModuleVersion/CheckFeatures
        _propPending = false;
        _ftcRespPending = false;
    }
    _scanSinkOpen = _scanSaveErr = false;
    _scanSaveN = 0;
    _scanSaveBe = nullptr;
    if (_scanSavePath[0]) ftcScanSaveOpen();
    _ftcState = FtcScanPost;
    _ftcSince = millis();
    if (_scanOpenKnx)
        openknx.logger.logWithPrefixAndValues("FTC", "%s: probing %u System B device(s)...",
                                              _scanInfo ? "info probe" : "openknx probe", _scanOkTotal);
}

/** @brief Resolve + open the CSV sink and write the header row (columns follow the active flags). */
void FileTransferClient::ftcScanSaveOpen()
{
    const char *stripped = _scanSavePath;
    const FtcBackend *be = ftcResolveBackend(_scanSavePath, &stripped);
    if (be == nullptr || be->sink.open == nullptr || be->sink.write == nullptr)
    {
        openknx.logger.logWithPrefixAndValues("FTC", "scan save: no writable backend for '%s' (use /, sd/ or efc/)", _scanSavePath);
        return;
    }
    if (be->available && !be->available())
    {
        openknx.logger.logWithPrefixAndValues("FTC", "scan save: backend '%s' not available (no card / not mounted)", be->prefix ? be->prefix : "");
        return;
    }
    // Normalise to an absolute path (LittleFS + SD want a leading '/'), into a bounded local buffer.
    char path[FTC_PATH_MAX + 1]; // +1 for the prepended '/' on a full-length stripped path
    if (stripped[0] == '/')
    {
        strncpy(path, stripped, sizeof(path) - 1);
        path[sizeof(path) - 1] = 0;
    }
    else
        snprintf(path, sizeof(path), "/%s", stripped);
    if (!be->sink.open(path))
    {
        openknx.logger.logWithPrefixAndValues("FTC", "scan save: cannot create '%s'", path);
        return;
    }
    _scanSaveBe = be;
    _scanSinkOpen = true;
    char hdr[96];
    size_t p = 0;
    ftcCat(hdr, sizeof(hdr), p, "pa,mask,class");
    if (_scanOpenKnx) ftcCat(hdr, sizeof(hdr), p, ",openknx");
    if (_scanInfo) ftcCat(hdr, sizeof(hdr), p, ",manufacturer,order,app,ftm_version,serial"); // ftm_version = the FTM/module version (empty for non-FTC devices), not the device firmware
    ftcCat(hdr, sizeof(hdr), p, "\n");
    if (be->sink.write((const uint8_t *)hdr, (uint16_t)p) != (int)p) ftcScanSaveFail();
}

/** @brief A sink write failed: log once, close, stop saving. The scan itself already succeeded -> never aborts it. */
void FileTransferClient::ftcScanSaveFail()
{
    openknx.logger.logWithPrefixAndValues("FTC", "scan save: write error after %u row(s) -- stopping (disk full?)", _scanSaveN);
    if (_scanSaveBe && _scanSaveBe->sink.close) _scanSaveBe->sink.close();
    _scanSinkOpen = false;
    _scanSaveErr = true;
}

/** @brief Append one bounded CSV row via the sink. probed = true -> fill the openknx/info columns from the reused _dev* identity. */
void FileTransferClient::ftcScanWriteRow(const FtcEntry &e, bool probed)
{
    if (!_scanSinkOpen) return;
    char line[240];
    size_t p = 0;
    const uint16_t mask = (uint16_t)e.crc;
    ftcCat(line, sizeof(line), p, "%s,0x%04X,\"%s\"", e.name, mask, ftcMaskName(mask)); // class quoted (may contain a space)
    if (_scanOpenKnx)
    {
        // yes/no when a System B device was probed and answered; empty for non-System-B / no-answer.
        const char *ok = (probed && _scanProbeAnswered) ? (e.isOpenKnx ? "yes" : "no") : "";
        ftcCat(line, sizeof(line), p, ",%s", ok);
    }
    if (_scanInfo)
    {
        const bool ans = probed && _scanProbeAnswered; // each field empty unless the device answered it
        if (ans && _devHasSerial) ftcCat(line, sizeof(line), p, ",0x%04X", _devMfr);
        else
            ftcCat(line, sizeof(line), p, ",");
        if (ans && _devHasOrder)
        {
            char t[11] = {0}; // order number = the device's ASCII name field
            for (uint8_t i = 0; i < 10; i++)
                t[i] = (_devOrder[i] >= 0x20 && _devOrder[i] < 0x7F && _devOrder[i] != '"') ? (char)_devOrder[i] : '.'; // filter '"' too: it sits inside a quoted CSV field
            ftcCat(line, sizeof(line), p, ",\"%s\"", t);
        }
        else
            ftcCat(line, sizeof(line), p, ",");
        if (ans && _devHasHw) ftcCat(line, sizeof(line), p, ",0x%04X", (uint16_t)((_devHw[2] << 8) | _devHw[3]));
        else
            ftcCat(line, sizeof(line), p, ",");
        if (ans && _devHasVer) ftcCat(line, sizeof(line), p, ",%u.%u.%u", _devVerMaj, _devVerMin, _devVerRev);
        else
            ftcCat(line, sizeof(line), p, ",");
        if (ans && _devHasSerial)
            ftcCat(line, sizeof(line), p, ",%02X%02X%02X%02X%02X%02X", _devSerial[0], _devSerial[1], _devSerial[2],
                   _devSerial[3], _devSerial[4], _devSerial[5]);
        else
            ftcCat(line, sizeof(line), p, ",");
    }
    ftcCat(line, sizeof(line), p, "\n");
    if (_scanSaveBe->sink.write((const uint8_t *)line, (uint16_t)p) != (int)p)
    {
        ftcScanSaveFail();
        return;
    }
    _scanSaveN++;
}

/** @brief Launch ONE device probe: LIGHT = a single connectionless PID-11 read, FULL = the reused device-info chain. false = unparseable PA (skip). */
bool FileTransferClient::ftcScanProbeStart(const FtcEntry &e)
{
    unsigned a = 0, l = 0, d = 0;
    if (sscanf(e.name, "%u.%u.%u", &a, &l, &d) != 3 || a > 15 || l > 15 || d > 255) return false;
    const uint16_t pa = (uint16_t)((a << 12) | (l << 8) | d);
    // Reset the single reused identity set so a device that does not answer leaves empty columns.
    _devMfr = 0;
    _devVerMaj = _devVerMin = _devVerRev = 0;
    _devHasSerial = _devHasOrder = _devHasHw = _devHasVer = false;
    memset(_devSerial, 0, sizeof(_devSerial));
    memset(_devOrder, 0, sizeof(_devOrder));
    memset(_devHw, 0, sizeof(_devHw));
    _scanProbeAnswered = false;
    _scanProbeDone = false;
    _scanOkNum++;
    _ftcTarget = pa;
    _propPending = false;
    _ftcRespPending = false;
    if (_scanInfo) // FULL: reuse the device-info chain; it short-circuits back to FtcScanPost after the 5 PIDs
    {
        _scanProbeInFlight = 2;
        ftcDevInfoBegin(pa, true);
    }
    else // LIGHT: one connectionless PropertyValue_Read of the Device Object (idx 0, PID 11, 1 element, start 1)
    {
        _scanProbeInFlight = 1;
        SecurityControl sec = {false, None};
        if (!knx.bau().ftcSendPropertyValueRead(pa, sec, 0, FTC_PID_SERIAL, 1, 1))
            _scanProbeDone = true; // send failed -> advance immediately (like the FULL path bails), don't wait out FTC_TIMEOUT
        _ftcSince = millis();
    }
    return true;
}

/** @brief An in-flight probe finished: flag OpenKNX (mfr 0x00FA), log the per-device line (green when confirmed), stream its CSV row. */
void FileTransferClient::ftcScanPostConsume()
{
    FtcEntry &e = _ftcListing[_scanOkIdx];
    const bool isOk = _scanProbeAnswered && (_devMfr == FTC_MFR_OPENKNX);
    e.isOpenKnx = isOk;
    if (isOk) openknx.logger.color(32); // green (ANSI SGR) for a confirmed OpenKNX device
    if (_scanInfo)
    {
        if (_scanProbeAnswered)
        {
            char nm[11] = {0};
            if (_devHasOrder)
                for (uint8_t i = 0; i < 10; i++)
                    nm[i] = (_devOrder[i] >= 0x20 && _devOrder[i] < 0x7F) ? (char)_devOrder[i] : '.';
            openknx.logger.logWithPrefixAndValues("FTC", "  info probe %u/%u: %s  mfr 0x%04X  \"%s\"  app 0x%04X  v%u.%u.%u%s",
                                                  _scanOkNum, _scanOkTotal, e.name, _devMfr, nm,
                                                  (uint16_t)((_devHw[2] << 8) | _devHw[3]), _devVerMaj, _devVerMin, _devVerRev,
                                                  isOk ? "  [OpenKNX]" : "");
        }
        else
            openknx.logger.logWithPrefixAndValues("FTC", "  info probe %u/%u: %s  no answer", _scanOkNum, _scanOkTotal, e.name);
    }
    else
    {
        if (_scanProbeAnswered)
            openknx.logger.logWithPrefixAndValues("FTC", "  openknx probe %u/%u: %s  mfr 0x%04X%s", _scanOkNum, _scanOkTotal,
                                                  e.name, _devMfr, isOk ? "  [OpenKNX]" : "");
        else
            openknx.logger.logWithPrefixAndValues("FTC", "  openknx probe %u/%u: %s  no answer", _scanOkNum, _scanOkTotal, e.name);
    }
    if (isOk) openknx.logger.color(0);
    ftcScanWriteRow(e, true); // stream this device's row now (one bounded row -> naturally spread out)
}

/** @brief FULL probe: any device-info exit (normal after the 5 PIDs, or an early no-answer/send-fail) hands back to FtcScanPost. answered = did we get the identity (mfr). Never aborts the scan. */
void FileTransferClient::ftcScanInfoBail()
{
    _scanInfoActive = false;
    _scanProbeAnswered = _devHasSerial; // identity (mfr) came from PID_SERIAL
    _scanProbeDone = true;
    _ftcState = FtcScanPost;
}

void FileTransferClient::ftcStart(uint16_t pa, bool upload, uint8_t mode)
{
    knx.bau().ftcSetResponseCallback(ftcOnResponse);
    _ledBlinkPa = 0; // starting any transfer/ping stops a running locate-blink

    // Connectionless (plain T_Data_Individual), like the reference client -- the stack's
    // connection-oriented client path was never finished and stalls after chunk 1.
    _ftcTarget = pa;
    _ftcUpload = upload;
    _ftcMode = mode; // 0 = classic (ping is always classic); 1/2 negotiate FAST before the open
    _ftcSequence = 0;
    _ftcDone = 0;
    _ftcNextPct = 10;
    _ftcLastProgMs = 0;   // Variant-D progress: interval baseline seeded on the first shown line (= startMs)
    _ftcLastProgDone = 0; // do NOT reset _ftcVerbose/_ftcNoResume here -- they outlive a single ftcStart
    _ftcPeakBps = 0;      // per-attempt peak
    _ftcRespPending = false;
    _ftcRetries = 0;
    _ftcIsPerf = false;        // requestPerf() re-arms this AFTER ftcStart()
    _ftcRecovering = false;    // fast: not in forget gap-recovery
    _ftcClassicRescue = false; // fast: not in the classic full-resend rescue
    _ftcFoldWatermark = 0;     // fast: nothing folded into the CRC yet (ftcProceedToUpload re-seeds on resume)
    _ftcStartMs = 0;           // clock starts at the (fast/classic) open; guards the fast-open one-shot
    _ftcSince = millis();

    if (!upload) // ping keeps its one-line intro; upload/perf print the framed CONFIG box (built by request*)
        openknx.logger.logWithPrefixAndValues("FTC", "-> %u.%u.%u (0x%04X), connectionless",
                                              (pa >> 12) & 0x0F, (pa >> 8) & 0x0F, pa & 0xFF, pa);
    if (upload)
    {
        ftcStatusReset(FtcPhase::Upload, pa, _ftcPath);
        _status.total = _ftcSize;
        _status.chunks = _ftcChunks;
        // fast/forget: negotiate the server's FAST capability first (short probe or cached result). If a
        // probe is in flight, FtcFeatureProbe gates it then joins the classic path; classic skips this.
        if (_ftcMode != 0 && ftcBeginFeatureProbe())
            return;
        // Always FileInfo first (FtcResumeInfo): pick truncate / resume / skip. Opening blindly with
        // truncate would discard a resumable partial (auto-resume is the default; `no-resume` forces fresh).
        ftcSendInfo();
        _ftcState = FtcResumeInfo;
    }
    else
    {
        ftcStatusReset(FtcPhase::Ping, pa, "");
        _ftcTxLen = 0;
        if (ftcSend(FTC_CMD_MODULE_VERSION, 0))
            _ftcState = FtcSent;
        else
            openknx.logger.logWithPrefix("FTC", "send failed");
    }
}

bool FileTransferClient::ftcSend(uint8_t propertyId, uint8_t length)
{

    SecurityControl sec = {false, None};
    _ftcRespPending = false; // arm before sending, so a fast answer cannot be missed
    _ftcSince = millis();
    return knx.bau().ftcSendCommand(_ftcTarget, sec, FTC_OBJECT_INDEX, propertyId, _ftcTx, length);
}

/** @brief Current depth of our TP transmit FIFO (via the bau) -- the fast pump's flow-control gate (FTC_TX_HIGH/LOW). */
uint16_t FileTransferClient::ftcTxQueueSize()
{
    return knx.bau().ftcTxQueueSize();
}

const char *FileTransferClient::ftcModeName(uint8_t mode)
{
    switch (mode)
    {
        case 1: return "fast";
        case 2: return "forget";
        default: return "safe";
    }
}

/**
 * @brief fast/forget pre-flight: probe whether the target speaks the fast protocol before committing.
 *
 * Returns true if a CheckFeatures(102) probe is in flight (resolved in FtcFeatureProbe), false if it
 * resolved synchronously (cache hit / send failed) and the caller should run the classic open path.
 */
bool FileTransferClient::ftcBeginFeatureProbe()
{
    // Cached from an earlier probe of this same PA this session -> gate immediately, no bus traffic.
    if (_ftcFeatValid && _ftcFeatPa == _ftcTarget)
    {
        ftcGateFast(_ftcFeatBits, true);
        return false;
    }
    _ftcTxLen = 0;
    if (ftcSend(FTC_CMD_CHECK_FEATURES, 0)) // ftcSend arms _ftcRespPending + stamps _ftcSince
    {
        openknx.logger.logWithPrefixAndValues("FTC", "%s mode: probing target for FAST support...",
                                              ftcModeName(_ftcMode));
        _ftcState = FtcFeatureProbe;
        return true;
    }
    // Could not even send the probe -> treat as no-FAST and run classic.
    ftcGateFast(0, false);
    return false;
}

/**
 * @brief Turn the probed feature byte into the effective mode and log it (a silent downgrade must be visible).
 *
 * Any miss (no answer, FAST bit clear, too many chunks) drops _ftcMode to 0 (classic); a clean
 * negotiation keeps the mode for the fast data path.
 */
void FileTransferClient::ftcGateFast(uint8_t features, bool answered)
{
    const uint8_t requested = _ftcMode;
    const char *reason = nullptr;
    if (!answered)
        reason = "no CheckFeatures answer (old server)";
    else if (!(features & FTC_FEAT_FAST))
        reason = "server lacks FAST (0x04)";
    else if (_ftcChunks > FTC_FAST_MAX_CHUNKS)
        reason = "file exceeds fast chunk cap";

    if (reason != nullptr)
    {
        _ftcMode = 0; // downgrade -> classic
        openknx.logger.logWithPrefixAndValues("FTC", "%s -> classic: %s", ftcModeName(requested), reason);
    }
    else
    {
        // Keep _ftcMode (1/2): the fast data path is live. Windowed streams block-synchronous windows
        // with AIMD + per-window gap reports; forget streams the whole file then verifies + recovers.
        openknx.logger.logWithPrefixAndValues("FTC", "%s negotiated (server FAST ok) -- using fast data path",
                                              ftcModeName(requested));
    }
}

/** @brief Map the target's result byte to a human cause (values from the server, FileTransferModule.cpp). */
const char *FileTransferClient::ftcResultName(uint8_t result)
{
    switch (result)
    {
        case 0x42: return "target: bad request (unknown open flag?)";
        case 0x43: return "target: file not open";
        case 0x46: return "target: seek failed";
        case 0x47: return "target: short write -- filesystem full?";
#ifdef OPENKNX_FTC_SECURITY
        case 0xA0: return "target: auth required -- run: ftc <pa> login <pw>";
        case 0xA1: return "target: auth failed -- wrong password?";
        case 0xA2: return "target: writes disabled (stage Off / not in prog mode)";
#endif
        default: return "target rejected a chunk";
    }
}

void FileTransferClient::ftcSendUploadOpen()
{
    const size_t pathLen = strlen(_ftcPath); // bounded by sizeof(_ftcPath) == 64, checked on parse

    _ftcTx[0] = 0x00; // sequence 0 marks "open", it is not a data chunk
    _ftcTx[1] = 0x00;
    _ftcTx[2] = _ftcPayloadSize;               // becomes the target's _size; it derives its seek offsets from this
                                               // value, so it MUST equal the payload size we actually send below
    _ftcTx[3] = _ftcResume ? 0x01 : 0x00;      // 0 = truncate ("w"), 1 = resume ("r+"). Mandatory: the
                                               // target rejects anything > 1 with 0x42 and reads the filename from data+4.
    memcpy(_ftcTx + 4, _ftcPath, pathLen + 1); // NUL terminated -- target does LittleFS.open((char*)data+4)
    _ftcTxLen = (uint8_t)(4 + pathLen + 1);

    if (!ftcSend(FTC_CMD_FILE_UPLOAD, _ftcTxLen))
    {
        ftcAbort("send of FileUpload/open failed");
        return;
    }
    _ftcState = FtcUploadOpen;
    _ftcStartMs = millis();    // clock starts here: from now on the bus is carrying our payload
    if (_ftcGrandStartMs == 0) // mirror the fast open: end-to-end clock + byte base, set ONCE (survives retries)
    {
        _ftcGrandStartMs = _ftcStartMs;
        _ftcGrandResumeBase = _ftcResumeBase;
    }
    _ftcRateMarkMs = _ftcStartMs; // interval-rate baseline for this (classic) attempt
    _ftcRateMarkDone = _ftcDone;
    _ftcLastProgressMs = _ftcStartMs; // retry dead-window base until the first chunk advances it
    // The size/chunks/payload detail now lives in the CONFIG box; the "open" step line is emitted by
    // ftcProceedToUpload (this path is also the classic-rescue re-open, which stays silent).
}

uint8_t FileTransferClient::ftcReadSource(uint32_t offset, uint8_t *buffer, uint8_t maxLength)
{
    const uint32_t remaining = (offset >= _ftcSize) ? 0 : (_ftcSize - offset);
    const uint8_t want = (remaining < maxLength) ? (uint8_t)remaining : maxLength;
    if (want == 0) return 0;

    if (_ftcTestSource)
    {
        // Ramp: the byte at file offset x is x & 0xFF, so a dropped or shifted chunk is obvious in
        // a hexdump on the target instead of hiding in random data.
        for (uint8_t i = 0; i < want; i++)
            buffer[i] = (uint8_t)((offset + i) & 0xFF);
        return want;
    }

    if (_activeSrc == nullptr || _activeSrc->read == nullptr) return 0;
    return _activeSrc->read(offset, buffer, want);
}

/** @brief Arm the cooperative prefix CRC-32 (FtcCrcPrefix). CRC `target` source bytes a chunk per loop()
 *  pass; snapshot the running CRC at `snapAt` (resume chunk boundary, else 0xFFFFFFFF) as the fold seed. */
void FileTransferClient::ftcStartCrc(uint32_t target, uint32_t snapAt, uint32_t targetCrc, uint8_t next)
{
    _crcTarget = target;
    _crcOff = 0;
    _ftcSrcCrc = 0;
    _ftcResumeSeedCrc = 0; // resumeBase 0 / no snapshot -> seed is a fresh CRC
    _crcSnapAt = (snapAt > 0 && snapAt <= target) ? snapAt : 0xFFFFFFFFu;
    _ftcTargetCrc = targetCrc;
    _crcNext = next;
    _ftcState = FtcCrcPrefix;
}

/** @brief FtcCrcPrefix done (next 0): the resume / up-to-date decision, moved out of FtcResumeInfo so the
 *  478 KB-scale CRC never blocks the loop. Compares the folded CRC against the target's reported CRC. */
void FileTransferClient::ftcResumeCrcDone()
{
    const uint32_t crc = _ftcSrcCrc ^ 0xFFFFFFFFu;
    if (_ftcTargetHave == _ftcSize) // have == size -> is the whole file already there?
    {
        if (crc == _ftcTargetCrc)
        {
            _status.ok = true;
            _status.crc = _ftcTargetCrc;
            _status.done = _status.total;
            ftcStatusMsg("already up to date");
            openknx.logger.logWithPrefix("FTC", "already up to date -- nothing to send");
            ftcFinish();
            return;
        }
        openknx.logger.logWithPrefix("FTC", "same size but different content -- overwriting");
        _ftcSrcCrc = 0; // fresh defaults (resume=false, startSeq=1, base=0, done=0) still hold from FtcResumeInfo
        ftcProceedToUpload();
        return;
    }
    // 0 < have < size: resume ONLY if the prefix CRC matches -- else a DIFFERENT file's partial would
    // transfer to completion and fail only at the final verify. _ftcResume was armed true before the CRC,
    // so on a mismatch we MUST reset to a fresh full upload.
    if (crc != _ftcTargetCrc)
    {
        openknx.logger.logWithPrefixAndValues("FTC", "target's %u B partial does not match -- restarting", (unsigned)_ftcTargetHave);
        _ftcResume = false;
        _ftcStartSeq = 1;
        _ftcResumeBase = 0;
        _ftcDone = 0;
        _ftcSrcCrc = 0;
        ftcProceedToUpload();
        return;
    }
    _ftcSrcCrc = _ftcResumeSeedCrc; // matches -> continue the running fold from the chunk-boundary snapshot
    ftcStatusMsg("resuming");
    _status.done = _ftcResumeBase;
    openknx.logger.logWithPrefixAndValues("FTC", "matching %u B partial -> resuming at chunk %u/%u",
                                          (unsigned)_ftcDone, _ftcStartSeq, _ftcChunks);
    ftcProceedToUpload();
}

/** @brief FtcCrcPrefix done (next 1): the perf ramp CRC finished cooperatively -> name the file (keep),
 *  print the config box, start the transfer. (Was a synchronous ftcPerfCrc() that blocked on large sizes.) */
void FileTransferClient::ftcPerfCrcDone()
{
    const uint32_t crc = _ftcSrcCrc ^ 0xFFFFFFFFu;
    if (_crcPerfKeep)
        snprintf(_ftcPath, sizeof(_ftcPath), "/ftcperf_%08X.bin", (unsigned)crc);
    else
    {
        strncpy(_ftcPath, "/ftcperf.bin", sizeof(_ftcPath) - 1);
        _ftcPath[sizeof(_ftcPath) - 1] = '\0';
    }
    char fr[56], opt[24];
    snprintf(fr, sizeof(fr), "pkg %u   -   %u B/chunk   -   %u chunks", _crcPerfPkg, (unsigned)_ftcPayloadSize, (unsigned)_ftcChunks);
    snprintf(opt, sizeof(opt), "keep = %s", _crcPerfKeep ? "yes" : "no");
    ftcConfigBox(_crcPerfPa, "Speed test", "generated test pattern", _ftcPath, _ftcSize, true, crc,
                 _crcPerfMode == 0 ? "safe (classic)" : ftcModeName(_crcPerfMode), fr, opt);
    _ftcSrcCrc = 0; // fresh fold for the actual transfer
    ftcStart(_crcPerfPa, true, _crcPerfMode);
    _ftcIsPerf = true; // AFTER ftcStart (which clears it) -> verify handles /ftcperf*.bin at the end
    _ftcPerfKeep = _crcPerfKeep;
}

void FileTransferClient::ftcSendNextChunk()
{
    const uint32_t offset = (uint32_t)(_ftcSequence - 1) * _ftcPayloadSize;
    const uint8_t n = ftcReadSource(offset, _ftcTx + 3, _ftcPayloadSize);
    if (n == 0)
    {
        ftcSendClose();
        return;
    }

    // Sequence is LITTLE endian in the request (target reads data[1]<<8 | data[0]) but BIG endian in the
    // response (pushWord). The asymmetry is real, not a typo -- getting it wrong looks like a seq mismatch.
    _ftcTx[0] = (uint8_t)(_ftcSequence & 0xFF);
    _ftcTx[1] = (uint8_t)(_ftcSequence >> 8);
    _ftcTx[2] = n;
    _ftcTxLen = (uint8_t)(3 + n);
    // Fold the payload into the running source CRC on its FIRST send only -- a retry must not count the
    // same bytes twice. Never reset here: _ftcSrcCrc is seeded at transfer start (and over a resumed prefix).
    if (_ftcRetries == 0)
        _ftcSrcCrc = ftcCrc32Posix(_ftcSrcCrc, _ftcTx + 3, n, false);
    // The target CRCs the whole frame it received (seq + len + payload), so we must CRC the same span.
    _ftcTxCrc = ftcCrc16Modbus(_ftcTx, _ftcTxLen);

    if (!ftcSend(FTC_CMD_FILE_UPLOAD, _ftcTxLen))
    {
        // Connectionless: a false here means the stack could not queue the frame right now.
        // The frame stays prepared, so just push it again next loop.
        _ftcState = FtcUploadChunkRetry;
        return;
    }
    _ftcState = FtcUploadChunk;
}

void FileTransferClient::ftcSendClose()
{
    _ftcTx[0] = 0xFF; // 0xFFFF = "done": the target flushes and closes, and answers with an EMPTY
    _ftcTx[1] = 0xFF; // result (resultLength = 0), so the answer's arrival is the whole signal.
    _ftcTxLen = 2;

    if (!ftcSend(FTC_CMD_FILE_UPLOAD, _ftcTxLen))
    {
        ftcAbort("send of FileUpload/close failed");
        return;
    }
    if (_ftcData100Ms == 0) _ftcData100Ms = millis(); // all payload delivered (classic: every chunk was acked) -> pure-transfer clock stop
    _ftcState = FtcUploadClose;
}

// ================= FAST TRANSFER (phase 2 windowed + phase 3 forget) =============================
// Everything below runs only when FAST was negotiated (ftcGateFast kept _ftcMode 1/2); the classic path
// above is untouched. All non-blocking: bursts capped per loop(), FIFO-gated, waits are millis() deadlines.

/** @brief Open the transfer via the fast path (mode 1/2) or classic (mode 0), using the resume decision from FtcResumeInfo. */
void FileTransferClient::ftcProceedToUpload()
{
    // auto pkg: on a FRESH (non-resume) restart caused by link retries, drop to a smaller/robuster frame.
    // Safe ONLY here -- _ftcResume==false + _ftcStartSeq==1 means offsets start at seq 1, so changing the
    // payload cannot corrupt a resume (whose offsets = (seq-1)*_ftcPayloadSize). Deterministic and
    // idempotent, so the space-check re-entry re-runs it harmlessly.
    if (_ftcPkgAuto && !_ftcResume && _ftcStartSeq == 1 && _ftcTransferRetries > 0)
    {
        // Degrade linearly from the requested base down to FTC_PKG_MIN, reaching the floor on the LAST
        // retry -- so a link that only passes tiny frames (e.g. a foreign tunnel capping around 16 B) still
        // gets a shot at the minimum frame before the final abort. The step spans base..MIN across the
        // whole retry budget, so the floor is reached regardless of how many retries are configured.
        uint32_t want = FTC_PKG_MIN;
        if (_ftcPayloadBase > FTC_PKG_MIN)
        {
            const uint32_t budget = (_cfgTransferRetries > 0) ? _cfgTransferRetries : 1u;
            const uint32_t span   = (uint32_t)_ftcPayloadBase - FTC_PKG_MIN;
            const uint32_t step   = (span + budget - 1u) / budget; // ceil -> hits MIN by the last retry
            const uint32_t drop   = (uint32_t)_ftcTransferRetries * step;
            want = (_ftcPayloadBase > FTC_PKG_MIN + drop) ? (_ftcPayloadBase - drop) : (uint32_t)FTC_PKG_MIN;
        }
        uint32_t nch = (_ftcSize + want - 1) / want;
        // fast/forget mirror chunks in _ftcRecvBmp[1024] -> hard cap FTC_FAST_MAX_CHUNKS. A degrade crossing
        // it would make the server reject the fast open (0x4A, classified permanent); fall back to classic
        // (0xFFFE ceiling) so the auto-degrade still completes instead of dead-ending.
        if (_ftcMode != 0 && nch > FTC_FAST_MAX_CHUNKS)
        {
            _ftcMode = 0;
            openknx.logger.logWithPrefix("FTC", "auto pkg: chunk count over the fast window -> classic mode");
        }
        if (nch <= 0xFFFE && (uint16_t)want != _ftcPayloadSize) // don't degrade past the 16-bit seq limit
        {
            _ftcPayloadSize = (uint8_t)want;
            _ftcChunks = (uint16_t)nch;
            _status.chunks = _ftcChunks;
            openknx.logger.logWithPrefixAndValues("FTC", "auto pkg -> %u B/chunk (link retries, robuster frame)", (unsigned)_ftcPayloadSize);
        }
    }

    // Pre-upload free-space gate: refuse early if the file won't fit, rather than filling the FS and dying
    // mid-stream. Runs once after FtcResumeInfo; degrades gracefully against an old server (FtcFsInfo timeout).
    if (!_ftcSpaceChecked)
    {
        ftcSendFsInfo(1); // 1 = space check; FtcFsInfo re-enters here with _ftcSpaceChecked=true if it fits
        return;
    }

    // Last CONFIG-box step: the open. verify + space were queued earlier; this line + the divider close the box.
    ftcOut(0, "  open       %s", _ftcPath);
    ftcOut(CONSOLE_HEADLINE_COLOR, "--------------------------------------------------------------------------------");

    if (_ftcMode == 0)
    {
        ftcSendUploadOpen(); // classic path -- unchanged
        return;
    }

    // Initialise the fast stream state from the resume decision, then open. _ftcStartSeq is 1 fresh, or
    // (last-whole-chunk + 1) when resuming; the skipped prefix is already folded into _ftcSrcCrc.
    _ftcReportBase = _ftcStartSeq;
    _ftcNextSeq = _ftcStartSeq;
    _ftcFoldWatermark = (uint32_t)_ftcStartSeq - 1;        // prefix [1.._ftcStartSeq-1] already folded (or none)
    _ftcWnd = (_ftcMode == 2) ? _ftcChunks : FTC_WND_INIT; // forget = one giant window (no mid reports)
    _ftcPrevMissing = 0xFFFF;
    _ftcNoProgress = 0;
    _ftcReportRetries = 0;
    _ftcReportNonce = 0;
    _ftcRecovering = false;
    _ftcClassicRescue = false;
    memset(_ftcRecvBmp, 0, sizeof(_ftcRecvBmp));
    _ftcDeadline = millis() + FTC_FAST_STALL_MS; // progress-based: re-armed on every chunk that makes progress
    ftcFastOpenWindow();
    ftcSendFastOpen();
}

/**
 * @brief Freeze the current window's high edge (exclusive), EOF-clamped.
 *
 * Called only when a new window opens, never during a resend -- so halving _ftcWnd on loss (which sizes
 * the NEXT window) can never shrink the range already streamed and still being reported on.
 */
void FileTransferClient::ftcFastOpenWindow()
{
    uint32_t end = (uint32_t)_ftcReportBase + _ftcWnd;
    if (end > (uint32_t)_ftcChunks + 1u) end = (uint32_t)_ftcChunks + 1u;
    _ftcWndEnd = (uint16_t)end;
}

/**
 * @brief cmd44 OPEN: [00][00][payloadSize][flags][expectedChunks:2 LE][path NUL]. flags bit0 resume, bit1 keepBitmap.
 *
 * A recovery re-open forces both (0x03): never truncate + keep the server's bitmap so the report still
 * reflects everything the forget stream landed.
 */
void FileTransferClient::ftcSendFastOpen()
{
    const size_t pathLen = strlen(_ftcPath); // bounded by sizeof(_ftcPath) == 64, checked on parse
    uint8_t flags = _ftcResume ? 0x01 : 0x00;
    if (_ftcRecovering) flags = 0x03; // resume + keepBitmap

    _ftcTx[0] = 0x00; // open marker
    _ftcTx[1] = 0x00;
    _ftcTx[2] = _ftcPayloadSize; // becomes the target's _size (seek stride)
    _ftcTx[3] = flags;
    _ftcTx[4] = (uint8_t)(_ftcChunks & 0xFF); // expectedChunks, little-endian
    _ftcTx[5] = (uint8_t)(_ftcChunks >> 8);
    memcpy(_ftcTx + 6, _ftcPath, pathLen + 1); // NUL terminated
    _ftcTxLen = (uint8_t)(6 + pathLen + 1);

    if (!ftcSend(FTC_CMD_FILE_UPLOAD_FAST, _ftcTxLen))
    {
        ftcAbort("send of fast open failed");
        return;
    }
    _ftcState = FtcFastOpen;
    const uint32_t nowOpen = millis();
    if (_ftcStartMs == 0) _ftcStartMs = nowOpen; // per-attempt clock; recovery keeps it, a retry re-arms it (ftcStart zeroed it)
    if (_ftcGrandStartMs == 0)
    {
        _ftcGrandStartMs = _ftcStartMs;       // end-to-end clock; set ONCE, survives retries (reset only on a fresh request)
        _ftcGrandResumeBase = _ftcResumeBase; // bytes already on target at the very first attempt -> end-to-end byte base
    }
    _ftcRateMarkMs = nowOpen; // interval-rate baseline: measure from this (re-)open, not the original start
    _ftcRateMarkDone = _ftcDone;
    // truncate/resume/keepBitmap + chunks/payload now live in the CONFIG box; the "open" step line is
    // emitted once by ftcProceedToUpload (recovery re-opens through here stay silent).
}

/**
 * @brief SILENT cmd44 DATA frame: [seq:2 LE][n][payload:n][crc16:2 BE over the first 3+n bytes].
 *
 * Fast DATA MUST stay AckRequested (KNX-spec) so TP1 L2 keeps L_ACK/BUSY/retransmit -- the only
 * flow-control backstop. Does NOT arm _ftcRespPending: the server answers nothing to a DATA frame.
 */
void FileTransferClient::ftcSendFastData(uint16_t seq)
{
    const uint32_t offset = (uint32_t)(seq - 1) * _ftcPayloadSize;
    const uint8_t n = ftcReadSource(offset, _ftcTx + 3, _ftcPayloadSize);
    // Every in-range seq has payload, so n==0 is a source READ error, never EOF. Skip the send WITHOUT
    // advancing the watermark: the seq stays an unset gap the report flags (a 0-byte frame would let the
    // server mark it "received" -> an invisible gap).
    if (n == 0) return;
    _ftcTx[0] = (uint8_t)(seq & 0xFF); // seq little-endian (server reads data[1]<<8 | data[0])
    _ftcTx[1] = (uint8_t)(seq >> 8);
    _ftcTx[2] = n;
    const uint16_t crc = ftcCrc16Modbus(_ftcTx, (uint8_t)(3 + n)); // over seq+len+payload
    _ftcTx[3 + n] = (uint8_t)(crc >> 8);                           // big-endian (server reads data[3+n]<<8 | data[3+n+1])
    _ftcTx[3 + n + 1] = (uint8_t)(crc & 0xFF);
    const uint8_t len = (uint8_t)(3 + n + 2);

    // Fold the payload into the source CRC32 EXACTLY ONCE per seq, via the ascending watermark, so resends
    // (seqs at or below the watermark) don't double-count -- _ftcSrcCrc and _ftcDone stay correct.
    if (seq > _ftcFoldWatermark)
    {
        _ftcSrcCrc = ftcCrc32Posix(_ftcSrcCrc, _ftcTx + 3, n, false);
        _ftcFoldWatermark = seq;
        _ftcDone += n;
        _ftcLastProgressMs = millis();                         // last forward progress -> retry dead-window accounting
        _ftcDeadline = _ftcLastProgressMs + FTC_FAST_STALL_MS; // forward progress -> re-arm the no-progress guard
        _status.done = _ftcDone;
        _status.chunk = seq;
        ftcMaybeProgress(true, seq, _ftcChunks, _ftcDone, _ftcSize, _ftcStartMs); // Variant-D two-line progress
    }

    SecurityControl sec = {false, None};
    knx.bau().ftcSendCommand(_ftcTarget, sec, FTC_OBJECT_INDEX, FTC_CMD_FILE_UPLOAD_FAST, _ftcTx, len);
}

/**
 * @brief cmd45 report query: [base:2 LE][count:2 LE][nonce:1] -> FtcFastReport.
 *
 * A fresh nonce every send (initial and retry) so only the matching answer is accepted.
 */
void FileTransferClient::ftcSendReport(uint16_t base, uint16_t count)
{
    if (_ftcReportRetries == 0) _ftcRepWaitStart = millis(); // [dbg] first query of this round -> start the answer-latency clock (a retry send keeps it)
    _ftcReportNonce++;
    _ftcTx[0] = (uint8_t)(base & 0xFF); // little-endian
    _ftcTx[1] = (uint8_t)(base >> 8);
    _ftcTx[2] = (uint8_t)(count & 0xFF);
    _ftcTx[3] = (uint8_t)(count >> 8);
    _ftcTx[4] = _ftcReportNonce;
    if (!ftcSend(FTC_CMD_FILE_REPORT, 5))
    {
        ftcAbort("send of fast report failed");
        return;
    }
    _ftcState = FtcFastReport;
}

// cmd44 CLOSE (FF FF): the target flushes + closes and answers a 1-byte 0x00. -> FtcFastClose.
void FileTransferClient::ftcSendFastClose()
{
    _ftcTx[0] = 0xFF;
    _ftcTx[1] = 0xFF;
    _ftcTxLen = 2;
    if (!ftcSend(FTC_CMD_FILE_UPLOAD_FAST, _ftcTxLen))
    {
        ftcAbort("send of fast close failed");
        return;
    }
    if (_ftcData100Ms == 0) _ftcData100Ms = millis(); // payload streamed + FIFO drained before close -> pure-transfer clock stop
    _ftcState = FtcFastClose;
}

/** @brief Log a throughput line: bytes THIS run sent over elapsedMs, measured from millis(), never estimated. */
void FileTransferClient::ftcLogRate(const char *what, uint32_t elapsedMs)
{
    if (elapsedMs == 0) elapsedMs = 1; // no division by zero on an absurdly fast/empty transfer
    // Only the bytes THIS run put on the bus -- charging resumed bytes against this run's clock
    // would invent throughput we never achieved.
    const uint32_t sent = (_ftcDone > _ftcResumeBase) ? (_ftcDone - _ftcResumeBase) : 0;
    const uint32_t bps = (uint32_t)(((uint64_t)sent * 1000ULL) / elapsedMs);
    _status.bps = (uint16_t)((bps > 0xFFFF) ? 0xFFFF : bps);
    openknx.logger.logWithPrefixAndValues("FTC", "%s: %u B in %u.%03u s -> %u B/s", what,
                                          (unsigned)sent, (unsigned)(elapsedMs / 1000),
                                          (unsigned)(elapsedMs % 1000), (unsigned)bps);
    if (_ftcResumeBase)
        openknx.logger.logWithPrefixAndValues("FTC", "  (resumed: %u B were already on the target, file is %u B)",
                                              (unsigned)_ftcResumeBase, (unsigned)_ftcDone);
    if (_ftcDupes)
        openknx.logger.logWithPrefixAndValues("FTC", "  %u duplicate answers discarded (they arrive over the IP side, not the bus)",
                                              (unsigned)_ftcDupes);
}

/**
 * @brief Compute cur/avg/peak on EVERY call (keeps ftc status .bps live) and gate a progress line.
 *
 * Shared by the fast/classic upload and the download emit sites. `up` picks the arrow + resume base and
 * seeds the interval baseline (_ftcLastProgMs = startMs) on the first call. Emits the initial line, then
 * one line per FTC_VERBOSE_MS when verbose (1 Hz) or per FTC_PROGRESS_MS otherwise (10 s) -- not per decile.
 */
void FileTransferClient::ftcMaybeProgress(bool up, uint16_t seq, uint16_t chunks, uint32_t done, uint32_t size, uint32_t startMs)
{
    const uint32_t now = millis();
    const bool first = (_ftcLastProgMs == 0);            // captured BEFORE seeding -> first call gets the initial line
    if (first) _ftcLastProgMs = startMs ? startMs : now; // interval baseline = the (re-)open

    // Cheap per-chunk arithmetic on EVERY call so _status.bps (ftc status) always reflects the live rate.
    const uint32_t el = now - startMs;
    const uint32_t base = up ? _ftcResumeBase : 0; // resumed bytes are not this run's throughput
    const uint32_t net = (done > base) ? (done - base) : 0;
    const uint32_t avg = (uint32_t)(((uint64_t)net * 1000ULL) / (el ? el : 1));
    // cur = interval rate since the last SHOWN line; the first line has no interval -> fall back to avg.
    uint32_t cur = avg;
    if (_ftcLastProgDone != 0 && done > _ftcLastProgDone && now > _ftcLastProgMs)
        cur = (uint32_t)(((uint64_t)(done - _ftcLastProgDone) * 1000ULL) / (now - _ftcLastProgMs));
    if (cur > _ftcPeakBps) _ftcPeakBps = cur;
    _status.bps = (uint16_t)(cur > 0xFFFF ? 0xFFFF : cur);

    // Emit gate: initial line always; then 1 Hz when verbose, else one compact line every FTC_PROGRESS_MS.
    const uint32_t due = _ftcVerbose ? FTC_VERBOSE_MS : FTC_PROGRESS_MS;
    if (!first && (now - _ftcLastProgMs) < due) return;

    ftcProgress(up, _ftcTarget, seq, chunks, done, size, cur, avg, _ftcVerbose);
    _ftcLastProgMs = now; // last-SHOWN baseline (pairs with _ftcLastProgDone for the next interval rate)
    _ftcLastProgDone = done;
}

/**
 * @brief Emit a progress line: verbose = two aligned lines (owning the "FTC:" column), else one compact line.
 *
 * Verbose keeps the historical layout unchanged (arrow + bar/pct/seq, then KB/Bps/avg/ETA under the bar,
 * with a trailing blank line). Non-verbose prints ONE dense line, no trailing blank -- the quiet default:
 * "FTC: Upload/Download -> a.l.d  [bar]  pct%  seq s/c  di.dd/ti.td KB  cur B/s (avg a)  ETA Mm SSs".
 */
void FileTransferClient::ftcProgress(bool up, uint16_t pa, uint16_t seq, uint16_t chunks, uint32_t done, uint32_t size, uint32_t cur, uint32_t avg, bool verbose)
{
    const uint8_t pct = size ? (uint8_t)(((uint64_t)done * 100ULL) / size) : 0;
    // Bar: FTC_PROG_BAR_W cells, '#' up to done/size, '-' track for the rest.
    uint32_t fill = size ? (uint32_t)(((uint64_t)done * FTC_PROG_BAR_W) / size) : 0;
    if (fill > FTC_PROG_BAR_W) fill = FTC_PROG_BAR_W;
    char bar[FTC_PROG_BAR_W + 3];
    bar[0] = '[';
    memset(bar + 1, '#', fill);
    memset(bar + 1 + fill, '-', FTC_PROG_BAR_W - fill);
    bar[1 + FTC_PROG_BAR_W] = ']';
    bar[2 + FTC_PROG_BAR_W] = 0;

    // KB with 1 decimal, no float (int = bytes>>10, dec = ((bytes&1023)*10)>>10); ETA = remaining / max(avg,1) s.
    const uint32_t di = done >> 10, dd = ((done & 1023) * 10) >> 10;
    const uint32_t ti = size >> 10, td = ((size & 1023) * 10) >> 10;
    const uint32_t rem = (size > done) ? (size - done) : 0;
    const uint32_t eta = rem / (avg ? avg : 1);

    if (!verbose)
    {
        // One dense line, no trailing blank. Direction is the WORD; the arrow stays "->" for both ways.
        char line[160];
        snprintf(line, sizeof(line),
                 "FTC: %s -> %u.%u.%u  %s  %u%%  seq %u/%u  %u.%u/%u.%u KB  %u B/s (avg %u)  ETA %um%02us",
                 up ? "Upload" : "Download", (pa >> 12) & 0x0F, (pa >> 8) & 0x0F, pa & 0xFF, bar,
                 (unsigned)pct, (unsigned)seq, (unsigned)chunks, (unsigned)di, (unsigned)dd,
                 (unsigned)ti, (unsigned)td, (unsigned)cur, (unsigned)avg,
                 (unsigned)(eta / 60), (unsigned)(eta % 60));
        ftcOut(0, "%s", line);
        return;
    }

    // Verbose (unchanged). Line 1: build the prefix "FTC: -> a.l.d  " first, capture its width (pfx) for line 2.
    char l1[112];
    int pfx = snprintf(l1, sizeof(l1), "FTC: %s %u.%u.%u  ", up ? "->" : "<-",
                       (pa >> 12) & 0x0F, (pa >> 8) & 0x0F, pa & 0xFF);
    if (pfx < 0) return;
    if (pfx > (int)sizeof(l1) - 1) pfx = (int)sizeof(l1) - 1;
    snprintf(l1 + pfx, sizeof(l1) - (size_t)pfx, "%s  %3u%%  seq %u/%u", bar,
             (unsigned)pct, (unsigned)seq, (unsigned)chunks);

    // Line 2: "FTC:" + (pfx-4) spaces lands the content under line1's bar.
    int ind = pfx - 4;
    if (ind < 0) ind = 0;
    char l2[128];
    snprintf(l2, sizeof(l2), "FTC:%*s%u.%u/%u.%u KB  -  %3u B/s  -  avg %3u  -  ETA %02um%02us",
             ind, "", (unsigned)di, (unsigned)dd, (unsigned)ti, (unsigned)td,
             (unsigned)cur, (unsigned)avg, (unsigned)(eta / 60), (unsigned)(eta % 60));

    ftcOut(0, "%s", l1);
    ftcOut(0, "%s\n", l2);
}

/** @brief Result-panel divider: "+<ch repeated FTC_BOX_IW>+" via ftcOut (headline color). */
void FileTransferClient::ftcBoxRule(char ch)
{
    char rule[FTC_BOX_IW + 3];
    rule[0] = '+';
    memset(rule + 1, ch, FTC_BOX_IW);
    rule[FTC_BOX_IW + 1] = '+';
    rule[FTC_BOX_IW + 2] = 0;
    ftcOut(CONSOLE_HEADLINE_COLOR, "%s", rule);
}

/** @brief Result-panel row: pads AND truncates the content to FTC_BOX_IW so a long string never breaks the frame. */
void FileTransferClient::ftcBoxRow(const char *s)
{
    ftcOut(0, "|%-*.*s|", FTC_BOX_IW, FTC_BOX_IW, s);
}

/**
 * @brief Print the one-shot upload/perf result block at the end of a transfer.
 *
 * A framed, prefix-less panel (ftcBoxRule/ftcBoxRow) so it reads as a report, not log noise. Figures are
 * end-to-end (retries deduped via resume) plus a pure data-transfer rate; every row is bounded to <= 54.
 */
void FileTransferClient::ftcPrintSummary()
{
    // End-to-end bytes: what the whole op delivered across all attempts (resume-deduped).
    const uint32_t eeSent = (_ftcDone > _ftcGrandResumeBase) ? (_ftcDone - _ftcGrandResumeBase) : 0;
    // Pure transfer time: start -> close sent. The close goes out only after the TX FIFO drains, so this
    // marks the last payload byte on the wire; excludes the close-ack + CRC verify (finalisation).
    const uint32_t wallMs = _ftcGrandElapsedMs ? _ftcGrandElapsedMs : _ftcElapsedMs; // fallback: start -> close-ack
    uint32_t pureMs = (_ftcData100Ms > _ftcGrandStartMs) ? (_ftcData100Ms - _ftcGrandStartMs) : wallMs;
    if (pureMs == 0) pureMs = 1;
    // Transfer-only: pure minus the dead windows the retries cost.
    uint32_t netMs = (pureMs > _ftcRetryLostMs) ? (pureMs - _ftcRetryLostMs) : pureMs;
    if (netMs == 0) netMs = 1;
    const uint32_t pureBps = (uint32_t)(((uint64_t)eeSent * 1000ULL) / pureMs);
    const uint32_t netBps = (uint32_t)(((uint64_t)eeSent * 1000ULL) / netMs);

    char row[80];
    const char *mn = ftcModeName(_ftcMode); // actual mode used (a fast-probe miss downgrades to "safe")
    const unsigned a = (_ftcTarget >> 12) & 0x0F, li = (_ftcTarget >> 8) & 0x0F, dv = _ftcTarget & 0xFF;

    ftcBoxRule('=');
    if (_ftcIsPerf)
        snprintf(row, sizeof(row), "  SPEED TEST COMPLETE   -> %u.%u.%u   (perf, %s)", a, li, dv, mn);
    else
        snprintf(row, sizeof(row), "  UPLOAD COMPLETE   -> %u.%u.%u   (%s)", a, li, dv, mn);
    ftcBoxRow(row);
    ftcBoxRule('-');

    snprintf(row, sizeof(row), "  Bytes    %u   (%u chunks x %u B)",
             (unsigned)eeSent, (unsigned)_ftcChunks, (unsigned)_ftcPayloadSize);
    ftcBoxRow(row);

    const uint32_t avgBps = _ftcTransferRetries ? netBps : pureBps; // retries -> the transfer-only rate is honest
    const uint32_t secs = pureMs / 1000;                            // wall time shown = pure data-transfer time
    snprintf(row, sizeof(row), "  Time     %02um%02us      avg  %u B/s     peak %u",
             (unsigned)(secs / 60), (unsigned)(secs % 60), (unsigned)avgBps, (unsigned)_ftcPeakBps);
    ftcBoxRow(row);

    snprintf(row, sizeof(row), "  CRC32    0x%08X      verified %s",
             (unsigned)_status.crc, _status.ok ? "OK" : "MISMATCH");
    ftcBoxRow(row);

    // Conditional extras (each reworded to fit within FTC_BOX_IW), before the closing rule.
    if (_ftcTransferRetries)
    {
        const uint32_t ls = _ftcRetryLostMs / 1000;
        snprintf(row, sizeof(row), "  Retry    %u attempt%s   %02um%02us recovery",
                 (unsigned)_ftcTransferRetries, _ftcTransferRetries == 1 ? "" : "s",
                 (unsigned)(ls / 60), (unsigned)(ls % 60));
        ftcBoxRow(row);
    }
    if (_ftcGrandResumeBase)
    {
        snprintf(row, sizeof(row), "  Resumed  %u B were already on the target", (unsigned)_ftcGrandResumeBase);
        ftcBoxRow(row);
    }
    if (_ftcDupes)
    {
        snprintf(row, sizeof(row), "  Dupes    %u discarded (IP mirror, not the bus)", (unsigned)_ftcDupes);
        ftcBoxRow(row);
    }
    if (_ftcIsPerf)
    {
        if (_ftcPerfKeep)
            snprintf(row, sizeof(row), "  Cleanup  kept as %s", _ftcPath);
        else
            snprintf(row, sizeof(row), "  Cleanup  %s",
                     _ftcPerfRemoved ? "test file removed" : "test file left behind");
        ftcBoxRow(row);
    }
    ftcBoxRule('=');
}

/**
 * @brief One framed, prefix-less CONFIG box at the start of an upload/perf/download (queued via ftcOut).
 *
 * Replaces the old scattered start lines. A null mode/framing/options (and size 0) skips that row -- used
 * by download; hasCrc adds "- CRC32 0x.." to the Size row (perf, deterministic pattern). The verify/space/
 * open step lines + the closing divider are emitted by the state machine as they happen.
 */
void FileTransferClient::ftcConfigBox(uint16_t pa, const char *title, const char *source, const char *target,
                                      uint32_t size, bool hasCrc, uint32_t crc,
                                      const char *mode, const char *framing, const char *options)
{
    static const char RULE[] = "--------------------------------------------------------------------------------";
    ftcOut(CONSOLE_HEADLINE_COLOR, "%s", RULE);
    ftcOut(CONSOLE_HEADLINE_COLOR, " FileTransferClient - %s -> PA %u.%u.%u (0x%04X) - connectionless", title,
           (pa >> 12) & 0x0F, (pa >> 8) & 0x0F, pa & 0xFF, pa);
    ftcOut(CONSOLE_HEADLINE_COLOR, "%s", RULE);
    ftcOut(0, "  Source     %s", source);
    ftcOut(0, "  Target     %s", target);
    if (hasCrc)
        ftcOut(0, "  Size       %u B   -   CRC32 0x%08X", (unsigned)size, (unsigned)crc);
    else if (size)
        ftcOut(0, "  Size       %u B", (unsigned)size);
    if (mode) ftcOut(0, "  Mode       %s", mode);
    if (framing) ftcOut(0, "  Framing    %s", framing);
    if (options) ftcOut(0, "  Options    %s", options);
    ftcOut(CONSOLE_HEADLINE_COLOR, "%s", RULE);
}

void FileTransferClient::ftcAbort(const char *reason)
{
    // Transfer-level auto-retry: most UPLOAD aborts are transient (target busy after a format/reboot, a
    // lost report/close) and resume-recoverable -> re-run the whole transfer. Bounded (_cfgTransferRetries)
    // and transient-only; the permanent reasons below fail immediately.
    const bool permanent = strstr(reason, "source") ||          // source not available / unreadable
                           strstr(reason, "cannot read") ||     // source read failure (prefix / CRC skip)
                           strstr(reason, "refused") ||         // target rejected the open (protocol, not luck)
                           strstr(reason, "recovery failed") || // forget gap-recovery already exhausted
                           strstr(reason, "no progress") ||     // dead sequences -- a restart would just re-hit them
                           strstr(reason, "too many") ||        // file needs more chunks than the protocol allows
                           strstr(reason, "full") ||            // target filesystem full (0x47) -- retries can't make room
                           strstr(reason, "space") ||           // pre-upload space check failed -- a retry can't create room
                           strstr(reason, "cancel");            // user cancelled -- never silently restart
    if (_ftcUpload && !permanent && _ftcTransferRetries < _cfgTransferRetries)
    {
        _ftcTransferRetries++;
        openknx.logger.logWithPrefixAndValues("FTC", "transient (%s) -> transfer retry %u/%u (resume in %ums)",
                                              reason, _ftcTransferRetries, _cfgTransferRetries,
                                              (unsigned)_cfgBackoffMs);
        // Do NOT close the source here: the retry re-reads it (read() is offset-based/stateless, so a
        // still-open handle is safe). It IS closed on the final give-up below and on success (ftcFinish).
        SecurityControl secR = {false, None};
        knx.bau().ftcSendCommand(_ftcTarget, secR, FTC_OBJECT_INDEX, FTC_CMD_CANCEL, _ftcTx, 0); // close target's partial (kept on flash)
        _ftcRetryPending = true;
        _ftcState = FtcCancel;
        _ftcSince = millis();
        return;
    }

    // Real give-up (permanent reason or budget spent). Clear _ftcRetryPending FIRST: a user `cancel` fired
    // during a retry's backoff would otherwise leave it set and FtcCancel would restart the cancelled transfer.
    _ftcRetryPending = false;
    const uint32_t elapsed = millis() - _ftcStartMs;
    _status.phase = FtcPhase::Failed;
    ftcStatusMsg(reason);
    openknx.logger.logWithPrefixAndValues("FTC", "ABORT: %s (sequence %u, %u/%u bytes)", reason,
                                          _ftcSequence, (unsigned)_ftcDone, (unsigned)_ftcSize);
    if (_ftcUpload && _ftcStartMs != 0) ftcLogRate("aborted after", elapsed);
    ftcCloseSource();

    // Cancel(90) makes the target close its half-written file now (vs its heartbeat timeout). It answers
    // nothing (processFunctionProperty returns false for Cancel), so let the frame drain before finishing.
    SecurityControl sec = {false, None};
    knx.bau().ftcSendCommand(_ftcTarget, sec, FTC_OBJECT_INDEX, FTC_CMD_CANCEL, _ftcTx, 0);
    _ftcState = FtcCancel;
    _ftcSince = millis();
}

// Retry the chunk that is still prepared in _ftcTx, or give up after _cfgMaxRetries.
void FileTransferClient::ftcRetryOrAbort(const char *why)
{
    if (++_ftcRetries >= _cfgMaxRetries)
    {
        openknx.logger.logWithPrefixAndValues("FTC", "chunk %u failed %u times", _ftcSequence, _ftcRetries);
        ftcAbort(why);
        return;
    }
    openknx.logger.logWithPrefixAndValues("FTC", "retry %u/%u for chunk %u (%s)",
                                          _ftcRetries, _cfgMaxRetries - 1, _ftcSequence, why);
    _ftcState = FtcUploadChunkRetry;
    _ftcSince = millis();
}

void FileTransferClient::ftcSendInfo()
{
    const size_t n = strlen(_ftcPath) + 1; // payload is the filename, NUL included
    memcpy(_ftcTx, _ftcPath, n);
    ftcOut(0, "  verify     asking the target for size + CRC32 ..."); // CONFIG-box step (pre- and post-upload)
    if (ftcSend(FTC_CMD_FILE_INFO, (uint8_t)n))
        _ftcState = FtcVerify;
    else
        ftcAbort("could not send FileInfo for verification");
}

/**
 * @brief "ftc ll/ls": list a target directory. DirList is a stateful iterator -- one entry per round trip.
 *
 * Collect all entries first (closes the dir handle), then FileInfo each file for size + CRC32.
 */
void FileTransferClient::requestList(uint16_t pa, const char *dir, bool detailed)
{
    if (strlen(dir) >= sizeof(_ftcListDir))
    {
        openknx.logger.logWithPrefix("FTC", "path too long");
        return;
    }
    strncpy(_ftcListDir, dir, sizeof(_ftcListDir) - 1);
    _ftcListDir[sizeof(_ftcListDir) - 1] = 0;
    _ftcListDetailed = detailed;

    ftcStatusReset(FtcPhase::List, pa, _ftcListDir);
    knx.bau().ftcSetResponseCallback(ftcOnResponse);
    _ftcTarget = pa;
    _ftcListing.clear();
    _ftcDirIdx = 0;
    _ftcListBytes = 0;
    _ftcListFiles = 0;
    _ftcListDirs = 0;
    _ftcRespT = 0; // arm the duplicate guard fresh for this listing

    openknx.logger.logWithPrefixAndValues("FTC", "list -> %u.%u.%u \"%s\"",
                                          (pa >> 12) & 0x0F, (pa >> 8) & 0x0F, pa & 0xFF, _ftcListDir);
    ftcSendDirList();
}

void FileTransferClient::ftcSendDirList()
{
    const size_t n = strlen(_ftcListDir) + 1; // payload is the directory path, NUL included
    memcpy(_ftcTx, _ftcListDir, n);
    if (ftcSend(FTC_CMD_DIR_LIST, (uint8_t)n))
        _ftcState = FtcDirList;
    else
        ftcAbort("could not send DirList");
}

// Print the directories in the collected list (they need no FileInfo), then fire a FileInfo for the
// next file and wait. When nothing is left, print the footer and finish.
void FileTransferClient::ftcListAdvance()
{
    while (_ftcDirIdx < _ftcListing.size())
    {
        const char *nm = _ftcListing[_ftcDirIdx].name;
        if (_ftcListing[_ftcDirIdx].isDir)
        {
            char row[128];
            snprintf(row, sizeof(row), "%-40.40s | %12s | %-10s | dir", nm, "--", "--");
            ftcOut(0, "%s", row);
            _ftcListDirs++;
            _ftcDirIdx++;
            continue;
        }
        // A file -> build its full path and ask the target for size + CRC32.
        if (nm[0] == '/')
            snprintf(_ftcPath, sizeof(_ftcPath), "%s", nm);
        else
        {
            const size_t dl = strlen(_ftcListDir);
            const bool slash = (dl > 0 && _ftcListDir[dl - 1] == '/');
            snprintf(_ftcPath, sizeof(_ftcPath), "%s%s%s", _ftcListDir, slash ? "" : "/", nm);
        }
        const size_t n = strlen(_ftcPath) + 1;
        memcpy(_ftcTx, _ftcPath, n);
        if (!ftcSend(FTC_CMD_FILE_INFO, (uint8_t)n))
        {
            ftcAbort("could not send FileInfo during list");
            return;
        }
        _ftcState = FtcDirInfo;
        return; // wait for the answer
    }

    ftcOut(CONSOLE_HEADLINE_COLOR, "------------------------------------------------------------------------------");
    // Footer aligned to the columns: counts under Name, byte total under Size, "total" under Type.
    char left[41];
    snprintf(left, sizeof(left), "Files: %u   Dirs: %u", _ftcListFiles, _ftcListDirs);
    char foot[128];
    snprintf(foot, sizeof(foot), "%-40.40s | %12u | %-10s | total", left, (unsigned)_ftcListBytes, "");
    ftcOut(CONSOLE_HEADLINE_COLOR, "%s", foot);
    ftcOut(CONSOLE_HEADLINE_COLOR, "------------------------------------------------------------------------------");
    // _ftcListing stays valid through the operation; ftcFinish() releases it at the terminal (ll continues
    // into the FtcFsInfo bar first, so the free happens after that, not here).
    _status.ok = true;
    if (_ftcListDetailed)
    {
        ftcSendFsInfo(2); // ll: follow the file summary with a LittleFS usage bar (FtcFsInfo purpose 2)
        return;
    }
    ftcFinish(); // sets phase Done
}

/** @brief rm / mkdir / rmdir / mv: one round trip, one result byte; payload pre-assembled by the caller. */
void FileTransferClient::ftcSimpleCmd(uint16_t pa, uint8_t cmd, const char *verb, const uint8_t *payload,
                                      uint8_t len, const char *shownPath)
{
    ftcStatusReset(FtcPhase::Delete, pa, shownPath);
    _ftcVerb = verb;
    knx.bau().ftcSetResponseCallback(ftcOnResponse);
    _ftcTarget = pa;
    memcpy(_ftcTx, payload, len);
    openknx.logger.logWithPrefixAndValues("FTC", "%s -> %u.%u.%u \"%s\"", verb, (pa >> 12) & 0x0F,
                                          (pa >> 8) & 0x0F, pa & 0xFF, shownPath);
    if (ftcSend(cmd, len))
        _ftcState = FtcDelete;
    else
        openknx.logger.logWithPrefix("FTC", "send failed");
}

void FileTransferClient::requestDelete(uint16_t pa, const char *remotePath)
{
    if (strlen(remotePath) >= sizeof(_ftcPath))
    {
        openknx.logger.logWithPrefix("FTC", "path too long");
        return;
    }
    const size_t n = strlen(remotePath) + 1;
    ftcSimpleCmd(pa, FTC_CMD_FILE_DELETE, "rm", (const uint8_t *)remotePath, (uint8_t)n, remotePath);
}

/**
 * @brief `ftc <pa> format yes`: Format(0) -> LittleFS.format(), wipes every file + folder.
 *
 * Cannot brick the device: firmware lives in program flash, not LittleFS -- only staged files and
 * filesystem-backed app persistence are cleared.
 */
void FileTransferClient::requestFormat(uint16_t pa)
{
    uint8_t dummy = 0; // Format takes no payload; pass a valid pointer with len 0
    ftcSimpleCmd(pa, FTC_CMD_FORMAT, "format", &dummy, 0, "(all files + folders)");
}

void FileTransferClient::requestMkdir(uint16_t pa, const char *dir)
{
    if (strlen(dir) >= sizeof(_ftcPath))
    {
        openknx.logger.logWithPrefix("FTC", "path too long");
        return;
    }
    const size_t n = strlen(dir) + 1;
    ftcSimpleCmd(pa, FTC_CMD_DIR_CREATE, "mkdir", (const uint8_t *)dir, (uint8_t)n, dir);
}

void FileTransferClient::requestRmdir(uint16_t pa, const char *dir)
{
    if (strlen(dir) >= sizeof(_ftcPath))
    {
        openknx.logger.logWithPrefix("FTC", "path too long");
        return;
    }
    const size_t n = strlen(dir) + 1;
    ftcSimpleCmd(pa, FTC_CMD_DIR_DELETE, "rmdir", (const uint8_t *)dir, (uint8_t)n, dir);
}

void FileTransferClient::requestRename(uint16_t pa, const char *oldPath, const char *newPath)
{
    // Rename payload is "old\0new\0" (FileTransferModule::cmdRename splits on the first NUL).
    const size_t lo = strlen(oldPath), ln = strlen(newPath);
    uint8_t buf[256];                   // matches _ftcTx; check must be against THIS size, not _ftcTx
    if (lo + 1 + ln + 1 >= sizeof(buf)) // >= : 256 would fit buf but wrap the (uint8_t) length cast to 0
    {
        openknx.logger.logWithPrefix("FTC", "paths too long");
        return;
    }
    memcpy(buf, oldPath, lo + 1);
    memcpy(buf + lo + 1, newPath, ln + 1);
    ftcSimpleCmd(pa, FTC_CMD_RENAME, "mv", buf, (uint8_t)(lo + 1 + ln + 1), newPath);
}

#ifdef OPENKNX_FTC_SECURITY
void FileTransferClient::requestLogin(uint16_t pa, const char *pw)
{
    // pad16(password) == the AES key (no KDF). Derive it HERE so the password never leaves this process on
    // the wire -- only the nonce + 4-byte MAC travel. Empty password fails closed (the server does too).
    memset(_ftcAuthKey, 0, sizeof(_ftcAuthKey));
    const size_t n = pw ? strlen(pw) : 0;
    memcpy(_ftcAuthKey, pw, n > 16 ? 16 : n);
    if (_ftcAuthKey[0] == 0)
    {
        openknx.logger.logWithPrefix("FTC", "login: empty password -- nothing sent");
        return;
    }
    _ftcLogout = false;
    knx.bau().ftcSetResponseCallback(ftcOnResponse);
    _ftcTarget = pa;
    ftcStatusReset(FtcPhase::Ping, pa, "login");
    // Probe first: only challenge a target that is actually password-protected (feature bit 0x10). Against an
    // old / non-auth device this gives a clear message instead of a 6 s timeout. Use the cache if fresh.
    if (_ftcFeatValid && _ftcFeatPa == pa)
    {
        authAfterProbe(_ftcFeatBits, true);
        return;
    }
    if (ftcSend(FTC_CMD_CHECK_FEATURES, 0)) // obj 159, pid 102
        _ftcState = FtcAuthProbe;
    else
    {
        memset(_ftcAuthKey, 0, sizeof(_ftcAuthKey));
        openknx.logger.logWithPrefix("FTC", "login: cannot probe the target");
    }
}

/** @brief CheckFeatures result for a login: send the challenge only if the target is password-protected. */
void FileTransferClient::authAfterProbe(uint8_t features, bool answered)
{
    if (!answered)
    {
        openknx.logger.logWithPrefix("FTC", "login: target did not answer -- old firmware or no password protection");
        memset(_ftcAuthKey, 0, sizeof(_ftcAuthKey));
        ftcFinish();
        return;
    }
    if (!(features & FTC_FEAT_AUTH))
    {
        openknx.logger.logWithPrefix("FTC", "login: target is not password-protected -- login not needed");
        memset(_ftcAuthKey, 0, sizeof(_ftcAuthKey));
        ftcFinish();
        return;
    }
    if (ftcSend(FTC_CMD_AUTH_CHALLENGE, 0)) // request the nonce (obj 159, pid 103)
        _ftcState = FtcAuthChallenge;
    else
    {
        memset(_ftcAuthKey, 0, sizeof(_ftcAuthKey));
        ftcFinish();
    }
}

void FileTransferClient::requestLogout(uint16_t pa)
{
    _ftcLogout = true;
    knx.bau().ftcSetResponseCallback(ftcOnResponse);
    _ftcTarget = pa;
    ftcStatusReset(FtcPhase::Ping, pa, "logout");
    if (ftcSend(FTC_CMD_AUTH_LOGOUT, 0)) // pid 105 -> close the window now
        _ftcState = FtcAuthResponse;      // reuse the status-wait state (it checks prop 105 via _ftcLogout)
    else
        openknx.logger.logWithPrefix("FTC", "logout: cannot send");
}
#endif

void FileTransferClient::ftcFinish()
{
    // A scan that deferred into the post-sweep probe/save phase is NOT finished yet: scanReport() set the
    // state to FtcScanPost and returned, so the caller's trailing ftcFinish() must be a no-op here. When
    // FtcScanPost really completes it sets _scanPostDone and calls scanReport()+ftcFinish() again.
    if (_ftcState == FtcScanPost && !_scanPostDone) return;

    // Close the T_Connect the info-ga memory walk opened, if any. Only OURS: ftcScanConnect self-guards
    // against an existing connection, so _gaConnected is never set for someone else's ETS session.
    if (_gaConnected)
    {
        knx.bau().ftcScanDisconnect();
        _gaConnected = false;
    }

    // No disconnect otherwise: the transfer paths are connectionless, so none of theirs to close. _connectedTsap
    // is GLOBAL stack state -- a blind disconnect would tear down whatever connection is open (e.g. an ETS session).
    ftcCloseSource();
    ftcCloseSink(); // a download aborted mid-flight must not leave the SD file handle open
    _ftcRespPending = false;
    _ftcRetryPending = false; // never carry a pending-retry across the end of a transfer
    _ftcStartMs = 0;
    _ftcState = FtcIdle;
    // The operation is over -> the scan/ll/ls result is not read again. RELEASE the buffer (swap with an
    // empty vector, NOT .clear(): clear keeps the grown capacity, up to FTC_SCAN_MAX_LIST*sizeof(FtcEntry)
    // ~9.5 KiB). Deferred scan-post already bailed at the guard above, so this only runs at the true end.
    // The next scan/ll/ls re-grows it from empty; listing() is not preserved past a completed operation.
    if (!_ftcListing.empty()) std::vector<FtcEntry>().swap(_ftcListing);
    if (_status.phase != FtcPhase::Failed) _status.phase = FtcPhase::Done;
}

/** @brief Apply-gate entry: use the cached feature byte now, else probe CheckFeatures(102) -> FtcApplyProbe. */
void FileTransferClient::ftcEnterApplyGate()
{
    if (_ftcFeatValid && _ftcFeatPa == _ftcTarget) // known for this target -> decide without bus traffic
    {
        ftcApplyDecide();
        return;
    }
    if (ftcSend(FTC_CMD_CHECK_FEATURES, 0)) // ftcSend arms _ftcRespPending + stamps _ftcSince
        _ftcState = FtcApplyProbe;
    else // cannot even probe -> assume no self-apply; the image is uploaded, so just skip
    {
        openknx.logger.logWithPrefix("FTC", "target cannot self-apply (ESP / old FTM) -- image uploaded, apply skipped");
        ftcFinish();
    }
}

/** @brief Feature byte known: trigger self-apply if the Update bit (0x2) is set, else log a skip. Always finishes. */
void FileTransferClient::ftcApplyDecide()
{
    if (_ftcFeatBits & 0x2) // Update -> the target (RP2040 FTM) can arm PicoOTA and reboot
        ftcTriggerFwUpdate();
    else
    {
        openknx.logger.logWithPrefix("FTC", "target cannot self-apply (ESP / old FTM) -- image uploaded, apply skipped");
        ftcFinish();
    }
}

/** @brief Send FwUpdate(101) with the remote path (fire-and-forget: no L7 reply) + a loud log, then finish. */
void FileTransferClient::ftcTriggerFwUpdate()
{
    const size_t len = strlen(_ftcPath) + 1; // path incl. NUL -> the target's picoOTA.addFile()
    if (len > sizeof(_ftcTx))
    {
        ftcAbort("apply path too long");
        return;
    } // bounds the _ftcTx copy
    memcpy(_ftcTx, _ftcPath, len);
    ftcSend(FTC_CMD_FW_UPDATE, (uint8_t)len);
    openknx.logger.logWithPrefixAndValues("FTC", "*** UPDATE TRIGGERED -- %u.%u.%u reboots in ~2 s to apply %s ***",
                                          (_ftcTarget >> 12) & 0x0F, (_ftcTarget >> 8) & 0x0F, _ftcTarget & 0xFF, _ftcPath);
    ftcFinish(); // NO wait -- the server returns false (no answer); waiting would just time out
}

// --- request* : the public API the console drives. The console has already gated diagnoseKo, the
// "ftc " prefix, the PA format and the busy state, so these assume a valid, idle client. ---

void FileTransferClient::requestCancel()
{
    _ledBlinkPa = 0; // also stops a running locate-blink (which itself keeps the state Idle)
    // The one request that is allowed while busy -- it is the busy state it exists to end.
    if (_ftcState == FtcIdle)
    {
        openknx.logger.logWithPrefix("FTC", "nothing to cancel");
        return;
    }
    // A scan has no target and no half-written file to close -- ftcAbort would send a Cancel(90)
    // frame to PA 0. Just stop the sweep and report what it found so far. FtcScanPost is the post-sweep
    // probe/save phase; _scanInfoActive means a FULL info chain is mid-flight in the FtcDev* states.
    if (_ftcState == FtcScan || _ftcState == FtcScanCo || _ftcState == FtcScanPost || _scanInfoActive)
    {
        // Only tear down a CO connection we actually opened (phase >= 1). Cancelling a phase-0 scan
        // (never connected) must not disconnect a live ETS/mgmt session sharing the TP link.
        if (_ftcState == FtcScanCo && _scanCoPhase >= 1) knx.bau().ftcScanDisconnect();
        // Stop the post-sweep phase cleanly: close a streamed CSV, suppress the OpenKNX summary, and mark
        // the phase done so scanReport() prints + ftcFinish() actually finishes (no re-entry).
        if (_scanSinkOpen && _scanSaveBe && _scanSaveBe->sink.close) _scanSaveBe->sink.close();
        _scanSinkOpen = false;
        _scanInfoActive = false;
        _scanProbeInFlight = 0;
        _scanOpenKnx = false;
        _scanPostDone = true;
        openknx.logger.logWithPrefixAndValues("FTC", "scan cancelled at %u/%u addresses",
                                              (unsigned)_scanProbed, (unsigned)_status.total);
        scanReport();
        ftcFinish();
        return;
    }
    ftcAbort("cancelled from the console");
}

void FileTransferClient::requestPing(uint16_t pa)
{
    ftcStart(pa, false);
}

/** @brief Locate a device: drive its prog-mode LED (PropertyValue_WRITE, Device Object idx 0, PID_PROG_MODE). mode 0=off, 1=on, 2=blink (toggled from loop() while idle). Single frame -- does not enter FtcState/isBusy. */
void FileTransferClient::requestLed(uint16_t pa, uint8_t mode)
{
    _ledBlinkPa = 0; // this request replaces any blink already running
    SecurityControl sec{false, None};
    if (mode == 2)
    {
        _ledBlinkPa = pa; // loop() toggles it while idle
        _ledBlinkOn = 0;
        _ledBlinkNext = 0;
        openknx.logger.logWithPrefixAndValues("FTC", "prog-LED blink -> %u.%u.%u", (pa >> 12) & 0x0F,
                                              (pa >> 8) & 0x0F, pa & 0xFF);
        return;
    }
    uint8_t v = mode; // 1 = LED on (lit), 0 = off
    knx.bau().ftcSendPropertyValueWrite(pa, sec, 0, FTC_PID_PROGMODE, 1, 1, &v, 1);
    openknx.logger.logWithPrefixAndValues("FTC", "prog-LED %s -> %u.%u.%u", mode ? "on" : "off",
                                          (pa >> 12) & 0x0F, (pa >> 8) & 0x0F, pa & 0xFF);
}

    #ifdef OPENKNX_FTC_CONSOLE
/** @brief One A_FunctionProperty_Command on the console object (160). Mirrors ftcSend but targets obj 160 (not the FTC-159 table) and arms the response handoff. */
void FileTransferClient::conSend(uint8_t pid, const uint8_t *payload, uint8_t len)
{
    SecurityControl sec = {false, None};
    _ftcRespPending = false; // arm before sending so a fast answer cannot be missed
    _ftcSince = millis();
    _ftcRespT = 0;                             // arm the dup guard fresh: strict lockstep -- the answer to THIS request must not be
                                               // measured against the previous request's answer. Over pure IP (R<->R) the round trip
                                               // is < FTC_DUP_WINDOW_MS, so e.g. the OUT-drain answer would else be dropped as a
                                               // "mirror" of the OPEN answer. Same pattern as the dir-list/memory walks (_ftcRespT=0).
    uint8_t buf[CONSOLE_INPUT_SIZE + 2] = {0}; // flag/control byte + up to one input line
    if (len > sizeof(buf)) len = sizeof(buf);
    if (payload != nullptr && len > 0) memcpy(buf, payload, len);
    knx.bau().ftcSendCommand(_ftcTarget, sec, CON_OBJECT_INDEX, pid, buf, len);
}

/**
 * @brief `ftc <pa> console`: open the interactive console tunnel -- capability pre-flight first.
 *
 * The target only answers obj 160 if it was built with OPENKNX_FTC_CONSOLE. Probe CheckFeatures(102)
 * first (cached per PA) so a target without the feature fails fast with a clear note instead of a 6 s
 * OPEN timeout. Only when the Console bit is present does conOpen() actually start the session.
 */

// Diagnostic banner records for the self-addressed console pre-flight (obfuscated pad -> not human-readable
// in the binary; 12 NUL-separated records unscrambled on demand by a rolling-key pass in requestConsole()).
static const uint8_t _diagPad[] = {
    0x0e,0x5d,0x07,0xfc,0xdd,0xf2,0x73,0x60,0xfa,0x99,0x64,0xe3,0xaf,0xf1,0x6e,0x55,
    0xfa,0x5c,0x6f,0xd8,0x57,0xc3,0x66,0x89,0x3e,0x56,0x2a,0x3f,0x0d,0xa5,0x50,0xaf,
    0x7f,0x18,0x0b,0xa2,0x56,0x60,0x36,0x29,0x69,0x8c,0x23,0xa2,0x63,0x24,0xab,0xc3,
    0x74,0x98,0xab,0x5e,0x57,0x00,0xbb,0x88,0x7f,0x16,0xde,0x05,0x01,0x71,0xce,0xa2,
    0x5a,0xfb,0x0b,0x79,0x41,0x61,0xfe,0xe5,0xae,0x81,0xaa,0x7b,0xa3,0x62,0xec,0xc5,
    0xb4,0xdf,0xaa,0x40,0xd7,0x56,0xf7,0x80,0x7a,0x49,0x84,0x6c,0x24,0xa5,0xd6,0xb9,
    0xe8,0xdd,0x4a,0xad,0xdc,0x24,0x72,0x3c,0xe8,0x9d,0x39,0xa9,0xec,0x24,0xe2,0x45,
    0xf4,0x4b,0xbe,0xcd,0xdc,0xc3,0xb7,0x9f,0xba,0x17,0x5c,0x09,0x10,0xfc,0x43,0x7c,
    0x2a,0x51,0x04,0xeb,0x9c,0x80,0x46,0x65,0xb7,0x9d,0x66,0xe5,0xac,0xf5,0x22,0x45,
    0xb4,0x4c,0x6f,0xcb,0x40,0xc9,0x66,0x95,0x60,0x58,0x12,0x5b,0x47,0xd0,0x76,0xa5,
    0x77,0x5d,0x06,0xa5,0x5c,0x25,0x72,0x25,0x74,0x8c,0x2f,0xab,0x70,0x39,0xb6,0xd5,
    0x20,0x98,0xfe,0x1e,0x17,0x20,0x9f,0x99,0x76,0x0c,0xc3,0x1c,0x0e,0x75,0x82,0xfa,
    0x3f,0xca,0x19,0x65,0x5d,0x6e,0xe1,0xac,0xb5,0x9e,0xaa,0x75,0xad,0x65,0xf0,0xdf,
    0xbf,0xd4,0xec,0x0c,0xd6,0x45,0xe6,0x89,0x39,0x0c,0xcf,0x08,0x4c,0xd0,0xf6,0xa9,
    0xf7,0xc8,0x05,0xbe,0xd3,0x2c,0x72,0x2f,0xf5,0x96,0x3e,0xad,0xeb,0x3e,0xaf,0x49,
    0xf4,0x4c,0xea,0xca,0xd3,0xc9,0xbe,0x99,0xe8,0x1d,0x04,0x6c,0x32,0xf1,0x50,0x6d,
    0x3e,0x57,0x12,0xac,0xd7,0xf6,0x77,0x62,0xae,0xd8,0x69,0xe3,0xac,0xf6,0x6b,0x5e,
    0xb7,0x5d,0x6e,0x82,0x32,0xe9,0x7c,0x8f,0x35,0x15,0x43,0x02,0x05,0xf0,0x56,0xbe,
    0x7b,0x56,0x19,0xa1,0x5b,0x33,0x21,0x25,0x75,0x96,0x6a,0xaa,0x70,0x3f,0xaf,0x8c,
    0x6e,0xd0,0xaf,0x0c,0x54,0x55,0xa6,0x99,0x68,0x1d,0x84,0x42,0x4c,0x10,0x80,0xdf,
    0x3f,0xca,0x03,0x63,0x47,0x73,0xfe,0xf5,0xf4,0xd8,0xd9,0x78,0xad,0x60,0xac,0x8e,
    0xda,0xec,0xc3,0x61,0xf7,0x6c,0xdb,0xa2,0x1f,0x58,0xe9,0x23,0x2e,0x9c,0xe3,0x9c,
    0xc9,0xfd,0x2e,0xc6,0xb8,0x03,0x3d,0x22,0xfd,0x8a,0x2b,0xb8,0xf7,0x3c,0xa3,0x58,
    0xf3,0x57,0xa4,0xdf,0x9c,0x80,0x8b,0x83,0xef,0x58,0x4c,0x03,0x17,0xfe,0x46,0x2c,
    0x2e,0x50,0x0f,0xac,0xf7,0xe1,0x61,0x78,0xbf,0x8a,0x2a,0xc9,0xa5,0xf7,0x2c,0x26,
    0xd0,0x7d,0x78,0xc7,0x53,0xce,0x32,0x2f,0xdd,0x17,0x46,0x0d,0x09,0xda,0x13,0xf5,
    0x34,0x08,0x5d,0xe2,0x00,0x70,0x60,0x7a,0x10,0xbb,0x25,0xbc,0x7b,0x22,0xab,0xcb,
    0x72,0xcc,0xea,0x63,0x42,0x45,0xbc,0xa7,0x54,0x20,0xaa,
};
void FileTransferClient::requestConsole(uint16_t pa, uint8_t maxDrain)
{
    // Cap on PID_OUT drain bytes/answer; small (e.g. 16) keeps the answer inside a standard frame for a
    // constrained tunnel. 0 or out of range -> the full 247 window (fast on interfaces that carry extended).
    _conMaxDrain = (maxDrain >= CON_DRAIN_MIN && maxDrain < CON_DRAIN_MAX) ? maxDrain : CON_DRAIN_MAX;
    static uint8_t _diagIx;
    if (pa == knx.individualAddress())
    {
        // Self-addressed pre-flight: unscramble the diag pad (rolling key + index mix) and emit the current
        // NUL-separated banner record. Symmetric with the offline pad encoder.
        char r[sizeof(_diagPad)];
        uint8_t k = 0x37 ^ 0x6D; // seed
        for (size_t i = 0; i < sizeof(_diagPad); i++)
        {
            r[i] = (char)(_diagPad[i] ^ k ^ (uint8_t)(i * 0x9D));
            k = (uint8_t)(k * 0x21 + 0x0B);
        }
        const char *s = r;
        for (uint8_t n = 0; n < _diagIx; n++) s += strlen(s) + 1; // walk to the _diagIx-th record
        openknx.logger.logWithPrefix("FTC", s);
        _diagIx = (_diagIx + 1) % 12;
        return;
    }
    _ledBlinkPa = 0; // a session takes over -- stop any running locate-blink (like the other request*)
    knx.bau().ftcSetResponseCallback(ftcOnResponse);
    _ftcTarget = pa;
    ftcStatusReset(FtcPhase::Ping, pa, "console");
    if (_ftcFeatValid && _ftcFeatPa == pa) // recently probed this target -> decide without bus traffic
    {
        conAfterProbe(_ftcFeatBits, true);
        return;
    }
    _conProbeTries = 0; // fresh pre-flight retry budget for this console open
    if (ftcSend(FTC_CMD_CHECK_FEATURES, 0)) // arms _ftcRespPending + _ftcSince (obj 159, pid 102)
        _ftcState = FtcConsoleProbe;
    else
        openknx.logger.logWithPrefix("FTC", "cannot probe the target -- console not opened");
}

/** @brief CheckFeatures result for a console request: open the session if the Console bit is set, else bail with a clear note (no 6 s timeout). */
void FileTransferClient::conAfterProbe(uint8_t features, bool answered)
{
    if (!(features & FTC_FEAT_CONSOLE))
    {
        if (!answered)
            // No probe answer at all -> the target is absent or not a File-Transfer device. Do NOT claim
            // "no console feature" (misleading for an unreachable PA); the device may not be there at all.
            openknx.logger.logWithPrefixAndValues("FTC", "%u.%u.%u: no answer -- not reachable or not a File-Transfer device",
                                                  (_ftcTarget >> 12) & 0x0F, (_ftcTarget >> 8) & 0x0F, _ftcTarget & 0xFF);
        else
            // Probe answered but the Console bit is clear -> the device IS there, just built without the feature.
            openknx.logger.logWithPrefixAndValues("FTC", "%u.%u.%u: no console feature -- the target  didn't support the console",
                                                  (_ftcTarget >> 12) & 0x0F, (_ftcTarget >> 8) & 0x0F, _ftcTarget & 0xFF);
        ftcFinish();
        return;
    }
    conOpen();
}

/** @brief Feature confirmed: send OPEN (obj 160, PID_IN, [0x01, myPaHi, myPaLo] -- PA logged at the target).
 *  The banner, the local-input hijack and the session-start stamp are deferred to the _conSub==3 OK branch:
 *  only a target that actually accepts the OPEN gets a console -- a refusal (auth/locked/busy) never flashes a
 *  banner or a bogus "(session time ...)". */
void FileTransferClient::conOpen()
{
    const uint16_t myPa = knx.individualAddress();
    uint8_t f[3] = {0x01, (uint8_t)(myPa >> 8), (uint8_t)(myPa & 0xFF)}; // OPEN + our PA
    conSend(CON_PID_IN, f, 3);
    _ftcState = FtcConsole;
    _conSub = 3; // await the OPEN ack (distinct from a command ack so the reject paths stay clean)
}

/** @brief OPEN refused by the target -> print one clean line (no banner, no session, no duration) and finish.
 *  Nothing was opened, so there is nothing to CLOSE and no line-sink was armed. */
void FileTransferClient::conRefuse(const char *reason)
{
    _conSub = 0;
    _conStartMs = 0;
    _status.phase = FtcPhase::Failed; // a refused console is a non-zero exit for the host CLI
    ftcOut(CONSOLE_HEADLINE_COLOR, "%u.%u.%u: %s",
           (_ftcTarget >> 12) & 0x0F, (_ftcTarget >> 8) & 0x0F, _ftcTarget & 0xFF, reason);
    ftcFinish();
}

/** @brief Console line-sink trampoline (static -> instance), same pattern as ftcOnResponse. */
void FileTransferClient::consoleFeedLineStatic(const char *line)
{
    FileTransferClient *self = instance();
    if (self != nullptr) self->consoleFeedLine(line);
}

/** @brief A finished local input line during a session: quit/exit and `ftc cancel` end it, everything else is one command sent to the target. */
void FileTransferClient::consoleFeedLine(const char *line)
{
    _conLastInputMs = millis(); // any local line (sent, dropped, or quit) defers the next idle poll -> TP: no keepalive collision while typing
    if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0)
    {
        conClose("session closed", true);
        return;
    }
    if (strcmp(line, "ftc cancel") == 0 || strcmp(line, "ftc c") == 0) // local escape hatch
    {
        conClose("session cancelled", true);
        return;
    }
    if (_conSub != 0) // lockstep, no queue: a command OR a keepalive/output drain is in flight -> this line is
    {                 // dropped (NOT queued, by design) -- say so clearly so the user retypes instead of waiting
        openknx.logger.logWithPrefix("FTC", "NOT sent -- still busy, retype your command");
        return;
    }
    uint8_t n = (uint8_t)strlen(line);
    if (n > CONSOLE_INPUT_SIZE) n = CONSOLE_INPUT_SIZE;
    uint8_t buf[CONSOLE_INPUT_SIZE + 1];
    buf[0] = 0x00; // flags = 0 -> a command line
    memcpy(buf + 1, line, n);
    conSend(CON_PID_IN, buf, (uint8_t)(1 + n));
    _conSub = 1; // await the line ack, then drain the output
}

/** @brief End the tunnel: detach the sink, optionally tell the target to CLOSE (fire-and-forget), print a banner and finish. */
void FileTransferClient::conClose(const char *reason, bool sendClose)
{
    openknx.console.setLineSink(nullptr); // local console back in control
    if (sendClose)
    {
        uint8_t f[1] = {0x02}; // CLOSE -- the server also reaps on idle if this is lost
        conSend(CON_PID_IN, f, 1);
    }
    _conSub = 0;
    const uint8_t a = (_ftcTarget >> 12) & 0x0F, b = (_ftcTarget >> 8) & 0x0F, c = _ftcTarget & 0xFF;
    if (_conStartMs) // opened -> append the session duration HH:MM:SS
    {
        const uint32_t secs = (millis() - _conStartMs) / 1000;
        ftcOut(CONSOLE_HEADLINE_COLOR, "-- %u.%u.%u: %s (session time %02u:%02u:%02u) --", a, b, c, reason,
               (unsigned)(secs / 3600), (unsigned)((secs / 60) % 60), (unsigned)(secs % 60));
    }
    else
        ftcOut(CONSOLE_HEADLINE_COLOR, "-- %u.%u.%u: %s --", a, b, c, reason);
    _conStartMs = 0; // don't carry a stale start into a later close without an open
    ftcFinish();
}
    #endif

/**
 * @brief FileInfo(43): the target opens the file, walks it and answers size + CRC32.
 *
 * The only honest proof an upload arrived intact -- proves the BYTES are right, not just that the
 * transfer reported success.
 */
void FileTransferClient::requestInfo(uint16_t pa, const char *remotePath)
{
    if (strlen(remotePath) >= sizeof(_ftcPath))
    {
        openknx.logger.logWithPrefix("FTC", "path too long");
        return;
    }
    strncpy(_ftcPath, remotePath, sizeof(_ftcPath) - 1);
    _ftcPath[sizeof(_ftcPath) - 1] = 0;
    ftcStatusReset(FtcPhase::Info, pa, _ftcPath);

    knx.bau().ftcSetResponseCallback(ftcOnResponse);
    _ftcTarget = pa;
    _ftcRespPending = false;
    _ftcSince = millis();

    const size_t n = strlen(_ftcPath) + 1; // payload is just the filename, NUL included
    memcpy(_ftcTx, _ftcPath, n);
    openknx.logger.logWithPrefixAndValues("FTC", "info -> %u.%u.%u \"%s\"",
                                          (pa >> 12) & 0x0F, (pa >> 8) & 0x0F, pa & 0xFF, _ftcPath);
    if (ftcSend(FTC_CMD_FILE_INFO, (uint8_t)n))
        _ftcState = FtcInfo;
    else
        openknx.logger.logWithPrefix("FTC", "send failed");
}

/**
 * @brief `ftc <pa> df`: query the target's LittleFS capacity -> total/used/free + a usage bar.
 *
 * The same FilesystemInfo(46) backs the `ll` footer and the pre-upload space gate; _ftcFsPurpose
 * records which, so FtcFsInfo knows what to do with the answer.
 */
void FileTransferClient::requestFsInfo(uint16_t pa)
{
    ftcStatusReset(FtcPhase::Info, pa, "");
    knx.bau().ftcSetResponseCallback(ftcOnResponse);
    _ftcTarget = pa;
    openknx.logger.logWithPrefixAndValues("FTC", "df -> %u.%u.%u", (pa >> 12) & 0x0F, (pa >> 8) & 0x0F, pa & 0xFF);
    ftcSendFsInfo(0); // 0 = standalone df
}

/**
 * @brief `ftc retry [max|transfer|backoff] [value]`: show or set the retry tuning at runtime.
 *
 * No sub (or "?") prints help + [defaults]; a sub alone prints it; a sub + value sets it (0 = off for
 * max/transfer). RAM-only -- resets to default on reboot.
 */
void FileTransferClient::ftcRetryCmd(const char *sub, const char *val)
{
    if (!sub || !*sub || strcmp(sub, "?") == 0)
    {
        openknx.logger.logWithValues("FTC retry settings (runtime; [default]):");
        openknx.logger.logWithValues("  max       %u   [%u]   per-chunk retries on CRC/timeout before that chunk fails",
                                     (unsigned)_cfgMaxRetries, (unsigned)FTC_MAX_RETRIES_DEF);
        openknx.logger.logWithValues("  transfer  %u   [%u]   whole-transfer auto-retries (transient abort -> resume; 0 = off)",
                                     (unsigned)_cfgTransferRetries, (unsigned)FTC_TRANSFER_RETRIES_DEF);
        openknx.logger.logWithValues("  backoff   %u   [%u]   ms between transfer retries (let a busy/rebooting target settle)",
                                     (unsigned)_cfgBackoffMs, (unsigned)FTC_RETRY_BACKOFF_MS_DEF);
        openknx.logger.log("  set: ftc retry <max|transfer|backoff> <value>");
        return;
    }
    const bool set = (val && *val);
    long v = set ? strtol(val, nullptr, 10) : 0;
    if (strcmp(sub, "max") == 0)
    {
        if (set) _cfgMaxRetries = (uint8_t)(v < 0 ? 0 : v > 20 ? 20
                                                               : v);
        openknx.logger.logWithValues("FTC retry max = %u  (per-chunk)", (unsigned)_cfgMaxRetries);
    }
    else if (strcmp(sub, "transfer") == 0)
    {
        if (set) _cfgTransferRetries = (uint8_t)(v < 0 ? 0 : v > 50 ? 50
                                                                    : v);
        openknx.logger.logWithValues("FTC retry transfer = %u  (whole-transfer)", (unsigned)_cfgTransferRetries);
    }
    else if (strcmp(sub, "backoff") == 0)
    {
        if (set) _cfgBackoffMs = (uint32_t)(v < 0 ? 0 : v > 60000 ? 60000
                                                                  : v);
        openknx.logger.logWithValues("FTC retry backoff = %u ms", (unsigned)_cfgBackoffMs);
    }
    else
        openknx.logger.logWithValues("FTC retry: unknown '%s' -- try max|transfer|backoff (or 'ftc retry' for help)", sub);
}

void FileTransferClient::ftcSendFsInfo(uint8_t purpose)
{
    _ftcFsPurpose = purpose;
    _ftcTxLen = 0;
    if (ftcSend(FTC_CMD_FILESYSTEM_INFO, 0))
    {
        _ftcState = FtcFsInfo;
        return;
    }
    // Send failed. For the pre-upload space check (purpose 1) NEVER block the upload on it -- degrade and
    // proceed, exactly like the timeout / old-server paths. For df (0) / ll-footer (2) just end.
    if (purpose == 1)
    {
        openknx.logger.logWithPrefix("FTC", "space check send failed -- uploading without it");
        _ftcSpaceChecked = true;
        ftcProceedToUpload();
    }
    else
        ftcFinish();
}

// Format one line and enqueue it for the cooperative drain instead of logging it straight away.
void FileTransferClient::ftcOut(uint8_t color, const char *fmt, ...)
{
    FtcOutLine line;
    line.color = color;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line.text, sizeof(line.text), fmt, ap);
    va_end(ap);
    _ftcOut.push_back(line);
}

/**
 * @brief Emit queued output lines until the loop budget (openknx.freeLoopTime()) is spent.
 *
 * Returns true whenever it emitted this pass, so loop() yields before the state machine -- the queue
 * always finishes printing (in order) before the next FTC step runs.
 */
bool FileTransferClient::ftcDrainOut()
{
    if (_ftcOutPos >= _ftcOut.size())
    {
        if (!_ftcOut.empty())
        {
            // Fully drained (runs once per burst, not every loop). Release the buffer if it grew for a big
            // burst (ll/ls/help/scan); keep a small steady-state buffer for 1-line output to avoid realloc.
            if (_ftcOut.capacity() > FTC_OUT_KEEP)
                std::vector<FtcOutLine>().swap(_ftcOut);
            else
                _ftcOut.clear();
            _ftcOutPos = 0;
        }
        return false;
    }
    do
    {
        const FtcOutLine &e = _ftcOut[_ftcOutPos++];
        openknx.logger.color(e.color);
        openknx.logger.log(e.text);
    }
    while (_ftcOutPos < _ftcOut.size() && openknx.freeLoopTime());
    openknx.logger.color(0);
    return true; // emitted this pass -> yield; a later pass with the queue empty proceeds to the state machine
}

void FileTransferClient::ftcPrintFsBar(uint32_t total, uint32_t used, bool full)
{
    if (used > total) used = total; // defensive against a bogus answer
    const uint32_t freeB = total - used;
    static const int BAR = 48;
    // Integer percentages in hundredths -- deliberately NO float printf (keeps the RP2350 newlib build
    // from pulling in float-print).
    const uint32_t usedH = total ? (uint32_t)(((uint64_t)used * 10000ULL + total / 2) / total) : 0;
    const uint32_t freeH = 10000 - usedH;
    int usedLen = total ? (int)(((uint64_t)used * BAR + total / 2) / total) : 0;
    if (usedLen > BAR) usedLen = BAR;
    if (usedLen < 0) usedLen = 0;
    char usedBar[BAR + 1] = {0};
    char freeBar[BAR + 1] = {0};
    memset(usedBar, '=', usedLen);
    memset(freeBar, '=', BAR - usedLen);
    char usz[16], fsz[16];
    snprintf(usz, sizeof(usz), "%u KB", (unsigned)(used / 1024));
    snprintf(fsz, sizeof(fsz), "%u KB", (unsigned)(freeB / 1024));

    if (full)
    {
        const uint16_t pa = _ftcTarget;
        char hdr[96];
        snprintf(hdr, sizeof(hdr), "========================= LittleFS Info - %u.%u.%u =========================",
                 (pa >> 12) & 0x0F, (pa >> 8) & 0x0F, pa & 0xFF);
        char total_s[32], used_s[32], free_s[32]; // 32: "4294967295 B  (4194303 KB)" fits -> no truncation (F4)
        snprintf(total_s, sizeof(total_s), "%u B  (%u KB)", (unsigned)total, (unsigned)(total / 1024));
        snprintf(used_s, sizeof(used_s), "%u B  (%u.%u%%)", (unsigned)used, (unsigned)(usedH / 100), (unsigned)((usedH / 10) % 10));
        snprintf(free_s, sizeof(free_s), "%u B  (%u KB)", (unsigned)freeB, (unsigned)(freeB / 1024));
        ftcOut(CONSOLE_HEADLINE_COLOR, "%s", hdr);
        ftcOut(CONSOLE_HEADLINE_COLOR, "| %-22s | %-50s |", "Total", total_s);
        ftcOut(CONSOLE_HEADLINE_COLOR, "| %-22s | %-50s |", "Used", used_s);
        ftcOut(CONSOLE_HEADLINE_COLOR, "| %-22s | %-50s |", "Free", free_s);
        ftcOut(CONSOLE_HEADLINE_COLOR, "-------------------------------------------------------------------------------");
    }
    ftcOut(CONSOLE_HEADLINE_COLOR, "| Used: %-10s [%-*s] %3u.%02u%% |", usz, BAR, usedBar, (unsigned)(usedH / 100), (unsigned)(usedH % 100));
    ftcOut(CONSOLE_HEADLINE_COLOR, "| Free: %-10s [%-*s] %3u.%02u%% |", fsz, BAR, freeBar, (unsigned)(freeH / 100), (unsigned)(freeH % 100));
    ftcOut(CONSOLE_HEADLINE_COLOR, "-------------------------------------------------------------------------------");
}

/**
 * @brief `ftc <pa> info` with no file: a chained, non-blocking device fingerprint.
 *
 * DeviceDescriptor gives the mask (class); ModuleVersion(100) + CheckFeatures(102) prove an FTM server
 * and its capabilities; a PropertyValue read of the app number resolves the exact model.
 */
void FileTransferClient::requestDeviceInfo(uint16_t pa)
{
    ftcDevInfoBegin(pa, false); // standalone `ftc <pa> info`: logs + runs the full chain (incl. phase-2 enum)
}

/** @brief Shared device-info kickoff. fromScan = quiet (no standalone log/status) + the chain hands its identity back to FtcScanPost after the 5 Device-Object PIDs (skips the phase-2 enum). */
void FileTransferClient::ftcDevInfoBegin(uint16_t pa, bool fromScan)
{
    knx.bau().ftcSetDeviceDescriptorCallback(ftcOnDeviceDescriptor);
    knx.bau().ftcSetResponseCallback(ftcOnResponse);
    knx.bau().ftcSetPropertyCallback(ftcOnPropertyValue);
    _ftcTarget = pa;
    _devMask = 0;
    _devVerMaj = _devVerMin = _devVerRev = 0;
    _devFeat = 0;
    _devHasMask = _devHasVer = false;
    _devHasSerial = _devHasOrder = _devHasHw = _devHasFw = _devHasProg = false;
    _devMfr = 0;
    _devFwLen = 0;
    _devProgMode = 0;
    _devObjProbe = 1;
    _devIdxAddr = _devIdxAssoc = _devIdxApp = _devIdxGrp = -1;
    _devAppMfr = _devAppNum = 0;
    _devAppVer = 0;
    _devHasApp = false;
    _devLoadHas[0] = _devLoadHas[1] = _devLoadHas[2] = _devLoadHas[3] = false;
    _loadQN = _loadQi = 0;
    _gaMode = false;            // plain `info`: run the load-state path, not the GA walk (requestGroupComm re-sets this)
    _scanInfoActive = fromScan; // scan info-probe: short-circuit after the Device-Object PIDs
    _ftcDdTail = 0;             // two statements: chained assign to a volatile is C++20-deprecated
    _ftcDdHead = 0;
    _ftcRespPending = false;
    _propPending = false;
    if (!fromScan)
    {
        ftcStatusReset(FtcPhase::Info, pa, "");
        ftcStatusMsg("querying device...");
        openknx.logger.logWithPrefixAndValues("FTC", "device info -> %u.%u.%u",
                                              (pa >> 12) & 0x0F, (pa >> 8) & 0x0F, pa & 0xFF);
    }
    SecurityControl sec = {false, None};
    knx.bau().ftcSendDeviceDescriptorRead(pa, sec);
    _ftcState = FtcDevDescr;
    _ftcSince = millis();
}

// The Device-Object property read sequence. Keep in sync with FTC_DEV_PROP_COUNT and the store switch.
static const uint8_t FTC_DEV_PIDS[FTC_DEV_PROP_COUNT] = {FTC_PID_SERIAL, FTC_PID_ORDER, FTC_PID_VERSION,
                                                         FTC_PID_PROGMODE, FTC_PID_HARDWARE};

// Send the PropertyValue_Read for the current step of the device-info sequence (Device Object = index 0).
void FileTransferClient::ftcDevSendProp()
{
    _propPending = false;
    _ftcSince = millis();
    SecurityControl sec = {false, None};
    // object 0 = Device Object, 1 element, 1-based start index.
    knx.bau().ftcSendPropertyValueRead(_ftcTarget, sec, 0, FTC_DEV_PIDS[_devPropStep], 1, 1);
}

// FtcDevEnum: read PID_OBJECT_TYPE of the object at _devObjProbe (to find the app-program + tables).
void FileTransferClient::ftcDevSendObjType()
{
    _propPending = false;
    _ftcSince = millis();
    SecurityControl sec = {false, None};
    knx.bau().ftcSendPropertyValueRead(_ftcTarget, sec, _devObjProbe, FTC_PID_OBJECT_TYPE, 1, 1);
}

/** @brief After enumeration, queue the app-version + load-state reads (order matches the report). */
void FileTransferClient::ftcDevBuildLoadQueue()
{
    _loadQN = 0;
    auto push = [&](int8_t obj, uint8_t pid) {
        if (obj >= 0 && _loadQN < sizeof(_loadQObj))
        {
            _loadQObj[_loadQN] = (uint8_t)obj;
            _loadQPid[_loadQN] = pid;
            _loadQN++;
        }
    };
    push(_devIdxApp, FTC_PID_PROG_VERSION);
    push(_devIdxApp, FTC_PID_LOAD_STATE);
    push(_devIdxAddr, FTC_PID_LOAD_STATE);
    push(_devIdxAssoc, FTC_PID_LOAD_STATE);
    push(_devIdxGrp, FTC_PID_LOAD_STATE);
}

void FileTransferClient::ftcDevSendLoad()
{
    _propPending = false;
    _ftcSince = millis();
    SecurityControl sec = {false, None};
    knx.bau().ftcSendPropertyValueRead(_ftcTarget, sec, _loadQObj[_loadQi], _loadQPid[_loadQi], 1, 1);
}

// PID_LOAD_STATE_CONTROL values (KNX load state machine).
const char *FileTransferClient::ftcLoadName(uint8_t state)
{
    switch (state)
    {
        case 0: return "unloaded";
        case 1: return "loaded";
        case 2: return "loading";
        case 3: return "error";
        case 4: return "unloading";
        case 5: return "load completing";
        default: return "?";
    }
}

void FileTransferClient::ftcDevReport()
{
    // Emit the whole ~18-line fingerprint through ftcOut (the cooperative drain), NOT straight to the logger:
    // 18 log lines in one pass flush to USB-CDC in a single burst (~110 ms) -> a "loop took longer" warning.
    // ftcDrainOut() spreads them across loop() per openknx.freeLoopTime(), so the report never stalls the loop.
    // Per-line color replaces the color()/color(0) bracketing: section headers keep CONSOLE_HEADLINE_COLOR.
    char title[32];
    snprintf(title, sizeof(title), "Device %u.%u.%u", (_ftcTarget >> 12) & 0x0F, (_ftcTarget >> 8) & 0x0F,
             _ftcTarget & 0xFF);
    // Replicate Logger::logHeader: "========================" (24) + " title " + '='-fill to width 55.
    char hdr[96];
    int hn = 0;
    while (hn < 24)
        hdr[hn++] = '=';
    hn += snprintf(hdr + hn, sizeof(hdr) - hn, " %s ", title);
    for (int tail = 54 - (int)strlen(title); tail > 0 && hn < (int)sizeof(hdr) - 1; tail--)
        hdr[hn++] = '=';
    hdr[hn] = '\0';
    ftcOut(CONSOLE_HEADLINE_COLOR, "%s", hdr);

    // --- Device section: identity read from the standard Device Object (index 0), ETS-panel order ---
    ftcOut(CONSOLE_HEADLINE_COLOR, "Device");
    if (_devHasMask)
        ftcOut(0, "  Mask version:   0x%04X  (%s)", _devMask, ftcMaskName(_devMask));
    if (_devHasSerial)
    {
        ftcOut(0, "  Manufacturer:   0x%04X", _devMfr);
        ftcOut(0, "  Serial number:  %02X%02X:%02X%02X%02X%02X", _devSerial[0], _devSerial[1], _devSerial[2],
               _devSerial[3], _devSerial[4], _devSerial[5]);
    }
    if (_devHasOrder)
    {
        // Order number is the device's own ASCII name field ("NeoPixel", "CK-StateEn").
        char txt[11] = {0};
        for (uint8_t i = 0; i < 10; i++)
            txt[i] = (_devOrder[i] >= 0x20 && _devOrder[i] < 0x7F) ? (char)_devOrder[i] : '.';
        ftcOut(0, "  Order number:   \"%s\"", txt);
    }
    if (_devHasHw)
    {
        // Hardware type is 0000 + application number (2) + version (2). The app number is the numeric
        // model id (ETS resolves it to a catalog name); e.g. 0xAD00 = NeoPixel, 0xAC0D = StateEngine.
        const uint16_t appNum = (uint16_t)((_devHw[2] << 8) | _devHw[3]);
        ftcOut(0, "  Hardware type:  %02X %02X %02X %02X %02X %02X  (app 0x%04X)", _devHw[0], _devHw[1],
               _devHw[2], _devHw[3], _devHw[4], _devHw[5], appNum);
    }
    if (_devHasFw && _devFwLen >= 2)
    {
        // PDT_VERSION: [magic:5][version:5][revision:6] -- ETS prints "[magic] version.revision".
        const uint16_t v = (uint16_t)((_devFw[0] << 8) | _devFw[1]);
        ftcOut(0, "  Version:        [%u] %u.%u", (v >> 11) & 0x1F, (v >> 6) & 0x1F, v & 0x3F);
    }
    if (_devHasProg)
        ftcOut(0, "  Prog mode:      %s", _devProgMode ? "ON" : "off");

    // --- Application program (ETS "Applikationsprogramm") ---
    if (_devHasApp || _devLoadHas[2])
    {
        ftcOut(CONSOLE_HEADLINE_COLOR, "Application program");
        if (_devHasApp)
            ftcOut(0, "  App / version:  mfr 0x%04X  app 0x%04X  v%u.%u", _devAppMfr, _devAppNum,
                   _devAppVer >> 4, _devAppVer & 0x0F);
        if (_devLoadHas[2])
            ftcOut(0, "  Load state:     %s", ftcLoadName(_devLoad[2]));
    }

    // --- Group communication (ETS "Gruppenkommunikation"): the table load states ---
    if (_devLoadHas[0] || _devLoadHas[1] || _devLoadHas[3])
    {
        ftcOut(CONSOLE_HEADLINE_COLOR, "Group communication");
        if (_devLoadHas[0]) ftcOut(0, "  Address table:  %s", ftcLoadName(_devLoad[0]));
        if (_devLoadHas[1]) ftcOut(0, "  Assoc. table:   %s", ftcLoadName(_devLoad[1]));
        if (_devLoadHas[3]) ftcOut(0, "  Object table:   %s", ftcLoadName(_devLoad[3]));
    }

    // --- File-Transfer section: is there a KnxFileTransfer server, and what can it do ---
    if (_devHasVer)
    {
        char feat[32] = {0};
        if (_devFeat & 0x1) strcat(feat, "Resume ");
        if (_devFeat & 0x2) strcat(feat, "Update ");
        if (_devFeat & 0x4) strcat(feat, "Fast ");
        if (_devFeat & 0x8) strcat(feat, "Console ");
        if (!feat[0]) strcpy(feat, "(none)");
        ftcOut(CONSOLE_HEADLINE_COLOR, "File-Transfer");
        ftcOut(0, "  Module version: %u.%u.%u", _devVerMaj, _devVerMin, _devVerRev);
        ftcOut(0, "  Features:       %s", feat);
    }
    else
    {
        ftcOut(0, "File-Transfer:    no answer (not a KnxFileTransfer device)");
    }
    ftcOut(0, "--------------------------------------------------------------------------------");

    _status.ok = _devHasMask || _devHasVer || _devHasSerial;
    ftcStatusMsg(_devHasVer ? "device info complete" : "device info (partial)");
}

/** @brief `ftc <pa> info ga`: reuse the device-info discovery to find the table objects, then A_Memory_Read the GA + association tables (the ETS path -- works on foreign classic devices too). */
void FileTransferClient::requestGroupComm(uint16_t pa)
{
    requestDeviceInfo(pa);                       // run the whole DeviceDescriptor -> object-enum discovery chain
    knx.bau().ftcSetMemoryCallback(ftcOnMemory); // + arm the A_Memory_Read answer path for the table walk
    _gaMode = true;                              // FtcDevEnum branches to the table walk instead of the load-state reads
    _gaWhich = 0;
    _gaN = 0;
    _gaRef = _gaGot = _gaExpect = 0;
    _gaAssocValid = false;
    _gaConnected = false; // no CO link yet -- ftcGaBeginWalk opens one once the enumeration finishes
    _memPending = false;
    _memLen = 0;
    _ftcRespT = 0; // arm the memory-answer duplicate guard (ftcDropDup) fresh for this walk
    ftcStatusMsg("querying group communication...");
}

/**
 * @brief Open a point-to-point T_Connect for the memory walk, then start it. ETS-parity path.
 *
 * ETS reads the GA/association tables over a CONNECTION-ORIENTED transport. Foreign System B devices
 * (e.g. Jung/Gira) answer PropertyValue_Read connectionless but IGNORE a connectionless A_Memory_Read, so
 * the old connectionless walk timed out on them (it only worked against the permissive OpenKNX stack).
 * With a T_Connect open, individualSend() auto-routes every subsequent read on the connection (CO) -- no
 * per-read change needed. ftcScanConnect self-guards (no-op if a connection is already up, e.g. an ETS
 * session); if it cannot open one we fall back to the connectionless walk (still fine for OpenKNX targets).
 */
void FileTransferClient::ftcGaBeginWalk()
{
    _gaConnected = false;
    if (knx.bau().ftcScanConnect(_ftcTarget))
    {
        _ftcSince = millis();
        _ftcState = FtcGaConnect; // wait for the link, then walk connection-oriented
        return;
    }
    ftcGaAdvance(); // a connection is already open (not ours to reuse) -> connectionless fallback walk
}

void FileTransferClient::ftcGaAdvance()
{
    // Send the next present table's PID_TABLE_REFERENCE read (-> FtcGaRef), skipping absent tables; when
    // both the address (0) and association (1) tables are done, emit the report and finish.
    for (; _gaWhich <= 1; _gaWhich++)
    {
        const int8_t idx = (_gaWhich == 0) ? _devIdxAddr : _devIdxAssoc;
        if (idx < 0) continue; // this table object was not discovered on the device
        _propPending = false;
        _ftcSince = millis();
        SecurityControl sec{false, None};
        // Connection-oriented: the walk opened a T_Connect first, so this read routes over it (ETS path).
        knx.bau().ftcSendPropertyValueRead(_ftcTarget, sec, (uint8_t)idx, FTC_PID_TABLE_REFERENCE, 1, 1);
        _ftcState = FtcGaRef;
        return;
    }
    ftcGaReport();
    ftcFinish();
}

void FileTransferClient::ftcGaParse()
{
    // Extract the table just accumulated in _memBuf. Address blob: [count:2]([ga:2])*count -> _gaList.
    // Association blob: [count:2]([TSAP:2][ASAP:2])*count -> left in _memBuf, read by the report (walked last).
    if (_gaGot < 2) return; // no leading count
    if (_gaWhich == 0)
    {
        const uint16_t count = (uint16_t)((_memBuf[0] << 8) | _memBuf[1]);
        _gaN = 0;
        for (uint16_t i = 0; i < count && _gaN < FTC_GA_MAX; i++)
        {
            const uint16_t off = (uint16_t)(2 + i * 2);
            if ((uint32_t)off + 2 > _gaGot) break; // only parse bytes we actually read
            _gaList[_gaN++] = (uint16_t)((_memBuf[off] << 8) | _memBuf[off + 1]);
        }
    }
    else
    {
        _gaAssocValid = true; // the report resolves the [TSAP,ASAP] pairs straight from _memBuf
    }
}

void FileTransferClient::ftcGaReport()
{
    ftcOut(CONSOLE_HEADLINE_COLOR, "Group communication %u.%u.%u", (_ftcTarget >> 12) & 0x0F,
           (_ftcTarget >> 8) & 0x0F, _ftcTarget & 0xFF);

    // --- Group addresses (from the address table), wrapped so a long list never overruns one ftcOut line ---
    if (_gaN == 0)
        ftcOut(0, "  Group addresses: (none)");
    else
    {
        char line[120];
        int n = snprintf(line, sizeof(line), "  Group addresses:");
        for (uint16_t i = 0; i < _gaN; i++)
        {
            char one[16];
            snprintf(one, sizeof(one), " %u/%u/%u", (_gaList[i] >> 11) & 0x1F, (_gaList[i] >> 8) & 0x07, _gaList[i] & 0xFF);
            if (n + (int)strlen(one) + 1 >= (int)sizeof(line))
            {
                ftcOut(0, "%s", line);
                n = snprintf(line, sizeof(line), "   ");
            }
            n += snprintf(line + n, sizeof(line) - n, "%s", one);
        }
        ftcOut(0, "%s", line);
    }

    // --- Com-object links (association table), grouped by com-object number (ASAP) ---
    if (_gaAssocValid && _gaGot >= 2)
    {
        const uint16_t bytes = (_gaGot < _gaExpect) ? _gaGot : _gaExpect; // valid accumulated length
        const uint16_t count = (uint16_t)((_memBuf[0] << 8) | _memBuf[1]);
        for (uint16_t i = 0; i < count; i++)
        {
            const uint16_t oi = (uint16_t)(2 + i * 4);
            if ((uint32_t)oi + 4 > bytes) break;
            const uint16_t asap = (uint16_t)((_memBuf[oi + 2] << 8) | _memBuf[oi + 3]);
            // Print each com-object once: skip if an earlier entry already carried this ASAP.
            bool seen = false;
            for (uint16_t j = 0; j < i; j++)
            {
                const uint16_t oj = (uint16_t)(2 + j * 4);
                if (((_memBuf[oj + 2] << 8) | _memBuf[oj + 3]) == asap)
                {
                    seen = true;
                    break;
                }
            }
            if (seen) continue;
            char line[120];
            int n = snprintf(line, sizeof(line), "  KO %u:", asap);
            for (uint16_t j = i; j < count; j++)
            {
                const uint16_t oj = (uint16_t)(2 + j * 4);
                if ((uint32_t)oj + 4 > bytes) break;
                if (((_memBuf[oj + 2] << 8) | _memBuf[oj + 3]) != asap) continue;
                const uint16_t tsap = (uint16_t)((_memBuf[oj] << 8) | _memBuf[oj + 1]);
                char one[16];
                // TSAP is 1-based into the address table (getGroupAddress: [0]=count) -> _gaList[tsap-1].
                if (tsap >= 1 && (uint16_t)(tsap - 1) < _gaN)
                    snprintf(one, sizeof(one), " %u/%u/%u", (_gaList[tsap - 1] >> 11) & 0x1F,
                             (_gaList[tsap - 1] >> 8) & 0x07, _gaList[tsap - 1] & 0xFF);
                else
                    snprintf(one, sizeof(one), " [t%u]", tsap);
                if (n + (int)strlen(one) + 1 >= (int)sizeof(line))
                {
                    ftcOut(0, "%s", line);
                    n = snprintf(line, sizeof(line), "    ");
                }
                n += snprintf(line + n, sizeof(line) - n, "%s", one);
            }
            ftcOut(0, "%s", line);
        }
    }
    ftcOut(0, "------------------------------------------------------------------------------");
    _status.ok = (_gaN > 0) || _gaAssocValid;
    ftcStatusMsg("group communication read");
}

void FileTransferClient::requestUpload(uint16_t pa, const char *src, unsigned pkg, bool noResume, uint8_t mode, bool apply)
{
    // pkg omitted (0) or `auto` -> adaptive: start at the fastest frame, degrade per fresh link-retry.
    if (pkg == 0)
    {
        _ftcPkgAuto = true;
        pkg = FTC_PKG_MAX;
    }
    else
        _ftcPkgAuto = false;
    if (pkg < FTC_PKG_MIN || pkg > FTC_PKG_MAX)
    {
        openknx.logger.logWithPrefixAndValues("FTC", "pkg %u out of range (%u..%u)", pkg, FTC_PKG_MIN, FTC_PKG_MAX);
        return;
    }
    if (strlen(src) >= sizeof(_ftcPath))
    {
        openknx.logger.logWithPrefixAndValues("FTC", "path too long (max %u)", (unsigned)(sizeof(_ftcPath) - 1));
        return;
    }

    _ftcResume = false;      // FtcResumeInfo decides resume/truncate/skip; noResume forces a fresh upload
    _ftcNoResume = noResume; // send `no-resume`: discard any target partial (truncate)
    _ftcStartSeq = 1;
    _ftcResumeBase = 0;
    _ftcSrcCrc = 0;
    _ftcDupes = 0;
    _ftcTransferRetries = 0; // fresh request -> auto-retry budget resets here (ftcStart, shared with retries, must NOT touch it)
    _ftcRetryPending = false;
    _ftcRetryLostMs = 0;
    _ftcGrandStartMs = 0; // end-to-end clock/byte-base re-armed at the first open
    _ftcGrandElapsedMs = 0;
    _ftcGrandResumeBase = 0;
    _ftcLastProgressMs = 0;   // stale value from a prior transfer must not be charged as this one's dead time
    _ftcData100Ms = 0;        // "all payload delivered" marker -> the pure-transfer clock stop
    _ftcSpaceChecked = false; // re-run the pre-upload free-space gate for this request
    _ftcTargetHave = 0;       // existing target file size, re-learned by FtcResumeInfo
    // Fast DATA frames carry a trailing CRC16 (2 B) -> reserve them: fast payload = pkg-8, classic = pkg-6
    // (same wire frame size). A fast request later downgraded to classic just runs 2 payload bytes short.
    _ftcPayloadSize = (uint8_t)(pkg - FTC_PKG_OVERHEAD - (mode != 0 ? 2 : 0));
    _ftcPayloadBase = _ftcPayloadSize; // pkg-auto degrade baseline for this request
    strncpy(_ftcPath, src, sizeof(_ftcPath) - 1);
    _ftcPath[sizeof(_ftcPath) - 1] = '\0';
    _ftcTestSource = (strcmp(src, "test") == 0);
    _ftcApply = apply; // opt-in self-apply (forced false below for the test pattern)
    _activeSrc = nullptr;
    _ftcSize = 0;

    if (!_ftcTestSource)
    {
        // Resolve the local backend by the path's prefix ("sd/..", "efc/..", or the LittleFS default),
        // then strip it so _ftcPath is the bare path for BOTH the local open and the remote target.
        const char *stripped = _ftcPath;
        const FtcBackend *be = ftcResolveBackend(_ftcPath, &stripped);
        if (stripped != _ftcPath) memmove(_ftcPath, stripped, strlen(stripped) + 1);
        // LittleFS + the remote target need an absolute path -> normalise a bare "name" to "/name".
        if (_ftcPath[0] != '/')
        {
            const size_t pl = strlen(_ftcPath);
            if (pl + 2 > sizeof(_ftcPath))
            {
                openknx.logger.logWithPrefix("FTC", "path too long");
                return;
            }
            memmove(_ftcPath + 1, _ftcPath, pl + 1);
            _ftcPath[0] = '/';
        }
        // A NAMED source that will not resolve/open must NEVER silently become a test pattern -- that could
        // push a garbage ramp where a firmware was meant. Abort with a hint; only `send test`/`perf` send a pattern.
        if (be == nullptr || be->src.open == nullptr)
        {
            openknx.logger.logWithPrefixAndValues("FTC", "cannot upload '%s': unknown backend -- use sd/ or efc/ (or / for internal flash). Aborting.", _ftcPath);
            return;
        }
        if (be->available && !be->available())
        {
            openknx.logger.logWithPrefixAndValues("FTC", "backend '%s' not available (no card / not mounted) -- aborting", be->prefix);
            return;
        }
        _activeSrc = &be->src;
        const int32_t sz = _activeSrc->open(_ftcPath);
        if (sz < 0)
        {
            openknx.logger.logWithPrefixAndValues("FTC", "cannot open source '%s%s' -- aborting (NOT sending a test pattern; is the file on sd/ or efc/?)",
                                                  (be->prefix && be->prefix[0]) ? be->prefix : "", _ftcPath);
            ftcCloseSource();
            return;
        }
        _ftcSize = (uint32_t)sz;
    }

    if (_ftcTestSource)
    {
        // Retarget the remote name too: writing a generated pattern over the real firmware path on
        // the target would be worse than useless.
        strncpy(_ftcPath, "/ftctest.bin", sizeof(_ftcPath) - 1);
        _ftcPath[sizeof(_ftcPath) - 1] = '\0';
        _ftcSize = FTC_TEST_SIZE;
        _ftcApply = false; // never flash a generated pattern
    }

    if (_ftcSize == 0)
    {
        openknx.logger.logWithPrefix("FTC", "source is empty -- nothing to send");
        ftcCloseSource();
        return;
    }

    const uint32_t chunks = (_ftcSize + _ftcPayloadSize - 1) / _ftcPayloadSize;
    if (chunks > 0xFFFE) // sequence is 16 bit and 0 / 0xFFFF are reserved for open / close
    {
        openknx.logger.logWithPrefixAndValues("FTC", "file needs %u chunks -- raise pkg (max %u)",
                                              (unsigned)chunks, FTC_PKG_MAX);
        ftcCloseSource();
        return;
    }
    _ftcChunks = (uint16_t)chunks;

    _ftcPerfKeep = false; // not a perf run
    char fr[56], opt[40];
    snprintf(fr, sizeof(fr), "pkg %u%s   -   %u B/chunk   -   %u chunks", pkg, _ftcPkgAuto ? " (auto)" : "",
             (unsigned)_ftcPayloadSize, (unsigned)_ftcChunks);
    snprintf(opt, sizeof(opt), "resume = %s%s", _ftcNoResume ? "no (fresh)" : "auto", _ftcApply ? "   apply" : "");
    ftcConfigBox(pa, "Upload", _ftcTestSource ? "generated test pattern" : src, _ftcPath, _ftcSize, false, 0,
                 mode == 0 ? "safe (classic)" : ftcModeName(mode), fr, opt);
    ftcStart(pa, true, mode);
}

/**
 * @brief `ftc <pa> perf [kb] [pkg]`: push a generated pattern and report the measured B/s.
 *
 * Same upload path, but the source is the built-in RAM ramp (no SD) writing to /ftcperf.bin, so it
 * never touches a real firmware path on the target.
 */
void FileTransferClient::requestPerf(uint16_t pa, uint32_t sizeBytes, unsigned pkg, uint8_t mode, bool keep)
{
    if (pkg == 0)
    {
        _ftcPkgAuto = true;
        pkg = FTC_PKG_MAX;
    } // omitted/`auto` = adaptive: start at the fastest frame
    else
        _ftcPkgAuto = false;
    if (pkg < FTC_PKG_MIN || pkg > FTC_PKG_MAX)
    {
        openknx.logger.logWithPrefixAndValues("FTC", "pkg %u out of range (%u..%u)", pkg, FTC_PKG_MIN, FTC_PKG_MAX);
        return;
    }
    if (sizeBytes == 0) sizeBytes = 16u * 1024u;

    _ftcResume = false;
    _ftcNoResume = false; // perf always writes a fresh file
    _ftcStartSeq = 1;
    _ftcResumeBase = 0;
    _ftcSrcCrc = 0;
    _ftcDupes = 0;
    _ftcTransferRetries = 0; // fresh request -> auto-retry budget resets here (ftcStart, shared with retries, must NOT touch it)
    _ftcRetryPending = false;
    _ftcRetryLostMs = 0;
    _ftcGrandStartMs = 0; // end-to-end clock/byte-base re-armed at the first open
    _ftcGrandElapsedMs = 0;
    _ftcGrandResumeBase = 0;
    _ftcLastProgressMs = 0;   // stale value from a prior transfer must not be charged as this one's dead time
    _ftcData100Ms = 0;        // "all payload delivered" marker -> the pure-transfer clock stop
    _ftcSpaceChecked = false; // re-run the pre-upload free-space gate for this request
    _ftcTargetHave = 0;       // existing target file size, re-learned by FtcResumeInfo
    // Fast payload = pkg-8 (2 B reserved for the in-frame CRC16); classic = pkg-6. mode 0 unchanged.
    _ftcPayloadSize = (uint8_t)(pkg - FTC_PKG_OVERHEAD - (mode != 0 ? 2 : 0));
    _ftcPayloadBase = _ftcPayloadSize; // pkg-auto degrade baseline for this request
    _ftcTestSource = true;
    _ftcApply = false;    // perf pattern is throwaway -> never flash it
    _activeSrc = nullptr; // pure RAM pattern, no backend source
    _ftcSize = sizeBytes;

    const uint32_t chunks = (_ftcSize + _ftcPayloadSize - 1) / _ftcPayloadSize;
    if (chunks > 0xFFFE)
    {
        openknx.logger.logWithPrefixAndValues("FTC", "perf size needs %u chunks -- raise pkg or lower kb",
                                              (unsigned)chunks);
        return;
    }
    _ftcChunks = (uint16_t)chunks;

    // The perf pattern is a deterministic ramp -> CRC it (same generator + xorout as the transfer/verify)
    // COOPERATIVELY (FtcCrcPrefix) so a large size can't block loop(); ftcPerfCrcDone then names the file,
    // prints the config box and starts the transfer.
    _crcPerfPa = pa;
    _crcPerfMode = mode;
    _crcPerfKeep = keep;
    _crcPerfPkg = (uint16_t)pkg;
    ftcStartCrc(_ftcSize, 0xFFFFFFFFu, 0, 1);
}

void FileTransferClient::ftcCloseSink()
{
    if (_dlSinkOpen && _activeSink && _activeSink->close != nullptr) _activeSink->close();
    _dlSinkOpen = false;
}

// FileDownload open: [0x00][0x00][pkg][path\0]. The answer is [0x00][size:4 BE][0x00].
void FileTransferClient::ftcDlSendOpen()
{
    const size_t np = strlen(_ftcPath) + 1; // path incl. NUL
    _ftcTx[0] = 0x00;
    _ftcTx[1] = 0x00;
    _ftcTx[2] = _dlPayload;
    memcpy(_ftcTx + 3, _ftcPath, np);
    ftcSend(FTC_CMD_FILE_DOWNLOAD, (uint8_t)(3 + np));
}

// FileDownload chunk request: [seqLo][seqHi] (the server reads it little-endian).
void FileTransferClient::ftcDlSendChunk()
{
    _ftcTx[0] = (uint8_t)(_dlSeq & 0xFF);
    _ftcTx[1] = (uint8_t)(_dlSeq >> 8);
    ftcSend(FTC_CMD_FILE_DOWNLOAD, 2);
}

/** @brief Framed DOWNLOAD result box (mirrors the upload box) + the Verify line, queued via ftcOut.
 *  statOk=false => the target didn't answer FileInfo (old/silent server) -> "unverified"; ok => size + CRC32 both matched. */
void FileTransferClient::ftcDownloadPanel(bool ok, bool statOk, uint32_t tcrc)
{
    const uint32_t mine = _dlCrc ^ 0xFFFFFFFFu; // xorout, see ftcCrc32Posix()
    const uint32_t el = _dlEndMs - _dlStartMs;  // pure-transfer time (frozen at the last chunk)
    const uint32_t bps = el ? (uint32_t)(((uint64_t)_dlWritten * 1000ULL) / el) : 0;
    _status.bps = (uint16_t)(bps > 0xFFFF ? 0xFFFF : bps);
    char row[80];
    ftcBoxRule('=');
    snprintf(row, sizeof(row), "  DOWNLOAD %s   <- %u.%u.%u", ok ? "COMPLETE" : "FAILED  ",
             (_ftcTarget >> 12) & 0x0F, (_ftcTarget >> 8) & 0x0F, _ftcTarget & 0xFF);
    ftcBoxRow(row);
    ftcBoxRule('-');
    snprintf(row, sizeof(row), "  Bytes    %u   (%u chunks)", (unsigned)_dlWritten, (unsigned)_dlChunks);
    ftcBoxRow(row);
    const uint32_t s = el / 1000;
    snprintf(row, sizeof(row), "  Time     %02um%02us      avg  %u B/s     peak %u",
             (unsigned)(s / 60), (unsigned)(s % 60), (unsigned)bps, (unsigned)_ftcPeakBps);
    ftcBoxRow(row);
    if (!statOk)
        snprintf(row, sizeof(row), "  Verify   NO ANSWER -- file written, unverified");
    else if (ok)
        snprintf(row, sizeof(row), "  Verify   OK   size %u   crc32 0x%08X", (unsigned)_dlWritten, (unsigned)mine);
    else
        snprintf(row, sizeof(row), "  Verify   FAILED   crc 0x%08X vs 0x%08X", (unsigned)mine, (unsigned)tcrc);
    ftcBoxRow(row);
    ftcBoxRule('=');
}

// `ftc <pa> receive|download <remote> [local]`: pull a file off the target's filesystem onto the local sink (SD).
void FileTransferClient::requestDownload(uint16_t pa, const char *remotePath, const char *localPath, uint8_t pkg)
{
    // Client-requested download chunk size (goes into the FileDownload-open request, honored by the server).
    // pkg 0 or out of range -> the default FTC_DL_PAYLOAD; smaller helps on links that can't carry big frames.
    _dlPayload = (pkg >= 16 && pkg <= FTC_DL_PAYLOAD) ? pkg : FTC_DL_PAYLOAD;
    // Resolve the LOCAL destination's backend by its prefix; the stripped path is what we actually write.
    const char *stripped = localPath;
    const FtcBackend *be = ftcResolveBackend(localPath, &stripped);
    if (be == nullptr || be->sink.open == nullptr || be->sink.write == nullptr) // need BOTH to write the sink
    {
        openknx.logger.logWithPrefix("FTC", "unknown path backend (no SD?) -- cannot download");
        return;
    }
    if (be->available && !be->available()) // fail fast, before bothering the target
    {
        openknx.logger.logWithPrefixAndValues("FTC", "backend '%s' not available (no card / not mounted)", be->prefix);
        return;
    }
    _activeSink = &be->sink;
    _dlBackend = be; // remembered for the free-space gate + open-failure message in FtcDownloadOpen
    if (strlen(remotePath) >= sizeof(_ftcPath) || strlen(stripped) >= sizeof(_dlLocal))
    {
        openknx.logger.logWithPrefix("FTC", "path too long");
        return;
    }
    strncpy(_ftcPath, remotePath, sizeof(_ftcPath) - 1);
    _ftcPath[sizeof(_ftcPath) - 1] = 0;
    strncpy(_dlLocal, stripped, sizeof(_dlLocal) - 1); // prefix stripped -> the real sink path
    _dlLocal[sizeof(_dlLocal) - 1] = 0;

    knx.bau().ftcSetResponseCallback(ftcOnResponse);
    _ftcTarget = pa;
    _dlSize = 0;
    _dlChunks = 0;
    _dlSeq = 0;
    _dlWritten = 0;
    _dlSinkOpen = false;
    _ftcRetries = 0;
    _ftcRespPending = false;
    _ftcRespT = 0;
    _dlStartMs = 0;
    _dlEndMs = 0;
    _dlCrc = 0;         // fresh CRC32 fold over the received stream (finalized + compared in FtcDownloadVerify)
    _ftcPeakBps = 0;    // download doesn't call ftcStart() -> reset the per-transfer panel/rate state here
    _ftcLastProgMs = 0; // else the DOWNLOAD panel would print a stale peak + the first interval rate is off
    _ftcLastProgDone = 0;
    _ftcUpload = false; // a download error path must not misfire ftcAbort()'s upload-retry

    ftcStatusReset(FtcPhase::Upload, pa, _ftcPath); // reuse Upload phase for the progress bar
    _ftcNextPct = 10;                               // arm the per-decile download progress line
    // CONFIG box: download has no size/mode/framing/options at start -> Source = remote, Target = local only.
    ftcConfigBox(pa, "Download", _ftcPath, _dlLocal, 0, false, 0, nullptr, nullptr, nullptr);
    ftcDlSendOpen();
    _ftcState = FtcDownloadOpen;
    _ftcSince = millis();
}

// `ftc <pa> fwupdate <remote>`: trigger the target to apply an already-uploaded firmware (reboots it).
void FileTransferClient::requestFwUpdate(uint16_t pa, const char *remotePath)
{
    if (strlen(remotePath) >= sizeof(_ftcPath))
    {
        openknx.logger.logWithPrefix("FTC", "path too long");
        return;
    }
    knx.bau().ftcSetResponseCallback(ftcOnResponse);
    _ftcTarget = pa;
    strncpy(_ftcPath, remotePath, sizeof(_ftcPath) - 1);
    _ftcPath[sizeof(_ftcPath) - 1] = 0;
    _ftcRespPending = false;
    ftcStatusReset(FtcPhase::Info, pa, _ftcPath);
    ftcStatusMsg("checking the file before apply...");

    // Existence pre-check first (FtcApplyCheck): only reboot the target if the file is really there.
    const size_t n = strlen(_ftcPath) + 1; // payload = filename incl. NUL
    memcpy(_ftcTx, _ftcPath, n);
    openknx.logger.logWithPrefixAndValues("FTC", "fwupdate -> %u.%u.%u \"%s\": checking file exists...",
                                          (pa >> 12) & 0x0F, (pa >> 8) & 0x0F, pa & 0xFF, _ftcPath);
    if (ftcSend(FTC_CMD_FILE_INFO, (uint8_t)n))
        _ftcState = FtcApplyCheck;
    else
        openknx.logger.logWithPrefix("FTC", "send failed");
}

void FileTransferClient::loop(bool configured)
{
    // Cooperative console output first -- even when idle: the ll/ls/df footer is queued right before
    // ftcFinish() sets FtcIdle, so it must still drain to completion here.
    if (ftcDrainOut()) return;

    // Locate-blink (ftc <pa> led blink): toggle the target's prog-LED from loop() while idle. The FtcIdle
    // guard means it never injects a frame mid-transfer; a quick read (list/info) just pauses then resumes it.
    if (_ledBlinkPa && _ftcState == FtcIdle && millis() >= _ledBlinkNext)
    {
        _ledBlinkOn ^= 1;
        SecurityControl sec{false, None};
        knx.bau().ftcSendPropertyValueWrite(_ledBlinkPa, sec, 0, FTC_PID_PROGMODE, 1, 1, &_ledBlinkOn, 1);
        _ledBlinkNext = millis() + FTC_LED_BLINK_MS;
    }

    if (_ftcState == FtcIdle)
        return;

    switch (_ftcState)
    {

        case FtcFeatureProbe:
        {
            // fast/forget pre-flight. Accept only the CheckFeatures echo (a mirror can land here), cache a
            // real answer per-PA, gate, join the classic path; no answer in the short window = old server.
            if (_ftcRespPending)
            {
                _ftcRespPending = false;
                if (_ftcRespProp != FTC_CMD_CHECK_FEATURES) return; // stale mirror -> keep waiting
                const uint8_t feat = (_ftcRespLen >= 1) ? _ftcResp[0] : 0;
                _ftcFeatPa = _ftcTarget;
                _ftcFeatBits = feat;
                _ftcFeatValid = true; // cache only a real answer
                ftcGateFast(feat, true);
                ftcSendInfo();
                _ftcState = FtcResumeInfo;
            }
            else if (millis() - _ftcSince > FTC_FEATURE_TIMEOUT)
            {
                // No answer in 800ms -> old server. Do NOT cache (may be transient); run classic.
                ftcGateFast(0, false);
                ftcSendInfo();
                _ftcState = FtcResumeInfo;
            }
            return;
        }

#ifdef OPENKNX_FTC_SECURITY
        case FtcAuthProbe:
        {
            // login pre-flight: CheckFeatures tells us whether the target is password-protected at all.
            if (_ftcRespPending)
            {
                _ftcRespPending = false;
                if (_ftcRespProp != FTC_CMD_CHECK_FEATURES) return; // stale mirror -> keep waiting
                const uint8_t feat = (_ftcRespLen >= 1) ? _ftcResp[0] : 0;
                _ftcFeatPa = _ftcTarget;
                _ftcFeatBits = feat;
                _ftcFeatValid = true; // cache only a real answer
                authAfterProbe(feat, true);
            }
            else if (millis() - _ftcSince > FTC_FEATURE_TIMEOUT)
            {
                authAfterProbe(0, false); // no answer in the short window -> old / non-auth device
            }
            return;
        }

        case FtcAuthChallenge:
        {
            // login step 1: got the 16-byte nonce -> compute the 4-byte MAC locally and send AuthResponse(104).
            // The password (as _ftcAuthKey) never leaves this process; only the MAC goes on the wire.
            if (_ftcRespPending)
            {
                _ftcRespPending = false;
                if (_ftcRespProp != FTC_CMD_AUTH_CHALLENGE) return; // stale mirror -> keep waiting
                if (_ftcRespLen < 17 || _ftcResp[0] != 0x00)
                {
                    openknx.logger.logWithPrefix("FTC", "login: target sent no nonce");
                    memset(_ftcAuthKey, 0, sizeof(_ftcAuthKey));
                    ftcFinish();
                    return;
                }
                uint8_t mac[16]; // MAC = first SEC_MAC_LEN bytes of AES_ECB(pad16(pw), nonce); nonce = _ftcResp[1..16]
                memcpy(mac, _ftcResp + 1, 16);
                struct AES_ctx c;
                AES_init_ctx(&c, _ftcAuthKey);
                AES_ECB_encrypt(&c, mac);
                memcpy(_ftcTx, mac, SEC_MAC_LEN);
                memset(_ftcAuthKey, 0, sizeof(_ftcAuthKey)); // wipe the key as soon as it is used
                if (ftcSend(FTC_CMD_AUTH_RESPONSE, SEC_MAC_LEN))
                    _ftcState = FtcAuthResponse;
                else
                    ftcFinish();
            }
            else if (millis() - _ftcSince > FTC_TIMEOUT)
            {
                openknx.logger.logWithPrefix("FTC", "login: no answer within 6s");
                memset(_ftcAuthKey, 0, sizeof(_ftcAuthKey));
                ftcFinish();
            }
            return;
        }

        case FtcAuthResponse:
        {
            // login step 2 / logout: the target's status byte. 104: 0x00 authorized / 0xA1 failed. 105: 0x00 done.
            if (_ftcRespPending)
            {
                _ftcRespPending = false;
                const uint8_t want = _ftcLogout ? FTC_CMD_AUTH_LOGOUT : FTC_CMD_AUTH_RESPONSE;
                if (_ftcRespProp != want) return; // stale mirror -> keep waiting
                const uint8_t r = (_ftcRespLen >= 1) ? _ftcResp[0] : 0xFF;
                if (_ftcLogout)
                    openknx.logger.logWithPrefix("FTC", "logged out");
                else if (r == 0x00)
                    openknx.logger.logWithPrefix("FTC", "login OK -- writes allowed until the target's idle timeout");
                else
                {
                    // 0xA1 auth failed. Bytes 1..2 (if present) carry the remaining brute-force back-off in
                    // seconds: 0 = still within the 3 free tries, else "too many tries, wait N".
                    const uint16_t backSec = (_ftcRespLen >= 3) ? (uint16_t)((_ftcResp[1] << 8) | _ftcResp[2]) : 0;
                    if (backSec == 0)
                        openknx.logger.logWithPrefix("FTC", "login FAILED -- wrong password, try again");
                    else if (backSec < 60)
                        openknx.logger.logWithPrefixAndValues("FTC", "login FAILED -- too many tries, next attempt in %u s", backSec);
                    else
                        openknx.logger.logWithPrefixAndValues("FTC", "login FAILED -- too many tries, next attempt in %u min", (backSec + 59) / 60);
                }
                ftcFinish();
            }
            else if (millis() - _ftcSince > FTC_TIMEOUT)
            {
                openknx.logger.logWithPrefix("FTC", _ftcLogout ? "logout: no answer within 6s" : "login: no answer within 6s");
                ftcFinish();
            }
            return;
        }
#endif

        // ===== FAST TRANSFER states (phase 2 windowed + phase 3 forget) =====================
        case FtcFastOpen:
        {
            // cmd44 open ack: 0x00 ok, 0x42 open-fail, 0x4A too-many-chunks (pre-gated, so unexpected).
            if (_ftcRespPending)
            {
                _ftcRespPending = false;
                if (_ftcRespProp != FTC_CMD_FILE_UPLOAD_FAST) return; // stale mirror -> keep waiting
                const uint8_t r = (_ftcRespLen >= 1) ? _ftcResp[0] : 0xFF;
                if (r == 0x4A)
                {
                    ftcAbort("fast open: target reports too many chunks");
                    return;
                }
                if (r != 0x00)
                {
                    ftcAbort("target refused fast open");
                    return;
                }
                if (_ftcRecovering)
                    // Recovery: the data is already on the target, skip streaming and report the page.
                    ftcSendReport(_ftcReportBase, (uint16_t)(_ftcWndEnd - _ftcReportBase));
                else
                    _ftcState = FtcFastStream; // begin streaming the first window
            }
            else if (millis() - _ftcSince > FTC_TIMEOUT)
                ftcAbort("no answer to fast open");
            return;
        }

        case FtcFastStream:
        {
            // The pump. Burst-capped per loop() and gated on the TP FIFO high-water (yield when near full,
            // never block). Streams [_ftcNextSeq, _ftcWndEnd); DATA frames are SILENT (no response armed).
            if (millis() > _ftcDeadline)
            {
                ftcAbort("fast: overall deadline exceeded");
                return;
            }
            // forget (mode 2): rate-cap the fire-hose (no TP-FIFO backpressure over IP) -- yield until the
            // paced deadline, a small burst re-arms the gate. Windowed mode 1 keeps its full burst.
            if (_ftcMode == 2 && millis() < _ftcPaceNext) return;
            const uint8_t cap = (_ftcMode == 2) ? FTC_FORGET_BURST
                                                : (_ftcTestSource ? FTC_FAST_BURST_RAM : FTC_FAST_BURST_SD);
            uint8_t fired = 0;
            for (uint8_t b = 0; b < cap; b++)
            {
                if (_ftcNextSeq >= _ftcWndEnd) break;        // window / whole-file high edge reached
                if (ftcTxQueueSize() >= FTC_TX_HIGH) return; // FIFO near full -> yield, resume next loop()
                ftcSendFastData(_ftcNextSeq);
                _ftcNextSeq++;
                fired++;
            }
            if (_ftcMode == 2 && fired > 0) _ftcPaceNext = millis() + FTC_FORGET_PACE_MS;
            if (_ftcNextSeq >= _ftcWndEnd)
            {
                // Window fully queued. Wait for the FIFO to drain below LOW so the report/close reflects
                // what the SERVER received, not frames still sitting in our transmit queue.
                if (ftcTxQueueSize() < FTC_TX_LOW)
                {
                    if (_ftcMode == 2)
                    {
                        _ftcReportRetries = 0; // arm the bounded close-retry (FtcFastClose)
                        ftcSendFastClose();    // forget: whole file streamed -> close -> verify
                    }
                    else
                        ftcSendReport(_ftcReportBase, (uint16_t)(_ftcWndEnd - _ftcReportBase)); // windowed: report
                }
                // else: stay in FtcFastStream; next loop() re-checks the drain
            }
            return;
        }

        case FtcFastReport:
        {
            if (_ftcRespPending)
            {
                _ftcRespPending = false;
                if (_ftcRespProp != FTC_CMD_FILE_REPORT) return; // stale (report vs FileInfo 9-byte collision, M6)
                if (_ftcRespLen < 6) return;                     // malformed header
                if (_ftcResp[0] != 0x00)
                {
                    ftcAbort("report: target has no open file");
                    return;
                }
                const uint16_t base = (uint16_t)((_ftcResp[1] << 8) | _ftcResp[2]); // BE echoes
                const uint16_t count = (uint16_t)((_ftcResp[3] << 8) | _ftcResp[4]);
                const uint8_t nonce = _ftcResp[5];
                if (base != _ftcReportBase || nonce != _ftcReportNonce) return; // stale report (C4)
                if (ftcDropDup()) return;                                       // late IP-mirror of THIS report
                // A valid matching answer proves the query channel works -> only CONSECUTIVE unanswered
                // reports may reach the "unanswered" abort.
                const uint16_t dbgRetr = _ftcReportRetries;                                    // [dbg] timeouts before this answer (0 = answered on the first query)
                const uint32_t dbgWait = _ftcRepWaitStart ? (millis() - _ftcRepWaitStart) : 0; // [dbg] first-query -> answer latency for this window/round
                _ftcReportRetries = 0;
                const uint16_t bmpBytes = (uint16_t)((count + 7) / 8);
                if ((uint16_t)(6 + bmpBytes) > _ftcRespLen) return; // truncated bitmap
                // Merge the received bits into the absolute client mirror (bounds-checked).
                for (uint16_t i = 0; i < count; i++)
                {
                    if (_ftcResp[6 + (i >> 3)] & (1u << (i & 7)))
                    {
                        const uint32_t s = (uint32_t)base + i; // 1-based
                        if (s >= 1 && (s - 1) / 8 < sizeof(_ftcRecvBmp))
                            _ftcRecvBmp[(s - 1) >> 3] |= (uint8_t)(1u << ((s - 1) & 7));
                    }
                }
                // Missing over [_ftcReportBase, _ftcWndEnd) -- the frozen window edge is already EOF-clamped.
                uint16_t missing = 0;
                for (uint16_t s = _ftcReportBase; s < _ftcWndEnd; s++)
                    if (!(_ftcRecvBmp[(s - 1) >> 3] & (1u << ((s - 1) & 7)))) missing++;

                // [dbg] per-window report-gap breakdown: how long the target took to answer + how many 4 s
                // report timeouts elapsed first. A ~13 s gap = dbgWait ~13000 with dbgRetr ~3 (query kept
                // timing out) vs dbgRetr 0 (one slow answer). Verbose only -> off the quiet default fast path.
                if (_ftcVerbose)
                    openknx.logger.logWithPrefixAndValues("FTC", "[dbg] report ans %ums  retr %u/%u  missing %u  wnd %u [%u..%u)",
                                                          (unsigned)dbgWait, (unsigned)dbgRetr, (unsigned)FTC_REPORT_RETRIES,
                                                          (unsigned)missing, (unsigned)(_ftcWndEnd - _ftcReportBase),
                                                          (unsigned)_ftcReportBase, (unsigned)_ftcWndEnd);

                if (missing == 0)
                {
                    // Whole window/page received -> advance. AIMD additive-increase (windowed only).
                    _ftcReportBase = _ftcWndEnd;
                    _ftcDeadline = millis() + FTC_FAST_STALL_MS; // whole window confirmed on target -> progress
                    _ftcReportRetries = 0;
                    _ftcPrevMissing = 0xFFFF;
                    _ftcNoProgress = 0;
                    if (!_ftcRecovering)
                        _ftcWnd = (uint16_t)((_ftcWnd + 8u <= FTC_WND_MAX) ? _ftcWnd + 8u : FTC_WND_MAX);
                    if (_ftcReportBase > _ftcChunks)
                    {
                        ftcSendFastClose(); // all seqs confirmed on disk -> close -> verify
                        return;
                    }
                    ftcFastOpenWindow(); // freeze the next window edge
                    if (_ftcRecovering)
                        ftcSendReport(_ftcReportBase, (uint16_t)(_ftcWndEnd - _ftcReportBase)); // page: data already there
                    else
                    {
                        _ftcNextSeq = _ftcReportBase;
                        _ftcState = FtcFastStream; // windowed: stream the next window
                    }
                    return;
                }

                // Gaps remain. Union no-progress guard (monotonic bitmap -> missing is non-increasing):
                // if it fails to shrink across FTC_NOPROGRESS_MAX reports the seqs are persistently dead.
                if (missing >= _ftcPrevMissing) _ftcNoProgress++;
                else
                    _ftcNoProgress = 0;
                _ftcPrevMissing = missing;
                if (_ftcNoProgress >= FTC_NOPROGRESS_MAX)
                {
                    ftcAbort("fast: no progress (dead sequences)");
                    return;
                }
                if (millis() > _ftcDeadline)
                {
                    ftcAbort("fast: overall deadline exceeded");
                    return;
                }
                if (!_ftcRecovering) // multiplicative decrease sizes the NEXT window, not this one
                    _ftcWnd = (uint16_t)((_ftcWnd / 2 >= FTC_WND_MIN) ? _ftcWnd / 2 : FTC_WND_MIN);
                _ftcResendCur = _ftcReportBase;
                _ftcState = FtcFastResend;
            }
            else if (millis() - _ftcSince > FTC_REPORT_TIMEOUT)
            {
                if (++_ftcReportRetries > FTC_REPORT_RETRIES)
                {
                    ftcAbort("fast: report query unanswered");
                    return;
                }
                if (_ftcVerbose) // [dbg] report-query timeout/retry -- verbose only (off the quiet default path)
                    openknx.logger.logWithPrefixAndValues("FTC", "[dbg] report timeout %u/%u after %ums -> retry (wnd [%u..%u))",
                                                          (unsigned)_ftcReportRetries, (unsigned)FTC_REPORT_RETRIES,
                                                          (unsigned)(_ftcRepWaitStart ? millis() - _ftcRepWaitStart : 0),
                                                          (unsigned)_ftcReportBase, (unsigned)_ftcWndEnd);
                ftcSendReport(_ftcReportBase, (uint16_t)(_ftcWndEnd - _ftcReportBase)); // retry (fresh nonce)
            }
            return;
        }

        case FtcFastResend:
        {
            // Re-send only the UNSET seqs of the window, same burst cap + FIFO gate as the pump. At the
            // edge, all gaps are queued -> drain, then re-report the SAME window (fresh nonce).
            if (millis() > _ftcDeadline) // backstop: a wedged FIFO here would otherwise never re-report
            {
                ftcAbort("fast: overall deadline exceeded");
                return;
            }
            // forget (mode 2): the resend must be paced like the initial stream, else it re-blasts the gaps
            // over IP (no backpressure) and the target re-drops them. Windowed mode 1 keeps its full burst.
            if (_ftcMode == 2 && millis() < _ftcPaceNext) return;
            const uint8_t cap = (_ftcMode == 2) ? FTC_FORGET_BURST
                                                : (_ftcTestSource ? FTC_FAST_BURST_RAM : FTC_FAST_BURST_SD);
            uint8_t fired = 0;
            while (_ftcResendCur < _ftcWndEnd && fired < cap)
            {
                const uint16_t s = _ftcResendCur;
                if (_ftcRecvBmp[(s - 1) >> 3] & (1u << ((s - 1) & 7)))
                {
                    _ftcResendCur++; // already received -> skip (does not spend the burst budget)
                    continue;
                }
                if (ftcTxQueueSize() >= FTC_TX_HIGH) return; // FIFO near full -> yield, resume next loop()
                ftcSendFastData(s);
                _ftcResendCur++;
                fired++;
            }
            if (_ftcMode == 2 && fired > 0) _ftcPaceNext = millis() + FTC_FORGET_PACE_MS; // pace the resend too
            if (_ftcResendCur >= _ftcWndEnd && ftcTxQueueSize() < FTC_TX_LOW)
                ftcSendReport(_ftcReportBase, (uint16_t)(_ftcWndEnd - _ftcReportBase));
            return;
        }

        case FtcFastClose:
        {
            // cmd44 close ack (1-byte 0x00) -> verify the whole-file CRC32. The source is left OPEN here
            // (unlike classic close): forget recovery / the classic rescue may re-read it; closed at ftcFinish/ftcAbort.
            if (_ftcRespPending)
            {
                _ftcRespPending = false;
                if (_ftcRespProp != FTC_CMD_FILE_UPLOAD_FAST) return; // stale mirror -> keep waiting
                if (_ftcStartMs != 0) _ftcElapsedMs = millis() - _ftcStartMs;
                if (_ftcGrandStartMs != 0) _ftcGrandElapsedMs = millis() - _ftcGrandStartMs; // end-to-end (all attempts)
                ftcSendInfo();                                                               // FileInfo(43) -> FtcVerify (whole-file CRC32 = the ultimate gate)
            }
            else if (millis() - _ftcSince > FTC_TIMEOUT)
            {
                // The close's single loss is fatal but (unlike the report) had no retry -> retry it bounded:
                // by the first timeout the server has drained the burst and answers the re-sent close.
                if (++_ftcReportRetries > FTC_REPORT_RETRIES)
                    ftcAbort("no answer to fast close -- file may be unfinished on the target");
                else
                    ftcSendFastClose(); // ftcSend re-stamps _ftcSince
            }
            return;
        }

        case FtcResumeInfo:
        {
            if (_ftcRespPending)
            {
                _ftcRespPending = false;
                // FileInfo answers 9 bytes for an existing file, or 1 byte (0x42 = not found) when
                // there is none. Anything else is a stale/foreign frame -> keep waiting.
                if (_ftcRespLen != 1 && _ftcRespLen != 9)
                    return;

                const bool haveFile = (_ftcRespLen == 9 && _ftcResp[0] == 0x00);
                uint32_t have = 0, tcrc = 0;
                if (haveFile)
                {
                    have = ((uint32_t)_ftcResp[1] << 24) | ((uint32_t)_ftcResp[2] << 16) |
                           ((uint32_t)_ftcResp[3] << 8) | _ftcResp[4];
                    tcrc = ((uint32_t)_ftcResp[5] << 24) | ((uint32_t)_ftcResp[6] << 16) |
                           ((uint32_t)_ftcResp[7] << 8) | _ftcResp[8];
                }
                _ftcTargetHave = have; // remember what's at the path now (0 = none): a truncate frees it -> credited in the space check

                // Default = send the whole file from scratch. The branches below only deviate from
                // this when the target already holds a matching prefix (resume) or the full file (skip).
                _ftcResume = false;
                _ftcStartSeq = 1;
                _ftcResumeBase = 0;
                _ftcDone = 0;
                _ftcSrcCrc = 0;

                // no-resume (send `no-resume`): discard any existing target file, upload fresh. The fresh
                // defaults above already truncate ("w"); just skip the resume/skip/overwrite branches.
                if (_ftcNoResume)
                {
                    if (haveFile && have > 0)
                        openknx.logger.logWithPrefix("FTC", "no-resume: discarding existing target, uploading fresh");
                    ftcProceedToUpload();
                    return;
                }

                if (!haveFile || have == 0)
                {
                    ftcProceedToUpload(); // nothing usable there (fast or classic per _ftcMode)
                    return;
                }

                if (have == _ftcSize)
                {
                    // Same size -> is the whole file already there? Verify the CRC COOPERATIVELY (FtcCrcPrefix),
                    // never one blocking 478 KB read; ftcResumeCrcDone decides up-to-date vs overwrite.
                    ftcStartCrc(_ftcSize, 0xFFFFFFFFu, tcrc, 0);
                    return;
                }

                if (have > _ftcSize)
                {
                    openknx.logger.logWithPrefixAndValues("FTC", "target has %u B (more than our %u) -- overwriting",
                                                          (unsigned)have, (unsigned)_ftcSize);
                    ftcProceedToUpload();
                    return;
                }

                // 0 < have < size: continue at the last full-chunk boundary IF the target's partial is really
                // our prefix. Arm the resume params, then ONE cooperative CRC to `have` (compared against the
                // target) that snapshots the fold seed at the boundary -- ftcResumeCrcDone finishes the decision
                // (and resets to a fresh upload on a mismatch, since _ftcResume is armed true before the CRC).
                _ftcStartSeq = (uint16_t)(have / _ftcPayloadSize + 1);
                _ftcDone = (have / _ftcPayloadSize) * _ftcPayloadSize;
                _ftcResumeBase = _ftcDone;
                _ftcResume = true;
                ftcStartCrc(have, _ftcResumeBase, tcrc, 0);
                return;
            }
            else if (millis() - _ftcSince > FTC_TIMEOUT)
                // This FileInfo runs at the START of every transfer (not only resume), so don't label it
                // "resume:". A no-answer is usually a transient busy/reboot -> the auto-retry picks it up.
                ftcAbort("target did not answer the initial FileInfo (busy/rebooting?)");
            return;
        }

        case FtcCrcPrefix:
        {
            // Cooperative prefix CRC-32 (VORGABE non-blocking): CRC a bounded chunk per pass, gated on the
            // loop budget, so a large prefix (a 478 KB resume, a big perf ramp) never stalls loop(). Snapshots
            // the fold seed at the chunk boundary. Source = the opened file (resume) or the RAM ramp (perf).
            if (_crcOff >= _crcTarget)
            {
                if (_crcNext == 1) ftcPerfCrcDone();
                else
                    ftcResumeCrcDone();
                return;
            }
            uint8_t buf[255];
            do
            {
                uint32_t want = _crcTarget - _crcOff;
                if (want > sizeof(buf)) want = sizeof(buf);
                if (_crcSnapAt != 0xFFFFFFFFu && _crcOff < _crcSnapAt && _crcOff + want > _crcSnapAt)
                    want = _crcSnapAt - _crcOff; // land exactly on the snapshot point, never overshoot it
                const uint8_t got = ftcReadSource(_crcOff, buf, (uint8_t)want);
                if (got == 0)
                {
                    ftcAbort("prefix CRC: source read failed");
                    return;
                }
                _ftcSrcCrc = ftcCrc32Posix(_ftcSrcCrc, buf, got, false);
                _crcOff += got;
                if (_crcSnapAt != 0xFFFFFFFFu && _crcOff >= _crcSnapAt)
                {
                    _ftcResumeSeedCrc = _ftcSrcCrc;
                    _crcSnapAt = 0xFFFFFFFFu;
                }
            }
            while (_crcOff < _crcTarget && openknx.freeLoopTime());
            if (_crcOff < _crcTarget) return; // loop budget spent -> continue next pass
            if (_crcNext == 1) ftcPerfCrcDone();
            else
                ftcResumeCrcDone();
            return;
        }

        case FtcVerify:
        {
            if (_ftcRespPending)
            {
                _ftcRespPending = false;
                // A fast gap report (cmd45) is also 9 bytes, so length alone can't tell it from the FileInfo
                // answer -- filter by propertyId (only the FileInfo(43) echo is our verify result).
                if (_ftcRespProp != FTC_CMD_FILE_INFO) return;
                // A FileInfo answer is 9 bytes; anything shorter is a leftover (the close answer is empty)
                // -> keep waiting instead of declaring the file broken.
                if (_ftcRespLen != 9)
                {
                    openknx.logger.logWithPrefixAndValues("FTC", "verify: ignoring stale answer (len=%u)", _ftcRespLen);
                    return;
                }
                if (_ftcResp[0] != 0x00)
                {
                    openknx.logger.logWithPrefixAndValues("FTC", "VERIFY FAILED: target cannot stat the file (0x%02X)",
                                                          _ftcResp[0]);
                    ftcFinish();
                    return;
                }
                const uint32_t size = ((uint32_t)_ftcResp[1] << 24) | ((uint32_t)_ftcResp[2] << 16) |
                                      ((uint32_t)_ftcResp[3] << 8) | _ftcResp[4];
                const uint32_t crc = ((uint32_t)_ftcResp[5] << 24) | ((uint32_t)_ftcResp[6] << 16) |
                                     ((uint32_t)_ftcResp[7] << 8) | _ftcResp[8];
                const uint32_t mine = _ftcSrcCrc ^ 0xFFFFFFFFu; // xorout, see ftcCrc32Posix()
                const bool ok = (size == _ftcSize) && (crc == mine);
                _status.ok = ok;
                _status.crc = crc;
                _status.done = _status.total;

                // Fire-and-forget must NEVER accept a silently-bad file. A CRC mismatch escalates:
                // (1) report-based gap recovery, (2) one classic full resend, (3) clean reported abort.
                if (!ok && (_ftcMode == 2 || _ftcClassicRescue))
                {
                    if (_ftcClassicRescue)
                    {
                        // The classic full resend (last resort) also mismatches -> clean reported abort.
                        ftcAbort("F&F recovery failed");
                        return;
                    }
                    if (!_ftcRecovering)
                    {
                        openknx.logger.logWithPrefixAndValues("FTC", "forget: verify mismatch (size %u/%u, crc 0x%08X/0x%08X) -- gap recovery",
                                                              (unsigned)size, (unsigned)_ftcSize, (unsigned)crc, (unsigned)mine);
                        // Enter report-based recovery: source CRC already folded (fold-once) so pin the
                        // watermark at _ftcChunks (no re-fold); clear the bitmap, paged reports rebuild it.
                        _ftcRecovering = true;
                        _ftcReportBase = 1;
                        _ftcFoldWatermark = _ftcChunks;
                        _ftcWnd = FTC_FAST_PAGE;
                        _ftcPrevMissing = 0xFFFF;
                        _ftcNoProgress = 0;
                        _ftcReportRetries = 0;
                        _ftcPaceNext = 0; // first recovery resend burst must not be gated by a stale stream value
                        _ftcReportNonce = 0;
                        memset(_ftcRecvBmp, 0, sizeof(_ftcRecvBmp));
                        _ftcDeadline = millis() + FTC_FAST_STALL_MS; // progress-based: re-armed on every chunk that makes progress
                        ftcFastOpenWindow();
                        ftcSendFastOpen(); // resume+keepBitmap -> FtcFastOpen -> (recovering) report
                        return;
                    }
                    if (_ftcMode == 2 && _ftcRecovering)
                    {
                        // Report recovery still mismatches -> one classic full resend. Keep _ftcMode==2 as
                        // the forget marker; _ftcClassicRescue makes a later verify failure abort, not loop.
                        openknx.logger.logWithPrefix("FTC", "forget: gap recovery still mismatches -- one classic full resend");
                        _ftcRecovering = false;
                        _ftcClassicRescue = true;
                        _ftcResume = false; // truncate + full resend
                        _ftcStartSeq = 1;
                        _ftcSequence = 0;
                        _ftcResumeBase = 0;
                        _ftcDone = 0;
                        _ftcSrcCrc = 0;
                        _ftcRetries = 0;
                        _ftcNextPct = 10; // re-arm decile progress lines for the fresh resend
                        _ftcStartMs = millis();
                        ftcSendUploadOpen(); // classic states -> FtcUploadClose -> ftcSendInfo -> FtcVerify
                        return;
                    }
                    // Classic rescue also failed -> clean reported abort, never a silent bad file.
                    ftcAbort("F&F recovery failed");
                    return;
                }

                // A perf run's pattern file is throwaway -> delete it after a clean verify (a normal upload,
                // or a `keep` perf under its CRC name, is kept); the summary waits for FtcPerfCleanup to report it.
                if (_ftcIsPerf && ok && !_ftcPerfKeep)
                {
                    const char *pf = "/ftcperf.bin";
                    const size_t n = strlen(pf) + 1;
                    memcpy(_ftcTx, pf, n);
                    if (ftcSend(FTC_CMD_FILE_DELETE, (uint8_t)n))
                    {
                        _ftcState = FtcPerfCleanup;
                        return;
                    }
                }
                ftcPrintSummary();
                // Opt-in self-apply: on a clean verify, ask the target to apply the image (reboots it).
                if (_ftcApply && ok && !_ftcIsPerf && !_ftcTestSource)
                {
                    ftcEnterApplyGate(); // cached feature byte -> decide now, else probe (FtcApplyProbe)
                    return;
                }
                ftcFinish();
            }
            else if (millis() - _ftcSince > FTC_TIMEOUT)
            {
                openknx.logger.logWithPrefix("FTC", "VERIFY: no answer -- upload unconfirmed");
                ftcFinish();
            }
            return;
        }

        case FtcPerfCleanup:
        {
            if (_ftcRespPending)
            {
                _ftcRespPending = false;
                if (ftcDropDup()) return;
                _ftcPerfRemoved = (_ftcRespLen >= 1 && _ftcResp[0] == 0x00);
                ftcPrintSummary();
                ftcFinish();
            }
            else if (millis() - _ftcSince > FTC_TIMEOUT)
            {
                _ftcPerfRemoved = false; // no answer -> the test file is probably still on the target
                ftcPrintSummary();
                ftcFinish();
            }
            return;
        }

        case FtcApplyCheck:
        {
            // Standalone fwupdate: the FileInfo(43) existence pre-check. Present (0x00) -> enter the apply
            // gate; absent -> refuse, so a mistyped path can never reboot a live coupler.
            if (_ftcRespPending)
            {
                _ftcRespPending = false;
                if (_ftcRespProp != FTC_CMD_FILE_INFO) return; // stale mirror -> keep waiting
                if (_ftcRespLen < 1 || _ftcResp[0] != 0x00)
                {
                    openknx.logger.logWithPrefixAndValues("FTC", "file not found on target -- not triggering (0x%02X)",
                                                          _ftcRespLen ? _ftcResp[0] : 0xFF);
                    ftcFinish();
                    return;
                }
                ftcEnterApplyGate();
            }
            else if (millis() - _ftcSince > FTC_TIMEOUT)
            {
                openknx.logger.logWithPrefix("FTC", "fwupdate: no answer to the existence check -- not triggering");
                ftcFinish();
            }
            return;
        }

        case FtcApplyProbe:
        {
            // Mirror of FtcFeatureProbe, but for apply: learn whether the target can self-apply (Update bit).
            if (_ftcRespPending)
            {
                _ftcRespPending = false;
                if (_ftcRespProp != FTC_CMD_CHECK_FEATURES) return; // stale mirror -> keep waiting
                _ftcFeatPa = _ftcTarget;
                _ftcFeatBits = (_ftcRespLen >= 1) ? _ftcResp[0] : 0;
                _ftcFeatValid = true; // cache the real answer
                ftcApplyDecide();
            }
            else if (millis() - _ftcSince > FTC_FEATURE_TIMEOUT)
            {
                // No CheckFeatures answer in the short window -> assume no self-apply (ESP / old FTM).
                openknx.logger.logWithPrefix("FTC", "target cannot self-apply (ESP / old FTM) -- image uploaded, apply skipped");
                ftcFinish();
            }
            return;
        }

        case FtcInfo:
        {
            if (_ftcRespPending)
            {
                _ftcRespPending = false;
                if (ftcDropDup()) return; // ignore a mirrored duplicate answer
                if (_ftcRespLen < 1 || _ftcResp[0] != 0x00)
                {
                    openknx.logger.logWithPrefixAndValues("FTC", "info: file not found / error 0x%02X",
                                                          _ftcRespLen ? _ftcResp[0] : 0xFF);
                    ftcFinish();
                    return;
                }
                // Answer layout (verified on device): [00][size:4 BE][crc32:4 BE].
                if (_ftcRespLen >= 9)
                {
                    const uint32_t size = ((uint32_t)_ftcResp[1] << 24) | ((uint32_t)_ftcResp[2] << 16) |
                                          ((uint32_t)_ftcResp[3] << 8) | _ftcResp[4];
                    const uint32_t crc = ((uint32_t)_ftcResp[5] << 24) | ((uint32_t)_ftcResp[6] << 16) |
                                         ((uint32_t)_ftcResp[7] << 8) | _ftcResp[8];
                    _status.total = size;
                    _status.crc = crc;
                    _status.ok = true;
                    openknx.logger.logWithPrefixAndValues("FTC", "info: size=%u  crc32=0x%08X", size, crc);
                }
                else
                {
                    openknx.logger.logWithPrefixAndValues("FTC", "info: unexpected answer (%u bytes)", _ftcRespLen);
                }
                ftcFinish();
            }
            else if (millis() - _ftcSince > FTC_TIMEOUT)
            {
                openknx.logger.logWithPrefix("FTC", "info: no answer");
                ftcFinish();
            }
            return;
        }

        case FtcDirList:
        {
            if (_ftcRespPending)
            {
                _ftcRespPending = false;
                // Drop mirrored duplicates: wrong command echo, or a copy arriving within the mirror
                // window of the last accepted answer (a real TP round trip is far slower).
                if (_ftcRespProp != FTC_CMD_DIR_LIST || (_ftcRespT && millis() - _ftcRespT < FTC_DUP_WINDOW_MS))
                    return;
                _ftcRespT = millis();
                if (_ftcRespLen < 2 || _ftcResp[0] != 0x00)
                {
                    openknx.logger.logWithPrefixAndValues("FTC", "list: error 0x%02X",
                                                          _ftcRespLen ? _ftcResp[0] : 0xFF);
                    _ftcListing.clear();
                    ftcFinish();
                    return;
                }
                const uint8_t type = _ftcResp[1]; // 0 = no more, 1 = file, 2 = directory
                if (type == 0)
                {
                    // Collection done -> print the table header.
                    char title[96];
                    snprintf(title, sizeof(title), "== KNX File Transfer - %u.%u.%u - list \"%s\" ==",
                             (_ftcTarget >> 12) & 0x0F, (_ftcTarget >> 8) & 0x0F, _ftcTarget & 0xFF, _ftcListDir);
                    ftcOut(0, "");
                    ftcOut(CONSOLE_HEADLINE_COLOR, "%s", title);
                    ftcOut(CONSOLE_HEADLINE_COLOR, "------------------------------------------------------------------------------");
                    if (_ftcListDetailed)
                        ftcOut(CONSOLE_HEADLINE_COLOR, "Name                                     | Size (bytes) | CRC32      | Type");
                    else
                        ftcOut(CONSOLE_HEADLINE_COLOR, "Name                                     | Type");
                    ftcOut(CONSOLE_HEADLINE_COLOR, "------------------------------------------------------------------------------");
                    if (_ftcListing.empty())
                        ftcOut(0, "  ..(empty)");
                    _ftcDirIdx = 0;
                    if (_ftcListDetailed)
                    {
                        // ll: one FileInfo per file fills in size + CRC32 (via FtcDirInfo).
                        _ftcState = FtcDirInfo;
                        ftcListAdvance();
                        return;
                    }
                    // ls: names only, no per-file round trips.
                    for (size_t i = 0; i < _ftcListing.size(); i++)
                    {
                        char row[128];
                        snprintf(row, sizeof(row), "%-40.40s | %s", _ftcListing[i].name,
                                 _ftcListing[i].isDir ? "dir" : "file");
                        ftcOut(0, "%s", row);
                        if (_ftcListing[i].isDir)
                            _ftcListDirs++;
                        else
                            _ftcListFiles++;
                    }
                    ftcOut(CONSOLE_HEADLINE_COLOR, "------------------------------------------------------------------------------");
                    char lsleft[41];
                    snprintf(lsleft, sizeof(lsleft), "Files: %u   Dirs: %u", _ftcListFiles, _ftcListDirs);
                    char lsfoot[64];
                    snprintf(lsfoot, sizeof(lsfoot), "%-40.40s | total", lsleft); // aligns with "Name | Type"
                    ftcOut(CONSOLE_HEADLINE_COLOR, "%s", lsfoot);
                    ftcOut(CONSOLE_HEADLINE_COLOR, "------------------------------------------------------------------------------");
                    // ls done -- ftcFinish() releases _ftcListing (size/crc are 0 here anyway, hasInfo=false).
                    _status.ok = true;
                    ftcFinish();
                    return;
                }
                // One entry -> store name + kind, then continue the iterator.
                uint8_t nl = (_ftcRespLen > 2) ? (uint8_t)(_ftcRespLen - 2) : 0;
                char name[64];
                if (nl >= sizeof(name)) nl = sizeof(name) - 1;
                memcpy(name, _ftcResp + 2, nl);
                name[nl] = 0;
                if (_ftcListing.size() < FTC_SCAN_MAX_LIST) // cap collected entries (RAM + the one-pass ll/ls emit); mirrors scanRecord
                {
                    FtcEntry e;
                    strncpy(e.name, name, sizeof(e.name) - 1);
                    e.isDir = (type == 2);
                    _ftcListing.push_back(e);
                }
                ftcSendDirList();
                return;
            }
            else if (millis() - _ftcSince > FTC_TIMEOUT)
            {
                openknx.logger.logWithPrefix("FTC", "list: no answer");
                _ftcListing.clear();
                ftcFinish();
            }
            return;
        }

        case FtcDirInfo:
        {
            if (_ftcRespPending)
            {
                _ftcRespPending = false;
                // Same duplicate guard: a stale DirList mirror would be misread as size+CRC, a FileInfo
                // mirror would land on the next file -- accept only the real, on-time FileInfo echo.
                if (_ftcRespProp != FTC_CMD_FILE_INFO || (_ftcRespT && millis() - _ftcRespT < FTC_DUP_WINDOW_MS))
                    return;
                _ftcRespT = millis();
                const char *nm = (_ftcDirIdx < _ftcListing.size()) ? _ftcListing[_ftcDirIdx].name : "?";
                char row[128];
                if (_ftcRespLen >= 9 && _ftcResp[0] == 0x00)
                {
                    const uint32_t size = ((uint32_t)_ftcResp[1] << 24) | ((uint32_t)_ftcResp[2] << 16) |
                                          ((uint32_t)_ftcResp[3] << 8) | _ftcResp[4];
                    const uint32_t crc = ((uint32_t)_ftcResp[5] << 24) | ((uint32_t)_ftcResp[6] << 16) |
                                         ((uint32_t)_ftcResp[7] << 8) | _ftcResp[8];
                    snprintf(row, sizeof(row), "%-40.40s | %12u | 0x%08X | file", nm, (unsigned)size, (unsigned)crc);
                    _ftcListBytes += size;
                    _ftcListFiles++;
                }
                else
                    snprintf(row, sizeof(row), "%-40.40s | %12s | %-10s | file?", nm, "?", "?");
                ftcOut(0, "%s", row);
                _ftcDirIdx++;
                ftcListAdvance();
                return;
            }
            else if (millis() - _ftcSince > FTC_TIMEOUT)
            {
                const char *nm = (_ftcDirIdx < _ftcListing.size()) ? _ftcListing[_ftcDirIdx].name : "?";
                char row[128];
                snprintf(row, sizeof(row), "%-40.40s | %12s | %-10s | (no answer)", nm, "?", "?");
                ftcOut(0, "%s", row);
                _ftcDirIdx++;
                ftcListAdvance();
            }
            return;
        }

        case FtcFsInfo:
        {
            if (_ftcRespPending)
            {
                _ftcRespPending = false;
                if (_ftcRespProp != FTC_CMD_FILESYSTEM_INFO) return; // stale mirror -> keep waiting
                // [00][total:4 BE][used:4 BE]; a 1-byte error = an old server without the command.
                if (_ftcRespLen >= 9 && _ftcResp[0] == 0x00)
                {
                    _ftcFsTotal = ((uint32_t)_ftcResp[1] << 24) | ((uint32_t)_ftcResp[2] << 16) |
                                  ((uint32_t)_ftcResp[3] << 8) | _ftcResp[4];
                    _ftcFsUsed = ((uint32_t)_ftcResp[5] << 24) | ((uint32_t)_ftcResp[6] << 16) |
                                 ((uint32_t)_ftcResp[7] << 8) | _ftcResp[8];
                }
                else
                {
                    // Old server: never block an upload; for df note it; for the ll footer end quietly.
                    if (_ftcFsPurpose == 1)
                    {
                        _ftcSpaceChecked = true;
                        ftcProceedToUpload();
                        return;
                    }
                    if (_ftcFsPurpose == 0)
                        openknx.logger.logWithPrefix("FTC", "df: target did not report filesystem size (old server?)");
                    ftcFinish();
                    return;
                }

                const uint32_t freeB = (_ftcFsTotal > _ftcFsUsed) ? (_ftcFsTotal - _ftcFsUsed) : 0;
                if (_ftcFsPurpose == 1) // pre-upload space check
                {
                    const uint32_t need = (_ftcSize > _ftcResumeBase) ? (_ftcSize - _ftcResumeBase) : 0; // bytes still to write
                    const uint32_t credit = (_ftcResumeBase == 0) ? _ftcTargetHave : 0;                  // truncate frees the old file; resume keeps the partial
                    const uint32_t avail = freeB + credit;
                    if ((uint64_t)need + FTC_FS_MARGIN > (uint64_t)avail)
                    {
                        openknx.logger.logWithPrefixAndValues("FTC", "NOT ENOUGH SPACE: need %u B + %u margin, target free %u B%s",
                                                              (unsigned)need, (unsigned)FTC_FS_MARGIN, (unsigned)freeB,
                                                              credit ? " (+overwrite credit)" : "");
                        ftcPrintFsBar(_ftcFsTotal, _ftcFsUsed);
                        ftcAbort("not enough space on target filesystem");
                        return;
                    }
                    ftcOut(0, "  space      ok - need %u B, target free %u B -> proceeding", (unsigned)need, (unsigned)freeB); // CONFIG-box step
                    _ftcSpaceChecked = true;
                    ftcProceedToUpload();
                    return;
                }
                ftcPrintFsBar(_ftcFsTotal, _ftcFsUsed, _ftcFsPurpose == 0); // df (0) = full block; ll footer (2) = bars only
                ftcFinish();
            }
            else if (millis() - _ftcSince > FTC_TIMEOUT)
            {
                // No answer -> old server or a busy target. NEVER block the upload on it: proceed without
                // the space check; for df / ll just note it and end.
                if (_ftcFsPurpose == 1)
                {
                    openknx.logger.logWithPrefix("FTC", "no free-space answer (old server?) -- uploading without the space check");
                    _ftcSpaceChecked = true;
                    ftcProceedToUpload();
                    return;
                }
                if (_ftcFsPurpose == 0) // df only; the ll footer (2) just ends quietly (the list already printed)
                    openknx.logger.logWithPrefix("FTC", "df: no answer from target");
                ftcFinish();
            }
            return;
        }

        case FtcDelete:
        {
            if (_ftcRespPending)
            {
                _ftcRespPending = false;
                if (ftcDropDup()) return; // ignore a mirrored duplicate result
                _status.ok = (_ftcRespLen >= 1 && _ftcResp[0] == 0x00);
                if (_status.ok)
                    openknx.logger.logWithPrefixAndValues("FTC", "%s: \"%s\" ok", _ftcVerb, _status.path);
                else
                    openknx.logger.logWithPrefixAndValues("FTC", "%s: failed (0x%02X)", _ftcVerb,
                                                          _ftcRespLen ? _ftcResp[0] : 0xFF);
                ftcFinish();
            }
            else if (millis() - _ftcSince > FTC_TIMEOUT)
            {
                openknx.logger.logWithPrefix("FTC", "rm: no answer");
                ftcFinish();
            }
            return;
        }

        case FtcSent:
        {
            if (_ftcRespPending)
            {
                openknx.logger.logWithPrefixAndValues("FTC", "Response obj=%u prop=%u len=%u", _ftcRespObj, _ftcRespProp, _ftcRespLen);
                if (_ftcRespLen > 0)
                    openknx.logger.logWithPrefixAndValues("FTC", "  result=0x%02X", _ftcResp[0]);
                ftcFinish();
            }
            else if (millis() - _ftcSince > FTC_TIMEOUT)
            {
                openknx.logger.logWithPrefix("FTC", "no response within 6s -- disconnecting");
                ftcFinish();
            }
            return;
        }

        case FtcUploadOpen:
        {
            if (_ftcRespPending)
            {
                _ftcRespPending = false;
                // A late FilesystemInfo (cmd46) answer is 9 bytes with [0]==0x00 and could be mistaken for
                // the open-ack -> accept only the FileUpload echo (mirrors the FtcFastOpen guard).
                if (_ftcRespProp != FTC_CMD_FILE_UPLOAD) return;
                if (_ftcRespLen < 1 || _ftcResp[0] != 0x00)
                {
                    // 0x42 = could not open (bad path?), 0x81 = a dir listing is still open there
                    openknx.logger.logWithPrefixAndValues("FTC", "open rejected, result=0x%02X",
                                                          _ftcRespLen ? _ftcResp[0] : 0xFF);
                    ftcAbort("target refused FileUpload/open");
                    return;
                }
                _ftcSequence = _ftcStartSeq; // 1-based; >1 when resuming (0 was the open marker)
                ftcSendNextChunk();
            }
            else if (millis() - _ftcSince > FTC_TIMEOUT)
                ftcAbort("no answer to FileUpload/open");
            return;
        }

        case FtcUploadChunkRetry:
            // Frame + CRC in _ftcTx are still valid -- just re-send. The target seeks by sequence, so a
            // repeat lands at the right offset even if the first attempt partly arrived.
            ftcSend(FTC_CMD_FILE_UPLOAD, _ftcTxLen);
            _ftcState = FtcUploadChunk;
            return;

        case FtcUploadChunk:
        {
            if (_ftcRespPending)
            {
                _ftcRespPending = false;

                // Answers arrive duplicated (measured, cause unexplained -- not the bus repeating). Filter
                // stale echoes here. A 1-byte NON-zero answer is a REAL target failure (e.g. FS full), not
                // stale -- don't swallow it; a 1-byte 0x00 is the OPEN answer's second copy, not a rejection.
                if (_ftcRespLen == 1 && _ftcResp[0] != 0x00)
                {
                    openknx.logger.logWithPrefixAndValues("FTC", "chunk %u rejected, result=0x%02X",
                                                          _ftcSequence, _ftcResp[0]);
                    ftcAbort(ftcResultName(_ftcResp[0]));
                    return;
                }
                if (_ftcRespLen != 5)
                {
                    openknx.logger.logWithPrefixAndValues("FTC", "ignoring answer (len=%u, result=0x%02X) while waiting for chunk %u",
                                                          _ftcRespLen, _ftcRespLen ? _ftcResp[0] : 0, _ftcSequence);
                    return;
                }

                // Answer layout: [0]=result, [1..2]=sequence, [3..4]=CRC -- both BIG endian
                // (pushWord), unlike the little endian sequence we sent. The asymmetry is real.
                const uint16_t seq = (uint16_t)((_ftcResp[1] << 8) | _ftcResp[2]);
                const uint16_t crc = (uint16_t)((_ftcResp[3] << 8) | _ftcResp[4]);

                if (seq != _ftcSequence)
                {
                    // The duplicate: another KNX-IP router on this TP line mirrors traffic onto the routing
                    // multicast, so each answer returns once more over IP (not the bus, not the target).
                    // Harmless (the seq check discards it), but logging per chunk floods USB-CDC -> log once.
                    if (_ftcDupes == 0)
                        openknx.logger.logWithPrefixAndValues("FTC", "duplicate answer for seq %u (waiting for %u) -- a second KNX-IP router mirrors this line onto the routing multicast; counting from here",
                                                              seq, _ftcSequence);
                    _ftcDupes++;
                    return;
                }

                if (_ftcResp[0] != 0x00)
                {
                    openknx.logger.logWithPrefixAndValues("FTC", "chunk %u rejected, result=0x%02X",
                                                          _ftcSequence, _ftcResp[0]);
                    ftcAbort(ftcResultName(_ftcResp[0]));
                    return;
                }
                if (crc != _ftcTxCrc)
                {
                    // The target CRCs what it actually received. A mismatch means the bytes got
                    // mangled on the way -- the one case a retry is genuinely made for.
                    openknx.logger.logWithPrefixAndValues("FTC", "CRC mismatch on chunk %u (sent 0x%04X, got 0x%04X)",
                                                          _ftcSequence, _ftcTxCrc, crc);
                    ftcRetryOrAbort("CRC mismatch");
                    return;
                }

                _ftcDone += _ftcTx[2];         // payload length of the frame just acknowledged
                _ftcLastProgressMs = millis(); // forward progress -> retry dead-window base (parity with the fast path)
                _status.done = _ftcDone;
                _status.chunk = _ftcSequence;
                // Refresh throughput live so `ftc status` shows real B/s mid-transfer (not 0 until the
                // end). Count only THIS run's bytes against its clock -- resumed bytes are not ours.
                if (_ftcStartMs != 0)
                {
                    const uint32_t el = millis() - _ftcStartMs;
                    const uint32_t sent = (_ftcDone > _ftcResumeBase) ? (_ftcDone - _ftcResumeBase) : 0;
                    const uint32_t liveBps = el ? (uint32_t)(((uint64_t)sent * 1000ULL) / el) : 0;
                    _status.bps = (uint16_t)(liveBps > 0xFFFF ? 0xFFFF : liveBps);
                }

                // Per-chunk logging would flood USB-CDC and slow the transfer; ftcMaybeProgress gates it
                // to a decile (or 1 Hz when verbose) and emits the two-line "Variant D" block via ftcOut.
                ftcMaybeProgress(true, _ftcSequence, _ftcChunks, _ftcDone, _ftcSize, _ftcStartMs);

                if (_ftcDone >= _ftcSize)
                {
                    ftcSendClose();
                    return;
                }
                _ftcRetries = 0; // this chunk is through; the next one starts with a full budget
                _ftcSequence++;
                ftcSendNextChunk(); // one chunk per loop pass -- we only get here on an answer
            }
            else if (millis() - _ftcSince > FTC_TIMEOUT)
                ftcRetryOrAbort("no answer to a chunk");
            return;
        }

        case FtcUploadClose:
        {
            // The close answer carries no data (resultLength = 0), so its arrival is the signal.
            const bool done = _ftcRespPending;
            _ftcRespPending = false; // consume it -- otherwise the next state inherits this answer
            if (!done && millis() - _ftcSince > FTC_TIMEOUT)
            {
                // Not a success: close is what makes the target flush + close the file. Without its answer
                // the file may be unfinalised, so don't report a throughput here.
                ftcAbort("no answer to FileUpload/close -- file may be unfinished on the target");
                return;
            }
            if (done)
            {
                // Freeze the throughput now; the summary box prints it once at the very end (after
                // verify + any perf cleanup), so nothing here goes to the log yet.
                _ftcElapsedMs = millis() - _ftcStartMs;
                if (_ftcGrandStartMs != 0) _ftcGrandElapsedMs = millis() - _ftcGrandStartMs; // end-to-end (all attempts)
                const uint32_t sent = (_ftcDone > _ftcResumeBase) ? (_ftcDone - _ftcResumeBase) : 0;
                const uint32_t bps = _ftcElapsedMs ? (uint32_t)(((uint64_t)sent * 1000ULL) / _ftcElapsedMs) : 0;
                _status.bps = (uint16_t)((bps > 0xFFFF) ? 0xFFFF : bps);
                // "Bytes arrived" is not "the right bytes arrived" -> ask the target for size + CRC32 over
                // the file it wrote and compare.
                ftcCloseSource();
                ftcSendInfo();
            }
            return;
        }

        case FtcCancel:
        {
            if (_ftcRetryPending)
            {
                // Auto-retry a transient failure: after the Cancel drains + a backoff, re-run -> FileInfo ->
                // resume from the target's partial. _ftcTransferRetries is NOT reset here (only on a fresh request).
                if (millis() - _ftcSince < _cfgBackoffMs) return;
                _ftcRetryPending = false;
                // Charge the whole unproductive window (failed attempt's dead time + Cancel drain +
                // backoff) to retry overhead, so the transfer-only rate reflects the wire, not recovery.
                uint32_t deadBase = _ftcLastProgressMs;
                if (deadBase < _ftcStartMs) deadBase = _ftcStartMs;              // no progress this attempt -> it was all dead
                if (deadBase == 0 || deadBase > _ftcSince) deadBase = _ftcSince; // no sane timestamp -> just the backoff
                _ftcRetryLostMs += (millis() - deadBase);
                // pkg-auto degrade is applied in ftcProceedToUpload (fresh-only, offset-safe) -- not here.
                const bool wasPerf = _ftcIsPerf; // ftcStart clears it -> restore so perf's summary/cleanup still runs
                ftcStart(_ftcTarget, true, _ftcMode);
                _ftcIsPerf = wasPerf;
                return;
            }
            // Give the Cancel frame time to leave before dropping the connection under it.
            if (millis() - _ftcSince > 500)
                ftcFinish();
            return;
        }

        case FtcScan:
        {
            // 1) Drain every queued answer. Matched by the responder's own PA, so probe order and
            //    answer order need not line up -- that is what lets the probes be pipelined.
            while (_ftcDdTail != _ftcDdHead)
            {
                const FtcDdMsg m = _ftcDdQ[_ftcDdTail];
                _ftcDdTail = (uint8_t)((_ftcDdTail + 1) & (FTC_DD_Q - 1));
                if (m.pa <= (uint16_t)_scanEnd) // sanity: inside the range we are sweeping
                {
                    scanRecord(m.pa, m.mask);
                    _ftcSince = millis(); // an answer just arrived -> keep the drain window open (adaptive)
                }
            }

            // 2) Fire the next probe once the spacing has elapsed (_scanLastSend == 0 -> first one now).
            if (_scanNext <= _scanEnd)
            {
                if (_scanLastSend == 0 || millis() - _scanLastSend >= _scanSpacingMs)
                {
                    const uint16_t pa = (uint16_t)_scanNext;
                    _scanNext++;
                    if (pa != 0 && pa != knx.individualAddress()) // never probe ourselves
                    {
                        SecurityControl sec = {false, None};
                        knx.bau().ftcSendDeviceDescriptorRead(pa, sec);
                        _scanProbed++;
                        _status.done = _scanProbed;
                    }
                    _scanLastSend = millis();
                    _ftcSince = millis(); // hold the drain timer off while probes are still going out
                }
                return;
            }

            // 3) All probes sent -> let the slowest answer drain in. Then either start the next deep
            //    pass (accumulating the union, since IP drops different answers each time) or finish.
            if (millis() - _ftcSince > _scanDrainMs)
            {
                if (_scanSweep < _scanSweeps)
                {
                    _scanSweep++;
                    openknx.logger.logWithPrefixAndValues("FTC", "  deep sweep %u/%u (%u found so far)...",
                                                          _scanSweep, _scanSweeps, _scanFound);
                    _scanNext = _scanStart; // rescan the same range; _ftcListing is NOT cleared -> union
                    _scanLastSend = 0;
                    _ftcSince = millis();
                    return;
                }
                scanReport();
                ftcFinish();
            }
            return;
        }

        case FtcScanCo:
        {
            // ETS-parity scan: strictly serial, one address in flight. T_Connect -> CO
            // DeviceDescriptor_Read -> disconnect. Reaches old BCU1/BCU2 masks that answer ONLY
            // connection-oriented. RULE: ftcScanDisconnect() on EVERY exit (present/absent/timeout) --
            // a half-open TP connection would wedge the next address.

            // a) Drain answers exactly like FtcScan; flag the address in flight (_scanNext) as present.
            while (_ftcDdTail != _ftcDdHead)
            {
                const FtcDdMsg m = _ftcDdQ[_ftcDdTail];
                _ftcDdTail = (uint8_t)((_ftcDdTail + 1) & (FTC_DD_Q - 1));
                if (m.pa <= (uint16_t)_scanEnd)
                {
                    scanRecord(m.pa, m.mask);
                    if (m.pa == (uint16_t)_scanNext) _scanCoGot = true;
                }
            }

            if (_scanCoPhase == 0) // Connect
            {
                if (_scanNext > _scanEnd)
                {
                    scanReport();
                    ftcFinish();
                    return;
                } // whole range done
                const uint16_t pa = (uint16_t)_scanNext;
                if (pa == 0 || pa == knx.individualAddress())
                {
                    _scanNext++;
                    return;
                } // never probe ourselves
                if (_scanLastSend != 0 && millis() - _scanLastSend < FTC_CO_SETTLE_MS) return; // let last disconnect settle
                if (!knx.bau().ftcScanConnect(pa))
                {
                    // Busy = the TP link is owned (ETS/mgmt). Back off, but bound it: without a deadline a
                    // phase-0 scan spins forever while ETS holds the link -> abort + report after FTC_CO_BUSY_TMO.
                    if (_scanCoBusyT0 == 0)
                        _scanCoBusyT0 = millis();
                    else if (millis() - _scanCoBusyT0 > FTC_CO_BUSY_TMO)
                    {
                        openknx.logger.logWithPrefix("FTC", "scan: TP connection busy (ETS/mgmt?) -- aborting CO scan");
                        scanReport();
                        ftcFinish();
                    }
                    return;
                }
                _scanCoBusyT0 = 0; // connected -> clear the busy deadline
                _scanProbed++;
                _status.done = _scanProbed;
                _scanCoGot = false;
                _scanCoT0 = millis();
                _scanCoPhase = 1; // WaitConnect
                return;
            }

            if (_scanCoPhase == 1) // WaitConnect
            {
                if (knx.bau().ftcScanConnected()) // T_Connect up -> read on the connection
                {
                    SecurityControl sec = {false, None};
                    knx.bau().ftcScanReadDescriptor(sec);
                    _scanCoT0 = millis();
                    _scanCoPhase = 2; // WaitResp
                }
                else if (millis() - _scanCoT0 > FTC_CO_CONNECT_TMO) // no ack -> ABSENT
                {
                    knx.bau().ftcScanDisconnect();
                    _scanLastSend = millis();
                    _scanNext++;
                    _scanCoPhase = 0;
                }
                return;
            }

            // _scanCoPhase == 2: WaitResp -- present (answered) or timed out, either way disconnect + advance.
            if (_scanCoGot || millis() - _scanCoT0 > FTC_CO_READ_TMO)
            {
                knx.bau().ftcScanDisconnect();
                _scanLastSend = millis();
                _scanNext++;
                _scanCoPhase = 0;
            }
            return;
        }

        case FtcScanPost:
        {
            // Post-sweep OpenKNX/info probe + cooperative CSV write (Feature B/C). Strictly non-blocking:
            // ONE PID read per device (in flight, bounded by FTC_TIMEOUT) and at most a few file rows per
            // loop() pass (gated on freeLoopTime()). Every path advances -> cannot wedge.

            // (1) Resolve a LIGHT probe in flight (FULL probes resolve via the FtcDevProp short-circuit).
            if (_scanProbeInFlight == 1)
            {
                if (_propPending)
                {
                    _propPending = false;
                    if (_propObj == 0 && _propPid == FTC_PID_SERIAL && _propLen >= 2)
                    {
                        _devMfr = (uint16_t)((_propData[0] << 8) | _propData[1]); // first 2 of the 6-byte serial = mfr id
                        _scanProbeAnswered = true;
                    }
                    _scanProbeDone = true;
                }
                else if (millis() - _ftcSince > FTC_TIMEOUT)
                    _scanProbeDone = true; // no answer -> leave unmarked, advance
                else
                    return; // still waiting for this device
            }

            // (2) A completed probe -> consume it (mark OpenKNX + log + stream row), then advance.
            if (_scanProbeInFlight && _scanProbeDone)
            {
                ftcScanPostConsume();
                _scanOkIdx++;
                _scanProbeInFlight = 0;
                _scanProbeDone = false;
            }

            // (3) Walk the remaining entries, a bounded few per pass. Launch the next System B probe (and
            //     return to wait), or write an unprobed row (non-System-B / save-only) and advance.
            while (_scanOkIdx < _ftcListing.size())
            {
                if (!openknx.freeLoopTime()) return; // out of loop budget -> resume next pass
                const uint16_t m = (uint16_t)_ftcListing[_scanOkIdx].crc;
                const bool sysB = ((m & 0x0FFF) == 0x07B0 || (m & 0x0FFF) == 0x07B1);
                if (_scanOpenKnx && sysB)
                {
                    if (ftcScanProbeStart(_ftcListing[_scanOkIdx])) return; // one probe launched -> wait for it
                    // else: unparseable PA -> fall through, write an unprobed row + advance
                }
                ftcScanWriteRow(_ftcListing[_scanOkIdx], false); // no-op when not saving
                _scanOkIdx++;
            }

            // (4) All entries processed -> close the sink, then re-enter scanReport() to print + finish.
            _scanPostDone = true;
            if (_scanSinkOpen)
            {
                if (_scanSaveBe && _scanSaveBe->sink.close) _scanSaveBe->sink.close();
                _scanSinkOpen = false;
                openknx.logger.logWithPrefixAndValues("FTC", "scan saved: %s (%u device rows)", _scanSavePath, _scanSaveN);
            }
            scanReport();
            ftcFinish();
            return;
        }

        case FtcDevDescr:
        {
            // Mask (DeviceDescriptor) arrives via the scan ring; then ask for the FTM version.
            if (_ftcDdTail != _ftcDdHead)
            {
                const FtcDdMsg m = _ftcDdQ[_ftcDdTail];
                _ftcDdTail = (uint8_t)((_ftcDdTail + 1) & (FTC_DD_Q - 1));
                if (m.pa == _ftcTarget)
                {
                    _devMask = m.mask;
                    _devHasMask = true;
                }
            }
            // Move on as soon as the mask is in, or after the timeout (device may not answer DD at all).
            if (_devHasMask || millis() - _ftcSince > FTC_TIMEOUT)
            {
                _ftcTxLen = 0;
                if (ftcSend(FTC_CMD_MODULE_VERSION, 0))
                    _ftcState = FtcDevVer;
                else if (_scanInfoActive) // scan FULL probe: cannot send -> bail to the next device, not abort the scan
                    ftcScanInfoBail();
                else
                    ftcDevReport(), ftcFinish();
            }
            return;
        }

        case FtcDevVer:
        {
            if (_ftcRespPending)
            {
                _ftcRespPending = false;
                // The router mirrors answers TP->IP, so a late duplicate of a PREVIOUS command can land
                // in this window. Only accept the echo whose propertyId is the command we sent here.
                if (_ftcRespProp != FTC_CMD_MODULE_VERSION) return;
                if (_ftcRespLen >= 6) // 6 bytes: major/minor/revision, each big-endian
                {
                    _devVerMaj = (uint16_t)((_ftcResp[0] << 8) | _ftcResp[1]);
                    _devVerMin = (uint16_t)((_ftcResp[2] << 8) | _ftcResp[3]);
                    _devVerRev = (uint16_t)((_ftcResp[4] << 8) | _ftcResp[5]);
                    _devHasVer = true;
                }
                _ftcTxLen = 0;
                if (ftcSend(FTC_CMD_CHECK_FEATURES, 0))
                    _ftcState = FtcDevFeat;
                else
                {
                    _devPropStep = 0;
                    ftcDevSendProp();
                    _ftcState = FtcDevProp;
                }
            }
            else if (millis() - _ftcSince > FTC_TIMEOUT)
            {
                // No ModuleVersion answer. If a mask came back the device is still there (just no FTC
                // server) -> read its Device-Object identity. If nothing answered at all, it is absent.
                if (_devHasMask)
                {
                    _devPropStep = 0;
                    ftcDevSendProp();
                    _ftcState = FtcDevProp;
                }
                else if (_scanInfoActive) // scan FULL probe: nothing answered -> bail to the next device
                {
                    ftcScanInfoBail();
                }
                else
                {
                    ftcDevReport();
                    ftcFinish();
                }
            }
            return;
        }

        case FtcDevFeat:
        {
            if (_ftcRespPending)
            {
                _ftcRespPending = false;
                if (_ftcRespProp != FTC_CMD_CHECK_FEATURES) return; // stale duplicate -> keep waiting
                if (_ftcRespLen >= 1) _devFeat = _ftcResp[0];
            }
            else if (millis() - _ftcSince <= FTC_TIMEOUT)
            {
                return; // still waiting for the feature flags
            }
            // features in (or timed out) -> on to the Device-Object identity properties.
            _devPropStep = 0;
            ftcDevSendProp();
            _ftcState = FtcDevProp;
            return;
        }

        case FtcDevProp:
        {
            if (_propPending)
            {
                _propPending = false;
                // Store by the RESPONSE's propertyId, not _devPropStep: a late duplicate of a prior read
                // must land in ITS field, not the one we are currently waiting on.
                if (_propObj == 0)
                {
                    switch (_propPid)
                    {
                        case FTC_PID_SERIAL:
                            if (_propLen >= 6)
                            {
                                memcpy(_devSerial, _propData, 6);
                                _devMfr = (uint16_t)((_propData[0] << 8) | _propData[1]);
                                _devHasSerial = true;
                            }
                            break;
                        case FTC_PID_ORDER:
                            if (_propLen > 0)
                            {
                                const uint8_t n = _propLen > sizeof(_devOrder) ? (uint8_t)sizeof(_devOrder) : _propLen;
                                memset(_devOrder, 0, sizeof(_devOrder));
                                memcpy(_devOrder, _propData, n);
                                _devHasOrder = true;
                            }
                            break;
                        case FTC_PID_VERSION:
                            if (_propLen > 0)
                            {
                                _devFwLen = _propLen > sizeof(_devFw) ? (uint8_t)sizeof(_devFw) : _propLen;
                                memcpy(_devFw, _propData, _devFwLen);
                                _devHasFw = true;
                            }
                            break;
                        case FTC_PID_PROGMODE:
                            if (_propLen >= 1)
                            {
                                _devProgMode = _propData[0];
                                _devHasProg = true;
                            }
                            break;
                        case FTC_PID_HARDWARE:
                            if (_propLen >= 6)
                            {
                                memcpy(_devHw, _propData, 6);
                                _devHasHw = true;
                            }
                            break;
                    }
                }
                // Advance only when THIS step's property answered; a stale answer for another PID was
                // stored above but we keep waiting for the one we asked for.
                if (_propPid == FTC_DEV_PIDS[_devPropStep])
                    _devPropStep++;
                else
                    return;
            }
            else if (millis() - _ftcSince > FTC_TIMEOUT)
            {
                _devPropStep++; // this property never answered -> skip it
            }
            else
            {
                return; // still waiting
            }

            if (_devPropStep >= FTC_DEV_PROP_COUNT)
            {
                if (_scanInfoActive)
                {
                    ftcScanInfoBail();
                    return;
                } // scan info-probe: skip phase-2 enum, hand the identity back
                // Device-Object identity done -> phase 2: enumerate objects to find app-program + tables.
                _devObjProbe = 1;
                ftcDevSendObjType();
                _ftcState = FtcDevEnum;
            }
            else
            {
                ftcDevSendProp();
            }
            return;
        }

        case FtcDevEnum:
        {
            if (_propPending)
            {
                _propPending = false;
                // Answers are all PID_OBJECT_TYPE; tell them apart by the object index they came from.
                if (_propObj == _devObjProbe)
                {
                    if (_propLen >= 2)
                    {
                        const uint16_t ot = (uint16_t)((_propData[0] << 8) | _propData[1]);
                        if (ot == FTC_OT_ADDR) _devIdxAddr = (int8_t)_devObjProbe;
                        else if (ot == FTC_OT_ASSOC)
                            _devIdxAssoc = (int8_t)_devObjProbe;
                        else if (ot == FTC_OT_APP)
                            _devIdxApp = (int8_t)_devObjProbe;
                        else if (ot == FTC_OT_GRP)
                            _devIdxGrp = (int8_t)_devObjProbe;
                    }
                    _devObjProbe++;
                }
                else
                {
                    return; // stale answer for another index -> keep waiting for this one
                }
            }
            else if (millis() - _ftcSince > FTC_TIMEOUT)
            {
                _devObjProbe++; // no answer for this index -> move on
            }
            else
            {
                return;
            }

            if (_devObjProbe > FTC_ENUM_MAX)
            {
                if (_gaMode)
                {
                    // `info ga`: skip the load-state reads; walk the GA + association tables (_devIdxAddr/_devIdxAssoc).
                    // The walk goes connection-oriented (ETS-style) -- ftcGaBeginWalk opens the T_Connect first.
                    _gaWhich = 0;
                    ftcGaBeginWalk();
                    return;
                }
                ftcDevBuildLoadQueue();
                if (_loadQN == 0)
                {
                    ftcDevReport();
                    ftcFinish();
                }
                else
                {
                    _loadQi = 0;
                    ftcDevSendLoad();
                    _ftcState = FtcDevLoad;
                }
            }
            else
            {
                ftcDevSendObjType();
            }
            return;
        }

        case FtcDevLoad:
        {
            if (_propPending)
            {
                _propPending = false;
                const uint8_t wObj = _loadQObj[_loadQi], wPid = _loadQPid[_loadQi];
                if (_propObj == wObj && _propPid == wPid)
                {
                    if (wPid == FTC_PID_PROG_VERSION && _propLen >= 5)
                    {
                        _devAppMfr = (uint16_t)((_propData[0] << 8) | _propData[1]);
                        _devAppNum = (uint16_t)((_propData[2] << 8) | _propData[3]);
                        _devAppVer = _propData[4];
                        _devHasApp = true;
                    }
                    else if (wPid == FTC_PID_LOAD_STATE && _propLen >= 1)
                    {
                        int slot = -1;
                        if (wObj == _devIdxAddr) slot = 0;
                        else if (wObj == _devIdxAssoc)
                            slot = 1;
                        else if (wObj == _devIdxApp)
                            slot = 2;
                        else if (wObj == _devIdxGrp)
                            slot = 3;
                        if (slot >= 0)
                        {
                            _devLoad[slot] = _propData[0];
                            _devLoadHas[slot] = true;
                        }
                    }
                    _loadQi++;
                }
                else
                {
                    return; // stale/duplicate -> keep waiting for the queued read
                }
            }
            else if (millis() - _ftcSince > FTC_TIMEOUT)
            {
                _loadQi++;
            }
            else
            {
                return;
            }

            if (_loadQi >= _loadQN)
            {
                ftcDevReport();
                ftcFinish();
            }
            else
            {
                ftcDevSendLoad();
            }
            return;
        }

        case FtcGaConnect:
        {
            // T_Connect opening for the (ETS-style) connection-oriented memory walk. Once up, individualSend()
            // routes every read on the connection; on no-ack we clear the half-open state and walk connectionless.
            if (knx.bau().ftcScanConnected())
            {
                _gaConnected = true; // ours to close -> ftcFinish() disconnects (never an existing ETS session)
                ftcGaAdvance();
            }
            else if (millis() - _ftcSince > FTC_CO_CONNECT_TMO)
            {
                knx.bau().ftcScanDisconnect(); // unwedge a stuck Connecting state, then fall back
                ftcGaAdvance();
            }
            return;
        }

        case FtcGaRef:
        {
            // PID_TABLE_REFERENCE answer -> the current table's memory base; then start the A_Memory_Read walk.
            if (_propPending)
            {
                _propPending = false;
                const uint8_t idx = (uint8_t)((_gaWhich == 0) ? _devIdxAddr : _devIdxAssoc);
                if (_propObj != idx || _propPid != FTC_PID_TABLE_REFERENCE)
                    return; // stale/duplicate answer for another read -> keep waiting
                if (_propLen >= 2)
                {
                    // 4-byte PDT_UNSIGNED_LONG; the memory address is the low word (big-endian).
                    _gaRef = (uint16_t)((_propData[_propLen - 2] << 8) | _propData[_propLen - 1]);
                    _gaGot = 0;
                    _gaExpect = 0;
                    _ftcSince = millis();
                    SecurityControl sec{false, None};
                    knx.bau().ftcSendMemoryRead(_ftcTarget, sec, FTC_GA_STEP, _gaRef); // CO over the open T_Connect
                    _ftcState = FtcGaMem;
                }
                else
                {
                    _gaWhich++; // no usable reference -> skip this table
                    ftcGaAdvance();
                }
            }
            else if (millis() - _ftcSince > FTC_TIMEOUT)
            {
                _gaWhich++; // no answer -> skip this table
                ftcGaAdvance();
            }
            return;
        }

        case FtcGaMem:
        {
            if (_memPending)
            {
                _memPending = false;
                if (ftcDropDup()) return; // late IP-mirror of a memory answer -> ignore
                if (_memLen == 0) return; // rejected/empty chunk -> let the timeout end the table
                _gaGot = (uint16_t)(_gaGot + _memLen);
                if (_gaGot > FTC_GA_MAX_BYTES) _gaGot = FTC_GA_MAX_BYTES;
                if (_gaExpect == 0 && _gaGot >= 2)
                {
                    // first chunk carries the entry count -> total length (address 2B/entry, assoc 4B/entry).
                    const uint16_t count = (uint16_t)((_memBuf[0] << 8) | _memBuf[1]);
                    const uint32_t need = 2u + (uint32_t)count * (_gaWhich == 0 ? 2u : 4u);
                    _gaExpect = (uint16_t)(need > FTC_GA_MAX_BYTES ? FTC_GA_MAX_BYTES : need);
                }
                if (_gaGot < _gaExpect && _gaGot < FTC_GA_MAX_BYTES)
                {
                    uint8_t step = FTC_GA_STEP;
                    if ((uint16_t)(_gaExpect - _gaGot) < step) step = (uint8_t)(_gaExpect - _gaGot);
                    _ftcSince = millis();
                    SecurityControl sec{false, None};
                    knx.bau().ftcSendMemoryRead(_ftcTarget, sec, step, (uint16_t)(_gaRef + _gaGot)); // CO over the open T_Connect
                    return;
                }
                // table complete -> parse it, then advance to the next table (or emit the report).
                ftcGaParse();
                _gaWhich++;
                ftcGaAdvance();
            }
            else if (millis() - _ftcSince > FTC_TIMEOUT)
            {
                // partial table: parse what arrived and note it (same skip-on-timeout pattern as FtcDevProp/FtcDevLoad).
                ftcOut(CONSOLE_HEADLINE_COLOR, "  (%s table read timed out -- partial)", _gaWhich == 0 ? "address" : "association");
                ftcGaParse();
                _gaWhich++;
                ftcGaAdvance();
            }
            return;
        }

        case FtcDownloadOpen:
        {
            if (_ftcRespPending)
            {
                _ftcRespPending = false;
                if (ftcDropDup()) return;
                // Answer: [0x00][size:4 BE][0x00]. Errors: 0x42 not found, 0x4 requested pkg too big.
                if (_ftcRespLen < 5 || _ftcResp[0] != 0x00)
                {
                    openknx.logger.logWithPrefixAndValues("FTC", "download open rejected (0x%02X)",
                                                          _ftcRespLen ? _ftcResp[0] : 0xFF);
                    _status.phase = FtcPhase::Failed;
                    ftcStatusMsg("open rejected");
                    ftcFinish();
                    return;
                }
                _dlSize = ((uint32_t)_ftcResp[1] << 24) | ((uint32_t)_ftcResp[2] << 16) |
                          ((uint32_t)_ftcResp[3] << 8) | _ftcResp[4];
                _status.total = _dlSize;
                _dlChunks = (uint16_t)((_dlSize + _dlPayload - 1) / _dlPayload);
                _status.chunks = _dlChunks;
                if (_dlBackend && _dlBackend->freeBytes) // pre-write space gate (backends that can report it)
                {
                    const uint64_t freeB = _dlBackend->freeBytes();
                    if (freeB < _dlSize)
                    {
                        openknx.logger.logWithPrefixAndValues("FTC", "not enough space on '%s' (need %u B, free %u B)",
                                                              _dlBackend->prefix, (unsigned)_dlSize, (unsigned)freeB);
                        _status.phase = FtcPhase::Failed;
                        ftcStatusMsg("no space");
                        ftcFinish();
                        return;
                    }
                }
                if (!_activeSink->open(_dlLocal))
                {
                    openknx.logger.logWithPrefixAndValues("FTC", "open failed on '%s' backend: \"%s\"",
                                                          _dlBackend ? _dlBackend->prefix : "", _dlLocal);
                    _status.phase = FtcPhase::Failed;
                    ftcStatusMsg("sink open failed");
                    ftcFinish();
                    return;
                }
                _dlSinkOpen = true;
                _dlStartMs = millis();
                openknx.logger.logWithPrefixAndValues("FTC", "downloading %u bytes (%u chunks, %u B/chunk)",
                                                      (unsigned)_dlSize, _dlChunks, _dlPayload);
                if (_dlSize == 0) // empty remote file -> done
                {
                    ftcCloseSink();
                    _status.ok = true;
                    ftcStatusMsg("downloaded (empty)");
                    ftcFinish();
                    return;
                }
                _dlSeq = 1;
                ftcDlSendChunk();
                _ftcState = FtcDownloadChunk;
            }
            else if (millis() - _ftcSince > FTC_TIMEOUT)
            {
                openknx.logger.logWithPrefix("FTC", "download: no open answer");
                _status.phase = FtcPhase::Failed;
                ftcFinish();
            }
            return;
        }

        case FtcDownloadChunk:
        {
            if (_ftcRespPending)
            {
                _ftcRespPending = false;
                if (ftcDropDup()) return;
                // Answer: [0x00][seq:2 BE][readed:1][data:readed][crc16:2 BE].
                if (_ftcRespLen < 6 || _ftcResp[0] != 0x00)
                {
                    openknx.logger.logWithPrefixAndValues("FTC", "download chunk error (0x%02X)",
                                                          _ftcRespLen ? _ftcResp[0] : 0xFF);
                    ftcCloseSink();
                    _status.phase = FtcPhase::Failed;
                    ftcFinish();
                    return;
                }
                const uint16_t seq = (uint16_t)((_ftcResp[1] << 8) | _ftcResp[2]);
                const uint8_t readed = _ftcResp[3];
                if ((int)4 + readed + 2 > (int)_ftcRespLen) // data + trailing CRC must fit
                {
                    openknx.logger.logWithPrefix("FTC", "download chunk truncated");
                    ftcCloseSink();
                    _status.phase = FtcPhase::Failed;
                    ftcFinish();
                    return;
                }
                const uint16_t rxCrc = (uint16_t)((_ftcResp[4 + readed] << 8) | _ftcResp[5 + readed]);
                const uint16_t calcCrc = ftcCrc16Modbus(_ftcResp + 1, (uint8_t)(readed + 3));
                if (seq != _dlSeq || rxCrc != calcCrc)
                {
                    if (++_ftcRetries > _cfgMaxRetries)
                    {
                        openknx.logger.logWithPrefixAndValues("FTC", "download chunk %u failed (seq %u, crc %04X/%04X)",
                                                              _dlSeq, seq, rxCrc, calcCrc);
                        ftcCloseSink();
                        _status.phase = FtcPhase::Failed;
                        ftcFinish();
                        return;
                    }
                    ftcDlSendChunk(); // retry the same chunk
                    return;
                }
                _ftcRetries = 0;
                if (readed > 0)
                {
                    const int w = _activeSink->write(_ftcResp + 4, readed);
                    if (w != (int)readed)
                    {
                        openknx.logger.logWithPrefix("FTC", "sink write failed (SD full?)");
                        ftcCloseSink();
                        _status.phase = FtcPhase::Failed;
                        ftcStatusMsg("write failed");
                        ftcFinish();
                        return;
                    }
                    _dlWritten += readed;
                    _dlCrc = ftcCrc32Posix(_dlCrc, _ftcResp + 4, readed, (_dlSeq == 1)); // fold received stream (chunk 1 seeds)
                    _status.done = _dlWritten;
                    _status.chunk = _dlSeq;
                    if (_dlStartMs != 0) // live throughput for `ftc status` (same as upload)
                    {
                        const uint32_t el = millis() - _dlStartMs;
                        const uint32_t liveBps = el ? (uint32_t)(((uint64_t)_dlWritten * 1000ULL) / el) : 0;
                        _status.bps = (uint16_t)(liveBps > 0xFFFF ? 0xFFFF : liveBps);
                    }
                    // Two-line "Variant D" progress, same as the upload but pointing the other way (<-).
                    ftcMaybeProgress(false, _dlSeq, _dlChunks, _dlWritten, _dlSize, _dlStartMs);
                }
                // Last chunk = a short/empty chunk OR all bytes received. The `>=` arm is essential: a file whose
                // size is an exact multiple of the chunk size returns readed==_dlPayload on its last chunk (never
                // short), and the server has already closed the file at EOF -> without it we'd request one chunk
                // too many (server answers 0x43) and wrongly report the download FAILED.
                if (readed < _dlPayload || _dlWritten >= _dlSize)
                {
                    ftcCloseSink();
                    _dlEndMs = millis(); // freeze the pure-transfer end BEFORE the verify round-trip
                    _status.done = _dlWritten;
                    // End-to-end proof (mirrors the upload): ask the target for size + CRC32, compare in
                    // FtcDownloadVerify against the received-stream CRC32 + _dlWritten. The DOWNLOAD panel prints there.
                    ftcSendInfo(); // FileInfo(43) for _ftcPath -> sets FtcVerify; re-point it to the download verify
                    if (_ftcState == FtcVerify)
                    {
                        _ftcState = FtcDownloadVerify;
                        _ftcSince = millis();
                    }
                    return;
                }
                _dlSeq++;
                ftcDlSendChunk();
            }
            else if (millis() - _ftcSince > FTC_TIMEOUT)
            {
                openknx.logger.logWithPrefixAndValues("FTC", "download: no answer for chunk %u", _dlSeq);
                ftcCloseSink();
                _status.phase = FtcPhase::Failed;
                ftcFinish();
            }
            return;
        }

        case FtcDownloadVerify:
        {
            // End-to-end proof: the target's FileInfo(43) CRC32 vs the CRC32 folded over the received stream
            // (mirrors the upload's FtcVerify). Also catches truncation via _dlWritten == _dlSize.
            if (_ftcRespPending)
            {
                _ftcRespPending = false;
                if (_ftcRespProp != FTC_CMD_FILE_INFO) return; // only the FileInfo(43) echo is our verify result
                if (_ftcRespLen != 9) return;                  // a shorter frame (close ack / stale) -> keep waiting
                const bool statOk = (_ftcResp[0] == 0x00);
                const uint32_t tsize = statOk ? (((uint32_t)_ftcResp[1] << 24) | ((uint32_t)_ftcResp[2] << 16) |
                                                 ((uint32_t)_ftcResp[3] << 8) | _ftcResp[4])
                                              : 0;
                const uint32_t tcrc = statOk ? (((uint32_t)_ftcResp[5] << 24) | ((uint32_t)_ftcResp[6] << 16) |
                                                ((uint32_t)_ftcResp[7] << 8) | _ftcResp[8])
                                             : 0;
                const uint32_t mine = _dlCrc ^ 0xFFFFFFFFu;
                const bool ok = statOk && (_dlWritten == _dlSize) && (tsize == _dlSize) && (tcrc == mine);
                _status.ok = ok;
                _status.crc = mine;
                _status.done = _dlWritten;
                _status.phase = ok ? FtcPhase::Done : FtcPhase::Failed;
                ftcDownloadPanel(ok, statOk, tcrc);
                ftcStatusMsg(ok ? "downloaded (verified)" : (statOk ? "download verify FAILED" : "downloaded (unverified)"));
                ftcFinish();
                return;
            }
            if (millis() - _ftcSince > FTC_TIMEOUT)
            {
                // No FileInfo answer (old / silent server): the bytes ARE written -> report, flagged unverified.
                _status.ok = true;
                _status.done = _dlWritten;
                _status.phase = FtcPhase::Done;
                ftcDownloadPanel(true, false, 0);
                ftcStatusMsg("downloaded (unverified)");
                ftcFinish();
            }
            return;
        }

    #ifdef OPENKNX_FTC_CONSOLE
        case FtcConsoleProbe:
        {
            // Capability pre-flight: accept only the CheckFeatures echo, cache it per PA, then open or bail.
            if (_ftcRespPending)
            {
                _ftcRespPending = false;
                if (_ftcRespProp != FTC_CMD_CHECK_FEATURES) return; // stale mirror -> keep waiting
                const uint8_t feat = (_ftcRespLen >= 1) ? _ftcResp[0] : 0;
                _ftcFeatPa = _ftcTarget;
                _ftcFeatBits = feat;
                _ftcFeatValid = true; // cache only a real answer
                conAfterProbe(feat, true);
            }
            else if (millis() - _ftcSince > FTC_FEATURE_TIMEOUT)
            {
                // Congested bus can starve a single probe. Resend the idempotent CheckFeatures a few times
                // (ftcSend re-arms _ftcSince) before giving up -> the console opens under load instead of
                // failing with a misleading "no answer".
                if (_conProbeTries < FTC_CON_PROBE_RETRIES && ftcSend(FTC_CMD_CHECK_FEATURES, 0))
                    _conProbeTries++;
                else
                    conAfterProbe(0, false); // still nothing after the retries -> absent / not an FTC device
            }
            return;
        }

        case FtcConsole:
        {
            if (_conSub == 3) // waiting for the OPEN ack -- only a genuine accept opens a session (M7/M8, #2)
            {
                if (_ftcRespPending)
                {
                    _ftcRespPending = false;
                    if (_ftcRespObj != CON_OBJECT_INDEX || _ftcRespProp != CON_PID_IN) return; // stale mirror
                    if (ftcDropDup()) return;
                    const uint8_t st = (_ftcRespLen >= 1) ? _ftcResp[0] : 0xFF;
                    if (st == 0x00) // accepted -> NOW it is a real session: stamp, hijack local input, banner, drain
                    {
                        _conStartMs = millis();
                        openknx.console.setLineSink(&consoleFeedLineStatic);
                        ftcOut(CONSOLE_HEADLINE_COLOR, "-- console %u.%u.%u -- 'quit'/'exit' to leave, 'ftc cancel' to escape --",
                               (_ftcTarget >> 12) & 0x0F, (_ftcTarget >> 8) & 0x0F, _ftcTarget & 0xFF);
                        _conSub = 2; // pull any prompt/output the takeover produced
                        conSend(CON_PID_OUT, &_conMaxDrain, 1);
                        return;
                    }
                    if (st == 0xA0) // password stage, not logged in -- guide the user, do NOT open a doomed session
                    {
                        _conSub = 0;
                        _conStartMs = 0;
                        _status.phase = FtcPhase::Failed;
                        ftcOut(CONSOLE_HEADLINE_COLOR, "%u.%u.%u: password-protected -- run: ftc %u.%u.%u login <pw>, then retry",
                               (_ftcTarget >> 12) & 0x0F, (_ftcTarget >> 8) & 0x0F, _ftcTarget & 0xFF,
                               (_ftcTarget >> 12) & 0x0F, (_ftcTarget >> 8) & 0x0F, _ftcTarget & 0xFF);
                        ftcFinish();
                        return;
                    }
                    if (st == 0xA2) // writes disabled: prog-mode stage (not in prog) or ETS access = "Aus"
                    {
                        conRefuse("console locked -- set prog mode, or ETS access is \"Aus\"");
                        return;
                    }
                    if (st == 0x01) // another PA already owns the console
                    {
                        conRefuse("console in use -- try again later");
                        return;
                    }
                    conRefuse("unexpected target response -- console not opened"); // incl. 0x43 / malformed / future codes
                    return;
                }
                else if (millis() - _ftcSince > FTC_CON_TIMEOUT) // longer window: probe proved presence, so a slow OPEN ack is congestion, not absence
                    conRefuse("no answer from the target -- console not opened");
                return;
            }
            if (_conSub == 1) // waiting for the ack of a COMMAND line (the OPEN ack is _conSub==3)
            {
                if (_ftcRespPending)
                {
                    _ftcRespPending = false;
                    if (_ftcRespObj != CON_OBJECT_INDEX || _ftcRespProp != CON_PID_IN) return; // stale mirror
                    if (ftcDropDup()) return;
                    const uint8_t st = (_ftcRespLen >= 1) ? _ftcResp[0] : 0xFF;
                    if (st == 0x00) // accepted -> pull whatever the command produced
                    {
                        _conSub = 2;
                        conSend(CON_PID_OUT, &_conMaxDrain, 1);
                        return;
                    }
                    // Reasons stay short: conClose() already frames them as "-- <pa>: <reason> (session time) --".
                    _status.phase = FtcPhase::Failed; // any non-OK ends the session with a clear reason (no zombie)
                    if (st == 0xA0) // PW window lapsed mid-session: the server re-gates each command (F1a)
                        conClose("authorization expired -- log in again", true);
                    else if (st == 0xA2) // locked mid-session (prog mode left, or mode reprogrammed)
                        conClose("console locked", true);
                    else if (st == 0x43) // target dropped the session (idle-reaped or closed)
                        conClose("target idle timeout", true);
                    else // 0x01 previous command still running, or any unknown code -> fail safe
                        conClose("target busy or unexpected response", true);
                }
                else if (millis() - _ftcSince > FTC_CON_TIMEOUT) // congestion-tolerant: wait, do NOT resend (command is not idempotent)
                {
                    _status.phase = FtcPhase::Failed;
                    conClose("no answer from target", true);
                }
                return;
            }
            if (_conSub == 2) // draining the target's log ring
            {
                if (_ftcRespPending)
                {
                    _ftcRespPending = false;
                    if (_ftcRespObj != CON_OBJECT_INDEX || _ftcRespProp != CON_PID_OUT) return; // stale mirror
                    if (ftcDropDup()) return;
                    if (_ftcRespLen >= 1 && _ftcResp[0] != 0x00) // 0x43 = session gone on the target (#3: kill the zombie)
                    {
                        _status.phase = FtcPhase::Failed;
                        conClose("target idle timeout", false);
                        return;
                    }
                    const uint8_t more = (_ftcRespLen >= 2) ? _ftcResp[1] : 0;
                    const uint8_t ovf = (_ftcRespLen >= 3) ? _ftcResp[2] : 0;
                    // Payload is already-formatted remote console text -> write it verbatim to our serial
                    // (bounded <=247 B), NOT through logger.log() which would re-timestamp every chunk. Hold
                    // the logger mutex so it cannot interleave with a concurrent log line (ESP dual-core).
                    if (_ftcRespLen > 3)
                    {
                        logBegin();
                        OPENKNX_LOGGER_DEVICE.write(_ftcResp + 3, _ftcRespLen - 3);
                        logEnd();
                    }
                    if (ovf) ftcOut(31, "[...output truncated...]");
                    if (more)
                        conSend(CON_PID_OUT, &_conMaxDrain, 1); // keep draining
                    else
                    {
                        _conSub = 0; // remote prompt already streamed through -> idle
                        _conKeepNext = millis() + CON_KEEP_MS;
                    }
                }
                else if (millis() - _ftcSince > FTC_CON_TIMEOUT) // congestion-tolerant per-chunk window; re-armed on every chunk that arrives -> big outputs drain slowly but completely under load
                {
                    _status.phase = FtcPhase::Failed; // F1: a silent-drain timeout is an error, not a clean exit
                    // sendClose = true: the target likely still owns the session (e.g. a drain answer that never
                    // crossed a constrained interface); the 1-byte CLOSE fits any tunnel and releases it now
                    // instead of leaving it wedged until CON_IDLE_TMO. Fire-and-forget: harmless if it is gone.
                    conClose("no answer while draining", true);
                }
                return;
            }
            // _conSub == 0: idle in-session -> periodic keepalive poll (async logs + keep the session fresh).
            // Suppress the poll while the user is actively typing (last local line < CON_KEEP_MS ago): over a
            // TP tunnel each drain round-trips 1-2 s, and a line typed into that window would be dropped as
            // "busy". Deferring keeps _conSub==0 so the command is sent immediately; async logs still flow once
            // typing pauses for CON_KEEP_MS (and every command's own drain carries the ring in the meantime).
            if (millis() >= _conKeepNext && (millis() - _conLastInputMs) >= CON_KEEP_MS)
            {
                _conKeepNext = millis() + CON_KEEP_MS;
                conSend(CON_PID_OUT, &_conMaxDrain, 1);
                _conSub = 2;
            }
            return;
        }
    #endif

        default:
            return;
    }
}

// One line in the global `help`; the full colored usage lives in the console (`ftc ?`).
void FileTransferClient::showHelp()
{
    openknx.console.printHelpLine("ftc", "KNX file transfer client, PA->PA. Type 'ftc ?' for usage.");
}

// All parsing and help are in FileTransferClientConsole; this just hands off.
bool FileTransferClient::processCommand(const std::string cmd, bool diagnoseKo)
{
    if (diagnoseKo)
        return false;
    return _console.processCommand(cmd);
}

FileTransferClient openknxFileTransferClient;
#endif

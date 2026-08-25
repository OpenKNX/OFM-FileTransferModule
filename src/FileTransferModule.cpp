/**
 * @file        FileTransferModule.cpp
 * @brief       KNX file transfer SERVER: serves file, directory, firmware-update and console commands
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 */
#if defined(ARDUINO_ARCH_RP2040) || defined(ARDUINO_ARCH_ESP32)
#include "FileTransferConfig.h" // switches first -- every guard below depends on it
#include "FileTransferModule.h"
#ifdef OPENKNX_FTC_DELTA_UPDATE
extern "C" {
    #include "third_party/uzlib/uzlib.h"
}
#endif
#include "versions.h"
#ifdef OPENKNX_SDCARD
    #include "SdFileStore.h" // sd::fileStore for the server-side `ll sd/…` listing (SdFat-free header)
#endif
#ifdef OPENKNX_EXTFLASH
    #include "EfcFileStore.h" // efc::fileStore for the server-side `ll efc/…` listing
#endif
#ifdef ARDUINO_ARCH_RP2040
#include <PicoOTA.h>
#elif defined(ARDUINO_ARCH_ESP32)

    #include <esp_ota_ops.h> // esp_ota_get_running_partition: the chip the running image was built for
#include <Update.h>
#endif
#ifdef OPENKNX_FTC_SECURITY
#include "knx/aes.hpp" // C++ wrapper (extern "C") around the AES already linked by knx; zero extra flash
// Fixed public constant, domain separation for the nonce seed only. Not a secret.
static const uint8_t FTM_SEC_K0[16] = {0x4F, 0x70, 0x65, 0x6E, 0x4B, 0x4E, 0x58, 0x46, 0x54, 0x43, 0x53, 0x65, 0x63, 0x75, 0x72, 0x65};
#endif

#ifdef OPENKNX_FTC_CONSOLE
// OGM-Common compat: the two-argument (reason) form is picked where it compiles, else the reason is
// dropped, so every product builds against whichever OGM-Common it pins.
template <typename C>
static auto ftcDisableConsole(C& c, bool disable, const char* reason, int)
    -> decltype(c.disableConsole(disable, reason), void())
{
    c.disableConsole(disable, reason);
}
template <typename C>
static void ftcDisableConsole(C& c, bool disable, const char* /*reason*/, long)
{
    c.disableConsole(disable);
}
#endif

// Module name, shown in log output.
const std::string FileTransferModule::name()
{
    return "FileTransfer";
}

// Module version, shown in command info.
const std::string FileTransferModule::version()
{
    return MODULE_FileTransferModule_Version;
}

void FileTransferModule::loop(bool configured)
{
    // Auto-close file/dir after HEARTBEAT_INTERVAL with no heartbeat.
    if (_fileOpen && delayCheck(_heartbeat, HEARTBEAT_INTERVAL))
    {
        ftmXferClose();
        _fileOpen = false;
        logErrorP("File closed due no heartbeat");
    }

    if (_dirOpen && delayCheck(_heartbeat, HEARTBEAT_INTERVAL))
    {
        _dirOpen = false;
        logErrorP("Directory closed due no heartbeat");
    }

    if (_rebootRequested && delayCheck(_rebootRequested, 2000))
    {
        openknx.flash.save();
        openknx.restart();
    }

#ifdef OPENKNX_FTC_CONSOLE
    conLoop();
#endif
#ifdef OPENKNX_FTC_DELTA_UPDATE
    deltaLoop(); // advance a running delta apply by one slice (non-blocking)
    probeLoop(); // advance the source checksum a client is asking about (non-blocking)
#endif
    crcLoop(); // advance a cooperative file-CRC job (LittleFS/SD/EFC), if one is running (non-blocking)
}

enum class FtmCommands
{
    Format,
    Exists, // LittleFS.exists(path)
    Rename,
    FileUpload = 40,
    FileDownload,
    FileDelete,
    FileInfo,
    FileUploadFast = 44, // open(answered) / data(SILENT) / close(answered) -- keeps classic cmd40 untouched
    FileReport = 45,     // received-bitmap gap query (answered)
    FilesystemInfo = 46, // LittleFS total + used bytes -> `ftc df` and the pre-upload free-space check
    DirList = 80,
    DirCreate,
    DirDelete,
    Cancel = 90,
    ModuleVersion = 100,
    FwUpdate,
    CheckFeatures,
    AuthChallenge = 103, // FTC access control: request a nonce (OPENKNX_FTC_SECURITY)
    AuthResponse = 104,  // FTC access control: submit the MAC over the nonce
    AuthLogout = 105, // FTC access control: close the authorized window immediately
    // 106 is taken: FwProbe (delta update). Kept out of the list unless that feature is compiled in,
    // because merely naming it here shifts this switch's jump table -- but do not reuse the number.
#ifdef OPENKNX_FTC_DELTA_UPDATE
    FwProbe = 106 // delta update: does the running image match (len, crc)? also carries job status
#endif
};

// --- Drive routing: a path targets LittleFS (default), the SD card ("sd/…") or ext-flash ("efc/…"). Each op
// strips the prefix and routes to that provider's identical-API store. Everything compiles to LittleFS-only
// when neither module is present, so the default behaviour is byte-for-byte unchanged. ---
namespace
{
enum FtmDrive
{
    FD_INT = 0,
    FD_SD = 1,
    FD_EFC = 2
};

// Classify @p path; on a match, advance @p rel past the prefix (keeping the leading '/') and return the drive.
uint8_t ftmDrive(const char *path, const char **rel)
{
    *rel = path;
#ifdef OPENKNX_SDCARD
    if (strncmp(path, "sd", 2) == 0 && path[2] == '/')
    {
        *rel = path + 2;
        return FD_SD;
    }
#endif
#ifdef OPENKNX_EXTFLASH
    if (strncmp(path, "efc", 3) == 0 && path[3] == '/')
    {
        *rel = path + 3;
        return FD_EFC;
    }
#endif
    return FD_INT;
}

bool ftmExists(const char *path)
{
    const char *r;
    switch (ftmDrive(path, &r))
    {
#ifdef OPENKNX_SDCARD
        case FD_SD: return sd::fileStore.exists(r);
#endif
#ifdef OPENKNX_EXTFLASH
        case FD_EFC: return efc::fileStore.exists(r);
#endif
        default: return LittleFS.exists(r);
    }
}
bool ftmRemove(const char *path)
{
    const char *r;
    switch (ftmDrive(path, &r))
    {
#ifdef OPENKNX_SDCARD
        case FD_SD: return sd::fileStore.remove(r);
#endif
#ifdef OPENKNX_EXTFLASH
        case FD_EFC: return efc::fileStore.remove(r);
#endif
        default: return LittleFS.remove(r);
    }
}
bool ftmMkdir(const char *path)
{
    const char *r;
    switch (ftmDrive(path, &r))
    {
#ifdef OPENKNX_SDCARD
        case FD_SD: return sd::fileStore.mkdir(r);
#endif
#ifdef OPENKNX_EXTFLASH
        case FD_EFC: return efc::fileStore.mkdir(r);
#endif
        default: return LittleFS.mkdir(r);
    }
}
bool ftmRmdir(const char *path)
{
    const char *r;
    switch (ftmDrive(path, &r))
    {
#ifdef OPENKNX_SDCARD
        case FD_SD: return sd::fileStore.rmdir(r);
#endif
#ifdef OPENKNX_EXTFLASH
        case FD_EFC: return efc::fileStore.rmdir(r);
#endif
        default: return LittleFS.rmdir(r);
    }
}
bool ftmRename(const char *oldPath, const char *newPath)
{
    const char *ro, *rn;
    const uint8_t d = ftmDrive(oldPath, &ro);
    const uint8_t dn = ftmDrive(newPath, &rn);
    // A rename never crosses a drive. When the new path names one explicitly and it is not the old path's,
    // refuse: assuming "same drive" renames on the SOURCE drive under the stripped name, hitting a file the
    // caller never asked for. A new path WITHOUT a prefix keeps its old meaning: the same drive.
    if (dn != FD_INT && dn != d) return false;
    switch (d)
    {
#ifdef OPENKNX_SDCARD
        case FD_SD: return sd::fileStore.rename(ro, rn);
#endif
#ifdef OPENKNX_EXTFLASH
        case FD_EFC: return efc::fileStore.rename(ro, rn);
#endif
        default: return LittleFS.rename(ro, rn);
    }
}
} // namespace

bool FileTransferModule::checkOpenFile(uint8_t *resultData, uint8_t &resultLength)
{
    if (_fileOpen)
    {
        resultLength = 1;
        pushByte(0x41, resultData);
        logErrorP("File already open");
        return true;
    }
    return false;
}

bool FileTransferModule::checkOpenedFile(uint8_t *resultData, uint8_t &resultLength)
{
    if (!_fileOpen)
    {
        resultLength = 1;
        pushByte(0x43, resultData);
        logErrorP("File not opened");
        return false;
    }
    return true;
}

bool FileTransferModule::checkOpenDir(uint8_t *resultData, uint8_t &resultLength)
{
    if (_dirOpen)
    {
        resultLength = 1;
        pushByte(0x81, resultData);
        logErrorP("Dir already open");
        return true;
    }
    return false;
}

bool FileTransferModule::checkOpenedDir(uint8_t *resultData, uint8_t &resultLength)
{
    if (!_dirOpen)
    {
        resultLength = 1;
        pushByte(0x83, resultData);
        logErrorP("Dir not opened");
        return false;
    }
    return true;
}

// Open the download source on the drive named by the path prefix. Returns the file size, or -1.
int32_t FileTransferModule::ftmXferOpenRead(const char *path)
{
    const char *rel;
    _xferDrive = ftmDrive(path, &rel);
    _xferWrite = false;
    _xferSize = 0;
    if (_xferDrive == FD_INT)
    {
        _file = LittleFS.open(path, "r");
        if (!_file) return -1;
        _xferSize = (uint32_t)_file.size();
        return (int32_t)_xferSize;
    }
    int32_t sz = -1;
#ifdef OPENKNX_SDCARD
    if (_xferDrive == FD_SD) sz = sd::fileStore.open(rel);
#endif
#ifdef OPENKNX_EXTFLASH
    if (_xferDrive == FD_EFC) sz = efc::fileStore.open(rel);
#endif
    if (sz < 0) return -1;
    _xferSize = (uint32_t)sz;
    return sz;
}

// Open the upload sink on the named drive. resume -> open existing (r+, no truncate); else create/truncate.
// sizeHint (exact total size, 0 = unknown) -> the SD store pre-allocates the whole file on a fresh write so
// exFAT never allocates a cluster mid-transfer (no loop-blocking 128 KB boundary stall). Internal LittleFS
// and efc ignore it (small blocks, no contiguous preAllocate).
bool FileTransferModule::ftmXferOpenWrite(const char *path, bool resume, uint32_t sizeHint)
{
    const char *rel;
    _xferDrive = ftmDrive(path, &rel);
    _xferWrite = true;
    if (_xferDrive == FD_INT)
    {
        _file = LittleFS.open(path, resume ? "r+" : "w");
        return (bool)_file;
    }
#ifdef OPENKNX_SDCARD
    if (_xferDrive == FD_SD) return sd::fileStore.sinkOpen(rel, resume ? 1u : 0u, sizeHint);
#endif
#ifdef OPENKNX_EXTFLASH
    if (_xferDrive == FD_EFC) return efc::fileStore.sinkOpen(rel, resume ? 1u : 0u, sizeHint);
#endif
    return false;
}

// Close the active transfer handle (LittleFS _file, or the store's src/sink) for the current drive.
void FileTransferModule::ftmXferClose()
{
    if (_xferDrive == FD_INT)
    {
        if (_xferWrite) _file.flush();
        _file.close();
        return;
    }
#ifdef OPENKNX_SDCARD
    if (_xferDrive == FD_SD)
    {
        if (_xferWrite) sd::fileStore.sinkClose();
        else sd::fileStore.close();
        return;
    }
#endif
#ifdef OPENKNX_EXTFLASH
    if (_xferDrive == FD_EFC)
    {
        if (_xferWrite) efc::fileStore.sinkClose();
        else efc::fileStore.close();
    }
#endif
}

void FileTransferModule::readFile(uint16_t sequence, uint8_t *resultData, uint8_t &resultLength)
{
    logIndentUp();

    pushByte(0x0, resultData);
    pushWord(sequence, resultData + 1);
    uint8_t readed = 0;
    bool done = true;
    if (_xferDrive == FD_INT)
    {
        if (_lastSequence + 1 != sequence)
            _file.seek((sequence - 1) * (_size));
        readed = _file.readBytes((char *)resultData + 4, _size);
        done = (readed == 0 || !_file.available());
    }
    else
    {
        const uint32_t off = (uint32_t)(sequence - 1) * _size;
#ifdef OPENKNX_SDCARD
        if (_xferDrive == FD_SD) readed = sd::fileStore.read(off, resultData + 4, _size);
#endif
#ifdef OPENKNX_EXTFLASH
        if (_xferDrive == FD_EFC) readed = efc::fileStore.read(off, resultData + 4, _size);
#endif
        done = (readed == 0 || off + readed >= _xferSize);
    }
    pushByte(readed, resultData + 3);

    logDebugP("Readed sequence %i (%i/%i bytes)", sequence, readed, _size);
    if (done)
    {
        ftmXferClose();
        _fileOpen = false;
        logInfoP("The file download was successfully completed");
    }

    FastCRC16 crc16;
    uint16_t crc = crc16.modbus(resultData + 1, readed + 3);
    pushWord(crc, resultData + readed + 4);
    logTraceP("CRC16 (Modbus): 0x%04X", crc);

    resultLength = readed + 6;

    logIndentDown();
}

/**
 * @brief Shared position-write core: seek to (sequence-1)*_size, write n payload bytes; true iff seek AND full write ok.
 *
 * Seeks on every call by design: the fast path writes out of order, so a resend of the same seq must
 * land at its absolute offset, not the current position (an in-order seek is a littlefs no-op = free).
 */
bool FileTransferModule::writeChunk(uint16_t sequence, const uint8_t *payload, uint8_t n)
{
    const uint32_t off = (uint32_t)(sequence - 1) * _size;
    if (_xferDrive == FD_INT)
    {
        if (!_file.seek(off)) return false;
        return _file.write(payload, n) == n;
    }
#ifdef OPENKNX_SDCARD
    if (_xferDrive == FD_SD) return sd::fileStore.sinkWriteAt(off, payload, n) == n;
#endif
#ifdef OPENKNX_EXTFLASH
    if (_xferDrive == FD_EFC) return efc::fileStore.sinkWriteAt(off, payload, n) == n;
#endif
    return false;
}

void FileTransferModule::writeFile(uint16_t sequence, uint8_t *data, uint8_t length, uint8_t *resultData, uint8_t &resultLength)
{
    logIndentUp();

    // Reject a frame whose claimed chunk length (data[2]) runs past the received APDU (3-byte header + payload).
    // The fast DATA path guards this; the classic path did not -> writeChunk would read up to 255 B of adjacent
    // memory into the file. length is the on-wire APDU octet count.
    if ((uint16_t)3 + data[2] > length)
    {
        pushByte(0x47, resultData);
        resultLength = 1;
        logErrorP("Chunk length exceeds the received frame");
        logIndentDown();
        return;
    }

    #ifdef OPENKNX_DEBUG
    if (_lastSequence + 1 != sequence)
        logDebugP("Not continous sequence - seek to position %d [expected %i, got %i]",
                  (uint32_t)((sequence - 1) * _size), _lastSequence + 1, sequence);
    size_t filePos = _file.position();
    #endif

    // Position-write through the shared core. On failure, re-test the seek so the classic answer
    // keeps the exact two codes it always had: 0x46 = seek failed, 0x47 = short write (fs full).
    if (!writeChunk(sequence, (const uint8_t *)data + 3, data[2]))
    {
        if (_xferDrive == FD_INT && !_file.seek((uint32_t)(sequence - 1) * _size))
        {
            pushByte(0x46, resultData);
            resultLength = 1;
            logErrorP("The file can't seek to position");
        }
        else
        {
            pushByte(0x47, resultData);
            resultLength = 1;
            logErrorP("The file could not be written completely");
        }
        logIndentDown();
        return;
    }

    #ifdef OPENKNX_DEBUG
    logDebugP("Write sequence %i (%i bytes) %i.%i", sequence, data[2], filePos, _file.position() - 1);
    #endif

    FastCRC16 crc16;
    uint16_t crc = crc16.modbus(data, length);

    pushByte(0x0, resultData);
    pushWord(sequence, resultData + 1);
    pushWord(crc, resultData + 3);
    resultLength = 5;
    _lastSequence = sequence;

    logIndentDown();
}

bool FileTransferModule::processFunctionProperty(uint8_t objectIndex, uint8_t propertyId, uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength)
{
#ifdef OPENKNX_FTC_CONSOLE
    if (objectIndex == CON_OBJECT_INDEX)
    {
        _lastAccess = millis();
        openknx.common.skipLooptimeWarning();
        return conFunctionProperty(propertyId, length, data, resultData, resultLength);
    }
#endif
    if (objectIndex != 159) return false;
    _lastAccess = millis();
    openknx.common.skipLooptimeWarning();

#ifdef OPENKNX_FTC_SECURITY
    // The auth handshake itself carries no write side effect -> always processable.
    if ((FtmCommands)propertyId == FtmCommands::AuthChallenge)
    {
        cmdAuthChallenge(resultData, resultLength);
        return true;
    }
    if ((FtmCommands)propertyId == FtmCommands::AuthResponse)
    {
        cmdAuthResponse(length, data, resultData, resultLength);
        return true;
    }
    if ((FtmCommands)propertyId == FtmCommands::AuthLogout)
    {
        // Explicit logout: close the window now. Unconditional + harmless if already closed.
        _authorized = false;
        _authLastMs = 0;
        _challengePending = false;
        resultData[0] = 0x00;
        resultLength = 1;
        return true;
    }
    // Stage "Off" locks the WHOLE file transfer (reads included). CheckFeatures stays answerable so a client
    // can still discover the device is locked. Other stages gate writes only.
    if (secStage() == FTM_SEC_OFF && knx.configured() && (FtmCommands)propertyId != FtmCommands::CheckFeatures)
    {
        resultData[0] = ST_WRITES_DISABLED;
        resultLength = 1;
        return true;
    }
    // Gate WRITE commands; reads (download/info/list/df/...) stay open in stages 1-3.
    if (secIsWriteCommand(propertyId) && !secWriteAllowed())
    {
        // Password stage -> tell the client to authenticate; otherwise writes are simply off (Off handled above).
        resultData[0] = (secStage() == FTM_SEC_PW) ? ST_AUTH_REQUIRED : ST_WRITES_DISABLED;
        resultLength = 1;
        // Which command was turned away, and on what grounds. Without this the console shows nothing at
        // all while the other end reports a refusal, and neither side can tell a closed window from a
        // wrong password or from writes being switched off entirely.
        logInfoP("write refused (cmd %u): %s", (unsigned)propertyId,
                 (secStage() == FTM_SEC_PW) ? "not signed in / session expired"
                                            : (secStage() == FTM_SEC_PROG ? "programming button not pressed"
                                                                          : "writes are switched off"));
        return true;
    }
    if (secIsWriteCommand(propertyId)) secRefreshWindow(); // accepted write extends the idle window
#endif

    // A cmdFileInfo CRC job holds a persistent file/store handle across loop() passes. Any OTHER command means
    // the client moved on -> drop it, so a format/upload/delete can never run against an open handle (UAF /
    // concurrent second handle). FileInfo manages its own job (re-poll same path / switch path).
    if ((FtmCommands)propertyId != FtmCommands::FileInfo) crcCancel();

#ifdef OPENKNX_FTC_DELTA_UPDATE
    // A running delta apply owns the single open file and, on ESP32, an open OTA slot. Anything that
    // would touch either is refused while it runs -- except Cancel, which is how it is stopped, and the
    // two commands a client needs to watch it.
    if (deltaBusy())
    {
        switch ((FtmCommands)propertyId)
        {
            case FtmCommands::Cancel:
                // Release everything, then clear the record: a cancel is not a failure, and a later probe
                // asking about a base must not be answered with "the last update failed".
                deltaStop(FirmwarePatch::ERR_READ);
                _deltaPhase = DELTA_IDLE;
                _deltaError = FirmwarePatch::ERR_NONE;
                logInfoP("delta update cancelled by the client");
                resultData[0] = 0x00;
                resultLength = 1;
                return true;
            case FtmCommands::FwUpdate:
                // Keep this command's contract: it answers nothing, ever. A status byte here would be a
                // new answer to a command no client waits on.
                logErrorP("an update is already running -- ignored");
                return false;
            case FtmCommands::FwProbe:
            case FtmCommands::CheckFeatures:
            case FtmCommands::ModuleVersion:
                break; // read-only, safe to answer while the job runs
            default:
                resultData[0] = 0x4C; // busy: an update is being applied
                resultLength = 1;
                return true;
        }
    }
#endif

    switch ((FtmCommands)propertyId)
    {
        case FtmCommands::Format:
        {
            cmdFormat(length, data, resultData, resultLength);
            return true;
        }

        case FtmCommands::Exists:
        {
            cmdExists(length, data, resultData, resultLength);
            return true;
        }

        case FtmCommands::Rename:
        {
            cmdRename(length, data, resultData, resultLength);
            return true;
        }

#ifdef OPENKNX_FTC_DOWNLOAD
        case FtmCommands::FileDownload:
        {
            cmdFileDownload(length, data, resultData, resultLength);
            return true;
        }
#endif

        case FtmCommands::FileUpload:
        {
            cmdFileUpload(length, data, resultData, resultLength);
            return true;
        }

        case FtmCommands::FileDelete:
        {
            cmdFileDelete(length, data, resultData, resultLength);
            return true;
        }

#ifdef OPENKNX_FTC_DIROPS
        case FtmCommands::DirCreate:
        {
            cmdDirCreate(length, data, resultData, resultLength);
            return true;
        }

        case FtmCommands::DirDelete:
        {
            cmdDirDelete(length, data, resultData, resultLength);
            return true;
        }

        case FtmCommands::DirList:
        {
            cmdDirList(length, data, resultData, resultLength);
            return true;
        }
#endif

        case FtmCommands::Cancel:
        {
            cmdCancel(length, data, resultData, resultLength);
            return false;
        }

        case FtmCommands::FileInfo:
        {
            cmdFileInfo(length, data, resultData, resultLength);
            return true;
        }

#ifdef OPENKNX_FTC_FASTUPLOAD
        case FtmCommands::FileUploadFast:
        {
            // Return is the "handled" flag: open/close true (answered), a DATA frame false so no L7
            // response is sent -- the silent fast-data path.
            return cmdFileUploadFast(length, data, resultData, resultLength);
        }

        case FtmCommands::FileReport:
        {
            cmdFileReport(length, data, resultData, resultLength);
            return true;
        }
#endif

        case FtmCommands::FilesystemInfo:
        {
            cmdFilesystemInfo(length, data, resultData, resultLength);
            return true;
        }

        case FtmCommands::ModuleVersion:
        {
            cmdModuleVersion(length, data, resultData, resultLength);
            return true;
        }

        case FtmCommands::CheckFeatures:
        {
            cmdCheckFeatures(length, data, resultData, resultLength);
            return true;
        }
#if defined(ARDUINO_ARCH_RP2040) || defined(ARDUINO_ARCH_ESP32)
        case FtmCommands::FwUpdate:
        {
            cmdFwUpdate(length, data, resultData, resultLength);
            return false; // never answers: it sets no result, and on success the device restarts
        }
#endif
#ifdef OPENKNX_FTC_DELTA_UPDATE
        case FtmCommands::FwProbe:
        {
            cmdFwProbe(length, data, resultData, resultLength);
            return true;
        }
#endif
    }
    return false;
}

#ifdef OPENKNX_FTC_CONSOLE
// Console-tunnel FunctionProperty branch (obj 160). Runs in the KNX stack dispatch, so it only parks a
// line / copies a bounded ring window (<=247 B) -- the command itself runs later in conLoop().
bool FileTransferModule::conFunctionProperty(uint8_t pid, uint8_t len, uint8_t *data, uint8_t *res, uint8_t &resLen)
{
    if (pid == CON_PID_IN)
    {
        const uint8_t flags = (len > 0) ? data[0] : 0;
        if (flags & 0x01) // OPEN
        {
#ifdef OPENKNX_FTC_SECURITY
            // Console take-over is a write action: gate it on the same global window (finding C -> one auth
            // mechanism, no separate console sub-flag). Client answers 0xA0 by running the 103/104 handshake.
            if (!secWriteAllowed())
            {
                res[0] = (secStage() == FTM_SEC_PW) ? ST_AUTH_REQUIRED : ST_WRITES_DISABLED;
                resLen = 1;
                return true;
            }
            secRefreshWindow();
#endif
            if (_conActive)
            {
                // Single-owner console: a different owner is refused; the same owner re-opening takes over its
                // own lost session (died without CLOSE) instead of waiting out the idle timeout.
                const uint16_t reqPa = (len >= 3) ? (uint16_t)((data[1] << 8) | data[2]) : 0;
                if (reqPa == 0 || reqPa != _conOwnerPa)
                {
                    res[0] = 0x01; // BUSY: a session is already owned by someone else
                    resLen = 1;
                    return true;
                }
                logInfoP("Console re-opened by same owner %u.%u.%u -- resetting session", (reqPa >> 12) & 0x0F, (reqPa >> 8) & 0x0F, reqPa & 0xFF);
            }
            _conActive = true;
            _conCmdPending = false;
            _conOverflow = false;
            _conCursor = openknx.logger.ringWritePos(); // start "now"
            _conOwnerPa = (len >= 3) ? (uint16_t)((data[1] << 8) | data[2]) : 0;
            _conLastAccess = millis();
            snprintf(_conOwnerStr, sizeof(_conOwnerStr), "remote %u.%u.%u", (_conOwnerPa >> 12) & 0x0F, (_conOwnerPa >> 8) & 0x0F, _conOwnerPa & 0xFF);
            // Silence the local console; local input now gets a one-line "session taken over" notice.
            ftcDisableConsole(openknx.console, true, _conOwnerStr, 0);
            logInfoP("Console taken over by %u.%u.%u", (_conOwnerPa >> 12) & 0x0F, (_conOwnerPa >> 8) & 0x0F, _conOwnerPa & 0xFF);
            res[0] = 0x00;
            resLen = 1;
            return true;
        }
        if (flags & 0x02) // CLOSE
        {
            _conActive = false;
            _conCmdPending = false;
            openknx.console.disableConsole(false);
            logInfoP("Console released");
            res[0] = 0x00;
            resLen = 1;
            return true;
        }
        if (!_conActive) // a line without a session
        {
            res[0] = 0x43;
            resLen = 1;
            return true;
        }
        if (_conCmdPending) // previous command still running
        {
            res[0] = 0x01; // BUSY
            resLen = 1;
            return true;
        }
#ifdef OPENKNX_FTC_SECURITY
        // F1(a): a console command line runs arbitrary privileged actions -> re-gate EVERY line, not just the
        // OPEN. The PW auth window (or prog mode) can lapse mid-session; refuse + let the client auto-close.
        if (!secWriteAllowed())
        {
            res[0] = (secStage() == FTM_SEC_PW) ? ST_AUTH_REQUIRED : ST_WRITES_DISABLED;
            resLen = 1;
            return true;
        }
        secRefreshWindow(); // an executed console command is activity -> extend the idle (inactivity) window
#endif
        const uint8_t n = (len > 1) ? (uint8_t)MIN(len - 1, CONSOLE_INPUT_SIZE) : 0;
        memcpy(_conLine, data + 1, n);
        _conLine[n] = 0;
        _conCmdPending = true;
        _conLastAccess = millis();
        res[0] = 0x00; // chunk#0 in the IN answer is a later optimisation
        resLen = 1;
        return true;
    }
    if (pid == CON_PID_OUT) // bounded ring drain, at most 1 APDU
    {
        if (!_conActive)
        {
            res[0] = 0x43;
            resLen = 1;
            return true;
        }
        _conLastAccess = millis();
#ifdef OPENKNX_FTC_SECURITY
        // An attached console is active use, not idle: keep the auth window fresh so a watcher is not logged
        // out mid-session; the idle countdown resumes when the console closes.
        if (secStage() == FTM_SEC_PW && _authorized) secRefreshWindow();
#endif
        const uint32_t wp = openknx.logger.ringWritePos(); // snapshot once
        uint32_t pending = wp - _conCursor;
        if (pending > OpenKNX::Log::Logger::RING_SIZE) // wrapped past the cursor -> resync + flag
        {
            _conOverflow = true;
            _conCursor = wp - OpenKNX::Log::Logger::RING_SIZE;
            pending = OpenKNX::Log::Logger::RING_SIZE;
        }
        // A request byte (data[0], 8..246) lets the client cap the drain so the answer fits a constrained
        // tunnel's max frame (e.g. a standard-frames-only IP interface); absent/0 (old client) = full window.
        uint32_t cap = CON_DRAIN_MAX;
        if (len >= 1 && data[0] >= CON_DRAIN_MIN && data[0] < CON_DRAIN_MAX) cap = data[0];
        const uint8_t n = (uint8_t)MIN(pending, cap);
        const char *rb = openknx.logger.ringBuf();
        for (uint8_t i = 0; i < n; i++)
            res[3 + i] = rb[(_conCursor + i) % OpenKNX::Log::Logger::RING_SIZE];
        _conCursor += n;
        res[0] = 0x00;
        res[1] = (_conCursor != wp) ? 1 : 0; // more
        res[2] = _conOverflow ? 1 : 0;       // overflow (truncated)
        _conOverflow = false;
        resLen = (uint8_t)(3 + n);
        return true;
    }
    return false;
}

// Run a parked command outside the stack dispatch (same context as the local console) and reap idle
// sessions. VORGABE non-blocking: the command runs only under a freeLoopTime() budget, exactly like the
// local USB console (processCommand is skipLooptimeWarning-covered) -- no new stall type.
void FileTransferModule::conLoop()
{
    if (!_conActive) return;
    if (_conCmdPending && openknx.common.freeLoopTime()) // exec OUTSIDE the dispatch
    {
        openknx.console.processCommand(_conLine); // may knx.loop()/flash.save()/restart() -- safe here
        _conCmdPending = false;
        _conLine[0] = 0;
        _conLastAccess = millis();
    }
    if (delayCheck(_conLastAccess, CON_IDLE_TMO)) // reap an orphaned session
    {
        _conActive = false;
        _conCmdPending = false;
        openknx.console.disableConsole(false);
        logInfoP("Console released (idle timeout)");
    }
}
#endif

// Advance a cooperative file CRC (LittleFS/SD/EFC) by a bounded slice per loop() pass -> NEVER a whole-file
// read in the dispatch (that reboots on a large file). The read handle was opened by cmdFileInfo when the job
// started and stays open until the answer flips to 0x00. A stranded job (client stopped polling) is reaped
// after HEARTBEAT_INTERVAL so it can never hold a handle forever.
void FileTransferModule::crcLoop()
{
    if (!_crcActive) return;
    uint8_t buf[64];
    while (_crcOff < _crcSize && openknx.common.freeLoopTime())
    {
        const uint8_t want = (uint8_t)MIN((uint32_t)sizeof(buf), _crcSize - _crcOff);
        uint8_t got = 0;
        if (_crcDrive == FD_INT) got = (uint8_t)_crcFile.readBytes((char *)buf, want); // LittleFS, sequential
    #ifdef OPENKNX_SDCARD
        else if (_crcDrive == FD_SD) got = sd::fileStore.read(_crcOff, buf, want);
    #endif
    #ifdef OPENKNX_EXTFLASH
        else if (_crcDrive == FD_EFC) got = efc::fileStore.read(_crcOff, buf, want);
    #endif
        if (got == 0) { _crcOff = _crcSize; break; } // read error/short -> stop; a wrong CRC then trips the client verify
        _crcVal = _crcFirst ? _crc32.cksum(buf, got) : _crc32.cksum_upd(buf, got);
        _crcFirst = false;
        _crcOff += got;
    }
    if (delayCheck(_crcLastAccess, HEARTBEAT_INTERVAL)) crcCancel(); // client vanished -> release the handle
}

void FileTransferModule::crcCancel()
{
    if (!_crcActive) return;
    if (_crcDrive == FD_INT) _crcFile.close();
    #ifdef OPENKNX_SDCARD
    else if (_crcDrive == FD_SD) sd::fileStore.close();
    #endif
    #ifdef OPENKNX_EXTFLASH
    else if (_crcDrive == FD_EFC) efc::fileStore.close();
    #endif
    _crcActive = false;
    _crcOff = 0;
    _crcSize = 0;
    _crcFirst = true;
}

void FileTransferModule::cmdFormat(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength)
{
    resultLength = 1;
    if (_fileOpen) { ftmXferClose(); _fileOpen = false; } // release an open transfer handle before wiping the FS

    if (!LittleFS.format())
    {
        pushByte(0x02, resultData);
        logErrorP("Formatting of the file system has failed");
        return;
    }

    logInfoP("The file system was successfully formatted");
    pushByte(0x0, resultData);
}

void FileTransferModule::cmdExists(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength)
{
    bool exists = ftmExists((char *)data);
    if (exists)
        logDebugP("The file or directory \"%s\" exists", data);
    else
        logDebugP("The file or directory \"%s\" does not exist", data);

    resultLength = 2;
    pushByte(0x0, resultData);
    pushByte(exists, resultData + 1);
}

void FileTransferModule::cmdCancel(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength)
{
    logDebugP("Cancel");

    // Close file
    if (_fileOpen)
    {
        ftmXferClose();
        _fileOpen = false;
    }

    // Close directory
    if (_dirOpen) _dirOpen = false;
}

void FileTransferModule::cmdRename(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength)
{
    resultLength = 1;

    // The payload is "old\0new\0". BOTH names must be terminated inside the frame. A separator alone is not
    // enough: a frame that ends ON the separator leaves the second name starting one past the payload, and
    // strlen() then walks whatever follows it in the buffer -- and that walked-together path is what gets
    // renamed to. Same for a second name without a terminator of its own.
    uint8_t sep = 0;
    bool haveSep = false;
    for (uint8_t i = 0; i < length; i++)
        if (data[i] == 0)
        {
            sep = i;
            haveSep = true;
            break;
        }
    bool haveEnd = false;
    if (haveSep)
        for (uint8_t i = (uint8_t)(sep + 1); i < length; i++)
            if (data[i] == 0)
            {
                haveEnd = true;
                break;
            }

    if (!haveSep || !haveEnd || data[sep + 1] == 0)
    {
        pushByte(0x45, resultData);
        logErrorP("Rename frame is not two terminated names");
        return;
    }

    const char *oldPath = (const char *)data;
    const char *newPath = (const char *)(data + sep + 1);

    if (!ftmRename(oldPath, newPath))
    {
        logErrorP("Renaming of the file \"%s\" to \"%s\" failed", oldPath, newPath);
        pushByte(0x45, resultData);
        return;
    }

    logInfoP("Renaming of the file \"%s\" to \"%s\" was successful", oldPath, newPath);
    pushByte(0x0, resultData);
}

void FileTransferModule::cmdModuleVersion(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength)
{
    resultLength = 6;
    resultData[0] = _major >> 8;
    resultData[1] = _major & 0xFF;
    resultData[2] = _minor >> 8;
    resultData[3] = _minor & 0xFF;
    resultData[4] = _revision >> 8;
    resultData[5] = _revision & 0xFF;
}

// A second app slot is where an update is written. RP2040 always has one (PicoOTA stages via the filesystem).
// On ESP32 esp_ota_get_next_update_partition() wraps to the RUNNING partition on a single-app layout, so
// Update.begin() would succeed and erase the running code -- hence the explicit check.
bool FileTransferModule::otaSlotAvailable()
{
#ifdef ARDUINO_ARCH_ESP32
    const esp_partition_t *slot = esp_ota_get_next_update_partition(nullptr);
    return slot != nullptr && slot != esp_ota_get_running_partition();
#else
    return true;
#endif
}

#ifdef ARDUINO_ARCH_RP2040
void FileTransferModule::cmdFwUpdate(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength)
{
    logInfoP("Update initiated");
    logIndentUp();
#ifdef OPENKNX_FTC_DELTA_UPDATE
    if (deltaIsPatchFile((const char *)data))
    {
        deltaArm((const char *)data); // logs its own reason; the work then happens in loop()
        logIndentDown();
        return;
    }
    // Anything else is about to be copied into the application area unchanged, so it has to look like an
    // image first. Without this a patch handed to the plain update path would be flashed as firmware.
    if (!fwImagePlausible((const char *)data))
    {
        logErrorP("staged file is not a bootable image -- not written: %s", (const char *)data);
        applyFailed(APPLY_NOT_IMAGE);
        logIndentDown();
        return;
    }
#endif
    picoOTA.begin();
    if (!picoOTA.addFile((char *)data)) // false = staged image missing / unreadable
    {
        logErrorP("staged image not found or unreadable: %s", (const char *)data);
        applyFailed(APPLY_NOT_FOUND);
        logIndentDown();
        return;
    }
    if (!picoOTA.commit()) // false = OTA command page not written -> do NOT reboot
    {
        logErrorP("PicoOTA commit failed -- apply aborted");
        applyFailed(APPLY_WRITE);
        logIndentDown();
        return;
    }
    _rebootRequested = millis();
    logInfoP("Firmware armed; device will restart in ~2s to apply");
    logIndentDown();
}
#elif defined(ARDUINO_ARCH_ESP32)

    #ifdef OPENKNX_FTC_GZIP_UPDATE
        #include <miniz.h> // tinfl lives in the chip's mask ROM (esp_rom): streaming inflate at zero flash cost

/**
 * @brief Unpack a gzipped staged image straight into the OTA slot. Returns the number of bytes written.
 * @details The ESP updater takes a raw image only — it checks the image magic on the very first byte — so a
 *          compressed transfer has to be undone here rather than on the wire. Worth it: an ESP application
 *          is around 2 MB, the bus carries ~400 B/s, and compression takes that from ~88 to ~54 minutes.
 *
 *          Streams through a 32 KiB dictionary window, the size tinfl needs to resolve back-references; the
 *          window doubles as the output buffer and is flushed to Update.write() whenever it fills. All of it
 *          is heap, freed on every exit — this runs once, immediately before a reboot, so it does not sit in
 *          any hot path. `outSize` is the uncompressed length from the gzip trailer, which Update.begin()
 *          needs up front.
 */
size_t FileTransferModule::inflateToOta(File &img, size_t dataStart, size_t outSize)
{
    tinfl_decompressor *inf = (tinfl_decompressor *)malloc(sizeof(tinfl_decompressor));
    uint8_t *dict = (uint8_t *)malloc(TINFL_LZ_DICT_SIZE);
    uint8_t *in = (uint8_t *)malloc(FTM_GZIP_IN_CHUNK);
    if (inf == nullptr || dict == nullptr || in == nullptr)
    {
        logErrorP("not enough memory to unpack the firmware");
        free(inf); free(dict); free(in);
        return 0;
    }
    tinfl_init(inf);
    img.seek(dataStart);

    size_t written = 0, dictOfs = 0, inAvail = 0, inPos = 0;
    bool eof = false;
    for (;;)
    {
        if (inAvail == 0 && !eof)
        {
            const int rd = img.read(in, FTM_GZIP_IN_CHUNK);
            if (rd <= 0) eof = true;
            else { inAvail = (size_t)rd; inPos = 0; }
        }
        size_t inBytes = inAvail;
        size_t outBytes = TINFL_LZ_DICT_SIZE - dictOfs;
        const tinfl_status st = tinfl_decompress(inf, in + inPos, &inBytes, dict, dict + dictOfs, &outBytes,
                                                 eof ? 0 : TINFL_FLAG_HAS_MORE_INPUT);
        inPos += inBytes;
        inAvail -= inBytes;
        dictOfs += outBytes;

        // Flush when the window is full, and once more at the end — the window IS the output buffer, so it
        // must be handed over before tinfl wraps around and overwrites it.
        if (dictOfs == TINFL_LZ_DICT_SIZE || st == TINFL_STATUS_DONE)
        {
            if (written == 0 && dictOfs >= 24) // the very first bytes are the image header: check the chip now
            {
                uint16_t imgChip = 0, runChip = 0;
                if (!espImageFitsThisChip(dict, imgChip, runChip))
                {
                    applyFailed(APPLY_WRONG_CHIP);
                    logErrorP("this firmware is for chip 0x%04X, this device is 0x%04X -- not written",
                              (unsigned)imgChip, (unsigned)runChip);
                    free(inf); free(dict); free(in);
                    return 0;
                }
            }
            if (dictOfs > 0 && Update.write(dict, dictOfs) != dictOfs)
            {
                logErrorP("Update.write failed: %s", Update.errorString());
                free(inf); free(dict); free(in);
                return written;
            }
            written += dictOfs;
            dictOfs = 0;
        }
        if (st == TINFL_STATUS_DONE) break;
        if (st < TINFL_STATUS_DONE) // any negative status is a corrupt stream
        {
            logErrorP("the compressed firmware is damaged (%d)", (int)st);
            break;
        }
        if (eof && inAvail == 0 && outBytes == 0 && inBytes == 0)
        {
            logErrorP("the compressed firmware ended early");
            break;
        }
    }
    free(inf); free(dict); free(in);
    if (written != outSize) logErrorP("unpacked %u bytes, expected %u", (unsigned)written, (unsigned)outSize);
    return written;
}
    #endif // OPENKNX_FTC_GZIP_UPDATE

/**
 * @brief Refuse an application image built for a different ESP chip, before a single byte is written.
 * @details The Arduino updater checks only the image magic; the chip is verified by the bootloader, which
 *          means a wrong-silicon image is written, activated, and only rejected at the next boot. The
 *          rollback then saves the device — but it costs a reboot and a confusing round trip, and the user
 *          is left guessing. Comparing against the RUNNING image's own header needs no chip table: both
 *          headers carry the same field, so like is compared with like.
 */
bool FileTransferModule::espImageFitsThisChip(const uint8_t *hdr24, uint16_t &imgChip, uint16_t &runChip)
{
    imgChip = 0xFFFF;
    runChip = 0xFFFF;
    if (hdr24 == nullptr || hdr24[0] != 0xE9) return false; // not an application image at all
    imgChip = (uint16_t)(hdr24[12] | (hdr24[13] << 8));

    const esp_partition_t *running = esp_ota_get_running_partition();
    uint8_t own[24];
    if (running == nullptr || esp_partition_read(running, 0, own, sizeof(own)) != ESP_OK) return true; // cannot tell -> do not block
    if (own[0] != 0xE9) return true;
    runChip = (uint16_t)(own[12] | (own[13] << 8));
    return imgChip == runChip;
}

/**
 * @brief ESP32 self-apply: stream the staged LittleFS image into the OTA (app1) slot, then defer-reboot to boot it.
 * @details A gzipped image is unpacked on the fly; anything else is written through unchanged, so a client
 *          that knows nothing about compression keeps working exactly as before.
 */
void FileTransferModule::cmdFwUpdate(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength)
{
    logInfoP("Update initiated");
    logIndentUp();
    const char *path = (const char *)data; // NUL-terminated staged remote path from the client
#ifdef OPENKNX_FTC_DELTA_UPDATE
    if (deltaIsPatchFile(path))
    {
        deltaArm(path); // logs its own reason; the work then happens in loop()
        logIndentDown();
        return;
    }
#endif
    if (_fileOpen)                          // release any open transfer file before re-opening it read-only
    {
        ftmXferClose();
        _fileOpen = false;
    }
    File img = LittleFS.open(path, "r");
    if (!img)
    {
        logErrorP("staged image not found: %s", path);
        applyFailed(APPLY_NOT_FOUND);
        logIndentDown();
        return;
    }
    size_t sz = img.size();

    // gzip? The magic is the only thing that decides — the file name is cosmetic.
    bool gz = false;
    size_t outSize = sz, dataStart = 0;
#ifdef OPENKNX_FTC_GZIP_UPDATE
    uint8_t hdr[10];
    if (sz > 18 && img.read(hdr, 10) == 10 && hdr[0] == 0x1F && hdr[1] == 0x8B && hdr[2] == 0x08)
    {
        dataStart = 10;
        const uint8_t flg = hdr[3];
        if (flg & 0x04) // FEXTRA: a 2-byte length followed by that many bytes
        {
            uint8_t xl[2];
            img.seek(dataStart);
            if (img.read(xl, 2) != 2) { img.close(); logErrorP("damaged gzip header"); applyFailed(APPLY_GZIP); logIndentDown(); return; }
            dataStart += 2 + ((size_t)xl[0] | ((size_t)xl[1] << 8));
        }
        if (flg & 0x08) { img.seek(dataStart); while (img.available() && img.read() != 0) {} dataStart = img.position(); } // FNAME
        if (flg & 0x10) { img.seek(dataStart); while (img.available() && img.read() != 0) {} dataStart = img.position(); } // FCOMMENT
        if (flg & 0x02) dataStart += 2;                                                                                    // FHCRC
        uint8_t isize[4];
        img.seek(sz - 4);
        if (img.read(isize, 4) != 4) { img.close(); logErrorP("damaged gzip trailer"); applyFailed(APPLY_GZIP); logIndentDown(); return; }
        outSize = (size_t)isize[0] | ((size_t)isize[1] << 8) | ((size_t)isize[2] << 16) | ((size_t)isize[3] << 24);
        if (outSize == 0 || dataStart >= sz - 8) { img.close(); logErrorP("implausible compressed firmware"); applyFailed(APPLY_GZIP); logIndentDown(); return; }
        gz = true;
        logInfoP("compressed image: %u -> %u bytes", (unsigned)sz, (unsigned)outSize);
    }
    img.seek(0);
#endif

    if (!gz) // a raw image states its header directly; the compressed path checks after the first inflate
    {
        uint8_t hdr24[24];
        img.seek(0);
        uint16_t imgChip = 0, runChip = 0;
        if (img.read(hdr24, sizeof(hdr24)) == (int)sizeof(hdr24) && !espImageFitsThisChip(hdr24, imgChip, runChip))
        {
            applyFailed(APPLY_WRONG_CHIP);
            logErrorP("this firmware is for chip 0x%04X, this device is 0x%04X -- not written",
                      (unsigned)imgChip, (unsigned)runChip);
            img.close();
            logIndentDown();
            return;
        }
        img.seek(0);
    }

    // Must be checked BEFORE Update.begin(): with a single app partition begin() does NOT fail, it targets
    // the running partition, and the write below would erase the code currently executing.
    if (!otaSlotAvailable())
    {
        logErrorP("no second OTA slot in this partition layout -- update over the bus not possible, use USB");
        applyFailed(APPLY_NO_SLOT);
        img.close();
        logIndentDown();
        return;
    }
    if (!Update.begin(outSize))
    {
        applyFailed(APPLY_BEGIN);
        logErrorP("Update.begin(%u) failed: %s", (unsigned)outSize, Update.errorString());
        img.close();
        logIndentDown();
        return;
    }
    size_t w;
#ifdef OPENKNX_FTC_GZIP_UPDATE
    if (gz) w = inflateToOta(img, dataStart, outSize);
    else
#endif
        w = Update.writeStream(img);
    img.close();
    if (w != outSize || !Update.end(true) || !Update.isFinished())
    {
        applyFailed(APPLY_WRITE);
        logErrorP("Update failed: %s", Update.errorString());
        Update.abort(); // frees the sector buffer and re-arms begin(); idempotent
        logIndentDown();
        return;
    }
    logInfoP("Firmware written to OTA slot; reboot in ~2s to apply");
    _rebootRequested = millis(); // reuse loop()'s deferred flash.save()+restart()
    logIndentDown();
}
#endif

#ifdef OPENKNX_FTC_DELTA_UPDATE
/**
 * @brief Is the staged file a patch rather than an image?
 * @details Decided by the file's own first bytes, exactly like a gzipped image: the name says nothing
 *          about the content, and a client that guessed wrong would otherwise flash a patch as firmware.
 */
bool FileTransferModule::deltaIsPatchFile(const char *path)
{
    File probe = LittleFS.open(path, "r");
    if (!probe) return false;
    uint8_t magic[4] = {0};
    const bool isPatch = probe.read(magic, sizeof(magic)) == (int)sizeof(magic) &&
                         memcmp(magic, FirmwarePatch::MAGIC, sizeof(magic)) == 0;
    probe.close();
    return isPatch;
}

// --- Unpacking a packed patch -------------------------------------------------------------------
// Only one job runs at a time, so the decompressor's read callback finds its module through a file
// static. uzlib's C interface carries no user pointer, and inventing one would mean patching a
// vendored library for nothing.
static FileTransferModule *_deltaUnzipOwner = nullptr;
static uint8_t *_deltaUnzipIn = nullptr;
static constexpr uint32_t DELTA_UNZIP_IN = 1024;   // bytes pulled from the file per refill
static constexpr uint32_t DELTA_UNZIP_DICT = 32768; // the window the format is compressed with

static int deltaUnzipRead(struct uzlib_uncomp *m)
{
    if (_deltaUnzipOwner == nullptr) return -1;
    const int got = _deltaUnzipOwner->deltaUnzipRefill(_deltaUnzipIn, DELTA_UNZIP_IN);
    if (got <= 0) return -1;
    m->source = _deltaUnzipIn;
    m->source_limit = _deltaUnzipIn + got;
    return *(m->source++);
}

/** @brief Hand the decompressor the next slice of the packed file. */
int FileTransferModule::deltaUnzipRefill(uint8_t *dst, uint32_t len)
{
    const uint32_t left = _deltaPatchSize - _deltaPackedRead;
    if (left == 0) return 0;
    const uint32_t want = (left < len) ? left : len;
    if (!deltaPatchRead(this, dst, _deltaPackedRead, want)) return -1;
    _deltaPackedRead += want;
    return (int)want;
}

/**
 * @brief Prepare unpacking: header out, decompressor up, plain file open.
 * @details The plain patch is rebuilt exactly as an unpacked one would have arrived -- same header with
 *          the flag cleared, then the two streams -- so the interpreter afterwards cannot tell the
 *          difference and needs to know nothing about compression.
 */
bool FileTransferModule::deltaUnzipBegin()
{
    _deltaUnzipErr = FirmwarePatch::ERR_READ; // replaced below by whatever actually happens
    uint8_t hdr[FirmwarePatch::HDR_SIZE];
    if (!deltaPatchRead(this, hdr, 0, sizeof(hdr))) return false;
    hdr[5] &= (uint8_t)~FirmwarePatch::FLAG_PACKED; // the plain form is not packed
    // The header checksum covers the flag byte, so it has to be restamped.
    const uint32_t crc = FirmwarePatch::crcFinal(FirmwarePatch::crcUpdate(FirmwarePatch::CRC_INIT, hdr, FirmwarePatch::HDR_SIZE - 4));
    hdr[32] = (uint8_t)(crc & 0xFF);
    hdr[33] = (uint8_t)((crc >> 8) & 0xFF);
    hdr[34] = (uint8_t)((crc >> 16) & 0xFF);
    hdr[35] = (uint8_t)((crc >> 24) & 0xFF);

    const uint32_t opsLen = (uint32_t)hdr[24] | ((uint32_t)hdr[25] << 8) | ((uint32_t)hdr[26] << 16) |
                            ((uint32_t)hdr[27] << 24);
    const uint32_t litLen = (uint32_t)hdr[28] | ((uint32_t)hdr[29] << 8) | ((uint32_t)hdr[30] << 16) |
                            ((uint32_t)hdr[31] << 24);
    _deltaPlainExpect = opsLen + litLen;
    // These are the UNPACKED lengths, so nothing else in the file bounds them: a damaged header can
    // claim any size at all. The sum catches its own overflow (a wrapped sum is smaller than opsLen),
    // and the room check below is done by subtraction so the header size cannot wrap it either.
    if (_deltaPlainExpect == 0 || opsLen > _deltaPlainExpect) { _deltaUnzipErr = FirmwarePatch::ERR_SIZE; return false; }
    if (_deltaPlainExpect > 0xFFFFFFFFu - FirmwarePatch::HDR_SIZE) { _deltaUnzipErr = FirmwarePatch::ERR_SIZE; return false; }

    LittleFS.remove(DELTA_PLAIN_PATH);
    if (deltaStagingRoom() < _deltaPlainExpect + FirmwarePatch::HDR_SIZE)
    {
        logErrorP("not enough space to unpack %u bytes of patch", (unsigned)_deltaPlainExpect);
        _deltaUnzipErr = FirmwarePatch::ERR_WRITE;
        return false;
    }
    _deltaPlain = LittleFS.open(DELTA_PLAIN_PATH, "w");
    if (!_deltaPlain) { _deltaUnzipErr = FirmwarePatch::ERR_WRITE; return false; }
    if (_deltaPlain.write(hdr, sizeof(hdr)) != sizeof(hdr)) { _deltaUnzipErr = FirmwarePatch::ERR_WRITE; return false; }

    _deltaDict = (uint8_t *)malloc(DELTA_UNZIP_DICT);
    _deltaUnzipIn = (uint8_t *)malloc(DELTA_UNZIP_IN);
    struct uzlib_uncomp *d = (struct uzlib_uncomp *)malloc(sizeof(struct uzlib_uncomp));
    if (_deltaDict == nullptr || _deltaUnzipIn == nullptr || d == nullptr)
    {
        free(d);
        // Distinct from a full filesystem: making room does not help here, and sending the patch
        // unpacked does -- so the client has to be able to tell the two apart.
        logErrorP("not enough memory to unpack the patch");
        _deltaUnzipErr = FirmwarePatch::ERR_SIZE;
        return false;
    }
    memset(d, 0, sizeof(*d));
    _deltaInflate = d;
    _deltaUnzipOwner = this;
    _deltaPackedRead = FirmwarePatch::HDR_SIZE; // the compressed stream starts right after the header
    _deltaPlainWritten = 0;

    uzlib_init();
    d->source = nullptr;
    d->source_limit = nullptr;
    d->source_read_cb = deltaUnzipRead;
    uzlib_uncompress_init(d, _deltaDict, DELTA_UNZIP_DICT);
    if (uzlib_zlib_parse_header(d) < 0)
    {
        logErrorP("the packed patch has a damaged header");
        _deltaUnzipErr = FirmwarePatch::ERR_HEADER_CRC;
        return false;
    }
    return true;
}

/** @brief Unpack one slice into the plain patch file. */
bool FileTransferModule::deltaUnzipStep()
{
    struct uzlib_uncomp *d = (struct uzlib_uncomp *)_deltaInflate;
    _deltaUnzipErr = FirmwarePatch::ERR_TRUNCATED; // replaced below by whatever actually happens
    const uint32_t left = _deltaPlainExpect - _deltaPlainWritten;
    const uint32_t want = (left < FTM_DELTA_SLICE) ? left : FTM_DELTA_SLICE;

    d->dest_start = _deltaBuf;
    d->dest = _deltaBuf;
    d->dest_limit = _deltaBuf + want;
    const int res = uzlib_uncompress(d);
    if (res != TINF_OK && res != TINF_DONE)
    {
        logErrorP("the packed patch is damaged (%d)", res);
        _deltaUnzipErr = FirmwarePatch::ERR_READ;
        return false;
    }
    const uint32_t produced = (uint32_t)(d->dest - _deltaBuf);
    if (produced == 0 && res != TINF_DONE)
    {
        logErrorP("the packed patch ended early");
        return false;
    }
    if (produced > 0 && _deltaPlain.write(_deltaBuf, produced) != produced)
    {
        // A full filesystem and a corrupt patch are not the same problem, and the client acts on the
        // difference: one is retried after making room, the other never succeeds.
        logErrorP("cannot write the unpacked patch -- the filesystem is full");
        _deltaUnzipErr = FirmwarePatch::ERR_WRITE;
        return false;
    }
    _deltaPlainWritten += produced;

    if (_deltaPlainWritten < _deltaPlainExpect)
    {
        // A finished stream that has not produced everything the header promised describes a different
        // patch than the one announced.
        if (res == TINF_DONE)
        {
            logErrorP("the packed patch ended early");
            return false;
        }
        return true;
    }
    return true;
}

/** @brief Give back everything the unpacking held. Safe to call twice. */
void FileTransferModule::deltaUnzipEnd()
{
    if (_deltaInflate != nullptr)
    {
        free(_deltaInflate);
        _deltaInflate = nullptr;
    }
    if (_deltaDict != nullptr)
    {
        free(_deltaDict);
        _deltaDict = nullptr;
    }
    if (_deltaUnzipIn != nullptr)
    {
        free(_deltaUnzipIn);
        _deltaUnzipIn = nullptr;
    }
    _deltaUnzipOwner = nullptr;
    if (_deltaPlain) _deltaPlain.close();
}

#ifdef ARDUINO_ARCH_RP2040
/**
 * @brief Does a staged file look like something this chip could boot?
 * @details The bootloader copies whatever it is pointed at straight into the application area, so a file
 *          that is not an image bricks the device until someone connects USB. Both current layouts are
 *          accepted: an RP2040 image carries a checksum over its first 252 bytes, an RP2350 image a block
 *          marker near the start. A gzipped image is passed through unchecked -- the bootloader unpacks
 *          it, and unpacking it here to look inside would cost more than the check is worth.
 */
bool FileTransferModule::fwImagePlausible(const char *path)
{
    File img = LittleFS.open(path, "r");
    if (!img) return false;

    uint8_t buf[260];
    const int first = img.read(buf, sizeof(buf));
    if (first < 4)
    {
        img.close();
        return false;
    }
    if (buf[0] == 0x1F && buf[1] == 0x8B) // gzip: the bootloader unpacks it
    {
        img.close();
        return true;
    }

    if (first >= 256)
    {
        // CRC-32/MPEG-2 over the first 252 bytes, stored little-endian right behind them.
        uint32_t crc = 0xFFFFFFFFu;
        for (uint16_t i = 0; i < 252; i++)
        {
            crc ^= (uint32_t)buf[i] << 24;
            for (uint8_t bit = 0; bit < 8; bit++)
                crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : (crc << 1);
        }
        const uint32_t stored = (uint32_t)buf[252] | ((uint32_t)buf[253] << 8) | ((uint32_t)buf[254] << 16) |
                                ((uint32_t)buf[255] << 24);
        if (crc == stored)
        {
            img.close();
            return true;
        }
    }

    // RP2350 has no such checksum; its bootrom looks for a block marker in the first pages instead.
    static const uint8_t MARKER[4] = {0xD3, 0xDE, 0xFF, 0xFF};
    uint32_t scanned = 0;
    int have = first;
    while (have >= 4 && scanned < 4096)
    {
        for (int i = 0; i + 4 <= have; i++)
            if (memcmp(buf + i, MARKER, sizeof(MARKER)) == 0)
            {
                img.close();
                return true;
            }
        // Carry the last three bytes so a marker straddling two reads is still found.
        memmove(buf, buf + have - 3, 3);
        scanned += (uint32_t)have;
        const int got = img.read(buf + 3, sizeof(buf) - 3);
        have = (got > 0) ? got + 3 : 0;
    }

    img.close();
    return false;
}
#endif

/**
 * @brief Highest offset a patch may read from the running image.
 * @details On RP2040/RP2350 the KNX and OpenKNX data areas live INSIDE the sketch region. A patch that
 *          reached into them would depend on the parameters of the individual device, so the source is
 *          capped below them. On ESP32 the application owns its whole partition.
 */
uint32_t FileTransferModule::deltaSourceLimit()
{
#ifdef ARDUINO_ARCH_RP2040
    return (uint32_t)KNX_FLASH_OFFSET;
#else
    const esp_partition_t *running = esp_ota_get_running_partition();
    return running != nullptr ? (uint32_t)running->size : 0;
#endif
}

bool FileTransferModule::deltaSrcRead(void *ctx, uint8_t *dst, uint32_t ofs, uint32_t len)
{
    FileTransferModule *self = (FileTransferModule *)ctx;
    if (len == 0) return true;
    const uint32_t limit = self->deltaSourceLimit();
    if (ofs > limit || len > limit - ofs) return false;
#ifdef ARDUINO_ARCH_RP2040
    // The running image is memory mapped; copying out of it costs nothing and needs no driver.
    memcpy(dst, (const uint8_t *)(XIP_BASE + ofs), len);
    return true;
#else
    // Read through the partition API rather than a mapped pointer: this runs while the OTHER slot is
    // being written, and the API is what serialises that.
    const esp_partition_t *running = esp_ota_get_running_partition();
    return running != nullptr && esp_partition_read(running, ofs, dst, len) == ESP_OK;
#endif
}

bool FileTransferModule::deltaPatchRead(void *ctx, uint8_t *dst, uint32_t ofs, uint32_t len)
{
    FileTransferModule *self = (FileTransferModule *)ctx;
    if (len == 0) return true;
    if (!self->_deltaPatchFile) return false;
    if (ofs > self->_deltaPatchSize || len > self->_deltaPatchSize - ofs) return false;
    if (!self->_deltaPatchFile.seek(ofs)) return false;
    return self->_deltaPatchFile.read(dst, len) == (int)len;
}

bool FileTransferModule::deltaSinkWrite(void *ctx, const uint8_t *buf, uint32_t len)
{
    FileTransferModule *self = (FileTransferModule *)ctx;
    if (len == 0) return true;
#ifdef ARDUINO_ARCH_RP2040
    return self->_deltaStage && self->_deltaStage.write(buf, len) == len;
#else
    (void)self;
    return Update.write((uint8_t *)buf, len) == len;
#endif
}

/**
 * @brief Release everything a job holds and remember why it ended.
 * @details Called on success, on failure and by the reaper alike, so exactly one place knows what has to
 *          be given back. A failed job never leaves a half-written target armed.
 */
void FileTransferModule::deltaStop(uint8_t err)
{
    deltaUnzipEnd(); // idempotent; the unpack phase may or may not have run
    if (_deltaBuf != nullptr)
    {
        free(_deltaBuf);
        _deltaBuf = nullptr;
    }
    // Close BEFORE deleting. After unpacking, the patch being interpreted IS the unpacked file, so
    // removing it while the handle is open leaves it on the filesystem -- which is how a finished update
    // was found still holding 78 KB.
    if (_deltaPatchFile) _deltaPatchFile.close();
    LittleFS.remove(DELTA_PLAIN_PATH);
#ifdef ARDUINO_ARCH_RP2040
    if (_deltaStage) _deltaStage.close();
    if (err != FirmwarePatch::ERR_NONE) LittleFS.remove(DELTA_STAGE_PATH);
#else
    if (err != FirmwarePatch::ERR_NONE) Update.abort(); // frees the sector buffer and re-arms begin()
#endif
    // A patch that was applied has done its job; leaving it behind costs space on exactly the devices
    // that have least of it. A FAILED one stays, so a retry does not need the whole transfer again.
    if (err == FirmwarePatch::ERR_NONE && _deltaPackedPath[0] != 0) LittleFS.remove(_deltaPackedPath);
    _deltaPackedPath[0] = 0;

    _deltaError = err;
    _deltaPhase = (err == FirmwarePatch::ERR_NONE) ? DELTA_IDLE : DELTA_FAILED;
    _deltaPatchSize = 0;
}

#ifdef ARDUINO_ARCH_RP2040
/**
 * @brief Give back the staged image once the bootloader has really used it.
 * @details The bootloader copies a file into the application area and clears only its own command page;
 *          the image file itself stays behind and can be most of the filesystem. It must not be deleted
 *          blindly, though: if the copy did NOT happen the bootloader will retry at the next boot, and
 *          deleting the file would turn a retry into a brick.
 *
 *          So the file is only released once the flash it was meant to produce actually contains it.
 *          Head and tail are enough to tell: the copy is all-or-nothing, and two matching 4 KB windows
 *          of a 800 KB image do not happen by accident.
 */
void FileTransferModule::deltaReclaim()
{
    File img = LittleFS.open(DELTA_STAGE_PATH, "r");
    if (!img) return;
    const uint32_t size = (uint32_t)img.size();
    if (size == 0 || size > deltaSourceLimit())
    {
        img.close();
        return;
    }

    uint8_t *buf = (uint8_t *)malloc(FTM_DELTA_SLICE);
    if (buf == nullptr)
    {
        img.close();
        return;
    }

    bool applied = true;
    const uint32_t window = (size < FTM_DELTA_SLICE) ? size : FTM_DELTA_SLICE;
    const uint32_t spots[2] = {0, size - window};
    for (uint8_t i = 0; i < 2 && applied; i++)
    {
        if (!img.seek(spots[i]) || img.read(buf, window) != (int)window) applied = false;
        else if (memcmp(buf, (const uint8_t *)(XIP_BASE + spots[i]), window) != 0) applied = false;
    }
    free(buf);
    img.close();

    if (!applied) return; // the bootloader still has work to do -- leave everything alone
    LittleFS.remove(DELTA_STAGE_PATH);
    LittleFS.remove("/otacommand.bin"); // consumed; the bootloader only erases its data block
    logInfoP("Reclaimed %u bytes: the staged image is already running", (unsigned)size);
}
#endif

/** @brief Once per start: release what a finished update left behind. */
void FileTransferModule::processAfterStartupDelay()
{
#ifdef ARDUINO_ARCH_RP2040
    deltaReclaim();
#endif
}

/**
 * @brief Arm a delta apply. Returns immediately; the work happens in loop().
 * @details Everything that can be refused cheaply is refused here, before a slot is opened or a byte is
 *          written: no second job, a slot to write into, a sound patch header, and enough room to stage
 *          the rebuilt image where the platform needs it.
 */
bool FileTransferModule::deltaArm(const char *patchPath)
{
    if (deltaBusy())
    {
        logErrorP("an update is already running");
        return false;
    }
    deltaStop(FirmwarePatch::ERR_NONE); // clear a previous failure before taking anything

    if (_fileOpen) // a transfer handle would fight the job over the single open file
    {
        ftmXferClose();
        _fileOpen = false;
    }

    _deltaPatchFile = LittleFS.open(patchPath, "r");
    if (!_deltaPatchFile)
    {
        logErrorP("staged patch not found: %s", patchPath);
        return false;
    }
    _deltaPatchSize = (uint32_t)_deltaPatchFile.size();
    // Remembered only so a packed patch can be released once it is unpacked. Too long a path simply
    // means it stays -- the update still works, it just needs the room.
    _deltaPackedPath[0] = 0;
    if (strlen(patchPath) < sizeof(_deltaPackedPath)) strcpy(_deltaPackedPath, patchPath);

#ifdef ARDUINO_ARCH_RP2040
    // A rebuild interrupted by a power cut leaves a part-written image behind. deltaReclaim() will not
    // touch it -- it cannot tell one apart from an image still waiting to be flashed -- so it survives
    // every restart and eats about a megabyte. Without /otacommand.bin the bootloader has no instruction
    // to copy anything, which makes the file provably dead. Removed HERE, before the room checks, because
    // otherwise the next attempt is refused for lack of space by a file that is already rubbish.
    if (!LittleFS.exists("/otacommand.bin") && LittleFS.exists(DELTA_STAGE_PATH))
    {
        File stale = LittleFS.open(DELTA_STAGE_PATH, "r");
        const uint32_t staleSize = stale ? (uint32_t)stale.size() : 0;
        if (stale) stale.close();
        LittleFS.remove(DELTA_STAGE_PATH);
        logInfoP("Removed %u bytes left by an interrupted update", (unsigned)staleSize);
    }
#endif


    if (!otaSlotAvailable())
    {
        logErrorP("no second OTA slot in this partition layout -- update over the bus not possible, use USB");
        deltaStop(FirmwarePatch::ERR_WRITE);
        return false;
    }

    _deltaBuf = (uint8_t *)malloc(FTM_DELTA_SLICE);
    if (_deltaBuf == nullptr)
    {
        logErrorP("not enough memory to apply the patch");
        deltaStop(FirmwarePatch::ERR_WRITE);
        return false;
    }

    // The header is always readable, packed or not -- that is what the flag bit is for. Read it here to
    // decide which of the two ways in to take.
    uint8_t head[8] = {0};
    if (!deltaPatchRead(this, head, 0, sizeof(head)) || memcmp(head, FirmwarePatch::MAGIC, 4) != 0)
    {
        logErrorP("not a patch file");
        deltaStop(FirmwarePatch::ERR_MAGIC);
        return false;
    }
    if (head[4] != FirmwarePatch::VERSION)
    {
        logErrorP("patch format version %u is newer than this firmware understands", (unsigned)head[4]);
        deltaStop(FirmwarePatch::ERR_VERSION);
        return false;
    }
    _deltaPacked = (head[5] & FirmwarePatch::FLAG_PACKED) != 0;
    if ((head[5] & ~FirmwarePatch::FLAG_PACKED) != 0)
    {
        logErrorP("patch uses a feature this firmware does not know");
        deltaStop(FirmwarePatch::ERR_FLAGS);
        return false;
    }

    if (_deltaPacked)
    {
        if (!deltaUnzipBegin())
        {
            deltaStop(_deltaUnzipErr);
            return false;
        }
        _deltaPhase = DELTA_UNZIP;
        _deltaError = FirmwarePatch::ERR_NONE;
        logInfoP("Delta update armed: unpacking %u bytes of patch", (unsigned)_deltaPatchSize);
        return true;
    }

    if (!deltaStartJob()) return false;
    logInfoP("Delta update armed: %u -> %u bytes", (unsigned)_deltaJob.sourceLen(),
             (unsigned)_deltaJob.targetLen());
    return true;
}

/**
 * @brief Wire the interpreter to the running image and the platform's sink, and check there is room.
 * @details Reached with a PLAIN patch in `_deltaPatchFile`, whether it arrived that way or was just
 *          unpacked. Everything platform-specific about where the result goes lives here.
 */
bool FileTransferModule::deltaStartJob()
{
    FirmwarePatch::Io io;
    io.src = deltaSrcRead;
    io.patch = deltaPatchRead;
    io.sink = deltaSinkWrite;
    io.ctx = this;
    if (!_deltaJob.begin(io, _deltaPatchSize, deltaSourceLimit()))
    {
        logErrorP("patch refused, reason %u", (unsigned)_deltaJob.error());
        deltaStop(_deltaJob.error());
        return false;
    }

    const uint32_t target = _deltaJob.targetLen();
#ifdef ARDUINO_ARCH_RP2040
    // The bootloader copies a FILE to flash, so the rebuilt image has to fit next to the patch. Checked
    // here rather than half way through, where the only answer left would be to throw the work away.
    LittleFS.remove(DELTA_STAGE_PATH);
    if (deltaStagingRoom() < target)
    {
        logErrorP("not enough space to stage %u bytes -- update over the bus not possible on this device",
                  (unsigned)target);
        deltaStop(FirmwarePatch::ERR_WRITE);
        return false;
    }
    _deltaStage = LittleFS.open(DELTA_STAGE_PATH, "w");
    if (!_deltaStage)
    {
        logErrorP("cannot create the staging file");
        deltaStop(FirmwarePatch::ERR_WRITE);
        return false;
    }
    _deltaVerifyPos = 0;
    _deltaVerifyCrc = FirmwarePatch::CRC_INIT;
#else
    if (!Update.begin(target))
    {
        logErrorP("Update.begin(%u) failed: %s", (unsigned)target, Update.errorString());
        deltaStop(FirmwarePatch::ERR_WRITE);
        return false;
    }
#endif

    _deltaPhase = DELTA_RUN;
    _deltaError = FirmwarePatch::ERR_NONE;
    return true;
}

/**
 * @brief One slice of the running job.
 * @details Bounded by the scratch buffer, which is one flash sector -- the largest step the hardware
 *          cannot interrupt anyway. The KNX stack keeps running between slices, so nothing on the bus is
 *          lost while an update is being applied.
 */
void FileTransferModule::deltaLoop()
{
    if (!deltaBusy()) return;
    if (!openknx.common.freeLoopTime()) return;

    // No stall timer here on purpose. Once armed the job waits for nothing external -- every slice either
    // advances or fails -- so a wall clock could only ever abort a healthy job that was starved of loop
    // time by something else. The client cannot hang it either; it is not part of the loop.

    switch (_deltaPhase)
    {
        case DELTA_UNZIP:
        {
            if (!deltaUnzipStep())
            {
                deltaStop(_deltaUnzipErr);
                return;
            }
            if (_deltaPlainWritten < _deltaPlainExpect) return;

            // Unpacked. Hand the interpreter the plain file and give the window back before the rebuild
            // starts, so the two allocations never exist at the same time.
            _deltaPlain.flush();
            deltaUnzipEnd();
            _deltaPatchFile.close();
            // The packed original is no longer needed, and on a device whose filesystem barely holds the
            // rebuilt image it is the difference between fitting and not. A retry costs a fresh upload,
            // which is the cheaper of the two prices.
            if (_deltaPackedPath[0] != 0) LittleFS.remove(_deltaPackedPath);
            _deltaPatchFile = LittleFS.open(DELTA_PLAIN_PATH, "r");
            if (!_deltaPatchFile)
            {
                deltaStop(FirmwarePatch::ERR_READ);
                return;
            }
            _deltaPatchSize = (uint32_t)_deltaPatchFile.size();
            if (!deltaStartJob()) return; // logs and stops on its own
            logInfoP("Patch unpacked: %u -> %u bytes", (unsigned)_deltaJob.sourceLen(),
                     (unsigned)_deltaJob.targetLen());
            return;
        }

        case DELTA_RUN:
        {
            if (!_deltaJob.step(_deltaBuf, FTM_DELTA_SLICE))
            {
                logErrorP("patch refused, reason %u", (unsigned)_deltaJob.error());
                deltaStop(_deltaJob.error());
                return;
            }
                    if (!_deltaJob.done()) return;
#ifdef ARDUINO_ARCH_RP2040
            _deltaStage.flush();
            _deltaStage.close();
            _deltaStage = LittleFS.open(DELTA_STAGE_PATH, "r");
            if (!_deltaStage)
            {
                deltaStop(FirmwarePatch::ERR_WRITE);
                return;
            }
            _deltaPhase = DELTA_VERIFY;
#else
            _deltaPhase = DELTA_ARM;
#endif
            return;
        }

#ifdef ARDUINO_ARCH_RP2040
        case DELTA_VERIFY:
        {
            // The interpreter checked what it PRODUCED. This checks what the filesystem actually kept --
            // the last chance to notice a bad write before the bootloader is pointed at the file.
            const uint32_t left = _deltaJob.targetLen() - _deltaVerifyPos;
            const uint32_t want = (left < FTM_DELTA_SLICE) ? left : FTM_DELTA_SLICE;
            if (_deltaStage.read(_deltaBuf, want) != (int)want)
            {
                logErrorP("staged image could not be read back");
                deltaStop(FirmwarePatch::ERR_READ);
                return;
            }
            _deltaVerifyCrc = FirmwarePatch::crcUpdate(_deltaVerifyCrc, _deltaBuf, want);
            _deltaVerifyPos += want;
                    if (_deltaVerifyPos < _deltaJob.targetLen()) return;
            if (FirmwarePatch::crcFinal(_deltaVerifyCrc) != _deltaJob.targetCrc())
            {
                logErrorP("staged image does not match the expected checksum");
                deltaStop(FirmwarePatch::ERR_DST_CRC);
                return;
            }
            _deltaStage.close();
            _deltaPhase = DELTA_ARM;
            return;
        }
#endif

        case DELTA_ARM:
        {
#ifdef ARDUINO_ARCH_RP2040
            picoOTA.begin();
            if (!picoOTA.addFile(DELTA_STAGE_PATH))
            {
                logErrorP("staged image not readable by the bootloader");
                deltaStop(FirmwarePatch::ERR_WRITE);
                return;
            }
            if (!picoOTA.commit())
            {
                logErrorP("PicoOTA commit failed -- apply aborted");
                deltaStop(FirmwarePatch::ERR_WRITE);
                return;
            }
#else
            if (!Update.end(true) || !Update.isFinished())
            {
                logErrorP("Update failed: %s", Update.errorString());
                deltaStop(FirmwarePatch::ERR_WRITE);
                return;
            }
#endif
            deltaStop(FirmwarePatch::ERR_NONE);
            _rebootRequested = millis(); // reuse loop()'s deferred flash.save() + restart()
            logInfoP("Firmware rebuilt and armed; device will restart in ~2s to apply");
            return;
        }

        default:
            return;
    }
}

/**
 * @brief cmd 106: is the running image the one a patch was built against, and what is the job doing?
 * @details Request is (u32 length, u32 checksum) of a candidate base. The checksum runs over the whole
 *          image, which is far too much for one dispatch, so the answer is 0x02 "still computing" until
 *          the cooperative job finishes -- the client asks again. The same command carries the status of
 *          a running or failed apply, because cmd 101 answers nothing at all by contract.
 */
void FileTransferModule::cmdFwProbe(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength)
{
    auto answer = [&](uint8_t status, uint32_t arg) {
        resultData[0] = status;
        resultData[1] = (uint8_t)(arg & 0xFF);
        resultData[2] = (uint8_t)((arg >> 8) & 0xFF);
        resultData[3] = (uint8_t)((arg >> 16) & 0xFF);
        resultData[4] = (uint8_t)((arg >> 24) & 0xFF);
        resultLength = 5;
    };

    if (_deltaPhase == DELTA_FAILED)
    {
        // Reported once. Leaving it set would answer every later base check with "the last update
        // failed", and the client would never get to ask what it actually came to ask.
        answer(0x05, _deltaError);
        _deltaPhase = DELTA_IDLE;
        return;
    }
    if (deltaBusy())
    {
        answer(0x03, _deltaJob.produced());
        return;
    }
    if (length < 8)
    {
        answer(0x4B, 0);
        return;
    }

    const uint32_t wantLen = (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) |
                             ((uint32_t)data[3] << 24);
    const uint32_t wantCrc = (uint32_t)data[4] | ((uint32_t)data[5] << 8) | ((uint32_t)data[6] << 16) |
                             ((uint32_t)data[7] << 24);
    const uint32_t limit = deltaSourceLimit();
    if (wantLen == 0 || wantLen > limit)
    {
        answer(0x4B, limit);
        return;
    }

    // A different candidate than the one being computed restarts the job; the client walks its archive
    // newest first and must not be answered about the previous candidate.
    if (!_probeActive || _probeLen != wantLen)
    {
        _probeActive = true;
        _probeLen = wantLen;
        _probeOff = 0;
        _probeCrc = FirmwarePatch::CRC_INIT;
    }
    _probeLastAccess = millis();
    if (_probeOff < _probeLen)
    {
        answer(0x02, _probeOff);
        return;
    }

    const uint32_t have = FirmwarePatch::crcFinal(_probeCrc);
    answer(have == wantCrc ? 0x00 : 0x42, deltaStagingRoom());
}

/** @brief Advance the source checksum by one slice; the client polls cmd 106 until it is done. */
void FileTransferModule::probeLoop()
{
    if (!_probeActive || _probeOff >= _probeLen) return;
    if (delayCheck(_probeLastAccess, HEARTBEAT_INTERVAL)) // client stopped asking
    {
        _probeActive = false;
        return;
    }
    // 64 bytes meant deltaSrcRead -- and with it esp_ota_get_running_partition() and a partition read --
    // ran 16k times for a one-megabyte image, on the one operation a person sits and waits for. 256 costs
    // 192 more bytes of stack and a quarter of the calls.
    uint8_t buf[256];
    while (_probeOff < _probeLen && openknx.common.freeLoopTime())
    {
        const uint32_t left = _probeLen - _probeOff;
        const uint32_t want = (left < sizeof(buf)) ? left : sizeof(buf);
        if (!deltaSrcRead(this, buf, _probeOff, want))
        {
            _probeActive = false;
            return;
        }
        _probeCrc = FirmwarePatch::crcUpdate(_probeCrc, buf, want);
        _probeOff += want;
    }
}

/** @brief Bytes available to stage a rebuilt image, or all-ones where none is needed (ESP32). */
uint32_t FileTransferModule::deltaStagingRoom()
{
    // What the FILESYSTEM has left, on every platform. It used to answer "unlimited" on an ESP32 on the
    // grounds that no image is staged there -- but the unpacked patch is written to the filesystem just
    // the same, so the one check that guards it was switched off exactly where it still applied. Whether
    // the rebuilt image also needs room is the caller's question, and only the RP path asks it.
    uint32_t total = 0, used = 0;
#ifdef ARDUINO_ARCH_RP2040
    FSInfo fsinfo = {0};
    LittleFS.info(fsinfo);
    total = (uint32_t)fsinfo.totalBytes;
    used = (uint32_t)fsinfo.usedBytes;
#else
    total = (uint32_t)LittleFS.totalBytes();
    used = (uint32_t)LittleFS.usedBytes();
#endif
    return total > used ? total - used : 0;
}
#endif // OPENKNX_FTC_DELTA_UPDATE

void FileTransferModule::cmdFileInfo(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength)
{
    const char *filename = (char *)data;
    if(_fileOpen)
    {
        logInfoP("Closed open file");
        ftmXferClose();
        _fileOpen = false;
    }

#if defined(OPENKNX_SDCARD) || defined(OPENKNX_EXTFLASH)
    {
        const char *rel;
        const uint8_t drive = ftmDrive(filename, &rel);
        if (drive != FD_INT)
        {
            // Optional trailing flag byte AFTER the NUL path: bit0 = compute the whole-file CRC cooperatively
            // (non-blocking, spread over loop() passes) so SD/EFC gets resume + a real verify. Old clients send
            // no flag -> size-only (0x01); old servers ignore the trailing byte -> also 0x01. Fully compatible.
            const size_t plen = strnlen(filename, length);
            const bool wantCrc = (plen + 1 < (size_t)length) && (data[plen + 1] & 0x01);
            if (wantCrc)
            {
                const bool sameJob = _crcActive && _crcDrive == drive &&
                                     strncmp(_crcPath, rel, sizeof(_crcPath) - 1) == 0;
                if (sameJob && _crcOff >= _crcSize) // CRC done -> answer size + crc, release the read handle
                {
                    pushByte(0x00, resultData);
                    pushInt(_crcSize, resultData + 1);
                    pushInt(_crcVal, resultData + 5);
                    resultLength = 9;
                    crcCancel();
                    return;
                }
                if (sameJob) // still computing -> tell the client to poll again
                {
                    _crcLastAccess = millis();
                    pushByte(0x02, resultData);
                    pushInt(_crcSize, resultData + 1);
                    resultLength = 5;
                    return;
                }
                // a new (or switched) path: cancel any prior job, open + init this one
                crcCancel();
                int32_t csz = -1;
    #ifdef OPENKNX_SDCARD
                if (drive == FD_SD) csz = sd::fileStore.open(rel);
    #endif
    #ifdef OPENKNX_EXTFLASH
                if (drive == FD_EFC) csz = efc::fileStore.open(rel);
    #endif
                if (csz < 0)
                {
                    pushByte(0x42, resultData);
                    resultLength = 1;
                    _dirOpen = false;
                    return;
                }
                _crcActive = true;
                _crcDrive = drive;
                _crcSize = (uint32_t)csz;
                _crcOff = 0;
                _crcFirst = true;
                _crcVal = 0;
                _crcLastAccess = millis();
                strncpy(_crcPath, rel, sizeof(_crcPath) - 1);
                _crcPath[sizeof(_crcPath) - 1] = 0;
                pushByte(0x02, resultData); // 0x02: CRC computing; size is known now
                pushInt(_crcSize, resultData + 1);
                resultLength = 5;
                return;
            }

            crcCancel(); // a size-only probe is not a CRC re-poll -> drop any active job before touching the single-slot store
            int32_t sz = -1;
    #ifdef OPENKNX_SDCARD
            if (drive == FD_SD) sz = sd::fileStore.open(rel);
    #endif
    #ifdef OPENKNX_EXTFLASH
            if (drive == FD_EFC) sz = efc::fileStore.open(rel);
    #endif
            // Size comes from open() (a stat) -> close at once, do NOT read the content (a whole-file read
            // here blocks the KNX dispatch and reboots on a large file). Default answer 0x01 = size, CRC n/a.
    #ifdef OPENKNX_SDCARD
            if (drive == FD_SD) sd::fileStore.close();
    #endif
    #ifdef OPENKNX_EXTFLASH
            if (drive == FD_EFC) efc::fileStore.close();
    #endif
            if (sz < 0)
            {
                pushByte(0x42, resultData);
                resultLength = 1;
                _dirOpen = false;
                return;
            }
            pushByte(0x01, resultData); // 0x01: size valid, CRC not computed (SD/EFC default)
            pushInt((uint32_t)sz, resultData + 1);
            resultLength = 5;
            return;
        }
    }
#endif

    // LittleFS: cooperative whole-file CRC (never a blocking read in the dispatch). Mirrors the SD/EFC job:
    // 0x02 = size known + CRC computing (client polls again); 0x00 = size + CRC ready; 0x42 = not found.
    const bool sameJob = _crcActive && _crcDrive == FD_INT &&
                         strncmp(_crcPath, filename, sizeof(_crcPath) - 1) == 0;
    if (sameJob && _crcOff >= _crcSize) // CRC done -> answer size + crc, release the read handle
    {
        pushByte(0x00, resultData);
        pushInt(_crcSize, resultData + 1);
        pushInt(_crcVal, resultData + 5);
        resultLength = 9;
        crcCancel();
        return;
    }
    if (sameJob) // still computing -> tell the client to poll again
    {
        _crcLastAccess = millis();
        pushByte(0x02, resultData);
        pushInt(_crcSize, resultData + 1);
        resultLength = 5;
        return;
    }
    // a new (or switched) path: cancel any prior job, open + init this one
    crcCancel();
    _crcFile = LittleFS.open(filename, "r");
    if (!_crcFile)
    {
        pushByte(0x42, resultData);
        resultLength = 1;
        _dirOpen = false;
        return;
    }
    _crcActive = true;
    _crcDrive = FD_INT;
    _crcSize = (uint32_t)_crcFile.size();
    _crcOff = 0;
    _crcFirst = true;
    _crcVal = 0;
    _crcLastAccess = millis();
    strncpy(_crcPath, filename, sizeof(_crcPath) - 1);
    _crcPath[sizeof(_crcPath) - 1] = 0;
    pushByte(0x02, resultData); // 0x02: CRC computing; size is known now
    pushInt(_crcSize, resultData + 1);
    resultLength = 5;
}

/**
 * @brief LittleFS capacity for `ftc df` and the client's pre-upload check: [00][total:4 BE][used:4 BE].
 *
 * Client derives free = total - used. Read-only -- safe any time, never touches an open file/dir.
 */
void FileTransferModule::cmdFilesystemInfo(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength)
{
    // A path ("sd/" / "efc/") selects that provider; no path -> LittleFS (unchanged). LittleFS reports BYTES
    // (status 0x00); a provider reports KILOBYTES (status 0x01) because a >4 GB card overflows the 32-bit field.
    uint32_t total = 0, used = 0;
    uint8_t status = 0x00;
#if defined(OPENKNX_SDCARD) || defined(OPENKNX_EXTFLASH)
    const char *rel;
    const uint8_t drive = (length > 0 && data && data[0]) ? ftmDrive((const char *)data, &rel) : FD_INT;
    if (drive != FD_INT)
    {
        uint64_t t = 0, f = 0;
    #ifdef OPENKNX_SDCARD
        if (drive == FD_SD) { t = sd::fileStore.totalBytes(); f = sd::fileStore.freeBytes(); }
    #endif
    #ifdef OPENKNX_EXTFLASH
        if (drive == FD_EFC) { t = efc::fileStore.totalBytes(); f = efc::fileStore.freeBytes(); }
    #endif
        total = (uint32_t)(t / 1024);
        used = (uint32_t)((t >= f ? t - f : 0) / 1024);
        status = 0x01; // values are KB
    }
    else
#endif
    {
    #ifdef ARDUINO_ARCH_RP2040
        FSInfo fsinfo = {0};
        LittleFS.info(fsinfo);
        total = (uint32_t)fsinfo.totalBytes;
        used = (uint32_t)fsinfo.usedBytes;
    #else
        total = (uint32_t)LittleFS.totalBytes();
        used = (uint32_t)LittleFS.usedBytes();
    #endif
    }
    pushByte(status, resultData);
    pushInt(total, resultData + 1);
    pushInt(used, resultData + 5);
    resultLength = 9;
    logInfoP("Filesystem: total %u, used %u, free %u (%s)", (unsigned)total, (unsigned)used, (unsigned)(total >= used ? total - used : 0), status ? "KB" : "B");
}

#ifdef OPENKNX_FTC_DIROPS
void FileTransferModule::cmdDirList(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength)
{
    _heartbeat = millis();

    if (!_dirOpen)
    {
        const char *path = (char *)data;
        logDebugP("List directory \"%s\"", path);
        // "sd/…" -> sd::fileStore · "efc/…" -> efc::fileStore · else the LittleFS default (unchanged).
        bool routed = false;
    #ifdef OPENKNX_SDCARD
        if (_sdDirActive) sd::fileStore.dirClose(); // close any stale iteration first
        _sdDirActive = false;
        if (strncmp(path, "sd", 2) == 0 && path[2] == '/') routed = _sdDirActive = sd::fileStore.dirOpen(path + 2);
    #endif
    #ifdef OPENKNX_EXTFLASH
        if (_efcDirActive) efc::fileStore.dirClose();
        _efcDirActive = false;
        if (!routed && strncmp(path, "efc", 3) == 0 && path[3] == '/') routed = _efcDirActive = efc::fileStore.dirOpen(path + 3);
    #endif
        if (!routed) _dir = LittleFS.open(path, "r");
        _dirOpen = true;
    }

    if (!checkOpenedDir(resultData, resultLength)) return;

    // --- Provider-served (sd/efc): one entry per round-trip; both expose the same dirNext ---
    #if defined(OPENKNX_SDCARD) || defined(OPENKNX_EXTFLASH)
    if (_sdDirActive || _efcDirActive)
    {
        char name[64];
        name[0] = '\0';
        uint8_t type = 0;
        #ifdef OPENKNX_SDCARD
        if (_sdDirActive) type = sd::fileStore.dirNext(name, sizeof(name));
        #endif
        #ifdef OPENKNX_EXTFLASH
        if (_efcDirActive) type = efc::fileStore.dirNext(name, sizeof(name));
        #endif
        if (type == 0)
        {
            resultLength = 2;
            pushByte(0x0, resultData);
            pushByte(0x0, resultData + 1);
            logDebugP("List directory completed");
        #ifdef OPENKNX_SDCARD
            if (_sdDirActive) sd::fileStore.dirClose();
        #endif
        #ifdef OPENKNX_EXTFLASH
            if (_efcDirActive) efc::fileStore.dirClose();
        #endif
            _sdDirActive = false;
            _efcDirActive = false;
            _dirOpen = false;
            return;
        }
        name[sizeof(name) - 1] = '\0';
        pushByte(0x0, resultData);
        pushByte(type, resultData + 1); // 1 = file, 2 = dir
        const uint16_t nlen = (uint16_t)strlen(name);
        memcpy(resultData + 2, name, nlen);
        resultLength = (uint8_t)(nlen + 2);
        return;
    }
    #endif

    // --- LittleFS default (unchanged) ---
    File subDirectory = _dir.openNextFile();
    if (!subDirectory)
    {
        resultLength = 2;
        pushByte(0x0, resultData);
        pushByte(0x0, resultData + 1);
        logDebugP("List directory completed");
        _dirOpen = false;
        return;
    }

    pushByte(0x0, resultData);
    pushByte(!subDirectory.isDirectory() ? 0x01 : 0x02, resultData + 1); // 0x00 = no more content

    String fileName = subDirectory.name();
    logDebugP("- %s", fileName.c_str());

    // Filesystem-supplied name (up to LFS_NAME_MAX), not ours -- unclamped it overruns the answer and resultData.
    size_t nlen = fileName.length();
    if (nlen > (size_t)(FTM_RESULT_MAX - 2)) nlen = FTM_RESULT_MAX - 2;
    memcpy(resultData + 2, fileName.c_str(), nlen);
    resultLength = (uint8_t)(nlen + 2);
}

void FileTransferModule::cmdDirCreate(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength)
{
    if (checkOpenFile(resultData, resultLength) || checkOpenDir(resultData, resultLength)) return;
    resultLength = 1;

    if (!ftmMkdir((char *)data))
    {
        pushByte(0x85, resultData);

        logErrorP("Creation of the folder \"%s\" failed", data);
        return;
    }

    logInfoP("Creation of the folder \"%s\" was successful", data);
    pushByte(0x0, resultData);
}

void FileTransferModule::cmdDirDelete(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength)
{
    if (checkOpenFile(resultData, resultLength) || checkOpenDir(resultData, resultLength)) return;
    resultLength = 1;

    if (!ftmRmdir((char *)data))
    {
        pushByte(0x84, resultData);
        logInfoP("Deleting of the folder \"%s\" failed", data);
        return;
    }

    logInfoP("Deleting of the folder \"%s\" was successful", data);
    pushByte(0x0, resultData);
}
#endif

void FileTransferModule::cmdFileDelete(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength)
{
    if (checkOpenFile(resultData, resultLength) || checkOpenDir(resultData, resultLength)) return;
    resultLength = 1;

    if (!ftmRemove((char *)data))
    {
        pushByte(0x44, resultData);
        logErrorP("Deleting of the file \"%s\" failed", data);
        return;
    }

    logInfoP("Deleting of the file \"%s\" was successful", data);
    pushByte(0x0, resultData);
}

// Optional exact total size (LE uint32) appended after the NUL-terminated name (scan from nameStart).
// Bounds-checked within length; absent (older client) -> 0 = no SD preAllocate (legacy behaviour).
static uint32_t ftmParseSizeHint(const uint8_t *data, uint8_t nameStart, uint8_t length)
{
    for (uint8_t i = nameStart; i < length; i++)
        if (data[i] == '\0')
        {
            if ((size_t)i + 5 <= (size_t)length)
                return (uint32_t)data[i + 1] | ((uint32_t)data[i + 2] << 8) | ((uint32_t)data[i + 3] << 16) | ((uint32_t)data[i + 4] << 24);
            return 0;
        }
    return 0;
}

void FileTransferModule::cmdFileUpload(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength)
{
    _heartbeat = millis();

    if (data[0] == 0x00 && data[1] == 0x00)
    {
        const char *filename = (const char *)(data + 4);
        if (_fileOpen)
        {
            logInfoP("Closed open file");
            ftmXferClose();
            _fileOpen = false; // handle is closed -> clear the flag so an early-return below leaves clean state
        }
        if (checkOpenDir(resultData, resultLength)) return;

        if(data[3] > 1)
        {
            pushByte(0x42, resultData);
            resultLength = 1; // unreachable for a well-formed client, but never return an uninitialised length
            logErrorP("Start file upload to \"%s\" is failed", filename);
            return;
        }

        _size = data[2];
        resultLength = 1;

        // TODO maybe add a byte to truncate the file if it is not the same
        bool isResume = data[3] == 0x01;
        if(isResume)
        {
            logInfoP("Resume file upload");
        } else {
            logInfoP("Truncate file upload");
        }

        uint32_t sizeHint = ftmParseSizeHint(data, 4, length);

        if (!ftmXferOpenWrite(filename, isResume, sizeHint))
        {
            pushByte(0x42, resultData);
            logErrorP("Start file upload to \"%s\" is failed", filename);
            return;
        }

        logInfoP("Start file upload to \"%s\"", filename);
        _heartbeat = millis();
        _fileOpen = true;
        _lastSequence = 0;
        pushByte(0x0, resultData);
        return;
    }

    if (data[0] == 0xFF && data[1] == 0xFF)
    {
        logInfoP("The file upload was successfully completed");
        ftmXferClose();
        _fileOpen = false;
        // An answer without a return code means "not a PDT_Function property" (03_03_07 3.4.7.3), not success.
        pushByte(0x00, resultData);
        resultLength = 1;
        return;
    }

    if (!checkOpenedFile(resultData, resultLength)) return;

    uint16_t sequence = data[1] << 8 | data[0];
    writeFile(sequence, data, length, resultData, resultLength);
}

/**
 * @brief FAST upload (cmd44): open(00 00)/close(FF FF) answered, DATA silent (returns false => no L7 answer).
 *
 * The silence is deliberate -- one frame per chunk instead of two, confirmation batched into the cmd45
 * bitmap. Deviates from 03_03_07 3.4.7.1 (every command must be answered), so fast needs this server.
 * Flow control is the client window + that bitmap, NOT the L2 ack (not evaluated on the TP send path).
 */
#ifdef OPENKNX_FTC_FASTUPLOAD
bool FileTransferModule::cmdFileUploadFast(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength)
{
    // Refresh on EVERY frame: silent DATA never hits the classic dispatch that touches _heartbeat, so
    // without this the 30 s no-heartbeat auto-close (loop()) fires mid-stream and drops the file.
    _heartbeat = millis();

    if (length < 2) return false; // even the OPEN/DATA/CLOSE discriminator needs data[0..1]; runt -> silent drop

    // OPEN: [00][00][payloadSize][flags][expectedChunks:2 LE][name NUL-terminated]
    if (data[0] == 0x00 && data[1] == 0x00)
    {
        if (length < 7) // need [00][00][sz][flags][chunks:2] + at least a 1-byte (NUL) name -> else malformed
        {
            pushByte(0x42, resultData);
            resultLength = 1;
            logErrorP("Fast open: short frame (%u B)", (unsigned)length);
            return true;
        }
        const char *filename = (const char *)(data + 6);
        if (_fileOpen)
        {
            ftmXferClose();
            _fileOpen = false;
        }
        if (checkOpenDir(resultData, resultLength)) return true;

        resultLength = 1;
        const uint16_t exp = (uint16_t)(data[4] | (data[5] << 8));
        if (exp > FTM_FAST_MAX_CHUNKS) // more chunks than the static bitmap holds -> tell the client to go classic
        {
            pushByte(0x4A, resultData);
            logErrorP("Fast upload refused: %u chunks exceed cap %u", exp, (unsigned)FTM_FAST_MAX_CHUNKS);
            return true;
        }

        _size = data[2];               // payload B/chunk -> seek stride ((seq-1)*_size)
        const uint8_t flags = data[3]; // bit0 resume (r+ vs w), bit1 keepBitmap (recovery re-open)
        uint32_t sizeHint = ftmParseSizeHint(data, 6, length);
        if (!ftmXferOpenWrite(filename, flags & 0x01, sizeHint))
        {
            pushByte(0x42, resultData);
            logErrorP("Start fast upload to \"%s\" failed", filename);
            return true;
        }
        _fileOpen = true;
        _lastSequence = 0;
        _fastExpectedChunks = exp;
        _fastRateBytes = 0;
        _fastRateMs = millis(); // start the ingest-rate interval at the open
        if (!(flags & 0x02)) memset(_fastBitmap, 0, sizeof(_fastBitmap)); // clear unless keepBitmap (fixes S2)
        logInfoP("Start fast upload to \"%s\" (%s, %u chunks, %u B/chunk)", filename,
                 (flags & 0x01) ? "resume" : "truncate", exp, _size);
        pushByte(0x00, resultData);
        return true;
    }

    // CLOSE: [FF][FF] -- flush + close, answer a deterministic 1-byte 0x00.
    if (data[0] == 0xFF && data[1] == 0xFF)
    {
        if (_fileOpen)
        {
            ftmXferClose();
            _fileOpen = false;
        }
        logInfoP("Fast upload completed");
        pushByte(0x00, resultData);
        resultLength = 1;
        return true;
    }

    // DATA (SILENT): [seq:2 LE][n][payload:n][crc16:2 BE over data[0 .. 2+n]]. return false => no answer.
    if (!_fileOpen) return false;
    if (length < 3) return false;                            // need [seq:2][n] present before reading n
    const uint16_t seq = (uint16_t)(data[1] << 8 | data[0]); // LE, matches writeFile's parse
    const uint8_t n = data[2];
    if ((uint16_t)length < (uint16_t)(3u + n + 2u)) return false;       // malformed -> silent gap (never hash past length, S6)
    const uint16_t rx = (uint16_t)(data[3 + n] << 8 | data[3 + n + 1]); // trailing CRC16, big-endian
    FastCRC16 crc16;
    if (crc16.modbus(data, 3 + n) != rx) return false; // corrupt -> leave bit CLEAR -> recoverable gap (C5)
    // Bounds-check the seq BEFORE writeChunk (short-circuit &&) so a bad/forged seq can never seek+write
    // wild nor index the bitmap out of bounds. Bit set <=> the correct bytes are on disk.
    if (seq >= 1 && seq <= _fastExpectedChunks && n <= _size && (uint32_t)(seq - 1) / 8 < sizeof(_fastBitmap) &&
        writeChunk(seq, data + 3, n)) // n <= _size: a chunk never overruns its ((seq-1)*_size) slot into the next
    {
        const uint8_t mask = (uint8_t)(1u << ((seq - 1) & 7));
        uint8_t &cell = _fastBitmap[(seq - 1) >> 3];
        if (!(cell & mask)) _fastRateBytes += n; // count NEW chunks only -> true forward-progress ingest rate
        cell |= mask;
    }
    return false; // SILENT (no L7 answer) -- deliberate, see the function header.
}

/**
 * @brief FAST gap report (cmd45): request [base:2 LE][count:2 LE][nonce:1], response
 *        [00][base:2 BE][count:2 BE][nonce:1][bitmap ceil(count/8) B]; bit i (LSB-first) <=> seq base+i received.
 *
 * count is clamped so the answer stays <= 247 B (one frame). 0x42 = no file open.
 */
void FileTransferModule::cmdFileReport(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength)
{
    _heartbeat = millis();
    if (!_fileOpen) // the bitmap only means anything for the currently-open transfer file
    {
        pushByte(0x42, resultData);
        resultLength = 1;
        return;
    }
    if (length < 5) // need [base:2][count:2][nonce] before indexing data[0..4] -> malformed
    {
        pushByte(0x42, resultData);
        resultLength = 1;
        return;
    }

    uint16_t base = (uint16_t)(data[0] | (data[1] << 8));
    uint16_t count = (uint16_t)(data[2] | (data[3] << 8));
    const uint8_t nonce = data[4];

    // Clamp count so resultLength = 6 + ceil(count/8) <= 247 -> ceil(count/8) <= 241 -> count <= 1928.
    const uint16_t maxBmp = 247 - 6; // 241 bitmap bytes
    if ((uint32_t)(count + 7) / 8 > maxBmp) count = (uint16_t)(maxBmp * 8);

    resultData[0] = 0x00;
    resultData[1] = (uint8_t)(base >> 8); // base, big-endian echo
    resultData[2] = (uint8_t)(base & 0xFF);
    resultData[3] = (uint8_t)(count >> 8); // count, big-endian echo
    resultData[4] = (uint8_t)(count & 0xFF);
    resultData[5] = nonce; // echoed for the client's staleness / dup rejection (fixes C4)
    const uint16_t bmp = (uint16_t)((count + 7) / 8);
    memset(resultData + 6, 0, bmp);
    for (uint16_t i = 0; i < count; i++)
    {
        const uint32_t s = (uint32_t)base + i; // 1-based seq for response bit i
        if (s >= 1 && s <= _fastExpectedChunks && (s - 1) / 8 < sizeof(_fastBitmap) &&
            (_fastBitmap[(s - 1) >> 3] & (1u << ((s - 1) & 7))))
            resultData[6 + (i >> 3)] |= (uint8_t)(1u << (i & 7));
    }
    resultLength = (uint8_t)(6 + bmp);

    // Append the target's MEASURED ingest rate (new bytes written / interval, B/s) so the client paces to the
    // real ceiling instead of guessing. Two trailing bytes, BE. Backward-compatible: an old client stops at the
    // bitmap; a new one reads them when resultLength > 6 + bmp. Skipped only when a huge bitmap leaves no room.
    if ((uint16_t)(resultLength + 2) <= 247)
    {
        const uint32_t now = millis();
        const uint32_t dt = (_fastRateMs && now > _fastRateMs) ? (now - _fastRateMs) : 0;
        uint32_t bps = dt ? (uint32_t)(((uint64_t)_fastRateBytes * 1000ULL) / dt) : 0;
        if (bps > 0xFFFF) bps = 0xFFFF;
        resultData[resultLength] = (uint8_t)(bps >> 8);
        resultData[resultLength + 1] = (uint8_t)(bps & 0xFF);
        resultLength = (uint8_t)(resultLength + 2);
        _fastRateBytes = 0;
        _fastRateMs = now;
    }
}
#endif

#ifdef OPENKNX_FTC_DOWNLOAD
void FileTransferModule::cmdFileDownload(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength)
{
    _heartbeat = millis();

    if (data[0] == 0x00 && data[1] == 0x00)
    {
        logInfoP("Download file \"%s\"", (char *)(data + 3));
        if (checkOpenFile(resultData, resultLength) || checkOpenDir(resultData, resultLength)) return;

        _size = data[2];

        // -6 = the 4-byte header (result/seq) + 2-byte CRC readFile() appends; bound by FTM_RESULT_MAX, not the buffer.
        if (data[2] > FTM_RESULT_MAX - 6)
        {
            logIndentUp();
            logErrorP("Requested pkg is greater than max resultLength");
            logIndentDown();
            resultLength = 1;
            pushByte(0x4, resultData);
            return;
        }

        int32_t fileSize = ftmXferOpenRead((char *)(data + 3));
        if (fileSize < 0)
        {
            logIndentUp();
            logErrorP("File can't be opened");
            logIndentDown();
            resultLength = 1;
            pushByte(0x42, resultData);
            return;
        }
        _heartbeat = millis();
        _fileOpen = true;

        _lastSequence = 0;
        pushByte(0x0, resultData);
        pushInt(fileSize, resultData + 1);
        resultLength = 5;
        return;
    }
    if (!checkOpenedFile(resultData, resultLength)) return;

    uint16_t sequence = data[1] << 8 | data[0];
    readFile(sequence, resultData, resultLength);
    _lastSequence = sequence;
}
#endif

void FileTransferModule::cmdCheckFeatures(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength)
{
    uint8_t result = 0;
    result |= 0x1; // Resume
#if defined(ARDUINO_ARCH_RP2040) || defined(ARDUINO_ARCH_ESP32)
    // On ESP32 the partition table decides if an update can be applied; advertising it on a single-app
    // layout would send a client through a ~50 min transfer that only fails at the end.
    if (otaSlotAvailable()) result |= 0x2; // Update
#endif
#ifdef OPENKNX_FTC_FASTUPLOAD
    result |= 0x4; // FAST: server understands cmd44/cmd45 (windowed fast upload).
#endif
#if defined(ARDUINO_ARCH_ESP32) && defined(OPENKNX_FTC_GZIP_UPDATE)
    // Only meaningful together with Update -- unpacking is a property of the apply step.
    if (otaSlotAvailable()) result |= 0x40; // GZIP_UPDATE: staged firmware may be sent compressed
#endif
#ifdef OPENKNX_FTC_DELTA_UPDATE
    // Only together with Update: a patch without a slot to apply it to is useless, and a client that saw
    // the bit without one would transfer for minutes only to be refused at the end.
    if (otaSlotAvailable()) result |= 0x80; // DELTA: server understands cmd 106 and the OKD1 format
#endif
#ifdef OPENKNX_FTC_CONSOLE
    result |= 0x8; // Console: obj-160 console tunnel available (ftc <pa> console)
#endif
#ifdef OPENKNX_FTC_SECURITY
    if (secStage() == FTM_SEC_PW) result |= 0x10; // AUTH_REQUIRED: device uses password challenge-response
    if (!secWriteAllowed()) result |= 0x20;       // WRITES_DISABLED: writes currently blocked (state, per-connect)
#endif
    resultData[0] = result;
    resultLength = 1;
}

#ifdef OPENKNX_FTC_SECURITY
// Live access stage. Unconfigured -> ALWAYS (nothing to protect; the caller also short-circuits on
// !configured, this keeps the enum sane for any direct reader).
uint8_t FileTransferModule::secStage()
{
    if (_secStageOvr >= 0) return (uint8_t)_secStageOvr; // TEST override (local console); reboot restores ETS
    return knx.configured() ? ParamFTM_Security : FTM_SEC_ALWAYS;
}

// Live ETS idle timeout (auto-logout) in ms, clamped to [30 s, 3600 s]. Read live so an ETS change applies
// without a reboot; clamped so an out-of-range / erased value can never mean "never" or "instant".
uint32_t FileTransferModule::secWindowMs()
{
    if (_secWinOvrS) return _secWinOvrS * 1000; // TEST override (unclamped, local console)
    uint32_t s = ParamFTM_AuthTimeout;
    if (s < SEC_WINDOW_MIN) s = SEC_WINDOW_MIN;
    if (s > SEC_WINDOW_MAX) s = SEC_WINDOW_MAX;
    return s * 1000;
}

// Is a write allowed right now? Unconfigured -> yes (nothing to protect, avoids a lock-out like OTA does).
bool FileTransferModule::secWriteAllowed()
{
    if (!knx.configured()) return true;
    switch (secStage())
    {
        case FTM_SEC_OFF: return false;
        case FTM_SEC_PROG: return knx.progMode();
        case FTM_SEC_ALWAYS: return true;
        case FTM_SEC_PW:
            if (!_authorized) return false;
            if ((millis() - _authLastMs) > secWindowMs()) // idle -> auto-logout (close the window)
            {
                // Said out loud, once. A window that closes silently is indistinguishable from a wrong
                // password at the other end, and that is exactly the guess people were left making.
                logInfoP("FTC session expired after %u s idle -- log in again", (unsigned)(secWindowMs() / 1000));
                _authorized = false;
                return false;
            }
            return true;
    }
    return false;
}

// The obj-159 commands that mutate the filesystem / firmware. Everything else (download/info/list/df/cancel/
// version/features) is a read and stays open.
bool FileTransferModule::secIsWriteCommand(uint8_t propertyId)
{
    switch ((FtmCommands)propertyId)
    {
        case FtmCommands::Format:
        case FtmCommands::Rename:
        case FtmCommands::FileUpload:
        case FtmCommands::FileDelete:
        case FtmCommands::DirCreate:
        case FtmCommands::DirDelete:
        case FtmCommands::FileUploadFast:
#if defined(ARDUINO_ARCH_RP2040) || defined(ARDUINO_ARCH_ESP32)
        case FtmCommands::FwUpdate:
#endif
            return true;
        default:
            return false;
    }
}

// Local serial-console test hooks: flip the access stage / set a known test password / shorten the idle
// window at RUNTIME so all four modes + login can be exercised without an ETS download (which reboots and
// kills sessions). Never persisted -> a reboot restores the ETS config. Every override change drops any open
// auth window (_authorized=false) so the next action re-evaluates cleanly.
bool FileTransferModule::processCommand(const std::string cmd, bool diagnoseKo)
{
    if (diagnoseKo) return false;

    if (cmd == "ftm" || cmd == "ftm sec") // status
    {
        static const char* const names[] = {"OFF (locked)", "PROG-only", "ALWAYS (open)", "PW (password)"};
        logInfoP("stage: %s [%s]", names[secStage() & 3], (_secStageOvr >= 0) ? "override" : "ETS");
        logInfoP("idle window: %u s [%s]", (unsigned)(secWindowMs() / 1000), _secWinOvrS ? "override" : "ETS");
        logInfoP("authorized now: %s | test-pw: %s", secWriteAllowed() ? "yes" : "no", _secPwOvrSet ? "set" : "off");
        return true;
    }
    if (cmd.rfind("ftm sec ", 0) == 0)
    {
        const std::string a = cmd.substr(8);
        if (a == "ets") _secStageOvr = -1;
        else if (a == "off") _secStageOvr = FTM_SEC_OFF;
        else if (a == "prog") _secStageOvr = FTM_SEC_PROG;
        else if (a == "always") _secStageOvr = FTM_SEC_ALWAYS;
        else if (a == "pw") _secStageOvr = FTM_SEC_PW;
        else { logInfoP("usage: ftm sec off|prog|always|pw|ets"); return true; }
        _authorized = false;
        logInfoP("stage override -> %s", a.c_str());
        return true;
    }
    if (cmd == "ftm pw") // clear the test password -> ETS password active again
    {
        _secPwOvrSet = false;
        _authorized = false;
        logInfoP("test password cleared (ETS password active)");
        return true;
    }
    if (cmd.rfind("ftm pw ", 0) == 0)
    {
        const std::string p = cmd.substr(7);
        if (p.length() > 16) { logInfoP("test password: max 16 chars"); return true; }
        memset(_secPwOvr, 0, sizeof(_secPwOvr));
        memcpy(_secPwOvr, p.c_str(), p.length()); // null-padded 16 == the AES key (same pad16 as ETS)
        _secPwOvrSet = true;
        _authorized = false;
        logInfoP("test password set (%u chars) -- log in with it now", (unsigned)p.length());
        return true;
    }
    if (cmd.rfind("ftm secwin ", 0) == 0)
    {
        _secWinOvrS = (uint32_t)atoi(cmd.substr(11).c_str());
        logInfoP("idle window override -> %u s%s", (unsigned)_secWinOvrS, _secWinOvrS ? "" : " (cleared, ETS active)");
        return true;
    }
    return false;
}

void FileTransferModule::showHelp()
{
    openknx.console.printHelpLine("ftm, ftm sec", "FTC: show the access stage / auth state (TEST overrides)");
    openknx.console.printHelpLine("ftm sec <mode>", "FTC: override stage off|prog|always|pw|ets (reboot restores ETS)");
    openknx.console.printHelpLine("ftm pw <password>", "FTC: set a runtime TEST password (empty arg = clear)");
    openknx.console.printHelpLine("ftm secwin <seconds>", "FTC: override the auth idle window (0 = clear)");
}

// One-block seeded AES CTR: monotonic counter guarantees single-use, AES under a seed the bus observer never
// sees guarantees unpredictability. Seed once (own PA is set by first challenge) from cheap device entropy.
void FileTransferModule::secMakeNonce()
{
    if (!_seeded)
    {
        uint8_t s[16] = {0};
        uint16_t pa = knx.individualAddress();
        uint32_t t = micros();
        uintptr_t sp = (uintptr_t)&s; // stack-address entropy
        memcpy(s + 0, &pa, 2);
        memcpy(s + 2, &t, 4);
        memcpy(s + 6, &sp, sizeof sp);
        struct AES_ctx k0;
        AES_init_ctx(&k0, FTM_SEC_K0);
        AES_ECB_encrypt(&k0, s);
        memcpy(_seedKey, s, 16);
        _seeded = true;
    }
    uint8_t n[16] = {0};
    _nonceCtr++;
    uint32_t t = micros();
    memcpy(n + 0, &_nonceCtr, 4);
    memcpy(n + 4, &t, 4);
    struct AES_ctx sk;
    AES_init_ctx(&sk, _seedKey);
    AES_ECB_encrypt(&sk, n); // n = the 16-byte nonce
    memcpy(_nonce, n, 16);
}

// CBC-MAC over a single 16-byte block (IV=0) collapses to one ECB op: MAC = AES_ECB(key, nonce).
void FileTransferModule::secComputeMac(const uint8_t *key, const uint8_t *nonce, uint8_t *out16)
{
    memcpy(out16, nonce, 16);
    struct AES_ctx c;
    AES_init_ctx(&c, key);
    AES_ECB_encrypt(&c, out16);
}

// cmd 103: hand out a fresh nonce (single outstanding, 30 s TTL). Answer = 0x00 + 16 nonce bytes.
void FileTransferModule::cmdAuthChallenge(uint8_t *resultData, uint8_t &resultLength)
{
    secMakeNonce();
    _challengePending = true;
    _challengeMs = millis();
    resultData[0] = 0x00;
    memcpy(resultData + 1, _nonce, 16);
    resultLength = 17;
}

// cmd 104: verify the client's MAC over the outstanding nonce. Success -> open the window. Non-blocking
// back-off after repeated failures (a timestamp compare, never delay()).
void FileTransferModule::cmdAuthResponse(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength)
{
    resultLength = 1;
    // pad16: the ETS TypeText password is stored null-padded to 16 bytes == the AES key (no KDF). Read live.
    // A runtime test-password override ("ftm pw", local console) wins over the ETS key when set.
    uint8_t pwKey[16];
    if (_secPwOvrSet) memcpy(pwKey, _secPwOvr, 16);
    else memcpy(pwKey, ParamFTM_Password, 16);
    const bool pwEmpty = (pwKey[0] == 0);
    // Already under back-off: reject at once and report the REMAINING wait (so the client can say "next try in
    // N min"). An attempt during the window neither extends it nor counts as a new guess. (Brute-force throttle.)
    if (_authBackoffDur && (millis() - _authBackoffStart) < _authBackoffDur)
    {
        _challengePending = false;
        secAuthFail(resultData, resultLength, (_authBackoffDur - (millis() - _authBackoffStart) + 999) / 1000);
        return;
    }
    // No/expired challenge, empty password, or malformed MAC -> fail closed, but this is NOT a password guess
    // (no back-off, no wait reported).
    if (!_challengePending || (millis() - _challengeMs) > SEC_CHALLENGE_TTL || pwEmpty || length < SEC_MAC_LEN)
    {
        _challengePending = false;
        secAuthFail(resultData, resultLength, 0);
        return;
    }

    uint8_t expected[16];
    secComputeMac(pwKey, _nonce, expected);
    uint8_t diff = 0;
    for (uint8_t i = 0; i < SEC_MAC_LEN; i++) diff |= (uint8_t)(expected[i] ^ data[i]); // constant-time compare
    _challengePending = false; // single-use regardless of outcome

    if (diff == 0)
    {
        _authorized = true; // the ONLY place the window opens (security review MED: never in secRefreshWindow)
        secRefreshWindow();
        _authFailCount = 0; // a successful login resets the brute-force protection
        _authBackoffDur = 0;
        resultData[0] = 0x00;
        resultLength = 1;
        logInfoP("FTC authorized");
    }
    else
    {
        if (_authFailCount < 255) _authFailCount++;
        // 3 free tries; from the 4th failure the wait escalates 1,2,4,8,16,32,64 min (doubling, capped at 64).
        uint32_t backMs = 0;
        if (_authFailCount > 3)
        {
            uint8_t sh = (uint8_t)(_authFailCount - 4);
            if (sh > 6) sh = 6;                 // cap at 2^6 = 64 min
            backMs = (uint32_t)60000u << sh;
        }
        _authBackoffStart = millis();
        _authBackoffDur = backMs;
        secAuthFail(resultData, resultLength, backMs / 1000);
        logInfoP("FTC auth failed (%u), backoff %us", _authFailCount, (unsigned)(backMs / 1000));
    }
}
#endif

FileTransferModule openknxFileTransferModule;
#endif

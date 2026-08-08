#if defined(ARDUINO_ARCH_RP2040) || defined(ARDUINO_ARCH_ESP32)
#include "FileTransferModule.h"
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
#include <Update.h>
#endif
#ifdef OPENKNX_FTC_SECURITY
#include "knx/aes.hpp" // C++ wrapper (extern "C") around the AES already linked by knx; zero extra flash
// Fixed public constant, domain separation for the nonce seed only. Not a secret.
static const uint8_t FTM_SEC_K0[16] = {0x4F, 0x70, 0x65, 0x6E, 0x4B, 0x4E, 0x58, 0x46, 0x54, 0x43, 0x53, 0x65, 0x63, 0x75, 0x72, 0x65};
#endif

#ifdef OPENKNX_FTC_CONSOLE
// OGM-Common compat: newer commons have disableConsole(bool, const char* reason) (the "occupied (remote
// a.b.c)" notice); older ones (as some products still pin) only have disableConsole(bool). This shim picks
// the two-arg form when it compiles and silently drops the reason otherwise -> every downstream product
// builds regardless of its pinned OGM-Common version, with NO OGM-Common edit. Overload ranking: the `int`
// tag is preferred; if disableConsole(bool, const char*) is ill-formed, SFINAE falls back to the `long` one.
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
#if defined(OPENKNX_SDCARD) || defined(OPENKNX_EXTFLASH)
    crcLoop(); // advance a cooperative SD/EFC file-CRC job, if one is running (non-blocking)
#endif
}

enum class FtmCommands
{
    Format, // LittleFS.format()
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
    AuthLogout = 105     // FTC access control: close the authorized window immediately
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
    ftmDrive(newPath, &rn); // same drive assumed; use the stripped new-path
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
        return true;
    }
    if (secIsWriteCommand(propertyId)) secRefreshWindow(); // accepted write extends the idle window
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

        case FtmCommands::FileDownload:
        {
            cmdFileDownload(length, data, resultData, resultLength);
            return true;
        }

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
            return false; // false is correct
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
                // A DIFFERENT owner is refused (single-owner console, no hijack). But the SAME owner
                // re-opening means its previous session was lost (e.g. a drain answer that never crossed a
                // constrained interface, or a client that died without a CLOSE) -> take it over cleanly and
                // reset below, instead of locking it out until CON_IDLE_TMO. reqPa 0 (no PA sent) stays busy.
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
            ftcDisableConsole(openknx.console, true, _conOwnerStr, 0); // silence the local console; local input now gets a one-line "occupied (remote a.b.c)" notice (reason dropped on older OGM-Common)
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
        // An attached console (its 3 s keepalive) is ACTIVE diagnosis, not inactivity -> keep the PW auth
        // window fresh so a logged-in user is NEVER auto-logged-out while the console stays open (error
        // analysis may sit idle watching logs). The "Automatischer Logout nach Inaktivität" resumes counting
        // down normally once the console is closed. No effect on non-PW stages (window unused there).
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

#if defined(OPENKNX_SDCARD) || defined(OPENKNX_EXTFLASH)
// Advance the cooperative SD/EFC file CRC by a bounded slice per loop() pass -> NEVER a whole-file read in the
// dispatch (that reboots on a large file). The read handle (_src) was opened by cmdFileInfo when the job
// started and stays open until the answer flips to 0x00. A stranded job (client stopped polling) is reaped
// after HEARTBEAT_INTERVAL so it can never hold the drive's single handle forever.
void FileTransferModule::crcLoop()
{
    if (!_crcActive) return;
    uint8_t buf[64];
    while (_crcOff < _crcSize && openknx.common.freeLoopTime())
    {
        const uint8_t want = (uint8_t)MIN((uint32_t)sizeof(buf), _crcSize - _crcOff);
        uint8_t got = 0;
    #ifdef OPENKNX_SDCARD
        if (_crcDrive == FD_SD) got = sd::fileStore.read(_crcOff, buf, want);
    #endif
    #ifdef OPENKNX_EXTFLASH
        if (_crcDrive == FD_EFC) got = efc::fileStore.read(_crcOff, buf, want);
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
    #ifdef OPENKNX_SDCARD
    if (_crcDrive == FD_SD) sd::fileStore.close();
    #endif
    #ifdef OPENKNX_EXTFLASH
    if (_crcDrive == FD_EFC) efc::fileStore.close();
    #endif
    _crcActive = false;
    _crcOff = 0;
    _crcSize = 0;
    _crcFirst = true;
}
#endif

void FileTransferModule::cmdFormat(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength)
{
    resultLength = 1;

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
    uint8_t offset = 0;
    resultLength = 1;

    for (int i = 0; i < length; i++)
    {
        if (data[i] == 0)
        {
            offset = i + 1;
            break;
        }
    }

    // No NUL separator within the frame -> the two names aren't delimited (a self-rename + a %s read past the
    // frame). Reject rather than act on unterminated data.
    if (offset == 0)
    {
        pushByte(0x45, resultData);
        logErrorP("Rename frame has no name separator");
        return;
    }

    if (!ftmRename((char *)data, (char *)(data + offset)))
    {
        logErrorP("Renaming of the file \"%s\" to \"%s\" failed", data, data + offset);
        pushByte(0x45, resultData);
        return;
    }

    logInfoP("Renaming of the file \"%s\" to \"%s\" was successful", data, data + offset);
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

#ifdef ARDUINO_ARCH_RP2040
void FileTransferModule::cmdFwUpdate(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength)
{
    logInfoP("Update initiated");
    logIndentUp();
    picoOTA.begin();
    if (!picoOTA.addFile((char *)data)) // false = staged image missing / unreadable
    {
        logErrorP("staged image not found or unreadable: %s", (const char *)data);
        logIndentDown();
        return;
    }
    if (!picoOTA.commit()) // false = OTA command page not written -> do NOT reboot
    {
        logErrorP("PicoOTA commit failed -- apply aborted");
        logIndentDown();
        return;
    }
    _rebootRequested = millis();
    logInfoP("Firmware armed; device will restart in ~2s to apply");
    logIndentDown();
}
#elif defined(ARDUINO_ARCH_ESP32)
/**
 * @brief ESP32 self-apply: stream the staged LittleFS image into the OTA (app1) slot, then defer-reboot to boot it.
 */
void FileTransferModule::cmdFwUpdate(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength)
{
    logInfoP("Update initiated");
    logIndentUp();
    const char *path = (const char *)data; // NUL-terminated staged remote path from the client
    if (_fileOpen)                          // release any open transfer file before re-opening it read-only
    {
        ftmXferClose();
        _fileOpen = false;
    }
    File img = LittleFS.open(path, "r");
    if (!img)
    {
        logErrorP("staged image not found: %s", path);
        logIndentDown();
        return;
    }
    size_t sz = img.size();
    if (!Update.begin(sz)) // fails loud on a non-OTA/single-app partition layout
    {
        logErrorP("Update.begin(%u) failed: %s", (unsigned)sz, Update.errorString());
        img.close();
        logIndentDown();
        return;
    }
    size_t w = Update.writeStream(img);
    img.close();
    if (w != sz || !Update.end(true) || !Update.isFinished())
    {
        logErrorP("Update failed: %s", Update.errorString());
        Update.abort(); // release the OTA engine (frees the ~4KB sector buffer, re-arms begin()); idempotent if a write-fail already reset it
        logIndentDown();
        return;
    }
    logInfoP("Firmware written to OTA slot; reboot in ~2s to apply");
    _rebootRequested = millis(); // reuse loop()'s deferred flash.save()+restart()
    logIndentDown();
}
#endif

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

    _file = LittleFS.open(filename, "r");

    if (!_file)
    {
        pushByte(0x42, resultData);
        resultLength = 1;
        //logErrorP("File can't be opened"); // pre checks - should not throw this error
        _dirOpen = false;
        return;
    }

    size_t filesize = _file.size();
    // Later used in v2 with clock
    // time_t cr = _file.getCreationTime();
    // time_t lw = _file.getLastWrite();
    FastCRC32 crc32;
    uint32_t crc = 0;
    int len = 1000;
    bool first = true;
    uint8_t buf[len];
    while (_file.available())
    {
        int readed = _file.readBytes((char *)buf, len);
        if (first)
            crc = crc32.cksum((uint8_t *)buf, readed);
        else
            crc = crc32.cksum_upd((uint8_t *)buf, readed);
        first = false;
    }
    _file.close(); // close the CRC-loop handle; this path leaves _fileOpen false, so nothing auto-closes it later

    logInfoP("Read file info of \"%s\"", filename);
    logIndentUp();
    logInfoP("Filesize: %i bytes", filesize);
    logInfoP("CRC32: 0x%08X", crc);
    logIndentDown();

    pushByte(0x0, resultData);
    pushInt(filesize, resultData + 1);
    pushInt(crc, resultData + 5);
    resultLength = 9;

    // logHexDebugP(resultData, resultLength);
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
    #if defined(OPENKNX_SDCARD) || defined(OPENKNX_EXTFLASH) || defined(OPENKNX_EXTFLASH)
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

    memcpy(resultData + 2, fileName.c_str(), fileName.length());
    resultLength = fileName.length() + 2;
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

        // Optional exact total size (LE uint32) appended AFTER the NUL-terminated name -> SD preAllocate.
        // Bounds-checked within `length`; absent (older client) -> 0 = no preAllocate (legacy behaviour).
        uint32_t sizeHint = 0;
        for (uint8_t i = 4; i < length; i++)
            if (data[i] == '\0')
            {
                if ((size_t)i + 5 <= (size_t)length)
                    sizeHint = (uint32_t)data[i + 1] | ((uint32_t)data[i + 2] << 8) | ((uint32_t)data[i + 3] << 16) | ((uint32_t)data[i + 4] << 24);
                break;
            }

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
        resultLength = 0;
        return;
    }

    if (!checkOpenedFile(resultData, resultLength)) return;

    uint16_t sequence = data[1] << 8 | data[0];
    writeFile(sequence, data, length, resultData, resultLength);
}

/**
 * @brief FAST upload (cmd44): open(00 00)/close(FF FF) answered, DATA silent (returns false => no L7 answer).
 *
 * DATA frames are still AckRequested on the wire, so TP1 L2 keeps doing L_ACK/BUSY/retransmit --
 * the only remaining flow-control backstop for the silent stream.
 */
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
        // Optional exact total size (LE uint32) after the NUL name (from data+6) -> SD preAllocate; else 0 (legacy).
        uint32_t sizeHint = 0;
        for (uint8_t i = 6; i < length; i++)
            if (data[i] == '\0')
            {
                if ((size_t)i + 5 <= (size_t)length)
                    sizeHint = (uint32_t)data[i + 1] | ((uint32_t)data[i + 2] << 8) | ((uint32_t)data[i + 3] << 16) | ((uint32_t)data[i + 4] << 24);
                break;
            }
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
    return false; // SILENT (no L7 answer). L2 still ACKs -- the client sends fast DATA AckRequested.
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

void FileTransferModule::cmdFileDownload(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength)
{
    _heartbeat = millis();

    if (data[0] == 0x00 && data[1] == 0x00)
    {
        logInfoP("Download file \"%s\"", (char *)(data + 3));
        if (checkOpenFile(resultData, resultLength) || checkOpenDir(resultData, resultLength)) return;

        _size = data[2];

        // -6 = the 4-byte header (result/seq) + 2-byte CRC readFile() appends after the payload; without it a
        // 250..255 pkg overruns the fixed result buffer (bau resultData[0xFF]).
        if (data[2] > resultLength - 6)
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
        pushByte(0x0, resultData);
        resultLength = 5;
        return;
    }
    if (!checkOpenedFile(resultData, resultLength)) return;

    uint16_t sequence = data[1] << 8 | data[0];
    readFile(sequence, resultData, resultLength);
    _lastSequence = sequence;
}

void FileTransferModule::cmdCheckFeatures(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength)
{
    uint8_t result = 0;
    result |= 0x1; // Resume
#if defined(ARDUINO_ARCH_RP2040) || defined(ARDUINO_ARCH_ESP32)
    result |= 0x2; // Update
#endif
    result |= 0x4; // FAST: server understands cmd44/cmd45; one bit covers both windowed & forget.
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

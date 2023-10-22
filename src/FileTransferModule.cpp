#include "FileTransferModule.h"
#include "versions.h"
#include <PicoOTA.h>

// Give your Module a name
// it will be displayed when you use the method log("Hello")
//  -> Log     Hello
const std::string FileTransferModule::name()
{
    return "FileTransfer";
}

// You can also give it a version
// will be displayed in Command Infos
const std::string FileTransferModule::version()
{
    return MODULE_FileTransferModule_Version;
}

void FileTransferModule::loop(bool configured)
{
    // check lastAction
    // close file or directory after HEARTBEAT_INTERVAL

    if (_fileOpen && delayCheck(_heartbeat, HEARTBEAT_INTERVAL))
    {
        _file.close();
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
        logInfoP("Restarting now");
        logIndentUp();
        openknx.flash.save();
        logIndentDown();
        delay(100);
        rp2040.reboot();
    }
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
    DirList = 80,
    DirCreate,
    DirDelete,
    Cancel = 90,
    ModuleVersion = 100,
    FwUpdate
};

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

void FileTransferModule::readFile(uint16_t sequence, uint8_t *resultData, uint8_t &resultLength)
{
    logIndentUp();

    if (_lastSequence + 1 != sequence)
        _file.seek((sequence - 1) * (_size - 6));

    pushByte(0x0, resultData);
    pushWord(sequence, resultData + 1);
    uint8_t readed = _file.readBytes((char *)resultData + 4, _size - 6);
    pushByte(readed, resultData + 3);

    logDebugP("Readed sequence %i (%i/%i bytes)", sequence, readed, _size - 6);
    if (readed == 0 || !_file.available())
    {
        _file.close();
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

void FileTransferModule::writeFile(uint16_t sequence, uint8_t *data, uint8_t length, uint8_t *resultData, uint8_t &resultLength)
{
    logIndentUp();

    if (_lastSequence + 1 != sequence)
    {
        if (!_file.seek((sequence - 1) * (_size - 3)))
        {
            pushByte(0x46, resultData);
            resultLength = 1;
            logErrorP("The file can't seek to position");
            logIndentDown();
            return;
        }
    }

    uint8_t written = _file.write((char *)data + 3, data[2]);

    if (sequence % 10 == 0)
        _file.flush();

    if (written != data[2])
    {
        pushByte(0x47, resultData);
        resultLength = 1;
        logErrorP("The file could not be written completely (%i/%i)", written, length);
        logIndentDown();
        return;
    }

    logDebugP("Written sequence %i (%i/%i bytes)", sequence, written, data[2]);

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
    if (objectIndex != 159) return false;
    _lastAccess = millis();
    openknx.common.skipLooptimeWarning();

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
            return true;
        }

        case FtmCommands::FileInfo:
        {
            cmdFileInfo(length, data, resultData, resultLength);
            return true;
        }

        case FtmCommands::ModuleVersion:
        {
            cmdModuleVersion(length, data, resultData, resultLength);
            return true;
        }

        case FtmCommands::FwUpdate:
        {
            cmdFwUpdate(length, data, resultData, resultLength);
            return false; // false is correct
        }
    }
    return false;
}

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
    bool exists = LittleFS.exists((char *)data);
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
        _file.close();
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

    if (!LittleFS.rename((char *)data, (char *)(data + offset)))
    {
        logErrorP("Renaming of the file \"%s\" to \"%s\" failed %i", data, data + offset);
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
    resultData[4] = _build >> 8;
    resultData[5] = _build & 0xFF;
}

void FileTransferModule::cmdFwUpdate(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength)
{
    logInfoP("Updated initiated");
    logIndentUp();
    picoOTA.begin();
    picoOTA.addFile((char *)data);
    picoOTA.commit();
    _rebootRequested = millis();
    logInfoP("Device will restart in 2000ms");
    logIndentDown();
}

void FileTransferModule::cmdFileInfo(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength)
{
    const char *filename = (char *)data;
    _file = LittleFS.open(filename, "r");

    if (!_file)
    {
        pushByte(0x42, resultData);
        resultLength = 1;
        logErrorP("File can't be opened");
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

void FileTransferModule::cmdDirList(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength)
{
    _heartbeat = millis();

    if (!_dirOpen)
    {
        logDebugP("List directory \"%s\"", (char *)data);
        _dir = LittleFS.openDir((char *)data);
        _dirOpen = true;
    }

    if (!checkOpenedDir(resultData, resultLength))
        return;

    if (!_dir.next())
    {
        resultLength = 2;
        pushByte(0x0, resultData);
        pushByte(0x0, resultData + 1);
        logDebugP("List directory completed");
        _dirOpen = false;
        return;
    }

    pushByte(0x0, resultData);
    pushByte(_dir.isFile() ? 0x01 : 0x02, resultData + 1); // 0x00 = no more content

    String fileName = _dir.fileName();
    logDebugP("- %s", fileName.c_str());

    memcpy(resultData + 2, fileName.c_str(), fileName.length());
    resultLength = fileName.length() + 2;
}

void FileTransferModule::cmdDirCreate(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength)
{
    if (checkOpenFile(resultData, resultLength) || checkOpenDir(resultData, resultLength)) return;
    resultLength = 1;

    if (!LittleFS.mkdir((char *)data))
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

    if (!LittleFS.rmdir((char *)data))
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

    if (!LittleFS.remove((char *)data))
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
        const char *filename = (const char *)(data + 3);
        if (checkOpenFile(resultData, resultLength) || checkOpenDir(resultData, resultLength)) return;

        _size = data[2];

        _file = LittleFS.open(filename, "w");
        if (!_file)
        {
            pushByte(0x42, resultData);
            resultLength = 1;
            logErrorP("Start file upload to \"%s\" is failed", filename);
            return;
        }

        logInfoP("Start file upload to \"%s\"", filename);
        _fileOpen = true;
        _lastSequence = 0;
        pushByte(0x0, resultData);
        resultLength = 1;
        return;
    }

    if (data[0] == 0xFF && data[1] == 0xFF)
    {
        logInfoP("The file upload was successfully completed");
        _file.flush();
        _file.close();
        _fileOpen = false;
        resultLength = 0;
        return;
    }

    if (!checkOpenedFile(resultData, resultLength)) return;

    uint16_t sequence = data[1] << 8 | data[0];
    writeFile(sequence, data, length, resultData, resultLength);
}

void FileTransferModule::cmdFileDownload(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength)
{
    _heartbeat = millis();

    if (data[0] == 0x00 && data[1] == 0x00)
    {
        logInfoP("Download file \"%s\"", (char *)(data + 3));
        if (checkOpenFile(resultData, resultLength) || checkOpenDir(resultData, resultLength)) return;

        _size = data[2];

        if (data[2] > resultLength)
        {
            logIndentUp();
            logErrorP("Requested pkg is greater than max resultLength");
            logIndentDown();
            pushByte(0x4, resultData);
            resultLength = 1;
            return;
        }

        _file = LittleFS.open((char *)(data + 3), "r");
        if (!_file)
        {
            pushByte(0x42, resultData);
            resultLength = 1;
            logIndentUp();
            logErrorP("File can't be opened");
            logIndentDown();
            return;
        }
        _fileOpen = true;

        _lastSequence = 0;
        int fileSize = _file.size();
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

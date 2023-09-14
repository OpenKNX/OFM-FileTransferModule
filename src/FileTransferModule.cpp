#include "FileTransferModule.h"
#include "versions.h"
#include <PicoOTA.h>

// Give your Module a name
// it will be displayed when you use the method log("Hello")
//  -> Log     Hello
const std::string FileTransferModule::name()
{
    return "FileTransferModule";
}

// You can also give it a version
// will be displayed in Command Infos
const std::string FileTransferModule::version()
{
    return MODULE_FileTransferModule_Version;
}

void FileTransferModule::loop(bool conf)
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
    GetVersion = 100,
    DoUpdate
};

bool FileTransferModule::checkOpenFile(uint8_t *resultData, uint8_t &resultLength)
{
    if (_fileOpen)
    {
        resultLength = 1;
        resultData[0] = 0x41;
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
        resultData[0] = 0x43;
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
        resultData[0] = 0x81;
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
        resultData[0] = 0x83;
        logErrorP("Dir not opened");
        return false;
    }
    return true;
}

void FileTransferModule::FileRead(uint16_t sequence, uint8_t *resultData, uint8_t &resultLength)
{
    if (_lastSequence + 1 != sequence)
        _file.seek((sequence - 1) * _size);

    resultData[0] = 0x00;
    resultData[1] = sequence & 0xFF;
    resultData[2] = (sequence >> 8) & 0xFF;
    int readed = _file.readBytes((char *)resultData + 4, _size - 6);
    resultData[3] = readed & 0xFF;

    logDebugP("readed %i/%i bytes", readed, _size - 6);
    logIndentUp();
    if (readed == 0)
    {
        _file.close();
        _fileOpen = false;
        logInfoP("file closed");
    }

    FastCRC16 crc16;
    uint16_t crc = crc16.modbus(resultData + 1, readed + 3);
    resultData[readed + 4] = crc >> 8;
    resultData[readed + 5] = crc & 0xFF;
    logInfoP("crc: %i", crc);
    logIndentDown();

    resultLength = readed + 6;
}

void FileTransferModule::FileWrite(uint16_t sequence, uint8_t *data, uint8_t length, uint8_t *resultData, uint8_t &resultLength)
{
    if (_lastSequence + 1 != sequence)
    {
        logErrorP("seeking to %i", (sequence - 1) * (_size - 3));
        if (!_file.seek((sequence - 1) * (_size - 3)))
        {
            resultData[0] = 0x46;
            resultLength = 1;
            logErrorP("File can't seek position");
            return;
        }
    }

    uint8_t xx = _file.write((char *)data + 3, data[2]);

    if (sequence % 10 == 0)
        _file.flush();

    if (xx != data[2])
    {
        logErrorP("nicht so viel geschrieben wie bekommen %i - %i", xx, length);
    }

    FastCRC16 crc16;
    uint16_t crc = crc16.modbus(data, length);

    resultData[0] = 0x00;
    resultData[1] = sequence & 0xFF;
    resultData[2] = (sequence >> 8) & 0xFF;
    resultData[3] = crc >> 8;
    resultData[4] = crc & 0xFF;

    resultLength = 5;

    _lastSequence = sequence;
}

bool FileTransferModule::processFunctionProperty(uint8_t objectIndex, uint8_t propertyId, uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength)
{
    if (objectIndex != 159) return false;
    _lastAccess = millis();

    switch ((FtmCommands)propertyId)
    {
        case FtmCommands::Format:
        {
            if (!LittleFS.format())
            {
                resultLength = 1;
                resultData[0] = 0x02;
                logErrorP("Formatting of the file system has failed");
                return true;
            }

            logInfoP("The file system was successfully formatted");
            resultLength = 1;
            resultData[0] = 0x00;
            return true;
        }

        case FtmCommands::Exists:
        {
            resultData[0] = 0x00;
            resultData[1] = LittleFS.exists((char *)data);
            if (resultData[1])
                logDebugP("The file or directory \"%s\" exists", data);
            else
                logDebugP("The file or directory \"%s\" does not exist", data);

            resultLength = 2;
            return true;
        }

        case FtmCommands::Rename:
        {
            int offset = 0;
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
                resultLength = 1;
                resultData[0] = 0x45;
                logErrorP("Renaming of the file \"%s\" to \"%s\" failed", data, data + offset);
                return true;
            }

            logInfoP("Renaming of the file \"%s\" to \"%s\" was successful", data, data + offset);
            resultLength = 1;
            resultData[0] = 0x00;
            return true;
        }

        case FtmCommands::FileDownload:
        {
            _heartbeat = millis();

            logInfoP("File download x");
            if (data[0] == 0x00 && data[1] == 0x00)
            {
                logInfoP("File download");
                if (checkOpenFile(resultData, resultLength) || checkOpenDir(resultData, resultLength))
                    return true;

                _size = data[2] - 5;

                if(data[2] > resultLength)
                {
                    logErrorP("Angeforderte pkg ist größer als max resultLength");
                    resultLength = 1;
                    resultData[0] = 0x04;
                    return true;
                }

                logInfoP("Path: %s", data + 3);

                _file = LittleFS.open((char *)(data + 3), "r");
                if (!_file)
                {
                    resultLength = 1;
                    resultData[0] = 0x42;
                    logErrorP("File can't be opened");
                    return true;
                }
                _fileOpen = true;

                _lastSequence = 0;
                int fileSize = _file.size();
                resultData[0] = 0x00;
                resultData[1] = fileSize & 0xFF;
                resultData[2] = (fileSize >> 8) & 0xFF;
                resultData[3] = (fileSize >> 16) & 0xFF;
                resultData[4] = (fileSize >> 24) & 0xFF;
                resultLength = 5;
                return true;
            }
            if (!checkOpenedFile(resultData, resultLength))
                return true;

            uint16_t sequence = data[1] << 8 | data[0];
            FileRead(sequence, resultData, resultLength);
            _lastSequence = sequence;
            return true;
        }

        case FtmCommands::FileUpload:
        {
            _heartbeat = millis();

            if (data[0] == 0x00 && data[1] == 0x00)
            {
                const char *filename = (const char *)(data + 3);
                if (checkOpenFile(resultData, resultLength) || checkOpenDir(resultData, resultLength))
                    return true;

                _size = data[2];

                _file = LittleFS.open(filename, "w");
                if (!_file)
                {
                    resultLength = 1;
                    resultData[0] = 0x42;
                    logErrorP("Start file upload to \"%s\" is failed", filename);
                    return true;
                }

                logInfoP("Start file upload to \"%s\"", filename);
                _fileOpen = true;
                _lastSequence = 0;
                resultData[0] = 0x00;
                resultLength = 1;
                return true;
            }
            if (data[0] == 0xFF && data[1] == 0xFF)
            {
                logInfoP("The file upload was successfully completed");
                _file.flush();
                _file.close();
                _fileOpen = false;
                resultLength = 0;
                return true;
            }
            if (!checkOpenedFile(resultData, resultLength))
                return true;

            uint16_t sequence = data[1] << 8 | data[0];
            FileWrite(sequence, data, length, resultData, resultLength);
            return true;
        }

        case FtmCommands::FileDelete:
        {
            if (checkOpenFile(resultData, resultLength) || checkOpenDir(resultData, resultLength))
                return true;

            if (!LittleFS.remove((char *)data))
            {
                resultLength = 1;
                resultData[0] = 0x44;
                logErrorP("Deleting of the file \"%s\" failed", data);
                return true;
            }

            logInfoP("Deleting of the file \"%s\" was successful", data);
            resultLength = 1;
            resultData[0] = 0x00;
            return true;
        }

        case FtmCommands::DirCreate:
        {
            if (checkOpenFile(resultData, resultLength) || checkOpenDir(resultData, resultLength))
                return true;

            if (!LittleFS.mkdir((char *)data))
            {
                resultLength = 1;
                resultData[0] = 0x85;
                logErrorP("Creation of the folder \"%s\" failed", data);
                return true;
            }

            logInfoP("Creation of the folder \"%s\" was successful", data);
            resultLength = 1;
            resultData[0] = 0x00;
            return true;
        }

        case FtmCommands::DirDelete:
        {
            if (checkOpenFile(resultData, resultLength) || checkOpenDir(resultData, resultLength))
                return true;

            if (!LittleFS.rmdir((char *)data))
            {
                resultLength = 1;
                resultData[0] = 0x84;
                logInfoP("Deleting of the folder \"%s\" failed", data);
                return true;
            }

            logInfoP("Deleting of the folder \"%s\" was successful", data);
            resultLength = 1;
            resultData[0] = 0x00;
            return true;
        }

        case FtmCommands::DirList:
        {
            _heartbeat = millis();

            if (!_dirOpen)
            {
                logDebugP("List directory \"%s\"", (char *)data);
                _dir = LittleFS.openDir((char *)data);
                _dirOpen = true;
            }

            if (!checkOpenedDir(resultData, resultLength))
                return true;

            if (!_dir.next())
            {
                resultLength = 2;
                resultData[0] = 0x00;
                resultData[1] = 0x00;
                logDebugP("List directory completed");
                _dirOpen = false;
                return true;
            }

            resultData[0] = 0x00;
            resultData[1] = _dir.isFile() ? 0x01 : 0x02; // 0x00 = no more content

            String fileName = _dir.fileName();
            logDebugP("- %s", fileName.c_str());

            memcpy(resultData + 2, fileName.c_str(), fileName.length());
            resultLength = fileName.length() + 2;

            return true;
        }

        case FtmCommands::Cancel:
        {
            logDebugP("Cancel");
            if (_fileOpen)
            {
                _file.close();
                _fileOpen = false;
            }

            if (_dirOpen)
            {
                _dirOpen = false;
            }
            resultLength = 0;
            return true;
        }

        case FtmCommands::FileInfo:
        {
            const char *filename = (char *)data;
            _file = LittleFS.open(filename, "r");

            if (!_file)
            {
                resultLength = 2;
                resultData[0] = 0x42;
                resultData[1] = 0x00;
                logErrorP("File can't be opened");
                _dirOpen = false;
                return true;
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

            resultData[0] = 0x00;
            memcpy(resultData + 1, (uint8_t *)&filesize, 4);
            memcpy(resultData + 5, (uint8_t *)&crc, 4);
            resultLength = 1 + 4 + 4;
            // logHexDebugP(resultData, resultLength);
            return true;
        }

        case FtmCommands::GetVersion:
        {
            resultLength = 6;
            resultData[0] = _major >> 8;
            resultData[1] = _major & 0xFF;
            resultData[2] = _minor >> 8;
            resultData[3] = _minor & 0xFF;
            resultData[4] = _build >> 8;
            resultData[5] = _build & 0xFF;
            return true;
        }

        case FtmCommands::DoUpdate:
        {
            logInfoP("Updated initiated");
            logIndentUp();
            picoOTA.begin();
            picoOTA.addFile((char *)data);
            picoOTA.commit();
            resultLength = 0;
            _rebootRequested = millis();
            openknx.flash.save();
            logInfoP("Device will restart in 2000ms");
            logIndentDown();
        }
    }
    return false;
}
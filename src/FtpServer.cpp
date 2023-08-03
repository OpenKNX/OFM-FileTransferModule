#include "FtpServer.h"


//Give your Module a name
//it will be displayed when you use the method log("Hello")
// -> Log     Hello
const std::string FtpServer::name()
{
    return "FtpServer";
}

//You can also give it a version
//will be displayed in Command Infos 
const std::string FtpServer::version()
{
    //also update library.json
    return "0.0dev";
}

void FtpServer::loop()
{  
    //check lastAction
    //close file or directory after 3 seconds    

    if(_fileOpen && millis() - _heartbeat > 3000)
    {
        _file.close();
        _fileOpen = false;
        logErrorP("File closed due no heartbeat");
    }
    
    if(_dirOpen && millis() - _heartbeat > 3000)
    {
        _dirOpen = false;
        logErrorP("Dir closed due no heartbeat");
    }
}


enum class FtpCommands
{
    Format,     //LittleFS.format()
    Exists,     //LittleFS.exists(path)
    Rename,
    FileUpload = 40,
    FileDownload,
    FileDelete,
    DirList = 80,
    DirCreate,
    DirDelete,
    Cancel = 90
};


bool FtpServer::openFileSystem()
{
    if(_fsOpen) return true;

    return LittleFS.begin();
}

bool FtpServer::checkOpenFile(uint8_t *resultData, uint8_t &resultLength)
{
    if(_fileOpen)
    {
        resultLength = 1;
        resultData[0] = 0x41;
        logErrorP("File already open");
        return true;
    }
    return false;
}

bool FtpServer::checkOpenedFile(uint8_t *resultData, uint8_t &resultLength)
{
    if(!_fileOpen)
    {
        resultLength = 1;
        resultData[0] = 0x43;
        logErrorP("File not opened");
        return false;
    }
    return true;
}

bool FtpServer::checkOpenDir(uint8_t *resultData, uint8_t &resultLength)
{
    if(_dirOpen)
    {
        resultLength = 1;
        resultData[0] = 0x81;
        logErrorP("Dir already open");
        return true;
    }
    return false;
}

bool FtpServer::checkOpenedDir(uint8_t *resultData, uint8_t &resultLength)
{
    if(!_dirOpen)
    {
        resultLength = 1;
        resultData[0] = 0x83;
        logErrorP("Dir not opened");
        return false;
    }
    return true;
}

void FtpServer::FileRead(uint16_t sequence, uint8_t *resultData, uint8_t &resultLength)
{
    if(_lastSequence+1 != sequence)
        _file.seek((sequence-1) * _size);

    resultData[0] = 0x00;
    resultData[1] = sequence & 0xFF;
    resultData[2] = (sequence >> 8) & 0xFF;
    int readed = _file.readBytes((char*)resultData+3, _size);

    FastCRC16 crc16;
    uint16_t crc = crc16.modbus(resultData+1, readed+2);
    resultData[readed+3] = crc >> 8;
    resultData[readed+4] = crc & 0xFF;

    if(readed != _size)
    {
        _file.close();
        _fileOpen = false;
        logInfoP("File closed - Read");
    }
    
    resultLength = readed+5;
    _lastSequence = sequence;
}

void FtpServer::FileWrite(uint16_t sequence, uint8_t *data, uint8_t length, uint8_t *resultData, uint8_t &resultLength)
{
    if(_lastSequence+1 != sequence)
    {
        if(!_file.seek((sequence-1) * _size))
        {
            resultData[0] = 0x46;
            resultLength = 1;
            logErrorP("File can't seek position");
            return;
        }
    }

    uint8_t xx = _file.write((char*)data+2, length-2);

    if(xx != length)
    {
        logErrorP("nicht so viel geschrieben wie bekommen");
    }

    if(length != _size)
    {
        _file.close();
        _fileOpen = false;
        logInfoP("File closed - Write");
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

bool FtpServer::processFunctionProperty(uint8_t objectIndex, uint8_t propertyId, uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength)
{
    if(objectIndex != 159) return false;
    _lastAccess = millis();

    switch((FtpCommands)propertyId)
    {
        case FtpCommands::Format:
        {
            logInfoP("Format");
            if(!openFileSystem())
            {
                resultLength = 1;
                resultData[0] = 0x01;
                logErrorP("LittleFS.begin() failed");
                return true;
            }
            logInfoP("Format filesystem");
            if(!LittleFS.format())
            {
                resultLength = 1;
                resultData[0] = 0x02;
                logErrorP("LittleFS.format() failed");
                return true;
            }
            resultLength = 1;
            resultData[0] = 0x00;
            return true;
        }
        
        case FtpCommands::Exists:
        {
            logInfoP("Exists");
            if(!openFileSystem())
            {
                resultLength = 1;
                resultData[0] = 0x01;
                logErrorP("LittleFS.begin() failed");
                return true;
            }
            
            resultData[0] = 0x00;
            resultData[1] = LittleFS.exists((char*)data);
            resultLength = 2;
            return true;
        }

        case FtpCommands::Rename:
        {
            logInfoP("Rename");
            if(!openFileSystem())
            {
                resultLength = 1;
                resultData[0] = 0x01;
                logErrorP("Filesystem begin failed");
                return true;
            }
            
            int offset = 0;
            for(int i = 0; i < length; i++)
            {
                if(data[i] == 0)
                {
                    offset = i + 1;
                    break;
                }
            }

            logInfoP("from %s to %s", data, data+offset);

            if(!LittleFS.rename((char*)data, (char*)(data+offset)))
            {
                resultLength = 1;
                resultData[0] = 0x45;
                logErrorP("File can't be renamed");
                return true;
            }
            
            resultLength = 1;
            resultData[0] = 0x00;
            return true;
        }
        
        case FtpCommands::FileDownload:
        {
            _heartbeat = millis();
            if(!openFileSystem())
            {
                resultLength = 1;
                resultData[0] = 0x01;
                logErrorP("LittleFS.begin() failed");
                return true;
            }

            if(data[0] == 0x00 && data[1] == 0x00)
            {
                logInfoP("File download");
                if(checkOpenFile(resultData, resultLength) || checkOpenDir(resultData, resultLength))
                    return true;

                _size = data[2] - 5;

                logInfoP("Path: %s", data+3);

                _file = LittleFS.open((char*)(data+3), "r");
                if (!_file) {
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
            if(!checkOpenedFile(resultData, resultLength))
                return true;

            uint16_t sequence = data[1] << 8 | data[0];
            FileRead(sequence, resultData, resultLength);
            _lastSequence = sequence;
            return true;
        }

        case FtpCommands::FileUpload:
        {
            _heartbeat = millis();
            logInfoP("File upload");
            if(!openFileSystem())
            {
                resultLength = 1;
                resultData[0] = 0x01;
                logErrorP("LittleFS.begin() failed");
                return true;
            }

            if(data[0] == 0x00 && data[1] == 0x00)
            {
                if(checkOpenFile(resultData, resultLength) || checkOpenDir(resultData, resultLength))
                    return true;

                _size = data[2] - 5;

                logInfoP("Path: %s", data+3);

                _file = LittleFS.open((char*)(data+3), "w");
                if (!_file) {
                    resultLength = 1;
                    resultData[0] = 0x42;
                    logErrorP("File can't be opened");
                    return true;
                }
                _fileOpen = true;
                _lastSequence = 0;
                resultData[0] = 0x00;
                resultLength = 1;
                logInfoP("File opened");
                return true;
            }
            if(!checkOpenedFile(resultData, resultLength))
                return true;

            uint16_t sequence = data[1] << 8 | data[0];
            FileWrite(sequence, data, length, resultData, resultLength);
            return true;
        }
        
        case FtpCommands::FileDelete:
        {
            logInfoP("File delete %s", data);
            if(!openFileSystem())
            {
                resultLength = 1;
                resultData[0] = 0x01;
                logErrorP("LittleFS.begin() failed");
                return true;
            }
            
            if(checkOpenFile(resultData, resultLength) || checkOpenDir(resultData, resultLength))
                return true;

            if(!LittleFS.remove((char*)data))
            {
                resultLength = 1;
                resultData[0] = 0x44;
                logErrorP("File can't be deleted");
                return true;
            }
            
            resultLength = 0;
            resultData[0] = 0x00;
            return true;
        }
        
        case FtpCommands::DirCreate:
        {
            logInfoP("Dir create %s", data);
            if(!openFileSystem())
            {
                resultLength = 1;
                resultData[0] = 0x01;
                logErrorP("LittleFS.begin() failed");
                return true;
            }

            if(checkOpenFile(resultData, resultLength) || checkOpenDir(resultData, resultLength))
                return true;

            if(!LittleFS.mkdir((char*)data))
            {
                resultLength = 1;
                resultData[0] = 0x85;
                logErrorP("Dir can't be created");
                return true;
            }
            
            resultLength = 1;
            resultData[0] = 0x00;
            return true;
        }
        
        case FtpCommands::DirDelete:
        {
            logInfoP("Dir delete %s", data);
            if(!openFileSystem())
            {
                resultLength = 1;
                resultData[0] = 0x01;
                logErrorP("LittleFS.begin() failed");
                return true;
            }

            if(checkOpenFile(resultData, resultLength) || checkOpenDir(resultData, resultLength))
                return true;

            if(!LittleFS.rmdir((char*)data))
            {
                resultLength = 1;
                resultData[0] = 0x84;
                logErrorP("Dir can't be deleted");
                return true;
            }
            
            resultLength = 1;
            resultData[0] = 0x00;
            return true;
        }

        case FtpCommands::DirList:
        {
            _heartbeat = millis();
            if(!openFileSystem())
            {
                resultLength = 1;
                resultData[0] = 0x01;
                logErrorP("LittleFS.begin() failed");
                return true;
            }

            if(!_dirOpen)
            {
                logInfoP("Dir list %s", (char*)data);
                _dir = LittleFS.openDir((char*)data);
                _dirOpen = true;
            }

            if(checkOpenedDir(resultData, resultLength))
                return true;
            
            if(!_dir.next())
            {
                resultLength = 2;
                resultData[0] = 0x00;
                resultData[1] = 0x00;
                logErrorP("Dir has no more files");
                _dirOpen = false;
                return true;
            }

            resultData[0] = 0x00;
            resultData[1] = _dir.isFile() ? 0x01 : 0x02; //0x00 = no more content
            
            String fileName = _dir.fileName();
            int pathlength = fileName.length();

            uint8_t *chars = new uint8_t[pathlength+1];
            for(int i = 0; i < pathlength; i++)
                chars[i] = fileName[i];
            chars[pathlength] = 0x00;

            memcpy(resultData + 2, chars, pathlength+1);
            logInfoP(_dir.fileName().c_str());
            resultLength = pathlength + 2;

            delete[] chars;
            return true;
        }

        case FtpCommands::Cancel:
        {
            if(_fileOpen)
            {
                _file.close();
                _fileOpen = false;
                logInfoP("File closed - Cancel");
            }

            if(_dirOpen)
            {
                _dirOpen = false;
            }
            resultLength = 0;
            return true;
        }
    }
    return false;
}
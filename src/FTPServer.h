#include <Arduino.h>
#include <LittleFS.h>
#include "OpenKNX.h"
#include "FastCRC.h"

#define INFO_INTERVAL 10000

class FtpServer : public OpenKNX::Module
{
	public:
		const std::string name() override;
		const std::string version() override;
		void loop() override;

	private:
        long _lastAccess = 0;
        File _file;
        Dir _dir;
        bool _fileOpen = false;
        bool _dirOpen = false;
        bool _fsOpen = false;
        bool openFileSystem();
        bool checkOpenedFile(uint8_t *resultData, uint8_t &resultLength);
        bool checkOpenedDir(uint8_t *resultData, uint8_t &resultLength);
        bool checkOpenFile(uint8_t *resultData, uint8_t &resultLength);
        bool checkOpenDir(uint8_t *resultData, uint8_t &resultLength);
		bool processFunctionProperty(uint8_t objectIndex, uint8_t propertyId, uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength) override;
		//bool processFunctionPropertyState(uint8_t objectIndex, uint8_t propertyId, uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength) override;
};

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
    return "0.0dev";
}

void FtpServer::loop()
{
    
}


enum class FtpCommands
{
    Format,     //LittleFS.format()
    Exists,     //LittleFS.exists(path)
    End,
    FileOpen = 40,
    FileWrite,
    FileRead,
    FileInfo,
    FileSize,
    FileClose,
    FileRename, //LittleFS.rename(pathFrom, pathTo)
    FileRemove, //LittleFS.remove(path)
    DirOpen = 80,    //LittleFS.open(path).next()
    DirGet,
    DirClose,
    DirMake,    //LittleFS.mkdir(path)
    DirRemove,  //LittleFS.rmdir(path)
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
        return true;
    }
    return false;
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
        return true;
    }
    return false;
}

bool FtpServer::processFunctionProperty(uint8_t objectIndex, uint8_t propertyId, uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength)
{
    if(objectIndex != 164) return false;
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
            logInfoP("Format end");
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
        
        case FtpCommands::End:
        {
            logInfoP("End");
            if(!_fsOpen)
            {
                resultLength = 1;
                resultData[0] = 0x03;
                logErrorP("LittleFS not initialized");
                return true;
            }
            LittleFS.end();
            resultLength = 1;
            resultData[0] = 0x00;
            return true;
        }
        
        case FtpCommands::FileOpen:
        {
            logInfoP("File open");
            if(!openFileSystem())
            {
                resultLength = 1;
                resultData[0] = 0x01;
                logErrorP("LittleFS.begin() failed");
                return true;
            }

            if(checkOpenFile(resultData, resultLength) || checkOpenDir(resultData, resultLength))
                return true;

            uint8_t accessmode = data[0];
            char* mode;
            switch(accessmode)
            {
                case 0:
                    mode = (char*)"r";
                    break;
                case 1:
                    mode = (char*)"r+";
                    break;
                case 2:
                    mode = (char*)"w";
                    break;
                case 3:
                    mode = (char*)"w+";
                    break;
                case 4:
                    mode = (char*)"a";
                    break;
                case 5:
                    mode = (char*)"a+";
                    break;
            }

            _file = LittleFS.open((char*)(data+1), mode);
            if (!_file) {
                resultLength = 1;
                resultData[0] = 0x42;
                logErrorP("File can't be opened");
                return true;
            }

            _fileOpen = true;
            resultLength = 0;
            resultData[0] = 0x00;
            return true;
        }
        
        case FtpCommands::FileWrite:
        {
            logInfoP("File write");
            
            if(checkOpenedFile(resultData, resultLength))
                return true;

            uint32_t position = data[3] << 24 | data[2] << 16 | data[1] << 8 | data[0];
            _file.seek(position);

            if(_file.write(data +4, length -4) != length -4)
            {
                resultData[0] = 0x46;
                resultLength = 1;
                logErrorP("File wrote less than it should");
                return true;
            }

            FastCRC16 crc16;
            uint16_t crc = crc16.modbus(data, length);

            resultData[0] = 0x00;
            resultData[1] = crc >> 8;
            resultData[2] = crc & 0xFF;
            resultLength = 3;
            return true;
        }
        
        case FtpCommands::FileRead:
        {
            logInfoP("File read");
            
            if(checkOpenedFile(resultData, resultLength))
                return true;

            uint32_t position = data[3] << 24 | data[2] << 16 | data[1] << 8 | data[0];
            uint8_t size = data[4];
            _file.seek(position);

            uint8_t* buffer = new uint8_t[size + 4];
            buffer[0] = data[0];
            buffer[1] = data[1];
            buffer[2] = data[2];
            buffer[3] = data[3];
            int readed = _file.read(buffer + 4, size);

            if(readed == 0)
            {
                resultData[0] = 0x47;
                resultLength = 1;
                logInfoP("File position reached end");
                return true;
            }

            FastCRC16 crc16;
            uint16_t crc = crc16.modbus(buffer, readed + 4);

            resultData[0] = 0x00;
            resultData[1] = crc >> 8;
            resultData[2] = crc & 0xFF;

            memcpy(resultData+3, buffer+4, readed);

            resultLength = size + 3;

            delete[] buffer;
            return true;
        }
        
        case FtpCommands::FileClose:
        {
            logInfoP("File close");
            
            if(checkOpenedFile(resultData, resultLength))
                return true;

            _fileOpen = false;
            resultLength = 0;
            resultData[0] = 0x00;
            return true;
        }
        
        case FtpCommands::FileRename:
        {
            if(!openFileSystem())
            {
                resultLength = 1;
                resultData[0] = 0x01;
                logErrorP("Filesystem begin failed");
                return true;
            }
            
            if(!checkOpenedFile(resultData, resultLength))
                return true;
            
            int offset = 0;
            for(int i = 0; i < length; i++)
                if(data[i] == 0)
                    offset = i + 1;

            if(!LittleFS.rename((char*)data, (char*)(data+offset)));
            {
                resultLength = 1;
                resultData[0] = 0x45;
                logErrorP("File can't be renamed");
                return true;
            }
            
            resultLength = 0;
            resultData[0] = 0x00;
            return true;
        }
        
        case FtpCommands::FileRemove:
        {
            if(!openFileSystem())
            {
                resultLength = 1;
                resultData[0] = 0x01;
                logErrorP("Filesystem begin failed");
                return true;
            }
            
            if(checkOpenFile(resultData, resultLength) || checkOpenDir(resultData, resultLength))
                return true;

            if(!LittleFS.remove((char*)data));
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
        
        case FtpCommands::DirOpen:
        {
            logInfoP("Dir open %s", data);
            if(!openFileSystem())
            {
                resultLength = 1;
                resultData[0] = 0x01;
                logErrorP("LittleFS.begin() failed");
                return true;
            }

            if(checkOpenFile(resultData, resultLength) || checkOpenDir(resultData, resultLength))
                return true;

            _dir = LittleFS.openDir((char*)data);
            
            //Todo doesnt work for now
            /*if(!_dir)
            {
                resultLength = 1;
                resultData[0] = 0x82;
                logErrorP("Dir can't be opened");
                return true;
            }*/

            resultLength = 1;
            resultData[0] = 0x00;
            return true;
        }
        
        case FtpCommands::DirGet:
        {
            logInfoP("Dir get");
            if(!openFileSystem())
            {
                resultLength = 1;
                resultData[0] = 0x01;
                logErrorP("LittleFS.begin() failed");
                return true;
            }

            if(checkOpenedDir(resultData, resultLength))
                return true;
            
            if(!_dir.next())
            {
                resultLength = 1;
                resultData[0] = 0x86;
                logErrorP("Dir has no more files");
                return true;
            }

            resultData[0] = 0x00;
            resultData[1] = _dir.isFile() ? 0x01 : 0x02;
            
            String fileName = _dir.fileName();
            int pathlength = fileName.length();
            const char* name = _dir.fileName().c_str();
            memcpy(resultData + 2, name, pathlength+1);
            logInfoP(_dir.fileName().c_str());
            resultLength = pathlength + 2;

            return true;
        }
        
        case FtpCommands::DirClose:
        {
            logInfoP("Dir close %s", data);
            if(!openFileSystem())
            {
                resultLength = 1;
                resultData[0] = 0x01;
                logErrorP("LittleFS.begin() failed");
                return true;
            }

            if(checkOpenedDir(resultData, resultLength))
                return true;
            
            //_dir = nullptr;
            _dirOpen = false;

            resultLength = 1;
            resultData[0] = 0x00;
            return true;
        }
        
        case FtpCommands::DirMake:
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
        
        case FtpCommands::DirRemove:
        {
            logInfoP("Dir remove %s", data);
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
        
    }
    return false;
}
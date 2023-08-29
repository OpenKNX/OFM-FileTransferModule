#include <Arduino.h>
#include <LittleFS.h>
#include "OpenKNX.h"
#include "FastCRC.h"

#define INFO_INTERVAL 10000

class FileTransferModule : public OpenKNX::Module
{
	public:
		const std::string name() override;
		const std::string version() override;
        const uint8_t _major = 0; // also update library.json
        const uint8_t _minor = 0;
        const uint8_t _build = 3;
		void loop(bool conf) override;

	private:
        unsigned long _heartbeat = 0;
        long _lastAccess = 0;
        File _file;
        Dir _dir;
        uint8_t _size = 0;
        bool _fileOpen = false;
        bool _dirOpen = false;
        bool _fsOpen = false;
        uint16_t _lastSequence = 0;
        bool openFileSystem();
        bool checkOpenedFile(uint8_t *resultData, uint8_t &resultLength);
        bool checkOpenedDir(uint8_t *resultData, uint8_t &resultLength);
        bool checkOpenFile(uint8_t *resultData, uint8_t &resultLength);
        bool checkOpenDir(uint8_t *resultData, uint8_t &resultLength);
		bool processFunctionProperty(uint8_t objectIndex, uint8_t propertyId, uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength) override;
		void FileRead(uint16_t sequence, uint8_t *resultData, uint8_t &resultLength);
		void FileWrite(uint16_t sequence, uint8_t *data, uint8_t length, uint8_t *resultData, uint8_t &resultLength);
        //bool processFunctionPropertyState(uint8_t objectIndex, uint8_t propertyId, uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength) override;
};

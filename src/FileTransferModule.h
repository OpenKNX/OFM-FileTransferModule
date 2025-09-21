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
    bool processFunctionProperty(uint8_t objectIndex, uint8_t propertyId, uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength) override;
    void readFile(uint16_t sequence, uint8_t *resultData, uint8_t &resultLength);
    void writeFile(uint16_t sequence, uint8_t *data, uint8_t length, uint8_t *resultData, uint8_t &resultLength);
    // bool processFunctionPropertyState(uint8_t objectIndex, uint8_t propertyId, uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength) override;

    bool checkOpenedFile(uint8_t *resultData, uint8_t &resultLength);
    bool checkOpenedDir(uint8_t *resultData, uint8_t &resultLength);
    bool checkOpenFile(uint8_t *resultData, uint8_t &resultLength);
    bool checkOpenDir(uint8_t *resultData, uint8_t &resultLength);
    void cmdModuleVersion(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength);
    void cmdFormat(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength);
    void cmdRename(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength);
    void cmdExists(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength);
#ifdef ARDUINO_ARCH_RP2040
    void cmdFwUpdate(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength);
#endif
    void cmdFileInfo(uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength);
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
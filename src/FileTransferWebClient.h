#pragma once
/**
 * @file        FileTransferWebClient.h
 * @brief       knxOTA -- the web page that drives the embedded client: firmware, delta or throughput
 *              to a target PA. Only a bridge; every transfer runs in FileTransferClient::loop().
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 */
#include "FileTransferConfig.h" // switches first -- every guard below depends on it

#ifdef OPENKNX_FTC_KNXOTA_WEB
    #include "OpenKNX/Network/Module.h" // the web server this page hangs off
    #include <string>
    #include <vector>

/**
 * @brief Serves /knxota and the handful of routes behind it.
 * @details The page never holds a request open: start() arms a job and answers at once, the browser
 *          polls /knxota/status. That is what keeps a 20-minute firmware transfer compatible with a
 *          web server that must stay responsive.
 */
class FileTransferWebClient
{
  public:
    void setup();

  private:
    void handlePage(OpenKNX::Network::WebRequest &req, OpenKNX::Network::WebResponse &res);
    void handleStatus(OpenKNX::Network::WebRequest &req, OpenKNX::Network::WebResponse &res);
    void handleFiles(OpenKNX::Network::WebRequest &req, OpenKNX::Network::WebResponse &res);
    void handleStart(OpenKNX::Network::WebRequest &req, OpenKNX::Network::WebResponse &res);
    void handleCancel(OpenKNX::Network::WebRequest &req, OpenKNX::Network::WebResponse &res);
    void handleTrigger(OpenKNX::Network::WebRequest &req, OpenKNX::Network::WebResponse &res);
    void handleAuth(OpenKNX::Network::WebRequest &req, OpenKNX::Network::WebResponse &res);
    void handleAction(OpenKNX::Network::WebRequest &req, OpenKNX::Network::WebResponse &res);
    void handleGa(OpenKNX::Network::WebRequest &req, OpenKNX::Network::WebResponse &res);
    void handleDrives(OpenKNX::Network::WebRequest &req, OpenKNX::Network::WebResponse &res);
    void handleProgMode(OpenKNX::Network::WebRequest &req, OpenKNX::Network::WebResponse &res);

    static uint16_t parsePa(const std::string &s); // "5.0.3" -> 0x5003, 0 = unparsable
    static std::string paText(uint16_t pa);        // 0x5003 -> "5.0.3"

    /**
     * @brief Keep the sweep's hits, because the client drops them when the sweep ends.
     * @details FileTransferClient releases its listing in ftcFinish(), so a poll after the last address
     *          would find nothing. Called on every status poll while the scan runs.
     */
    void collectScanHits();

    struct ScanHit
    {
        uint16_t pa = 0;
        uint16_t mask = 0;   // the sweep records it per hit -- reliable, unlike the late OpenKNX flag
        bool openKnx = false;
    };
    std::vector<ScanHit> _scanHits;      // what the running/last sweep found
    static constexpr size_t SCAN_HIT_MAX = 64;
    uint16_t _featPa = 0;                // PA the last "Gerät lesen" was asked for

    /**
     * @brief Pick up the answer of a finished FilesystemInfo, because only the page knows which drive it asked for.
     * @details The client keeps one fsInfo() slot; the drive is not part of it. Called on every status poll.
     */
    void collectDriveAnswer();

    struct DriveInfo
    {
        uint8_t state = 0;   // 0 not asked · 1 absent (target ignored the prefix) · 2 present · 3 empty slot
        uint32_t total = 0;  // in bytes for the internal FS, in KB for a provider -- `kb` says which
        uint32_t free = 0;
        bool kb = false;
    };
    DriveInfo _drv[3];                   // 0 internal · 1 sd/ · 2 efc/
    int8_t _dfPending = -1;              // drive index of the FilesystemInfo currently in flight
    uint16_t _drvPa = 0;                 // target the drive answers belong to
    bool _gaLost = false;                // a fast upload overwrites the group-address table's memory
    uint16_t _gaPa = 0;                  // target the group-address table belongs to
};

extern FileTransferWebClient openknxFtcWeb;
#endif

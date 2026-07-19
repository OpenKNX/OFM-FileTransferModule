#pragma once
/**
 * @file        FileTransferClientConsole.h
 * @brief       "ftc" console: command parsing, validation and the colored help
 * @version     0.0.1
 * @date        2026-07-16
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 */
#include "OpenKNX.h"

#ifdef OPENKNX_FTC

// The "ftc" console: parsing, validation and the colored help, split from the transfer logic in
// FileTransferClient (as DdcConsole/DeviceDisplay). Only ever calls the client's public request* API.
class FileTransferClient;

class FileTransferClientConsole
{
  public:
    explicit FileTransferClientConsole(FileTransferClient &client) : _client(client) {}

    // Returns true if the command was ours (handled or rejected), false to let other modules see it.
    bool processCommand(const std::string &cmd);
    // The full, colored `ftc ?` help.
    void showUsage();

  private:
    FileTransferClient &_client;
    void showStatus();                     // `ftc status`: reads the client's status() (progress %, B/s, result)
    void showScan(const std::string &cmd); // `ftc scan ...`: parse the range/gates, drive requestScan
};

#endif

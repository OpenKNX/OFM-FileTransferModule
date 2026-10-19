/**
 * @file        Worker.h
 * @brief       `oknx --_worker -i <ip> [--port N]`: one process per interface, driven by `oknx serve` over a pipe.
 * @details     The worker owns every KNXnet/IP connection to its interface: the management tunnel (the
 *              process-wide g_knxTunnel, the FTC client, the shim -- unchanged) in one thread, and each monitor
 *              in its own thread with its own socket. It ends when the pipe closes, and closes every connection
 *              with DISCONNECT_REQUEST before it does.
 * @date        2026-10-19
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once

namespace ftc
{
namespace serve
{

/** @brief Entry point for `oknx --_worker …`; returns the process exit code. */
int workerMain(int argc, char** argv);

} // namespace serve
} // namespace ftc

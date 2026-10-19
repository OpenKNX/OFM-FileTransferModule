/**
 * @file        Bridge.h
 * @brief       `oknx mcp`: MCP over stdio, passed on to the running `oknx serve`.
 * @date        2026-10-19
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once

namespace ftc
{
namespace serve
{

/** @brief Run the stdio bridge against serve on 127.0.0.1:@p port until stdin closes. */
int mcpBridgeMain(int port);

} // namespace serve
} // namespace ftc

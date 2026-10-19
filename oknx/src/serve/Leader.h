/**
 * @file        Leader.h
 * @brief       `oknx serve`: the leader process. HTTP/SSE API and MCP on 127.0.0.1/::1, one worker per interface.
 * @details     Subcommands: `oknx serve [--port N] [--rules F] [--detach]`, `serve stop`, `serve open [--open]`,
 *              `serve logout`, `serve rekey`, `serve lease rm <id>`, `serve status`.
 * @date        2026-10-19
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include <string>
#include <vector>

namespace ftc
{
namespace serve
{

static constexpr int SERVE_PORT_DEFAULT = 8671;

/** @brief Entry point for `oknx serve …`; @p args are the words after `serve`. Returns the exit code. */
int serveMain(const std::string& selfPath, const std::vector<std::string>& args, const std::string& lang,
              const std::string& version);

} // namespace serve
} // namespace ftc

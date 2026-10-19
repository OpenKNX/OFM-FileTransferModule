/**
 * @file        BenchCmd.h
 * @brief       `oknx bench lock|unlock|status`: the bench lock by hand.
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

/** @brief Entry point for `oknx bench …`; @p args are the words after `bench`. */
int benchMain(const std::vector<std::string>& args, int servePort);

} // namespace serve
} // namespace ftc

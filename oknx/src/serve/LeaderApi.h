/**
 * @file        LeaderApi.h
 * @brief       What compare, MCP and the pages may ask of the leader: run an operation through the one guarded
 *              path, read a shared monitor.
 * @date        2026-10-19
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include "serve/Http.h"
#include "serve/LeaderState.h"
#include "serve/Rules.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace ftc
{
namespace serve
{

using ProgressFn = std::function<void(const json&)>;

class LeaderApi
{
  public:
    virtual ~LeaderApi() = default;
    virtual const std::string& lang() const = 0;
    virtual const std::string& version() const = 0;
    virtual int port() const = 0;
    /**
     * @brief Run @p name through guard, allocation and execution; the envelope comes back.
     * @param progress  called for every progress event (MCP notifications); may be empty
     * @param cancel    the caller's cancel mark (MCP); empty = a fresh one
     * @param wait      false = return at once with the id
     */
    virtual json invoke(const std::string& name, const json& args, const Access& acc, ProgressFn progress,
                        std::shared_ptr<std::atomic<bool>> cancel, bool wait) = 0;
    /** @brief A reader on the shared monitor @p mode of an interface; nullptr with @p error filled. */
    virtual std::shared_ptr<Reader> openReader(const std::string& ip, uint16_t port, const std::string& mode, bool raw,
                                               const std::string& who, json& error) = 0;
    virtual void closeReader(const std::string& ip, uint16_t port, const std::string& mode, const std::shared_ptr<Reader>& r) = 0;
    virtual json opsList() = 0;
    virtual const Rules& rules() const = 0;
    virtual std::string benchConfigPath() const = 0;
};

} // namespace serve
} // namespace ftc

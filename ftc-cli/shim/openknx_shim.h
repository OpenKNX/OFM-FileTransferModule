// ┬────┴  OFM-FileTransferModule / ftc-cli
// ■ KNX   2026 OpenKNX - Erkan Çolak
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026, Erkan Çolak
//
// Host stand-in for the OpenKNX stack slice the four FTC files touch (shim contract §4, §5).
// Only the exact surface is provided: Module/Base bases, Log::Logger, Console, Facade.
#pragma once
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>

namespace OpenKNX
{
    /// @brief Minimal Base: the three Base-declared virtuals the client overrides.
    class Base
    {
      public:
        virtual ~Base() {}
        virtual const std::string name() { return std::string(); }
        virtual void setup(bool /*configured*/) {}
        virtual void loop(bool /*configured*/) {}
    };

    /// @brief Minimal Module: the three Module-declared virtuals the client overrides.
    class Module : public Base
    {
      public:
        virtual const std::string version() { return std::string(); }
        virtual bool processCommand(const std::string /*cmd*/, bool /*diagnoseKo*/) { return false; }
        virtual void showHelp() {}
    };

    namespace Log
    {
        /// @brief Host logger: the 5 methods the four files call. Variadics forward to vprintf.
        class Logger
        {
          public:
            // Prefixed printf-style line.
            void logWithPrefixAndValues(const char* prefix, const char* message, ...)
            {
                std::printf("[%s] ", prefix ? prefix : "");
                va_list args;
                va_start(args, message);
                std::vprintf(message, args);
                va_end(args);
                std::printf("\n");
            }
            // Prefixed plain line.
            void logWithPrefix(const char* prefix, const char* message)
            {
                std::printf("[%s] %s\n", prefix ? prefix : "", message ? message : "");
            }
            // Unprefixed printf-style line.
            void logWithValues(const char* message, ...)
            {
                va_list args;
                va_start(args, message);
                std::vprintf(message, args);
                va_end(args);
                std::printf("\n");
            }
            // Unprefixed plain line (both overloads present in the real Logger).
            void log(const char* message) { std::printf("%s\n", message ? message : ""); }
            void log(const std::string& message) { std::printf("%s\n", message.c_str()); }
            // Raw, un-timestamped device write (console-tunnel drain: OPENKNX_LOGGER_DEVICE.write).
            size_t write(const uint8_t* buf, size_t len)
            {
                if (buf == nullptr || len == 0) return 0;
                return std::fwrite(buf, 1, len, stdout);
            }
            // Logger mutex guard (no-op on host); logBegin()/logEnd() forward here.
            void begin() {}
            void end() { std::fflush(stdout); }
            // ANSI SGR color; 0 resets. Emits an escape or a no-op reset.
            void color(uint8_t color = 0)
            {
                if (color == 0)
                    std::printf("\033[0m");
                else
                    std::printf("\033[%um", (unsigned)color);
            }
        };
    } // namespace Log

    /// @brief Host console: line-sink (FTC console) + help-line printer.
    class Console
    {
      public:
#ifdef OPENKNX_FTC_CONSOLE
        // Redirect finished local lines to a sink (the remote console tunnel); nullptr restores local.
        void setLineSink(void (*s)(const char*)) { _lineSink = s; }
        // Host-input entry (ftc-cli main() calls this per finished stdin line): forward one line to the
        // installed sink (the client's consoleFeedLine). No-op when no session set a sink. This is the
        // console-INPUT counterpart to setLineSink; the shim contract only specified the output setter.
        void feedLine(const char* line)
        {
            if (_lineSink && line) _lineSink(line);
        }
#endif
        // One aligned help row for `<module> ?`.
        void printHelpLine(const char* command, const char* message)
        {
            std::printf("  %-20s %s\n", command ? command : "", message ? message : "");
        }

      private:
#ifdef OPENKNX_FTC_CONSOLE
        void (*_lineSink)(const char*) = nullptr;
#endif
    };

    /// @brief Host facade: exposes logger, console and the cooperative loop-budget gate.
    class Facade
    {
      public:
        Log::Logger logger;
        Console console;
        // Host has no loop budget -> always allow another drain step.
        bool freeLoopTime() { return true; }
    };
} // namespace OpenKNX

// Global facade instance (defined in shim.cpp).
extern OpenKNX::Facade openknx;

// Stack macros the console-tunnel path uses (real: OGM-Common Console.h / Log/Logger.h).
#ifndef CONSOLE_INPUT_SIZE
    #define CONSOLE_INPUT_SIZE 100 // max console input line (non-RTT default)
#endif
#ifndef OPENKNX_LOGGER_DEVICE
    #define OPENKNX_LOGGER_DEVICE openknx.logger // raw byte sink with .write(const uint8_t*, size_t)
#endif
#ifndef logBegin
    #define logBegin() openknx.logger.begin()
#endif
#ifndef logEnd
    #define logEnd() openknx.logger.end()
#endif

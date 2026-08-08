#pragma once
/**
 * @file        openknx_shim.h
 * @brief       Host stand-in for the OpenKNX stack slice the four FTC files touch (shim contract §4, §5).
 * @details     Provides only the exact surface: OpenKNX::Module/Base bases, Log::Logger (with a host-only
 *              per-line reformat hook), Console (line-sink + help printer) and Facade (`openknx` global).
 * @date        2026-07-25
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 */
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
        // Host-only reformat seam: a hook that receives each COMPLETE body line (+ the client's intended
        // color) so the CLI can re-render it (path a — the embedded client stays byte-identical). Returns
        // true if it handled/printed the line. Default (nullptr) = the legacy behaviour below, unchanged.
        using LineHook = bool (*)(const std::string& line, uint8_t color);

        /// @brief Host logger: the log/write methods the client calls. Each assembles a full line -> emit().
        class Logger
        {
          public:
            static void setLineHook(LineHook h) { s_hook = h; }

            // Prefixed printf-style line.
            void logWithPrefixAndValues(const char* prefix, const char* message, ...)
            {
                char buf[1024];
                va_list args;
                va_start(args, message);
                std::vsnprintf(buf, sizeof(buf), message ? message : "", args);
                va_end(args);
                std::string s = "[";
                s += (prefix ? prefix : "");
                s += "] ";
                s += buf;
                emit(s);
            }
            // Prefixed plain line.
            void logWithPrefix(const char* prefix, const char* message)
            {
                std::string s = "[";
                s += (prefix ? prefix : "");
                s += "] ";
                s += (message ? message : "");
                emit(s);
            }
            // Unprefixed printf-style line.
            void logWithValues(const char* message, ...)
            {
                char buf[1024];
                va_list args;
                va_start(args, message);
                std::vsnprintf(buf, sizeof(buf), message ? message : "", args);
                va_end(args);
                emit(buf);
            }
            // Unprefixed plain line (both overloads present in the real Logger).
            void log(const char* message) { emit(message ? message : ""); }
            void log(const std::string& message) { emit(message); }
            // Raw, un-timestamped device write (console-tunnel drain: OPENKNX_LOGGER_DEVICE.write). With a
            // hook installed it is assembled into complete lines and routed through the SAME per-line hook
            // (so a full-screen TUI can place each line into its scroll region instead of straight to
            // stdout). Without a hook it prints directly, exactly as before.
            size_t write(const uint8_t* buf, size_t len)
            {
                if (buf == nullptr || len == 0) return 0;
                if (!s_hook) return std::fwrite(buf, 1, len, stdout);
                for (size_t i = 0; i < len; ++i)
                {
                    const char ch = (char)buf[i];
                    if (ch == '\n') flushRaw();
                    else if (ch != '\r') { _rawLine += ch; if (_rawLine.size() > 1000) flushRaw(); }
                }
                return len;
            }
            // Logger mutex guard (no-op on host); logBegin()/logEnd() forward here.
            void begin() {}
            void end() { std::fflush(stdout); }
            // ANSI SGR color; 0 resets. With a hook installed, the color is remembered (passed to the hook)
            // instead of emitted; without a hook it prints immediately, exactly as before.
            void color(uint8_t color = 0)
            {
                if (s_hook) { _color = color; return; }
                if (color == 0)
                    std::printf("\033[0m");
                else
                    std::printf("\033[%um", (unsigned)color);
            }

          private:
            // Flush the accumulated raw-device line through the hook (color 0 = plain device text).
            void flushRaw()
            {
                if (s_hook) s_hook(_rawLine, 0);
                _rawLine.clear();
            }

            // Assemble-then-emit: give the hook first refusal; else fall back to the legacy line print.
            void emit(const std::string& line)
            {
                if (s_hook)
                {
                    if (s_hook(line, _color)) { _color = 0; return; }
                    if (_color) std::printf("\033[%um", (unsigned)_color);
                    std::fputs(line.c_str(), stdout);
                    if (_color) std::printf("\033[0m");
                    std::putchar('\n');
                    _color = 0;
                    return;
                }
                std::fputs(line.c_str(), stdout);
                std::putchar('\n');
            }

            uint8_t _color = 0;
            std::string _rawLine; // partial raw-device line accumulated across write() calls
            inline static LineHook s_hook = nullptr; // C++17 inline static
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

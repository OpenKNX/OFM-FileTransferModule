/**
 * @file        cli/Keys.h
 * @brief       Non-blocking single-keypress reader for the live views (monitor · compare · anywhere a
 *              hot-key belongs). RAII: raw mode on construction (only on a TTY), guaranteed restore on
 *              destruction. Ctrl-C is left to the process signal handler (ISIG stays on).
 * @copyright   Copyright (c) 2026, Erkan Çolak — GNU GPL v3.0
 */
#pragma once

#include <cstdio>
#ifdef _WIN32
    #include <conio.h>
    #include <io.h>
    #include <windows.h>
#else
    #include <sys/select.h>
    #include <termios.h>
    #include <unistd.h>
#endif

namespace ftc
{
/** @brief Raw-mode hot-key reader. Construct once around a live loop, poll() each tick. */
class Keys
{
  public:
    Keys()
    {
#ifdef _WIN32
        if (!_isatty(_fileno(stdin))) return;
        _h = GetStdHandle(STD_INPUT_HANDLE);
        GetConsoleMode(_h, &_saved);
        SetConsoleMode(_h, _saved & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT)); // keep PROCESSED_INPUT -> Ctrl-C still signals
#else
        if (!isatty(STDIN_FILENO)) return;
        tcgetattr(STDIN_FILENO, &_saved);
        struct termios raw = _saved;
        raw.c_lflag &= ~(ICANON | ECHO); // unbuffered, no echo; ISIG stays -> Ctrl-C -> SIGINT
        raw.c_iflag &= ~(IXON | IXOFF);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
#endif
        _raw = true;
    }
    ~Keys() { restore(); }
    Keys(const Keys&) = delete;
    Keys& operator=(const Keys&) = delete;

    bool active() const { return _raw; }

    /** @brief Next pending key as a lowercase char, or 0 if none. Non-blocking; arrow/escape sequences swallowed. */
    char poll()
    {
        if (!_raw) return 0;
#ifdef _WIN32
        if (!_kbhit()) return 0;
        int c = _getch();
        if (c == 0 || c == 0xE0)
        {
            _getch(); // discard the extended (arrow/function) second byte
            return 0;
        }
        return lower(c);
#else
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(STDIN_FILENO, &rf);
        struct timeval tv{0, 0};
        if (::select(STDIN_FILENO + 1, &rf, nullptr, nullptr, &tv) <= 0) return 0;
        unsigned char b;
        if (::read(STDIN_FILENO, &b, 1) != 1) return 0;
        if (b == 0x1b) // ESC: drain the rest of an arrow/function sequence so it never leaks as a letter
        {
            unsigned char d;
            while (::read(STDIN_FILENO, &d, 1) == 1) {}
            return 0;
        }
        return lower(b);
#endif
    }

    /** @brief Restore the saved terminal mode. Idempotent; also runs from the destructor. */
    void restore()
    {
        if (!_raw) return;
#ifdef _WIN32
        SetConsoleMode(_h, _saved);
#else
        tcsetattr(STDIN_FILENO, TCSANOW, &_saved);
#endif
        _raw = false;
    }

  private:
    static char lower(int c) { return (char)((c >= 'A' && c <= 'Z') ? c + 32 : c); }

    bool _raw = false;
#ifdef _WIN32
    HANDLE _h = nullptr;
    DWORD _saved = 0;
#else
    struct termios _saved{};
#endif
};
} // namespace ftc

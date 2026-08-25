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
/** @brief Navigation keys, numbered above any byte value so they cannot collide with a character. */
enum : int
{
    K_UP = 0x100,
    K_DOWN,
    K_LEFT,
    K_RIGHT,
    K_HOME,
    K_END,
    K_PGUP,
    K_PGDN,
    K_ENTER,
    K_ESC,
    K_BACK,
};

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

    /**
     * @brief One keypress, waited for -- arrows and the other navigation keys decoded, not swallowed.
     * @details poll() exists for live views, where a key that is not there must not block the view; it
     *          throws arrow sequences away for exactly that reason. A chooser is the opposite case: it
     *          has nothing to do until a key arrives, and the arrows are the whole point. Returns a
     *          plain character for ordinary keys, or one of the K_* codes.
     *
     *          A lone Escape and the start of an arrow sequence look identical for one byte, so after
     *          an Escape the next byte is waited for only briefly: present means a sequence, absent
     *          means the user pressed Escape.
     */
    int waitKey()
    {
        if (!_raw) return 0;
#ifdef _WIN32
        int c = _getch();
        if (c == 0 || c == 0xE0)
        {
            const int e = _getch();
            switch (e)
            {
                case 72: return K_UP;
                case 80: return K_DOWN;
                case 75: return K_LEFT;
                case 77: return K_RIGHT;
                case 71: return K_HOME;
                case 79: return K_END;
                case 73: return K_PGUP;
                case 81: return K_PGDN;
                default: return 0;
            }
        }
        if (c == '\r' || c == '\n') return K_ENTER;
        if (c == 27) return K_ESC;
        if (c == 8 || c == 127) return K_BACK;
        return c;
#else
        // Wait for the byte with select(), do not rely on read() blocking: raw mode here is set up for
        // poll() with VMIN=0/VTIME=0, where read() returns 0 at once when nothing is pending. Blocking
        // on read() therefore spins instead of waiting -- which looked exactly like keys being ignored.
        unsigned char b;
        {
            fd_set rf;
            FD_ZERO(&rf);
            FD_SET(STDIN_FILENO, &rf);
            if (::select(STDIN_FILENO + 1, &rf, nullptr, nullptr, nullptr) <= 0) return 0;
        }
        if (::read(STDIN_FILENO, &b, 1) != 1) return 0;
        if (b == '\r' || b == '\n') return K_ENTER;
        if (b == 8 || b == 127) return K_BACK;
        if (b != 0x1b) return (int)b;

        // Escape: a sequence follows immediately or not at all.
        if (!waitByte(b, 60)) return K_ESC;
        if (b != '[' && b != 'O') return K_ESC;
        if (!waitByte(b, 60)) return K_ESC;
        switch (b)
        {
            case 'A': return K_UP;
            case 'B': return K_DOWN;
            case 'C': return K_RIGHT;
            case 'D': return K_LEFT;
            case 'H': return K_HOME;
            case 'F': return K_END;
            default: break;
        }
        if (b >= '0' && b <= '9')
        {
            const unsigned char n = b;
            unsigned char t;
            while (waitByte(t, 60) && t != '~') {} // drain to the terminator
            if (n == '5') return K_PGUP;
            if (n == '6') return K_PGDN;
            if (n == '1' || n == '7') return K_HOME;
            if (n == '4' || n == '8') return K_END;
        }
        return 0;
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

#ifndef _WIN32
    /** @brief One byte within `ms`, or false. Distinguishes a bare Escape from an arrow sequence. */
    static bool waitByte(unsigned char& out, int ms)
    {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(STDIN_FILENO, &rf);
        struct timeval tv{0, ms * 1000};
        if (::select(STDIN_FILENO + 1, &rf, nullptr, nullptr, &tv) <= 0) return false;
        return ::read(STDIN_FILENO, &out, 1) == 1;
    }
#endif

    bool _raw = false;
#ifdef _WIN32
    HANDLE _h = nullptr;
    DWORD _saved = 0;
#else
    struct termios _saved{};
#endif
};
} // namespace ftc

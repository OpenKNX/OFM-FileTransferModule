/**
 * @file        Prompt.h
 * @brief       Terminal prompts: a yes/no question and a password that never appears on screen.
 * @details     The CLI had no masked input — the only prompt was a plain fgets on a cooked terminal, which
 *              is right for `[y/N]` and wrong for a password. Keys.h has the raw-mode plumbing but folds
 *              A-Z to lowercase, so it cannot carry one. This adds the two prompts knxOTA needs, built the
 *              same way Keys.h is (termios on POSIX, console API on Windows) so it works on all eight
 *              targets. The terminal is always restored, including on the abort path — a shell left without
 *              echo is worse than the failure that caused it.
 * @date        2026-08-11
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
    #include <conio.h>
    #include <io.h>
    #include <windows.h>
#else
    #include <termios.h>
    #include <unistd.h>
#endif

#include "I18n.h"
#include "Term.h"
#include "Theme.h"

namespace ftc
{

/** @brief Turns terminal echo off for as long as it lives, and back on however it is left. */
class NoEcho
{
  public:
    NoEcho()
    {
#ifndef _WIN32
        if (tcgetattr(STDIN_FILENO, &_saved) == 0)
        {
            _armed = true;
            struct termios raw = _saved;
            raw.c_lflag &= ~ECHO; // keep ICANON: the kernel still gives us line editing and backspace
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
        }
#endif
    }
    ~NoEcho() { restore(); }
    NoEcho(const NoEcho&) = delete;
    NoEcho& operator=(const NoEcho&) = delete;

    void restore()
    {
#ifndef _WIN32
        if (_armed)
        {
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &_saved);
            _armed = false;
        }
#endif
    }

  private:
#ifndef _WIN32
    struct termios _saved{};
    bool _armed = false;
#endif
};

/**
 * @brief Read a secret without echoing it; shows one dot per character so the user sees they are typing.
 * @details Returns false when the user aborted (Ctrl-C / EOF) or the input was empty. On a non-tty the line
 *          is read plainly, so a scripted run still works instead of hanging on a prompt nobody can see.
 */
inline bool readSecret(Term& t, Theme& c, const std::string& label, std::string& out, size_t maxLen = 16)
{
    out.clear();
    std::printf("  %s %s  ", c.amber("?").c_str(), c.dim(label).c_str());
    std::fflush(stdout);

    if (!t.isTty())
    {
        char buf[256];
        if (std::fgets(buf, sizeof(buf), stdin) == nullptr) return false;
        size_t n = std::strlen(buf);
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
            buf[--n] = '\0';
        out.assign(buf, n > maxLen ? maxLen : n);
        return !out.empty();
    }

#ifdef _WIN32
    for (;;)
    {
        const int ch = _getch();
        if (ch == 3) { std::printf("\n"); out.clear(); return false; }        // Ctrl-C
        if (ch == '\r' || ch == '\n') break;
        if (ch == '\b' || ch == 127)
        {
            if (!out.empty()) { out.pop_back(); std::printf("\b \b"); std::fflush(stdout); }
            continue;
        }
        if (ch == 0 || ch == 0xE0)                                            // arrow/function key: a
        {                                                                     // prefix followed by a scan
            _getch();                                                         // code — swallow both, or the
            continue;                                                         // code lands in the password
        }
        if (ch < 32 || ch > 126) continue;                                    // ignore other control keys
        if (out.size() >= maxLen) continue;
        out.push_back((char)ch);
        std::printf("%s", t.glyph("•", "*"));
        std::fflush(stdout);
    }
    std::printf("\n");
#else
    {
        NoEcho quiet; // ICANON stays on, so the kernel handles backspace for us
        char buf[256];
        if (std::fgets(buf, sizeof(buf), stdin) == nullptr)
        {
            quiet.restore();
            std::printf("\n");
            return false;
        }
        size_t n = std::strlen(buf);
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
            buf[--n] = '\0';
        out.assign(buf, n > maxLen ? maxLen : n);
    }
    // Echo was off, so nothing appeared: draw the dots ourselves for feedback.
    for (size_t i = 0; i < out.size(); ++i)
        std::printf("%s", t.glyph("•", "*"));
    std::printf("\n");
#endif
    return !out.empty();
}

/**
 * @brief A yes/no question that defaults to no; the accepted keys are appended automatically.
 * @details Accepts y and j in both languages -- muscle memory beats locale.
 * @details On a non-tty it never blocks: it returns `noTtyAnswer`, so a scripted run either proceeds because
 *          the caller passed --yes, or refuses — but it never waits for a key nobody will press.
 */
inline bool confirm(Term& t, Theme& c, I18n& L, const std::string& question, bool noTtyAnswer = false)
{
    // The accepted keys are part of the question. Appended here so no call site can forget them, and so
    // they read the same everywhere -- the capital letter is what a bare Enter does.
    std::printf("  %s %s %s ", c.amber("?").c_str(), c.bold(question).c_str(),
                c.dim(L.tr("[y/N]", "[j/N]")).c_str());
    std::fflush(stdout);
    if (!t.isTty())
    {
        std::printf("%s\n", noTtyAnswer ? "yes" : "no");
        return noTtyAnswer;
    }
    char buf[16];
    if (std::fgets(buf, sizeof(buf), stdin) == nullptr) return false;
    const char ch = (char)std::tolower((unsigned char)buf[0]);
    return ch == 'y' || ch == 'j';
}

} // namespace ftc

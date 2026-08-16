/**
 * @file        I18n.h
 * @brief       User-facing language selection (DE / EN) with an English fallback.
 * @details     English is the source of truth AND the fallback; German is a per-call translation. Only
 *              USER-FACING chrome is translated — protocol tokens, PAs, filenames, CRC and KNX terms stay
 *              verbatim. Detection order: --lang de|en > FTC_LANG > LC_ALL / LC_MESSAGES / LANG (de* -> de)
 *              > default en.
 * @date        2026-08-03
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(__APPLE__)
    #include <CoreFoundation/CoreFoundation.h>
#elif defined(_WIN32)
    #include <windows.h>
#endif

namespace ftc
{

class I18n
{
  public:
    enum class Lang
    {
        En,
        De
    };

    /**
     * @brief The language the operating system is set to, "" where the OS has no separate setting.
     * @details On macOS and Windows the desktop language is its own setting, and LC_* in a terminal is
     *          usually leftover tooling rather than a choice — a German Mac routinely reports
     *          AppleLanguages "de-DE" while the shell carries LC_ALL=en_US. On Linux LANG/LC_MESSAGES IS
     *          the OS setting, so there is nothing else to ask and this returns empty.
     */
    static std::string osLanguage()
    {
#if defined(__APPLE__)
        std::string out;
        CFArrayRef langs = CFLocaleCopyPreferredLanguages();
        if (langs != nullptr)
        {
            if (CFArrayGetCount(langs) > 0)
            {
                CFStringRef first = (CFStringRef)CFArrayGetValueAtIndex(langs, 0);
                char buf[32] = {0};
                if (first != nullptr && CFStringGetCString(first, buf, sizeof(buf), kCFStringEncodingUTF8))
                    out = buf;
            }
            CFRelease(langs);
        }
        return out;
#elif defined(_WIN32)
        wchar_t name[LOCALE_NAME_MAX_LENGTH] = {0};
        if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) > 0)
        {
            char buf[32] = {0};
            for (int i = 0; i < 8 && name[i]; ++i)
                buf[i] = (char)name[i];
            return buf;
        }
        return std::string();
#else
        return std::string();
#endif
    }

    /**
     * @brief Resolve the language: an explicit choice, then FTC_LANG, then the OS, then the POSIX variables.
     * @details FTC_LANG stays above the OS so a single shell can still be pinned; LC_* stays below it on
     *          macOS/Windows for the reason in osLanguage(), and remains the only source on Linux.
     */
    void detect(const char* explicitLang)
    {
        if (explicitLang && *explicitLang && std::strcmp(explicitLang, "auto") != 0)
        {
            _lang = looksGerman(explicitLang) ? Lang::De : Lang::En;
            return;
        }
        if (const char* pinned = std::getenv("FTC_LANG"); pinned != nullptr && *pinned != '\0')
        {
            _lang = looksGerman(pinned) ? Lang::De : Lang::En;
            return;
        }
        const std::string os = osLanguage();
        if (!os.empty())
        {
            _lang = looksGerman(os.c_str()) ? Lang::De : Lang::En;
            return;
        }
        const char* env = firstSet("LC_ALL", "LC_MESSAGES", "LANG", nullptr);
        _lang = (env && looksGerman(env)) ? Lang::De : Lang::En;
    }

    Lang lang() const { return _lang; }
    bool german() const { return _lang == Lang::De; }
    void setLang(Lang l) { _lang = l; }

    /**
     * @brief Pick the English or German variant of one string.
     */
    const char* tr(const char* en, const char* de) const { return _lang == Lang::De ? de : en; }
    std::string tr(const std::string& en, const std::string& de) const { return _lang == Lang::De ? de : en; }

  private:
    static bool looksGerman(const char* s) { return s && (s[0] == 'd' || s[0] == 'D') && (s[1] == 'e' || s[1] == 'E'); }
    static const char* firstSet(const char* a, const char* b, const char* c, const char* d)
    {
        for (const char* name : {a, b, c, d})
        {
            if (name == nullptr) continue;
            const char* v = std::getenv(name);
            if (v && *v) return v;
        }
        return nullptr;
    }

    Lang _lang = Lang::En;
};

} // namespace ftc

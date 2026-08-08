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
     * @brief Resolve the language from an explicit override ("de"/"en"/"auto"/nullptr) then the environment.
     */
    void detect(const char* explicitLang)
    {
        if (explicitLang && *explicitLang && std::strcmp(explicitLang, "auto") != 0)
        {
            _lang = looksGerman(explicitLang) ? Lang::De : Lang::En;
            return;
        }
        const char* env = firstSet("FTC_LANG", "LC_ALL", "LC_MESSAGES", "LANG");
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
            const char* v = std::getenv(name);
            if (v && *v) return v;
        }
        return nullptr;
    }

    Lang _lang = Lang::En;
};

} // namespace ftc

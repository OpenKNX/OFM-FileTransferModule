/**
 * @file        FtcXferOptions.h
 * @brief       ftc token walker + transfer-option table (host and device share one grammar)
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 */
#pragma once
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

/**
 * @brief Next whitespace-delimited token into `out`. False at end of line; `trunc` set if it did not fit.
 * @details Replaces the fixed-width sscanf fields the transfer commands used: those silently cut a token
 *          at 23 characters, so a remote path longer than that wrote to the wrong name.
 */
inline bool ftcTok(const char *&p, char *out, size_t cap, bool &trunc)
{
    while (*p == ' ' || *p == '\t') p++;
    if (*p == 0) return false;
    size_t n = 0;
    while (*p && *p != ' ' && *p != '\t')
    {
        if (n + 1 < cap) out[n++] = *p;
        else trunc = true;
        p++;
    }
    out[n] = 0;
    return true;
}

inline bool ftcFlagToken(const char *t, uint8_t &verbosity, bool &fast, bool &apply, bool &keep, bool &noResume);

/** @brief Transfer options; `seen` flags drive the requires check. Used by send/upload; perf + get still parse their own. */
struct FtcXferOpts
{
    unsigned pkg = 0;      // 0 = auto
    unsigned window = 0;   // 0 = adaptive AIMD
    uint8_t mode = 0;      // 0 safe, 1 fast
    uint8_t verbosity = 0xFF; // 0xFF = leave as configured
    bool apply = false, noResume = false, keep = false;
    bool modeSeen = false, windowSeen = false;
};

/**
 * @brief Apply one option token. Returns false if the token is not an option (caller treats it as operand).
 * @details Accepts three spellings of the same thing: long `--mode fast` / `--mode=fast`, bundled short
 *          `-fa`, and the legacy bare words (`fast`, `nr`, `apply`, `w16`, a plain number). `err` is set
 *          when the token IS an option but its value is wrong -- so a typo is reported, never ignored.
 *          `next` is the following token (may be nullptr) and is consumed via `usedNext` for `--opt value`.
 */
inline bool ftcXferOpt(const char *t, const char *next, FtcXferOpts &o, bool &usedNext, const char *&err)
{
    usedNext = false;
    err = nullptr;
    auto optVal = [&](const char *inl) -> const char * {
        if (inl) return inl;
        if (next && next[0]) { usedNext = true; return next; }
        return nullptr;
    };
    // NOT `isNum`: OGM-Common Helper.h defines that as a macro, which would rewrite every call site.
    auto allDigits = [](const char *s) {
        if (!s || !*s) return false;
        for (const char *q = s; *q; q++)
            if (!isdigit((unsigned char)*q)) return false;
        return true;
    };

    if (t[0] == '-' && t[1] == '-')
    {
        const char *name = t + 2;
        const char *inl = strchr(name, '=');
        char key[16] = {0};
        const size_t kl = inl ? (size_t)(inl - name) : strlen(name);
        if (kl >= sizeof(key)) { err = "unknown option"; return true; }
        memcpy(key, name, kl);
        if (inl) inl++;

        if (strcmp(key, "mode") == 0)
        {
            const char *v = optVal(inl);
            if (!v) { err = "needs a value (safe|fast)"; return true; }
            if (strcmp(v, "safe") == 0) o.mode = 0;
            else if (strcmp(v, "fast") == 0 || strcmp(v, "win") == 0 || strcmp(v, "windowed") == 0) o.mode = 1;
            else { err = "expected safe|fast"; return true; }
            o.modeSeen = true;
            return true;
        }
        if (strcmp(key, "pkg") == 0)
        {
            const char *v = optVal(inl);
            if (!v) { err = "needs a value (16..254 or auto)"; return true; }
            if (strcmp(v, "auto") == 0) { o.pkg = 0; return true; }
            if (!allDigits(v)) { err = "expected a number or auto"; return true; }
            o.pkg = (unsigned)atoi(v);
            return true;
        }
        if (strcmp(key, "window") == 0)
        {
            const char *v = optVal(inl);
            if (!v || !allDigits(v)) { err = "needs a number"; return true; }
            o.window = (unsigned)atoi(v);
            o.windowSeen = true;
            return true;
        }
        if (strcmp(key, "apply") == 0) { o.apply = true; return true; }
        if (strcmp(key, "no-apply") == 0) { o.apply = false; return true; }
        if (strcmp(key, "no-resume") == 0) { o.noResume = true; return true; }
        if (strcmp(key, "keep") == 0) { o.keep = true; return true; }
        if (strcmp(key, "progress") == 0) { o.verbosity = 2; return true; }
        if (strcmp(key, "quiet") == 0) { o.verbosity = 0; return true; }
        err = "unknown option";
        return true;
    }

    if (t[0] == '-' && t[1])
    {
        uint8_t vb = (o.verbosity == 0xFF) ? 1 : o.verbosity;
        bool f = o.mode != 0;
        if (!ftcFlagToken(t, vb, f, o.apply, o.keep, o.noResume)) { err = "unknown flag letter"; return true; }
        if (f) { o.mode = 1; o.modeSeen = true; }
        o.verbosity = vb;
        return true;
    }

    // --- legacy bare words: still accepted, they are what the device console has always taken ---
    if (allDigits(t)) { o.pkg = (unsigned)atoi(t); return true; }
    if (strcmp(t, "auto") == 0) { o.pkg = 0; return true; }
    if (strcmp(t, "safe") == 0) { o.mode = 0; o.modeSeen = true; return true; }
    if (strcmp(t, "fast") == 0 || strcmp(t, "win") == 0 || strcmp(t, "windowed") == 0)
        { o.mode = 1; o.modeSeen = true; return true; }
    if (strcmp(t, "apply") == 0 || strcmp(t, "on") == 0 || strcmp(t, "yes") == 0) { o.apply = true; return true; }
    if (strcmp(t, "no") == 0 || strcmp(t, "off") == 0 || strcmp(t, "noapply") == 0) { o.apply = false; return true; }
    if (strcmp(t, "no-resume") == 0 || strcmp(t, "noresume") == 0 || strcmp(t, "nr") == 0 || strcmp(t, "fresh") == 0)
        { o.noResume = true; return true; }
    if (strcmp(t, "keep") == 0) { o.keep = true; return true; }
    if (strcmp(t, "verbose") == 0 || strcmp(t, "v") == 0) { o.verbosity = 2; return true; }
    if ((t[0] == 'w' || t[0] == 'W') && allDigits(t + 1)) { o.window = (unsigned)atoi(t + 1); o.windowSeen = true; return true; }
    return false; // not an option -> operand
}

/** @brief Cross-option rules. Returns the message to report, or nullptr when the set is consistent. */
inline const char *ftcXferCheck(const FtcXferOpts &o)
{
    if (o.windowSeen && o.mode == 0) return "window needs fast mode (--mode fast / -f)";
    if (o.pkg != 0 && (o.pkg < 16 || o.pkg > 254)) return "pkg out of range (16..254)";
    if (o.windowSeen && (o.window < 4 || o.window > 64)) return "window out of range (4..64)";
    return nullptr;
}

/**
 * @brief Consume one bundled option token: `-fva`, `-v2`, `-q`. Returns false if it is not a flag token.
 * @details Only valueless flags bundle, the same rule the host CLI follows. `w<N>` stays a word of its own
 *          because it carries a value. An unknown letter makes the whole token fall through, so a typo is
 *          reported rather than half-applied.
 */
inline bool ftcFlagToken(const char *t, uint8_t &verbosity, bool &fast, bool &apply, bool &keep, bool &noResume)
{
    if (t[0] != '-' || t[1] == 0) return false;
    uint8_t vb = verbosity;
    bool f = fast, a = apply, k = keep, n = noResume;
    for (const char *c = t + 1; *c; ++c)
    {
        switch (*c)
        {
            case 'f': f = true; break;
            case 'a': a = true; break;
            case 'k': k = true; break;
            case 'n': n = true; break;
            case 'q': vb = 0; break;
            case 'v':
                if (isdigit((unsigned char)c[1])) { vb = (uint8_t)(c[1] - '0'); ++c; }
                else vb = 2;
                break;
            default: return false; // not ours -- let the caller report it
        }
    }
    verbosity = vb > 2 ? 2 : vb;
    fast = f; apply = a; keep = k; noResume = n;
    return true;
}


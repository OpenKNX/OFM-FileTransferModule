/**
 * @file        SelfInstall.h
 * @brief       Self-install / self-uninstall of the ftc binary — zero external dependency (no pwsh, sh or
 *              python), identical on macOS, Linux (incl. Raspberry Pi arm64/armhf) and Windows. CLI-agnostic CORE.
 * @details     The running binary copies itself into a bin directory. Default target is the per-user, sudo-free
 *              location (~/.local/bin, %LOCALAPPDATA%\Programs\ftc); --system targets /usr/local/bin (needs root)
 *              resp. %ProgramFiles%\ftc. Never edits shell rc files or the registry — it only prints a PATH hint.
 *              Uninstall removes only a regular file named ftc/ftc.exe at the target directory.
 * @date        2026-08-10
 * @copyright   Copyright (c) 2024-2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <cstdint>
#include <mach-o/dyld.h>
#endif

namespace ftc
{
namespace fs = std::filesystem;

/** @brief Outcome of an install/uninstall: success flag, the affected path, and an optional human hint. */
struct InstallResult
{
    bool ok = false;
    fs::path dest;    // where the binary was placed / removed
    std::string note; // PATH hint or error reason (may be empty)
};

/** @brief Absolute path of the currently running executable (symlinks resolved where possible). */
inline fs::path selfExePath()
{
#ifdef _WIN32
    wchar_t buf[32768]; // max \\?\ path length in wide chars
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    if (n == 0 || n >= (sizeof(buf) / sizeof(buf[0]))) return {};
    return fs::path(std::wstring(buf, n));
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size); // first call: query required length (incl. NUL)
    std::string buf(size ? size : 1, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    std::error_code ec;
    fs::path p = fs::weakly_canonical(fs::path(buf.c_str()), ec);
    return ec ? fs::path(buf.c_str()) : p;
#else // Linux / other POSIX
    std::error_code ec;
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path() : p;
#endif
}

/** @brief The install file name (ftc / ftc.exe). */
inline std::string binaryName()
{
#ifdef _WIN32
    return "ftc.exe";
#else
    return "ftc";
#endif
}

/** @brief Read an environment variable as a path (empty when unset/blank). */
inline fs::path envPath(const char* var)
{
    const char* v = std::getenv(var);
    return (v && *v) ? fs::path(v) : fs::path();
}

/** @brief Resolve the target install directory (per-user default, --system, or an explicit override). */
inline fs::path installDir(bool system, const std::string& dirOverride)
{
    if (!dirOverride.empty()) return fs::path(dirOverride);
#ifdef _WIN32
    if (system)
    {
        fs::path pf = envPath("ProgramFiles");
        return (pf.empty() ? fs::path("C:\\Program Files") : pf) / "ftc";
    }
    fs::path base = envPath("LOCALAPPDATA");
    if (base.empty()) base = envPath("USERPROFILE"); // fallback for stripped environments
    return (base.empty() ? fs::path(".") : base) / "Programs" / "ftc";
#else
    if (system) return fs::path("/usr/local/bin");
    fs::path home = envPath("HOME");
    return (home.empty() ? fs::path(".") : home) / ".local" / "bin";
#endif
}

/** @brief Is @p dir present on the PATH env? Used only for a hint; PATH is never modified. */
inline bool onPath(const fs::path& dir)
{
    const char* p = std::getenv("PATH");
    if (!p) return false;
#ifdef _WIN32
    const char sep = ';';
#else
    const char sep = ':';
#endif
    std::error_code ec;
    fs::path want = fs::weakly_canonical(dir, ec);
    if (ec) want = dir;
    const std::string path(p);
    size_t start = 0;
    while (start <= path.size())
    {
        size_t end = path.find(sep, start);
        if (end == std::string::npos) end = path.size();
        const std::string seg = path.substr(start, end - start);
        if (!seg.empty())
        {
            fs::path have = fs::weakly_canonical(fs::path(seg), ec);
            if (ec) have = fs::path(seg);
            if (have == want) return true;
        }
        start = end + 1;
    }
    return false;
}

/** @brief A shell-appropriate PATH hint for @p dir. Informational only — never applied automatically. */
inline std::string pathHint(const fs::path& dir)
{
#ifdef _WIN32
    return "setx PATH \"%PATH%;" + dir.string() + "\"   (then open a new terminal)";
#else
    return "export PATH=\"" + dir.string() + ":$PATH\"   (add to ~/.profile, ~/.bashrc or ~/.zshrc)";
#endif
}

/** @brief True if both files exist and are byte-identical (size first, then content). */
inline bool filesEqual(const fs::path& a, const fs::path& b)
{
    std::error_code ec;
    if (!fs::exists(a, ec) || !fs::exists(b, ec)) return false;
    const auto sa = fs::file_size(a, ec);
    if (ec) return false;
    const auto sb = fs::file_size(b, ec);
    if (ec || sa != sb) return false;
    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    if (!fa || !fb) return false;
    constexpr size_t N = 64 * 1024;
    std::vector<char> ba(N), bb(N);
    for (;;)
    {
        fa.read(ba.data(), (std::streamsize)N);
        fb.read(bb.data(), (std::streamsize)N);
        const std::streamsize na = fa.gcount(), nb = fb.gcount();
        if (na != nb) return false;
        if (na == 0) break;
        if (std::memcmp(ba.data(), bb.data(), (size_t)na) != 0) return false;
    }
    return true;
}

/** @brief Parse the first "MAJOR.MINOR.PATCH" in @p s into @p v[3]. Returns false when none is found. */
inline bool parseSemver(const std::string& s, int v[3])
{
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (!std::isdigit((unsigned char)s[i])) continue;
        int comp[3] = {0, 0, 0}, ci = 0;
        size_t j = i;
        bool ok = true;
        while (ci < 3)
        {
            if (j >= s.size() || !std::isdigit((unsigned char)s[j])) { ok = false; break; }
            long val = 0;
            while (j < s.size() && std::isdigit((unsigned char)s[j]) && val <= 100000) { val = val * 10 + (s[j] - '0'); ++j; }
            comp[ci++] = (int)val;
            if (ci < 3)
            {
                if (j < s.size() && s[j] == '.') ++j;
                else { ok = false; break; }
            }
        }
        if (ok && ci == 3) { v[0] = comp[0]; v[1] = comp[1]; v[2] = comp[2]; return true; }
    }
    return false;
}

/** @brief Compare two semver strings: +1 a>b, -1 a<b, 0 equal. @p ok=false if either is unparseable. */
inline int semverCmp(const std::string& a, const std::string& b, bool& ok)
{
    int va[3], vb[3];
    ok = parseSemver(a, va) && parseSemver(b, vb);
    if (!ok) return 0;
    for (int k = 0; k < 3; ++k)
        if (va[k] != vb[k]) return va[k] < vb[k] ? -1 : 1;
    return 0;
}

/** @brief Best-effort raw output of "<bin> --version" (path safely quoted; empty on any failure). */
inline std::string readBinaryVersionRaw(const fs::path& bin)
{
#ifdef _WIN32
    const std::string cmd = "\"" + bin.string() + "\" --version 2>NUL"; // Windows paths cannot contain '"'
    FILE* p = _popen(cmd.c_str(), "r");
#else
    std::string safe = "'"; // single-quote + escape embedded quotes -> neutralises every shell metacharacter
    for (char c : bin.string()) { if (c == '\'') safe += "'\\''"; else safe += c; }
    safe += "'";
    const std::string cmd = safe + " --version 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
#endif
    if (!p) return {};
    std::string out;
    char buf[256];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
#ifdef _WIN32
    _pclose(p);
#else
    pclose(p);
#endif
    return out;
}

/** @brief What an install would do, resolved before touching anything (so the caller can prompt/skip). */
struct InstallPlan
{
    fs::path self, dir, dest;
    std::string selfVersion, oldVersion; // oldVersion set only when a version could be read
    bool destExists = false;             // a regular ftc file already sits at dest
    bool selfIsDest = false;             // running the very binary we would install
    bool oldKnown = false;               // oldVersion parsed OK -> cmp is meaningful
    bool identical = false;              // dest is byte-identical to what we would install
    int cmp = 0;                         // valid iff oldKnown: +1 self newer, 0 equal, -1 self older
    std::string error;                   // non-empty -> cannot proceed
};

/** @brief Resolve the install plan: target, whether a copy already exists, and how versions compare. */
inline InstallPlan planInstall(bool system, const std::string& dirOverride, const std::string& selfVersion)
{
    InstallPlan p;
    p.selfVersion = selfVersion;
    p.self = selfExePath();
    if (p.self.empty()) { p.error = "could not determine the running executable path"; return p; }
    p.dir = installDir(system, dirOverride);
    p.dest = p.dir / binaryName();

    std::error_code ec;
    p.destExists = fs::exists(p.dest, ec) && fs::is_regular_file(p.dest, ec);
    const fs::path selfCanon = fs::weakly_canonical(p.self, ec);
    const fs::path destCanon = fs::weakly_canonical(p.dest, ec);
    p.selfIsDest = (!selfCanon.empty() && selfCanon == destCanon);

    if (p.destExists && !p.selfIsDest)
    {
        p.identical = filesEqual(p.self, p.dest);
        int tmp[3];
        if (parseSemver(readBinaryVersionRaw(p.dest), tmp))
        {
            char vb[32];
            std::snprintf(vb, sizeof(vb), "%d.%d.%d", tmp[0], tmp[1], tmp[2]);
            p.oldVersion = vb;
            bool ok = false;
            p.cmp = semverCmp(p.selfVersion, p.oldVersion, ok);
            p.oldKnown = ok;
        }
    }
    return p;
}

/** @brief Perform the copy resolved by @p plan (create dir, replace, chmod). Call after any confirm. */
inline InstallResult commitInstall(const InstallPlan& plan)
{
    InstallResult r;
    r.dest = plan.dest;
    std::error_code ec;
    fs::create_directories(plan.dir, ec);
    if (ec) { r.note = "cannot create '" + plan.dir.string() + "': " + ec.message(); return r; }

    if (plan.selfIsDest) // running the installed copy itself -> nothing to copy
    {
        r.ok = true;
        if (!onPath(plan.dir)) r.note = "directory is not on PATH";
        return r;
    }

    // Install via a temp file in the SAME directory + an atomic rename, so the destination gets a FRESH inode.
    // This is essential on macOS/Apple Silicon: overwriting a code-signed binary IN PLACE (same inode) leaves
    // the kernel's cached signature stale and it SIGKILLs the reinstalled binary on exec (exit 137) even though
    // `codesign -v` still passes on disk. A fresh inode via rename validates cleanly. Also atomic (never a
    // half-written binary) and it frees a running target on unix.
    fs::path tmp = plan.dest;
    tmp += ".new";
    fs::remove(tmp, ec);
    ec.clear();
    fs::copy_file(plan.self, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec)
    {
        r.note = "copy to '" + tmp.string() + "' failed: " + ec.message();
#ifndef _WIN32
        r.note += " (insufficient permissions? for a system dir use: sudo ftc install --system)";
#endif
        return r;
    }
#ifndef _WIN32
    ::chmod(tmp.string().c_str(), 0755); // rwxr-xr-x so it is directly runnable
#endif
#ifdef _WIN32
    // A running target .exe is locked -> rename it aside first so the rename-into-place succeeds (cleaned up later).
    if (fs::exists(plan.dest, ec))
    {
        fs::path bak = plan.dest;
        bak += ".old";
        fs::remove(bak, ec);
        fs::rename(plan.dest, bak, ec);
    }
#endif
    ec.clear();
    fs::rename(tmp, plan.dest, ec); // atomic replace (tmp is in the same dir/filesystem) -> fresh inode
    if (ec)
    {
        fs::remove(tmp, ec);
        r.note = "install (rename into place) failed: " + ec.message();
        return r;
    }
    r.ok = true;
    if (!onPath(plan.dir)) r.note = "directory is not on PATH";
    return r;
}

/** @brief Remove an installed ftc binary (only a regular file named ftc/ftc.exe) from the target dir. */
inline InstallResult uninstall(bool system, const std::string& dirOverride)
{
    InstallResult r;
    const fs::path dir = installDir(system, dirOverride);
    const fs::path dest = dir / binaryName();

    std::error_code ec;
    if (!fs::exists(dest, ec))
    {
        r.note = "not installed at '" + dest.string() + "'";
        return r;
    }
    if (!fs::is_regular_file(dest, ec))
    {
        r.note = "'" + dest.string() + "' is not a regular file -- refusing to remove";
        return r;
    }
    if (!fs::remove(dest, ec))
    {
        r.note = "remove of '" + dest.string() + "' failed: " + ec.message();
#ifndef _WIN32
        if (system) r.note += " (try: sudo ftc uninstall --system)";
#endif
        return r;
    }
    r.ok = true;
    r.dest = dest;
    return r;
}

} // namespace ftc

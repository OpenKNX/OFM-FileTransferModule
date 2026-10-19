/**
 * @file        Key.h
 * @brief       The program key of `oknx serve`: 32 random bytes as hex, in a file only the owner can read.
 * @details     POSIX: ~/.config/oknx/serve.key, created 0600 under a temporary name and renamed into place.
 *              Windows: %APPDATA%\oknx\serve.key, created with an access list that grants the current user
 *              alone. Every request except the plain health check carries it in X-Oknx-Key.
 * @date        2026-10-19
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include "serve/Paths.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>

#ifdef _WIN32
    #include <winsock2.h>
    #include <windows.h>
    #include <bcrypt.h>
    #include <sddl.h>
#else
    #include <fcntl.h>
    #include <sys/stat.h>
    #include <unistd.h>
#endif

namespace ftc
{
namespace serve
{

/** @brief Fill @p n bytes from the operating system's generator; false when it is not available. */
inline bool osRandom(uint8_t* out, size_t n)
{
#ifdef _WIN32
    return BCryptGenRandom(nullptr, out, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    size_t got = 0;
    while (got < n)
    {
        const ssize_t r = ::read(fd, out + got, n - got);
        if (r <= 0)
        {
            ::close(fd);
            return false;
        }
        got += (size_t)r;
    }
    ::close(fd);
    return true;
#endif
}

/** @brief A random token of @p bytes bytes as lower-case hex (keys, session ids, one-time links). */
inline std::string randomHex(size_t bytes)
{
    std::string out;
    uint8_t b[64];
    if (bytes > sizeof(b) || !osRandom(b, bytes)) return out;
    static const char* H = "0123456789abcdef";
    for (size_t i = 0; i < bytes; ++i)
    {
        out += H[b[i] >> 4];
        out += H[b[i] & 15];
    }
    return out;
}

/** @brief Compare in time independent of where the first difference is. */
inline bool sameSecret(const std::string& a, const std::string& b)
{
    if (a.size() != b.size() || a.empty()) return false;
    unsigned char d = 0;
    for (size_t i = 0; i < a.size(); ++i) d |= (unsigned char)(a[i] ^ b[i]);
    return d == 0;
}

/** @brief Write @p body to @p path readable only by the owner; replaces an existing file. */
inline bool writeOwnerOnly(const std::string& path, const std::string& body, std::string& err)
{
    std::string mk;
    const size_t cut = path.find_last_of("/\\");
    if (cut != std::string::npos) makeDirs(path.substr(0, cut), mk);
#ifdef _WIN32
    const std::string tmp = path + ".tmp." + std::to_string((unsigned long)GetCurrentProcessId());
#else
    const std::string tmp = path + ".tmp." + std::to_string((long)::getpid()); // two serves on one folder: one file each
#endif
#ifdef _WIN32
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
    {
        err = "cannot read the process token";
        return false;
    }
    DWORD len = 0;
    GetTokenInformation(tok, TokenUser, nullptr, 0, &len);
    std::string tu(len, '\0');
    LPSTR sidStr = nullptr;
    const bool okSid = GetTokenInformation(tok, TokenUser, &tu[0], len, &len) &&
                       ConvertSidToStringSidA(((TOKEN_USER*)&tu[0])->User.Sid, &sidStr);
    CloseHandle(tok);
    if (!okSid)
    {
        err = "cannot read the user SID";
        return false;
    }
    const std::string sddl = std::string("D:P(A;;FA;;;") + sidStr + ")"; // protected DACL: this user, full access
    LocalFree(sidStr);
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr))
    {
        err = "cannot build the access list";
        return false;
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    DeleteFileA(tmp.c_str());
    HANDLE h = CreateFileA(tmp.c_str(), GENERIC_WRITE, 0, &sa, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    LocalFree(sd);
    if (h == INVALID_HANDLE_VALUE)
    {
        err = "cannot create " + tmp;
        return false;
    }
    DWORD w = 0;
    const bool okW = WriteFile(h, body.data(), (DWORD)body.size(), &w, nullptr) && w == body.size();
    CloseHandle(h);
    if (!okW || !MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        DeleteFileA(tmp.c_str());
        err = "cannot write " + path;
        return false;
    }
    return true;
#else
    ::unlink(tmp.c_str());
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
    {
        err = "cannot create " + tmp;
        return false;
    }
    ::fchmod(fd, 0600); // whatever the umask said
    size_t off = 0;
    while (off < body.size())
    {
        const ssize_t w = ::write(fd, body.data() + off, body.size() - off);
        if (w <= 0)
        {
            ::close(fd);
            ::unlink(tmp.c_str());
            err = "cannot write " + tmp;
            return false;
        }
        off += (size_t)w;
    }
    ::fsync(fd);
    ::close(fd);
    if (::rename(tmp.c_str(), path.c_str()) != 0)
    {
        ::unlink(tmp.c_str());
        err = "cannot replace " + path;
        return false;
    }
    return true;
#endif
}

/**
 * @brief Read the key; with @p create a missing key is made. POSIX also refuses a key file others can read.
 * @return the key, or empty with @p err set.
 */
inline std::string loadKey(bool create, std::string& err)
{
    const std::string p = keyPath();
#ifndef _WIN32
    struct stat st{};
    if (::stat(p.c_str(), &st) == 0)
    {
        if (st.st_uid != ::getuid() || (st.st_mode & 077) != 0)
        {
            err = p + " must belong to you and be readable by you alone (chmod 600)";
            return std::string();
        }
    }
#endif
    std::ifstream f(p, std::ios::binary);
    std::string k;
    if (f) std::getline(f, k);
    while (!k.empty() && (k.back() == '\r' || k.back() == '\n' || k.back() == ' ')) k.pop_back();
    if (k.size() == 64 && k.find_first_not_of("0123456789abcdef") == std::string::npos) return k;
    if (!create)
    {
        err = f ? p + " does not hold a key; `oknx serve rekey` makes one" : "no key yet: start `oknx serve` once";
        return std::string();
    }
    k = randomHex(32);
    if (k.empty())
    {
        err = "the operating system gave no random bytes";
        return std::string();
    }
    if (!writeOwnerOnly(p, k + "\n", err)) return std::string();
    return k;
}

/** @brief Replace the key with a new one (`oknx serve rekey`). */
inline std::string newKey(std::string& err)
{
    const std::string k = randomHex(32);
    if (k.empty())
    {
        err = "the operating system gave no random bytes";
        return std::string();
    }
    if (!writeOwnerOnly(keyPath(), k + "\n", err)) return std::string();
    return k;
}

} // namespace serve
} // namespace ftc

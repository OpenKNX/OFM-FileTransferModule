/**
 * @file        Pipe.h
 * @brief       The pipe between `oknx serve` and one worker: start a worker with exactly one pipe pair, and
 *              move JSON lines over it.
 * @details     The worker gets the pair as its stdin (jobs) and stdout (events) and nothing else: POSIX closes
 *              every other descriptor in the child before exec, Windows hands CreateProcess a handle list that
 *              holds only the pair and the log file. The worker moves the pair away from 0/1 at its start, so
 *              nothing it prints can reach the protocol.
 * @date        2026-10-19
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
    #include <winsock2.h> // before windows.h, else it pulls in the legacy winsock.h
    #include <windows.h>
    #include <fcntl.h>
    #include <io.h>
#else
    #include <cerrno>
    #include <csignal>
    #include <fcntl.h>
    #include <sys/resource.h>
    #include <sys/wait.h>
    #include <unistd.h>
    #include <climits>
    #include <cstdlib>
    #if defined(__linux__)
        #include <sys/syscall.h>
    #endif
    #if defined(__APPLE__)
        #include <mach-o/dyld.h>
    #endif
#endif

namespace ftc
{
namespace serve
{

#ifdef _WIN32
using PipeEnd = HANDLE;
static const PipeEnd PIPE_NONE = INVALID_HANDLE_VALUE;
#else
using PipeEnd = int;
static const PipeEnd PIPE_NONE = -1;
#endif

/** @brief The path of the running program, for starting workers (argv[0] may be a bare name from PATH). */
inline std::string selfExePath(const std::string& argv0)
{
#ifdef _WIN32
    char b[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameA(nullptr, b, sizeof(b));
    if (n > 0 && n < sizeof(b)) return std::string(b, n);
#elif defined(__APPLE__)
    char b[4096];
    uint32_t sz = sizeof(b);
    if (_NSGetExecutablePath(b, &sz) == 0)
    {
        char real[4096];
        if (::realpath(b, real)) return real;
        return b;
    }
#else
    char b[4096];
    const ssize_t n = ::readlink("/proc/self/exe", b, sizeof(b) - 1);
    if (n > 0) return std::string(b, (size_t)n);
#endif
    return argv0;
}

/** @brief Largest protocol line; anything larger travels as a file and the line carries its path. */
static constexpr size_t LINE_MAX_BYTES = 1u << 20;

/** @brief Write all of @p n bytes; false when the other end is gone. */
inline bool pipeWriteAll(PipeEnd p, const char* d, size_t n)
{
    while (n > 0)
    {
#ifdef _WIN32
        DWORD w = 0;
        if (!WriteFile(p, d, (DWORD)(n > 0x10000 ? 0x10000 : n), &w, nullptr) || w == 0) return false;
#else
        const ssize_t w = ::write(p, d, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
#endif
        d += w;
        n -= (size_t)w;
    }
    return true;
}

/** @brief Read some bytes; 0 = the other end closed (or an error), never blocks forever on a closed pipe. */
inline long pipeRead(PipeEnd p, char* d, size_t cap)
{
#ifdef _WIN32
    DWORD r = 0;
    if (!ReadFile(p, d, (DWORD)cap, &r, nullptr)) return 0;
    return (long)r;
#else
    for (;;)
    {
        const ssize_t r = ::read(p, d, cap);
        if (r < 0 && errno == EINTR) continue;
        return r < 0 ? 0 : (long)r;
    }
#endif
}

inline void pipeClose(PipeEnd& p)
{
    if (p == PIPE_NONE) return;
#ifdef _WIN32
    CloseHandle(p);
#else
    ::close(p);
#endif
    p = PIPE_NONE;
}

/**
 * @brief Splits a byte stream into lines. A line over LINE_MAX_BYTES is dropped whole and counted.
 */
class LineReader
{
  public:
    explicit LineReader(PipeEnd p) : _p(p) {}

    /** @brief Next line without its newline; false when the pipe closed and nothing is left. */
    bool next(std::string& line)
    {
        for (;;)
        {
            const size_t nl = _buf.find('\n', _scan);
            if (nl != std::string::npos)
            {
                if (_skipping)
                {
                    _skipping = false;
                    _buf.erase(0, nl + 1);
                    _scan = 0;
                    continue;
                }
                line.assign(_buf, 0, nl);
                _buf.erase(0, nl + 1);
                _scan = 0;
                return true;
            }
            _scan = _buf.size();
            if (_buf.size() > LINE_MAX_BYTES)
            {
                _oversize++;
                _skipping = true;
                _buf.clear();
                _scan = 0;
            }
            char tmp[65536];
            const long n = pipeRead(_p, tmp, sizeof(tmp));
            if (n <= 0) return false;
            _buf.append(tmp, (size_t)n);
        }
    }
    uint64_t oversize() const { return _oversize; }

  private:
    PipeEnd _p;
    std::string _buf;
    size_t _scan = 0;
    bool _skipping = false;
    uint64_t _oversize = 0;
};

/** @brief A started worker: its pipe ends on our side and its process. */
struct Child
{
    PipeEnd toChild = PIPE_NONE;   ///< we write jobs here (the worker's stdin)
    PipeEnd fromChild = PIPE_NONE; ///< we read events here (the worker's stdout)
#ifdef _WIN32
    HANDLE proc = nullptr;
    DWORD pid = 0;
#else
    pid_t pid = -1;
#endif

    /** @brief Hard stop, for a worker that does not leave on its own. */
    void kill()
    {
#ifdef _WIN32
        if (proc) TerminateProcess(proc, 1);
#else
        if (pid > 0) ::kill(pid, SIGKILL);
#endif
    }
    /** @brief Reap the process (blocks until it ended). */
    void wait()
    {
#ifdef _WIN32
        if (proc)
        {
            WaitForSingleObject(proc, INFINITE);
            CloseHandle(proc);
            proc = nullptr;
        }
#else
        if (pid > 0)
        {
            int st = 0;
            while (::waitpid(pid, &st, 0) < 0 && errno == EINTR)
            {
            }
            pid = -1;
        }
#endif
    }
    long pidValue() const { return (long)pid; }
};

#ifndef _WIN32
/** @brief Close every descriptor from 3 up in a forked child; only async-signal-safe calls. */
inline void closeFromThree(int maxFd)
{
    #if defined(__linux__) && defined(SYS_close_range)
    if (::syscall(SYS_close_range, 3u, ~0u, 0u) == 0) return;
    #endif
    for (int fd = 3; fd < maxFd; ++fd) ::close(fd);
}
#endif

/**
 * @brief Start `self args…` with stdin/stdout on a fresh pipe pair and stderr on @p logPath (empty = discard).
 * @details The child runs in its own process group and inherits nothing else; with @p newSession (serve --detach)
 *          in its own session, without the terminal, and its stdout goes to @p logPath as well.
 */
inline bool spawnChild(const std::string& self, const std::vector<std::string>& args, const std::string& logPath,
                       Child& out, std::string& err, bool newSession = false)
{
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr;
    if (!CreatePipe(&inR, &inW, &sa, 1u << 20) || !CreatePipe(&outR, &outW, &sa, 1u << 20))
    {
        err = "CreatePipe failed";
        return false;
    }
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0); // our ends stay ours
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    HANDLE errH = CreateFileA(logPath.empty() ? "NUL" : logPath.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (errH == INVALID_HANDLE_VALUE)
    {
        err = "cannot open the worker log";
        CloseHandle(inR); CloseHandle(inW); CloseHandle(outR); CloseHandle(outW);
        return false;
    }
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<char> attrBuf(attrSize);
    auto attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)attrBuf.data();
    HANDLE inherit[3] = {inR, outW, errH};
    bool okAttr = InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) &&
                  UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr,
                                            nullptr);
    STARTUPINFOEXA si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = inR;
    si.StartupInfo.hStdOutput = newSession ? errH : outW; // a detached serve writes its own lines to the log
    si.StartupInfo.hStdError = errH;
    si.lpAttributeList = attrs;
    std::string cmd = "\"" + self + "\"";
    for (const auto& a : args) cmd += " \"" + a + "\""; // arguments are addresses and flags, no quotes inside
    PROCESS_INFORMATION pi{};
    const DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW;
    // detached: leave the job of the starting terminal, so closing it does not end serve; a job that forbids
    // that refuses with ERROR_ACCESS_DENIED, then serve starts inside it as before
    BOOL ok = okAttr && newSession &&
              CreateProcessA(nullptr, &cmd[0], nullptr, nullptr, TRUE, flags | CREATE_BREAKAWAY_FROM_JOB, nullptr, nullptr,
                             &si.StartupInfo, &pi);
    if (okAttr && !ok) ok = CreateProcessA(nullptr, &cmd[0], nullptr, nullptr, TRUE, flags, nullptr, nullptr, &si.StartupInfo, &pi);
    if (okAttr) DeleteProcThreadAttributeList(attrs);
    CloseHandle(inR);
    CloseHandle(outW);
    CloseHandle(errH);
    if (!ok)
    {
        err = "CreateProcess failed";
        CloseHandle(inW);
        CloseHandle(outR);
        return false;
    }
    CloseHandle(pi.hThread);
    out.proc = pi.hProcess;
    out.pid = pi.dwProcessId;
    out.toChild = inW;
    out.fromChild = outR;
    return true;
#else
    int in[2], o[2];
    if (::pipe(in) != 0) { err = "pipe failed"; return false; }
    if (::pipe(o) != 0) { ::close(in[0]); ::close(in[1]); err = "pipe failed"; return false; }
    for (int fd : {in[0], in[1], o[0], o[1]}) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    const int logFd = logPath.empty() ? ::open("/dev/null", O_WRONLY | O_CLOEXEC)
                                      : ::open(logPath.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (logFd < 0)
    {
        err = "cannot open the worker log";
        for (int fd : {in[0], in[1], o[0], o[1]}) ::close(fd);
        return false;
    }
    // Everything the child needs is prepared here: after fork only async-signal-safe calls are allowed.
    std::vector<std::string> store;
    store.push_back(self);
    for (const auto& a : args) store.push_back(a);
    std::vector<char*> argv;
    for (auto& s : store) argv.push_back(&s[0]);
    argv.push_back(nullptr);
    struct rlimit rl{};
    int maxFd = 1024;
    if (::getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY) maxFd = (int)rl.rlim_cur;
    if (maxFd > 65536) maxFd = 65536;

    const pid_t pid = ::fork();
    if (pid < 0)
    {
        err = "fork failed";
        for (int fd : {in[0], in[1], o[0], o[1], logFd}) ::close(fd);
        return false;
    }
    if (pid == 0)
    {
        if (newSession) ::setsid(); // no controlling terminal: closing it neither stops nor hangs up serve
        else
            ::setpgid(0, 0);
        ::dup2(in[0], 0);
        ::dup2(newSession ? logFd : o[1], 1);
        ::dup2(logFd, 2);
        closeFromThree(maxFd);
        ::execv(argv[0], argv.data());
        ::_exit(127);
    }
    ::close(in[0]);
    ::close(o[1]);
    ::close(logFd);
    out.pid = pid;
    out.toChild = in[1];
    out.fromChild = o[0];
    return true;
#endif
}

/**
 * @brief Worker side: take the protocol pair off stdin/stdout so a stray print cannot reach it.
 * @details fd 1 then points at stderr (the worker log), stdin at nothing.
 */
inline bool claimProtocolPipes(PipeEnd& in, PipeEnd& out)
{
#ifdef _WIN32
    // Own duplicates: the CRT descriptors 0/1 wrap the very same handles, and _dup2 below closes fd 1.
    const HANDLE self = GetCurrentProcess();
    if (!DuplicateHandle(self, GetStdHandle(STD_INPUT_HANDLE), self, &in, 0, FALSE, DUPLICATE_SAME_ACCESS) ||
        !DuplicateHandle(self, GetStdHandle(STD_OUTPUT_HANDLE), self, &out, 0, FALSE, DUPLICATE_SAME_ACCESS))
        return false;
    HANDLE errH = GetStdHandle(STD_ERROR_HANDLE);
    SetStdHandle(STD_OUTPUT_HANDLE, errH);
    _dup2(2, 1);
    HANDLE nul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (nul != INVALID_HANDLE_VALUE) SetStdHandle(STD_INPUT_HANDLE, nul);
    return true;
#else
    in = ::dup(0);
    out = ::dup(1);
    if (in < 0 || out < 0) return false;
    ::fcntl(in, F_SETFD, FD_CLOEXEC);
    ::fcntl(out, F_SETFD, FD_CLOEXEC);
    ::dup2(2, 1);
    const int nul = ::open("/dev/null", O_RDONLY);
    if (nul >= 0)
    {
        ::dup2(nul, 0);
        ::close(nul);
    }
    return true;
#endif
}

} // namespace serve
} // namespace ftc

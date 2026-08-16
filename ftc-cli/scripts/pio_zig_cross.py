#!/usr/bin/env python3
# Open ■
# ┬────┴  pio_zig_cross
# ■ KNX   2026 OpenKNX - Erkan Çolak
#
# PlatformIO pre-build hook: make ONE `pio run` cross-build the whole desktop matrix.
#
# PlatformIO's `platform = native` has no built-in cross-toolchain, so this script IS how pio pulls it:
# for a zig target it provisions a project-local `zig` (downloaded + cached under .tools/, gitignored),
# writes tiny cc/c++/ar/ranlib wrappers that exec `zig cc|c++ -target <triple>`, and points the SCons
# compiler at them; for a macOS target it uses the host clang with `-arch` + the Xcode SDK. The env NAME
# selects the target (ftc-cli-<os>-<arch>), so `pio run` (all default_envs) yields every ftc-<os>-<arch>.

import os
import platform
import stat
import sys
import tarfile
import urllib.request
import zipfile

Import("env")  # noqa: F821  (injected by PlatformIO/SCons)

ZIG_VERSION = "0.13.0"
ZIG_BASE_URL = "https://ziglang.org/download"
GLIBC_PIN = "2.28"      # portable glibc floor for the linux-gnu targets
WINSOCK = ["-lws2_32"]  # Winsock for the Windows tunnel/discovery sockets

# target -> (os, engine, mac_arch, zig_triple, extra_link_flags)
TARGETS = {
    "macos-arm64":   ("macos",   "clang", "arm64",  None,                             []),
    "macos-x64":     ("macos",   "clang", "x86_64", None,                             []),
    "linux-x64":     ("linux",   "zig",   None,     "x86_64-linux-gnu." + GLIBC_PIN,  []),
    "linux-arm64":   ("linux",   "zig",   None,     "aarch64-linux-gnu." + GLIBC_PIN, []),
    "linux-armhf":   ("linux",   "zig",   None,     "arm-linux-gnueabihf." + GLIBC_PIN, []), # 32-bit ARM hard-float: Raspberry Pi OS 32-bit (armv7)
    "windows-x86":   ("windows", "zig",   None,     "x86-windows-gnu",                WINSOCK),
    "windows-x64":   ("windows", "zig",   None,     "x86_64-windows-gnu",             WINSOCK),
    "windows-arm64": ("windows", "zig",   None,     "aarch64-windows-gnu",            WINSOCK),
}

proj = env["PROJECT_DIR"]                        # .../ftc-cli
# zig + the cc/c++ wrappers live in the shared PlatformIO core cache, so they are downloaded ONCE and
# reused across projects / checkouts (survive a project clean).
tools_dir = os.path.join(env["PROJECT_CORE_DIR"], ".cache", "ftc-cli-zig")


def die(msg):
    sys.stderr.write("ftc-cli cross: " + msg + "\n")
    env.Exit(1)


def host_slug():
    sysname = platform.system().lower()
    mach = platform.machine().lower()
    arm = mach in ("arm64", "aarch64")
    if sysname == "darwin":
        return ("macos-aarch64" if arm else "macos-x86_64"), "tar.xz", "zig"
    if sysname == "linux":
        return ("linux-aarch64" if arm else "linux-x86_64"), "tar.xz", "zig"
    if sysname == "windows":
        return "windows-x86_64", "zip", "zig.exe"
    die("no pinned zig download for host %s/%s" % (sysname, mach))


def provision_zig():
    """Download + cache a project-local zig for the host; return the zig binary path."""
    slug, ext, zig_exe = host_slug()
    extract_dir = os.path.join(tools_dir, "zig-%s-%s" % (slug, ZIG_VERSION))
    zig_bin = os.path.join(extract_dir, zig_exe)
    if os.path.isfile(zig_bin):
        return zig_bin  # cached

    os.makedirs(tools_dir, exist_ok=True)
    archive = "zig-%s-%s.%s" % (slug, ZIG_VERSION, ext)
    url = "%s/%s/%s" % (ZIG_BASE_URL, ZIG_VERSION, archive)
    dst = os.path.join(tools_dir, archive)
    print("ftc-cli cross: provisioning zig %s %s (shared PlatformIO core cache)" % (slug, ZIG_VERSION))
    print("  fetch  : " + url)
    # Prefer curl: it uses the SYSTEM cert store (macOS Python's urllib often can't verify SSL).
    import shutil
    import subprocess
    if shutil.which("curl"):
        rc = subprocess.call(["curl", "-fsSL", "-o", dst, url])
        if rc != 0:
            die("zig download failed (curl exit %d) from %s" % (rc, url))
    else:
        try:
            urllib.request.urlretrieve(url, dst)
        except Exception as e:  # noqa: BLE001
            die("could not download zig from %s (%s)" % (url, e))
    print("  unpack : " + archive)
    if ext == "zip":
        with zipfile.ZipFile(dst) as z:
            z.extractall(tools_dir)
    else:
        with tarfile.open(dst) as t:
            t.extractall(tools_dir)
    os.remove(dst)
    if not os.path.isfile(zig_bin):
        die("zig binary not found after extraction: " + zig_bin)
    print("  ready  : " + zig_bin)
    return zig_bin


def write_wrappers(bin_dir, zig, triple):
    """Write cc/c++/gcc/g++/ar/ranlib that exec `zig <sub> -target <triple>` (POSIX host only)."""
    if platform.system().lower() == "windows":
        die("zig cross wrappers need a POSIX host (macOS/Linux); Windows-host cross is CI's job.")
    os.makedirs(bin_dir, exist_ok=True)
    scripts = {
        "cc":  '#!/bin/sh\nexec "%s" cc -target %s "$@"\n' % (zig, triple),
        "gcc": '#!/bin/sh\nexec "%s" cc -target %s "$@"\n' % (zig, triple),
        "c++": '#!/bin/sh\nexec "%s" c++ -target %s "$@"\n' % (zig, triple),
        "g++": '#!/bin/sh\nexec "%s" c++ -target %s "$@"\n' % (zig, triple),
        "ar":     '#!/bin/sh\nexec "%s" ar "$@"\n' % zig,
        "ranlib": '#!/bin/sh\nexec "%s" ranlib "$@"\n' % zig,
    }
    for name, body in scripts.items():
        p = os.path.join(bin_dir, name)
        with open(p, "w") as f:
            f.write(body)
        os.chmod(p, os.stat(p).st_mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)
    return bin_dir


def mac_sdk_path():
    try:
        import subprocess
        sdk = subprocess.check_output(["xcrun", "--sdk", "macosx", "--show-sdk-path"]).decode().strip()
        if sdk and os.path.isdir(sdk):
            return sdk
    except Exception:  # noqa: BLE001
        pass
    return "/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk"


# ---- resolve this env's target from its name ----------------------------------------------------
name = env["PIOENV"]
# The env NAME selects the target: "ftc-cli-<target>" (the CLI exe) or "libftc-<target>" (the shared lib).
if name.startswith("ftc-cli-"):
    target = name[len("ftc-cli-"):]
elif name.startswith("libftc-"):
    target = name[len("libftc-"):]
else:
    target = name
if target not in TARGETS:
    die("env '%s' is not a known target; expected ftc-cli-<%s>" % (name, "|".join(TARGETS)))
os_token, engine, mac_arch, triple, link_flags = TARGETS[target]

# The libftc-* envs build a SHARED library (not an executable): -shared is a LINK flag, so it must go to
# LINKFLAGS (build_flags would only reach the compile step -> the linker would still demand main()).
if name.startswith("libftc-"):
    env.Append(LINKFLAGS=["-shared"])

if engine == "clang":
    # macOS: host clang, target arch + Xcode SDK (safe -isysroot; no libc++ override).
    if platform.system().lower() != "darwin":
        die("target %s needs a macOS host" % target)
    sdk = mac_sdk_path()
    cxx_inc = os.path.join(sdk, "usr", "include", "c++", "v1")
    # -nostdinc++ + the SDK's libc++ headers works around a broken CommandLineTools libc++ stub
    # (the active CLT often lacks <chrono> etc.); -isysroot/-arch pick the SDK + target arch.
    env.Append(CCFLAGS=["-arch", mac_arch, "-isysroot", sdk],
               CXXFLAGS=["-nostdinc++", "-isystem", cxx_inc],
               LINKFLAGS=["-arch", mac_arch, "-isysroot", sdk,
                          # CoreFoundation: the desktop language on macOS is its own setting, not LC_*
                          # (cli/I18n.h). It has to go here — build_flags never reach the link step.
                          "-framework", "CoreFoundation"])
else:
    # linux/windows: project-local zig cross-compiler. PlatformIO's native platform resolves the
    # compiler by NAME (cc/c++) from the build PATH, so prepend the wrapper dir there (env.Replace(CXX=...)
    # alone does not stick for `platform = native`).
    zig = provision_zig()
    wrap = write_wrappers(os.path.join(tools_dir, "wrap", target), zig, triple)
    env.PrependENVPath("PATH", wrap)
    env.Replace(CC="cc", CXX="c++", LINK="c++", AR="ar", RANLIB="ranlib")
    # Strip symbols at link time: the zig linux/windows exes are ~10 MB unstripped (debug/symbol
    # tables) vs ~1 MB stripped; these binaries are checked in under release/Tools, so keep them lean.
    env.Append(LINKFLAGS=["-s"])
    if link_flags:
        env.Append(LINKFLAGS=link_flags)

# ---- name the built program `ftc` (`ftc.exe` on Windows), in .pio/build/<env>/ -----------------
# pio's default output is "program"; rename the target so each env dir already holds ftc[.exe]. The
# consuming product (OAM Build-Release) then just copies .pio/build/<env>/ftc[.exe] -- no rename dance.
env.Replace(PROGNAME="ftc")
if os_token == "windows":
    env.Replace(PROGSUFFIX=".exe")

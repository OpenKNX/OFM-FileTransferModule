/**
 * @file        KnxOta.h
 * @brief       knxOTA — firmware update over the KNX bus. File analysis, version comparison, rendering.
 * @details     The deterministic half of the feature: everything that can be decided from the firmware file
 *              and one reading of the device, with no side effects. It is deliberately separate from the
 *              transfer itself so `--check` can run the whole judgement offline-plus-one-read and write
 *              nothing at all — that is the safe first run against any device.
 *
 *              The comparison only ever puts comparable fields side by side. A .uf2 states its identity in
 *              the KNX extension tag; the device states the same four values in PID 78 and PID 25. An
 *              ESP32 .bin states its chip but not (yet) its version, so its verdict is Unknown and no
 *              direction is claimed — claiming one would be worse than admitting we cannot tell.
 * @date        2026-08-11
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

#include "../core/EspImage.h"
#include "../core/Gzip.h"
#include "../core/HostFs.h"
#include "../core/Uf2.h"
#include "I18n.h"
#include "Templates.h"
#include "Theme.h"

namespace ftc
{

/** @brief What kind of firmware file we were handed. */
enum class FwKind
{
    None,
    Uf2,     ///< RP2040/RP2350, carries an identity tag
    EspBin,  ///< ESP32 application image
    RawBin,  ///< a bin we cannot attribute (an RP .bin loses the tag in the conversion)
    Gz,      ///< already compressed; passed through untouched
};

/** @brief The firmware file, as far as it describes itself. */
struct FwFile
{
    bool ok = false;
    std::string error;
    std::string path;
    FwKind kind = FwKind::None;
    Uf2Identity id;               ///< valid only when the file states an OpenKNX identity
    std::string hardware;         ///< "RP2040", "ESP32-S3", …
    uint32_t familyId = 0;        ///< UF2 family, 0 for ESP
    uint16_t chipId = 0xFFFF;     ///< ESP chip id, 0xFFFF for RP
    std::vector<uint8_t> payload; ///< what goes on the wire (gzip for RP, raw for ESP)
    size_t rawSize = 0;           ///< the flashed image size, before compression
    bool compressed = false;
};

/** @brief How the file relates to what is running on the device. */
enum class Verdict
{
    Upgrade,
    AlreadyInstalled,
    Downgrade,
    DifferentDevice, ///< another OpenKNX application or another vendor id — refuse
    Unknown,         ///< one side did not state a version; never claim a direction
};

/** @brief One device's identity, read from PID 78 + PID 25. */
struct DevVersion
{
    bool valid = false;
    uint8_t openKnxId = 0, appNumber = 0, major = 0, minor = 0, revision = 0;
};

/** @brief Decode the device identity out of an FtcDeviceInfo-shaped reading. */
inline DevVersion devVersionFrom(const uint8_t hardware[6], bool haveHw, uint16_t version, bool haveVersion)
{
    DevVersion v;
    if (!haveHw) return v;
    v.openKnxId = hardware[2];
    v.appNumber = hardware[3];
    v.major = (uint8_t)(hardware[4] >> 4);
    v.minor = (uint8_t)(hardware[4] & 0x0F);
    // OpenKNX packs its firmware revision into the magic field of the KNX version property.
    if (haveVersion) v.revision = (uint8_t)((version >> 11) & 0x1F);
    v.valid = true;
    return v;
}

/** @brief Compare file against device. Only comparable fields are ever put side by side. */
inline Verdict compareVersions(const Uf2Identity& file, const DevVersion& dev)
{
    if (!file.valid || !dev.valid) return Verdict::Unknown;
    if (file.openKnxId != dev.openKnxId || file.appNumber != dev.appNumber) return Verdict::DifferentDevice;
    // The device reports its revision in 5 bits of PID_VERSION; the file stamp is a full byte. Masking
    // makes the two comparable — without it, revision 32 reads as 0 on the device and every comparison
    // from there on is wrong in one direction or the other.
    const uint32_t f = ((uint32_t)file.major() << 16) | ((uint32_t)file.minor() << 8) | (file.revision & 0x1F);
    const uint32_t d = ((uint32_t)dev.major << 16) | ((uint32_t)dev.minor << 8) | dev.revision;
    if (f > d) return Verdict::Upgrade;
    if (f == d) return Verdict::AlreadyInstalled;
    return Verdict::Downgrade;
}

/** @brief The verdict as a chip, in the user's language. */
inline std::string verdictChip(const Tpl& t, const I18n& L, Verdict v)
{
    switch (v)
    {
        case Verdict::Upgrade: return t.chip(L.tr("UPGRADE", "UPGRADE"), 'g');
        case Verdict::AlreadyInstalled: return t.chip(L.tr("ALREADY INSTALLED", "SCHON DRAUF"), 'a');
        case Verdict::Downgrade: return t.chip(L.tr("DOWNGRADE", "DOWNGRADE"), 'a');
        case Verdict::DifferentDevice: return t.chip(L.tr("DIFFERENT DEVICE", "ANDERES GERÄT"), 'r');
        default: return t.chip(L.tr("VERSION UNKNOWN", "VERSION UNBEKANNT"), 'a');
    }
}

/** @brief "1.1.0" from the packed identity. */
inline std::string fwVersionText(const Uf2Identity& id)
{
    if (!id.valid) return "—";
    char b[24];
    std::snprintf(b, sizeof(b), "%u.%u.%u", (unsigned)id.major(), (unsigned)id.minor(), (unsigned)id.revision);
    return b;
}
inline std::string devVersionText(const DevVersion& d)
{
    if (!d.valid) return "—";
    char b[24];
    std::snprintf(b, sizeof(b), "%u.%u.%u", (unsigned)d.major, (unsigned)d.minor, (unsigned)d.revision);
    return b;
}

/** @brief Lower-cased file extension, "" when there is none. */
inline std::string fileExt(const std::string& p)
{
    const size_t dot = p.find_last_of('.');
    const size_t sl = p.find_last_of("/\\");
    if (dot == std::string::npos || (sl != std::string::npos && dot < sl)) return std::string();
    std::string e = p.substr(dot);
    for (char& c : e)
        c = (char)std::tolower((unsigned char)c);
    return e;
}

/**
 * @brief Read a firmware file and prepare exactly what should go on the wire.
 * @details RP images are converted out of their UF2 framing and gzipped — the bootloader unpacks them, and
 *          uncompressed they would not fit the device's filesystem anyway. ESP images go raw, because the
 *          ESP updater checks the image magic on the first byte and a gzip stream fails it outright.
 */
inline bool readFirmware(const std::string& path, FwFile& out, const I18n& L)
{
    out = FwFile();
    out.path = path;
    const std::string ext = fileExt(path);

    if (ext == ".uf2")
    {
        Uf2Image u;
        if (!parseUf2(path, u))
        {
            out.error = u.error;
            return false;
        }
        if (!uf2IsRpFlashFamily(u.familyId))
        {
            out.error = L.tr("this UF2 is not for an RP2040 or RP2350 device", "diese UF2 ist nicht für ein RP2040- oder RP2350-Gerät");
            return false;
        }
        out.kind = FwKind::Uf2;
        out.id = u.id;
        out.familyId = u.familyId;
        out.hardware = uf2FamilyName(u.familyId);
        out.rawSize = u.bin.size();
        if (!gzipBuffer(u.bin.data(), u.bin.size(), out.payload))
        {
            out.error = L.tr("the firmware could not be compressed", "die Firmware konnte nicht komprimiert werden");
            return false;
        }
        out.compressed = true;
        out.ok = true;
        return true;
    }

    if (ext == ".gz")
    {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (f == nullptr)
        {
            out.error = L.tr("cannot open the file", "die Datei lässt sich nicht öffnen");
            return false;
        }
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (n <= 18)
        {
            std::fclose(f);
            out.error = L.tr("the file is too small to be a compressed firmware", "die Datei ist zu klein für eine komprimierte Firmware");
            return false;
        }
        out.payload.resize((size_t)n);
        const size_t rd = std::fread(out.payload.data(), 1, out.payload.size(), f);
        std::fclose(f);
        if (rd != out.payload.size())
        {
            out.error = L.tr("the file could not be read completely", "die Datei konnte nicht vollständig gelesen werden");
            return false;
        }
        if (out.payload[0] != 0x1F || out.payload[1] != 0x8B)
        {
            out.error = L.tr("this .gz is not a gzip file", "diese .gz ist keine gzip-Datei");
            return false;
        }
        out.kind = FwKind::Gz;
        out.compressed = true;
        // the flashed size is the gzip trailer's ISIZE
        const size_t z = out.payload.size();
        out.rawSize = (size_t)out.payload[z - 4] | ((size_t)out.payload[z - 3] << 8) |
                      ((size_t)out.payload[z - 2] << 16) | ((size_t)out.payload[z - 1] << 24);
        out.hardware = "?";
        out.ok = true;
        return true;
    }

    if (ext == ".bin")
    {
        EspImage e;
        const bool esp = parseEspImage(path, e);
        if (!esp && e.error.rfind("cannot open", 0) == 0)
        {
            out.error = e.error; // a missing or unreadable file is not "the wrong kind of .bin"
            return false;
        }
        if (esp)
        {
            if (e.checksumChecked && !e.checksumOk)
            {
                out.error = "diese ESP32-Datei ist beschädigt (die Prüfsumme im Image stimmt nicht)";
                return false;
            }
            out.kind = FwKind::EspBin;
            out.chipId = e.chipId;
            out.hardware = espChipName(e.chipId);
            out.id = e.id; // set only once the build stamps it
            out.rawSize = e.size;
            // ESP32 flashes a raw image today; compressing it needs the target to unpack the stream.
            std::FILE* f = std::fopen(path.c_str(), "rb");
            if (f == nullptr)
            {
                out.error = L.tr("cannot open the file", "die Datei lässt sich nicht öffnen");
                return false;
            }
            out.payload.resize(e.size);
            const size_t rd = std::fread(out.payload.data(), 1, out.payload.size(), f);
            std::fclose(f);
            if (rd != out.payload.size())
            {
                out.error = L.tr("the file could not be read completely", "die Datei konnte nicht vollständig gelesen werden");
                return false;
            }
            out.compressed = false;
            out.ok = true;
            return true;
        }
        out.kind = FwKind::RawBin;
        out.error = L.tr("a .bin from an RP build does not say which device it is for — use the .uf2 next to it", "eine .bin aus einem RP-Build sagt nicht, für welches Gerät sie ist — nimm die .uf2 im selben Ordner");
        return false;
    }

    out.error = L.tr("unknown file type — knxOTA reads .uf2, .bin and .gz", "unbekannter Dateityp — knxOTA liest .uf2, .bin und .gz");
    return false;
}

/**
 * @brief Compress an already-read payload, once the target has said it can unpack one.
 * @details Deliberately NOT done while reading the file: whether compression is allowed depends on the
 *          device, and the device is only known after the file has been judged. An RP image is compressed
 *          from the start because its bootloader has always unpacked; an ESP image only when the target
 *          advertises it, so an older device keeps receiving exactly what it received before.
 */
inline bool compressForTarget(FwFile& fw)
{
    if (fw.compressed) return true;
    std::vector<uint8_t> gz;
    if (!gzipBuffer(fw.payload.data(), fw.payload.size(), gz)) return false;
    if (gz.size() >= fw.payload.size()) return false; // no gain -> stay raw, an image is not always compressible
    fw.rawSize = fw.payload.size();
    fw.payload.swap(gz);
    fw.compressed = true;
    return true;
}

/** @brief The firmware panel: what this file is, in plain words first. */
inline void renderFirmwarePanel(Tpl& t, Theme& c, I18n& L, const FwFile& fw, bool verbose)
{
    // Only the file name in the header — a build path is long enough to push the panel rule off screen,
    // and the full path is already on the command line the user just typed.
    const size_t sl = fw.path.find_last_of("/\\");
    t.panelTop(L.tr("Firmware file", "Firmware-Datei"),
               sl == std::string::npos ? fw.path : fw.path.substr(sl + 1));
    if (fw.id.valid)
    {
        char app[48];
        std::snprintf(app, sizeof(app), "0x%02X / %u", (unsigned)fw.id.openKnxId, (unsigned)fw.id.appNumber);
        t.kv(L.tr("Version", "Version"),
             c.bold(fwVersionText(fw.id)) +
                 c.dim(std::string("   ") + L.tr("application", "Applikation") + " " + app));
    }
    else
        t.kv(L.tr("Version", "Version"),
             c.amber(L.tr("not stated in this file", "steht nicht in dieser Datei")));
    t.kv(L.tr("Hardware", "Hardware"), c.txt(fw.hardware));
    char sz[96];
    if (fw.compressed)
        std::snprintf(sz, sizeof(sz), "%zu B  ->  %zu B", fw.rawSize, fw.payload.size());
    else
        std::snprintf(sz, sizeof(sz), "%zu B", fw.payload.size());
    t.kv(L.tr("Size", "Größe"), c.txt(sz) +
         (fw.compressed ? c.dim(L.tr("   compressed for the bus", "   für den Bus komprimiert")) : std::string()));
    // 350 B/s, not the 400 the bus can peak at: measured on hardware this image ran at 315-357 B/s, and
    // an estimate that overruns costs more trust than one that comes in early.
    const unsigned mins = (unsigned)((fw.payload.size() / 350) / 60);
    char eta[64];
    std::snprintf(eta, sizeof(eta), "~%u %s", mins, L.tr("min", "Min."));
    t.kv(L.tr("Transfer", "Übertragung"), c.txt(eta) +
         c.dim(L.tr("   the KNX bus carries about 350 bytes a second",
                    "   der KNX-Bus trägt etwa 350 Byte pro Sekunde")));
    if (verbose && (fw.familyId != 0 || fw.chipId != 0xFFFF))
    {
        char h[64];
        if (fw.familyId != 0) std::snprintf(h, sizeof(h), "UF2 family 0x%08X", (unsigned)fw.familyId);
        else std::snprintf(h, sizeof(h), "ESP chip id 0x%04X", (unsigned)fw.chipId);
        t.kv("", c.mut(h));
    }
    t.panelEnd();
}

} // namespace ftc

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
#include <vector>

#include "../core/EspImage.h"
#include "../core/Gzip.h"
#include "../core/HostFs.h"
#include "../core/Delta.h"
#include "../core/ImageFacts.h"
#include "../core/Uf2.h"
#include "I18n.h"
#include "Templates.h"
#include "Theme.h"

namespace ftc
{
// Enough of the application image to reach the OpenKNX stamp in its descriptor.
static constexpr size_t detail_espStampEnd = 44;


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
    DifferentApplication, ///< same product, a different application number — possible, but it costs the setup
    DifferentDevice,      ///< another vendor/product id — the wrong file, not a decision
    Unknown,              ///< one side did not state a version; never claim a direction
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
    // Two different things used to share one verdict. A different vendor/product id means the file
    // belongs to another product and nothing good can come of it. A different APPLICATION number, with
    // the same product id, is what a redesign looks like: same device, new application, and the user
    // has to program it again in ETS afterwards -- the physical address included. That is a decision
    // someone may well want to take, so it is put to them instead of being refused for them.
    if (file.openKnxId != dev.openKnxId) return Verdict::DifferentDevice;
    if (file.appNumber != dev.appNumber) return Verdict::DifferentApplication;
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
/**
 * @brief What a verdict means for the person about to decide, in their language.
 * @details The chip alone names the outcome; it does not say what the outcome costs. "NEW APPLICATION"
 *          is a label, not an answer to "should I press yes" -- and the one thing that matters about it,
 *          that the device comes back unprogrammed, is nowhere in those two words.
 */
inline std::vector<std::string> verdictExplain(const I18n& L, Verdict v)
{
    switch (v)
    {
        case Verdict::Upgrade:
            return {L.tr("A newer version. Settings, group addresses and the address are kept.",
                         "Neuere Version. Einrichtung, Gruppenadressen und Adresse bleiben erhalten.")};
        case Verdict::AlreadyInstalled:
            return {L.tr("The device already runs exactly this. Installing it again changes nothing.",
                         "Genau das läuft bereits auf dem Gerät. Erneutes Einspielen ändert nichts.")};
        case Verdict::Downgrade:
            return {L.tr("Older than what the device runs. Program it again in ETS afterwards.",
                         "Älter als das, was auf dem Gerät läuft. Danach in der ETS neu programmieren.")};
        case Verdict::DifferentApplication:
            return {L.tr("Same product, but a new ETS application.",
                         "Gleiches Produkt, aber eine neue ETS-Applikation."),
                    L.tr("The device comes back unprogrammed on 15.15.255: parameters, group",
                         "Das Gerät kommt unprogrammiert auf 15.15.255 zurück: Parameter,"),
                    L.tr("addresses and the physical address all have to be set again in ETS.",
                         "Gruppenadressen und die physikalische Adresse müssen in der ETS neu vergeben werden.")};
        case Verdict::DifferentDevice:
            return {L.tr("This firmware belongs to a different product and will not be installed.",
                         "Diese Firmware gehört zu einem anderen Produkt und wird nicht eingespielt.")};
        default:
            return {L.tr("One side states no version, so knxOTA cannot tell whether the file fits.",
                         "Eine Seite nennt keine Version, knxOTA kann also nicht sagen, ob die Datei passt.")};
    }
}

/** @brief "0xAD/1" -- the product id and the application number, the pair a verdict turns on. */
inline std::string appIdText(uint8_t openKnxId, uint8_t appNumber)
{
    char b[24];
    std::snprintf(b, sizeof(b), "0x%02X/%u", (unsigned)openKnxId, (unsigned)appNumber);
    return b;
}

inline std::string verdictChip(const Tpl& t, const I18n& L, Verdict v)
{
    switch (v)
    {
        case Verdict::Upgrade: return t.chip(L.tr("UPGRADE", "UPGRADE"), 'g');
        case Verdict::AlreadyInstalled: return t.chip(L.tr("ALREADY INSTALLED", "SCHON DRAUF"), 'a');
        case Verdict::Downgrade: return t.chip(L.tr("DOWNGRADE", "DOWNGRADE"), 'a');
        case Verdict::DifferentApplication: return t.chip(L.tr("NEW APPLICATION", "NEUE ANWENDUNG"), 'a');
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
        // Cut to the length the release states: the block padding is not part of the image, and sending
        // it would make what lands on the device differ from what a later difference is computed against.
        ImageFacts fx;
        if (readImageFacts(path, fx) && !applyImageFacts(fx, u.bin))
        {
            out.error = L.tr("this UF2 is shorter than the release states",
                             "diese UF2 ist kürzer, als das Release angibt");
            return false;
        }
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
        // An esptool bundle: the application is inside it, and the release states where. With that
        // stated, there is no reason to refuse the file a user actually has -- and it is what lets a
        // release ship one package per device instead of the same image three times.
        {
            ImageFacts fx;
            if (readImageFacts(path, fx) && fx.appLength > 0 && fx.format == "factory")
            {
                std::vector<uint8_t> whole;
                if (delta::readWholeFileInto(path, whole) &&
                    (size_t)fx.appOffset + fx.appLength <= whole.size())
                {
                    out.payload.assign(whole.begin() + fx.appOffset,
                                       whole.begin() + fx.appOffset + fx.appLength);
                    const uint8_t* a = out.payload.data();
                    if (out.payload.size() > detail_espStampEnd && a[0] == 0xE9)
                    {
                        out.kind = FwKind::EspBin;
                        out.chipId = (uint16_t)(a[12] | (a[13] << 8));
                        out.hardware = espChipName(out.chipId);
                        const uint8_t* st = a + 40; // esp_app_desc_t.reserv1 — the OpenKNX stamp
                        if (st[0] || st[1] || st[2] || st[3])
                        {
                            out.id.openKnxId = st[0];
                            out.id.appNumber = st[1];
                            out.id.appVersion = st[2];
                            out.id.revision = st[3];
                            out.id.valid = true;
                        }
                        out.rawSize = out.payload.size();
                        out.compressed = false; // compressed later, once the device says it can unpack
                        out.ok = true;
                        return true;
                    }
                }
                out.error = L.tr("this package does not hold the image the release describes",
                                 "dieses Paket enthält nicht das Image, das das Release angibt");
                return false;
            }
        }

        out.kind = FwKind::RawBin;
        // A raw RP image states no identity, so it cannot be checked against the device -- but it is also
        // exactly the file a difference is computed from, and knxOTA finds that one by itself. Saying only
        // "use the .uf2" would leave someone who reached for the .app.bin on purpose none the wiser.
        const bool isAppImage = (path.size() > 8 && path.compare(path.size() - 8, 8, ".app.bin") == 0);
        const bool isFactory = (path.size() > 12 && path.compare(path.size() - 12, 12, ".factory.bin") == 0);
        // A .factory.bin is an esptool package: bootloader and partition table sit in front of the image,
        // so nothing at offset 0 states an identity. Reaching this branch at all means the ESP parse
        // failed, and pointing at a .uf2 would send an ESP owner looking for a file that does not exist.
        if (isFactory)
            out.error = L.tr("this is the packaged image for a USB flash - over the bus use the .bin beside it",
                             "das ist das Paket für das Flashen über USB - über den Bus die .bin daneben nehmen");
        else if (isAppImage)
            out.error = L.tr("this is the raw application image - knxOTA finds it by itself; pass the .uf2 next to it",
                             "das ist das rohe Anwendungsimage - knxOTA findet es von selbst; nimm die .uf2 daneben");
        else
            out.error = L.tr("a .bin from an RP build does not say which device it is for - use the .uf2 next to it",
                             "eine .bin aus einem RP-Build sagt nicht, für welches Gerät sie ist - nimm die .uf2 im selben Ordner");
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
/**
 * @brief How long a payload takes over the bus, as the RANGE the hardware actually spans.
 * @details A single number here was read as a promise and then missed: what a bus carries depends on the
 *          interface, from about 350 B/s on a slow one to about 650 B/s on a fast one with `fast` mode.
 *          Quoting both ends is the only honest answer before a single byte has moved -- and once it has,
 *          the live rate in the transfer view replaces the estimate anyway.
 */
inline std::string transferEta(const I18n& L, size_t bytes)
{
    const unsigned slow = (unsigned)((bytes / 350) / 60); // minutes on a slow interface
    const unsigned fast = (unsigned)((bytes / 650) / 60); // ...and on a fast one
    char b[64];
    if (slow == 0) std::snprintf(b, sizeof(b), "< 1 %s", L.tr("min", "Min."));
    else if (fast == slow) std::snprintf(b, sizeof(b), "~%u %s", slow, L.tr("min", "Min."));
    else std::snprintf(b, sizeof(b), "%u-%u %s", fast, slow, L.tr("min", "Min."));
    return b;
}

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
    // No duration here. Three things it depends on are all still unknown at this point: which interface
    // will carry it (350-650 B/s between models), whether only a difference goes, and -- for an ESP --
    // whether the device unpacks. The Ready panel says it once all three are settled.
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

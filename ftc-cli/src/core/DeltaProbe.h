#pragma once

/**
 * @file        DeltaProbe.h
 * @brief       Asks a device whether it is running exactly the image a patch was built against.
 *
 * The device answers about the image it is really executing, not about a version number: two builds of
 * the same version differ, and only the checksum settles it. That is what makes a patch safe to offer
 * without a warning — a patch for the wrong base is refused before a byte is transferred.
 *
 * Checksumming a whole image takes the device seconds, so it answers "still computing" and expects to be
 * asked again. The re-poll here is bounded by ONE deadline taken when the probe starts; re-arming it on
 * every answer would leave a stuck device holding the caller for ever.
 * @date        2026-08-22
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 */

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "../cli/I18n.h"
#include "../knx_ip_tunnel.h"
#include "Delta.h"
#include "EspImage.h"
#include "ImageFacts.h"
#include "Uf2.h"

namespace ftc
{
    /** @brief Read a whole file; false if it cannot be read or is empty. */
    inline bool readWholeFile(const std::string& path, std::vector<uint8_t>& out)
    {
        return delta::readWholeFileInto(path, out);
    }

    /**
     * @brief The raw application image belonging to a firmware file.
     * @details A release carries the packaged form (.uf2 / .factory.bin) and, next to it, the raw image
     *          as <base>.app.bin. Only the raw one can serve a difference, so it is looked up here rather
     *          than asked for -- it is a property of the release layout, not a decision.
     */
    inline std::string siblingAppImage(const std::string& firmwarePath)
    {
        std::error_code ec;
        const std::filesystem::path p(firmwarePath);
        if (p.extension() == ".bin" && p.stem().extension() == ".app" &&
            std::filesystem::is_regular_file(p, ec))
            return firmwarePath; // already the raw image

        std::filesystem::path base = p;
        base.replace_extension(); // drop .uf2 / .bin / .gz
        if (base.extension() == ".factory") base.replace_extension();
        const std::filesystem::path candidate = base.string() + ".app.bin";
        if (std::filesystem::is_regular_file(candidate, ec)) return candidate.string();
        return "";
    }

    /**
     * @brief The raw application image behind whatever the user pointed at.
     * @details A release is what someone has at hand, and what they have is rarely the raw image: it is
     *          the .uf2 they flashed, or the folder they downloaded. A .uf2 CARRIES the image -- it is a
     *          block wrapper around it -- so it is unwrapped here rather than refused, which makes every
     *          RP release ever built usable as a base, including ones from before .app.bin shipped.
     *
     *          What must NOT happen is what happened before: accepting any file at all and checksumming
     *          the wrapper. The device then answers "different release", and the message blames the
     *          device for what was really the wrong kind of file.
     * @param used  what was actually taken, for the message that follows.
     * @param why   on failure, the reason in one sentence.
     */
    inline bool loadBaseImage(const std::string& hint, std::vector<uint8_t>& out,
                              std::string& used, std::string& why)
    {
        out.clear(); used.clear(); why.clear();
        if (hint.empty()) return false;
        std::error_code ec;
        const std::filesystem::path p(hint);

        auto takeApp = [&](const std::string& f) {
            if (!readWholeFile(f, out)) { why = "cannot be read"; return false; }
            used = f;
            return true;
        };
        auto takeUf2 = [&](const std::string& f) {
            Uf2Image u;
            if (!parseUf2(f, u) || u.bin.empty()) { why = u.error.empty() ? "not a usable UF2" : u.error; return false; }
            out = u.bin;
            // A .uf2 is padded to a whole 256-byte block, so what comes out is a few bytes longer than
            // the image. Those bytes are what a base check trips over, and only the facts file can say
            // where to cut. Without one the base is offered as-is and will simply not be confirmed.
            ImageFacts fx;
            if (readImageFacts(f, fx) && !applyImageFacts(fx, out))
            { why = "the image is shorter than this release states"; return false; }
            if (!fx.valid) why = "no .image.txt beside this package - the exact length is not stated";
            used = f;
            return true;
        };

        if (std::filesystem::is_regular_file(hint, ec))
        {
            const std::string n = p.filename().string();
            if (n.size() > 8 && n.compare(n.size() - 8, 8, ".app.bin") == 0) return takeApp(hint);
            if (p.extension() == ".uf2") return takeUf2(hint);
            // An esptool bundle carries the application inside it, at the offset its partition table
            // names -- so it is unwrapped, exactly as a .uf2 is, rather than refused.
            // The facts file says exactly where and how long -- no partition table to walk.
            ImageFacts fx;
            if (readImageFacts(hint, fx) && fx.appLength > 0)
            {
                std::vector<uint8_t> whole;
                if (readWholeFile(hint, whole) &&
                    (size_t)fx.appOffset + fx.appLength <= whole.size())
                {
                    out.assign(whole.begin() + fx.appOffset, whole.begin() + fx.appOffset + fx.appLength);
                    used = hint;
                    return true;
                }
            }
            std::string w2;
            if (espAppImageFromFactory(hint, out, w2)) { used = hint; return true; }
            // Last chance: the raw image may be lying next to it under its proper name.
            const std::string sib = siblingAppImage(hint);
            if (!sib.empty()) return takeApp(sib);
            why = w2.empty() ? "this file does not carry the raw image" : w2;
            return false;
        }

        if (!std::filesystem::is_directory(hint, ec)) { why = "no such file or folder"; return false; }

        // A folder: the raw image first, the .uf2 as the fallback that unwraps to the same thing.
        std::string app, uf2, fact;
        for (std::filesystem::recursive_directory_iterator it(hint, ec), end; it != end; it.increment(ec))
        {
            if (ec) { ec.clear(); continue; }
            if (it.depth() >= 3) it.disable_recursion_pending();
            if (!it->is_regular_file(ec)) continue;
            const std::string n = it->path().filename().string();
            if (app.empty() && n.size() > 8 && n.compare(n.size() - 8, 8, ".app.bin") == 0) app = it->path().string();
            else if (uf2.empty() && it->path().extension() == ".uf2") uf2 = it->path().string();
            else if (fact.empty() && n.size() > 12 && n.compare(n.size() - 12, 12, ".factory.bin") == 0)
                fact = it->path().string();
            if (!app.empty()) break; // the raw image beats the wrapper
        }
        if (!app.empty()) return takeApp(app);
        if (!uf2.empty()) return takeUf2(uf2);
        if (!fact.empty())
        {
            // Same order as for a named file: what the build wrote down beats what the partition table
            // implies. Walking the table happens to give the same length for a current build, but that
            // is the build's doing, not a guarantee -- and a base is checked against the exact length.
            ImageFacts fx;
            if (readImageFacts(fact, fx) && fx.appLength > 0)
            {
                std::vector<uint8_t> whole;
                if (readWholeFile(fact, whole) && (size_t)fx.appOffset + fx.appLength <= whole.size())
                {
                    out.assign(whole.begin() + fx.appOffset, whole.begin() + fx.appOffset + fx.appLength);
                    used = fact;
                    return true;
                }
            }
            std::string w2;
            if (espAppImageFromFactory(fact, out, w2)) { used = fact; return true; }
            why = w2;
            return false;
        }
        why = "no release image in this folder";
        return false;
    }

    /**
     * @brief The application image of a previous release, from a folder or a direct path.
     * @details Kept for callers that only need a path; loadBaseImage() is what knxOTA uses, because it
     *          also unwraps a .uf2, which a path alone cannot express.
     */
    inline std::string resolveBaseImage(const std::string& hint)
    {
        if (hint.empty()) return "";
        std::error_code ec;
        if (std::filesystem::is_regular_file(hint, ec))
        {
            const std::string n = std::filesystem::path(hint).filename().string();
            if (n.size() > 8 && n.compare(n.size() - 8, 8, ".app.bin") == 0) return hint;
            return siblingAppImage(hint);
        }
        if (!std::filesystem::is_directory(hint, ec)) return "";
        for (const auto& e : std::filesystem::recursive_directory_iterator(hint, ec))
        {
            if (!e.is_regular_file(ec)) continue;
            const std::string n = e.path().filename().string();
            if (n.size() > 8 && n.compare(n.size() - 8, 8, ".app.bin") == 0) return e.path().string();
        }
        return "";
    }

    /** Answers of cmd 106, mirroring FileTransferModule::cmdFwProbe. */
    enum class BaseAnswer : uint8_t
    {
        Match = 0x00,     ///< the device runs exactly this image
        Computing = 0x02, ///< checksum in progress, ask again
        Busy = 0x03,      ///< an update is already being applied
        Failed = 0x05,    ///< the device's last update failed; arg carries the reason
        NoMatch = 0x42,   ///< a different image
        OutOfRange = 0x4B ///< the length is outside what a patch may read
    };

    namespace detail
    {
        constexpr uint8_t FTC_CMD_FW_PROBE = 106;
        constexpr uint32_t PROBE_TIMEOUT_MS = 30000; ///< a whole-image checksum, not a round trip

        inline volatile bool g_probeSeen = false;
        inline uint8_t g_probeStatus = 0xFF;
        inline uint32_t g_probeArg = 0;
        inline uint16_t g_probePa = 0;

        inline void probeAnswer(uint16_t pa, uint8_t objectIndex, uint8_t propertyId, uint8_t* data, uint8_t length)
        {
            if (pa != g_probePa || objectIndex != FTC_OBJ_DATA || propertyId != FTC_CMD_FW_PROBE) return;
            if (length < 1 || data == nullptr) return;
            g_probeStatus = data[0];
            g_probeArg = (length >= 5) ? ((uint32_t)data[1] | ((uint32_t)data[2] << 8) |
                                          ((uint32_t)data[3] << 16) | ((uint32_t)data[4] << 24))
                                       : 0;
            g_probeSeen = true; // published last: the reader polls this flag
        }
    } // namespace detail

    /** @brief What a delta job on the device is doing right now. */
    enum class JobState : uint8_t
    {
        None,    ///< no job -- the device answered the base-check path instead
        Running, ///< still rebuilding; `produced` says how far
        Failed,  ///< it stopped; `error` is a FirmwarePatch::Error, reported exactly once
        NoAnswer ///< the device said nothing at all
    };

    /** @brief The FirmwarePatch::Error values, in enum order, as one line each. */
    inline const char* deltaErrorText(const I18n& L, uint32_t e)
    {
        switch (e)
        {
            case 1: return L.tr("not a patch file", "keine Patch-Datei");
            case 2: return L.tr("a newer patch format than this device understands",
                                "ein neueres Patch-Format, als dieses Gerät versteht");
            case 3: return L.tr("the patch sets a flag this device does not define",
                                "der Patch setzt ein Kennzeichen, das dieses Gerät nicht kennt");
            case 4: return L.tr("the patch header is damaged", "der Patch-Kopf ist beschädigt");
            case 5: return L.tr("the patch header lengths do not add up",
                                "die Längen im Patch-Kopf passen nicht zusammen");
            case 6: return L.tr("the patch would read past the allowed source region",
                                "der Patch würde über den erlaubten Quellbereich hinaus lesen");
            case 7: return L.tr("the device is NOT running the image this patch was built against",
                                "das Gerät fährt NICHT das Image, gegen das dieser Patch gebaut wurde");
            case 8: return L.tr("a length field in the patch is malformed",
                                "ein Längenfeld im Patch ist fehlerhaft");
            case 9: return L.tr("the patch contains an unknown instruction",
                                "der Patch enthält eine unbekannte Anweisung");
            case 10: return L.tr("the patch contains a zero-length instruction",
                                 "der Patch enthält eine Anweisung der Länge null");
            case 11: return L.tr("the patch copies from past the end of the source",
                                 "der Patch kopiert von hinter dem Ende der Quelle");
            case 12: return L.tr("the patch wants more literal data than it carries",
                                 "der Patch will mehr Literaldaten, als er mitbringt");
            case 13: return L.tr("the patch would write past the end of the target",
                                 "der Patch würde über das Ende des Ziels hinaus schreiben");
            case 14: return L.tr("the patch ended before the image was complete",
                                 "der Patch endete, bevor das Image vollständig war");
            case 15: return L.tr("the image was complete but patch data was left over",
                                 "das Image war vollständig, aber es blieb Patch-Material übrig");
            case 16: return L.tr("the rebuilt image does not match its checksum",
                                 "das zusammengesetzte Image passt nicht zu seiner Prüfsumme");
            case 17: return L.tr("the device could not read the patch or its own image",
                                 "das Gerät konnte den Patch oder sein eigenes Image nicht lesen");
            case 18: return L.tr("the device could not write the rebuilt image",
                                 "das Gerät konnte das zusammengesetzte Image nicht schreiben");
            // 0x80.. : the update never got as far as a patch. Same field, same question.
            case 0x80: return L.tr("the staged file is not there or cannot be read",
                                   "die abgelegte Datei ist nicht da oder nicht lesbar");
            case 0x81: return L.tr("the staged file is not a bootable image",
                                   "die abgelegte Datei ist kein startfähiges Image");
            case 0x82: return L.tr("this firmware is built for a different chip than this device",
                                   "diese Firmware ist für einen anderen Chip gebaut als dieses Gerät");
            case 0x83: return L.tr("this device has no second OTA slot - update over USB",
                                   "dieses Gerät hat keinen zweiten OTA-Platz - Update über USB");
            case 0x84: return L.tr("the device could not arm its flash",
                                   "das Gerät konnte seinen Flash nicht scharf schalten");
            case 0x85: return L.tr("writing the firmware failed", "das Schreiben der Firmware schlug fehl");
            case 0x86: return L.tr("the compressed firmware is damaged",
                                   "die komprimierte Firmware ist beschädigt");
            default: return L.tr("an unnamed error", "ein unbenannter Fehler");
        }
    }

    /**
     * @brief Ask what the delta job is doing, without starting a base check.
     * @details A ZERO-length FwProbe is the status-only form: the device answers the busy and the failed
     *          case before it ever looks at the payload, and falls through to 0x4B when there is no job.
     *          That is what makes it safe to poll while an update is being installed.
     */
    inline JobState probeDeltaJob(KnxIpTunnel& tunnel, uint16_t pa, uint32_t& argOut,
                                  const std::function<void()>& pump, const std::function<uint64_t()>& nowMs,
                                  uint32_t timeoutMs = 3000)
    {
        argOut = 0;
        const FtcResponseCb prev = tunnel.responseCallback();
        tunnel.setResponseCallback(&detail::probeAnswer);
        detail::g_probePa = pa;
        detail::g_probeSeen = false;
        detail::g_probeStatus = 0xFF;

        JobState out = JobState::NoAnswer;
        if (tunnel.sendCommand(pa, detail::FTC_OBJ_DATA, detail::FTC_CMD_FW_PROBE, nullptr, 0))
        {
            const uint64_t until = nowMs() + timeoutMs;
            while (!detail::g_probeSeen && nowMs() < until)
                pump();
            if (detail::g_probeSeen)
            {
                argOut = detail::g_probeArg;
                if (detail::g_probeStatus == 0x03) out = JobState::Running;
                else if (detail::g_probeStatus == 0x05) out = JobState::Failed;
                else out = JobState::None;
            }
        }
        tunnel.setResponseCallback(prev);
        return out;
    }

    /**
     * @brief Is the device running the image described by (len, crc)?
     * @param argOut  what the answer carries: staging room on a match, the reason on a failure.
     * @return false only when the device never answered at all — an old server without cmd 106.
     */
    inline bool probeBase(KnxIpTunnel& tunnel, uint16_t pa, uint32_t len, uint32_t crc,
                          BaseAnswer& answerOut, uint32_t& argOut,
                          const std::function<void()>& pump, const std::function<uint64_t()>& nowMs,
                          uint32_t timeoutMs = detail::PROBE_TIMEOUT_MS)
    {
        uint8_t payload[8] = {
            (uint8_t)(len & 0xFF), (uint8_t)((len >> 8) & 0xFF), (uint8_t)((len >> 16) & 0xFF), (uint8_t)((len >> 24) & 0xFF),
            (uint8_t)(crc & 0xFF), (uint8_t)((crc >> 8) & 0xFF), (uint8_t)((crc >> 16) & 0xFF), (uint8_t)((crc >> 24) & 0xFF)};

        const FtcResponseCb prev = tunnel.responseCallback();
        tunnel.setResponseCallback(&detail::probeAnswer);
        detail::g_probePa = pa;

        const uint64_t until = nowMs() + timeoutMs; // one deadline for the whole exchange, never re-armed
        bool answered = false;
        while (nowMs() < until)
        {
            detail::g_probeSeen = false;
            detail::g_probeStatus = 0xFF;
            if (!tunnel.sendCommand(pa, detail::FTC_OBJ_DATA, detail::FTC_CMD_FW_PROBE, payload, sizeof(payload)))
                break;

            const uint64_t roundTrip = nowMs() + 3000;
            while (!detail::g_probeSeen && nowMs() < roundTrip && nowMs() < until)
                pump();
            if (!detail::g_probeSeen) continue; // lost frame: ask again while the deadline allows

            answered = true;
            if (detail::g_probeStatus != (uint8_t)BaseAnswer::Computing) break;
            // Still working. Give it a moment rather than hammering a device that is busy checksumming.
            const uint64_t pause = nowMs() + 400;
            while (nowMs() < pause && nowMs() < until)
                pump();
        }

        tunnel.setResponseCallback(prev);
        if (!answered) return false;
        answerOut = (BaseAnswer)detail::g_probeStatus;
        argOut = detail::g_probeArg;
        // A device still computing when the deadline ran out is not a match; saying so is safer than
        // leaving the caller to guess from a stale status.
        if (answerOut == BaseAnswer::Computing) answerOut = BaseAnswer::NoMatch;
        return true;
    }
} // namespace ftc

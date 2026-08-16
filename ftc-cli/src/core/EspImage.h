/**
 * @file        EspImage.h
 * @brief       ESP32 application-image reader: chip family, integrity markers, OpenKNX identity. CORE.
 * @details     An ESP32 target ships a .bin, not a .uf2, so knxOTA identifies it from the image itself:
 *              the header names the chip (so a wrong-silicon flash is caught before anything is written),
 *              and esp_app_desc_t confirms it really is an application image rather than a bootloader or a
 *              factory bundle. The four OpenKNX identity bytes are read from the app descriptor's reserved
 *              field — a stamp OGM-Common's build can write the same way patch_uf2.py does for RP; until it
 *              does, `id.valid` stays false and the caller must not claim a version direction.
 * @date        2026-08-11
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "Uf2.h" // Uf2Identity — the same four bytes mean the same thing on both families

namespace ftc
{

/** @brief What an ESP32 application image states about itself. */
struct EspImage
{
    bool ok = false;
    std::string error;
    uint16_t chipId = 0xFFFF; ///< esp_image_header_t.chip_id
    uint8_t segments = 0;
    bool hashAppended = false; ///< a SHA-256 of the image is appended
    bool checksumChecked = false; ///< the trailing XOR byte was reachable and compared
    bool checksumOk = false;      ///< ...and it matched
    uint32_t size = 0;
    Uf2Identity id; ///< from the app descriptor's reserved field (absent until the build stamps it)
};

namespace detail
{

constexpr uint8_t ESP_IMAGE_MAGIC = 0xE9;
constexpr uint32_t ESP_APP_DESC_MAGIC = 0xABCD5432u;
constexpr uint32_t ESP_HDR_LEN = 24u;      // esp_image_header_t
constexpr uint32_t ESP_APP_DESC_OFF = 32u; // header + the first segment header
constexpr uint32_t ESP_STAMP_OFF = 40u;    // esp_app_desc_t.reserv1[2] — 8 unused bytes
constexpr uint8_t ESP_CHECKSUM_SEED = 0xEF;

} // namespace detail

/** @brief Human name for an ESP chip id (nullptr-safe, never returns null). */
inline const char* espChipName(uint16_t chipId)
{
    switch (chipId)
    {
        case 0x0000: return "ESP32";
        case 0x0002: return "ESP32-S2";
        case 0x0005: return "ESP32-C3";
        case 0x0009: return "ESP32-S3";
        case 0x000C: return "ESP32-C2";
        case 0x000D: return "ESP32-C6";
        case 0x0010: return "ESP32-H2";
        default: return "unknown ESP chip";
    }
}

/**
 * @brief Read an ESP32 application image.
 * @details Walks the segment table to reach the trailing XOR checksum — that walk is the only place the
 *          file's own numbers drive an offset, so every step is bounded against the real file size before
 *          it is taken. A failed integrity check is reported, not thrown: the caller decides.
 */
inline bool parseEspImage(const std::string& path, EspImage& out)
{
    out = EspImage();

    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr)
    {
        out.error = "cannot open the file";
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long fsz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (fsz < (long)(detail::ESP_APP_DESC_OFF + 16))
    {
        std::fclose(f);
        out.error = "the file is too small to be an ESP32 firmware";
        return false;
    }
    if (fsz > 32L * 1024 * 1024)
    {
        std::fclose(f);
        out.error = "the file is implausibly large for a firmware image";
        return false;
    }
    std::vector<uint8_t> d((size_t)fsz);
    const size_t rd = std::fread(d.data(), 1, d.size(), f);
    std::fclose(f);
    if (rd != d.size())
    {
        out.error = "the file could not be read completely";
        return false;
    }
    out.size = (uint32_t)d.size();

    if (d[0] != detail::ESP_IMAGE_MAGIC)
    {
        out.error = "this is not an ESP32 firmware file";
        return false;
    }
    out.segments = d[1];
    out.chipId = (uint16_t)(d[12] | (d[13] << 8));
    out.hashAppended = (d[23] == 1);

    if (detail::le32(d.data() + detail::ESP_APP_DESC_OFF) != detail::ESP_APP_DESC_MAGIC)
    {
        out.error = "this ESP32 file is not an application image (maybe a bootloader or a factory bundle)";
        return false;
    }

    // OpenKNX identity stamp — four bytes in the app descriptor's reserved field. All-zero = not stamped.
    const uint8_t* s = d.data() + detail::ESP_STAMP_OFF;
    if (s[0] != 0 || s[1] != 0 || s[2] != 0 || s[3] != 0)
    {
        out.id.openKnxId = s[0];
        out.id.appNumber = s[1];
        out.id.appVersion = s[2];
        out.id.revision = s[3];
        out.id.valid = true;
    }

    // Segment walk -> the XOR checksum byte that sits just before the optional SHA-256.
    uint64_t off = detail::ESP_HDR_LEN;
    uint8_t xorSum = detail::ESP_CHECKSUM_SEED;
    for (uint8_t i = 0; i < out.segments; ++i)
    {
        if (off + 8 > d.size())
        {
            out.error = "the segment table runs past the end of the file";
            return false;
        }
        const uint32_t len = detail::le32(d.data() + off + 4);
        off += 8;
        if (len > d.size() || off + len > d.size())
        {
            out.error = "a segment declares more data than the file holds";
            return false;
        }
        for (uint32_t k = 0; k < len; ++k)
            xorSum ^= d[(size_t)off + k];
        off += len;
    }
    const uint64_t csOff = off + (15u - (off % 16u)); // pad to 16, the last padded byte is the checksum
    if (csOff < d.size())
    {
        out.checksumChecked = true;
        out.checksumOk = (d[(size_t)csOff] == xorSum);
    }

    out.ok = true;
    return true;
}

} // namespace ftc

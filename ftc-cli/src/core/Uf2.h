/**
 * @file        Uf2.h
 * @brief       UF2 firmware reader: flat-image extraction + the OpenKNX identity tag. CLI-agnostic CORE.
 * @details     knxOTA must never hand a UF2 to the target — the RP bootloader writes the staged file
 *              verbatim to XIP_BASE, so the 512-byte block framing has to be stripped first. This parser
 *              rebuilds the flat image and reads the "KNX" extension tag that OGM-Common's patch_uf2.py
 *              stamps into block 0 (OpenKNX id, application number, version, revision) — the only place a
 *              firmware file states which device it belongs to. Every length and offset comes off untrusted
 *              bytes, so each one is range-checked before use.
 * @date        2026-08-11
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace ftc
{

/** @brief RP2040/RP2350 UF2 family ids (the only families an OpenKNX firmware image can carry). */
enum : uint32_t
{
    UF2_FAMILY_RP2040 = 0xE48BFF56u,
    UF2_FAMILY_RP2XXX_ABSOLUTE = 0xE48BFF57u,
    UF2_FAMILY_RP2XXX_DATA = 0xE48BFF58u,
    UF2_FAMILY_RP2350_ARM_S = 0xE48BFF59u,
    UF2_FAMILY_RP2350_RISCV = 0xE48BFF5Au,
    UF2_FAMILY_RP2350_ARM_NS = 0xE48BFF5Bu,
};

/** @brief The four bytes OGM-Common's patch_uf2.py writes into the block-0 "KNX" tag. */
struct Uf2Identity
{
    bool valid = false;
    uint8_t openKnxId = 0;  ///< MAIN_OpenKnxId
    uint8_t appNumber = 0;  ///< MAIN_ApplicationNumber
    uint8_t appVersion = 0; ///< MAIN_ApplicationVersion, packed 0xMN = major.minor
    uint8_t revision = 0;   ///< firmwareRevision from the product's main.cpp

    uint8_t major() const { return (uint8_t)(appVersion >> 4); }
    uint8_t minor() const { return (uint8_t)(appVersion & 0x0F); }
};

/** @brief The result of reading a .uf2: the flat image plus everything the file states about itself. */
struct Uf2Image
{
    bool ok = false;
    std::string error;          ///< empty on success; a finished sentence on failure
    std::vector<uint8_t> bin;   ///< flat image, starting at baseAddr
    uint32_t baseAddr = 0;      ///< target address of the first accepted block (must be XIP_BASE for RP flash)
    uint32_t familyId = 0;      ///< 0 = the file carried no family id
    uint32_t blocksUsed = 0;    ///< blocks that contributed to `bin`
    uint32_t blocksSkipped = 0; ///< NOT_MAIN_FLASH / FILE_CONTAINER blocks, legitimately ignored
    Uf2Identity id;
};

namespace detail
{

constexpr uint32_t UF2_MAGIC_0 = 0x0A324655u; // "UF2\n"
constexpr uint32_t UF2_MAGIC_1 = 0x9E5D5157u;
constexpr uint32_t UF2_MAGIC_END = 0x0AB16F30u;
constexpr uint32_t UF2_BLOCK = 512u;
constexpr uint32_t UF2_DATA_OFF = 32u;  // header size
constexpr uint32_t UF2_DATA_MAX = 476u; // 508 - 32; the last 4 bytes are magicEnd
constexpr uint32_t UF2_MAX_BLOCKS = 262144u; // 128 MB cap — reject before allocating

constexpr uint32_t UF2_FLAG_NOT_MAIN_FLASH = 0x00000001u;
constexpr uint32_t UF2_FLAG_FILE_CONTAINER = 0x00001000u;
constexpr uint32_t UF2_FLAG_FAMILY_ID = 0x00002000u;
constexpr uint32_t UF2_FLAG_EXT_TAGS = 0x00008000u;

constexpr uint32_t KNX_TAG_TYPE = 0x584E4Bu; // 'K','N','X' little-endian in the 3 type bytes
constexpr uint32_t XIP_BASE = 0x10000000u;   // where PicoOTA writes a staged image by default

inline uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/**
 * @brief Walk the extension tags of block 0 and pick out the OpenKNX "KNX" tag.
 * @details Tags live between the payload and magicEnd. A malformed walk stops the walk, it does not fail
 *          the file — an unreadable tag only means "identity unknown", which the caller handles.
 */
inline void readKnxTag(const uint8_t* blk, uint32_t payloadSize, Uf2Identity& out)
{
    uint32_t off = payloadSize;
    for (int guard = 0; guard < 64; ++guard) // bounded: a corrupt size must not spin forever
    {
        if (off + 4 > UF2_DATA_MAX) return;
        const uint32_t size = blk[UF2_DATA_OFF + off]; // total tag size incl. its 4-byte header
        if (size == 0) return;                         // terminator
        if (size < 4 || off + size > UF2_DATA_MAX) return; // malformed -> stop, keep what we have
        const uint32_t type = (uint32_t)blk[UF2_DATA_OFF + off + 1] |
                              ((uint32_t)blk[UF2_DATA_OFF + off + 2] << 8) |
                              ((uint32_t)blk[UF2_DATA_OFF + off + 3] << 16);
        if (type == KNX_TAG_TYPE && size >= 8)
        {
            const uint8_t* d = blk + UF2_DATA_OFF + off + 4;
            out.openKnxId = d[0];
            out.appNumber = d[1];
            out.appVersion = d[2];
            out.revision = d[3];
            out.valid = true;
            return;
        }
        const uint32_t next = off + ((size + 3u) & ~3u); // tags are 4-byte aligned
        if (next <= off) return;                          // no progress -> bail
        off = next;
    }
}

} // namespace detail

/** @brief Human name for a UF2 family id ("unknown family" when it is not an RP one). */
inline const char* uf2FamilyName(uint32_t familyId)
{
    switch (familyId)
    {
        case UF2_FAMILY_RP2040: return "RP2040";
        case UF2_FAMILY_RP2350_ARM_S: return "RP2350";
        case UF2_FAMILY_RP2350_RISCV: return "RP2350 RISC-V";
        case UF2_FAMILY_RP2350_ARM_NS: return "RP2350 non-secure";
        case UF2_FAMILY_RP2XXX_ABSOLUTE: return "RP2xxx (absolute)";
        case UF2_FAMILY_RP2XXX_DATA: return "RP2xxx (data)";
        default: return "unknown family";
    }
}

/** @brief True for a family this tool may flash to a device's main flash. */
inline bool uf2IsRpFlashFamily(uint32_t familyId)
{
    return familyId == UF2_FAMILY_RP2040 || familyId == UF2_FAMILY_RP2350_ARM_S ||
           familyId == UF2_FAMILY_RP2350_RISCV || familyId == UF2_FAMILY_RP2350_ARM_NS;
}

/**
 * @brief Read a .uf2 into a flat image + its OpenKNX identity.
 * @details Two passes: the first validates every block header and computes the extent, the second copies.
 *          The image must be contiguous and start at XIP_BASE — a gap or a foreign base cannot be expressed
 *          as one staged file and would be written to the wrong place, so both are refused rather than
 *          silently padded. Returns false with `out.error` set; `out.error` is user-facing.
 */
inline bool parseUf2(const std::string& path, Uf2Image& out)
{
    out = Uf2Image();

    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr)
    {
        out.error = "cannot open the file";
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long fsz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (fsz <= 0 || (uint64_t)fsz % detail::UF2_BLOCK != 0)
    {
        std::fclose(f);
        out.error = "not a UF2 file (its size is not a multiple of 512 bytes)";
        return false;
    }
    const uint32_t nBlocks = (uint32_t)((uint64_t)fsz / detail::UF2_BLOCK);
    if (nBlocks > detail::UF2_MAX_BLOCKS)
    {
        std::fclose(f);
        out.error = "the file is implausibly large for a firmware image";
        return false;
    }

    std::vector<uint8_t> raw((size_t)fsz);
    const size_t rd = std::fread(raw.data(), 1, raw.size(), f);
    std::fclose(f);
    if (rd != raw.size())
    {
        out.error = "the file could not be read completely";
        return false;
    }

    // --- pass 1: validate every block, establish the family, base address and extent ---
    bool haveFirst = false;
    uint32_t expectAddr = 0, lastEnd = 0, declaredBlocks = 0;
    for (uint32_t i = 0; i < nBlocks; ++i)
    {
        const uint8_t* b = raw.data() + (size_t)i * detail::UF2_BLOCK;
        if (detail::le32(b) != detail::UF2_MAGIC_0 || detail::le32(b + 4) != detail::UF2_MAGIC_1 ||
            detail::le32(b + 508) != detail::UF2_MAGIC_END)
        {
            char m[96];
            std::snprintf(m, sizeof(m), "block %u is damaged (wrong UF2 marker)", (unsigned)i);
            out.error = m;
            return false;
        }
        const uint32_t flags = detail::le32(b + 8);
        const uint32_t addr = detail::le32(b + 12);
        const uint32_t payload = detail::le32(b + 16);
        const uint32_t blockNo = detail::le32(b + 20);
        const uint32_t numBlocks = detail::le32(b + 24);
        const uint32_t info = detail::le32(b + 28);

        if (payload == 0 || payload > detail::UF2_DATA_MAX)
        {
            char m[96];
            std::snprintf(m, sizeof(m), "block %u declares an impossible payload size", (unsigned)i);
            out.error = m;
            return false;
        }
        if (numBlocks == 0 || blockNo >= numBlocks)
        {
            char m[96];
            std::snprintf(m, sizeof(m), "block %u has an inconsistent block count", (unsigned)i);
            out.error = m;
            return false;
        }
        if (declaredBlocks == 0) declaredBlocks = numBlocks;

        if ((flags & (detail::UF2_FLAG_NOT_MAIN_FLASH | detail::UF2_FLAG_FILE_CONTAINER)) != 0)
        {
            out.blocksSkipped++;
            continue; // legitimately not part of the flash image
        }

        if ((flags & detail::UF2_FLAG_FAMILY_ID) != 0)
        {
            if (out.familyId == 0) out.familyId = info;
            else if (out.familyId != info)
            {
                out.blocksSkipped++; // a multi-family UF2: keep the first family, skip the rest
                continue;
            }
        }

        if (!haveFirst)
        {
            out.baseAddr = addr;
            haveFirst = true;
            if ((flags & detail::UF2_FLAG_EXT_TAGS) != 0) detail::readKnxTag(b, payload, out.id);
        }
        else if (addr != expectAddr)
        {
            char m[128];
            std::snprintf(m, sizeof(m), "the image is not contiguous (a gap or overlap at block %u)", (unsigned)i);
            out.error = m;
            return false;
        }
        expectAddr = addr + payload;
        lastEnd = expectAddr;
        out.blocksUsed++;
    }

    if (!haveFirst || out.blocksUsed == 0)
    {
        out.error = "the file contains no flashable firmware";
        return false;
    }
    if (out.baseAddr != detail::XIP_BASE)
    {
        char m[128];
        std::snprintf(m, sizeof(m), "this image starts at 0x%08X, not at the firmware start 0x10000000",
                      (unsigned)out.baseAddr);
        out.error = m;
        return false;
    }
    const uint64_t span = (uint64_t)lastEnd - out.baseAddr;
    if (span == 0 || span > 16u * 1024u * 1024u)
    {
        out.error = "the image size is implausible";
        return false;
    }

    // --- pass 2: copy the payloads into one flat image ---
    out.bin.assign((size_t)span, 0);
    size_t at = 0;
    for (uint32_t i = 0; i < nBlocks; ++i)
    {
        const uint8_t* b = raw.data() + (size_t)i * detail::UF2_BLOCK;
        const uint32_t flags = detail::le32(b + 8);
        const uint32_t payload = detail::le32(b + 16);
        const uint32_t info = detail::le32(b + 28);
        if ((flags & (detail::UF2_FLAG_NOT_MAIN_FLASH | detail::UF2_FLAG_FILE_CONTAINER)) != 0) continue;
        if ((flags & detail::UF2_FLAG_FAMILY_ID) != 0 && out.familyId != 0 && info != out.familyId) continue;
        if (at + payload > out.bin.size()) // cannot happen after pass 1 — assert it anyway
        {
            out.error = "internal size mismatch while assembling the image";
            return false;
        }
        for (uint32_t k = 0; k < payload; ++k)
            out.bin[at + k] = b[detail::UF2_DATA_OFF + k];
        at += payload;
    }

    // Pass 1 and pass 2 decide family membership at different moments (pass 1 while familyId is still
    // being established, pass 2 against the final one). If they ever disagree, a block silently vanishes
    // and the tail stays zero-filled — a corrupt image that would otherwise be reported as good.
    if (at != out.bin.size())
    {
        out.error = "the firmware file is inconsistent (its blocks do not add up)";
        return false;
    }

    out.ok = true;
    return true;
}

} // namespace ftc

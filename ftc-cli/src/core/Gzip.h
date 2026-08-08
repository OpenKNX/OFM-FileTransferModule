/**
 * @file        Gzip.h
 * @brief       In-process gzip compression via vendored miniz (NO python, no shelling out). CLI-agnostic CORE.
 * @details     RP2040/ESP targets need gzip'd firmware (a raw .bin will not fit) — this produces a real gzip
 *              stream: a 10-byte gzip header + raw DEFLATE (miniz tdefl) + an 8-byte trailer (CRC32 + ISIZE,
 *              both little-endian), so the device's gzip loader accepts it. Header-only wrapper; miniz.c is
 *              compiled via build_src_filter.
 * @date        2026-08-03
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../third_party/miniz.h"

namespace ftc
{

/**
 * @brief Gzip a buffer into `out`. Returns false on a compression failure.
 */
inline bool gzipBuffer(const uint8_t* src, size_t n, std::vector<uint8_t>& out)
{
    // level 6, negative window bits => RAW deflate (no zlib header) — we add our own gzip framing.
    const mz_uint flags = (mz_uint)tdefl_create_comp_flags_from_zip_params(6, -15, MZ_DEFAULT_STRATEGY);
    size_t defLen = 0;
    void* def = tdefl_compress_mem_to_heap(src, n, &defLen, flags);
    if (def == nullptr) return false;

    const uint32_t crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, src, n);
    const uint32_t isize = (uint32_t)(n & 0xFFFFFFFFu);

    out.clear();
    out.reserve(defLen + 18);
    const uint8_t hdr[10] = {0x1f, 0x8b, 0x08, 0x00, 0, 0, 0, 0, 0x00, 0xff}; // magic, deflate, no flags, mtime 0, OS unknown
    out.insert(out.end(), hdr, hdr + 10);
    out.insert(out.end(), (const uint8_t*)def, (const uint8_t*)def + defLen);
    mz_free(def);
    for (int i = 0; i < 4; ++i)
        out.push_back((uint8_t)((crc >> (8 * i)) & 0xFF)); // CRC32, little-endian
    for (int i = 0; i < 4; ++i)
        out.push_back((uint8_t)((isize >> (8 * i)) & 0xFF)); // ISIZE, little-endian
    return true;
}

/**
 * @brief Gzip a file on disk. Returns false on any I/O or compression failure.
 */
inline bool gzipFile(const std::string& in, const std::string& out)
{
    std::ifstream f(in, std::ios::binary);
    if (!f) return false;
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<uint8_t> gz;
    if (!gzipBuffer(data.data(), data.size(), gz)) return false;
    std::ofstream o(out, std::ios::binary);
    if (!o) return false;
    o.write((const char*)gz.data(), (std::streamsize)gz.size());
    return (bool)o;
}

} // namespace ftc

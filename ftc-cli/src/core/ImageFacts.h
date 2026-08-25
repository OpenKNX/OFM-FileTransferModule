#pragma once

/**
 * @file        ImageFacts.h
 * @brief       What a firmware package does NOT say about itself, written down by the build that knew.
 *
 * A release ships a package -- a .uf2 wrapping the image in 256-byte blocks, or a .factory.bin with a
 * bootloader and a partition table in front of it. Both carry the application image, and neither states
 * two things a tool needs: where it starts, and where it ENDS.
 *
 * The end is the sharp one. A .uf2 is padded to a whole block, so unwrapping it yields up to 255 bytes
 * too many -- indistinguishable from image data. A firmware difference is checked against the exact
 * length, so those few bytes are the difference between a base the device confirms and one it rejects.
 *
 * So the build writes <name>.image.txt next to the package and verifies the slice it describes before
 * doing so. Everything here just reads it. Absent -- an older release -- the caller falls back to
 * unwrapping and says the length is not certain, rather than pretending it is.
 * @date        2026-08-22
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 */

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

namespace ftc
{
    struct ImageFacts
    {
        bool valid = false;
        std::string format;    ///< uf2 | factory | raw
        std::string package;   ///< the file these facts belong to
        uint32_t appOffset = 0; ///< where the application image starts inside the package
        uint32_t appLength = 0; ///< ...and how long it is. The one thing a .uf2 cannot say.
        std::string sha256;     ///< of the application image, for checking an unwrap
    };

    /** @brief The facts file beside a package, if the build wrote one. */
    inline bool readImageFacts(const std::string& packagePath, ImageFacts& out)
    {
        out = ImageFacts();
        std::error_code ec;
        std::filesystem::path p(packagePath);
        std::string stem = p.filename().string();
        // The stem is the name without the packaging suffix, and .factory.bin is two extensions deep.
        if (stem.size() > 12 && stem.compare(stem.size() - 12, 12, ".factory.bin") == 0)
            stem = stem.substr(0, stem.size() - 12);
        else if (stem.size() > 8 && stem.compare(stem.size() - 8, 8, ".app.bin") == 0)
            stem = stem.substr(0, stem.size() - 8);
        else if (p.has_extension())
            stem = p.stem().string();

        const std::filesystem::path f = p.parent_path() / (stem + ".image.txt");
        if (!std::filesystem::is_regular_file(f, ec)) return false;

        std::FILE* h = std::fopen(f.string().c_str(), "rb");
        if (h == nullptr) return false;
        char line[512];
        while (std::fgets(line, sizeof(line), h))
        {
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
            size_t b = 0;
            while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) ++b;
            s = s.substr(b);
            if (s.empty() || s[0] == '#') continue;
            const size_t eq = s.find('=');
            if (eq == std::string::npos || eq == 0) continue;
            std::string k = s.substr(0, eq), v = s.substr(eq + 1);
            while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
            size_t vb = 0;
            while (vb < v.size() && (v[vb] == ' ' || v[vb] == '\t')) ++vb;
            v = v.substr(vb);
            if (k == "format") out.format = v;
            else if (k == "package") out.package = v;
            else if (k == "appOffset") out.appOffset = (uint32_t)std::strtoul(v.c_str(), nullptr, 10);
            else if (k == "appLength") out.appLength = (uint32_t)std::strtoul(v.c_str(), nullptr, 10);
            else if (k == "appSha256") out.sha256 = v;
        }
        std::fclose(h);
        out.valid = (out.appLength > 0);
        return out.valid;
    }

    /**
     * @brief Cut an unwrapped payload to the length the build stated.
     * @details Only ever shortens. A payload that is SHORTER than stated means the unwrap went wrong,
     *          and silently accepting it would hand on an image that is not the one described.
     */
    template <typename Bytes>
    inline bool applyImageFacts(const ImageFacts& f, Bytes& payload)
    {
        if (!f.valid) return true;                       // nothing stated, nothing to apply
        if (payload.size() < f.appLength) return false;  // shorter than promised -> the unwrap is wrong
        if (payload.size() > f.appLength) payload.resize(f.appLength);
        return true;
    }
} // namespace ftc

#pragma once

/**
 * @file        Delta.h
 * @brief       Builds, inspects and replays an "OKD1" difference file on the host.
 *
 * The encoder is the only half of the scheme that never runs on a device: it needs the old and the
 * new image at once and a match finder with room to work. The decoder is not reimplemented here --
 * `apply()` drives the very source the firmware compiles (`src/FirmwarePatch.cpp`), so a round trip on the
 * host proves the device path and not a host-only lookalike.
 *
 * The encoder is reproducible: same inputs and same version produce the same bytes, with no
 * timestamp, no randomness and no container whose iteration order could leak into the output.
 * @date        2026-08-21
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../../src/FirmwarePatch.h"
#include "../third_party/miniz.h"

namespace ftc
{
    namespace delta
    {
        struct Stats
        {
            uint32_t copyOps = 0;
            uint32_t addOps = 0;
            uint32_t copyBytes = 0;
            uint32_t literalBytes = 0;
            uint32_t opsBytes = 0;
            uint32_t patchBytes = 0;
        };

        struct Info
        {
            uint8_t version = 0;
            uint8_t flags = 0;
            uint32_t srcLen = 0, srcCrc = 0;
            uint32_t dstLen = 0, dstCrc = 0;
            uint32_t opsLen = 0, litLen = 0;
            bool headerOk = false;
        };

        // Shortest match worth an opcode. Below this the operation costs more than the literals it
        // replaces; 12 was the best of 12/16/24 on real images.
        static const uint32_t MIN_MATCH = 12;
        // Candidates examined per position. Deeper finds slightly better matches for linearly more
        // time; 16 is where the curve flattens on a 1 MB image.
        static const uint32_t MAX_CHAIN = 16;

        inline bool readWholeFileInto(const std::string &path, std::vector<uint8_t> &out)
        {
            std::FILE *f = std::fopen(path.c_str(), "rb");
            if (f == nullptr) return false;
            std::fseek(f, 0, SEEK_END);
            const long n = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            if (n <= 0)
            {
                std::fclose(f);
                return false;
            }
            out.resize((size_t)n);
            const bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
            std::fclose(f);
            return ok;
        }

        inline uint32_t crc(const uint8_t *p, size_t n)
        {
            return FirmwarePatch::crcFinal(FirmwarePatch::crcUpdate(FirmwarePatch::CRC_INIT, p, (uint32_t)n));
        }

        inline void putVarint(std::vector<uint8_t> &out, uint32_t v)
        {
            while (v >= 0x80)
            {
                out.push_back((uint8_t)(v | 0x80));
                v >>= 7;
            }
            out.push_back((uint8_t)v);
        }

        inline void put32(std::vector<uint8_t> &out, uint32_t v)
        {
            out.push_back((uint8_t)(v & 0xFF));
            out.push_back((uint8_t)((v >> 8) & 0xFF));
            out.push_back((uint8_t)((v >> 16) & 0xFF));
            out.push_back((uint8_t)((v >> 24) & 0xFF));
        }

        inline uint32_t hash8(const uint8_t *p)
        {
            uint64_t v;
            memcpy(&v, p, 8);
            return (uint32_t)((v * 0x9E3779B97F4A7C15ull) >> 40);
        }

        /**
         * @brief Build a patch that turns `src` into `dst`.
         * @details Greedy match finder over a hash chain of the source, with backward extension into
         *          the literals already collected -- a match that starts a few bytes earlier turns
         *          those literals into part of the copy. Source offsets are stored as a signed delta
         *          to the end of the previous copy, which is what keeps the opcode stream small.
         */
        inline bool make(const std::vector<uint8_t> &src, const std::vector<uint8_t> &dst,
                         std::vector<uint8_t> &out, Stats *stats = nullptr)
        {
            out.clear();
            if (src.empty() || dst.empty()) return false;

            uint32_t bits = 12;
            while ((1u << bits) < (src.size() >> 1) && bits < 22) bits++;
            const uint32_t mask = (1u << bits) - 1;
            std::vector<int32_t> head(mask + 1, -1);
            std::vector<int32_t> chain(src.size(), -1);
            for (size_t i = 0; i + 8 <= src.size(); i++)
            {
                const uint32_t h = hash8(&src[i]) & mask;
                chain[i] = head[h];
                head[h] = (int32_t)i;
            }

            std::vector<uint8_t> ops, lit, pending;
            Stats st;
            size_t i = 0;
            int64_t lastEnd = 0;

            auto flushPending = [&]() {
                if (pending.empty()) return;
                ops.push_back(FirmwarePatch::OP_ADD);
                putVarint(ops, (uint32_t)pending.size());
                lit.insert(lit.end(), pending.begin(), pending.end());
                st.addOps++;
                st.literalBytes += (uint32_t)pending.size();
                pending.clear();
            };

            while (i < dst.size())
            {
                size_t bestLen = 0, bestOfs = 0, bestBack = 0;
                if (i + 8 <= dst.size())
                {
                    int32_t cand = head[hash8(&dst[i]) & mask];
                    for (uint32_t depth = 0; cand >= 0 && depth < MAX_CHAIN; depth++, cand = chain[cand])
                    {
                        const size_t s = (size_t)cand;
                        size_t fwd = 0;
                        while (s + fwd < src.size() && i + fwd < dst.size() && src[s + fwd] == dst[i + fwd]) fwd++;
                        size_t back = 0;
                        while (back < pending.size() && s > back && src[s - back - 1] == dst[i - back - 1]) back++;
                        if (fwd + back > bestLen + bestBack)
                        {
                            bestLen = fwd;
                            bestOfs = s;
                            bestBack = back;
                        }
                    }
                }

                if (bestLen > 0 && bestLen + bestBack >= MIN_MATCH)
                {
                    pending.resize(pending.size() - bestBack);
                    const size_t ofs = bestOfs - bestBack;
                    const size_t len = bestLen + bestBack;
                    flushPending();
                    const int64_t d = (int64_t)ofs - lastEnd;
                    ops.push_back(FirmwarePatch::OP_COPY);
                    putVarint(ops, (uint32_t)len);
                    putVarint(ops, (uint32_t)((d << 1) ^ (d >> 63))); // zigzag
                    lastEnd = (int64_t)(ofs + len);
                    st.copyOps++;
                    st.copyBytes += (uint32_t)len;
                    i += bestLen;
                }
                else
                {
                    pending.push_back(dst[i]);
                    i++;
                }
            }
            flushPending();

            out.reserve(FirmwarePatch::HDR_SIZE + ops.size() + lit.size());
            out.insert(out.end(), FirmwarePatch::MAGIC, FirmwarePatch::MAGIC + 4);
            out.push_back(FirmwarePatch::VERSION);
            out.push_back(0); // flags
            out.push_back(0);
            out.push_back(0);
            put32(out, (uint32_t)src.size());
            put32(out, crc(src.data(), src.size()));
            put32(out, (uint32_t)dst.size());
            put32(out, crc(dst.data(), dst.size()));
            put32(out, (uint32_t)ops.size());
            put32(out, (uint32_t)lit.size());
            put32(out, crc(out.data(), out.size())); // header checksum over the 32 bytes above
            out.insert(out.end(), ops.begin(), ops.end());
            out.insert(out.end(), lit.begin(), lit.end());

            st.opsBytes = (uint32_t)ops.size();
            st.patchBytes = (uint32_t)out.size();
            if (stats) *stats = st;
            return true;
        }

        /**
         * @brief Pack a patch: header stays readable, the two streams become one zlib stream.
         * @details The header is deliberately left in the clear. It carries which image the patch was
         *          built against, and a device that only forwards a patch must be able to read that
         *          without unpacking anything -- that is what lets one device update another.
         */
        inline bool pack(const std::vector<uint8_t> &plain, std::vector<uint8_t> &out)
        {
            if (plain.size() <= FirmwarePatch::HDR_SIZE) return false;
            out.assign(plain.begin(), plain.begin() + FirmwarePatch::HDR_SIZE);
            out[5] |= FirmwarePatch::FLAG_PACKED;
            const uint32_t hc = crc(out.data(), FirmwarePatch::HDR_SIZE - 4); // the flag byte is covered by it
            out[32] = (uint8_t)(hc & 0xFF);
            out[33] = (uint8_t)((hc >> 8) & 0xFF);
            out[34] = (uint8_t)((hc >> 16) & 0xFF);
            out[35] = (uint8_t)((hc >> 24) & 0xFF);

            const uint8_t *body = plain.data() + FirmwarePatch::HDR_SIZE;
            const size_t bodyLen = plain.size() - FirmwarePatch::HDR_SIZE;
            mz_ulong bound = mz_compressBound((mz_ulong)bodyLen);
            std::vector<uint8_t> packed(bound);
            if (mz_compress2(packed.data(), &bound, body, (mz_ulong)bodyLen, 9) != MZ_OK) return false;
            packed.resize(bound);
            out.insert(out.end(), packed.begin(), packed.end());
            return true;
        }

        /** @brief Undo pack(), so a packed patch can be inspected and replayed on the host too. */
        inline bool unpack(const std::vector<uint8_t> &packed, std::vector<uint8_t> &out)
        {
            if (packed.size() <= FirmwarePatch::HDR_SIZE) return false;
            out.assign(packed.begin(), packed.begin() + FirmwarePatch::HDR_SIZE);
            out[5] &= (uint8_t)~FirmwarePatch::FLAG_PACKED;
            const uint32_t hc = crc(out.data(), FirmwarePatch::HDR_SIZE - 4);
            out[32] = (uint8_t)(hc & 0xFF);
            out[33] = (uint8_t)((hc >> 8) & 0xFF);
            out[34] = (uint8_t)((hc >> 16) & 0xFF);
            out[35] = (uint8_t)((hc >> 24) & 0xFF);

            auto rd = [&](size_t o) {
                return (uint32_t)packed[o] | ((uint32_t)packed[o + 1] << 8) | ((uint32_t)packed[o + 2] << 16) |
                       ((uint32_t)packed[o + 3] << 24);
            };
            const size_t want = (size_t)rd(24) + (size_t)rd(28);
            std::vector<uint8_t> body(want);
            mz_ulong got = (mz_ulong)want;
            if (mz_uncompress(body.data(), &got, packed.data() + FirmwarePatch::HDR_SIZE,
                              (mz_ulong)(packed.size() - FirmwarePatch::HDR_SIZE)) != MZ_OK)
                return false;
            if (got != want) return false;
            out.insert(out.end(), body.begin(), body.end());
            return true;
        }

        inline bool describe(const std::vector<uint8_t> &patch, Info &info)
        {
            if (patch.size() < FirmwarePatch::HDR_SIZE) return false;
            auto rd = [&](size_t o) {
                return (uint32_t)patch[o] | ((uint32_t)patch[o + 1] << 8) | ((uint32_t)patch[o + 2] << 16) |
                       ((uint32_t)patch[o + 3] << 24);
            };
            if (memcmp(patch.data(), FirmwarePatch::MAGIC, 4) != 0) return false;
            info.version = patch[4];
            info.flags = patch[5];
            info.srcLen = rd(8);
            info.srcCrc = rd(12);
            info.dstLen = rd(16);
            info.dstCrc = rd(20);
            info.opsLen = rd(24);
            info.litLen = rd(28);
            info.headerOk = crc(patch.data(), FirmwarePatch::HDR_SIZE - 4) == rd(32);
            return true;
        }

        /** Buffer-backed callbacks, so the firmware interpreter runs unchanged on the host. */
        struct ApplyCtx
        {
            const std::vector<uint8_t> *src;
            const std::vector<uint8_t> *patch;
            std::vector<uint8_t> *out;
        };

        inline bool srcCb(void *ctx, uint8_t *dst, uint32_t ofs, uint32_t len)
        {
            auto *c = (ApplyCtx *)ctx;
            if ((size_t)ofs + len > c->src->size()) return false;
            memcpy(dst, c->src->data() + ofs, len);
            return true;
        }

        inline bool patchCb(void *ctx, uint8_t *dst, uint32_t ofs, uint32_t len)
        {
            auto *c = (ApplyCtx *)ctx;
            if ((size_t)ofs + len > c->patch->size()) return false;
            memcpy(dst, c->patch->data() + ofs, len);
            return true;
        }

        inline bool sinkCb(void *ctx, const uint8_t *buf, uint32_t len)
        {
            auto *c = (ApplyCtx *)ctx;
            c->out->insert(c->out->end(), buf, buf + len);
            return true;
        }

        /**
         * @brief Replay a patch against a source image, in the same slices a device would use.
         * @return true when the rebuilt image is complete and checksummed; `err` carries the reason otherwise.
         */
        inline bool apply(const std::vector<uint8_t> &src, const std::vector<uint8_t> &patch,
                          std::vector<uint8_t> &out, uint8_t &err, uint32_t srcLimit = 0xFFFFFFFFu,
                          uint32_t slice = 4096)
        {
            out.clear();
            // A packed patch is unpacked first -- exactly what the device does before its interpreter
            // ever sees the file.
            std::vector<uint8_t> plain;
            if (patch.size() > 5 && (patch[5] & FirmwarePatch::FLAG_PACKED) != 0)
            {
                if (!unpack(patch, plain)) { err = FirmwarePatch::ERR_TRUNCATED; return false; }
                return apply(src, plain, out, err, srcLimit, slice);
            }
            ApplyCtx ctx{&src, &patch, &out};
            FirmwarePatch::Io io;
            io.src = srcCb;
            io.patch = patchCb;
            io.sink = sinkCb;
            io.ctx = &ctx;

            FirmwarePatch::Job job;
            std::vector<uint8_t> scratch(slice);
            if (!job.begin(io, (uint32_t)patch.size(), srcLimit))
            {
                err = job.error();
                return false;
            }
            while (!job.done())
            {
                if (!job.step(scratch.data(), (uint32_t)scratch.size()))
                {
                    err = job.error();
                    return false;
                }
            }
            err = FirmwarePatch::ERR_NONE;
            return true;
        }
    } // namespace delta
} // namespace ftc

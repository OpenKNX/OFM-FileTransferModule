/**
 * @file        FirmwarePatch.cpp
 * @brief       Interpreter for the "OKD1" difference format.
 *
 * Every length taken from the file is checked against what is left, by subtraction rather than
 * addition, so no bound can be crossed by an overflow. A failed check ends the job; nothing is ever
 * written past the declared target length, and the caller learns the exact reason.
 * @date        2026-08-21
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 */

#include "FileTransferConfig.h" // switches first -- every guard below depends on it
#include "FirmwarePatch.h"

// Without the switch this file is a declaration-only translation unit: the interpreter is not merely
// dropped by the linker, it is never compiled.
#ifdef OPENKNX_FTC_DELTA_UPDATE

    #include <string.h>

namespace FirmwarePatch
{
    // CRC-32/POSIX. Bitwise on purpose: a table would cost flash for a job that runs once per
    // update, and the cost here is a fraction of the flash write it accompanies.
    uint32_t crcUpdate(uint32_t raw, const uint8_t *data, uint32_t len)
    {
        while (len--)
        {
            raw ^= (uint32_t)(*data++) << 24;
            for (uint8_t bit = 0; bit < 8; bit++)
                raw = (raw & 0x80000000u) ? (raw << 1) ^ 0x04C11DB7u : (raw << 1);
        }
        return raw;
    }

    uint32_t crcFinal(uint32_t raw)
    {
        return ~raw;
    }

    static uint32_t rd32(const uint8_t *p)
    {
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }

    bool Job::fail(uint8_t err)
    {
        _phase = PH_FAILED;
        _err = err;
        return false;
    }

    /**
     * @brief Read and validate the header; leave the job ready to verify the source.
     * @details Nothing beyond the header is touched here. The header checksum is verified before any
     *          length in it is believed, because every later bound is derived from those lengths.
     */
    bool Job::begin(const Io &io, uint32_t patchSize, uint32_t srcLimit)
    {
        _io = io;
        _phase = PH_IDLE;
        _err = ERR_NONE;
        _srcPos = _opsPos = _litPos = _dstPos = 0;
        _crcRaw = CRC_INIT;
        _opLeft = _opSrcOfs = _lastSrcEnd = 0;
        _opIsCopy = false;
        _opBufLen = _opBufPos = 0;

        if (io.src == nullptr || io.patch == nullptr || io.sink == nullptr) return fail(ERR_READ);
        if (patchSize < HDR_SIZE) return fail(ERR_SIZE);

        uint8_t hdr[HDR_SIZE];
        if (!_io.patch(_io.ctx, hdr, 0, HDR_SIZE)) return fail(ERR_READ);
        if (memcmp(hdr, MAGIC, sizeof(MAGIC)) != 0) return fail(ERR_MAGIC);
        if (hdr[4] != VERSION) return fail(ERR_VERSION);
        if (hdr[5] != 0 || hdr[6] != 0 || hdr[7] != 0) return fail(ERR_FLAGS); // no flag is defined yet
        if (crcFinal(crcUpdate(CRC_INIT, hdr, HDR_SIZE - 4)) != rd32(hdr + 32)) return fail(ERR_HEADER_CRC);

        _srcLen = rd32(hdr + 8);
        _srcCrc = rd32(hdr + 12);
        _dstLen = rd32(hdr + 16);
        _dstCrc = rd32(hdr + 20);
        _opsLen = rd32(hdr + 24);
        _litLen = rd32(hdr + 28);

        if (_srcLen == 0 || _dstLen == 0 || _opsLen == 0) return fail(ERR_SIZE);
        if (_srcLen > srcLimit) return fail(ERR_SRC_RANGE);
        // Subtraction only: the file size is known, so both stream lengths are checked against what
        // is left of it instead of adding them up.
        if (_opsLen > patchSize - HDR_SIZE) return fail(ERR_SIZE);
        if (_litLen != patchSize - HDR_SIZE - _opsLen) return fail(ERR_SIZE);

        _opsBase = HDR_SIZE;
        _litBase = HDR_SIZE + _opsLen;
        _phase = PH_SRC_CRC;
        return true;
    }

    bool Job::readOps(uint8_t *dst, uint32_t len)
    {
        if (len > _opsLen - _opsPos) return fail(ERR_TRUNCATED);
        if (!_io.patch(_io.ctx, dst, _opsBase + _opsPos, len)) return fail(ERR_READ);
        _opsPos += len;
        return true;
    }

    bool Job::nextByte(uint8_t &out)
    {
        if (_opBufPos == _opBufLen)
        {
            const uint32_t left = _opsLen - _opsPos;
            if (left == 0) return fail(ERR_TRUNCATED);
            const uint32_t want = (left < sizeof(_opBuf)) ? left : sizeof(_opBuf);
            if (!readOps(_opBuf, want)) return false;
            _opBufLen = (uint8_t)want;
            _opBufPos = 0;
        }
        out = _opBuf[_opBufPos++];
        return true;
    }

    /** @brief LEB128, capped at 32 bit. A fifth byte may only carry the top four bits. */
    bool Job::readVarint(uint32_t &out)
    {
        uint32_t value = 0;
        for (uint8_t shift = 0; shift <= 28; shift += 7)
        {
            uint8_t b = 0;
            if (!nextByte(b)) return false;
            if (shift == 28 && (b & 0x7F) > 0x0F) return fail(ERR_VARINT); // would exceed 32 bit
            value |= (uint32_t)(b & 0x7F) << shift;
            if ((b & 0x80) == 0)
            {
                out = value;
                return true;
            }
        }
        return fail(ERR_VARINT); // no terminator within five bytes
    }

    /** @brief Decode the next operation and set up how many bytes it owes. */
    bool Job::startNextOp()
    {
        uint8_t opcode = 0;
        if (!nextByte(opcode)) return false;

        uint32_t len = 0;
        if (!readVarint(len)) return false;
        if (len == 0) return fail(ERR_ZERO_LEN);
        if (len > _dstLen - _dstPos) return fail(ERR_DST_RANGE);

        if (opcode == OP_ADD)
        {
            if (len > _litLen - _litPos) return fail(ERR_LIT_RANGE);
            _opIsCopy = false;
        }
        else if (opcode == OP_COPY)
        {
            uint32_t zig = 0;
            if (!readVarint(zig)) return false;
            const int32_t delta = (int32_t)((zig >> 1) ^ (~(zig & 1) + 1)); // zigzag -> signed
            const int64_t ofs = (int64_t)_lastSrcEnd + (int64_t)delta;
            if (ofs < 0 || (uint64_t)ofs > _srcLen) return fail(ERR_COPY_RANGE);
            _opSrcOfs = (uint32_t)ofs;
            if (len > _srcLen - _opSrcOfs) return fail(ERR_COPY_RANGE);
            _lastSrcEnd = _opSrcOfs + len;
            _opIsCopy = true;
        }
        else
        {
            return fail(ERR_OPCODE);
        }

        _opLeft = len;
        return true;
    }

    bool Job::stepSrcCrc(uint8_t *scratch, uint32_t scratchLen)
    {
        const uint32_t left = _srcLen - _srcPos;
        const uint32_t want = (left < scratchLen) ? left : scratchLen;
        if (!_io.src(_io.ctx, scratch, _srcPos, want)) return fail(ERR_READ);
        _crcRaw = crcUpdate(_crcRaw, scratch, want);
        _srcPos += want;

        if (_srcPos < _srcLen) return true;
        if (crcFinal(_crcRaw) != _srcCrc) return fail(ERR_SRC_CRC);
        _crcRaw = CRC_INIT; // reused for the rebuilt image
        _phase = PH_OPS;
        return true;
    }

    bool Job::stepOps(uint8_t *scratch, uint32_t scratchLen)
    {
        uint32_t budget = scratchLen;
        while (budget > 0 && _dstPos < _dstLen)
        {
            if (_opLeft == 0 && !startNextOp()) return false;

            uint32_t chunk = (_opLeft < budget) ? _opLeft : budget;
            if (_opIsCopy)
            {
                if (!_io.src(_io.ctx, scratch, _opSrcOfs, chunk)) return fail(ERR_READ);
                _opSrcOfs += chunk;
            }
            else
            {
                if (!_io.patch(_io.ctx, scratch, _litBase + _litPos, chunk)) return fail(ERR_READ);
                _litPos += chunk;
            }

            if (!_io.sink(_io.ctx, scratch, chunk)) return fail(ERR_WRITE);
            _crcRaw = crcUpdate(_crcRaw, scratch, chunk);
            _dstPos += chunk;
            _opLeft -= chunk;
            budget -= chunk;
        }

        if (_dstPos < _dstLen) return true;

        // Complete: the target is full, so both streams must be exactly used up. Left-over data means
        // the patch describes something other than what was just built.
        if (_opLeft != 0) return fail(ERR_TRAILING);
        if (_opsPos - (_opBufLen - _opBufPos) != _opsLen) return fail(ERR_TRAILING);
        if (_litPos != _litLen) return fail(ERR_TRAILING);
        if (crcFinal(_crcRaw) != _dstCrc) return fail(ERR_DST_CRC);
        _phase = PH_DONE;
        return true;
    }

    /** @brief One slice of work, at most `scratchLen` bytes. */
    bool Job::step(uint8_t *scratch, uint32_t scratchLen)
    {
        if (_phase == PH_DONE) return true;
        if (_phase == PH_FAILED || scratch == nullptr || scratchLen == 0) return false;
        if (_phase == PH_SRC_CRC) return stepSrcCrc(scratch, scratchLen);
        if (_phase == PH_OPS) return stepOps(scratch, scratchLen);
        return fail(ERR_SIZE); // step() before begin()
    }
} // namespace FirmwarePatch

#endif // OPENKNX_FTC_DELTA_UPDATE

#pragma once

/**
 * @file        FirmwarePatch.h
 * @brief       Rebuilds a firmware image from the running one plus a difference file ("OKD1").
 *
 * The interpreter is platform-neutral and does no I/O of its own: the caller supplies three
 * callbacks (running image, patch file, target sink) and a scratch buffer. Work is handed out in
 * slices bounded by that buffer, so a caller on a device can drive it from loop() without ever
 * blocking longer than a single flash operation. The same source is compiled on the host, where the
 * round-trip against the encoder and the malformed-input cases are proven before any device runs it.
 * @date        2026-08-21
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 */

#include <stdint.h>
#include <stddef.h>

namespace FirmwarePatch
{
    /** Patch file layout. Header is little-endian; both streams follow it back to back. */
    static const uint32_t HDR_SIZE = 36;
    static const uint8_t VERSION = 1;
    static const uint8_t MAGIC[4] = {'O', 'K', 'D', '1'};

    /**
     * Flag bit 0: the two streams are stored as ONE zlib stream, and `opsLen`/`litLen` are their
     * unpacked lengths. The header itself always stays readable, which is what lets a device confirm
     * the base a patch expects without unpacking anything.
     *
     * The interpreter never sees a packed patch: whoever drives it unpacks first and hands it the plain
     * form. So this bit is a contract between the encoder and the caller, not something to handle here.
     */
    static const uint8_t FLAG_PACKED = 0x01;

    /** Opcodes. Exactly two -- anything else is a malformed patch. */
    enum Op : uint8_t
    {
        OP_ADD = 0x00,  // <varint n>                       : n bytes from the literal stream
        OP_COPY = 0x01, // <varint n> <zigzag varint delta>  : n bytes from the running image
    };

    /**
     * @brief Why a job stopped. Reported unchanged to the client, so every value stays meaningful
     *        on its own; never merge two causes into one code.
     */
    enum Error : uint8_t
    {
        ERR_NONE = 0,
        ERR_MAGIC,      // not a patch file
        ERR_VERSION,    // newer format than this device understands
        ERR_FLAGS,      // a flag bit is set that this version does not define
        ERR_HEADER_CRC, // header damaged -- every length below it is untrustworthy
        ERR_SIZE,       // header lengths do not add up to the file size
        ERR_SRC_RANGE,  // source would reach past the region the patch may read
        ERR_SRC_CRC,    // the device is not running the image this patch was built against
        ERR_VARINT,     // varint longer than 32 bit
        ERR_OPCODE,     // unknown opcode
        ERR_ZERO_LEN,   // zero-length operation -- would never terminate
        ERR_COPY_RANGE, // COPY reaches past the source
        ERR_LIT_RANGE,  // ADD wants more literals than the stream holds
        ERR_DST_RANGE,  // operation would write past the target length
        ERR_TRUNCATED,  // streams ended before the target was complete
        ERR_TRAILING,   // target complete but stream data left over
        ERR_DST_CRC,    // rebuilt image does not match the expected checksum
        ERR_READ,       // source or patch read failed
        ERR_WRITE,      // sink write failed
    };

    /** Callbacks. Each returns false on failure; the job then stops with ERR_READ / ERR_WRITE. */
    struct Io
    {
        bool (*src)(void *ctx, uint8_t *dst, uint32_t ofs, uint32_t len) = nullptr;
        bool (*patch)(void *ctx, uint8_t *dst, uint32_t ofs, uint32_t len) = nullptr;
        bool (*sink)(void *ctx, const uint8_t *buf, uint32_t len) = nullptr;
        void *ctx = nullptr;
    };

    /** CRC-32/POSIX, the variant the module already uses for files (check value 0x765E7680). */
    uint32_t crcUpdate(uint32_t raw, const uint8_t *data, uint32_t len);
    uint32_t crcFinal(uint32_t raw);
    static const uint32_t CRC_INIT = 0;

    /**
     * @brief One rebuild, driven in slices.
     * @details `begin()` reads and validates the header only. Every later call to `step()` does at
     *          most `scratchLen` bytes of work and returns, so the caller keeps control. The job
     *          never touches the target's activation -- that is the caller's decision, taken only
     *          after `done()` reports success.
     */
    class Job
    {
      public:
        bool begin(const Io &io, uint32_t patchSize, uint32_t srcLimit);
        bool step(uint8_t *scratch, uint32_t scratchLen);

        bool done() const { return _phase == PH_DONE; }
        bool failed() const { return _phase == PH_FAILED; }
        uint8_t error() const { return _err; }

        uint32_t produced() const { return _dstPos; }
        uint32_t targetLen() const { return _dstLen; }
        uint32_t sourceLen() const { return _srcLen; }
        uint32_t sourceCrc() const { return _srcCrc; }
        uint32_t targetCrc() const { return _dstCrc; }

      private:
        enum Phase : uint8_t
        {
            PH_IDLE = 0,
            PH_SRC_CRC, // verify the device runs the image the patch was built against
            PH_OPS,     // interpret the opcode stream
            PH_DONE,
            PH_FAILED,
        };

        bool fail(uint8_t err);
        bool readOps(uint8_t *dst, uint32_t len);
        bool nextByte(uint8_t &out);
        bool readVarint(uint32_t &out);
        bool startNextOp();
        bool stepSrcCrc(uint8_t *scratch, uint32_t scratchLen);
        bool stepOps(uint8_t *scratch, uint32_t scratchLen);

        Io _io;
        Phase _phase = PH_IDLE;
        uint8_t _err = ERR_NONE;

        uint32_t _srcLen = 0, _srcCrc = 0, _dstLen = 0, _dstCrc = 0;
        uint32_t _opsLen = 0, _litLen = 0;
        uint32_t _opsBase = 0, _litBase = 0; // absolute offsets of both streams in the patch file

        uint32_t _srcPos = 0;                // progress of the source check
        uint32_t _opsPos = 0, _litPos = 0;   // consumed bytes of each stream
        uint32_t _dstPos = 0;                // bytes handed to the sink
        uint32_t _crcRaw = CRC_INIT;         // running checksum of the rebuilt image

        uint32_t _opLeft = 0;   // bytes still owed by the operation in progress
        uint32_t _opSrcOfs = 0; // source cursor of a COPY in progress
        bool _opIsCopy = false;
        uint32_t _lastSrcEnd = 0; // where the previous COPY ended; base of the next delta

        // Opcode look-ahead. Varints are read byte by byte, and one file read per byte would make
        // the stream the slowest part of the job.
        uint8_t _opBuf[16];
        uint8_t _opBufLen = 0;
        uint8_t _opBufPos = 0;
    };
} // namespace FirmwarePatch

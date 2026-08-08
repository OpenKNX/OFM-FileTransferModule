#pragma once
/**
 * @file        KnxDeviceMap.h
 * @brief       The single place that knows how each KNX device family is read.
 * @details     Family classification, the fixed BCU identity memory map, and the per-family group-object
 *              descriptor layout. Host + embedded, no KNX/network deps.
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 */

#include <stddef.h>
#include <stdint.h>

namespace KnxDeviceMap
{
    // ---- family classification -----------------------------------------------------------------------------------

    /// @brief KNX device families, derived from the mask version. The family decides transport (some old masks answer
    /// only connection-oriented), identity source (fixed memory map vs. interface-object properties) and table layout.
    enum class Family : uint8_t
    {
        Unknown,
        Bcu1,    ///< System 1  (mask 0x0010-0x0012): memory-mapped, no properties, connection-oriented only
        Bcu2,    ///< System 2  (mask 0x0020-0x0025): classic tables; some variants have properties, some memory-mapped
        System7, ///< BIM M112  (mask 0x0700-0x0705): interface-object properties, own table layout
        SystemB, ///< System B  (mask 0x07B0/0x17B0/0x091A): properties, System B table layout
    };

    /// @brief Classify a device by its mask version.
    inline Family family(uint16_t mask)
    {
        if (mask >= 0x0010 && mask <= 0x0012) return Family::Bcu1;
        if (mask >= 0x0020 && mask <= 0x0025) return Family::Bcu2;
        if (mask >= 0x0700 && mask <= 0x0705) return Family::System7;
        if ((mask & 0x0FF0) == 0x07B0 || mask == 0x091A) return Family::SystemB;
        return Family::Unknown;
    }

    /// @brief Classic BCU table layout (1-octet counts, 2-octet address/assoc entries, 8-bit ASAPs, 3-octet
    /// group-object descriptors). BCU1 + BCU2 use it; everything else uses the System B layout.
    inline bool classicTables(Family f) { return f == Family::Bcu1 || f == Family::Bcu2; }

    /// @brief No interface-object property layer: identity from a fixed memory map, tables at fixed/pointer addresses.
    /// The family *capability* -- BCU2 variants with property objects are gated further at runtime (idxAddr >= 0).
    inline bool memoryMapped(Family f) { return f == Family::Bcu1 || f == Family::Bcu2; }

    /// @brief Answers only connection-oriented: a T_Connect must be open before any read.
    inline bool needsConnectionOriented(Family f) { return f == Family::Bcu1 || f == Family::Bcu2; }

    // ---- BCU1/BCU2 fixed identity memory map (ETS reads these from fixed addresses) -------------------------------

    static constexpr uint16_t IDENT_BASE = 0x0100;    ///< read the identity block from here
    static constexpr uint16_t IDENT_LEN = 0x20;       ///< 0x0100..0x011F covers every field below

    /// @brief Decoded BCU1/BCU2 identity (System 1 and System 2 share these fixed addresses). 0xFF/0 = not read.
    struct Identity
    {
        uint8_t runState = 0xFF;  ///< @0x0103 Ausführungszustand
        uint8_t peiType = 0xFF;   ///< @0x0109 Hardware PEI type
        uint8_t runError = 0xFF;  ///< @0x010D Ausführungsfehler (0xFE/0xFF = OK)
        uint8_t manufacturer = 0; ///< @0x0104 manufacturer code (0x01 = Siemens; verified vs ETS)
        uint8_t appBcd[3] = {0};  ///< @0x0105-0x0107 application id, packed BCD -> the 6-digit ETS number (last pair = version)
        bool haveApp = false;     ///< the application id bytes were present
        bool valid = false;       ///< the block was decoded
    };

    /// @brief Byte at absolute address @p addr within a block read from ::IDENT_BASE, or 0xFF if out of range.
    inline uint8_t at(const uint8_t *block, size_t len, uint16_t addr)
    {
        const uint16_t off = (uint16_t)(addr - IDENT_BASE);
        return (addr >= IDENT_BASE && off < len) ? block[off] : 0xFF;
    }

    /// @brief Decode the fixed identity block (read from ::IDENT_BASE, @p len bytes).
    inline Identity decodeIdentity(const uint8_t *block, size_t len)
    {
        Identity d;
        if (!block || len == 0) return d;
        // Fixed System-1/2 addresses (03_05_01 §4.16 + the BCU memory map; 0x010D = runtime error flags is spec-cited
        // in §4.16.3.4.1). run-state / PEI / manufacturer / app id verified against ETS on real Siemens devices.
        d.runState = at(block, len, 0x0103);
        d.manufacturer = at(block, len, 0x0104); // 0x01 = Siemens
        d.appBcd[0] = at(block, len, 0x0105);    // packed-BCD application id, e.g. 52 01 03 -> "520103" (last pair = version)
        d.appBcd[1] = at(block, len, 0x0106);
        d.appBcd[2] = at(block, len, 0x0107);
        d.peiType = at(block, len, 0x0109);
        d.runError = at(block, len, 0x010D);
        d.haveApp = (d.appBcd[0] != 0xFF || d.appBcd[1] != 0xFF || d.appBcd[2] != 0xFF);
        d.valid = true;
        return d;
    }

    // ---- group-object descriptor layout (flags / priority / size) ------------------------------------------------

    /// @brief Structured group-object config, family-independent. flags bits: 0 C(K) 1 R(L) 2 W(S) 3 T(Ü) 4 U(A).
    struct ComObject
    {
        uint8_t flags = 0;
        uint8_t prio = 0xFF;     ///< 0 system, 1 normal, 2 urgent, 3 low
        uint8_t sizeCode = 0xFF; ///< value-type code -> sizeName()
    };

    /// @brief Classic BCU group-object descriptor: [DataPtr][Config][Type] (03_05_01 §4.18.3). Config bit7 carries
    /// the Update(A) flag on the BCU realisations we read (verified vs ETS); bit5 is the segment selector, not a flag.
    inline ComObject decodeComObjectClassic(uint8_t config, uint8_t type)
    {
        ComObject o;
        if (config & 0x04) o.flags |= 0x01; // C(K) communication enable  (bit2)
        if (config & 0x08) o.flags |= 0x02; // R(L) read enable           (bit3)
        if (config & 0x10) o.flags |= 0x04; // W(S) write enable          (bit4)
        if (config & 0x40) o.flags |= 0x08; // T(Ü) transmit enable       (bit6)
        if (config & 0x80) o.flags |= 0x10; // U(A) update enable         (bit7)
        o.prio = (uint8_t)(config & 0x03);  // 0 system, 1 normal, 2 urgent, 3 low
        o.sizeCode = (uint8_t)(type & 0x3F);
        return o;
    }

    /// @brief System B group-object descriptor: one 16-bit big-endian word (knx group_object.cpp). bit15 update(A),
    /// bit14 transmit(Ü), bit12 write(S), bit11 read(L), bit10 comm(K), bits9-8 priority, low byte = value-type code.
    inline ComObject decodeComObjectSystemB(uint16_t word)
    {
        ComObject o;
        if (word & 0x0400) o.flags |= 0x01; // C(K) communicationEnable (bit10)
        if (word & 0x0800) o.flags |= 0x02; // R(L) readEnable          (bit11)
        if (word & 0x1000) o.flags |= 0x04; // W(S) writeEnable         (bit12)
        if (word & 0x4000) o.flags |= 0x08; // T(Ü) transmitEnable      (bit14)
        if (word & 0x8000) o.flags |= 0x10; // U(A) responseUpdateEnable(bit15)
        o.prio = (uint8_t)((word >> 8) & 0x03);
        o.sizeCode = (uint8_t)(word & 0xFF); // value-type code
        return o;
    }

    /// @brief value-type code (03_05_01 §4.18.3.2 / knx asapValueSize) -> a human object-size label ("1 bit".."14 byte").
    inline const char *sizeName(uint8_t code)
    {
        static const char *const n[] = {"1 bit", "2 bit", "3 bit", "4 bit", "5 bit", "6 bit", "7 bit", "1 byte",
                                        "2 byte", "3 byte", "4 byte", "6 byte", "8 byte", "10 byte", "14 byte",
                                        "5 byte", "7 byte", "9 byte", "11 byte", "12 byte", "13 byte"};
        return (code <= 20) ? n[code] : "?";
    }

    /// @brief KNX priority code (0 system / 1 normal / 2 urgent / 3 low) -> label.
    inline const char *prioName(uint8_t p)
    {
        switch (p)
        {
            case 0: return "system";
            case 1: return "normal";
            case 2: return "urgent";
            case 3: return "low";
            default: return "-";
        }
    }
} // namespace KnxDeviceMap

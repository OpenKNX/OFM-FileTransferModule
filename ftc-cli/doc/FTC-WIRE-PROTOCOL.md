# FTC Wire Protocol — Byte-Exact Transport Spec (host CLI / KNXnet-IP tunnel)

Definitive, byte-exact description of the OpenKNX **FileTransferClient (FTC)** wire protocol, extracted from
the firmware sources so a **native desktop KNXnet/IP tunnel client** can drive it byte-correct: inject cEMI
`L_Data.req` with the right APDU, and parse incoming `L_Data.ind` into the four client callbacks.

Every claim carries a `file:line` and/or a `README_FTC.md §` citation. Line numbers are as of the sources
read for this spec; if code shifts, re-verify against the cited symbol name.

**Sources (authoritative):**
- `lib/OFM-FileTransferModule/src/FileTransferModule.cpp` — the SERVER (authoritative for answer layout).
- `lib/OFM-FileTransferModule/src/FileTransferClient.cpp` — the CLIENT (send building + callback decode).
- `lib/OFM-FileTransferModule/src/FileTransferClientConsole.cpp` / `.h` — the console front-end.
- `knx/src/knx/bau_systemB.cpp` — the 5 `ftcSend*` + 4 scan shims + the 4 response callbacks.
- `knx/src/knx/application_layer.cpp` — APDU assembly + response-indication parsing.
- `knx/src/knx/{apdu,tpdu,npdu,cemi_frame,transport_layer,bits}.cpp` + `knx_types.h` — framing + APCI/TPCI.
- `lib/OFM-FileTransferModule/README_FTC.md`, `doc/errorcodes.txt`.

`lib/knx` is a symlink → `../../knx` (resolved: `/Users/ecolak/Entwicklung/OpenKNX-Router/knx`).

> **Scope note.** On the target device FTC rides the TPUART/NCN5130 host-UART. On the host we replace that
> wire driver with a **KNXnet/IP tunnel**: the APDU/APCI/TPCI bytes and the FTC payload layout are **identical**
> — only the L2 framing changes (cEMI over IP instead of a TP telegram). Everything in §1–§5 is transport-agnostic;
> §6–§8 are the IP-tunnel-specific framing and pacing.

---

## 0. The layer cake (what you actually put on the wire)

```
KNXnet/IP TUNNELING_REQUEST
  └─ Connection header (4 B: 0x04, channelId, seqCounter, 0x00)
  └─ cEMI  L_Data.req                             ← §7 builds this
       MC | AddIL | Ctrl1 | Ctrl2 | SA | DA | LEN | TPCI/APCI ... payload
                                                    └──────── APDU ────────┘
                                                              └─ §1–§5 define these bytes
```

The FTC-specific bytes you assemble live entirely **inside the APDU**. The cEMI/L2 wrapper (§7) is the same
for every FTC command. The APDU is: **`[TPCI|APCI-hi] [APCI-lo] [objectIndex] [propertyId] [FTC-payload...]`**
for the FunctionProperty path (`application_layer.cpp:686-703`).

---

## 1. The five send methods → exact APDU produced

All five are connectionless (`T_Data_Individual`, unnumbered) **except** the scan descriptor read (§6, CO).
All are **`LowPriority`** and (except scan) **`AckRequested`** (`bau_systemB.cpp:614-668`, README §2.1).

### 1.0 How the APCI/TPCI/object bytes are laid down

`ApplicationLayer::functionPropertyCommandRequest()` (`application_layer.cpp:686-703`):
```
CemiFrame frame(3 + length);      // NPDU octetCount = 3 + length
apdu.type(FunctionPropertyCommand);   // APCI = 0x2C7  (knx_types.h:198)
apduData = apdu.data() + 1;       // = frame._data + 2
apduData[0] = objectIndex;
apduData[1] = propertyId;
memcpy(&apduData[2], data, length);
dataIndividualRequest(...)        // connectionless (never _connectedTsap)
```

APCI/TPCI encoding (`apdu.cpp:20-24`, `tpdu.cpp`): the APDU's first two octets hold **TPCI in the top bits of
byte 0** and the **10-bit APCI** split as `byte0[1:0] = APCI[9:8]`, `byte1[7:0] = APCI[7:0]`
(`apdu.cpp:9-18` masks `& 0x3ff`). For connectionless `T_Data_Individual` the TPCI bits are `00` (`tpdu.cpp:46-47`
clears to `_data[0] &= 0x3`), so **byte0 = APCI[9:8]** only.

| Service | APCI (10-bit) | On-wire byte0 (TPCI=0) | byte1 | knx_types.h |
|---|---|---|---|---|
| FunctionProperty_Command | `0x2C7` | `0x02` | `0xC7` | :198 |
| DeviceDescriptor_Read | `0x300` | `0x03` | `0x00` | :201 |
| PropertyValue_Read | `0x3D5` | `0x03` | `0xD5` | :215 |
| PropertyValue_Write | `0x3D7` | `0x03` | `0xD7` | :217 |
| Memory_Read | `0x200` | `0x02` | `0x00`+`number` | :190 |

> `apdu.type()` writes the 16-bit value big-endian via `pushWord` (`apdu.cpp:22-23`), so byte0 = high byte,
> byte1 = low byte. `DeviceDescriptorRead`/`MemoryRead` **OR** their sub-field into byte1 (see below).

The **NPDU length octet** (`LEN` in the cEMI, §7) = `octetCount` = the number of APDU octets **after** the
first (TPCI) octet — i.e. `= 3 + length` for FunctionProperty (byte1 `0xC7` + objIdx + pid + `length` payload)
(`npdu.cpp:11-24`: `octetCount = _data[0]`, `length() = octetCount + 2`; `cemi_frame.cpp:87-98`).

### 1.1 `ftcSendCommand(asap, sec, objectIndex, propertyId, data, length)` — the FTC workhorse
`bau_systemB.cpp:599-618`. **Rejects `length > 251`** before send (stack-overflow guard, README §2.1).

APCI `0x2C7`. Full APDU:
```
[0x02][0xC7][objectIndex][propertyId][ data[0] .. data[length-1] ]
```
- `objectIndex` = **159** (`0x9F`) for all file-transfer commands (`FileTransferClient.cpp:143`,
  server hard-rejects anything else `FileTransferModule.cpp:218`). = **160** (`0xA0`) for the console tunnel (§5.4).
- `propertyId` = the FTC command ID (§2 table).
- `data[]` = the per-command request payload (§3), byte order per §4.

### 1.2 `ftcSendPropertyValueRead(asap, sec, objectIndex, propertyId, count, startIndex)`
`bau_systemB.cpp:637-644` → `application_layer.cpp:599-615`. APCI `0x3D5`, `CemiFrame(5)`.
```
[0x03][0xD5][objectIndex][propertyId][ (startIndex>>8)&0x0F | (count&0x0F)<<4 ][ startIndex & 0xFF ]
```
- byte after pid: high nibble = `count` (numberOfElements, `& 0xF`), low nibble = `startIndex[11:8]`
  (`application_layer.cpp:607-612`; note the `|=` fix so count survives).
- last byte = `startIndex[7:0]`.
- Used for device-identity reads: `objectIndex 0` (Device Object) PIDs — SERIAL=11, PROGMODE=54,
  ORDER, VERSION (`FileTransferClient.cpp:159-174`, `2379`), and table-reference walks (PID 7, obj = table object).

### 1.3 `ftcSendPropertyValueWrite(asap, sec, objectIndex, propertyId, count, startIndex, data, length)`
`bau_systemB.cpp:655-662` → `application_layer.cpp:639-644` (`propertyDataSend(PropertyValueWrite,...)`).
APCI `0x3D7`. Header identical to Read, then `length` data bytes appended:
```
[0x03][0xD7][objectIndex][propertyId][ (startIndex>>8)&0x0F | (count&0x0F)<<4 ][ startIndex & 0xFF ][ data... ]
```
- Only use in FTC: prog-mode LED (`objectIndex 0`, `PID 54`, count 1, startIndex 1, 1 data byte 0/1)
  (`FileTransferClient.cpp:1933`). Fire-and-forget; the echoed PropertyValue_Response is ignored.

### 1.4 `ftcSendDeviceDescriptorRead(asap, sec)` — scan probe (connectionless)
`bau_systemB.cpp:620-627` → `application_layer.cpp:454-464`. APCI `0x300`, `CemiFrame(1)`, descriptorType 0.
```
[0x03][0x00]        (descriptorType 0 OR-ed into byte1 low 6 bits; 0 here)
```
- **`AckDontCare`, not AckRequested** (a scan hits many absent PAs; AckRequested would storm retransmits)
  (`bau_systemB.cpp:622-625`). Present device → answers `DeviceDescriptor_Response` (§5.4/§6).

### 1.5 `ftcSendMemoryRead(asap, sec, number, memoryAddress)` — GA/assoc-table walk
`bau_systemB.cpp:664-669` → `application_layer.cpp:793-803`. APCI `0x200`, `CemiFrame(3)`.
```
[0x02][ 0x00 | (number & 0x3F) ][ memAddr>>8 ][ memAddr & 0xFF ]
```
- `number` = byte count to read (12 for classic table walk, `FTC_GA_STEP`, `FileTransferClient.cpp:175`).
- `memoryAddress` big-endian (`pushWord`, `bits.cpp`).
- **Sent connection-oriented in practice** for `info ga`: it runs over an already-open `T_Connect`
  (`FileTransferClient.cpp:4613,4652` "CO over the open T_Connect"). See §6. `individualSend` auto-routes
  CO when `asap == _connectedTsap` (`application_layer.cpp:1455-1462`).

### 1.6 Worked hexdumps (APDU only — the FTC bytes inside the cEMI)

**(a) CheckFeatures(102) command** — `ftcSendCommand(pa, sec, 159, 102, _ftcTx, 0)` (`FileTransferClient.cpp:879`):
```
02 C7 9F 66
│  │  │  └─ propertyId = 102 = 0x66
│  │  └──── objectIndex = 159 = 0x9F
└──┴─────── APCI 0x2C7 (FunctionProperty_Command), TPCI 0
NPDU LEN octet = 3 (byte1 C7 + 9F + 66)
```

**(b) FileUpload(40) fast DATA chunk** — silent cmd44 DATA, seq=5, n=245 payload
(`FileTransferClient.cpp:1243-1274`, layout `[seqLo][seqHi][n][payload:n][crcHi][crcLo]`):
```
02 C7 9F 2C  05 00 F5  <245 payload bytes>  <crcHi> <crcLo>
│  │  │  │   │  │  │                         └──────┴─ CRC16/MODBUS big-endian over [05 00 F5 payload]
│  │  │  │   │  │  └─ n = 245 = 0xF5
│  │  │  │   └──┴──── seq = 5, LITTLE-endian (05 00)
│  │  │  └─ propertyId 44 = 0x2C (FileUploadFast)
│  │  └──── objectIndex 159 = 0x9F
└──┴─────── APCI 0x2C7
FTC payload length passed to ftcSendCommand = 3 + 245 + 2 = 250 (== the 250 hard cap)
NPDU LEN octet = 3 + 250 = 253  → extended frame (§4.7 README)
```

**(c) FilesystemInfo(46) read** — `ftcSendCommand(pa, sec, 159, 46, _ftcTx, 0)` (`FileTransferClient.cpp:2213`),
no payload:
```
02 C7 9F 2E          (46 = 0x2E)   NPDU LEN = 3
```

**(d) Console PID_IN write (OPEN)** — `conSend(CON_PID_IN, {0x01, paHi, paLo}, 3)`
(`FileTransferClient.cpp:2048-2052`), obj 160, pid 1, flags bit0=OPEN, then the client's own PA:
```
02 C7 A0 01  01 <paHi> <paLo>
│  │  │  │   │  └────┴─ client PA (e.g. 5.0.7 = 0x5007 → 50 07), logged at the target
│  │  │  │   └─ flags = 0x01 (OPEN)   (bit1 0x02 = CLOSE; 0x00 = command line)
│  │  │  └─ propertyId 1 = CON_PID_IN
│  │  └──── objectIndex 160 = 0xA0
└──┴─────── APCI 0x2C7
NPDU LEN = 3 + 3 = 6
```

**(e) Console PID_OUT drain** — `conSend(CON_PID_OUT, nullptr, 0)` (`FileTransferClient.cpp:4919`),
obj 160, pid 2, no payload:
```
02 C7 A0 02          NPDU LEN = 3
```

---

## 2. Command-ID table (FunctionProperty `propertyId` on object 159)

From the server enum `FtmCommands` (`FileTransferModule.cpp:50-69`) and README §4.1. Object index **159** is
mandatory; unknown IDs are silently ignored (`FileTransferModule.cpp:218,328`). Object **160** is the separate
console table (§5.4) — NOT part of this list.

| ID (dec / hex) | Name | Answered? | Request payload → | Answer payload ← | Server fn |
|---:|---|---|---|---|---|
| 0 / 0x00 | Format | yes | (none) | `[00]` ok / `[02]` fail | `cmdFormat` :445 |
| 1 / 0x01 | Exists | yes | `path\0` | `[00][exists:1]` | `cmdExists` :460 |
| 2 / 0x02 | Rename | yes | `old\0new\0` | `[00]` / `[45]` | `cmdRename` :488 |
| 40 / 0x28 | FileUpload | yes* | see §3.1 | see §3.1 | `cmdFileUpload` :752 |
| 41 / 0x29 | FileDownload | yes | see §3.6 | see §3.6 | `cmdFileDownload` :961 |
| 42 / 0x2A | FileDelete | yes | `path\0` | `[00]` / `[44]` | `cmdFileDelete` :736 |
| 43 / 0x2B | FileInfo | yes | `path\0` | `[00][size:4BE][crc32:4BE]` / `[42]` | `cmdFileInfo` :591 |
| 44 / 0x2C | FileUploadFast | open/close yes, DATA **no** | see §3.2 | see §3.2 | `cmdFileUploadFast` :826 |
| 45 / 0x2D | FileReport | yes | `[base:2LE][count:2LE][nonce:1]` | see §3.3 | `cmdFileReport` :919 |
| 46 / 0x2E | FilesystemInfo | yes | (none) | `[00][total:4BE][used:4BE]` | `cmdFilesystemInfo` :651 |
| 80 / 0x50 | DirList | yes | `dir\0` | `[00][type:1][name...]` | `cmdDirList` :669 |
| 81 / 0x51 | DirCreate | yes | `dir\0` | `[00]` / `[85]` | `cmdDirCreate` :703 |
| 82 / 0x52 | DirDelete | yes | `dir\0` | `[00]` / `[84]` | `cmdDirDelete` :720 |
| 90 / 0x5A | Cancel | **no** | (none) | — (returns `false`, no L7 answer) | `cmdCancel` :473 |
| 100 / 0x64 | ModuleVersion | yes | (none) | `[majHi][majLo][minHi][minLo][revHi][revLo]` | `cmdModuleVersion` :513 |
| 101 / 0x65 | FwUpdate | **no** | `path\0` | — (returns `false`) | `cmdFwUpdate` :525/550 |
| 102 / 0x66 | CheckFeatures | yes | (none) | `[flags:1]` | `cmdCheckFeatures` :1010 |

\* FileUpload(40): OPEN and DATA are answered; CLOSE returns an **empty** answer (`resultLength = 0`,
`FileTransferModule.cpp:810`) — its arrival is the whole signal (§3.1).

**Cancel(90) / FwUpdate(101) / FileUploadFast-DATA return `false`** from `processFunctionProperty`
(`FileTransferModule.cpp:281,294,324`) → the app layer sends **no `FunctionProperty_State_Response`**. On a TP
line, L2 still ACKs the request frame; over an IP tunnel there is no L2 ACK — do **not** wait for an L7 answer
to these (README §4.1, §2.1).

---

## 3. Frame layouts (byte-by-byte, the `data[]` inside the APDU)

### 3.1 Classic upload — FileUpload (40) (`FileTransferClient.cpp:935-1113`, `FileTransferModule.cpp:752-818`)
```
OPEN   req : [00][00][payloadSize][flags][path... 00]
             payloadSize -> server _size (seek stride); flags: 0=truncate("w"), 1=resume("r+"), >1 -> 0x42
       ans : [00]  ok  |  [42] open failed  |  [81] a dir listing is open      (1 byte)
DATA   req : [seqLo][seqHi][n][payload:n]                 seq LITTLE-endian, n = payload length
       ans : [result][seqHi][seqLo][crcHi][crcLo]         5 bytes; seq & CRC16 BIG-endian
CLOSE  req : [FF][FF]
       ans : (empty, resultLength = 0)                    arrival is the whole signal
```
- OPEN build `FileTransferClient.cpp:939-946`; server parse `FileTransferModule.cpp:756-800` (`data[3] > 1 → 0x42`).
- DATA build `:1079-1082`; server `writeFile` answer `FileTransferModule.cpp:196-204`
  (`pushByte(0x0); pushWord(sequence); pushWord(crc)` — both BIG-endian, `bits.cpp`).
- Server reads seq as `data[1]<<8 | data[0]` (LITTLE) `FileTransferModule.cpp:816`; CRC16 is over the
  **whole received frame** `data[0..length-1]` = `[seq:2][n][payload]` (`FileTransferModule.cpp:197`), so the
  client CRCs the identical span (`FileTransferClient.cpp:1088`).

### 3.2 Fast upload — FileUploadFast (44) (`FileTransferClient.cpp:1204-1274`, `FileTransferModule.cpp:826-911`)
```
OPEN   req : [00][00][payloadSize][flags][expLo][expHi][path... 00]
             flags bit0=resume(r+), bit1=keepBitmap(recovery re-open); expectedChunks LITTLE-endian
       ans : [00] ok  |  [42] open failed  |  [4A] too many chunks (>8192 -> client goes classic)
DATA   req : [seqLo][seqHi][n][payload:n][crcHi][crcLo]   SILENT; CRC16/MODBUS BIG-endian over [seq:2][n][payload]
       ans : NONE                                         server returns false -> no L7 answer
CLOSE  req : [FF][FF]
       ans : [00]                                          1 byte
```
- OPEN flags/expected built `FileTransferClient.cpp:1204-1226`; server `FileTransferModule.cpp:835-878`
  (`exp = data[4] | (data[5]<<8)` LITTLE, `flags=data[3]`, `_size=data[2]`).
- DATA build `FileTransferClient.cpp:1251-1257` (`crc = ftcCrc16Modbus(_ftcTx, 3+n)`, stored `[3+n]=hi`,
  `[3+n+1]=lo` — BIG-endian); server verify `FileTransferModule.cpp:899-909`
  (`rx = data[3+n]<<8 | data[3+n+1]`, compares `crc16.modbus(data, 3+n)`).
- Server sets the received-bitmap bit **only after** CRC verifies AND `writeChunk` succeeds
  (`FileTransferModule.cpp:907-909`) — "bit set ⇔ correct bytes on disk" (README §4.3). Bad/short/out-of-range
  seq leaves the bit clear → a recoverable gap.
- CLOSE answer `[00]` `FileTransferModule.cpp:882-893`.

### 3.3 Gap report — FileReport (45) (`FileTransferClient.cpp:1282-1295`, `FileTransferModule.cpp:919-959`)
```
req : [baseLo][baseHi][cntLo][cntHi][nonce]                base/count LITTLE-endian
ans : [00][baseHi][baseLo][cntHi][cntLo][nonce][bitmap...] base/count BIG-endian echo; nonce echoed
      bitmap = ceil(count/8) bytes; bit i (LSB-first within each byte) = seq (base+i) received
```
- `count` clamped so `resultLength = 6 + ceil(count/8) ≤ 247` ⇒ **`count ≤ 1928`** (`FileTransferModule.cpp:939-941`).
- Answer decode `FileTransferClient.cpp:3153-3174` (base/count BE, nonce at `[5]`, bitmap from `[6]`).

### 3.4 FileInfo (43) (`FileTransferModule.cpp:591-644`, decode `FileTransferClient.cpp:3320-3328,3453-3456`)
```
req : [path... 00]
ans : [00][size:4 BE][crc32:4 BE]   9 bytes    |   [42]  file not found (1 byte)
```
- `crc32` = **CRC-32/POSIX (`cksum`)** over the whole file (`FastCRC32::cksum`, `FileTransferModule.cpp:617-640`); see §4.4.

### 3.5 FilesystemInfo (46) (`FileTransferModule.cpp:651-667`, decode `FileTransferClient.cpp:3792-3797`)
```
req : (none)
ans : [00][total:4 BE][used:4 BE]   9 bytes    |   1-byte error / no answer (old server)
```
- Client derives `free = total − used`. Read-only, safe any time. Missing command → skip check, upload anyway (README §5.4).

### 3.6 Download — FileDownload (41) (`FileTransferClient.cpp:2889-2906`, `FileTransferModule.cpp:961-1008,119-147`)
```
OPEN  req : [00][00][pkg][path... 00]        pkg = FTC_DL_PAYLOAD = 240
      ans : [00][size:4 BE][00]              size big-endian; note the TRAILING 0x00 (5+... bytes)
CHUNK req : [seqLo][seqHi]                   seq LITTLE-endian
      ans : [00][seqHi][seqLo][readed][data:readed][crcHi][crcLo]   seq & CRC16 BIG-endian; readed<pkg = last
```
- OPEN answer: `pushByte(0); pushInt(fileSize); pushByte(0)` → `[00][size:4BE][00]`, `resultLength=5`
  (`FileTransferModule.cpp:997-1000`). Decode `FileTransferClient.cpp:4687`.
- CHUNK answer `readFile` `FileTransferModule.cpp:119-147`: `[00][seq:2BE][readed][data][crc16:2BE]`,
  `resultLength = readed + 6`, CRC16/MODBUS over `resultData+1 .. resultData+1+(readed+3)` i.e.
  `[seq:2][readed][data]` (`:139-142`). Decode `FileTransferClient.cpp:4745-4772`
  (`readed=_ftcResp[3]`, `rxCrc=[4+readed]<<8|[5+readed]`).

### 3.7 DirList (80) / ModuleVersion (100) / CheckFeatures (102)
```
DirList  req : [dir\0]     ans : [00][type][name...]   type 0=no more, 1=file, 2=dir   (FileTransferModule.cpp:669-701; decode :3670)
ModuleVer ans : [majHi][majLo][minHi][minLo][revHi][revLo]   6 B, each field BIG-endian   (:513-522; decode :4311-4313)
CheckFeat ans : [flags]   bit0(0x01)=Resume, bit1(0x02)=Update, bit2(0x04)=FAST, bit3(0x08)=Console  (:1010-1023)
```
- CheckFeatures flags built `FileTransferModule.cpp:1012-1021`: `0x01` always; `0x02` on RP2040/ESP32;
  `0x04` always (server understands cmd44/45); `0x08` only if `OPENKNX_FTC_CONSOLE`. Decode `FileTransferClient.cpp:3050,3600`.

---

## 4. Endianness, CRCs, result codes

### 4.1 Endianness — "the one real trap" (README §4.2, `FileTransferClient.cpp:1077-1078` comment)
The protocol is **deliberately asymmetric**. Getting it wrong looks exactly like a sequence mismatch.

| Field | In the **request** | In the **answer** |
|---|---|---|
| chunk sequence (upload 40/44, download 41) | **LITTLE**-endian (`[seqLo][seqHi]`, server reads `data[1]<<8\|data[0]`) | **BIG**-endian (`pushWord`) |
| per-frame CRC16 (classic-DATA ans, download-chunk ans) | — | **BIG**-endian |
| fast-DATA trailing CRC16 | **BIG**-endian, over `[seq:2][n][payload]` | — |
| report `base`/`count` (45) | **LITTLE**-endian | **BIG**-endian (echoed) |
| fast-OPEN `expectedChunks` (44) | **LITTLE**-endian (`[expLo][expHi]`) | — |
| FileInfo `size`/`crc32` (43), FilesystemInfo (46), Download size (41), ModuleVersion (100) | — | **BIG**-endian |
| PropertyValue_Read `startIndex`/`count` (§1.2) | packed nibbles, see §1.2 | mirrored back |
| Memory_Read `memoryAddress` (§1.5) | **BIG**-endian (`pushWord`) | echoed BIG-endian |

`pushWord`/`pushInt` are big-endian (`bits.cpp`: `data[0]=high byte`). Fast-DATA seq is the **only** place a
2-byte field is written little-endian on the request side by hand (`FileTransferClient.cpp:1251-1252`).

### 4.2 The two CRCs (README §4.4)

| CRC | Parameters | Over which bytes | Placed | Source |
|---|---|---|---|---|
| **CRC-16/MODBUS** | init `0xFFFF`, reflected poly `0xA001`, no final xor | classic DATA ans: `[seq:2][n][payload]`; fast DATA trailer: `[seq:2][n][payload]`; download chunk: `[seq:2][readed][data]` | last 2 bytes of the frame, **BIG-endian** | client `FileTransferClient.cpp:308-319`; server `FastCRC16::modbus` `FileTransferModule.cpp:139-142,196-197,899-904` |
| **CRC-32/POSIX (`cksum`)** | poly `0x04C11DB7`, init 0, **no reflect** (MSB-first), **xorout `0xFFFFFFFF`** | whole file | FileInfo(43) answer `crc32`, and the client's end-of-transfer verify | client `ftcCrc32Posix` `FileTransferClient.cpp` (MSB-first loop, `^0xFFFFFFFF` xorout); server `FastCRC32::cksum` `FileTransferModule.cpp:617-640` |

The whole-file CRC is **POSIX `cksum`, NOT zlib** (README §4.4). Client folds source bytes into `_ftcSrcCrc`
as it streams, then compares `_ftcSrcCrc ^ 0xFFFFFFFF` against the target's FileInfo crc32.

> **Host CRC16 reference** (must match `ftcCrc16Modbus`, `FileTransferClient.cpp:309-319`):
> `crc=0xFFFF; for each byte: crc ^= byte; 8×{ crc = (crc&1) ? (crc>>1)^0xA001 : crc>>1 }`. Emit as `[hi][lo]`.
> **Host CRC32 reference** (`ftcCrc32Posix`): `for each byte: crc ^= byte<<24; 8×{ crc = (crc&0x80000000)?
> (crc<<1)^0x04C11DB7 : crc<<1 }`; final value XOR `0xFFFFFFFF`. (Note: `cksum` normally appends the length;
> here it does NOT — it is a plain MSB-first CRC32 over the file bytes with a final complement.)

### 4.3 Result codes (`doc/errorcodes.txt`, README §4.5)
`0x00` OK · `0x01` LittleFS.begin fail · `0x02` format fail · `0x03` FS not init · `0x04` pkg>maxResultLength ·
`0x41` file already open · `0x42` file can't open · `0x43` file not opened · `0x44` file can't delete ·
`0x45` file can't rename · `0x46` seek failed · `0x47` short write (**FS full**) · `0x4A` fast: too many chunks →
go classic · `0x81` dir already open · `0x82` dir can't open · `0x83` dir not opened · `0x84` dir can't delete ·
`0x85` dir can't create · `0x86` dir no more files. Console adds `0x01` = BUSY (session owned),
`0x43` = no open session (`FileTransferModule.cpp:343,371,393`).

---

## 5. Response decoding — what the transport hands each callback

Four callbacks are registered in the BAU and fire **inside the KNX stack dispatch** — they only park bytes;
the client `loop()` acts (README §2.2). The transport must call the equivalent of these with exactly the bytes
below. **All are parsed from the incoming `L_Data.ind` APDU** as follows.

`data = apdu.data() + 1` in the request builders, but in the **indication** parser `data = apdu.data()`
(= frame `_data + 1`, i.e. the byte after the TPCI octet), so `data[0]` = APCI-low byte, `data[1]` = objectIndex,
etc. `apdu.length()` = NPDU octetCount (`application_layer.cpp` indication switch).

### 5.1 `ftcOnResponse(pa, objectIndex, propertyId, data, length)` ← A_FunctionProperty_State_Response
APCI `0x2C9` (`knx_types.h:200`). Parsed `application_layer.cpp:1184-1189`:
```
objectIndex = data[1];  propertyId = data[2];  payload = &data[3];  length = apdu.length() - 3
```
So the callback receives the **server's `resultData` / `resultLength` verbatim** (the §3 answer bytes). Guarded
by `apdu.length() >= 3`. Client stores into `_ftcResp[]` / `_ftcRespLen` / `_ftcRespObj` / `_ftcRespProp`
(`FileTransferClient.cpp:321-341`) and **filters by source PA == target** (`:330`). Every §2 "answered" command's
answer arrives here; the client dispatches on `_ftcRespProp` (== the propertyId echoed by the server) to avoid
mixing a stale 9-byte FileInfo answer with e.g. an open-ack (`:3075,3146,3438,3790`).

### 5.2 `ftcOnPropertyValue(pa, objectIndex, propertyId, data, length)` ← A_PropertyValue_Response
APCI `0x3D6` (`knx_types.h:216`). Parsed `application_layer.cpp:1128-1139` (requires `apdu.length() >= 5`):
```
objectIndex = data[1];  propertyId = data[2];
numberOfElements = data[3] >> 4;  startIndex = popWord(data+3) & 0xFFF;
payload = &data[5];  length = apdu.length() - 5
```
Callback keeps `payload`/`length` into `_propData/_propObj/_propPid/_propLen`, PA-filtered
(`FileTransferClient.cpp:355-367`). Used to read Device-Object identity (SERIAL `data[0..1]`=mfr, `[2..5]`=serial,
`:4223`), version/order strings, and table references (`info`, `info ga`).

### 5.3 `ftcOnDeviceDescriptor(pa, descriptorType, data)` ← A_DeviceDescriptor_Response
APCI `0x340` (`knx_types.h:202`). Parsed `application_layer.cpp:1096-1098`:
```
descriptorType = data[0] & 0x3F;  deviceDescriptor = data + 1
```
For type 0 the descriptor is the **2-byte mask version**, big-endian: callback forms
`mask = data[0]<<8 | data[1]` (`FileTransferClient.cpp:343-352`) into the SPSC ring `_ftcDdQ[16]` (two devices
can answer back-to-back). This is the scan/ping-existence proof.

### 5.4 `ftcOnMemory(pa, addr, data, len)` ← A_Memory_Response
APCI `0x240` (`knx_types.h:191`). Parsed `application_layer.cpp:1232-1234` (guard `count ≤ apdu.length()-3`):
```
number(count) = data[0] & 0x3F;  memoryAddress = getWord(data+1) (BIG-endian);  memoryData = data + 3
```
Callback places `len` bytes at `_memBuf[addr - _gaRef]` (idempotent on IP-mirror dup), PA-filtered
(`FileTransferClient.cpp:369-388`). Used for the `info ga` GA/association-table walk over a T_Connect (§6).

### 5.5 Console answers (obj 160) also arrive on `ftcOnResponse`
`CON_PID_IN` ack: `[status]` — `0x00` ok, `0x01` BUSY, `0x43` no session (`FileTransferModule.cpp:336-388`;
decode `FileTransferClient.cpp:4905-4919`). `CON_PID_OUT` drain answer:
`[status][more][overflow][text... ≤247]` — `res[1]=more`(1=more pending), `res[2]=overflow`(1=ring wrapped),
`res[3..]` = verbatim console text (`FileTransferModule.cpp:390-417`; decode `FileTransferClient.cpp:4930-4945`).

---

## 6. Connection-oriented path (scan / `info ga`) — Transport-Layer sequence

The scan descriptor read and the `info ga` memory walk run **connection-oriented** over a `T_Connect`. Shims:
`ftcScanConnect` / `ftcScanConnected` / `ftcScanReadDescriptor` / `ftcScanDisconnect`
(`bau_systemB.cpp:680-704`).

### 6.1 TPCI byte encodings (`tpdu.cpp:40-104`)
| TPDU | byte0 | Notes |
|---|---|---|
| `T_Data_Individual` (unnumbered, connectionless) | `0x00` (top bits cleared) | the normal FTC path |
| `T_Data_Connected` (numbered) | `0x40 \| (seq&0xF)<<2` | data on an open connection |
| `T_Connect` | `0x80` | open |
| `T_Disconnect` | `0x81` | close |
| `T_ACK` | `0xC2 \| (seq&0xF)<<2` | ack of a received numbered TPDU |
| `T_NAK` | `0xC3 \| (seq&0xF)<<2` | nak |

Sequence number occupies **bits 5..2** of byte0: `seq = (byte0 >> 2) & 0xF` (`tpdu.cpp:95-104`).

### 6.2 The sequence a host must reproduce
1. **`ftcScanConnect(pa)`** (`bau_systemB.cpp:680-687`): self-guards if already connected, else
   `connectRequest(pa, SystemPriority)` → TL `A12` (`transport_layer.cpp:690-700`): send **`T_Connect`**
   (byte0 `0x80`) to `pa`, `AckRequested`, `SystemPriority`; reset `_seqNoSend = _seqNoRecv = 0`; state → Connecting.
2. Target answers **`T_ACK`** for the connect and the connection is up. `ftcScanConnected()` returns
   `applicationLayer().isConnected()` (`bau_systemB.cpp:689-692`).
3. **`ftcScanReadDescriptor(sec)`** → `ftcDeviceDescriptorReadConnected` (`application_layer.cpp:529-537`):
   builds APCI `DeviceDescriptorRead` (0x300, descriptorType 0) and sends to `_connectedTsap`, so `individualSend`
   takes the CO branch → TL `A7` (`transport_layer.cpp:642-653`): TPDU type `T_Data_Connected`,
   `sequenceNumber = _seqNoSend`, `AckRequested`, `SystemPriority`. On-wire APDU byte0 = `0x40 | (seq<<2)`,
   byte1 = `0x00` (APCI hi), byte2 = `0x00` (APCI lo, descriptorType 0).
4. Target sends **`T_ACK(seq)`**, then a **`T_Data_Connected` A_DeviceDescriptor_Response** carrying the mask.
   On receiving the numbered data TPDU the local TL sends **`T_ACK(_seqNoRecv)`** and increments `_seqNoRecv`
   (`A2`, `transport_layer.cpp:607-613`). The host must ACK every numbered TPDU it receives, echoing that TPDU's
   seq (`A3`/`sendControlTelegram(Ack, seqNo)` `:576-584,615-619`).
5. **`ftcScanDisconnect()`** (`bau_systemB.cpp:699-704`): `disconnectRequest(SystemPriority)` →
   send **`T_Disconnect`** (byte0 `0x81`). **Always callable** — also unwedges a stuck Connecting state.

**Seq-number rules** (`transport_layer.cpp`): both counters start at 0 on connect (`A12`/`A1` :595-597,697-698);
`_seqNoSend` increments after each sent numbered data TPDU is ACKed (`A8` :655-661, `incSeqNr` wraps at 0xF→0
:600-605); `_seqNoRecv` is the expected receive seq — a matching numbered TPDU is ACKed and `_seqNoRecv++`;
a duplicate of `(_seqNoRecv-1)&0xF` is re-ACKed but not re-delivered (`:45-63`). Connect/ACK/Disconnect control
TPDUs are all `AckRequested`.

> For `info ga` the same open connection then carries `A_Memory_Read` numbered TPDUs (§1.5) whose responses
> come back as `A_Memory_Response` (§5.4) (`FileTransferClient.cpp:4613,4652`). BCU1 (mask 0x0012) is unsupported
> for `info ga` (no property/interface-object layer) (README §11).

---

## 7. cEMI framing for the tunnel

Byte layout (`cemi_frame.cpp:6-75`):
```
[MC][AddIL][Ctrl1][Ctrl2][SA_hi][SA_lo][DA_hi][DA_lo][LEN][ TPCI/APCI + payload ... ]
```
- **MC** (message code, `knx_types.h:41-43`): host→bus `L_Data.req = 0x11`; bus→host `L_Data.ind = 0x29`;
  the tunnel/local confirmation `L_Data.con = 0x2E`.
- **AddIL** = `0x00` (no additional info) — the `CemiFrame(apduLength)` ctor produces `data[1]=0` (`cemi_frame.cpp:87-98`).
- **Ctrl1** (`cemi_frame.cpp:35-62,219-283`): bit7 FrameType (1=standard, **0=extended**); bit5 Repeat (1=do-not-repeat);
  bit4 Broadcast (1=broadcast domain, set by the ctor `_ctrl1[0] |= Broadcast` :96); bits3-2 Priority
  (**0b11 = Low**, FTC uses Low :254); bit1 **AckRequest** (1 for all FTC except scan-descriptor); bit0 Confirm (0 on req).
  For an FTC data chunk `octetCount > 15` ⇒ the stack emits an **extended frame** (bit7=0) (README §4.7,
  `cemi_frame.cpp:410-416`).
- **Ctrl2** (`cemi_frame.cpp:64-75,285-305`): bit7 **DestAddrType** (0=individual — FTC dest is always a PA);
  bits6-4 **HopCount** (0-7, from `NetworkLayerParameter`/default 6); bits3-0 Extended Frame Format = 0.
- **SA** = the **tunnel's assigned individual address** (the KNXnet/IP server gives the host its own PA; use it
  as source). **DA** = target device PA (individual). Both big-endian (`cemi_frame.cpp:314-329`, `pushWord`).
- **LEN** = NPDU `octetCount` (§1.0): FunctionProperty = `3 + payloadLength`; PropertyValueRead = 5;
  DeviceDescriptorRead = 1; MemoryRead = 3.
- **TPCI/APCI + payload**: exactly the APDU bytes from §1 (`[byte0][byte1][objIdx][pid][payload...]`, or the CO
  TPCI byte for §6).

### 7.1 `L_Data.req` for connectionless T_Data_Individual (the normal FTC path)
```
11 00 <Ctrl1> <Ctrl2> <SAhi><SAlo> <DAhi><DAlo> <LEN> 00 <APCIlo> <objIdx> <pid> <payload...>
   │  │        │        │            │            │    └── byte0: TPCI=00 (T_Data_Individual) + APCI[9:8]
   │  │        │        │            │            └─ octetCount = 3 + payloadLen
   │  │        │        │            └─ DA = target PA
   │  │        │        └─ SA = tunnel PA
   │  │        └─ Ctrl2: individual dest, hopcount 6 → e.g. 0x60 (extended) 
   │  └─ Ctrl1: e.g. 0xBC standard/low/ack ... but FTC chunks are EXTENDED → bit7=0
   └─ AddIL 0
```
Example Ctrl1 for an FTC command (extended, do-not-repeat, broadcast, Low, AckReq, no-confirm):
`bit7=0, bit5=1, bit4=1, bits3-2=11, bit1=1, bit0=0` → `0b00110110 = 0x36`. (Standard-frame short commands use
bit7=1 → `0xB6`/`0xBC`-class; the stack sets FrameType from `octetCount>15`.) Do not hand-force FrameType if
your IP stack computes it; if you build cEMI by hand, set bit7=0 whenever `LEN > 15`.

### 7.2 `L_Data.req` for T_Data_Connected (scan / info ga)
Same header; **byte0 (TPCI) = `0x40 | (seq&0xF)<<2`** (§6.1), payload = the CO APDU. Control TPDUs
(`T_Connect 0x80`, `T_Disconnect 0x81`, `T_ACK 0xC2|seq<<2`) are frames whose APDU is **just the 1 TPCI byte**
(`CemiFrame(0)` → `octetCount = 0`, LEN = 0; `transport_layer.cpp:576-584,690-700`).

### 7.3 Recognizing the matching `L_Data.ind` coming back
An answer is an inbound cEMI **`0x29` (L_Data.ind)** with **DA == your tunnel SA** and **SA == the target PA**;
APDU byte1 (APCI-lo, combined with byte0 bits) decodes to one of: `FunctionProperty_State_Response 0x2C9`,
`PropertyValue_Response 0x3D6`, `DeviceDescriptor_Response 0x340`, `Memory_Response 0x240`, or a CO control TPDU
(`T_ACK`/`T_Disconnect`). Recover the 10-bit APCI exactly as `apdu.cpp:9-18`: `apci = (byte0<<8 | byte1) & 0x3FF;
if ((apci>>6) < 11 && (apci>>6) != 7) apci &= 0x3C0;` (collapses the "short" APCIs). The client additionally
filters every FTC answer by **source PA == target** (`FileTransferClient.cpp:330,359,372`) and, for obj-159
commands, by the echoed `propertyId` in `data[2]`.

> **IP-mirror duplicates:** a second KNX-IP router mirrors TP→IP multicast, so answers can arrive twice
> (README fix #12). Dedup by a `FTC_DUP_WINDOW_MS = 12` time window + propertyId + sequence/nonce
> (`FileTransferClient.cpp:391-396`). Over a pure tunnel this is usually moot, but keep the seq/nonce checks.

---

## 8. Timing & pacing constants the host loop must honor

Values from `FileTransferClient.cpp` / `.h` (README §10.2) and the server:

| Constant | Value | Where it gates |
|---|---|---|
| `FTC_TIMEOUT` | 6000 ms | default per-state answer timeout (most waits) `:3093,3886,3913,4921` |
| `FTC_FEATURE_TIMEOUT` | 800 ms | short CheckFeatures(102) probe window — old server never answers → fast downgrade (README §3) |
| `FTC_FAST_MAX_CHUNKS` | 8192 | file needing more chunks → downgrade to classic (mirrors server bitmap) |
| `FTC_WND_INIT/MIN/MAX` | 8 / 4 / 64 | windowed-mode AIMD window (`+8` clean, `÷2` on loss) `:461-464 README §5.2` |
| `FTC_TX_HIGH / LOW` | 30 / 1 | TP-FIFO water marks: pump yields at ≥30 queued, waits to drain <1 before report/close `:3116,3126` |
| `FTC_FAST_BURST_SD / RAM` | 4 / 16 | per-`loop()` send cap (SD read costly, RAM cheap) `:3110-3111` |
| `FTC_FORGET_BURST / PACE_MS` | 4 / 25 | forget pacing: ≤4 frames then a 25 ms `millis()` gate (no FIFO backpressure over IP) `:3109-3121` |
| `FTC_REPORT_TIMEOUT / RETRIES` | 4000 ms / 3 | gap-report answer timeout & retries |
| `FTC_NOPROGRESS_MAX` | 4 | reports without the missing-count shrinking → abort |
| `FTC_FAST_PAGE` | 1024 | seqs per report during forget recovery |
| `FTC_FAST_STALL_MS` | 30000 ms | progress-based overall deadline (re-armed on every advancing chunk) `:1267,3102` |
| `FTC_DUP_WINDOW_MS` | 12 | IP-mirror duplicate window `:393` |
| `FTC_SCAN_SPACING_MS / DRAIN_MS / MAX_LIST` | 40 / 2500 / 128 | scan probe pacing, drain, listing cap |
| `FTC_DL_PAYLOAD` | 240 | download data bytes/chunk requested |
| `FTC_FS_MARGIN` | 8192 | pre-upload free-space headroom demanded `README §5.4` |
| `_cfgMaxRetries` / `_cfgTransferRetries` / `_cfgBackoffMs` | 3 / 8 / 3000 ms | per-chunk / whole-transfer retries / settle time (runtime-settable) |
| server `HEARTBEAT_INTERVAL` | 30000 ms | server auto-closes an idle open file/dir; every frame refreshes `_heartbeat` (`FileTransferModule.cpp:24-37,754,830`) |
| console `CON_IDLE_TMO` | 60000 ms | server reaps an idle console session (`FileTransferModule.h:36`) |
| `pkg` | 16..**254** (default 64) | frame size; 254 = KNX extended-frame max (255 = `0xFF` escape); needs NPDU length uint16 + send guard 251 (README §6.4) |

**Host-loop implications over IP:**
- Over a tunnel there is **no TP-FIFO and no L2 ACK**, so the `FTC_TX_HIGH/LOW` FIFO gate does not apply
  directly — but you must **honor `FTC_FORGET_BURST/PACE_MS`** (4 frames / 25 ms) to avoid overrunning the
  target's ~2 KB RX socket + flash (README fix #3: an un-paced forget stream dropped most chunks).
- Fast-DATA frames are **silent** (no L7 answer, §3.2). Do not wait for one. The only integrity feedback is the
  cmd45 gap report (windowed) or the end-of-file verify (forget).
- Cancel(90), FwUpdate(101), fast-DATA get **no L7 answer** (§2) — fire-and-forget them.
- The whole-file **verify** (FileInfo 43 → CRC32/POSIX compare) is the only honest proof of a good transfer
  (README §4.4) — always run it after the last chunk.
- `pkg 253` maps to a FunctionProperty payload of **exactly 250 bytes** (fast `245+5`, classic `247+3`) = the
  `ftcSendCommand` 250-byte hard cap (README §6.4, §2.1).

---

## 9. Open items / ambiguities not fully resolvable from code

1. **cEMI additional-info & exact Ctrl1/Ctrl2 bytes over the specific tunnel server.** The firmware builds cEMI
   through its own stack (`cemi_frame.cpp`); the precise Ctrl1 nibble your KNXnet/IP server expects (some accept
   `L_Data.req` with the frame-type bit left for the server to set, some do not) is server-dependent. The bit
   meanings above are exact; the concrete byte depends on whether your stack recomputes FrameType from `LEN>15`.
   Verify against a real CONNECT_RESPONSE / one round trip.
2. **Tunnel source address.** The host must use the individual address the KNXnet/IP server assigns
   (CONNECT_RESPONSE CRD, tunnelling). The firmware never chooses this (it is the device's own PA); on the host
   it is negotiated. Not derivable from these sources.
3. **`cksum` length-append.** `FastCRC32::cksum` here is used as a plain complemented MSB-first CRC32 over the
   file bytes (`ftcCrc32Posix` confirms: no length octets appended). If a host links a *standard* POSIX `cksum`
   (which appends the byte-length before finalizing), results will differ — use the reference in §4.2, not a
   library `cksum`.
4. **DirList `type` byte for the `df`/`ll` footer bar** is emitted by the server only when it has the command;
   old-server graceful-degrade paths (README §5.4) mean the host must tolerate a 1-byte error where a 9-byte
   answer was expected (already handled: `_ftcResp[0] != 0x00 → skip`).
5. **KO/`info ga` memory map** beyond the table-reference read (PID 7) is mask-specific and only partly walked in
   `FileTransferClient.cpp:4600-4660`; a full GA-resolution host feature would need the per-mask table layout,
   which is not in these sources.

*End of spec.*

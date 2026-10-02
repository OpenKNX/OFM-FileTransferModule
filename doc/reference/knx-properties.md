# KNX interface objects and properties

**For:** anyone using `ftc prop`, `ftc busprop` or `ftc … runstate` to read or write the KNX resources of
a device. What an object index is, which objects and PIDs a device actually has, and how to read the
answers.

Everything below was measured on 2026-09-27 against the devices named in each table; PID names and
numbers come from `knx/src/knx/property.h`, object types from `knx/src/knx/interface_object.h`.

## The two commands

| | addresses the object by | reaches | service |
|---|---|---|---|
| `prop read\|write <iot> <inst> <pid> [start] [hex]` | **type** + instance | the interface's **own** objects | local device management over the KNXnet/IP connection |
| `busprop read\|write <pa> <objIdx> <pid> [start] [hex]` | **index** | **any** device on the bus | `A_PropertyValue_Read` / `_Write` |

`busprop dump <pa>` walks indices 0…12 and every known PID and prints what answers.

Why two addressing modes: the KNX property services on the bus (03_03_07) carry an **object index**, not a
type. Local device management (cEMI `M_PropRead`) carries type + instance. The index is device specific —
see below.

## The object index is not fixed, and it may have gaps

An object index is only meaningful per device. The same object type sits at a different index on every
product, and the indices need not be contiguous.

Measured, three devices:

| index | IP-Router `5.0.0` (mask 091A) | IP-Interface `5.0.13` (mask 07B0) | Presence MR16 `5.0.8` |
|---|---|---|---|
| 0 | Device (OT 0) | Device (OT 0) | Device (OT 0) |
| 1 | **Router (OT 6)** | Address table (OT 1) | Address table (OT 1) |
| 2 | Application program (OT 3) | Association table (OT 2) | Association table (OT 2) |
| 3 | KNXnet/IP parameter (OT 11) | **Group object table (OT 9)** | **Group object table (OT 9)** |
| 4 | cEMI server (OT 8) | **Application program (OT 3)** | **Application program (OT 3)** |
| 5 | — | *nothing* | — |
| 6 | — | KNXnet/IP parameter (OT 11) | — |
| 7 | — | cEMI server (OT 8) | — |

Index 5 on the interface answers nothing while 6 and 7 do (`bau07B0_ip.cpp:95`, reserved for a second
application program object). That is permitted: 03_05_01 states a numbering requirement only for E-Mode
channel objects, and there explicitly says indices "do not need to be in consecutive order" (p.162, p.181).

**Consequence for any client: never guess an index, and never derive it by counting.** Read `PID_OBJECT_TYPE`
(PID 1) per index, or read the device object's `PID_IO_LIST`. This is not theory — `ftc … runstate` shipped
with a hard-coded index 3, asked the group object table instead of the application program, and reported a
property as "not implemented" that simply was not on that object.

### PID_IO_LIST (device object, PID 71)

Array of object types. Element 0 is the count, elements 1…n the types **in ascending order of the object
indexes** (03_05_01 §4.3.22.1 p.72). Measured on the interface:

```
elem 0: 0007      7 objects
elem 1: 0000      OT_DEVICE            -> index 0
elem 2: 0001      OT_ADDR_TABLE        -> index 1
elem 3: 0002      OT_ASSOC_TABLE       -> index 2
elem 4: 0009      OT_GRP_OBJ_TABLE     -> index 3
elem 5: 0003      OT_APPLICATION_PROG  -> index 4
elem 6: 000B      OT_IP_PARAMETER      -> index 6   <- NOT 5
elem 7: 0008      OT_CEMI_SERVER       -> index 7
```

The last two lines are the trap: the element position is the n-th **active** object, not the object index.
As soon as a device has a gap the two diverge.

## Object types

`OT_DEVICE 0` · `OT_ADDR_TABLE 1` · `OT_ASSOC_TABLE 2` · `OT_APPLICATION_PROG 3` · `OT_INTERFACE_PROG 4` ·
`OT_OJB_ASSOC_TABLE 5` · `OT_ROUTER 6` · `OT_LTE_ADDR_ROUTING_TABLE 7` · `OT_CEMI_SERVER 8` ·
`OT_GRP_OBJ_TABLE 9` · `OT_POLLING_MASTER 10` · `OT_IP_PARAMETER 11` · `OT_FILE_SERVER 13` ·
`OT_SECURITY 17` · `OT_RF_MEDIUM 19`.

## PIDs by object

A PID number is only unique **within** an object type: PID 51 is `PID_ROUTING_COUNT` on the device object,
`PID_PROJECT_INSTALLATION_ID` on the KNXnet/IP object and `PID_MEDIUM_TYPE` on the cEMI server. Always read
the object type first.

### Type independent (every object)

| PID | Name | Notes |
|---|---|---|
| 1 | PID_OBJECT_TYPE | the object type, 2 octets — the only reliable way to identify an index |
| 5 | PID_LOAD_STATE_CONTROL | load state machine, read 1 octet / write 10 (see below) |
| 6 | PID_RUN_STATE_CONTROL | run state machine, read 1 octet / write 1 (see below) |
| 7 | PID_TABLE_REFERENCE | memory address of the table this object manages |
| 11 | PID_SERIAL_NUMBER | 6 octets |
| 12 | PID_MANUFACTURER_ID | 2 octets, `00FA` = OpenKNX |
| 13 | PID_PROG_VERSION | 5 octets: manufacturer(2) application(2) version(1) |
| 14 | PID_DEVICE_CONTROL | bit field, incl. "application stopped" |
| 15 | PID_ORDER_INFO | 10 octets ASCII |
| 16 | PID_PEI_TYPE | |
| 23 | PID_TABLE | the table content itself |
| 25 | PID_VERSION | |
| 27 | PID_MCB_TABLE | memory control block: 8 octets = segment size(4) CRC control(1) read/write level(1) CRC16(2) |
| 28 | PID_ERROR_CODE | the last error before the load state went to `Error` |
| 29 | PID_OBJECT_INDEX | optional, read-only |

### Device object (OT 0)

| PID | Name | measured on `5.0.13` |
|---|---|---|
| 51 | PID_ROUTING_COUNT | `60` — hop count 6 in bits 6-4 |
| 54 | PID_PROG_MODE | `00` — 01 = programming mode on |
| 56 | PID_MAX_APDU_LENGTH | `00FE` = 254 |
| 57 | PID_SUBNET_ADDR | `50` = area/line 5.0 |
| 58 | PID_DEVICE_ADDR | `0D` = 13 → PA 5.0.13 |
| 71 | PID_IO_LIST | see above |
| 78 | PID_HARDWARE_TYPE | 6 octets |
| 83 | PID_DEVICE_DESCRIPTOR | **`07B0`** interface / **`091A`** router — the mask version |

PID 83 is the fastest way to tell the two products apart on the bus.

### KNXnet/IP parameter object (OT 11)

| PID | Name | interface `5.0.13` | router `5.0.0` |
|---|---|---|---|
| 51 | PID_PROJECT_INSTALLATION_ID | `0000` | `0000` |
| 52 | PID_KNX_INDIVIDUAL_ADDRESS | `500D` = 5.0.13 | `5000` = 5.0.0 |
| 53 | PID_ADDITIONAL_INDIVIDUAL_ADDRESSES | `506E` = first tunnel PA 5.0.110 | `50DC` = 5.0.220 |
| 54 | PID_CURRENT_IP_ASSIGNMENT_METHOD | `04` = DHCP | `04` |
| 55 | PID_IP_ASSIGNMENT_METHOD | `04` | `04` |
| 56 | PID_IP_CAPABILITIES | `02` | `02` |
| 57 | PID_CURRENT_IP_ADDRESS | `0B0B00D2` = 11.11.0.210 | `0B0B007E` = 11.11.0.126 |
| 58 | PID_CURRENT_SUBNET_MASK | `FFFFFF00` | `FFFFFF00` |
| 59 | PID_CURRENT_DEFAULT_GATEWAY | `0B0B0001` | `0B0B0001` |
| 60-62 | PID_IP_ADDRESS / SUBNET_MASK / DEFAULT_GATEWAY | `00000000` (static config unused) | same |
| 64 | PID_MAC_ADDRESS | 6 octets | 6 octets |
| 65 | PID_SYSTEM_SETUP_MULTICAST_ADDRESS | `E000170C` = 224.0.23.12, read-only per 03_08_02 §8.5.2.1 | same |
| 66 | PID_ROUTING_MULTICAST_ADDRESS | **`00000000`** | `E000170C` |
| 67 | PID_TTL | `20` = 32 | `20` |
| 68 | PID_KNXNETIP_DEVICE_CAPABILITIES | `0001` | `0001` |
| 69 | PID_KNXNETIP_DEVICE_STATE | `00` = no fault | `00` |
| 72-75 | queue overflow / message counters | — | router only |
| 76 | PID_FRIENDLY_NAME | array, one octet per read | array |
| 201/202 | reserved-tunnel control | OpenKNX private (PID ≥ 200) | |

PID 66 = `00000000` on the interface is deliberate: a non-router must read 0 there.

### Router object (OT 6) — router only

PID 51 `PID_MEDIUM_STATUS`, 52 `PID_MAIN_LCCONFIG`, 53 `PID_SUB_LCCONFIG`, 54 `PID_MAIN_LCGRPCONFIG`,
55 `PID_SUB_LCGRPCONFIG`, 58 `PID_MAX_APDU_LENGTH_ROUTER`. Measured `5.0.0`: 52=`0A`, 54=`0A` —
`0Ah` on the group config is `GROUP_LOCK6FFF`, i.e. the coupler blocks 0…6FFF in that direction
(03_05_01 §4.5.6 p.93).

### cEMI server object (OT 8)

PID 51 `PID_MEDIUM_TYPE`, 52 `PID_COMM_MODE`, 53 `PID_MEDIUM_AVAILABILITY`, 54 `PID_ADD_INFO_TYPES`,
55 `PID_TIME_BASE`, 64 `PID_COMM_MODES_SUPPORTED`, 68/69 max APDU length interface/local.

### Address / association / group object table (OT 1, 2, 9)

Only the type independent PIDs, in practice 1, 5, 7, 23, 27, 28. The interesting one is 5.

## Load state (PID 5) and run state (PID 6)

Both live on the **application program object**; the load state also on each table object. Read values:

| PID 5 | load state | | PID 6 | run state (Table 95 p.299) |
|---|---|---|---|---|
| `00` | Unloaded | | `00` | Halted — halted or not loaded |
| `01` | Loaded | | `01` | Running |
| `02` | Loading | | `02` | Ready — loaded, starts when the run conditions hold |
| `03` | Error | | `03` | Terminated — does not start again by itself (optional state) |
| `04` | Unloading | | `04` | Starting (only required if start-up takes > 2 s) |
| `05` | Load completing | | `05` | Shutting down (same condition) |

Writing PID 5 takes a **10 octet** load event (`00` NOP, `01` Start loading, `02` Load completed,
`03` Additional load controls, `04` Unload). Writing PID 6 takes **1 octet**: `00` NOP, `01` Restart,
`02` Stop (03_05_01 §4.24.2.3.1/§4.24.2.3.2, Tables 95/96 p.299).

This stack answers PID 6 with `00` while the load state is not `Loaded`, otherwise `01` or `03` — the two
optional states Starting and Shutting down are never reported because start-up and shutdown are immediate.

`ftc <pa> runstate [start|stop]` wraps this: it resolves the application program object by type, reads
PID 6, and with an argument writes `01`/`02`. Stop halts group communication — the device stops sending
group telegrams and ignores incoming group reads and writes — while property and memory access stay up, so
it can always be started again.

A device that answers **0 octets** to PID 6 has not implemented the property. That is not an error on the
bus: per 08_03_07 §2.13.3 an unknown PID is answered with `nr_of_elem = 0` and no data. Measured on `5.0.8`
(firmware without the property) exactly that happens.

## Reading the output

```
$ ftc -i 11.11.0.210 busprop read 5.0.8 4 6
busprop read pa=5.0.8 objIdx=4 PID=6 startIdx=1 noe=1
  -> 0 byte:
```

- `startIdx=1 noe=1` — element 1, one element. For an array property element **0** is the element count.
- `-> n byte: <hex>` — the property value as received, unformatted.
- `-> 0 byte:` — the device answered, but with no data: the property does not exist on that object, or the
  read was refused. It is **not** a timeout; a timeout says so.

Writes are refused the same way — the device answers with `nr_of_elem = 0`, so a write that reports 0 bytes
back did not take effect. Always read back after a write.

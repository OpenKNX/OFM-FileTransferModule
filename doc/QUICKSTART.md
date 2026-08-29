# Quick start

**For:** anyone operating a device. Five minutes, three front-ends, one question: **who is driving
the transfer?**

| You have | You use | Read on |
|---|---|---|
| a browser and an OpenKNX device with a web interface | the **knxOTA page** | [below](#1-browser-knxota-page) · [WEB.md](WEB.md) |
| a serial or web console on an OpenKNX device | the **`ftc` command** | [below](#2-device-console) · [CONSOLE.md](CONSOLE.md) |
| a PC and a KNXnet/IP interface | the **`ftc` binary** | [below](#3-pc-ftc-cli) · [FTC-CLI.md](FTC-CLI.md) |

All three drive the *same* client code against the *same* server in the target device. Nothing else
differs.

```
   browser ─┐
   console ─┼─▶  FileTransferClient  ──KNX──▶  FileTransferModule  ──▶  LittleFS · SD · ext-flash
   ftc-cli ─┘    (one state machine)           (in the target device)
```

## Before you start

* **The target needs the module.** A device without `OPENKNX_FTC_*` answers nothing. Check with
  `ftc <pa> info` — it prints the FTM version and the feature bits.
* **You cannot reach your own PA.** A KNX device does not process frames it sent itself. Use a second
  device or an external interface. Not a bug, a property of KNX.
* **Writing may be locked.** If the target was configured with access protection, `login` first
  ([SECURITY.md](SECURITY.md)).
* **Budget the time.** ~400 bytes per second. 43 KB config file ≈ 2 min, 1.8 MB firmware ≈ 78 min,
  the same firmware as a difference ≈ 2 min ([THROUGHPUT.md](THROUGHPUT.md)).

---

## 1. Browser: knxOTA page

Open `http://<device-ip>/knxota`. The page walks three numbered steps and only releases the next one
when the current is green.

1. **Target** — type the PA, press read. Traffic light: red = no PA, amber = PA but no access,
   green = access confirmed. Prog mode and the group-address report sit here too.
2. **File** — pick the firmware from this device's flash, SD or external flash.
3. **Transfer** — send, watch the curve, trigger the update.

Nothing is on the PC. The device you are looking at sends the firmware to the *other* device over the
bus. Details and the failure cases: [WEB.md](WEB.md).

---

## 2. Device console

On the serial console, in the web console, or through a tunnel — the same command:

```
ftc 5.0.3 info                    what is that device
ftc 5.0.3 ll                      list its files
ftc 5.0.3 login Secret99          unlock writing, if it is protected
ftc 5.0.3 send fw.bin fast        push a file from this device to it
ftc 5.0.3 apply fw.bin            make it boot that firmware
ftc 5.0.3 con                     open its console from here
```

The full command surface: [CONSOLE.md](CONSOLE.md).

---

## 3. PC: ftc-cli

One binary, no dependencies, macOS · Linux · Windows · Raspberry Pi.

```bash
ftc --discover                                   find interfaces on the LAN
ftc --ip 11.11.0.126 5.0.3 info                  fingerprint the target
ftc --ip 11.11.0.126 5.0.3 send fw.bin.gz fast   push a file
ftc --ip 11.11.0.126 5.0.3 knxota fw.bin         transfer and apply in one go
ftc --ip 11.11.0.126 5.0.3 con                   remote console
```

`--ip` is the **interface you tunnel through**, `<pa>` is the **target on the bus**. The full
reference: [FTC-CLI.md](FTC-CLI.md).

---

## Your first firmware update

The short version. The long one, including what to do when it fails: [FIRMWARE-UPDATE.md](FIRMWARE-UPDATE.md).

```bash
# 1. prepare the image (optional but worth it)
pwsh Prepare-Firmware.ps1              # offers full image, gzip or delta

# 2. transfer it
ftc --ip <interface> <pa> send firmware.bin fast

# 3. apply it -- the target reboots
ftc --ip <interface> <pa> apply firmware.bin
```

**Silence after `apply` means it worked** — the device answers nothing and reboots. An answer means
it refused, and the reason is printed.

## When something goes wrong

| Symptom | Cause |
|---|---|
| no answer at all | target has no FTM, wrong PA, or you addressed your own PA |
| `0xA0` / "login required" | access protection, stage password — run `login` |
| `0xA2` / "writes disabled" | access protection, stage blocked or prog mode — press the button or change the stage |
| transfer starts, then stalls | busy bus — use `safe` instead of `fast` |
| `apply` says triggered but nothing happens | the image is not bootable for that chip, or there is no free OTA slot; see [FIRMWARE-UPDATE.md](FIRMWARE-UPDATE.md) |

Any other code: [errorcodes.txt](errorcodes.txt).

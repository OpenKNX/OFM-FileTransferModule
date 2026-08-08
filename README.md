![Issues](https://badgen.net/github/open-issues/OpenKNX/ofm-filetransfermodule)
![Branches](https://badgen.net/github/branches/OpenKNX/ofm-filetransfermodule)
[![CodeFactor](https://www.codefactor.io/repository/github/openknx/ofm-filetransfermodule/badge)](https://www.codefactor.io/repository/github/openknx/ofm-filetransfermodule)

# FileTransfer over the KNX Bus

Add this module to make your OpenKNX device an FTC **target**: upload/download files, manage the flash and
run a firmware update — plus, optionally, an **interactive remote console** — all **over the KNX / KNXnet-IP
tunnel**. A native cross-platform host CLI (`ftc`) drives it from a PC.

## Step 1
Make shure you get this output in your build step:  
Sizes depends on your configuration, but you need the entry "Filesystem size".
```
Flash size: 2.00MB
Sketch size: 1.50MB
Filesystem size: 0.50MB
```

## Step 2
Add the Module to the OpenKnx Stack
```C++
#include <Arduino.h>
#include "OpenKNX.h"
#include "FileTransferModule.h"

void setup()
{
	const uint8_t firmwareRevision = 0;
    openknx.init(firmwareRevision);
    openknx.addModule(1, ...);
    openknx.addModule(2, FileTransferModule);
    openknx.setup();

}
```

## Step 3 — talk to it from a PC
Use the built-in **`ftc`** host CLI (in [`ftc-cli/`](ftc-cli/), cross-built for Windows / macOS / Linux). It
speaks KNXnet/IP tunnelling directly — also through third-party certified interfaces — for upload, download,
firmware update and the interactive console:
```
ftc --ip <interface-ip> <pa> ll            # list files on the device
ftc --ip <interface-ip> <pa> send <file>   # upload
ftc --ip <interface-ip> <pa> console       # interactive remote console (needs OPENKNX_FTC_CONSOLE)
```
> The older standalone [KnxFileTransferClient](https://github.com/OpenKNX/KnxFileTransferClient) still works too.

## Console & fast transfers — what you need
For a plain device to be an FTC **target** with the interactive console and the fast upload modes, you need:
- this **FileTransferModule** on branch **`ec/v1dev-ftc`** (it carries the console server + the fast/windowed modes), and
- the compile flag **`-D OPENKNX_FTC_CONSOLE`** on the target — enables the console tunnel (implies the log
  ring / web-console). Plain file transfer needs no extra flag once the module is added.

You do **not** need the tunnelling stack for a plain target: the FTC **client** and the KNXnet/IP front-end
live on the **interface / router** (knx branch `ec/ip-interface-bau07B0-wip`, TPUart `main-ec`). On the
OpenKNX common side, base **`v1dev`** is enough.

> These feature branches are **work-in-progress — for testing.** The `fast`/`windowed` (mode 1) and `forget`
> (mode 2) upload modes only work with an OpenKNX interface or router, not through third-party interfaces.
> See **[`README_FTC.md`](README_FTC.md)** for the full command set, the wire protocol and the measurements.

## Good to know
The FtpServer uses following FunctionProperties.  
These may not used by any other module.
|ObjectIndex|PropertyId|Used for|
|---|---|---|
|159|0|Format|
|159|1|Exists|
|159|2|Rename|
|159|40|File Upload|
|159|41|File Download|
|159|42|File Delete|
|159|43|File Info|
|159|44|File Upload (fast / windowed)|
|159|45|File Report (gap query)|
|159|46|Filesystem Info|
|159|80|Dir List|
|159|81|Dir Create|
|159|82|Dir Delete|
|159|90|Cancel|
|159|100|Get Version|
|159|101|Firmware Update|
|159|102|Check Features|
|159|103|Auth Challenge (with `OPENKNX_FTC_SECURITY`)|
|159|104|Auth Response (with `OPENKNX_FTC_SECURITY`)|
|159|105|Auth Logout (with `OPENKNX_FTC_SECURITY`)|
|160|1 / 2|Interactive console (with `OPENKNX_FTC_CONSOLE`)|

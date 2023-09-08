# FtpServer over KNX Bus

Implement this Module to upload/download files on your device.  

## Step 1
Add the Module to your platform.ini and set the fileystem size.  
```ini
board_build.filesystem_size = 0.5m
lib_deps = 
    [...]
	https://github.com/OpenKnx/OFM-FileTransferModule
```

Make shure you get this output in your build step:
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
    openknx.addModule(2, new FileTransferModule());
    openknx.setup();

}
```

## Step 3
You can use the [KnxFileTransferClient](https://github.com/OpenKNX/KnxFileTransferClient) to upload/download files to your device.

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
|159|80|Dir List|
|159|81|Dir Create|
|159|82|Dir Delete|
|159|90|Cancel|
|159|100|Get Version|
|159|101|Do Update|

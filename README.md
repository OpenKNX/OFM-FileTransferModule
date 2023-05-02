# FtpServer over KNX Bus

Implement this Module to upload/download files on your device.  

## Step 1
Add the Module to your platform.ini and set the fileystem size.  
```ini
board_build.filesystem_size = 0.5m
lib_deps = 
    [...]
	https://github.com/OpenKnx/OFM-FtpServer
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
#include "FtpServer.h"

void setup()
{
	const uint8_t firmwareRevision = 0;
    openknx.init(firmwareRevision);
    openknx.addModule(1, ...);
    openknx.addModule(2, new FtpServer());
    openknx.setup();

}
```

## Step 3
You can use the [FtpClient] to upload/download files to your device.

## Good to know
The FtpServer uses following FunctionProperties.  
These may not used by any other module.
|ObjectIndex|PropertyId|Used for|
|---|---|---|
|164|0|Format|
|164|1|Exists|
|164|2|End|
|164|40|File Open|
|164|41|File Write|
|164|42|File Read|
|164|43|File Info|
|164|44|File Size|
|164|45|File Close|
|164|46|File Rename|
|164|47|File Remove|
|164|80|Dir Open|
|164|81|Dir Get|
|164|82|Dir Close|
|164|83|Dir Make|
|164|84|Dir Remove|
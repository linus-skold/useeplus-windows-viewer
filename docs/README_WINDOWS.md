# Useeplus SuperCamera - Windows Driver

Windows user-mode driver for the proprietary USB camera (VID: 0x2ce3, PID: 0x3828) used in cheap video microscopes.

## Overview

This is a **user-mode driver** using WinUSB, meaning:
- ✅ No kernel driver development needed
- ✅ No driver signing required
- ✅ Easy installation and deployment
- ✅ Works on Windows 7, 8, 10, 11

The driver is based on the Linux kernel driver and uses the same proprietary protocol reverse-engineered from the camera's USB communication.

## Components

- **useeplus_camera.dll** - Reusable camera library with C API
- **camera_capture.exe** - Sample application to capture frames
- **useeplus_camera.h** - Public API header

## Quick Start

### Prerequisites

1. **Camera Hardware**
   - USB camera with VID: 0x2ce3, PID: 0x3828
   - Geek szitman supercamera or compatible device

2. **Build Tools** (choose one)
   - Visual Studio 2019 or 2022 (Community Edition is fine)
   - OR MinGW-w64
   - CMake 3.10 or later

3. **Driver Installation Tool**
   - [Zadig](https://zadig.akeo.ie/) - to install WinUSB driver

### Step 1: Install WinUSB Driver

**IMPORTANT:** You must install the WinUSB driver for the camera before using it!

1. Download and run [Zadig](https://zadig.akeo.ie/)
2. Plug in your camera
3. In Zadig:
   - Options → List All Devices
   - Select your camera from the dropdown (look for VID:2CE3 PID:3828 or "USB 2.0 Camera")
   - Select **WinUSB** as the target driver (use the up/down arrows)
   - Click **Replace Driver** or **Install Driver**
4. Wait for installation to complete
5. Close Zadig

**Note:** After installing WinUSB, the camera will no longer work with standard webcam applications. To revert, uninstall the device in Device Manager and let Windows reinstall the default driver.

### Step 2: Build the Driver

#### Using PowerShell (Recommended)
```powershell
.\build.ps1
```

#### Using PowerShell (recommended)
```powershell
.\build.ps1
```

#### Manual Build (any shell)
```cmd
mkdir build
cd build
cmake .. -G "Visual Studio 17 2022"
cmake --build . --config Release
```

**For MinGW:**
```bash
mkdir build
cd build
cmake .. -G "MinGW Makefiles"
cmake --build .
```

### Step 3: Test the Camera

```cmd
camera_capture.exe
```

This will:
- Enumerate connected cameras
- Open the first camera found
- Capture 10 JPEG frames
- Save them as `frame_000.jpg`, `frame_001.jpg`, etc.

To capture a different number of frames:
```cmd
camera_capture.exe 20
```

## API Usage

### Basic Example

```c
#include "useeplus_camera.h"
#include <stdio.h>

int main() {
    // Open camera
    CAMERA_HANDLE camera = camera_open();
    if (!camera) {
        printf("Failed to open camera: %s\n", camera_get_error());
        return 1;
    }
    
    // Start streaming
    camera_start_streaming(camera);
    
    // Read a frame
    unsigned char buffer[1024*1024];
    size_t bytes_read;
    
    int ret = camera_read_frame(camera, buffer, sizeof(buffer), 
                                &bytes_read, 5000);  // 5 second timeout
    
    if (ret == CAMERA_SUCCESS) {
        // Save JPEG frame
        FILE *fp = fopen("image.jpg", "wb");
        fwrite(buffer, 1, bytes_read, fp);
        fclose(fp);
    }
    
    // Cleanup
    camera_stop_streaming(camera);
    camera_close(camera);
    
    return 0;
}
```

### Advanced: Enumerate Devices

```c
camera_device_info_t devices[10];
int count = camera_enumerate(devices, 10);

for (int i = 0; i < count; i++) {
    printf("Camera %d: %s\n", i, devices[i].description);
    printf("  VID:PID = %04X:%04X\n", 
           devices[i].vendor_id, devices[i].product_id);
    printf("  Path: %s\n", devices[i].device_path);
}

// Open specific camera
CAMERA_HANDLE camera = camera_open_path(devices[0].device_path);
```

## API Reference

### Functions

| Function | Description |
|----------|-------------|
| `camera_enumerate()` | List all connected cameras |
| `camera_open()` | Open first available camera |
| `camera_open_path()` | Open specific camera by path |
| `camera_close()` | Close camera and free resources |
| `camera_start_streaming()` | Start receiving frames |
| `camera_stop_streaming()` | Stop receiving frames |
| `camera_read_frame()` | Read one JPEG frame (blocking) |
| `camera_get_error()` | Get last error message |
| `camera_is_streaming()` | Check if camera is streaming |
| `camera_get_stats()` | Get frame capture statistics |

### Error Codes

| Code | Description |
|------|-------------|
| `CAMERA_SUCCESS` (0) | Operation successful |
| `CAMERA_ERROR_NOT_FOUND` | No camera found |
| `CAMERA_ERROR_OPEN_FAILED` | Failed to open device |
| `CAMERA_ERROR_INIT_FAILED` | Initialization failed |
| `CAMERA_ERROR_NO_FRAME` | No frame available |
| `CAMERA_ERROR_BUFFER_SMALL` | Provided buffer too small |
| `CAMERA_ERROR_INVALID_PARAM` | Invalid parameter |
| `CAMERA_ERROR_USB_FAILED` | USB communication error |
| `CAMERA_ERROR_TIMEOUT` | Operation timed out |

## Troubleshooting

### "No cameras found"

**Causes:**
1. Camera not plugged in → Plug it in and try again
2. WinUSB driver not installed → See Step 1 above
3. Wrong device selected in Zadig → Verify VID:2CE3 PID:3828

**Solution:**
- Open Device Manager (devmgmt.msc)
- Look for "Universal Serial Bus devices" → "USB 2.0 Camera" or similar
- If it shows a yellow warning, reinstall WinUSB driver using Zadig

### "Failed to open camera"

**Causes:**
- Another application has the camera open
- Insufficient permissions

**Solution:**
- Close any other applications using the camera
- Try running as Administrator

### "Timeout waiting for frame"

**Causes:**
- Camera not streaming properly
- USB communication issue

**Solution:**
- Unplug and replug the camera
- Try a different USB port (preferably USB 2.0)
- Check cables and connections

### Build errors with Visual Studio

**Error:** "Cannot find Visual Studio 2022"

**Solution:**
```cmd
# For Visual Studio 2019
cmake .. -G "Visual Studio 16 2019"

# For Visual Studio 2017
cmake .. -G "Visual Studio 15 2017"
```

**Error:** "Cannot find WinUSB.h"

**Solution:**
- Install Windows SDK (included with Visual Studio)
- Or install standalone: [Windows SDK](https://developer.microsoft.com/en-us/windows/downloads/windows-sdk/)

### Reverting to Original Camera Driver

If you want to use the camera with other applications again:

1. Open Device Manager
2. Find the camera under "Universal Serial Bus devices"
3. Right-click → Uninstall device
4. Check "Delete driver software" → Uninstall
5. Unplug and replug the camera
6. Windows will reinstall the default driver

## Technical Details

### Protocol

The camera uses a **proprietary USB protocol** (not UVC):

- **Interface:** 1, alternate setting 1
- **Endpoints:** Bulk IN (0x81), Bulk OUT (0x01)
- **Init command:** `BB AA 05 00 00`
- **Packet format:** 12-byte header `AA BB 07 ...` + JPEG payload
- **Frame boundary:** JPEG SOI (`FF D8`) and EOI (`FF D9`) markers

### Frame Processing

1. Camera sends data in bulk transfers
2. Each packet has a 12-byte proprietary header
3. Payload contains JPEG data (may span multiple packets)
4. Driver accumulates data until JPEG EOI marker or 64KB size
5. Complete frames are queued for `camera_read_frame()`

### Thread Safety

- **Thread-safe:** `camera_read_frame()`, `camera_get_error()`, `camera_get_stats()`
- **Not thread-safe:** Opening/closing camera from multiple threads
- Background USB reading happens in a separate thread automatically

## Comparison with Linux Driver

| Feature | Linux Driver | Windows Driver |
|---------|-------------|----------------|
| Type | Kernel module | User-mode DLL |
| API | Character device (`/dev/supercamera`) | C function calls |
| Installation | `insmod`, driver signing | Zadig (WinUSB) |
| Protocol | Same | Same |
| Performance | Slightly faster | Slightly slower |
| Complexity | High | Low |
| Debugging | `dmesg`, kernel debugging | Standard debugging |

## Building for Distribution

### Static Linking (Advanced)

To create a standalone EXE without DLL dependency:

```cmake
# In CMakeLists.txt, change:
add_library(useeplus_camera STATIC ...)  # SHARED → STATIC

# Then build
cmake --build . --config Release
```

### Installer

You can create an installer using:
- NSIS (Nullsoft Scriptable Install System)
- WiX Toolset
- Inno Setup

Include:
- `useeplus_camera.dll` and `camera_capture.exe`
- Instructions for installing WinUSB driver (or automate with [libwdi](https://github.com/pbatard/libwdi))

## Contributing

Based on the Linux kernel driver by Mustafa Akcanca. Windows port maintains the same protocol handling and frame extraction logic.

## License

GPL (same as Linux driver)

## Support

For issues specific to the Windows port, check:
1. WinUSB driver is correctly installed
2. Camera works with Linux driver (if dual-booting)
3. Try different USB ports
4. Check Windows Event Viewer for USB errors

For protocol-level issues, refer to the original Linux driver documentation (README.md).

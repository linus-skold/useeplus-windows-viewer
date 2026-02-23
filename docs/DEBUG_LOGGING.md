# Debug Logging Quick Reference

## Overview

The Useeplus Camera driver includes a comprehensive debug logging system to help diagnose issues and report bugs.

## How to Enable

### Method 1: Environment Variable (All Applications)

Set before running any application:
```cmd
set USEEPLUS_DEBUG=1
live_viewer.exe
```

Or in PowerShell:
```powershell
$env:USEEPLUS_DEBUG = "1"
.\live_viewer.exe
```

### Method 2: Command-Line Flag (Per Application)

Run viewers with debug flag:
```cmd
live_viewer.exe --debug
live_viewer_imgui.exe -d
camera_capture.exe --debug
```

### Method 3: API (In Your Code)

```c
#include "useeplus_camera.h"

int main() {
    // Enable debug logging
    camera_set_debug_logging(true);
    
    // Your camera code here
    CAMERA_HANDLE cam = camera_open();
    // ...
    
    // Disable debug logging when done
    camera_set_debug_logging(false);
    
    return 0;
}
```

### Method 4: Check Status

```c
if (camera_is_debug_logging_enabled()) {
    printf("Debug logging is active\n");
}
```

## What Gets Logged

Debug logs are written to **`useeplus_debug.log`** in the current directory:

### Session Information
- Start/stop timestamps
- Session boundaries with separators

### USB Operations
- Device enumeration
- Device open/close with handle addresses
- WinUSB initialization success/failure
- Pipe abort, flush, reset operations
- USB cleanup sequences

### Streaming Events
- Stream start/stop
- Thread creation/termination
- Alternate interface settings

### Frame Capture
```
[12:34:56.789][TID:1234] process_data: Complete frame detected, size=12345 bytes
```
- Frame completion with size
- Frame timestamps
- Buffer overflow warnings
- Frame too large warnings

### Errors and Warnings
```
[12:34:56.789][TID:1234] camera_open_path: ERROR - CreateFileA failed: error 32 (0x20)
[12:34:56.790][TID:5678] process_data: WARNING - Frame dropped (buffer full), total_dropped=5
```
- USB errors with error codes
- Frame drops
- Buffer overflows
- Timeout events

## Log Format

Each log entry includes:
- **Timestamp**: `[HH:MM:SS.mmm]` - Precise timing
- **Thread ID**: `[TID:xxxx]` - Which thread logged the event
- **Function**: Where the log originated
- **Message**: Detailed diagnostic information

Example:
```
[14:23:45.123][TID:4567] camera_start_streaming: Starting streaming on handle 0x000001A2B3C4D5E6
[14:23:45.234][TID:4567] camera_start_streaming: Streaming started successfully
[14:23:45.345][TID:8901] read_thread_proc: Read thread started
[14:23:45.456][TID:8901] process_data: Complete frame detected, size=11234 bytes
```

## Log Files

When using viewers, you'll get two log files:

1. **`useeplus_debug.log`** - Driver diagnostics (USB, threading, frame processing)
2. **`frame_timing.log`** - Frame capture/display timing (performance analysis)

## Reporting Issues

When reporting issues, **please include both log files**:

1. Enable debug logging
2. Reproduce the issue
3. Attach to your bug report:
   - `useeplus_debug.log`
   - `frame_timing.log` (if using viewers)
   - Screenshot or description of issue
   - Windows version
   - USB chipset/controller info (if known)

## Performance Impact

Debug logging has minimal performance impact:
- Logs are written with buffered I/O
- Critical sections protect file writes
- File is flushed after each write for reliability
- Enable only when diagnosing issues

## Thread Safety

The logging system is fully thread-safe:
- Uses critical section (`g_log_lock`) for synchronization
- Multiple threads can log simultaneously without corruption
- Thread ID included in each entry for debugging threading issues

## Privacy Note

Debug logs contain:
- USB device paths
- Timestamps
- Frame sizes and counts
- Error messages

They do **not** contain:
- Image data
- Frame contents
- Personal information

Safe to share when reporting issues.

## Example Session

```
========================================
Useeplus Camera Debug Log
Session started: 2026-02-23 14:30:45
========================================
[14:30:45.123][TID:1234] camera_enumerate: Starting enumeration (max_devices=10)
[14:30:45.234][TID:1234] camera_open_path: Opening camera at '\\?\usb#vid_2ce3...'
[14:30:45.345][TID:1234] camera_open_path: CreateFileA succeeded, handle = 0x000001A2B3C4D5E6
[14:30:45.456][TID:1234] camera_open_path: WinUsb_Initialize succeeded! Handle = 0x000001B2C3D4E5F6
[14:30:45.567][TID:1234] camera_start_streaming: Starting streaming on handle 0x000001A2B3C4D5E6
[14:30:45.678][TID:1234] camera_start_streaming: Creating read thread
[14:30:45.789][TID:1234] camera_start_streaming: Streaming started successfully
[14:30:45.890][TID:5678] read_thread_proc: Read thread started
[14:30:46.001][TID:5678] process_data: Complete frame detected, size=11234 bytes
[14:30:46.112][TID:5678] process_data: Complete frame detected, size=11456 bytes
...
[14:35:00.123][TID:1234] camera_stop_streaming: Stopping streaming on handle 0x000001A2B3C4D5E6
[14:35:00.234][TID:1234] camera_close: Closing camera handle 0x000001A2B3C4D5E6
[14:35:00.345][TID:1234] camera_close: Beginning USB cleanup sequence
[14:35:00.456][TID:1234] camera_close: Aborting pipes
[14:35:00.567][TID:1234] camera_close: Freeing WinUSB and device handles
[14:35:00.678][TID:1234] camera_close: Camera closed successfully
========================================
Debug logging disabled
========================================
```

## Troubleshooting the Logger

**Log file not created:**
- Check write permissions in current directory
- Verify `camera_set_debug_logging(true)` returns `CAMERA_SUCCESS`
- Check `camera_get_error()` for error message

**Empty log file:**
- Ensure debug logging is enabled **before** calling other camera functions
- Environment variable must be set before application starts
- Command-line flag must be present in `lpCmdLine`

**Logs missing operations:**
- Some operations only log on errors
- Normal timeouts don't log (to avoid log spam)
- Increase verbosity by checking `g_debug_logging_enabled` in critical paths

## See Also

- [README.md](../README.md#debug-logging) - Main documentation
- [CONTRIBUTING.md](../CONTRIBUTING.md#reporting-issues) - How to report issues
- [CHANGELOG.md](../CHANGELOG.md) - Debug logging feature history

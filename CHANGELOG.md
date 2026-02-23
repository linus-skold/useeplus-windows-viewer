# Changelog

All notable changes and improvements to this project are documented in this file.

**Original Source:** This is a Windows port derived from [MAkcanca/useeplus-linux-driver](https://github.com/MAkcanca/useeplus-linux-driver/blob/main/supercamera_simple.c). The changes below document improvements and modifications made to the original Linux kernel driver for Windows user-mode operation.

## [Unreleased] - 2026-02-23

### Major Improvements

#### Frame Display Issues Fixed
- **Fixed severe flickering** in live viewer making image understanding difficult
  - Implemented double buffering with backbuffer (HDC/HBITMAP)
  - Added `WM_ERASEBKGND` handler returning 1 to prevent background erase flicker
  - Added `CS_OWNDC` window style for reduced flicker
  - Removed timer-based forced repaints that caused synchronization issues

#### Frame Parsing Improvements
- **Completely rewrote frame boundary detection** to fix corrupted/stitched images
  - Proper JPEG SOI (FF D8) detection to identify new frame starts
  - Complete buffer search for EOI markers (FF D9) instead of partial scans
  - Leftover data validation - only kept if starts with SOI
  - Added overflow protection (MAX_JPEG_SIZE = BUFFER_SIZE - 4096)
  - Fixed race condition where timing logic was outside critical section

#### USB Communication Enhancements
- **Improved camera cleanup for reliable reopening** - no longer requires replug
  - Added comprehensive pipe abort sequence before thread termination
  - Proper flush and reset of both IN and OUT pipes
  - Reset to alternate setting 0 before close with appropriate delays
  - Total cleanup time ~250ms with multiple Sleep() delays for USB stabilization
  - Added state clearing on camera_open() to handle stale data from previous sessions

#### Performance Optimizations
- **Rendering performance** improved significantly
  - Changed from `HighQualityBicubic` to `Bilinear` interpolation (31-47ms vs 62ms+)
  - Added `HighSpeed` composition and smoothing modes in GDI+
  - Eliminated message queue buildup with `g_paint_pending` flag
  - Prevents multiple `InvalidateRect` calls from queuing

#### Frame Smoothing System
- **Implemented circular buffer for smooth playback** despite camera hardware limitations
  - Camera has inherent 600ms delay every 16 frames (keyframe generation in firmware)
  - 32-frame circular buffer (configurable SMOOTHING_BUFFER_SIZE)
  - Timer-driven display at consistent intervals independent of capture timing
  - Adjustable display rate: 60-100ms intervals (10-16 fps smooth playback)
  - Absorbs irregular capture timing while maintaining smooth visual output
  - Adds ~1-2 seconds of latency but eliminates visible stuttering

#### Advanced Viewer with ImGui
- **Created live_viewer_imgui.cpp** with real-time adjustable parameters
  - DirectX11 + ImGui rendering pipeline
  - Windows Imaging Component (WIC) for JPEG decoding
  - **Adjustable Display FPS slider** (5-30 fps) - control latency vs smoothness
  - **Buffer Size slider** (2-32 frames) - tune frame smoothing
  - **Real-time statistics** - capture FPS, display FPS, buffer level
  - **Enable/Disable logging** toggle
  - Interactive UI for parameter tuning without recompiling
  - Press 'H' to toggle controls on/off

### USB & Driver Improvements

#### USB Optimization
- Added read timeouts (1000ms) to prevent infinite blocking
- Enabled RAW_IO pipe policy for better performance
- Increased ring buffer from 5 to 12 frames to handle camera's 10-frame internal buffer
- Added interface diagnostics on startup
- Reset to alt setting 0 then 1 for clean state initialization

#### Diagnostic & Logging
- **Comprehensive debug logging system** for diagnostics and bug reporting
  - Enable via environment variable: `USEEPLUS_DEBUG=1`
  - Enable via command-line flags: `--debug` or `-d` in viewers
  - Enable via API: `camera_set_debug_logging(true)`
  - Logs to `useeplus_debug.log` with timestamps and thread IDs
  - Detailed USB operations, frame capture, errors, and timing
  - Thread-safe logging with critical section protection
  - Helps users report issues with comprehensive diagnostic data
- **Comprehensive frame timing logging** system
  - Logs CAPTURE events with interval, size, and buffer level
  - Logs PAINT events with wait, copy, decode, render, and total times
  - Warnings for long capture intervals (>100ms) and slow paints (>50ms)
  - Helps diagnose performance bottlenecks
  - Output to `frame_timing.log`

### Code Organization

#### Project Restructuring
- Reorganized codebase into proper directory structure:
  - `src/` - Library source code
  - `include/` - Public headers
  - `examples/` - Example applications (camera_capture, live_viewer, live_viewer_imgui)
  - `tools/` - Diagnostic and testing tools
  - `docs/` - Documentation files
- Updated CMakeLists.txt with clear sections and proper include paths
- Created comprehensive .gitignore for build artifacts and output files

#### Documentation
- New comprehensive README.md with:
  - Project structure overview
  - Quick start guide
  - Feature descriptions
  - API examples
  - Performance tips
  - Troubleshooting section
- Organized existing Windows documentation into docs/ folder

### Build System
- Updated CMakeLists.txt for new directory structure
- Added ImGui integration via FetchContent
- Separate targets for live_viewer (GDI+) and live_viewer_imgui (DirectX11)
- Clear build status messages showing all targets

### Technical Details

#### Camera Hardware Limitations Identified
- **Stutters every 16 frames** - Hardware limitation, not fixable at driver level
  - Camera firmware generates keyframes every 16 frames
  - Keyframe generation takes ~600ms (vs ~62ms for P-frames)
  - Frame size pattern: 8.7KB keyframes growing to 12KB+ P-frames
  - Frame smoothing system designed to hide this limitation

#### Memory Management
- All dynamic allocations properly freed in cleanup paths
- Critical sections properly initialized and deleted
- Event handles closed appropriately
- No memory leaks detected in normal operation

### API Additions
- `camera_read_frame()` timeout parameter now properly utilized
- Better error reporting through `camera_get_error()`
- Thread-safe frame buffer access with proper critical sections

### Known Issues & Limitations
- Camera stutters every 16 frames (hardware limitation - 600ms keyframe generation)
  - Mitigated by frame smoothing buffer in viewers
- First frame after opening may be corrupted (expected behavior)
- Reopen may require 2-3 second wait on some systems (Windows USB stack delay)

### Breaking Changes
- None - API remains backward compatible

### Files Added
- `examples/live_viewer_imgui.cpp` - Advanced viewer with ImGui controls
- `.gitignore` - Comprehensive ignore rules
- `CHANGELOG.md` - This file

### Files Moved
- `useeplus_camera.c` → `src/useeplus_camera.c`
- `useeplus_camera.h` → `include/useeplus_camera.h`
- `camera_capture.c` → `examples/camera_capture.c`
- `live_viewer.cpp` → `examples/live_viewer.cpp`
- `diagnostic.c` → `tools/diagnostic.c`
- `simple_winusb_test.c` → `tools/simple_winusb_test.c`
- Documentation → `docs/` folder

### Dependencies Added
- ImGui v1.91.5 (fetched automatically via CMake)
- DirectX 11 (for live_viewer_imgui)
- Windows Imaging Component (for JPEG decoding in ImGui viewer)

---

## [Initial Release] - Original Windows Port

**Based on:** [MAkcanca/useeplus-linux-driver](https://github.com/MAkcanca/useeplus-linux-driver/blob/main/supercamera_simple.c)

### Ported from Linux to Windows
- Converted from Linux kernel driver to Windows user-mode driver
- Replaced Linux URB system with WinUSB API
- Replaced kernel synchronization primitives (wait_queue, spinlock) with Windows equivalents (Events, CRITICAL_SECTION)
- Adapted USB initialization from usb_set_interface to WinUsb_SetCurrentAlternateSetting
- Converted bulk transfers from usb_submit_urb to WinUsb_ReadPipe/WritePipe
- Replaced kernel memory allocation (kmalloc/kfree) with standard malloc/free

### Added
- Windows user-mode driver using WinUSB
- Device enumeration via SetupAPI
- Frame capture with proprietary protocol parsing
- Basic camera_capture.exe example
- CMake build system with Visual Studio support
- Initial documentation (README_WINDOWS.md)

### Protocol Implementation (Ported)
- USB initialization via WinUSB
- Init command sequence (BB AA 05 00 00)
- Bulk transfer handling
- 12-byte header parsing (AA BB 07)
- JPEG SOI/EOI detection
- Ring buffer for frame queuing

---

## Future Improvements (Potential)

- [ ] Add frame rate statistics to simple viewer
- [ ] Configuration file for saving user preferences
- [ ] Multiple camera support
- [ ] Frame interpolation during stutters
- [ ] Exposure/brightness controls (if supported by hardware)
- [ ] Recording to video file capability

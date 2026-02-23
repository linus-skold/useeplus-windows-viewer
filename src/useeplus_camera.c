/**
 * Useeplus SuperCamera Windows Driver - Core Implementation
 * 
 * WinUSB-based user-mode driver for Useeplus SuperCamera (VID: 0x2ce3, PID: 0x3828)
 * 
 * Originally derived from:
 * https://github.com/MAkcanca/useeplus-linux-driver/blob/main/supercamera_simple.c
 * 
 * This Windows port includes significant changes and improvements:
 * - Ported from Linux kernel driver to Windows user-mode WinUSB
 * - Robust frame boundary detection with full SOI/EOI validation
 * - Improved USB cleanup for reliable camera reopening
 * - Optimized bulk read with timeouts and RAW_IO mode
 * - Expanded ring buffer (12 frames) to handle camera's internal buffering
 * - Comprehensive pipe abort/flush/reset sequences
 * - State clearing on open to handle stale data from previous sessions
 * 
 * See CHANGELOG.md for complete list of changes and improvements.
 * 
 * Licensed under GPLv3 (same as original)
 */

#include "useeplus_camera.h"

#include <windows.h>
#include <setupapi.h>
#include <initguid.h>
#include <winusb.h>
#include <usb.h>
#include <usbiodef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

// Suppress MSVC security warnings
#pragma warning(disable: 4996)

// Define min macro for compatibility
#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif

// WinUSB device interface GUID (used by Zadig)
// {dee824ef-729b-4a0e-9c14-b7117d33a817}
DEFINE_GUID(GUID_DEVINTERFACE_WINUSB,
    0xdee824ef, 0x729b, 0x4a0e, 0x9c, 0x14, 0xb7, 0x11, 0x7d, 0x33, 0xa8, 0x17);

#pragma comment(lib, "winusb.lib")
#pragma comment(lib, "setupapi.lib")

// Camera device identifiers
#define VENDOR_ID  0x2ce3
#define PRODUCT_ID 0x3828

// USB interface and endpoints
#define INTERFACE_NUM 1
#define EP_IN  0x81
#define EP_OUT 0x01

// Protocol constants
#define CONNECT_CMD_SIZE 5
#define BUFFER_SIZE (64*1024)
#define MAX_FRAMES 12  // Camera has 10-frame buffer, use 12 for safety margin

// Frame buffer structure
typedef struct camera_frame {
    unsigned char *data;
    size_t size;
    size_t capacity;
    bool ready;
} camera_frame_t;

// Camera device structure
typedef struct camera_device {
    // USB handles
    HANDLE device_handle;
    WINUSB_INTERFACE_HANDLE winusb_handle;
    char device_path[256];
    
    // Streaming state
    bool streaming;
    HANDLE read_thread;
    HANDLE stop_event;
    
    // Frame ring buffer
    camera_frame_t frames[MAX_FRAMES];
    int write_frame;  // Where new data is written
    int read_frame;   // Where data is read from
    CRITICAL_SECTION frame_lock;
    HANDLE frame_ready_event;
    
    // Statistics
    unsigned int frames_captured;
    unsigned int frames_dropped;
    
    // Connection command
    unsigned char connect_cmd[CONNECT_CMD_SIZE];
} camera_device_t;

// Thread-local error storage
static __declspec(thread) char last_error[256] = {0};

// Debug logging state
static bool g_debug_logging_enabled = false;
static FILE *g_debug_log_file = NULL;
static CRITICAL_SECTION g_log_lock;
static bool g_log_lock_initialized = false;

// Forward declarations
static DWORD WINAPI read_thread_proc(LPVOID param);
static void process_data(camera_device_t *dev, unsigned char *data, int length);
static int send_command(camera_device_t *dev, unsigned char *data, int len);
static void init_debug_logging(void);

// Set last error message
static void set_error(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(last_error, sizeof(last_error), format, args);
    va_end(args);
}

// Initialize debug logging system
static void init_debug_logging(void) {
    if (!g_log_lock_initialized) {
        InitializeCriticalSection(&g_log_lock);
        g_log_lock_initialized = true;
        
        // Check environment variable USEEPLUS_DEBUG
        char *env_debug = getenv("USEEPLUS_DEBUG");
        if (env_debug && (strcmp(env_debug, "1") == 0 || 
                          _stricmp(env_debug, "true") == 0 ||
                          _stricmp(env_debug, "yes") == 0)) {
            camera_set_debug_logging(true);
        }
    }
}

// Write debug log entry with timestamp and thread ID
static void debug_log(const char *format, ...) {
    if (!g_debug_logging_enabled || !g_debug_log_file) {
        return;
    }
    
    EnterCriticalSection(&g_log_lock);
    
    // Get timestamp
    SYSTEMTIME st;
    GetLocalTime(&st);
    
    // Get thread ID
    DWORD thread_id = GetCurrentThreadId();
    
    // Write timestamp and thread ID
    fprintf(g_debug_log_file, "[%02d:%02d:%02d.%03d][TID:%lu] ", 
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, thread_id);
    
    // Write log message
    va_list args;
    va_start(args, format);
    vfprintf(g_debug_log_file, format, args);
    va_end(args);
    
    fprintf(g_debug_log_file, "\n");
    fflush(g_debug_log_file);
    
    LeaveCriticalSection(&g_log_lock);
}

// Enable/disable debug logging
CAMERA_API int camera_set_debug_logging(bool enable) {
    init_debug_logging();
    
    EnterCriticalSection(&g_log_lock);
    
    if (enable && !g_debug_logging_enabled) {
        // Open log file
        g_debug_log_file = fopen("useeplus_debug.log", "a");
        if (!g_debug_log_file) {
            LeaveCriticalSection(&g_log_lock);
            set_error("Failed to open debug log file");
            return CAMERA_ERROR_INVALID_PARAM;
        }
        
        g_debug_logging_enabled = true;
        
        // Write header
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(g_debug_log_file, "\n");
        fprintf(g_debug_log_file, "========================================\n");
        fprintf(g_debug_log_file, "Useeplus Camera Debug Log\n");
        fprintf(g_debug_log_file, "Session started: %04d-%02d-%02d %02d:%02d:%02d\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        fprintf(g_debug_log_file, "========================================\n");
        fflush(g_debug_log_file);
        
    } else if (!enable && g_debug_logging_enabled) {
        // Close log file
        if (g_debug_log_file) {
            fprintf(g_debug_log_file, "========================================\n");
            fprintf(g_debug_log_file, "Debug logging disabled\n");
            fprintf(g_debug_log_file, "========================================\n\n");
            fclose(g_debug_log_file);
            g_debug_log_file = NULL;
        }
        g_debug_logging_enabled = false;
    }
    
    LeaveCriticalSection(&g_log_lock);
    return CAMERA_SUCCESS;
}

// Check if debug logging is enabled
CAMERA_API bool camera_is_debug_logging_enabled(void) {
    return g_debug_logging_enabled;
}

// Enumerate cameras using SetupAPI
CAMERA_API int camera_enumerate(camera_device_info_t *devices, int max_devices) {
    HDEVINFO device_info_set;
    SP_DEVICE_INTERFACE_DATA device_interface_data;
    SP_DEVINFO_DATA device_info_data;
    DWORD index = 0;
    int found_count = 0;
    
    init_debug_logging();
    debug_log("camera_enumerate: Starting enumeration (max_devices=%d)", max_devices);
    
    // First try WinUSB device interface (what Zadig installs)
    device_info_set = SetupDiGetClassDevsA(&GUID_DEVINTERFACE_WINUSB,
                                           NULL, NULL,
                                           DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    
    if (device_info_set != INVALID_HANDLE_VALUE) {
        device_interface_data.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
        
        // Enumerate all WinUSB devices
        while (SetupDiEnumDeviceInterfaces(device_info_set, NULL,
                                           &GUID_DEVINTERFACE_WINUSB,
                                           index++, &device_interface_data)) {
            
            DWORD required_size = 0;
            
            // Get required buffer size
            SetupDiGetDeviceInterfaceDetailA(device_info_set, &device_interface_data,
                                            NULL, 0, &required_size, NULL);
            
            PSP_DEVICE_INTERFACE_DETAIL_DATA_A detail_data =
                (PSP_DEVICE_INTERFACE_DETAIL_DATA_A)malloc(required_size);
            
            if (!detail_data) continue;
            
            detail_data->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
            device_info_data.cbSize = sizeof(SP_DEVINFO_DATA);
            
            // Get device path
            if (SetupDiGetDeviceInterfaceDetailA(device_info_set, &device_interface_data,
                                                detail_data, required_size,
                                                NULL, &device_info_data)) {
                
                // Check if this is our camera by VID/PID in device path
                // Convert to lowercase for comparison
                char path_lower[512];
                strncpy(path_lower, detail_data->DevicePath, sizeof(path_lower) - 1);
                path_lower[sizeof(path_lower) - 1] = '\0';
                for (char *p = path_lower; *p; p++) *p = tolower(*p);
                
                // Must have our VID/PID AND be interface 1 (mi_01)
                if (strstr(path_lower, "vid_2ce3") && 
                    strstr(path_lower, "pid_3828") &&
                    strstr(path_lower, "mi_01")) {
                    if (devices && found_count < max_devices) {
                        devices[found_count].vendor_id = VENDOR_ID;
                        devices[found_count].product_id = PRODUCT_ID;
                        strncpy(devices[found_count].device_path,
                               detail_data->DevicePath,
                               sizeof(devices[found_count].device_path) - 1);
                        devices[found_count].device_path[sizeof(devices[found_count].device_path) - 1] = '\0';
                        strncpy(devices[found_count].description,
                               "Useeplus SuperCamera (WinUSB)",
                               sizeof(devices[found_count].description) - 1);
                        devices[found_count].description[sizeof(devices[found_count].description) - 1] = '\0';
                    }
                    found_count++;
                }
            }
            
            free(detail_data);
        }
        
        SetupDiDestroyDeviceInfoList(device_info_set);
    }
    
    // If we found cameras, return
    if (found_count > 0) {
        return found_count;
    }
    
    // Fallback: try generic USB device interface (might not work with WinUSB)
    device_info_set = SetupDiGetClassDevsA(&GUID_DEVINTERFACE_USB_DEVICE,
                                           NULL, NULL,
                                           DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    
    if (device_info_set == INVALID_HANDLE_VALUE) {
        set_error("Failed to enumerate devices");
        return CAMERA_ERROR_NOT_FOUND;
    }
    
    device_interface_data.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
    index = 0;
    
    // Enumerate all devices
    while (SetupDiEnumDeviceInterfaces(device_info_set, NULL,
                                       &GUID_DEVINTERFACE_USB_DEVICE,
                                       index++, &device_interface_data)) {
        
        DWORD required_size = 0;
        
        // Get required buffer size
        SetupDiGetDeviceInterfaceDetailA(device_info_set, &device_interface_data,
                                        NULL, 0, &required_size, NULL);
        
        PSP_DEVICE_INTERFACE_DETAIL_DATA_A detail_data =
            (PSP_DEVICE_INTERFACE_DETAIL_DATA_A)malloc(required_size);
        
        if (!detail_data) continue;
        
        detail_data->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        device_info_data.cbSize = sizeof(SP_DEVINFO_DATA);
        
        // Get device path
        if (SetupDiGetDeviceInterfaceDetailA(device_info_set, &device_interface_data,
                                            detail_data, required_size,
                                            NULL, &device_info_data)) {
            
            // Check if this is our camera by VID/PID in device path
            if (strstr(detail_data->DevicePath, "vid_2ce3") &&
                strstr(detail_data->DevicePath, "pid_3828")) {
                
                if (devices && found_count < max_devices) {
                    devices[found_count].vendor_id = VENDOR_ID;
                    devices[found_count].product_id = PRODUCT_ID;
                    strncpy(devices[found_count].device_path,
                           detail_data->DevicePath,
                           sizeof(devices[found_count].device_path) - 1);
                    strncpy(devices[found_count].description,
                           "Useeplus SuperCamera (non-WinUSB)",
                           sizeof(devices[found_count].description) - 1);
                }
                found_count++;
            }
        }
        
        free(detail_data);
    }
    
    SetupDiDestroyDeviceInfoList(device_info_set);
    return found_count;
}

// Open first available camera
CAMERA_API CAMERA_HANDLE camera_open(void) {
    camera_device_info_t device;
    
    if (camera_enumerate(&device, 1) <= 0) {
        set_error("No camera found");
        return NULL;
    }
    
    return camera_open_path(device.device_path);
}

// Open camera by device path
CAMERA_API CAMERA_HANDLE camera_open_path(const char *device_path) {
    camera_device_t *dev = NULL;
    BOOL result;
    USB_INTERFACE_DESCRIPTOR interface_desc;
    ULONG length_returned;
    
    init_debug_logging();
    
    if (!device_path) {
        set_error("Invalid device path");
        debug_log("camera_open_path: ERROR - Invalid device path (NULL)");
        return NULL;
    }
    
    debug_log("camera_open_path: Opening camera at '%s'", device_path);
    
    // Allocate device structure
    dev = (camera_device_t*)calloc(1, sizeof(camera_device_t));
    if (!dev) {
        set_error("Memory allocation failed");
        return NULL;
    }
    
    // Initialize device structure
    strncpy(dev->device_path, device_path, sizeof(dev->device_path) - 1);
    InitializeCriticalSection(&dev->frame_lock);
    dev->frame_ready_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    dev->stop_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    
    // Initialize connection command
    dev->connect_cmd[0] = 0xbb;
    dev->connect_cmd[1] = 0xaa;
    dev->connect_cmd[2] = 0x05;
    dev->connect_cmd[3] = 0x00;
    dev->connect_cmd[4] = 0x00;
    
    // Open device handle - WinUSB requires FILE_FLAG_OVERLAPPED
    dev->device_handle = CreateFileA(device_path,
                                     GENERIC_WRITE | GENERIC_READ,
                                     FILE_SHARE_WRITE | FILE_SHARE_READ,
                                     NULL,
                                     OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                                     NULL);
    
    if (dev->device_handle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        set_error("Failed to open device: error %lu (0x%lx)", error, error);
        debug_log("camera_open_path: ERROR - CreateFileA failed: error %lu (0x%lx)", error, error);
        goto error;
    }
    
    debug_log("camera_open_path: CreateFileA succeeded, handle = 0x%p", dev->device_handle);
    
    // Verify the handle is valid
    char debug_msg[512];
    snprintf(debug_msg, sizeof(debug_msg), "CreateFileA succeeded, handle = 0x%p", dev->device_handle);
    OutputDebugStringA(debug_msg);
    
    // Initialize WinUSB
    result = WinUsb_Initialize(dev->device_handle, &dev->winusb_handle);
    if (!result) {
        DWORD error = GetLastError();
        
        // Additional diagnostics
        snprintf(debug_msg, sizeof(debug_msg), 
                "WinUsb_Initialize failed: error %lu (0x%lx), handle was 0x%p", 
                error, error, dev->device_handle);
        OutputDebugStringA(debug_msg);
        
        set_error("WinUSB initialization failed: error %lu (0x%lx). Possible issues:\n"
                 "  1. WinUSB driver not correctly installed on Interface 1\n"
                 "  2. Another application has the device open\n"
                 "  3. Insufficient permissions (try running as Administrator)",
                 error, error);
        debug_log("camera_open_path: ERROR - WinUsb_Initialize failed: error %lu (0x%lx)", error, error);
        goto error;
    }
    
    debug_log("camera_open_path: WinUsb_Initialize succeeded! Handle = 0x%p", dev->winusb_handle);
    snprintf(debug_msg, sizeof(debug_msg), "WinUsb_Initialize succeeded! Handle = 0x%p", dev->winusb_handle);
    OutputDebugStringA(debug_msg);
    
    // Clear any stale state from previous sessions
    // Abort and flush all pipes first
    WinUsb_AbortPipe(dev->winusb_handle, EP_IN);
    WinUsb_AbortPipe(dev->winusb_handle, EP_OUT);
    WinUsb_FlushPipe(dev->winusb_handle, EP_IN);
    WinUsb_FlushPipe(dev->winusb_handle, EP_OUT);
    WinUsb_ResetPipe(dev->winusb_handle, EP_IN);
    WinUsb_ResetPipe(dev->winusb_handle, EP_OUT);
    
    // Ensure we start at alternate setting 0
    WinUsb_SetCurrentAlternateSetting(dev->winusb_handle, 0);
    Sleep(100);
    
    // Query interface to verify we have the right device
    result = WinUsb_QueryInterfaceSettings(dev->winusb_handle, 0, &interface_desc);
    if (result) {
        // Log interface info for debugging
        char debug_msg[256];
        snprintf(debug_msg, sizeof(debug_msg),
                "Camera interface: class=0x%02x, subclass=0x%02x, protocol=0x%02x, endpoints=%d",
                interface_desc.bInterfaceClass,
                interface_desc.bInterfaceSubClass,
                interface_desc.bInterfaceProtocol,
                interface_desc.bNumEndpoints);
        OutputDebugStringA(debug_msg);
    }
    
    return (CAMERA_HANDLE)dev;
    
error:
    if (dev) {
        if (dev->winusb_handle) WinUsb_Free(dev->winusb_handle);
        if (dev->device_handle != INVALID_HANDLE_VALUE) CloseHandle(dev->device_handle);
        if (dev->frame_ready_event) CloseHandle(dev->frame_ready_event);
        if (dev->stop_event) CloseHandle(dev->stop_event);
        DeleteCriticalSection(&dev->frame_lock);
        free(dev);
    }
    return NULL;
}

// Close camera
CAMERA_API void camera_close(CAMERA_HANDLE handle) {
    camera_device_t *dev = (camera_device_t*)handle;
    
    if (!dev) return;
    
    debug_log("camera_close: Closing camera handle 0x%p", handle);
    
    // Stop streaming if active
    camera_stop_streaming(handle);
    
    debug_log("camera_close: Beginning USB cleanup sequence");
    
    // Aggressive cleanup to ensure device can be reopened
    if (dev->winusb_handle) {
        // Abort any remaining transfers
        debug_log("camera_close: Aborting pipes");
        WinUsb_AbortPipe(dev->winusb_handle, EP_IN);
        WinUsb_AbortPipe(dev->winusb_handle, EP_OUT);
        Sleep(50);
        
        // Flush pipes thoroughly
        WinUsb_FlushPipe(dev->winusb_handle, EP_IN);
        WinUsb_FlushPipe(dev->winusb_handle, EP_OUT);
        Sleep(50);
        
        // Reset pipes to clear any stale state
        WinUsb_ResetPipe(dev->winusb_handle, EP_IN);
        WinUsb_ResetPipe(dev->winusb_handle, EP_OUT);
        Sleep(50);
        
        // Reset interface to default state before closing
        WinUsb_SetCurrentAlternateSetting(dev->winusb_handle, 0);
        Sleep(100);
    }
    
    debug_log("camera_close: Freeing WinUSB and device handles");
    
    // Free WinUSB
    if (dev->winusb_handle) {
        WinUsb_Free(dev->winusb_handle);
        dev->winusb_handle = NULL;
    }
    
    // Close device handle
    if (dev->device_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(dev->device_handle);
        dev->device_handle = INVALID_HANDLE_VALUE;
    }
    
    debug_log("camera_close: Camera closed successfully");
    
    // Free frame buffers
    for (int i = 0; i < MAX_FRAMES; i++) {
        free(dev->frames[i].data);
        dev->frames[i].data = NULL;
    }
    
    // Cleanup sync objects
    if (dev->frame_ready_event) {
        CloseHandle(dev->frame_ready_event);
    }
    if (dev->stop_event) {
        CloseHandle(dev->stop_event);
    }
    DeleteCriticalSection(&dev->frame_lock);
    
    free(dev);
}

// Send command to camera
static int send_command(camera_device_t *dev, unsigned char *data, int len) {
    ULONG bytes_sent;
    BOOL result;
    
    debug_log("send_command: Sending %d bytes to camera", len);
    
    result = WinUsb_WritePipe(dev->winusb_handle, EP_OUT, data, len, &bytes_sent, NULL);
    
    if (!result) {
        DWORD error = GetLastError();
        set_error("Failed to send command: %d", error);
        debug_log("send_command: ERROR - WinUsb_WritePipe failed: %lu", error);
        return CAMERA_ERROR_USB_FAILED;
    }
    
    if (bytes_sent != len) {
        set_error("Short write: %lu / %d bytes", bytes_sent, len);
        return CAMERA_ERROR_USB_FAILED;
    }
    
    return CAMERA_SUCCESS;
}

// Start streaming
CAMERA_API int camera_start_streaming(CAMERA_HANDLE handle) {
    camera_device_t *dev = (camera_device_t*)handle;
    BOOL result;
    UCHAR alt_setting = 1;
    USB_INTERFACE_DESCRIPTOR if_desc;
    
    if (!dev) {
        set_error("Invalid handle");
        debug_log("camera_start_streaming: ERROR - Invalid handle");
        return CAMERA_ERROR_INVALID_PARAM;
    }
    
    if (dev->streaming) {
        debug_log("camera_start_streaming: Already streaming, returning success");
        return CAMERA_SUCCESS;  // Already streaming
    }
    
    debug_log("camera_start_streaming: Starting streaming on handle 0x%p", handle);
    
    // Query current interface descriptor
    result = WinUsb_QueryInterfaceSettings(dev->winusb_handle, 0, &if_desc);
    if (result) {
        char debug_msg[256];
        snprintf(debug_msg, sizeof(debug_msg), 
                "Camera interface: %d alternate settings available\n", 
                if_desc.bNumEndpoints);
        OutputDebugStringA(debug_msg);
    }
    
    // Reset to alternate setting 0 first, then set to 1 (clean state)
    WinUsb_SetCurrentAlternateSetting(dev->winusb_handle, 0);
    Sleep(10);
    
    // Reset and flush the input pipe before starting
    WinUsb_ResetPipe(dev->winusb_handle, EP_IN);
    WinUsb_FlushPipe(dev->winusb_handle, EP_IN);
    
    // Set alternate interface setting to 1
    result = WinUsb_SetCurrentAlternateSetting(dev->winusb_handle, alt_setting);
    if (!result) {
        set_error("Failed to set alternate setting: %d", GetLastError());
        return CAMERA_ERROR_INIT_FAILED;
    }
    
    // Send connection command
    int ret = send_command(dev, dev->connect_cmd, CONNECT_CMD_SIZE);
    if (ret != CAMERA_SUCCESS) {
        return ret;
    }
    
    // Reset event
    ResetEvent(dev->stop_event);
    
    debug_log("camera_start_streaming: Creating read thread");
    
    // Start read thread
    dev->streaming = true;
    dev->read_thread = CreateThread(NULL, 0, read_thread_proc, dev, 0, NULL);
    
    if (!dev->read_thread) {
        dev->streaming = false;
        DWORD error = GetLastError();
        set_error("Failed to create read thread: %d", error);
        debug_log("camera_start_streaming: ERROR - Failed to create read thread: %lu", error);
        return CAMERA_ERROR_INIT_FAILED;
    }
    
    debug_log("camera_start_streaming: Streaming started successfully");
    return CAMERA_SUCCESS;
}

// Stop streaming
CAMERA_API void camera_stop_streaming(CAMERA_HANDLE handle) {
    camera_device_t *dev = (camera_device_t*)handle;
    
    if (!dev || !dev->streaming) return;
    
    debug_log("camera_stop_streaming: Stopping streaming on handle 0x%p", handle);
    
    // Signal thread to stop
    dev->streaming = false;
    SetEvent(dev->stop_event);
    
    // Abort any pending USB transfers FIRST (interrupts blocking reads)
    if (dev->winusb_handle) {
        WinUsb_AbortPipe(dev->winusb_handle, EP_IN);
    }
    
    // Wait for thread to exit with timeout
    if (dev->read_thread) {
        DWORD result = WaitForSingleObject(dev->read_thread, 2000);
        if (result == WAIT_TIMEOUT) {
            // Force terminate if still running
            TerminateThread(dev->read_thread, 0);
        }
        CloseHandle(dev->read_thread);
        dev->read_thread = NULL;
    }
    
    // Flush the USB pipe to clear any stale data
    if (dev->winusb_handle) {
        WinUsb_FlushPipe(dev->winusb_handle, EP_IN);
        WinUsb_ResetPipe(dev->winusb_handle, EP_IN);
    }
    
    // Clear all frames
    EnterCriticalSection(&dev->frame_lock);
    for (int i = 0; i < MAX_FRAMES; i++) {
        dev->frames[i].ready = false;
        dev->frames[i].size = 0;
    }
    dev->read_frame = 0;
    dev->write_frame = 0;
    ResetEvent(dev->frame_ready_event);
    LeaveCriticalSection(&dev->frame_lock);
    
    // Small delay to ensure USB operations complete
    Sleep(50);
}

// USB read thread
static DWORD WINAPI read_thread_proc(LPVOID param) {
    camera_device_t *dev = (camera_device_t*)param;
    unsigned char buffer[BUFFER_SIZE];
    ULONG bytes_read;
    BOOL result;
    ULONG timeout_ms = 1000;  // 1 second timeout
    
    debug_log("read_thread_proc: Read thread started");
    
    // Set pipe policy for timeouts
    if (dev->winusb_handle) {
        WinUsb_SetPipePolicy(dev->winusb_handle, EP_IN, PIPE_TRANSFER_TIMEOUT, 
                            sizeof(timeout_ms), &timeout_ms);
        
        // Try to enable RAW_IO for better performance
        UCHAR raw_io = TRUE;
        WinUsb_SetPipePolicy(dev->winusb_handle, EP_IN, RAW_IO, 
                            sizeof(raw_io), &raw_io);
    }
    
    while (dev->streaming) {
        // Check if we should stop
        if (WaitForSingleObject(dev->stop_event, 0) == WAIT_OBJECT_0) {
            break;
        }
        
        // Read from bulk endpoint with timeout
        result = WinUsb_ReadPipe(dev->winusb_handle, EP_IN,
                                buffer, BUFFER_SIZE, &bytes_read, NULL);
        
        if (!result) {
            DWORD error = GetLastError();
            if (error == ERROR_SEM_TIMEOUT || error == ERROR_TIMEOUT) {
                // Timeout - this is OK, just continue
                continue;
            } else if (error != ERROR_SEM_TIMEOUT) {
                // Real error occurred
                debug_log("read_thread_proc: ERROR - WinUsb_ReadPipe failed: %lu", error);
                break;
            }
            continue;
        }
        
        if (bytes_read > 0) {
            process_data(dev, buffer, bytes_read);
        }
    }
    
    return 0;
}

// Get last error
CAMERA_API const char* camera_get_error(void) {
    return last_error[0] ? last_error : "No error";
}

// Check if streaming
CAMERA_API bool camera_is_streaming(CAMERA_HANDLE handle) {
    camera_device_t *dev = (camera_device_t*)handle;
    return dev ? dev->streaming : false;
}

// Get statistics
CAMERA_API int camera_get_stats(CAMERA_HANDLE handle,
                                 unsigned int *frames_captured,
                                 unsigned int *frames_dropped) {
    camera_device_t *dev = (camera_device_t*)handle;
    
    if (!dev) {
        return CAMERA_ERROR_INVALID_PARAM;
    }
    
    if (frames_captured) *frames_captured = dev->frames_captured;
    if (frames_dropped) *frames_dropped = dev->frames_dropped;
    
    return CAMERA_SUCCESS;
}

// Process received USB data and extract JPEG frames
static void process_data(camera_device_t *dev, unsigned char *data, int length) {
    static int packet_count = 0;
    const int HEADER_SIZE = 12;  // Proprietary header is 12 bytes
    const int MIN_JPEG_SIZE = 1000;
    const int MAX_JPEG_SIZE = BUFFER_SIZE - 4096;  // Leave some headroom
    
    camera_frame_t *frame;
    
    EnterCriticalSection(&dev->frame_lock);
    frame = &dev->frames[dev->write_frame];
    
    // Debug first few packets (optional, can be removed in production)
    if (packet_count < 10) {
        char debug_buf[256] = {0};
        int offset = 0;
        offset += snprintf(debug_buf + offset, sizeof(debug_buf) - offset,
                          "Packet %d (%d bytes): ", packet_count, length);
        for (int i = 0; i < min(16, length) && offset < sizeof(debug_buf) - 10; i++) {
            offset += snprintf(debug_buf + offset, sizeof(debug_buf) - offset,
                             "%02x ", data[i]);
        }
        OutputDebugStringA(debug_buf);
        packet_count++;
    }
    
    // Check for valid packet header (AA BB 07)
    if (length >= 3 && data[0] == 0xaa && data[1] == 0xbb && data[2] == 0x07) {
        // Valid packet with proprietary header
        
        // Allocate frame buffer if needed
        if (!frame->data) {
            frame->data = (unsigned char*)malloc(BUFFER_SIZE);
            if (!frame->data) {
                LeaveCriticalSection(&dev->frame_lock);
                return;
            }
            frame->capacity = BUFFER_SIZE;
            frame->size = 0;
            frame->ready = false;
        }
        
        // Extract payload (skip 12-byte header)
        if (length > HEADER_SIZE) {
            int payload_size = length - HEADER_SIZE;
            unsigned char *payload = data + HEADER_SIZE;
            
            // Check for JPEG SOI marker (FF D8) at start of payload
            // This indicates a new frame is starting
            if (payload_size >= 2 && payload[0] == 0xFF && payload[1] == 0xD8) {
                // New JPEG starting - if we have incomplete frame, discard it
                if (frame->size > 0 && !frame->ready) {
                    // Incomplete frame - discard and start fresh
                    frame->size = 0;
                }
            }
            
            // Check if we have enough space in current frame
            if (frame->size + payload_size > frame->capacity) {
                // Buffer overflow - discard this incomplete frame and start fresh
                frame->size = 0;
                frame->ready = false;
                
                // If this packet has SOI, start new frame with it
                if (payload_size >= 2 && payload[0] == 0xFF && payload[1] == 0xD8) {
                    memcpy(frame->data, payload, payload_size);
                    frame->size = payload_size;
                }
                LeaveCriticalSection(&dev->frame_lock);
                return;
            }
            
            // Copy payload to frame buffer
            memcpy(frame->data + frame->size, payload, payload_size);
            frame->size += payload_size;
            
            // Look for JPEG EOI marker (FF D9) - search entire frame buffer
            for (size_t i = 1; i < frame->size; i++) {
                if (frame->data[i-1] == 0xFF && frame->data[i] == 0xD9) {
                    // Found potential end of JPEG
                    size_t complete_frame_size = i + 1;
                    
                    // Verify this is a complete valid JPEG (has SOI at start)
                    if (complete_frame_size >= MIN_JPEG_SIZE && 
                        frame->data[0] == 0xFF && frame->data[1] == 0xD8) {
                        
                        debug_log("process_data: Complete frame detected, size=%zu bytes", complete_frame_size);
                        
                        // Calculate leftover data (data after EOI)
                        size_t leftover = frame->size - complete_frame_size;
                        unsigned char *leftover_data = NULL;
                        
                        // Save leftover data if it exists
                        if (leftover > 0 && leftover < BUFFER_SIZE) {
                            leftover_data = (unsigned char*)malloc(leftover);
                            if (leftover_data) {
                                memcpy(leftover_data, frame->data + complete_frame_size, leftover);
                            }
                        }
                        
                        // Mark current frame as complete
                        frame->size = complete_frame_size;
                        frame->ready = true;
                        dev->frames_captured++;
                        SetEvent(dev->frame_ready_event);
                        
                        // Move to next frame slot
                        int next_write = (dev->write_frame + 1) % MAX_FRAMES;
                        
                        // Check if we're overwriting unread frames
                        if (next_write == dev->read_frame && dev->frames[dev->read_frame].ready) {
                            dev->frames_dropped++;
                            debug_log("process_data: WARNING - Frame dropped (buffer full), total_dropped=%u", dev->frames_dropped);
                            dev->read_frame = (dev->read_frame + 1) % MAX_FRAMES;
                        }
                        
                        dev->write_frame = next_write;
                        
                        // Initialize next frame
                        frame = &dev->frames[dev->write_frame];
                        if (!frame->data) {
                            frame->data = (unsigned char*)malloc(BUFFER_SIZE);
                            if (!frame->data) {
                                free(leftover_data);
                                LeaveCriticalSection(&dev->frame_lock);
                                return;
                            }
                            frame->capacity = BUFFER_SIZE;
                        }
                        
                        // Copy leftover data to new frame, but only if it looks like JPEG start
                        if (leftover_data && leftover >= 2) {
                            // Check if leftover starts with SOI marker
                            if (leftover_data[0] == 0xFF && leftover_data[1] == 0xD8) {
                                memcpy(frame->data, leftover_data, leftover);
                                frame->size = leftover;
                            } else {
                                // Leftover doesn't start a valid JPEG - discard it
                                frame->size = 0;
                            }
                            free(leftover_data);
                        } else {
                            free(leftover_data);
                            frame->size = 0;
                        }
                        frame->ready = false;
                        
                        // Found and processed EOI - stop searching
                        break;
                    }
                }
            }
            
            // Safety check: if frame is getting too large without EOI, discard it
            if (frame->size > MAX_JPEG_SIZE && !frame->ready) {
                debug_log("process_data: WARNING - Frame too large without EOI, discarding (size=%zu)", frame->size);
                frame->size = 0;
                frame->ready = false;
            }
        }
    }
    
    LeaveCriticalSection(&dev->frame_lock);
}

// Read frame - blocking call with timeout
CAMERA_API int camera_read_frame(CAMERA_HANDLE handle,
                                  unsigned char *buffer,
                                  size_t buffer_size,
                                  size_t *bytes_read,
                                  unsigned int timeout_ms) {
    camera_device_t *dev = (camera_device_t*)handle;
    camera_frame_t *frame;
    DWORD wait_result;
    DWORD timeout = timeout_ms ? timeout_ms : INFINITE;
    
    if (!dev || !buffer || !bytes_read) {
        set_error("Invalid parameters");
        return CAMERA_ERROR_INVALID_PARAM;
    }
    
    *bytes_read = 0;
    
    if (!dev->streaming) {
        set_error("Camera is not streaming");
        return CAMERA_ERROR_NO_FRAME;
    }
    
    // Wait for a frame to be ready
    while (true) {
        EnterCriticalSection(&dev->frame_lock);
        
        frame = &dev->frames[dev->read_frame];
        
        if (frame->ready) {
            // Frame is available
            if (frame->size > buffer_size) {
                LeaveCriticalSection(&dev->frame_lock);
                set_error("Buffer too small: need %zu bytes, have %zu", frame->size, buffer_size);
                return CAMERA_ERROR_BUFFER_SMALL;
            }
            
            // Copy frame data to user buffer
            memcpy(buffer, frame->data, frame->size);
            *bytes_read = frame->size;
            
            // Mark frame as consumed
            frame->ready = false;
            frame->size = 0;
            
            // Move to next frame
            dev->read_frame = (dev->read_frame + 1) % MAX_FRAMES;
            
            LeaveCriticalSection(&dev->frame_lock);
            return CAMERA_SUCCESS;
        }
        
        LeaveCriticalSection(&dev->frame_lock);
        
        // No frame ready - wait for one
        wait_result = WaitForSingleObject(dev->frame_ready_event, timeout);
        
        if (wait_result == WAIT_TIMEOUT) {
            set_error("Timeout waiting for frame");
            return CAMERA_ERROR_TIMEOUT;
        }
        
        if (wait_result != WAIT_OBJECT_0) {
            set_error("Wait failed: %d", GetLastError());
            return CAMERA_ERROR_USB_FAILED;
        }
        
        // Event was signaled, loop back to check for frame
    }
}

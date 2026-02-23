/**
 * Useeplus SuperCamera Windows Driver
 * User-mode driver for proprietary USB camera (VID: 0x2ce3, PID: 0x3828)
 * 
 * Originally derived from:
 * https://github.com/MAkcanca/useeplus-linux-driver/blob/main/supercamera_simple.c
 * 
 * This is a Windows port using WinUSB for cheap video microscopes with the
 * Geek szitman supercamera chipset. Significant architectural changes were made
 * to adapt from Linux kernel driver to Windows user-mode operation.
 * 
 * Licensed under GPLv3 (same as original)
 */

#ifndef USEEPLUS_CAMERA_H
#define USEEPLUS_CAMERA_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdbool.h>

// DLL export/import macros
#ifdef USEEPLUS_CAMERA_EXPORTS
    #define CAMERA_API __declspec(dllexport)
#else
    #define CAMERA_API __declspec(dllimport)
#endif

// Camera handle type (opaque pointer)
typedef void* CAMERA_HANDLE;

// Error codes
#define CAMERA_SUCCESS              0
#define CAMERA_ERROR_NOT_FOUND     -1
#define CAMERA_ERROR_OPEN_FAILED   -2
#define CAMERA_ERROR_INIT_FAILED   -3
#define CAMERA_ERROR_NO_FRAME      -4
#define CAMERA_ERROR_BUFFER_SMALL  -5
#define CAMERA_ERROR_INVALID_PARAM -6
#define CAMERA_ERROR_USB_FAILED    -7
#define CAMERA_ERROR_TIMEOUT       -8

// Camera device information
typedef struct {
    unsigned short vendor_id;
    unsigned short product_id;
    char device_path[256];
    char description[128];
} camera_device_info_t;

/**
 * Enumerate connected cameras
 * 
 * @param devices Array to store device information
 * @param max_devices Maximum number of devices to return
 * @return Number of devices found, or negative error code
 */
CAMERA_API int camera_enumerate(camera_device_info_t *devices, int max_devices);

/**
 * Open the first available camera
 * 
 * @return Camera handle on success, NULL on failure
 */
CAMERA_API CAMERA_HANDLE camera_open(void);

/**
 * Open a specific camera by device path
 * 
 * @param device_path Device path from camera_enumerate
 * @return Camera handle on success, NULL on failure
 */
CAMERA_API CAMERA_HANDLE camera_open_path(const char *device_path);

/**
 * Close the camera and release resources
 * 
 * @param handle Camera handle from camera_open
 */
CAMERA_API void camera_close(CAMERA_HANDLE handle);

/**
 * Start streaming from the camera
 * Camera must be opened before calling this
 * 
 * @param handle Camera handle
 * @return CAMERA_SUCCESS or error code
 */
CAMERA_API int camera_start_streaming(CAMERA_HANDLE handle);

/**
 * Stop streaming from the camera
 * 
 * @param handle Camera handle
 */
CAMERA_API void camera_stop_streaming(CAMERA_HANDLE handle);

/**
 * Read a complete JPEG frame from the camera
 * This function blocks until a frame is available or timeout occurs
 * 
 * @param handle Camera handle
 * @param buffer Buffer to store JPEG data
 * @param buffer_size Size of the buffer
 * @param bytes_read Pointer to store actual bytes read
 * @param timeout_ms Timeout in milliseconds (0 = no timeout)
 * @return CAMERA_SUCCESS or error code
 */
CAMERA_API int camera_read_frame(CAMERA_HANDLE handle, 
                                  unsigned char *buffer, 
                                  size_t buffer_size,
                                  size_t *bytes_read,
                                  unsigned int timeout_ms);

/**
 * Get the last error message
 * Thread-safe, returns error for the calling thread
 * 
 * @return Error message string
 */
CAMERA_API const char* camera_get_error(void);

/**
 * Check if the camera is currently streaming
 * 
 * @param handle Camera handle
 * @return true if streaming, false otherwise
 */
CAMERA_API bool camera_is_streaming(CAMERA_HANDLE handle);

/**
 * Get camera statistics
 * 
 * @param handle Camera handle
 * @param frames_captured Total frames captured since start
 * @param frames_dropped Frames dropped due to buffer overflow
 * @return CAMERA_SUCCESS or error code
 */
CAMERA_API int camera_get_stats(CAMERA_HANDLE handle,
                                 unsigned int *frames_captured,
                                 unsigned int *frames_dropped);

/**
 * Enable/disable debug logging to file
 * 
 * When enabled, detailed logs are written to 'useeplus_debug.log' in the current directory.
 * This includes USB operations, frame processing, timing information, and error details.
 * 
 * Logging can also be enabled by setting environment variable: USEEPLUS_DEBUG=1
 * 
 * @param enable true to enable logging, false to disable
 * @return CAMERA_SUCCESS or error code
 */
CAMERA_API int camera_set_debug_logging(bool enable);

/**
 * Check if debug logging is currently enabled
 * 
 * @return true if logging is enabled, false otherwise
 */
CAMERA_API bool camera_is_debug_logging_enabled(void);

#ifdef __cplusplus
}
#endif

#endif // USEEPLUS_CAMERA_H

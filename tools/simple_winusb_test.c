/**
 * Simple WinUSB Test - Minimal test to isolate the issue
 */

#include <windows.h>
#include <winusb.h>
#include <stdio.h>

int main() {
    // The exact path from enumeration
    const char *device_path = "\\\\?\\usb#vid_2ce3&pid_3828&mi_01#9&b37e6ff&0&0001#{dee824ef-729b-4a0e-9c14-b7117d33a817}";
    
    printf("Testing WinUSB initialization\n");
    printf("Device path: %s\n\n", device_path);
    
    // Try to open device
    printf("Step 1: Opening device with CreateFileA...\n");
    
    // WinUSB requires FILE_FLAG_OVERLAPPED
    HANDLE device_handle = CreateFileA(device_path,
                                       GENERIC_WRITE | GENERIC_READ,
                                       FILE_SHARE_WRITE | FILE_SHARE_READ,
                                       NULL,
                                       OPEN_EXISTING,
                                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                                       NULL);
    
    if (device_handle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        printf("FAILED: CreateFileA error %lu (0x%lx)\n", error, error);
        
        if (error == 2) printf("  -> File not found (device path wrong)\n");
        else if (error == 5) printf("  -> Access denied (run as Administrator)\n");
        else if (error == 32) printf("  -> Sharing violation (device in use)\n");
        
        return 1;
    }
    
    printf("SUCCESS: Device opened, handle = 0x%p\n\n", device_handle);
    
    // Try to initialize WinUSB
    printf("Step 2: Initializing WinUSB...\n");
    WINUSB_INTERFACE_HANDLE winusb_handle;
    BOOL result = WinUsb_Initialize(device_handle, &winusb_handle);
    
    if (!result) {
        DWORD error = GetLastError();
        printf("FAILED: WinUsb_Initialize error %lu (0x%lx)\n", error, error);
        
        if (error == 6) printf("  -> Invalid handle (something wrong with device handle)\n");
        else if (error == 31) printf("  -> Device not functioning\n");
        else if (error == 1167) printf("  -> Device not connected\n");
        
        CloseHandle(device_handle);
        return 1;
    }
    
    printf("SUCCESS: WinUSB initialized, handle = 0x%p\n\n", winusb_handle);
    
    // Clean up
    WinUsb_Free(winusb_handle);
    CloseHandle(device_handle);
    
    printf("All tests passed! WinUSB is working correctly.\n");
    return 0;
}

/**
 * Device Enumeration Diagnostic Tool
 * Shows all USB devices to help debug WinUSB issues
 */

#include <windows.h>
#include <setupapi.h>
#include <initguid.h>
#include <stdio.h>

// WinUSB GUID
DEFINE_GUID(GUID_DEVINTERFACE_WINUSB,
    0xdee824ef, 0x729b, 0x4a0e, 0x9c, 0x14, 0xb7, 0x11, 0x7d, 0x33, 0xa8, 0x17);

// Generic USB GUID
DEFINE_GUID(GUID_DEVINTERFACE_USB_DEVICE,
    0xA5DCBF10, 0x6530, 0x11D2, 0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED);

void enumerate_devices(const char *guid_name, GUID *guid) {
    HDEVINFO device_info_set;
    SP_DEVICE_INTERFACE_DATA device_interface_data;
    DWORD index = 0;
    int found = 0;
    
    printf("\n=== Enumerating %s ===\n", guid_name);
    
    device_info_set = SetupDiGetClassDevsA(guid, NULL, NULL,
                                           DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    
    if (device_info_set == INVALID_HANDLE_VALUE) {
        printf("Failed to get device list\n");
        return;
    }
    
    device_interface_data.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
    
    while (SetupDiEnumDeviceInterfaces(device_info_set, NULL, guid,
                                       index++, &device_interface_data)) {
        DWORD required_size = 0;
        
        SetupDiGetDeviceInterfaceDetailA(device_info_set, &device_interface_data,
                                        NULL, 0, &required_size, NULL);
        
        PSP_DEVICE_INTERFACE_DETAIL_DATA_A detail_data =
            (PSP_DEVICE_INTERFACE_DETAIL_DATA_A)malloc(required_size);
        
        if (!detail_data) continue;
        
        detail_data->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        
        if (SetupDiGetDeviceInterfaceDetailA(device_info_set, &device_interface_data,
                                            detail_data, required_size, NULL, NULL)) {
            
            // Check if this contains our VID/PID
            if (strstr(detail_data->DevicePath, "2ce3") || 
                strstr(detail_data->DevicePath, "2CE3")) {
                printf("\n[Device %d] CAMERA FOUND!\n", index - 1);
                printf("  Path: %s\n", detail_data->DevicePath);
                found++;
            } else if (index <= 10) {  // Show first 10 for reference
                printf("\n[Device %d]\n", index - 1);
                printf("  Path: %s\n", detail_data->DevicePath);
            }
        }
        
        free(detail_data);
    }
    
    SetupDiDestroyDeviceInfoList(device_info_set);
    
    if (found == 0) {
        printf("\n*** No camera devices found with this GUID ***\n");
    } else {
        printf("\n*** Found %d camera device(s) ***\n", found);
    }
}

int main() {
    printf("=================================================\n");
    printf("USB Device Enumeration Diagnostic\n");
    printf("Looking for VID:2CE3 PID:3828\n");
    printf("=================================================\n");
    
    // Enumerate using WinUSB GUID (what Zadig uses)
    enumerate_devices("WinUSB Interface", (GUID*)&GUID_DEVINTERFACE_WINUSB);
    
    // Enumerate using generic USB GUID
    enumerate_devices("Generic USB Device", (GUID*)&GUID_DEVINTERFACE_USB_DEVICE);
    
    printf("\n=================================================\n");
    printf("What to look for:\n");
    printf("- Camera should appear under WinUSB Interface\n");
    printf("- Path should contain 'vid_2ce3' and 'pid_3828'\n");
    printf("- Path should end with 'mi_01' for interface 1\n");
    printf("=================================================\n");
    
    return 0;
}

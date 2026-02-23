/**
 * Camera Capture Test Application
 * 
 * Simple Windows application that uses the useeplus_camera DLL
 * to capture JPEG frames from the camera and save them to disk.
 * 
 * Similar to the Linux simple-test.c
 */

#include "useeplus_camera.h"
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#define MAX_BUFFER_SIZE (1024*1024)  // 1MB buffer for JPEG frames

int main(int argc, char *argv[]) {
    CAMERA_HANDLE camera = NULL;
    unsigned char *buffer = NULL;
    int ret;
    int num_frames = 10;  // Default: capture 10 frames
    
    printf("Useeplus SuperCamera Capture Tool\n");
    printf("==================================\n\n");
    
    // Parse command line arguments
    if (argc > 1) {
        num_frames = atoi(argv[1]);
        if (num_frames <= 0 || num_frames > 1000) {
            fprintf(stderr, "Invalid number of frames. Using default (10).\n");
            num_frames = 10;
        }
    }
    
    // Allocate frame buffer
    buffer = (unsigned char*)malloc(MAX_BUFFER_SIZE);
    if (!buffer) {
        fprintf(stderr, "Failed to allocate buffer\n");
        return 1;
    }
    
    // Enumerate cameras
    printf("Enumerating cameras...\n");
    camera_device_info_t devices[5];
    int count = camera_enumerate(devices, 5);
    
    if (count < 0) {
        fprintf(stderr, "Failed to enumerate cameras: %s\n", camera_get_error());
        free(buffer);
        return 1;
    }
    
    if (count == 0) {
        fprintf(stderr, "No cameras found!\n");
        fprintf(stderr, "\nMake sure:\n");
        fprintf(stderr, "1. Camera is plugged in (VID:0x2ce3 PID:0x3828)\n");
        fprintf(stderr, "2. WinUSB driver is installed (use Zadig)\n");
        free(buffer);
        return 1;
    }
    
    printf("Found %d camera(s):\n", count);
    for (int i = 0; i < count; i++) {
        printf("  [%d] %s (VID:%04X PID:%04X)\n", i,
               devices[i].description,
               devices[i].vendor_id,
               devices[i].product_id);
        printf("      Path: %s\n", devices[i].device_path);
        
        // Check if this is the WinUSB interface path (has mi_01)
        if (strstr(devices[i].device_path, "mi_01")) {
            printf("      Type: WinUSB Interface 1 (CORRECT)\n");
        } else {
            printf("      Type: Generic USB (may not work with WinUSB)\n");
        }
    }
    printf("\n");
    
    // Open first camera
    printf("Opening camera...\n");
    camera = camera_open();
    if (!camera) {
        fprintf(stderr, "Failed to open camera: %s\n", camera_get_error());
        free(buffer);
        return 1;
    }
    printf("Camera opened successfully!\n\n");
    
    // Start streaming
    printf("Starting streaming...\n");
    ret = camera_start_streaming(camera);
    if (ret != CAMERA_SUCCESS) {
        fprintf(stderr, "Failed to start streaming: %s\n", camera_get_error());
        camera_close(camera);
        free(buffer);
        return 1;
    }
    printf("Streaming started!\n\n");
    
    // Capture frames
    printf("Capturing %d frames...\n", num_frames);
    int captured = 0;
    int failed = 0;
    
    for (int i = 0; i < num_frames; i++) {
        size_t bytes_read = 0;
        
        printf("  [%d/%d] Waiting for frame... ", i + 1, num_frames);
        fflush(stdout);
        
        // Read frame with 10 second timeout
        ret = camera_read_frame(camera, buffer, MAX_BUFFER_SIZE, &bytes_read, 10000);
        
        if (ret == CAMERA_SUCCESS) {
            // Verify it's a valid JPEG (starts with FF D8)
            bool is_jpeg = (bytes_read >= 2 && buffer[0] == 0xFF && buffer[1] == 0xD8);
            
            if (is_jpeg) {
                // Save to file
                char filename[64];
                sprintf(filename, "frame_%03d.jpg", i);
                
                FILE *fp = fopen(filename, "wb");
                if (fp) {
                    fwrite(buffer, 1, bytes_read, fp);
                    fclose(fp);
                    printf("OK! Saved %s (%zu bytes)\n", filename, bytes_read);
                    captured++;
                } else {
                    printf("FAILED to save file\n");
                    failed++;
                }
            } else {
                printf("FAILED - not a valid JPEG (first bytes: %02X %02X)\n",
                       bytes_read >= 1 ? buffer[0] : 0,
                       bytes_read >= 2 ? buffer[1] : 0);
                failed++;
            }
        } else if (ret == CAMERA_ERROR_TIMEOUT) {
            printf("TIMEOUT\n");
            failed++;
        } else {
            printf("ERROR: %s\n", camera_get_error());
            failed++;
        }
    }
    
    // Get statistics
    unsigned int total_frames, dropped_frames;
    camera_get_stats(camera, &total_frames, &dropped_frames);
    
    printf("\n");
    printf("Capture Summary:\n");
    printf("  Captured: %d\n", captured);
    printf("  Failed:   %d\n", failed);
    printf("  Total frames from camera: %u\n", total_frames);
    printf("  Dropped frames: %u\n", dropped_frames);
    printf("\n");
    
    // Stop streaming
    printf("Stopping streaming...\n");
    camera_stop_streaming(camera);
    
    // Close camera
    printf("Closing camera...\n");
    camera_close(camera);
    
    free(buffer);
    
    printf("Done!\n");
    return (captured > 0) ? 0 : 1;
}

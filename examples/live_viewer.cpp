/**
 * Camera Live Viewer (GDI+ Version)
 * 
 * Opens a window and displays live feed from the camera with frame smoothing.
 * 
 * Features:
 * - Double buffering for flicker-free display
 * - Circular frame buffer for smooth playback despite camera stutters
 * - Bilinear interpolation for fast rendering
 * - Adjustable via compile-time constants (see #defines below)
 * 
 * Controls:
 *   S   - Save snapshot
 *   ESC - Exit
 * 
 * Note: For runtime adjustable parameters, use live_viewer_imgui.exe instead.
 * See CHANGELOG.md for details on improvements made to eliminate flickering
 * and handle camera's 600ms keyframe generation stutters.
 */

#include "useeplus_camera.h"
#include <windows.h>
#include <gdiplus.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdlib.h>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma warning(disable: 4996)

using namespace Gdiplus;

// Frame smoothing buffer - circular buffer for consistent display rate
#define SMOOTHING_BUFFER_SIZE 12  // ~0.8 seconds buffer for smoothing
struct FrameSlot {
    unsigned char *data;
    size_t size;
    bool filled;
};

// Global variables
static CAMERA_HANDLE g_camera = NULL;
static bool g_running = true;
static FrameSlot g_frame_ring[SMOOTHING_BUFFER_SIZE];  // Circular buffer
static int g_write_pos = 0;  // Where camera writes next frame
static int g_read_pos = 0;   // Where display reads next frame
static unsigned char *g_display_buffer = NULL;  // Current frame being displayed
static size_t g_display_size = 0;
static CRITICAL_SECTION g_frame_lock;
static int g_snapshot_count = 0;
static unsigned int g_total_frames = 0;
static unsigned int g_displayed_frames = 0;
static DWORD g_start_time = 0;
static HWND g_hwnd = NULL;
static FILE *g_log_file = NULL;
static DWORD g_last_frame_time = 0;
static DWORD g_last_paint_time = 0;
static int g_buffer_fill_level = 0;  // How many frames are buffered

#define MAX_FRAME_SIZE (1024*1024)
#define WINDOW_WIDTH 800
#define WINDOW_HEIGHT 600
#define DISPLAY_TIMER_ID 1
#define DISPLAY_INTERVAL 70  // ms between display updates (~14fps, balances smoothness and latency)

// Camera reading thread - writes to circular buffer
DWORD WINAPI CameraReadThread(LPVOID param) {
    unsigned char *temp_buffer = (unsigned char*)malloc(MAX_FRAME_SIZE);
    if (!temp_buffer) return 1;
    
    while (g_running) {
        size_t bytes_read = 0;
        int ret = camera_read_frame(g_camera, temp_buffer, MAX_FRAME_SIZE, 
                                    &bytes_read, 1000);
        
        if (ret == CAMERA_SUCCESS && bytes_read > 0) {
            DWORD capture_time = GetTickCount();
            DWORD interval = g_last_frame_time ? (capture_time - g_last_frame_time) : 0;
            
            // Add frame to circular buffer
            EnterCriticalSection(&g_frame_lock);
            if (bytes_read <= MAX_FRAME_SIZE) {
                // Write to current write position
                memcpy(g_frame_ring[g_write_pos].data, temp_buffer, bytes_read);
                g_frame_ring[g_write_pos].size = bytes_read;
                g_frame_ring[g_write_pos].filled = true;
                
                // Advance write position
                g_write_pos = (g_write_pos + 1) % SMOOTHING_BUFFER_SIZE;
                
                // Track buffer fill level
                if (g_buffer_fill_level < SMOOTHING_BUFFER_SIZE) {
                    g_buffer_fill_level++;
                }
                
                g_total_frames++;
                
                // Log frame capture timing
                if (g_log_file && g_last_frame_time > 0) {
                    fprintf(g_log_file, "CAPTURE,frame=%u,interval=%lu ms,size=%zu bytes,buffered=%d\n", 
                            g_total_frames, interval, bytes_read, g_buffer_fill_level);
                    if (interval > 100) {
                        fprintf(g_log_file, "WARNING: Long capture interval! %lu ms (buffered frames will smooth this)\n", interval);
                        fflush(g_log_file);
                    }
                }
            }
            LeaveCriticalSection(&g_frame_lock);
            
            g_last_frame_time = capture_time;
        } else if (ret == CAMERA_ERROR_TIMEOUT) {
            // Timeout is OK, just continue
            continue;
        }
    }
    
    free(temp_buffer);
    return 0;
}

// Window procedure
LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_ERASEBKGND:
            // Prevent flicker by not erasing background
            return 1;
        
        case WM_TIMER:
            if (wparam == DISPLAY_TIMER_ID) {
                // Timer fires at regular intervals - trigger paint to display next buffered frame
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
            
        case WM_PAINT: {
            DWORD paint_start = GetTickCount();
            DWORD paint_wait = g_last_paint_time ? (paint_start - g_last_paint_time) : 0;
            
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            
            // Get window dimensions
            RECT rect;
            GetClientRect(hwnd, &rect);
            int width = rect.right;
            int height = rect.bottom;
            
            // Create backbuffer for double buffering
            HDC backDC = CreateCompatibleDC(hdc);
            HBITMAP backBuffer = CreateCompatibleBitmap(hdc, width, height);
            HBITMAP oldBitmap = (HBITMAP)SelectObject(backDC, backBuffer);
            
            // Create GDI+ graphics on backbuffer
            Graphics graphics(backDC);
            graphics.Clear(Color(0, 0, 0));
            
            DWORD copy_start = GetTickCount();
            
            // Try to get next frame from circular buffer
            size_t current_display_size = 0;
            bool got_new_frame = false;
            EnterCriticalSection(&g_frame_lock);
            if (g_buffer_fill_level > 0 && g_frame_ring[g_read_pos].filled) {
                // Frame available in buffer - copy to display buffer
                memcpy(g_display_buffer, g_frame_ring[g_read_pos].data, 
                       g_frame_ring[g_read_pos].size);
                current_display_size = g_frame_ring[g_read_pos].size;
                g_display_size = current_display_size;
                
                // Mark slot as consumed and advance read position
                g_frame_ring[g_read_pos].filled = false;
                g_read_pos = (g_read_pos + 1) % SMOOTHING_BUFFER_SIZE;
                g_buffer_fill_level--;
                g_displayed_frames++;
                got_new_frame = true;
            } else {
                // No new frame in buffer - reuse last displayed frame
                current_display_size = g_display_size;
            }
            LeaveCriticalSection(&g_frame_lock);
            
            DWORD copy_time = GetTickCount() - copy_start;
            DWORD decode_start = GetTickCount();
            
            // Draw from display buffer (not frame buffer)
            if (current_display_size > 0) {
                // Create GDI+ Image from JPEG bytes
                IStream *stream = SHCreateMemStream(g_display_buffer, current_display_size);
                if (stream) {
                    Image image(stream);
                    if (image.GetLastStatus() == Ok) {
                        DWORD render_start = GetTickCount();
                        DWORD decode_time = render_start - decode_start;
                        
                        // Use faster interpolation for smoother performance
                        graphics.SetInterpolationMode(InterpolationModeBilinear);
                        graphics.SetCompositingQuality(CompositingQualityHighSpeed);
                        graphics.SetSmoothingMode(SmoothingModeHighSpeed);
                        graphics.DrawImage(&image, 0, 0, width, height);
                        
                        DWORD render_time = GetTickCount() - render_start;
                        
                        // Log detailed paint timing
                        if (g_log_file && got_new_frame) {
                            DWORD total_paint = GetTickCount() - paint_start;
                            fprintf(g_log_file, "PAINT,frame=%u,wait=%lu ms,copy=%lu ms,decode=%lu ms,render=%lu ms,total=%lu ms\n",
                                    g_displayed_frames, paint_wait, copy_time, decode_time, render_time, total_paint);
                            
                            if (total_paint > 50) {  // Log slow paints
                                fprintf(g_log_file, "WARNING: Slow paint! %lu ms (decode=%lu, render=%lu)\n", 
                                        total_paint, decode_time, render_time);
                                fflush(g_log_file);
                            }
                        }
                    } else {
                        // Draw error message
                        Font font(L"Arial", 16);
                        SolidBrush brush(Color(255, 255, 0));
                        graphics.DrawString(L"Invalid JPEG frame", -1, &font, 
                                          PointF(10, 10), &brush);
                    }
                    stream->Release();
                }
            } else {
                // Draw "waiting" message
                Font font(L"Arial", 20);
                SolidBrush brush(Color(255, 255, 255));
                graphics.DrawString(L"Waiting for camera...", -1, &font, 
                                  PointF(200, 250), &brush);
            }
            
            // Draw FPS and info
            if (g_total_frames > 0) {
                DWORD elapsed = GetTickCount() - g_start_time;
                float capture_fps = (float)g_total_frames / (elapsed / 1000.0f);
                float display_fps = (float)g_displayed_frames / (elapsed / 1000.0f);
                
                wchar_t info[256];
                swprintf(info, 256, L"Display: %.1f fps | Capture: %.1f fps | Buffer: %d | 'S' snapshot | ESC exit", 
                        display_fps, capture_fps, g_buffer_fill_level);
                
                Font font(L"Arial", 12);
                SolidBrush brush(Color(0, 255, 0));
                SolidBrush bg(Color(192, 0, 0, 0));
                
                RectF textRect(5, 5, (float)width - 10, 30);
                graphics.FillRectangle(&bg, textRect);
                graphics.DrawString(info, -1, &font, PointF(10, 10), &brush);
            }
            
            // Blit backbuffer to screen (eliminates flicker)
            BitBlt(hdc, 0, 0, width, height, backDC, 0, 0, SRCCOPY);
            
            // Cleanup backbuffer
            SelectObject(backDC, oldBitmap);
            DeleteObject(backBuffer);
            DeleteDC(backDC);
            
            EndPaint(hwnd, &ps);
            
            g_last_paint_time = GetTickCount();
            return 0;
        }
        
        case WM_KEYDOWN: {
            if (wparam == VK_ESCAPE) {
                g_running = false;
                PostQuitMessage(0);
            } else if (wparam == 'S' || wparam == 's') {
                // Save snapshot - use most recent captured frame
                EnterCriticalSection(&g_frame_lock);
                if (g_total_frames > 0) {
                    // Get most recently written frame (write_pos - 1)
                    int recent_pos = (g_write_pos - 1 + SMOOTHING_BUFFER_SIZE) % SMOOTHING_BUFFER_SIZE;
                    if (g_frame_ring[recent_pos].size > 0) {
                        char filename[128];
                        sprintf(filename, "snapshot_%03d.jpg", g_snapshot_count++);
                        FILE *fp = fopen(filename, "wb");
                        if (fp) {
                            fwrite(g_frame_ring[recent_pos].data, 1, 
                                   g_frame_ring[recent_pos].size, fp);
                            fclose(fp);
                            printf("Saved: %s\n", filename);
                        }
                    }
                }
                LeaveCriticalSection(&g_frame_lock);
            }
            return 0;
        }
        
        case WM_DESTROY:
            g_running = false;
            PostQuitMessage(0);
            return 0;
            
        default:
            return DefWindowProc(hwnd, msg, wparam, lparam);
    }
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, 
                   LPSTR lpCmdLine, int nCmdShow) {
    // Check for --debug or -d flag in command line
    bool debug_mode = false;
    if (lpCmdLine && (strstr(lpCmdLine, "--debug") || strstr(lpCmdLine, "-d"))) {
        debug_mode = true;
        camera_set_debug_logging(true);
        printf("Debug logging enabled - output will be written to useeplus_debug.log\n");
    }
    
    // Initialize GDI+
    GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);
    
    // Initialize circular frame buffer for smoothing
    for (int i = 0; i < SMOOTHING_BUFFER_SIZE; i++) {
        g_frame_ring[i].data = (unsigned char*)malloc(MAX_FRAME_SIZE);
        g_frame_ring[i].size = 0;
        g_frame_ring[i].filled = false;
        if (!g_frame_ring[i].data) {
            MessageBoxA(NULL, "Failed to allocate frame ring buffer", "Error", MB_OK);
            // Cleanup already allocated slots
            for (int j = 0; j < i; j++) {
                free(g_frame_ring[j].data);
            }
            return 1;
        }
    }
    
    // Allocate display buffer
    g_display_buffer = (unsigned char*)malloc(MAX_FRAME_SIZE);
    if (!g_display_buffer) {
        MessageBoxA(NULL, "Failed to allocate display buffer", "Error", MB_OK);
        for (int i = 0; i < SMOOTHING_BUFFER_SIZE; i++) {
            free(g_frame_ring[i].data);
        }
        return 1;
    }
    
    InitializeCriticalSection(&g_frame_lock);
    
    // Open timing log file
    g_log_file = fopen("frame_timing.log", "w");
    if (g_log_file) {
        fprintf(g_log_file, "=== Live Viewer Frame Timing Log ===\n");
        fprintf(g_log_file, "Format: TYPE,frame=N,metric1=X,metric2=Y,...\n");
        fprintf(g_log_file, "CAPTURE: Frame capture events\n");
        fprintf(g_log_file, "PAINT: Frame paint/display events\n");
        fprintf(g_log_file, "===================================\n\n");
        fflush(g_log_file);
        printf("Frame timing log: frame_timing.log\n");
    }
    
    // Open camera
    printf("Opening camera...\n");
    g_camera = camera_open();
    if (!g_camera) {
        char msg[512];
        sprintf(msg, "Failed to open camera:\n%s\n\nMake sure:\n"
                     "1. Camera is plugged in\n"
                     "2. WinUSB driver installed via Zadig",
                camera_get_error());
        MessageBoxA(NULL, msg, "Camera Error", MB_OK);
        for (int i = 0; i < SMOOTHING_BUFFER_SIZE; i++) {
            free(g_frame_ring[i].data);
        }
        free(g_display_buffer);
        return 1;
    }
    printf("Camera opened!\n");
    
    // Start streaming
    printf("Starting streaming...\n");
    int ret = camera_start_streaming(g_camera);
    if (ret != CAMERA_SUCCESS) {
        char msg[512];
        sprintf(msg, "Failed to start streaming:\n%s", camera_get_error());
        MessageBoxA(NULL, msg, "Camera Error", MB_OK);
        camera_close(g_camera);
        for (int i = 0; i < SMOOTHING_BUFFER_SIZE; i++) {
            free(g_frame_ring[i].data);
        }
        free(g_display_buffer);
        return 1;
    }
    printf("Streaming started!\n");
    
    // Start camera reading thread
    g_start_time = GetTickCount();
    HANDLE thread = CreateThread(NULL, 0, CameraReadThread, NULL, 0, NULL);
    
    // Register window class
    WNDCLASSA wc = {0};
    wc.style = CS_OWNDC;  // Reduce flicker with own DC
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "CameraViewerClass";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassA(&wc);
    
    // Create window
    HWND hwnd = CreateWindowA("CameraViewerClass", 
                              "Useeplus Camera Live Viewer",
                              WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT,
                              WINDOW_WIDTH, WINDOW_HEIGHT,
                              NULL, NULL, hInstance, NULL);
    
    if (!hwnd) {
        MessageBoxA(NULL, "Failed to create window", "Error", MB_OK);
        g_running = false;
        WaitForSingleObject(thread, INFINITE);
        camera_stop_streaming(g_camera);
        camera_close(g_camera);
        for (int i = 0; i < SMOOTHING_BUFFER_SIZE; i++) {
            free(g_frame_ring[i].data);
        }
        free(g_display_buffer);
        return 1;
    }
    
    // Store window handle globally for camera thread
    g_hwnd = hwnd;
    
    // Start display timer for smooth frame rate
    SetTimer(hwnd, DISPLAY_TIMER_ID, DISPLAY_INTERVAL, NULL);
    
    ShowWindow(hwnd, nCmdShow);
    // Don't force synchronous initial paint - let message loop handle it
    // UpdateWindow(hwnd);
    
    printf("\n");
    printf("Live Viewer Controls:\n");
    printf("  S : Save snapshot\n");
    printf("  ESC : Exit\n");
    printf("\n");
    printf("Performance logging enabled - see frame_timing.log\n");
    printf("Frame smoothing active - consistent %d fps display with ~%.1f sec buffer\n", 
           1000/DISPLAY_INTERVAL, (float)SMOOTHING_BUFFER_SIZE * DISPLAY_INTERVAL / 1000.0f);
    printf("\n");
    
    // Message loop - timer drives regular display updates
    MSG msg;
    while (g_running && GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    
    // Cleanup
    g_running = false;
    KillTimer(hwnd, DISPLAY_TIMER_ID);
    
    printf("\nStopping camera thread...\n");
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    
    printf("Stopping streaming...\n");
    camera_stop_streaming(g_camera);
    
    printf("Closing camera...\n");
    camera_close(g_camera);
    
    // Free all circular buffer slots
    for (int i = 0; i < SMOOTHING_BUFFER_SIZE; i++) {
        free(g_frame_ring[i].data);
    }
    free(g_display_buffer);
    DeleteCriticalSection(&g_frame_lock);
    
    // Close timing log
    if (g_log_file) {
        fprintf(g_log_file, "\n=== Session Complete ===\n");
        fprintf(g_log_file, "Total frames captured: %u\n", g_total_frames);
        fprintf(g_log_file, "Total frames displayed: %u\n", g_displayed_frames);
        fclose(g_log_file);
        printf("Frame timing log saved: frame_timing.log\n");
    }
    
    GdiplusShutdown(gdiplusToken);
    
    printf("Total frames captured: %u\n", g_total_frames);
    printf("Total frames displayed: %u\n", g_displayed_frames);
    printf("Snapshots saved: %d\n", g_snapshot_count);
    printf("Camera closed successfully.\n");
    
    return 0;
}

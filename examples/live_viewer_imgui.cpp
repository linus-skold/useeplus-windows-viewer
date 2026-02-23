/**
 * Camera Live Viewer with ImGui Controls
 * 
 * Advanced viewer with DirectX11 rendering and real-time adjustable parameters.
 * Provides interactive control over display rate and buffer size to tune the
 * latency/smoothness tradeoff for your specific use case.
 * 
 * Features:
 * - DirectX11 + ImGui rendering pipeline
 * - WIC (Windows Imaging Component) JPEG decoding
 * - Adjustable display FPS (5-30 fps) via slider
 * - Adjustable buffer size (2-32 frames) via slider
 * - Real-time statistics display
 * - Toggle logging on/off
 * - Frame smoothing to hide camera's periodic 600ms stutters
 * 
 * Controls:
 *   H   - Toggle controls UI on/off
 *   S   - Save snapshot
 *   ESC - Exit
 *   UI  - Adjust parameters with mouse/sliders
 * 
 * See CHANGELOG.md for full details on frame smoothing implementation.
 */

#include "useeplus_camera.h"
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wincodec.h>
#include <stdio.h>
#include <stdlib.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma warning(disable: 4996)

// Forward declare message handler
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Frame smoothing buffer - circular buffer for consistent display rate
#define MAX_SMOOTHING_BUFFER_SIZE 32
#define DEFAULT_BUFFER_SIZE 12
struct FrameSlot {
    unsigned char *data;
    size_t size;
    bool filled;
};

// DirectX11 resources
static ID3D11Device* g_pd3dDevice = NULL;
static ID3D11DeviceContext* g_pd3dDeviceContext = NULL;
static IDXGISwapChain* g_pSwapChain = NULL;
static ID3D11RenderTargetView* g_mainRenderTargetView = NULL;
static ID3D11Texture2D* g_pTextureCamera = NULL;
static ID3D11ShaderResourceView* g_pTextureSRV = NULL;

// Global variables
static CAMERA_HANDLE g_camera = NULL;
static bool g_running = true;
static FrameSlot g_frame_ring[MAX_SMOOTHING_BUFFER_SIZE];  // Circular buffer
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

// Adjustable parameters
static int g_smoothing_buffer_size = DEFAULT_BUFFER_SIZE;
static int g_display_interval = 80;  // ms between display updates
static bool g_show_controls = true;
static bool g_enable_logging = true;
static float g_zoom = 1.0f;

#define MAX_FRAME_SIZE (1024*1024)
#define WINDOW_WIDTH 1024
#define WINDOW_HEIGHT 768
#define DISPLAY_TIMER_ID 1

// Helper function to decode JPEG to RGBA
static unsigned char* DecodeJPEG(const unsigned char* jpeg_data, size_t jpeg_size, int* out_width, int* out_height) {
    // Use Windows Imaging Component (WIC) to decode JPEG
    IWICImagingFactory* pFactory = NULL;
    IWICBitmapDecoder* pDecoder = NULL;
    IWICBitmapFrameDecode* pFrame = NULL;
    IWICFormatConverter* pConverter = NULL;
    IWICStream* pStream = NULL;
    unsigned char* rgba_data = NULL;
    
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                  IID_IWICImagingFactory, (LPVOID*)&pFactory);
    if (FAILED(hr)) return NULL;
    
    hr = pFactory->CreateStream(&pStream);
    if (FAILED(hr)) goto cleanup;
    
    hr = pStream->InitializeFromMemory((BYTE*)jpeg_data, jpeg_size);
    if (FAILED(hr)) goto cleanup;
    
    hr = pFactory->CreateDecoderFromStream(pStream, NULL, WICDecodeMetadataCacheOnDemand, &pDecoder);
    if (FAILED(hr)) goto cleanup;
    
    hr = pDecoder->GetFrame(0, &pFrame);
    if (FAILED(hr)) goto cleanup;
    
    UINT width, height;
    hr = pFrame->GetSize(&width, &height);
    if (FAILED(hr)) goto cleanup;
    
    hr = pFactory->CreateFormatConverter(&pConverter);
    if (FAILED(hr)) goto cleanup;
    
    hr = pConverter->Initialize(pFrame, GUID_WICPixelFormat32bppRGBA,
                                WICBitmapDitherTypeNone, NULL, 0.0,
                                WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) goto cleanup;
    
    rgba_data = (unsigned char*)malloc(width * height * 4);
    if (!rgba_data) goto cleanup;
    
    hr = pConverter->CopyPixels(NULL, width * 4, width * height * 4, rgba_data);
    if (FAILED(hr)) {
        free(rgba_data);
        rgba_data = NULL;
        goto cleanup;
    }
    
    *out_width = width;
    *out_height = height;
    
cleanup:
    if (pConverter) pConverter->Release();
    if (pFrame) pFrame->Release();
    if (pDecoder) pDecoder->Release();
    if (pStream) pStream->Release();
    if (pFactory) pFactory->Release();
    
    return rgba_data;
}

// Create DirectX11 device and swap chain
static bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    
    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, createDeviceFlags,
                                                featureLevelArray, 2, D3D11_SDK_VERSION, &sd,
                                                &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (FAILED(hr)) return false;

    // Create render target
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, NULL, &g_mainRenderTargetView);
    pBackBuffer->Release();
    
    return true;
}

static void CleanupDeviceD3D() {
    if (g_pTextureSRV) { g_pTextureSRV->Release(); g_pTextureSRV = NULL; }
    if (g_pTextureCamera) { g_pTextureCamera->Release(); g_pTextureCamera = NULL; }
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = NULL; }
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = NULL; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = NULL; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = NULL; }
}

// Update camera texture from JPEG data
static bool UpdateCameraTexture(const unsigned char* jpeg_data, size_t jpeg_size) {
    int width, height;
    unsigned char* rgba_data = DecodeJPEG(jpeg_data, jpeg_size, &width, &height);
    if (!rgba_data) return false;
    
    // Release old texture if size changed
    if (g_pTextureCamera) {
        D3D11_TEXTURE2D_DESC desc;
        g_pTextureCamera->GetDesc(&desc);
        if (desc.Width != width || desc.Height != height) {
            g_pTextureCamera->Release();
            g_pTextureCamera = NULL;
            if (g_pTextureSRV) {
                g_pTextureSRV->Release();
                g_pTextureSRV = NULL;
            }
        }
    }
    
    // Create texture if needed
    if (!g_pTextureCamera) {
        D3D11_TEXTURE2D_DESC desc;
        ZeroMemory(&desc, sizeof(desc));
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0;
        
        D3D11_SUBRESOURCE_DATA initData;
        initData.pSysMem = rgba_data;
        initData.SysMemPitch = width * 4;
        initData.SysMemSlicePitch = 0;
        
        HRESULT hr = g_pd3dDevice->CreateTexture2D(&desc, &initData, &g_pTextureCamera);
        if (FAILED(hr)) {
            free(rgba_data);
            return false;
        }
        
        // Create shader resource view
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
        ZeroMemory(&srvDesc, sizeof(srvDesc));
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = desc.MipLevels;
        srvDesc.Texture2D.MostDetailedMip = 0;
        hr = g_pd3dDevice->CreateShaderResourceView(g_pTextureCamera, &srvDesc, &g_pTextureSRV);
        if (FAILED(hr)) {
            free(rgba_data);
            return false;
        }
    } else {
        // Update existing texture
        g_pd3dDeviceContext->UpdateSubresource(g_pTextureCamera, 0, NULL, rgba_data, width * 4, 0);
    }
    
    free(rgba_data);
    return true;
}

// Camera reading thread
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
            if (bytes_read <= MAX_FRAME_SIZE && g_buffer_fill_level < g_smoothing_buffer_size) {
                // Write to current write position
                memcpy(g_frame_ring[g_write_pos].data, temp_buffer, bytes_read);
                g_frame_ring[g_write_pos].size = bytes_read;
                g_frame_ring[g_write_pos].filled = true;
                
                // Advance write position
                g_write_pos = (g_write_pos + 1) % g_smoothing_buffer_size;
                
                // Track buffer fill level
                if (g_buffer_fill_level < g_smoothing_buffer_size) {
                    g_buffer_fill_level++;
                }
                
                g_total_frames++;
                
                // Log frame capture timing
                if (g_log_file && g_enable_logging && g_last_frame_time > 0) {
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
            continue;
        }
    }
    
    free(temp_buffer);
    return 0;
}

// Render ImGui controls
static void RenderControls() {
    if (!g_show_controls) return;
    
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(400, 350), ImGuiCond_FirstUseEver);
    
    if (ImGui::Begin("Camera Controls", &g_show_controls)) {
        ImGui::Text("Camera Feed Parameters");
        ImGui::Separator();
        
        // Display rate control
        float display_fps = 1000.0f / g_display_interval;
        if (ImGui::SliderFloat("Display FPS", &display_fps, 5.0f, 30.0f, "%.1f fps")) {
            g_display_interval = (int)(1000.0f / display_fps);
            KillTimer(g_hwnd, DISPLAY_TIMER_ID);
            SetTimer(g_hwnd, DISPLAY_TIMER_ID, g_display_interval, NULL);
        }
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Lower = less latency, higher = smoother");
        
        // Buffer size control
        if (ImGui::SliderInt("Buffer Size", &g_smoothing_buffer_size, 2, MAX_SMOOTHING_BUFFER_SIZE, "%d frames")) {
            // Adjust read/write positions if buffer shrunk
            EnterCriticalSection(&g_frame_lock);
            if (g_write_pos >= g_smoothing_buffer_size) g_write_pos = 0;
            if (g_read_pos >= g_smoothing_buffer_size) g_read_pos = 0;
            if (g_buffer_fill_level > g_smoothing_buffer_size) g_buffer_fill_level = g_smoothing_buffer_size;
            LeaveCriticalSection(&g_frame_lock);
        }
        float latency_sec = (float)(g_smoothing_buffer_size * g_display_interval) / 1000.0f;
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Max latency: %.2f seconds", latency_sec);
        
        ImGui::Separator();
        
        // Statistics
        ImGui::Text("Statistics");
        DWORD elapsed = GetTickCount() - g_start_time;
        float capture_fps = elapsed > 0 ? (float)g_total_frames / (elapsed / 1000.0f) : 0.0f;
        float actual_display_fps = elapsed > 0 ? (float)g_displayed_frames / (elapsed / 1000.0f) : 0.0f;
        
        ImGui::Text("Capture Rate: %.1f fps", capture_fps);
        ImGui::Text("Display Rate: %.1f fps", actual_display_fps);
        ImGui::Text("Buffer Level: %d / %d frames", g_buffer_fill_level, g_smoothing_buffer_size);
        ImGui::Text("Total Captured: %u", g_total_frames);
        ImGui::Text("Total Displayed: %u", g_displayed_frames);
        
        ImGui::Separator();
        
        // Options
        ImGui::Checkbox("Enable Logging", &g_enable_logging);
        
        ImGui::Separator();
        
        // Actions
        if (ImGui::Button("Save Snapshot (S)", ImVec2(180, 30))) {
            PostMessage(g_hwnd, WM_KEYDOWN, 'S', 0);
        }
        ImGui::SameLine();
        if (ImGui::Button("Exit (ESC)", ImVec2(180, 30))) {
            PostMessage(g_hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
        }
        
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Tips:");
        ImGui::BulletText("Lower FPS = less lag, but may show stutters");
        ImGui::BulletText("Larger buffer = smoother during stutters");
        ImGui::BulletText("Camera captures at ~16fps with periodic stutters");
    }
    ImGui::End();
}

// Window procedure
LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam))
        return true;
    
    switch (msg) {
        case WM_SIZE:
            if (g_pd3dDevice != NULL && wparam != SIZE_MINIMIZED) {
                if (g_mainRenderTargetView) {
                    g_mainRenderTargetView->Release();
                    g_mainRenderTargetView = NULL;
                }
                g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lparam), (UINT)HIWORD(lparam), DXGI_FORMAT_UNKNOWN, 0);
                ID3D11Texture2D* pBackBuffer;
                g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
                g_pd3dDevice->CreateRenderTargetView(pBackBuffer, NULL, &g_mainRenderTargetView);
                pBackBuffer->Release();
            }
            return 0;
        
        case WM_TIMER:
            if (wparam == DISPLAY_TIMER_ID) {
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
            
        case WM_PAINT: {
            // Start ImGui frame
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            
            // Try to get next frame from circular buffer
            size_t current_display_size = 0;
            bool got_new_frame = false;
            EnterCriticalSection(&g_frame_lock);
            if (g_buffer_fill_level > 0 && g_read_pos < g_smoothing_buffer_size && g_frame_ring[g_read_pos].filled) {
                memcpy(g_display_buffer, g_frame_ring[g_read_pos].data, 
                       g_frame_ring[g_read_pos].size);
                current_display_size = g_frame_ring[g_read_pos].size;
                g_display_size = current_display_size;
                
                g_frame_ring[g_read_pos].filled = false;
                g_read_pos = (g_read_pos + 1) % g_smoothing_buffer_size;
                g_buffer_fill_level--;
                g_displayed_frames++;
                got_new_frame = true;
            } else {
                current_display_size = g_display_size;
            }
            LeaveCriticalSection(&g_frame_lock);
            
            // Update camera texture if we have a frame
            if (current_display_size > 0) {
                UpdateCameraTexture(g_display_buffer, current_display_size);
            }
            
            // Render camera feed
            const float clear_color[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, NULL);
            g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
            
            // Get window size
            RECT rect;
            GetClientRect(hwnd, &rect);
            float window_width = (float)(rect.right - rect.left);
            float window_height = (float)(rect.bottom - rect.top);
            
            // Draw camera texture with ImGui
            if (g_pTextureSRV) {
                D3D11_TEXTURE2D_DESC desc;
                g_pTextureCamera->GetDesc(&desc);
                float tex_width = (float)desc.Width;
                float tex_height = (float)desc.Height;
                
                // Calculate aspect-fit scaling
                float scale = min(window_width / tex_width, window_height / tex_height);
                float display_width = tex_width * scale;
                float display_height = tex_height * scale;
                float pos_x = (window_width - display_width) / 2.0f;
                float pos_y = (window_height - display_height) / 2.0f;
                
                // Draw as ImGui image in background window
                ImGui::SetNextWindowPos(ImVec2(0, 0));
                ImGui::SetNextWindowSize(ImVec2(window_width, window_height));
                ImGui::Begin("Camera Feed", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | 
                            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | 
                            ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus |
                            ImGuiWindowFlags_NoBackground);
                ImGui::SetCursorPos(ImVec2(pos_x, pos_y));
                ImGui::Image((ImTextureID)g_pTextureSRV, ImVec2(display_width, display_height));
                ImGui::End();
            }
            
            // Render controls
            RenderControls();
            
            // Render ImGui
            ImGui::Render();
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            
            g_pSwapChain->Present(1, 0);
            
            ValidateRect(hwnd, NULL);
            return 0;
        }
        
        case WM_KEYDOWN: {
            if (wparam == VK_ESCAPE) {
                g_running = false;
                PostQuitMessage(0);
            } else if (wparam == 'S' || wparam == 's') {
                // Save snapshot
                EnterCriticalSection(&g_frame_lock);
                if (g_total_frames > 0) {
                    int recent_pos = (g_write_pos - 1 + g_smoothing_buffer_size) % g_smoothing_buffer_size;
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
            } else if (wparam == 'H' || wparam == 'h') {
                g_show_controls = !g_show_controls;
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
    
    // Initialize COM for WIC
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    
    // Initialize circular frame buffer
    for (int i = 0; i < MAX_SMOOTHING_BUFFER_SIZE; i++) {
        g_frame_ring[i].data = (unsigned char*)malloc(MAX_FRAME_SIZE);
        g_frame_ring[i].size = 0;
        g_frame_ring[i].filled = false;
        if (!g_frame_ring[i].data) {
            MessageBoxA(NULL, "Failed to allocate frame ring buffer", "Error", MB_OK);
            for (int j = 0; j < i; j++) {
                free(g_frame_ring[j].data);
            }
            return 1;
        }
    }
    
    g_display_buffer = (unsigned char*)malloc(MAX_FRAME_SIZE);
    if (!g_display_buffer) {
        MessageBoxA(NULL, "Failed to allocate display buffer", "Error", MB_OK);
        for (int i = 0; i < MAX_SMOOTHING_BUFFER_SIZE; i++) {
            free(g_frame_ring[i].data);
        }
        return 1;
    }
    
    InitializeCriticalSection(&g_frame_lock);
    
    // Open timing log file
    g_log_file = fopen("frame_timing.log", "w");
    if (g_log_file) {
        fprintf(g_log_file, "=== Live Viewer Frame Timing Log (ImGui Version) ===\n");
        fprintf(g_log_file, "Format: TYPE,frame=N,metric1=X,metric2=Y,...\n");
        fprintf(g_log_file, "CAPTURE: Frame capture events\n");
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
        for (int i = 0; i < MAX_SMOOTHING_BUFFER_SIZE; i++) {
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
        for (int i = 0; i < MAX_SMOOTHING_BUFFER_SIZE; i++) {
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
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "CameraViewerClass";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassA(&wc);
    
    // Create window
    HWND hwnd = CreateWindowA("CameraViewerClass", 
                              "Useeplus Camera Live Viewer - ImGui Controls (H to toggle)",
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
        for (int i = 0; i < MAX_SMOOTHING_BUFFER_SIZE; i++) {
            free(g_frame_ring[i].data);
        }
        free(g_display_buffer);
        return 1;
    }
    
    g_hwnd = hwnd;
    
    // Initialize DirectX11
    if (!CreateDeviceD3D(hwnd)) {
        MessageBoxA(NULL, "Failed to create DirectX11 device", "Error", MB_OK);
        CleanupDeviceD3D();
        DestroyWindow(hwnd);
        return 1;
    }
    
    // Setup ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
    
    // Start display timer
    SetTimer(hwnd, DISPLAY_TIMER_ID, g_display_interval, NULL);
    
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    
    printf("\n");
    printf("Live Viewer Controls:\n");
    printf("  S : Save snapshot\n");
    printf("  H : Toggle controls UI\n");
    printf("  ESC : Exit\n");
    printf("\n");
    printf("Adjustable parameters available in UI:\n");
    printf("  - Display FPS (5-30 fps)\n");
    printf("  - Buffer size (2-32 frames)\n");
    printf("  - Enable/disable logging\n");
    printf("\n");
    
    // Message loop
    MSG msg;
    ZeroMemory(&msg, sizeof(msg));
    while (msg.message != WM_QUIT && g_running) {
        if (PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            continue;
        }
        Sleep(1);
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
    
    // Cleanup ImGui
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    
    // Cleanup DirectX
    CleanupDeviceD3D();
    
    // Free buffers
    for (int i = 0; i < MAX_SMOOTHING_BUFFER_SIZE; i++) {
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
    
    CoUninitialize();
    return 0;
}

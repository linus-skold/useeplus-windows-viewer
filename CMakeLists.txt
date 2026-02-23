cmake_minimum_required(VERSION 3.10)
project(useeplus_camera C CXX)

# Set C/C++ standards
set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 11)

# Add include directory
include_directories(${CMAKE_SOURCE_DIR}/include)

# ============================================================================
# External Dependencies
# ============================================================================

# Fetch ImGui for advanced viewer
include(FetchContent)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.5
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)

# Create ImGui library with Win32 + DirectX11 backend
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)

target_include_directories(imgui PUBLIC 
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

target_link_libraries(imgui PUBLIC d3d11 d3dcompiler)

# ============================================================================
# Main Library - useeplus_camera.dll
# ============================================================================

add_library(useeplus_camera SHARED
    src/useeplus_camera.c
    include/useeplus_camera.h
)

target_compile_definitions(useeplus_camera PRIVATE USEEPLUS_CAMERA_EXPORTS)

target_include_directories(useeplus_camera PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(useeplus_camera
    winusb
    setupapi
    user32
)

set_target_properties(useeplus_camera PROPERTIES
    OUTPUT_NAME "useeplus_camera"
    PREFIX ""
)

# ============================================================================
# Example Applications
# ============================================================================

# Simple capture example
add_executable(camera_capture
    examples/camera_capture.c
)

target_link_libraries(camera_capture useeplus_camera)

# Live viewer (GDI+ based)
add_executable(live_viewer WIN32
    examples/live_viewer.cpp
)

set_source_files_properties(examples/live_viewer.cpp PROPERTIES LANGUAGE CXX)

target_link_libraries(live_viewer 
    useeplus_camera
    gdiplus
    shlwapi
)

set_target_properties(live_viewer PROPERTIES
    LINK_FLAGS "/SUBSYSTEM:WINDOWS"
)

# Live viewer with ImGui (advanced controls)
add_executable(live_viewer_imgui WIN32
    examples/live_viewer_imgui.cpp
)

set_source_files_properties(examples/live_viewer_imgui.cpp PROPERTIES LANGUAGE CXX)

target_link_libraries(live_viewer_imgui 
    useeplus_camera
    imgui
    d3d11
    d3dcompiler
    windowscodecs
)

set_target_properties(live_viewer_imgui PROPERTIES
    LINK_FLAGS "/SUBSYSTEM:WINDOWS"
)

# ============================================================================
# Diagnostic and Testing Tools
# ============================================================================

# Diagnostic tool - enumerate USB devices
add_executable(diagnostic
    tools/diagnostic.c
)

target_link_libraries(diagnostic setupapi)

# Simple WinUSB test
add_executable(simple_winusb_test
    tools/simple_winusb_test.c
)

target_link_libraries(simple_winusb_test winusb)

# ============================================================================
# Installation
# ============================================================================

install(TARGETS useeplus_camera camera_capture live_viewer live_viewer_imgui
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(FILES include/useeplus_camera.h DESTINATION include)

install(DIRECTORY docs/ DESTINATION docs)

# ============================================================================
# Build Information
# ============================================================================

message(STATUS "=== Useeplus Camera Driver for Windows ===")
message(STATUS "Library:")
message(STATUS "  - useeplus_camera.dll")
message(STATUS "Examples:")
message(STATUS "  - camera_capture.exe (simple capture)")
message(STATUS "  - live_viewer.exe (GDI+ based)")
message(STATUS "  - live_viewer_imgui.exe (with adjustable controls)")
message(STATUS "Tools:")
message(STATUS "  - diagnostic.exe (USB enumeration)")
message(STATUS "  - simple_winusb_test.exe (WinUSB testing)")
message(STATUS "==========================================")


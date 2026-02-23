# Useeplus Camera - Windows Build Script
# This script builds the DLL and test application using Visual Studio

$ErrorActionPreference = "Stop"

Write-Host "Useeplus Camera - Windows Build" -ForegroundColor Cyan
Write-Host "================================" -ForegroundColor Cyan
Write-Host ""

# Check if CMake is available
$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmake) {
    Write-Host "ERROR: CMake not found!" -ForegroundColor Red
    Write-Host "Please install CMake from https://cmake.org/download/" -ForegroundColor Yellow
    exit 1
}

Write-Host "Found CMake: $($cmake.Source)" -ForegroundColor Green

# Create build directory
$buildDir = "build"
if (Test-Path $buildDir) {
    Write-Host "Cleaning existing build directory..." -ForegroundColor Yellow
    Remove-Item -Recurse -Force $buildDir
}

New-Item -ItemType Directory -Path $buildDir | Out-Null
Write-Host "Created build directory" -ForegroundColor Green

# Configure with CMake
Write-Host ""
Write-Host "Configuring project..." -ForegroundColor Cyan
Set-Location $buildDir

$cmakeArgs = @(
    ".."
    "-G", "Visual Studio 17 2022"
)

try {
    & cmake @cmakeArgs
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configuration failed"
    }
} catch {
    Write-Host ""
    Write-Host "Note: If you don't have Visual Studio 2022, try one of:" -ForegroundColor Yellow
    Write-Host '  cmake .. -G "Visual Studio 16 2019"' -ForegroundColor Yellow
    Write-Host '  cmake .. -G "MinGW Makefiles"' -ForegroundColor Yellow
    Write-Host '  cmake .. -G "NMake Makefiles"' -ForegroundColor Yellow
    Set-Location ..
    exit 1
}

Write-Host "Configuration successful!" -ForegroundColor Green

# Build
Write-Host ""
Write-Host "Building..." -ForegroundColor Cyan
& cmake --build . --config Release

if ($LASTEXITCODE -ne 0) {
    Write-Host "Build failed!" -ForegroundColor Red
    Set-Location ..
    exit 1
}

Write-Host "Build successful!" -ForegroundColor Green

# Copy outputs to root directory for convenience
Write-Host ""
Write-Host "Copying outputs..." -ForegroundColor Cyan
Copy-Item "Release\useeplus_camera.dll" ".." -Force
Copy-Item "Release\camera_capture.exe" ".." -Force
Copy-Item "Release\diagnostic.exe" ".." -Force
Copy-Item "Release\live_viewer.exe" ".." -Force

Set-Location ..

Write-Host ""
Write-Host "Build complete!" -ForegroundColor Green
Write-Host ""
Write-Host "Output files:" -ForegroundColor Cyan
Write-Host "  - useeplus_camera.dll" -ForegroundColor White
Write-Host "  - camera_capture.exe" -ForegroundColor White
Write-Host "  - diagnostic.exe" -ForegroundColor White
Write-Host "  - live_viewer.exe" -ForegroundColor White
Write-Host ""
Write-Host "Next steps:" -ForegroundColor Yellow
Write-Host "  1. Install WinUSB driver using Zadig (see README_WINDOWS.md)"
Write-Host "  2. Run: .\camera_capture.exe (for single capture)"
Write-Host "  3. Run: .\live_viewer.exe (for live preview)"
Write-Host ""

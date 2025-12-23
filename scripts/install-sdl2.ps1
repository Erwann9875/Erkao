Write-Host "Installing SDL2 dependencies for Erkao..." -ForegroundColor Cyan

if ($IsWindows -or $env:OS -eq "Windows_NT") {
    Write-Host "Detected Windows" -ForegroundColor Green
    
    $vcpkg = Get-Command vcpkg -ErrorAction SilentlyContinue
    if ($vcpkg) {
        Write-Host "Using vcpkg to install SDL2..." -ForegroundColor Yellow
        vcpkg install sdl2:x64-windows sdl2-image:x64-windows sdl2-ttf:x64-windows sdl2-mixer:x64-windows
        Write-Host "Done! Use -DCMAKE_TOOLCHAIN_FILE=[vcpkg root]/scripts/buildsystems/vcpkg.cmake when building" -ForegroundColor Green
    } else {
        Write-Host "vcpkg not found. Installing via winget (MSYS2)..." -ForegroundColor Yellow

        $winget = Get-Command winget -ErrorAction SilentlyContinue
        if ($winget) {
            winget install -e --id MSYS2.MSYS2
            Write-Host @"

MSYS2 installed. Now run these commands in MSYS2 UCRT64 terminal:
  pacman -S mingw-w64-ucrt-x86_64-SDL2
  pacman -S mingw-w64-ucrt-x86_64-SDL2_image
  pacman -S mingw-w64-ucrt-x86_64-SDL2_ttf
  pacman -S mingw-w64-ucrt-x86_64-SDL2_mixer

Then add C:\msys64\ucrt64\bin to your PATH
"@ -ForegroundColor Cyan
        } else {
            Write-Host @"

Please install SDL2 manually:
1. Download from https://github.com/libsdl-org/SDL/releases
2. Download SDL2_image, SDL2_ttf, SDL2_mixer from https://github.com/libsdl-org
3. Extract to C:\SDL2 and set SDL2_DIR environment variable

Or install vcpkg: https://vcpkg.io/en/getting-started.html
"@ -ForegroundColor Yellow
        }
    }
} else {
    Write-Host "This script is for Windows. For other platforms, use install-sdl2.sh" -ForegroundColor Yellow
}

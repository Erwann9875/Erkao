#!/bin/bash

set -e

echo "Installing SDL2 dependencies for Erkao..."

if [[ "$OSTYPE" == "darwin"* ]]; then
    echo "Detected macOS"
    
    if command -v brew &> /dev/null; then
        echo "Using Homebrew to install SDL2..."
        brew install sdl2 sdl2_image sdl2_ttf sdl2_mixer
        echo "Done! SDL2 installed via Homebrew."
    else
        echo "Homebrew not found. Please install it first:"
        echo '  /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"'
        exit 1
    fi

elif [[ "$OSTYPE" == "linux-gnu"* ]]; then
    echo "Detected Linux"
    
    if command -v apt-get &> /dev/null; then
        echo "Using apt to install SDL2..."
        sudo apt-get update
        sudo apt-get install -y libsdl2-dev libsdl2-image-dev libsdl2-ttf-dev libsdl2-mixer-dev
        echo "Done! SDL2 installed via apt."
        
    elif command -v dnf &> /dev/null; then
        echo "Using dnf to install SDL2..."
        sudo dnf install -y SDL2-devel SDL2_image-devel SDL2_ttf-devel SDL2_mixer-devel
        echo "Done! SDL2 installed via dnf."
        
    elif command -v pacman &> /dev/null; then
        echo "Using pacman to install SDL2..."
        sudo pacman -S --noconfirm sdl2 sdl2_image sdl2_ttf sdl2_mixer
        echo "Done! SDL2 installed via pacman."
        
    else
        echo "Unknown package manager. Please install SDL2 manually:"
        echo "  - libsdl2-dev"
        echo "  - libsdl2-image-dev"
        echo "  - libsdl2-ttf-dev"
        echo "  - libsdl2-mixer-dev"
        exit 1
    fi
else
    echo "Unknown OS: $OSTYPE"
    exit 1
fi

echo ""
echo "SDL2 installation complete! You can now build Erkao with graphics support."

#!/bin/sh
# Starts re2ds in melonDS. Double-click it in the Finder or run ./start.command
# in a terminal. Rebuilds the ROM first if the code changed (make only builds
# what is needed). The game data in nitrofiles/ comes from your own disc via
# tools/build_assets.sh.
cd "$(dirname "$0")" || exit 1

MELONDS="$HOME/Applications/melonDS.app"
export DEVKITPRO=/opt/devkitpro
export DEVKITARM=/opt/devkitpro/devkitARM

if [ ! -d nitrofiles/room ]; then
    echo "nitrofiles/ is missing: run tools/build_assets.sh <extracted disc> first."
    exit 1
fi
if [ -d "$DEVKITARM" ]; then
    make || { echo "Build failed."; exit 1; }
fi
if [ ! -f re2ds.nds ]; then
    echo "re2ds.nds is missing and devkitPro is not installed."
    exit 1
fi

# quit a running instance so the new ROM is loaded
pkill -f "melonDS.app/Contents/MacOS/melonDS" 2>/dev/null && sleep 1
open -a "$MELONDS" --args "$PWD/re2ds.nds"
echo "melonDS started: $PWD/re2ds.nds"

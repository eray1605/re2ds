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

# quit a running instance so the new ROM is loaded (and wait until it is gone:
# otherwise "open" only brings the old one to the front)
osascript -e 'quit app "melonDS"' >/dev/null 2>&1 || pkill -f "melonDS.app/Contents/MacOS/melonDS"
for i in 1 2 3 4 5 6 7 8 9 10; do
    pgrep -f "melonDS.app/Contents/MacOS/melonDS" >/dev/null || break
    sleep 1
done
sleep 1 # macOS still lists the app for a moment ("open" fails with -600)

# Saving needs melonDS's virtual SD card (Config > Emu settings > DLDI, with
# "Sync SD to folder" set to this folder). With DLDI on, the game reads its
# data from re2ds.nds on that card, so copy the ROM there.
SD_DIR="${RE2DS_SD:-$HOME/Documents/re2ds-sd}"
# (melonDS writes its own padded copy back when it quits, so compare the
# header - it differs between builds - rather than dates or sizes)
if [ -d "$SD_DIR" ] && ! cmp -s -n 352 re2ds.nds "$SD_DIR/re2ds.nds"; then
    cp re2ds.nds "$SD_DIR/re2ds.nds" && echo "Copied re2ds.nds to $SD_DIR"
fi

open -a "$MELONDS" --args "$PWD/re2ds.nds"
echo "melonDS started: $PWD/re2ds.nds"

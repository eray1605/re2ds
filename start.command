#!/bin/sh
# Startet re2ds in melonDS. Im Finder per Doppelklick oder im Terminal mit
# ./start.command. Baut die ROM vorher neu, falls sich der Code geändert hat
# (make baut nur, was nötig ist). Die Spieldaten in nitrofiles/ erzeugt
# tools/build_assets.sh aus der eigenen Disc.
cd "$(dirname "$0")" || exit 1

MELONDS="$HOME/Applications/melonDS.app"
export DEVKITPRO=/opt/devkitpro
export DEVKITARM=/opt/devkitpro/devkitARM

if [ ! -d nitrofiles/room ]; then
    echo "nitrofiles/ fehlt: zuerst tools/build_assets.sh <entpackte Disc> ausführen."
    exit 1
fi
if [ -d "$DEVKITARM" ]; then
    make || { echo "Build fehlgeschlagen."; exit 1; }
fi
if [ ! -f re2ds.nds ]; then
    echo "re2ds.nds fehlt und devkitPro ist nicht installiert."
    exit 1
fi

# eine laufende Instanz beenden, damit die neue ROM geladen wird
pkill -f "melonDS.app/Contents/MacOS/melonDS" 2>/dev/null && sleep 1
open -a "$MELONDS" --args "$PWD/re2ds.nds"
echo "melonDS gestartet: $PWD/re2ds.nds"

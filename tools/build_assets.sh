#!/bin/sh
# Convert one room + Leon from the extracted PS1 disc into nitrofiles/ for the
# DS build. Usage: tools/build_assets.sh <extracted disc dir> [stage room spawn_camera]
#   e.g. tools/build_assets.sh ~/re2_disc1 1 00 2
set -e
DISC="$1"
STAGE="${2:-1}"
ROOM="${3:-00}"
SPAWN_CAM="${4:-2}"
cd "$(dirname "$0")/.."

R="room${STAGE}${ROOM}"        # BSS name, e.g. room100
RDT="ROOM${STAGE}${ROOM}0"     # RDT name, e.g. ROOM1000 (player 0 = Leon)
mkdir -p build/bg build/room build/leon build/mask build/ds/bg build/ds/room build/ds/model build/ds/mask

[ -x tools/bss2ppm ] || clang -O2 -o tools/bss2ppm tools/bss2ppm.c -I/opt/homebrew/include \
    -L/opt/homebrew/lib -lavcodec -lavutil -lswscale

./tools/bss2ppm "$DISC/COMMON/BSS/ROOM${STAGE}${ROOM}.BSS" "build/bg/$R" 2>/dev/null
python3 -I tools/rdt2json.py "$DISC/PL0/RDT/$RDT.RDT" "build/room/$RDT.json"
python3 -I tools/bssmask.py "$DISC/COMMON/BSS/ROOM${STAGE}${ROOM}.BSS" "build/mask/$R"
python3 -I tools/pld2json.py "$DISC/PL0/PLD/PL00.PLD" build/leon/leon --plw "$DISC/PL0/PLD/PL00W00.PLW"

python3 -I tools/bg2ds.py "build/bg/$R" build/ds/bg
python3 -I tools/room2ds.py "build/room/$RDT.json" "build/ds/room/$(echo $RDT | tr A-Z a-z).bin" "$SPAWN_CAM"
python3 -I tools/mask2ds.py "build/room/$RDT.json" "build/mask/$R" "build/ds/mask/$R"
python3 -I tools/model2ds.py build/leon/leon.json "$DISC/PL0/PLD/PL00.PLD" build/ds/model/leon

rm -rf nitrofiles
mkdir -p nitrofiles
cp -R build/ds/bg build/ds/room build/ds/model build/ds/mask nitrofiles/
du -sh nitrofiles

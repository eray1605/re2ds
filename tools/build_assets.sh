#!/bin/sh
# Convert Leon's rooms and model from the extracted PS1 disc into nitrofiles/
# for the DS build. Rooms are converted in parallel.
# Usage: tools/build_assets.sh <extracted disc dir> [ROOM1000 ROOM1010 ...]
#        (default: every room in PL0/RDT)
set -e
DISC="$1"
shift
cd "$(dirname "$0")/.."
ROOMS="$*"
[ -n "$ROOMS" ] || ROOMS=$(ls "$DISC/PL0/RDT" | sed -n 's/^\(ROOM....\)\.RDT$/\1/p')

rm -rf build/ds
mkdir -p build/bg build/room build/leon build/mask build/ds/bg build/ds/room build/ds/model build/ds/mask build/ds/sound

[ -x tools/bss2ppm ] || clang -O2 -o tools/bss2ppm tools/bss2ppm.c -I/opt/homebrew/include \
    -L/opt/homebrew/lib -lavcodec -lavutil -lswscale
[ -x tools/lz77 ] || clang -O2 -o tools/lz77 tools/lz77.c

python3 -I tools/pld2json.py "$DISC/PL0/PLD/PL00.PLD" build/leon/leon --plw "$DISC/PL0/PLD/PL00W00.PLW"
python3 -I tools/model2ds.py build/leon/leon.json "$DISC/PL0/PLD/PL00.PLD" build/ds/model/leon

# door sounds: sound/doorNN.bin from COMMON/DOOR/DOORNN.DO2
mkdir -p build/ds/sound
for f in "$DISC"/COMMON/DOOR/DOOR*.DO2; do
    n=$(basename "$f" .DO2 | tr A-Z a-z)
    python3 -I tools/vab2ds.py "$f" "build/ds/sound/$n.bin" > /dev/null
done

# base sounds (footsteps etc.)
python3 -I tools/vab2ds.py "$DISC/COMMON/SOUND/CORE00.EDH" build/ds/sound/core00.bin \
    --vb "$DISC/COMMON/SOUND/CORE00.VB" > /dev/null

JOBS=$(sysctl -n hw.ncpu 2>/dev/null || echo 4)
echo $ROOMS | tr ' ' '\n' | xargs -P "$JOBS" -I{} tools/convert_room.sh "$DISC" {}

# room/index.bin: u16 count, then (u8 stage, u8 room) per converted room
python3 -I - build/ds/room <<'PY'
import os, struct, sys
d = sys.argv[1]
rooms = sorted((int(n[4]), int(n[5:7], 16)) for n in os.listdir(d) if n.startswith("room") and n.endswith("0.bin"))
with open(os.path.join(d, "index.bin"), "wb") as f:
    f.write(struct.pack("<H", len(rooms)) + b"".join(struct.pack("BB", s, r) for s, r in rooms))
print(f"{len(rooms)} rooms in room/index.bin")
PY

rm -rf nitrofiles
mkdir -p nitrofiles
cp -R build/ds/bg build/ds/room build/ds/model build/ds/mask build/ds/sound nitrofiles/
du -sh nitrofiles nitrofiles/*

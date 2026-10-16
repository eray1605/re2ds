#!/bin/sh
# Convert one room (RDT + BSS) into build/ds/. Called by build_assets.sh.
# Usage: tools/convert_room.sh <extracted disc dir> ROOM10A0
set -e
DISC="$1"
RDT="$2"                                  # e.g. ROOM10A0 (stage 1, room 0x0A, Leon)
SR=$(echo "$RDT" | sed 's/^ROOM\(...\).$/\1/')   # 10A
R="room$(echo "$SR" | tr A-F a-f)"        # BSS / background name, e.g. room10a
BSS="$DISC/COMMON/BSS/ROOM$SR.BSS"
LOWER=$(echo "$RDT" | tr A-Z a-z)
# Leon's game starts in room 100; its spawn point should be in camera 2
SPAWN_CAM=0
[ "$RDT" = ROOM1000 ] && SPAWN_CAM=2

./tools/bss2ppm "$BSS" "build/bg/$R" 2>/dev/null
python3 -I tools/rdt2json.py "$DISC/PL0/RDT/$RDT.RDT" "build/room/$RDT.json" > /dev/null
python3 -I tools/bssmask.py "$BSS" "build/mask/$R" > /dev/null
python3 -I tools/bg2ds.py "build/bg/$R" build/ds/bg > /dev/null
python3 -I tools/room2ds.py "build/room/$RDT.json" "build/ds/room/$LOWER.bin" "$SPAWN_CAM" > /dev/null
python3 -I tools/mask2ds.py "build/room/$RDT.json" "build/mask/$R" "build/ds/mask/$R" > /dev/null
python3 -I tools/vab2ds.py "$DISC/PL0/RDT/$RDT.RDT" "build/ds/sound/$LOWER.bin" --rdt > /dev/null
python3 -I tools/rbj2ds.py "$DISC/PL0/RDT/$RDT.RDT" "build/ds/room/${LOWER}_rbj.bin" > /dev/null
python3 -I tools/blk2ds.py "$DISC/PL0/RDT/$RDT.RDT" "build/ds/room/${LOWER}_blk.bin" > /dev/null
python3 -I tools/obj2ds.py "$DISC/PL0/RDT/$RDT.RDT" "build/ds/room/${LOWER}_obj.bin" > /dev/null
echo "$RDT ok"

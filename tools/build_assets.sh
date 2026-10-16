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
mkdir -p build/bg build/room build/leon build/mask build/ds/bg build/ds/room build/ds/model build/ds/mask build/ds/sound build/ds/door

[ -x tools/bss2ppm ] || clang -O2 -o tools/bss2ppm tools/bss2ppm.c -I/opt/homebrew/include \
    -L/opt/homebrew/lib -lavcodec -lavutil -lswscale
[ -x tools/lz77 ] || clang -O2 -o tools/lz77 tools/lz77.c

python3 -I tools/pld2json.py "$DISC/PL0/PLD/PL00.PLD" build/leon/leon --plw "$DISC/PL0/PLD/PL00W00.PLW"
python3 -I tools/model2ds.py build/leon/leon.json "$DISC/PL0/PLD/PL00.PLD" build/ds/model/leon
# enemies and characters (model/emXX*.bin)
python3 -I tools/em2ds.py "$DISC/SLES_009.72" "$DISC/PL0/PLD/CDEMD0.EMS" build/ds/model > /dev/null

# doors: sound/doorNN.bin and door/doorNN_{model,tex,prog}.bin from COMMON/DOOR/DOORNN.DO2
mkdir -p build/ds/sound build/ds/door
for f in "$DISC"/COMMON/DOOR/DOOR*.DO2; do
    n=$(basename "$f" .DO2 | tr A-Z a-z)
    python3 -I tools/vab2ds.py "$f" "build/ds/sound/$n.bin" > /dev/null
    python3 -I tools/do2ds.py "$f" "build/ds/door/$n" > /dev/null
done

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
# enemy voices (bank 3, one set per room) and Leon's voice (bank 4: CORE00,
# file 0x132 of table 0x800a0118 for character 0)
python3 -I tools/enemse2ds.py "$DISC/SLES_009.72" "$DISC/COMMON/SOUND/ENEMSE.VBS" build/ds/sound > /dev/null
python3 -I tools/vab2ds.py "$DISC/COMMON/SOUND/CORE00.EDH" build/ds/sound/core00.bin \
    --vb "$DISC/COMMON/SOUND/CORE00.VB" > /dev/null

# weapon sounds (PS1 bank 1): sound/armsNN.bin per weapon id
for f in "$DISC"/COMMON/SOUND/ARMS*.EDH; do
    n=$(basename "$f" .EDH | tr 'A-Z' 'a-z')
    python3 -I tools/vab2ds.py "$f" "build/ds/sound/$n.bin" --vb "${f%.EDH}.VB" > /dev/null
done
# Leon with each weapon: model/leon_wNN.bin
for f in "$DISC"/PL0/PLD/PL00W*.PLW; do
    n=$(basename "$f" .PLW | sed 's/PL00W/leon_w/' | tr 'A-Z' 'a-z')
    python3 -I tools/plw2ds.py "$DISC/PL0/PLD/PL00.PLD" "$f" build/leon/w "build/ds/model/$n" > /dev/null
done

# music: MAINxx.BGM sequences and the room -> music table
mkdir -p build/ds/music
for f in "$DISC"/COMMON/SOUND/MAIN*.BGM; do
    n=$(basename "$f" .BGM | tr 'A-Z' 'a-z')
    python3 -I tools/bgm2ds.py "$f" "build/ds/music/$n.bin" > /dev/null
done
python3 -I tools/bgm2ds.py --table "$DISC/SLES_009.72" "$DISC/COMMON/DATA/INIT_TBL.DAT" \
    build/ds/music/bgm_table.bin > /dev/null

# status screen: item icons, names, Leon's portrait
mkdir -p build/ds/menu
python3 -I tools/menu2ds.py "$DISC/SLES_009.72" "$DISC/COMMON/DATA/ST0.TIM" \
    "$DISC/COMMON/DATA/ITEMALL.PIX" build/ds/menu/items.bin > /dev/null
python3 -I tools/itps2ds.py "$DISC/COMMON/DATA/ITPS.ITP" build/ds/menu/items.bin \
    build/ds/menu/pictures.bin > /dev/null

# effect sprites (blood, sparks, casings)
mkdir -p build/ds/effect
python3 -I tools/esp2ds.py "$DISC/COMMON/DATA/CORE00.ESP" "$DISC/COMMON/DATA/CORE01.TIM" \
    build/ds/effect/core.bin > /dev/null

# "YOU DIED" (Leon)
mkdir -p build/ds/die
python3 -I tools/die2ds.py "$DISC/COMMON/DATA/DIED00.DIE" build/ds/die/died00.bin

# the opening movies (Leon meets Claire): need the STRs as raw sectors,
# see tools/extract_str.py; skipped without them
mkdir -p build/ds/movie
for m in OPN1STL:opn1 OPN2NDL:opn2; do
    if [ -f "$DISC/PL0/ZMOVIE/${m%%:*}.STR.raw2352" ]; then
        python3 -I tools/str2ds.py "$DISC/PL0/ZMOVIE/${m%%:*}.STR.raw2352" "build/ds/movie/${m##*:}.bin" > /dev/null
    fi
done

cp -R build/ds/bg build/ds/room build/ds/model build/ds/mask build/ds/sound build/ds/door build/ds/effect build/ds/music build/ds/menu build/ds/movie build/ds/die nitrofiles/
du -sh nitrofiles nitrofiles/*

# RE2 (PS1, SLES-00972) – scripts, cutscenes, effects, media

Findings the DS engine is built on, beyond player control and enemies. Addresses are PS1 RAM
addresses (`SLES_009.72`, disc 1, Europe). **observed** = in the code as written,
**assumed** = interpretation.

---

## 1. Room scripts (SCD)

- Opcode handlers: jump table `0x8009f560`, **138 entries** (`0x00`–`0x89`). Each handler
  advances the thread's program counter (`thread+0x1c`) by the instruction size; the sizes
  in `source/script.c` (`scd_op_size`) and `tools/rdt2json.py` (`SCD_SIZES`) come from
  OpenBiohazard2 and, for the rest, from the handlers themselves. Only `0x1b` (a loop block
  with its count in a variable) is not implemented.
- A thread that jumps to the very end of its script block stops there (room 10F's init
  script does that).
- Work object (`work_set type, index`): 1 = Leon, 3 = enemy slot, 4 = room object (model of
  `obj_model_set`). Members (`FUN_8005202c`): see [enemy-zombie.md](enemy-zombie.md); for
  objects 0x0B–0x0D = position, 0x0F = facing.
- `speed_set` (`0x2f`): index, s16 → `thread+0x158 + 2 * index`. 0–2 position speed, 3–5
  rotation speed, 6–11 their accelerations. `add_speed` (`0x30`) adds 0–2 to the work
  object's position (`+0x38..0x40`) and 3–5 to its rotation (`+0x74..0x78`); `add_aspeed`
  (`0x31`) adds 6–11 to 0–5. Room 102's gate swings open this way.
- `flr_set` (`0x38`, 3 bytes): floor sound area n on (1) / off (else), bit `0x80` of its floor
  field.
- `0x7c`/`0x7d`/`0x7e`/`0x7f` (6 bytes): change the colours / positions of a camera's lights
  (room data `+0x2c`, 0x28 bytes per camera). The DS engine has no room lighting and skips
  them.
- `cut_chg` (`0x29`) also stops automatic camera cuts (`0x800c77d4 |= 0x100`) until
  `cut_auto 1`. `message_on` does not block on the PS1; the PAL dialogue messages of room 101
  are empty (voice only).
  The same holds for all spoken lines (Marvin in 202, Ada, ...): empty or only page codes
  (`\f`). The DS engine shows no box for those, so the English disc has no subtitle text.
  re2ds makes its own: `tools/subs2ds.py` runs whisper.cpp over the converted voice clips and
  movie audio (`subs/subs.txt`: `@clip`, then `start_ms end_ms text` lines); `source/subs.c`
  looks the line up by the clip's file name and the time into it, `menu_subtitle` draws it
  at the bottom of the lower screen (palette entries 240 white / 255 black, which the status
  palette has and the map's sets). During movies the lower screen is black with the lines.
- System flag 0:`0x19` (`0x800c77b4` bit `0x40`) is set once at boot (`FUN_8002a8b8`) and kept
  by the new-game reset (`FUN_8002b9f8`, which clears bits `0x26` only). About 30 rooms choose
  their items and enemies by it; with it Leon A has three more green herbs (103, 20B, 20F), a
  first aid spray (20D) and five more boxes of handgun bullets. `main.c` (`system_flags`) sets
  it for a new game and when loading. NORMAL / EASY (flag 0:`0x1a`) only changes enemy HP, see
  [enemy-others.md](enemy-others.md) section 10.
- `sce_item_get` (`0x76`, 3 bytes: item, count; `0x80054498`): someone hands Leon an item –
  Marvin's Blue Card Key in room 202. Into the inventory, else (DS) the item box.
  `sce_item_lost` (`0x62`, 2 bytes: item; `0x800542c4`): clears the slot holding the item
  (jewels placed, keys used up). Saves made before `0x76` worked get the card key when loaded
  (picked flag 8:5 set, terminal flag 4:0x46 not).
- `sce_em_set` (`0x44`): byte 9 is the enemy's "killed" bit, see
  [enemy-others.md](enemy-others.md) section 9.
- `work_copy` (`0x1d`, 4 bytes: variable, offset, size; `0x8005084c`): writes the variable
  (`0x800cc3e8 + 2 * n`) into the **next instruction** at the offset, a byte (size 0) or a
  u16. Scenes patch their own parameters this way: room 200's first-entry scene feeds a
  counting variable into `pos_set` (Leon's height) and `sce_scr_move` every frame. 42 rooms
  use it (112: 53 times, 500: 59). The room data lies in RAM and is loaded again on the next
  visit, on the PS1 as on the DS.
- `sce_scr_move` (`0x6d`, 4 bytes: s16 at +2; `0x80054998`): sets `0x800cc090`, a vertical
  pan between two background buffers (`FUN_8002ada8`): screen rows `0..s` show the last `s`
  rows of the current camera's picture, the rest the top of the other buffer
  (`0x80198000`). Rooms 200 (cut 11, `s` 100 → 205) and 216 use it. The DS engine skips it
  (the camera's picture stays still); which picture the other buffer holds is open.
- Room 200 (main hall), first entry (flag 4:6 clear, arrival camera 0): event 11 shows
  camera 11 with the pan and lowers Leon from y −1800 (the entrance landing) while the
  music starts; flag 4:6 is set either way. After Marvin (flag 4:7): event 5, Leon turns to
  a sound. Event 8: the Unicorn Medal (item `0x47`) in the statue plays movie 5.

## 2. Characters moved by the scripts (plc_*)

The `plc_*` commands put an object (Leon, Kendo, a zombie) into routine 4 (`0x80061488`) with
a mode from table `0x800a0de0`:

| Op   | Name        | Arguments                                   |
|------|-------------|---------------------------------------------|
| 0x3F | plc_motion  | mode, animation, flags                      |
| 0x40 | plc_dest    | pad, mode, done bit, s16 x, s16 z           |
| 0x41 | plc_neck    | (head turning, ignored here)                |
| 0x42 | plc_ret     | back to the object's own control            |
| 0x58 | plc_rot     | 0 = turn per tick / 1 = once at the end, s16 |
| 0x5B | plc_cnt     | jump to keyframe                            |
| 0x66 | plc_stop    | freeze / resume                             |

Modes (handlers `0x800a0de0`): 0-3 play an animation from one of four sets (`FUN_800621b0`):
0 the room's (RBJ, `obj+0x190`), 1 `obj+0x108`, 2 `obj+0x180` (Leon's walking moves), 3
`obj+0x188`; 4 walk to, 5 run to, 6 stand (idle animation 2), 7 walk backward, 8 backward
without turning first, 9 turn to face, 0x12 walk (and climb a box on the way), 0x14 paused,
0x15 turn to an absolute facing. Flags (`obj+0x1cc`, `plc_flag`): 4 keep going (a new
`plc_dest` of the same mode only moves the destination; on arrival the mode goes on), 8
double speed, 0x10 half speed, 0x40 no pose blending, 0x80 backward. Speed and turn tables
are indexed with 16 for the player, enemy type − 0x40 for people. Arriving sets bit `done`
of flag array 5 (`FUN_800727d8`); only the room start (`FUN_8004f484`) clears bits
0x20-0x3f. **DS port:** the scenes wait for the bit after each destination (room 202:
`do evt_next while ck 5:0x21 0`), so `plc_dest` clears it for the moving modes - else the
first arrival answers every later wait and Leon never reaches Marvin. A `plc_dest` for an
object that is not there counts as arrived. Implementation: `source/plc.c`.

**Start motion:** `sce_em_set` bytes 18-21 are a motion and a control word (`obj+0x158`,
`+0x15a`). At the room's start `FUN_80046e3c` puts every object with a motion into routine 4
as if by `plc_motion`: mode = high byte − 1, animation = low byte, flags = control word.
Marvin (`0x0105`, `0x14`) lies in room animation 5 from the start; Kendo, Ben, Annette and
others start their poses this way (`source/enemy.c`, `room_start`).

RBJ section of the RDT: pairs (mask + EMR offset, EDD offset), both relative to the RBJ start;
mask bit 0 = the player, bit n ≥ 2 = enemy slot n − 2. Leon's climb animations are 6–9.

**The partner** (Ada, Sherry): `sce_em_set` with slot `0xff` creates it (the PS1 keeps it
at `0x800c7a14`), `work_set 2` selects it for `plc_*` and the member commands, and its room
animations are the RBJ set with mask bit 1. Room 301's scene with Ben waits for Ada's
`plc_dest` to arrive; while she was missing the scene stood still.

**Messages** (lang1): `0xFC` new line, `0xFD` + byte a new page (waits for a button), `0xFB` +
byte a yes / no question; the answer is bit `0x1F` of flag array 11 (`0x800de0c4`, set = no),
the same bit the typewriter's question uses (`FUN_8004e2ac`).

**aot_on** (`scd_op_47`) calls the AOT's type handler (`0x8009f490`) as if the player had used
it: rooms hand out puzzle rewards (item AOTs) and change rooms (door AOTs) this way. Type 14
(`0x8004edb0`) starts a computer program (`0x80036a90`, `data[0] | data[2] << 4`, & 3): 0 the
hall's door lock service (`0x80037c6c`: "Hall side doors LOCKED", the Blue Card Key, item
`0x35`, sets flag 4:`0x46`), 1 the fingerprint check (`0x800384dc`).

## 3. Voices (xa_on)

- `xa_on kind, n` (`0x59` → `FUN_8001260c`) sets `0x800c77d4` bit `0x20`; `wsleeping` waits for
  it.
- Kind 0: the stage's clip table, its address stored to `0x800c6184` by
  `COMMON/BIN/STAGE<s>.BIN` (loaded at `0x8011a000`). Kind 1: the common table at `0x80092838`
  (19 entries, 13 used). Entries: u16 channel, u16 start, u16 length (sectors of that channel).
- Audio: `PL0/VOICE/VOICE0<s>.XAS` (stage s; `VOICE00` = common), 16 channels interleaved
  (file sector = block * 16 + channel), XA ADPCM 37.8 kHz mono. `tools/xa2ds.py`.
- When Leon dies (HP < 0 sets `0x800c77b4` bit 4), the death sequence plays common clip 0.

## 4. Room objects (items lying around, gates)

- RDT header byte 2 = model count; section 10 = (TIM offset, MD1 offset) per model. The TIM
  is 8 bpp and shared; each face's CLUT field picks the palette row.
- `obj_model_set` (`0x2d`, 38 bytes): ins[1] = model (255 = none), position at +14, rotation at
  +20.
- `item_aot_set`: data[6] = the model of the item; the PS1 skips items already taken and hides
  their model. `tools/obj2ds.py`.

## 5. Effect sprites (ESP)

`FUN_8001b8fc(param, dir, matrix, offset)` starts an effect: `param` = effect id << 24 |
(sub effect | palette row << 3) << 16 | parameter. Effect ids are looked up in the core ESP
(`COMMON/DATA/CORE00.ESP`, 8 ids) and the room's ESP (RDT offsets 18/19, 8 ids).

- Block: u16 frames, u16 sprites, u16 CLUT id, u16 texture page; frames of 8 bytes (first
  sprite, sprite count, time, size, …): time 255 = go to frame <first sprite>, time 0 = end;
  sprites of 4 bytes (u, v, s8 x, s8 y from the anchor); then a u16 table of sub effects,
  each a list of 24-byte parts (mode 11 = repeats, first frame, scale x/y in 1/4096, fall and
  speed for glass shards, 0x0800 = mirrored).
- Core texels: `COMMON/DATA/CORE01.TIM` (no header, 512 × 256 at 4 bpp) at VRAM (896, 256):
  pages 0x1e (left: smoke, fire) and 0x1f (right: blood, gun). Palettes: VRAM (272, 480 + row),
  as in `COMMON/DATA/TEX_PAL.TIM`.
- Room effects: RDT offset 20 = one 4 bpp TIM per used slot, with its own palettes.
- `sce_espr_on` (`0x3a`, 16 bytes): ?, id, sub | row << 3, object type, object index, u16
  parameter (high byte looks like a size), s16 x, y, z, s16 direction. `sce_espr_kill`
  (`0x4c`, 5 bytes): id, type, object.
- `tools/resp2ds.py`, `source/roomfx.c` (the engine plays the first part of each sub effect).

## 6. Item pictures

`COMMON/DATA/ITPS.ITP`: 100 TIMs of 12288 bytes, 112 × 72 at 8 bpp with their own palette;
picture n shows item n. The PS1 shows them on pickup and on CHECK. `tools/itps2ds.py`.

## 7. Movies (STR)

`movie_on` numbers (`scd_op_6f` → `FUN_80031494`, table `0x8009a578`): 4 R10B, 5 R200,
6/7 R505, 8 R108L, 9 R109, 10 R204L, 11 R409, 12 R700L, 13 R703L, 14/17 R704L, 15 OPN2NDL.
Each is named after the room that plays it (room 2040 plays 10, the licker).

`PL0/ZMOVIE/*.STR`: MDEC video 320 × 160 at 15 fps with interleaved XA audio (stereo,
37.8 kHz, one sector in 8). The audio sectors are Form 2, so the files must be read as raw
2352-byte sectors (`tools/extract_psx_iso.py` does that). Leon's opening: `OPN1STL.STR`
(the diner, Claire) and `OPN2NDL.STR` (the tanker crash). `tools/str2ds.py` (ffmpeg), player
`source/movie.c`.

## 8. Death sequence (DIEDEMO)

`COMMON/BIN/DIEDEMO.BIN`, loaded at `0x80190000`, states in table `0x80193560`, per game tick:

1. Delay by killer type (table `0x8019338c`; zombies 95 ticks).
2. The screen fades to white, faster and faster (`0x801902fc`); it stays white while the
   voice clip still plays.
3. A free camera circles the body (`0x80191200`): parameters per killer in `0x801933ac`
   (28 bytes: radius, shrink per tick, vertical speed, turn speed and start offsets, eye
   height, duration, target type, bone). Zombies: radius 3000 − 8/tick, eye 1000 above the
   hip, turning 2/tick from facing + 600, 91 ticks.
4. `COMMON/DATA/DIED00.DIE` (TIM 256 × 192, 8 bpp; the rest of the file is graphics for its
   ripple effect) behind the body, the light flashes red (`0x80190e88`, 170 ticks).
5. The camera rises and spins, the light dies, the screen fades out (`0x80191080`, 163 ticks);
   then the title.

## 9. Item use, reloading, effects of a shot

See [player-control.md](player-control.md), section 5.

## 10. Room collision, slopes and stairs

Per frame the main loop (`0x80025058`) runs the player routine (`0x80039114`), then
`FUN_80033224(player, 0x8000)`, which tests the room boxes with `FUN_80048964(pos, radius,
1 << obj+0x106, 0x1008000)` unless `obj & 8`:

- A box counts if its quadrant bits (type & 0xF) contain the player's quadrant around the
  collision header's centre (`FUN_80048940`), the circle overlaps it, `id & 0x8000` (player;
  `0x400` enemies), not `id & 0x100`, and its floor bits contain `1 << obj+0x106`.
- Each counted box pushes the player out by its shape (`id & 0xF`, table `0x8009f448`):
  0/9/10/12 as a box (`0x800490a8`); 1–4 (`0x80049378`, `0x800497f0`, `0x80049cb0`,
  `0x8004a15c`) the right triangle in the box's corner (x1 z1), (x0 z1), (x1 z0), (x0 z0)
  with the diagonal as hypotenuse; 5 (`0x8004a60c`) the diamond between the edges'
  midpoints; 6 (`0x8004b3f8`) a circle of diameter w; 7 / 8 (`0x8004b598` / `0x8004b7ec`) a
  stadium: a box along x / z with round ends of diameter d / w (between them the box push);
  11 slope (`0x8004ba54`), 13 `0x8004bea0`. Counts over all rooms: 1–5: 142 boxes, 6: 144,
  7/8: 502.
- **Slope (11)**: height `FUN_8004c060`, 0..1800 over the box from the bottom edge
  ((type >> 4) & 3: 0 x min, 1 x max, 2 z min, 3 z max), y = that − ((type >> 6) & 31) · 1800:
  the field is the top floor, a slope rises one floor. Floor `obj+0x106` = −y / 1800
  (truncated: the lower floor on the way). Off slopes y snaps to −1800 · round(−y / 1800).
- **Stairs (12)** are walls to the push-out. With the same test without push-out
  (`0x80025058`: a point ahead of the player, radius 450, mask `0x8000`) touching one sets
  bit `0x10` in `obj+0x112` and remembers the box (`0x800cf1dc`); in states 0, 1, 2 and 4
  key bit `0x80` (held or pressed) then starts the stairs sequence `FUN_8004bc44`: state 7 (`0x1a` from
  state 4) walks them with `obj & 8` set - no room collision - for
  `−1 − (top − bottom) / 1800` floors, and sets the floor at the end.
- State A (10) is pushing an object: `FUN_80033b10` sets `0x800c77d4 & 0x2000` after 6 frames
  against one that can move.

**DS port:** slopes rise one floor up to the floor in the type field (rooms 409: 5 and 6;
it used to be taken as the rise). Stairs are walked like slopes rising ((type >> 6) & 31)
floors from where Leon steps on. Leon's
collision there counts only boxes on both floors his height at the next step lies between
(`player_floors`, `source/main.c`): the walls beside the stairs are on all their floors, a
wall of one floor reaching over the steps is not (room 103: a floor 2 wall over the top
steps stopped him going up, not going down).

The DS measures these shapes (`penetration`: triangles and the diamond as polygons, the
stadiums by the distance to their axis); it used to take 1-5 as whole boxes (invisible walls
in their empty halves, e.g. room 103's walkway) and 7 / 8 as ellipses (corners cut).
The DS blocks a step that leads into a box deeper than the circle already was (the PS1
pushes out): whoever stands in a box may walk out and along it (room 101: the script puts a
zombie into the shop window, box 11, and makes the window solid again; with "not deeper or
equal" it was stuck there). Lines of sight (shots, enemies spotting Leon) ignore a box one end
stands in. Enemies keep off each other (`crowded`) but not off people (Kendo's body wedged
that zombie in). An enemy is hidden only when a tall solid wall stands both between the
camera and it and between Leon and it (`camera_occluded`; many cameras look through outer
walls), and never behind a box the room script switches (`sca_id_set`: windows, gates).

## 11. Music (BGM)

Three sequence slots: main (`0x800cf110`, 8 bytes: state, start mode, seq id, ..., volume)
and the two parts of a sub piece (`0x800cf118`, `0x800cf120`).

- **Room table** (`0x800cc2cc`, two bytes per room, saved): main byte bits 0-5 = `MAINxx`,
  bits 6-7 = start mode; sub byte bits 0-5 = `SUB_xx`, bit 6 / bit 7 = start mode of its
  first / second sequence; `0xFF` = none. At room entry (`FUN_80055ed8`) a new piece is
  loaded paused (`FUN_80056410`: `SsSeqPlay(seq, PAUSE, 1)`); `FUN_8005631c` then starts each
  slot whose mode is 0. A mode other than 0 waits for the room script's `sce_bgm_control`
  start (gun shop 101: `0x41`, room 204: `0x49`). The same piece goes on across rooms; entered
  with a mode other than 0 it fades out (90 frames). Leaving for another piece fades
  everything out over 90 frames (`FUN_800561a8` → `FUN_80056c20`).
- `SUB_xx.BGM` holds two sequences (the first at offset 0) and one VAB; the last 16 bytes give
  VB, VH, the second sequence, 0 (`FUN_80056828`). 47 of the 141 table entries name one
  (ambience and stingers: 104, 106, 108, 204, 206, 213, ...). The file index tables are
  `0x800a01b8` (MAIN) and `0x800a0238` (SUB), piece n = `MAINnn` / `SUB_nn` (checked by the
  sectors of `0x8009146c`; its size field belongs to the next file).
- Some sub sequences play programs only the main piece's VAB has: `SUB_00` (gun shop 101,
  the fight after the zombies break in) plays programs 5-10, which `MAIN01` holds besides
  its own 0-3; `SUB_00`'s VAB has only a 112-byte program 0. Room 101's script stops the main
  (`sce_bgm_control 0, 2`) and starts sub sequence 1 (`1, 1`) when the zombies come in.
- `sce_bgm_control` (`FUN_8005702c`): bits 28-31 slot (0 main, 1/2 sub), operation 1 start,
  2 stop, 3 resume, 4 pause, 5 fade out (`FUN_800755b8(seq, 0x7f, 0x5a)`, 90 frames); type
  0 = VAB header volume / pan (+0x18 / +0x19), n = program n − 1 (program attribute +0x01 /
  +0x04, any program). Volume and pan are stored minus 1, 0 = unchanged.
- **Pitch** (libsnd `FUN_8007e3e4`): the SPU pitch is `0x1000 · 2^((note − centre +
  shift / 128) / 12)` from the table at `0x800a3a10` (16 steps per semitone, index
  `shift >> 3`): the VAB tone's fine tune `shift` **raises** the pitch. The DS converters had
  it lowering, up to 1.7 semitones off for tones with shift 105-110 (the police station
  pieces 06/07, door and core sounds).
- libsnd applies controller 7 / 10 changes to the notes already sounding; piece 07 swells its
  chords with 246 of them. The police station pieces hold up to 14 notes at once.

**DS port** (`source/music.c`): three sequences (main, sub 1, sub 2) on two banks (MAIN,
SUB), start modes, fade, live volume and pan, 12 shared channels, the oldest held note gives
way. A stop keeps the bank (a start replays from the beginning). A sub sequence takes a
program its own bank lacks from the main bank. `tools/bgm2ds.py --sub` writes `RMU2` files.
The sequencer steps at 60 Hz by a tick task (`music_frame` catches up on up to 6 missed
frames): driven by the main loop it slowed down whenever a pass took two frames and stalled
with held notes during loads. `tests/music_host.c` plays a room's music on the host and
counts voices, steals and sound messages.
Voices follow the SPU envelope of their VAB tone (ADSR1/2, written by `bgm2ds.py`): attack,
exponential decay to the sustain level, sustain rising or falling while held (607 tones of the
music fade out on their own: bells, plucks - without it SUB_01's bell in the city rang on like
a siren), release after the note off (up to ~6 s). Step every 2^max(0, shift − 11) samples of
step << max(0, 11 − shift) (psx-spx), summed over 735 samples a frame; below 1/128 a fading
voice ends.

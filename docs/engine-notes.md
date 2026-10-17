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

Modes: 0 = the room's animation set (RBJ), 1 = the character's own, 4 walk to, 5 run to,
6 stand (idle animation 2), 7 walk backward, 9 turn to face, 0x14 paused, 0x15 turn to an
absolute facing. Flags: 4 loop, 8 double speed, 0x10 half speed, 0x40 no pose blending,
0x80 backward. Speed and turn tables are indexed with 16 for the player, enemy type − 0x40 for
people. Arriving sets bit `done` of flag array 5. Implementation: `source/plc.c`.

RBJ section of the RDT: pairs (mask + EMR offset, EDD offset), both relative to the RBJ start;
mask bit 0 = the player, bit n ≥ 2 = enemy slot n − 2. Leon's climb animations are 6–9.

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

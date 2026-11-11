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
- Item AOTs (handler `0x8004e02c`, type 2): data byte 7 bit 0 lets Leon crouch for it first
  (player routine 6); then the status screen opens as "item found" (`0x800cd750` = 2) with
  the item's picture and the system message "Will you take the\n`0xF9 1` name `0xF9 0`?"
  (`0xF8` the name, colour 1; at `0x80098bf0`) with YES / NO; YES takes it, or "You can't
  carry any\nmore items." (`0x80098c14`). **DS port:** the question in the message box with
  the picture (`pickup_step` in `main.c` reads the answer, flag 11:`0x1F`); the name in green
  (menu.c control code `\x01`); no crouching yet.
- **Using items** (observed, November 2026). AOT type 6 (`SCE_FLAG_CHG`, handler `0x8004e168`)
  sets flag `data[0]`:`data[2]` = `data[4]`. The AOT loop (`0x8004d9c0`) runs every frame: with
  SAT `0x40` and Leon in the area it writes the AOT's number to variable 1, else with SAT
  `0x20` and the point before him in it to variable 0, then calls the handler. The item places
  are type 6 with array 10: e.g. room 206 `aot_set 02 06 21 … 0a 00 2f 00 01 00` = flag 10:`0x2f`
  (the lighter) while Leon faces the fireplace. USE on the status screen (`0x800665a4`): items
  under 30 only buzz (sound 7); the spray and the herbs heal (jump table `0x800117f0`, part of
  the maximum HP `0x800c7956`: spray, G+R, G+G+G, G+R+B all; G+G, G+G+B half; G, G+B a
  quarter; blue and the mixes with it cure poison; a counter for the results); anything else
  needs flag 10:item (`FUN_8007282c` on `0x800c77dc`): then the item goes to variable 2
  (`0x800cc3ec`, step `0x80066790`) and the screen closes, else the buzz. The room's frame
  function asks for it (`cmp 00 02 00 2f 00` in 206, then its event; 10B also checks variable 0
  for which statue). After every frame's scripts `FUN_8004f484` sets variables 0–2 to −1 and
  clears the first 128 bits of array 10 (also at room entry). **DS port:** `check_aots` /
  `script_aot_frame_reset` / `game_item_use` (`main.c`), `use_item` (`menu.c`); array 10 is
  cleared just before the AOT check instead of after the scripts, so the status screen opened
  between two frames still sees it.
- **More AOT types** (handler table `0x8009f490`, observed November 2026). The AOT loop
  (`0x8004d880`) is run per object with a mask: SAT bit 0 the player, the others partner and
  enemies; byte 2 the floor (`0x80` any), byte 3 bit 7 a position relative to an object.
  - 7 water (`0x8004e1d4`): `obj+0x10c` = s16 `data[0]`, the surface (negative = above the
    floor). Walking (`pl_move_01_walk` `0x80039cf0`): on a step frame room effect `0x1a` at
    each foot while the surface is under 300 above it, the floor sound `0x1a` (`FUN_8005797c`);
    deeper than 500 above the floor (`obj+0x1c2`, `FUN_8004c348`) a third slower.
  - 8 move (`0x8004e1f0`): adds `data[0..2]` to the position (not on disc 1).
  - 11 damage (`0x8004e61c`): once per hit lock (`obj+0x1d3 & 0x80`) HP − `data[2]`, the hurt
    routine with kind `data[0]`, pushed at 200 away from the area's middle; room 100's fires
    (and 104, 108, 10A) are such areas with 0 damage.
  - 12 status (`0x8004e794`): opens the status screen.
  - 13 desk (`0x8004e7d4`): `data[0]` its lock bit (the doors' lock array `0x800cc630`, below
    `0x80`), `data[1]` the item AOT inside (at 0, 0), `data[2]` the camera of the desk. Locked:
    the Small Key (`0x1f`, `FUN_80064ba8(31)`; Claire's lockpick in slot 10) or "locked"; open:
    room sound `0x28`, the camera, the item's question. Rooms 10E, 113 (H. Gun Parts), 115,
    201. **DS:** the locks in flag array 63, the key used up.
  - 14 terminal: program 1 is the "RODEM" fingerprint registry (`0x800384dc`, 13 steps at
    `0x800cbe31`): the title, "Enter your user name." and a 10 × 3 letter board
    (`FUN_80036b24` / `FUN_80036e00`); "GUEST" (`0x80010ac0`) → "Register your fingerprint.",
    flags 1:`0x16` and 4:`0x5b`; the next time "Please wait", "Registration complete", "valid
    for 24 hours" and 1:`0x16` cleared. **DS:** `terminal_tick` `TR_*`.
- **Picking items up** (`0x8004e02c`): data byte 7 bit 0 sends Leon into routine 6 first
  (`pl_move_06` `0x8003afe4`: PLD animation 6 forward, the "item found" screen, the animation
  backwards); with system flag 0:`0x19` ink ribbons come as 2. **DS:** `ST_PICKUP`.
- **Combining** (table `0x800a1cb8`, 4 bytes: item, operation, item / result, extra): operation 5
  loads ammunition, 4 upgrades a weapon with its parts (VP70 + `0x20` → `0x04`, magnum + `0x21`
  → `0x06`, shotgun + `0x22` → `0x08`), `0x0b` mixes herbs.
- **Leon's own item** (observed): when the status screen opens it puts the lighter (`0x2F`)
  into inventory slot 10 (`0x800cc660`, after the 8 + 2 others) while flag 3:`0xa2` is set
  (`0x80063fd8`). On disc 1 only room 611 sets that flag (in a scene); where it is set for the
  start was not found. **DS port:** set at a new game and when loading (`system_flags`); the
  status screen shows it in a box beside the condition (`SEL_SPECIAL` in `menu.c`).
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

### Camera zones, random numbers, object compares (observed, October 2026)

- **Camera zones** (RVD, 20 bytes): byte 0 switches the record on, 1 the floor (`0xFF` any),
  2 / 3 cam0 / cam1, then the quad. The cut check (`FUN_8002ba94`) walks the records after
  the shown camera's first one while their cam0 is that camera and takes one only if byte 0
  is set: 21 zones in 11 rooms start off. `cut_be_set` (op `0x61`, `scd_op_61`: camera, n,
  value) sets byte 0 of the n-th record from the camera's first (room 206 turns off the cut to
  the fireplace's close-up after the painting burned). **DS:** room format 5, `role` bit
  `SWITCH_ON`; `game_cut_be_set`.
- The first record of a camera is its view area (`FUN_8002bbbc`, `0x800c5f78` on each cut);
  characters outside it are not drawn (`FUN_80026598`; parts flagged `0x9000`, the shotgun's
  standing legs, still are) - Leon too (he stood in the terminal's close-up).
- **Masks** (observed, November 2026): on each cut `FUN_80046334` builds a sprite per mask
  and a table of (on, group, depth); the PRI's offset groups are numbered from their count
  down to 1. Every frame `FUN_800464c0` links a mask into the ordering table at index
  **depth** (`0x800465c8`), but only while bit *group* of flag array **13 + camera** is set
  (`0x8004655c`; arrays 13-28 are cameras 0-15, 16 bytes apart from `0x800cc524`). Room
  entry (`FUN_8004f5a0`, `0x8004f6c4`) sets all 256 bytes; 211 `set`s in about 40 rooms
  switch groups (room 109: two sets of masks taking turns). A model's triangle goes to index
  OTZ / 8 (`0x80027190`), none below 1 (`0x8002716c` leaves out OTZ < 8), so a mask of
  **depth 0 covers everything**: 921 masks in 34 cameras (room 108 camera 0: the railing in
  front; room 204 camera 6: the wall around a window). The DS had put them at the eye (all
  four corners the same point, never drawn) and showed Leon and the enemies through them.
  **DS:** `mask2ds.py` keeps the screen rectangle of a mask nearer than its near plane
  (`MASK_NEAR_DEPTH`), drawn without the camera in front of every model; `MaskQuad.group`,
  `FLAGS_MASKS`; the masks are drawn before the models so a full polygon RAM drops models
  rather than masks.
- **Threads** (observed): at room entry `FUN_8004fb28` runs the init script's function 0 in
  thread 0 and starts the main script's function 0 in thread 1; `evt_exec ff` takes a free
  thread from 2 on. Every frame the main loop (`FUN_8004fbdc`) starts the main script's
  **function 1 afresh in thread 0** before the threads run: the rooms' checks that go on all
  the time. Room 101's stops the fight music (`sce_bgm_control 01 05`, a fade) once flag 5:5
  and the four zombies' kill flags 6:3e-41 are set - the DS never ran it.
- **Variable `0x1d`** is a random number: the script runner (`FUN_8004fb28`) draws a new one
  every frame before the threads (`FUN_8004f338`: `s = (s + ((s & 0x7fffffff) >> 15) &
  0xffff) + (s & 0x7fff8000) * 2`, the low 16 bits), and `sce_rnd` (op `0x28`) once more.
  It stayed 0 here: room 204's dripping blood (`if cmp 00 1d 01 48 71`: over `0x7148`, then
  sound `0x11` and effect 6/5 where the drop lands) and other random sounds never came.
- `member_cmp` compares Leon's and room objects' members too (position, facing): room 206's
  painting falls `while member_cmp 00 0c 03 a0 f6`; taken as always true the event never
  ended and the automatic cuts stayed off.
- `sce_espr3d_on` (op `0x54`): id, sub, object it follows, u16 parameter, s16 x y z, s16
  vector, s16 direction (`FUN_8001bc10`); without an object the DS starts it like
  `sce_espr_on`.
- **More opcodes** (observed, November 2026; handler table `0x8009f560`):
  - `sce_key_ck` / `sce_trg_ck` (`0x4f` / `0x50`): wanted result, u16 action bits held
    (`0x800c5f4c`) / newly pressed (`0x800c5f50`).
  - `sce_fade_set` (`0x53`, `0x80053bd8`): number 0–2, kind (2 takes the colours away, 1
    adds them), colours, s16 step: the level (table `0x800d76cc`, 0..`0x7fff`) goes by the
    step every frame, from 0 up for a step > 0, from full down for one < 0 (`00 02 07 00 04`
    = to black in 32 frames, `… 00 fc` back). `sce_fade_adjust` (`0x74`): number, s16 level.
    **DS:** the master brightness, the deepest of the three; reset with the room.
  - `sce_shake_on` (`0x5c`, `0x80053e44`): s8 x, s8 y - the picture's offset for all 240
    lines; the scripts give one every frame (the train, the lifts: 40D, 500, 601, 604, 703,
    704). **DS:** the bitmap layer's reference point and a shift after the 3D projection.
  - `weapon_chg` (`0x5a`): the weapon in hand, 0 none. `0x81`: compares the rounds in it
    (compare op, value). `0x88`: item, count taken off it (gone at 0).
  - `sce_espr_on2` / `sce_espr3d_on2` (`0x64` / `0x75`): as `sce_espr_on` / `_3d_on` with a
    number in byte 1; `sce_espr_kill2` (`0x65`) ends the effect by it.
  - `dir_ck` (`0x39`, `FUN_80014fb4`): s16 x, z, angle - the point lies within the angle to
    either side of the work object's facing. `sce_em_pos_ck` (`0x85`, `FUN_8004d000`): two
    characters with no room box (flags `0x8400`) between them (room 109).
  - `plc_neck` (`0x41`, `0x800531d8`): mode 0-6 into `obj+0x1c0` (| `0x80`), s16 x y z into
    `obj+0x170`, two speeds; 862 uses, most on the people of the scenes.
  - `plc_flg` (`0x43`, `0x800533b4`): 0 ors, 1 sets, 2 xors the u16 into `obj+0x1cc` (member
    `0x16`); what its bits do is not looked into. `parts_set` (`0x6e`, `0x80051d4c`): pad,
    part, field, u16 value into the work object's part (124 bytes each, `obj+0x198`): field 0
    / 1 the words at +0 / +2, 2 at +112, 3 the byte at +114, 4-6 its place at +104 / 106 / 108
    (112 and 60D).
  - Still skipped: `plc_neck` (`0x41`), `plc_flg` (`0x43`), `parts_set` (`0x6e`) and the
    parts that burst or drop (`0x7a` / `0x7b`), the lights (`0x6a`, `0x6b`, `0x7c`–`0x7f`),
    `super_on` (`0x72`), the lines (`0x77`–`0x79`), `xa_vol` / `se_vol`, `rbj_reset`,
    `mizu_div_set`, `splc_ret` / `splc_sce`, `sce_espr_task`, `sce_scr_move`, `st_map_hint`,
    `mirror_set`.
- Flag array 33: the area maps found (bit per map page, `maps.bin`), set by the scripts where
  a map lies (room 20B: 2-4); not set at the start (`INIT_TBL`). The map shows rooms not
  visited only with the page's bit.

## 2. Characters moved by the scripts (plc_*)

The `plc_*` commands put an object (Leon, Kendo, a zombie) into routine 4 (`0x80061488`) with
a mode from table `0x800a0de0`:

| Op   | Name        | Arguments                                   |
|------|-------------|---------------------------------------------|
| 0x3F | plc_motion  | mode, animation, flags                      |
| 0x40 | plc_dest    | pad, mode, done bit, s16 x, s16 z           |
| 0x41 | plc_neck    | head turning (`docs/player-control.md`; ignored here) |
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
(`0x8004edb0`) starts a computer program (`0x80036a90`, `0x800c77b8` = `data[2] << 4 | data[0]`):
`& 3` picks it – 0 the hall's door lock service (`0x80037c6c`: "Hall side doors LOCKED", the
Blue Card Key, item `0x35`, sets flag 4:`0x46`), 1 the fingerprint check (`0x800384dc`) – and
`>> 4` is the camera that shows the computer (`FUN_80038a60` cuts to it and keeps the old one in
`+0x2dd`; the hall's camera 15 is the desktop). Observed, October 2026:
- A window opens over 12 frames (`FUN_800373c0(12, 10, 252, 139)`, room sound 6; closing:
  `0x80038b90`, sound 7, back to the old camera).
- `FUN_80037510(per_frame, text, &done)` types into a 30 × 12 character screen (`+4` chars,
  `+0x16c` colours), drawn by `FUN_80037924` as 8 × 10 sprites from (16, 21), CLUT `0x7b40` +
  colour: `|n` colour n, `]` new line, `[nnn` waits nnn frames, `^nn` nn back, room sound
  `0x10` per character, a blinking cursor; past line 12 it scrolls (`FUN_8003781c`). The texts
  (`0x800108c0`..): "DOOR LOCK SERVICE]------------------------------", "Hall side doors:
  |2RELEASED|0]" / "|2LOCKED|0]", "]The doors can be unlocked]by a |2CARD KEY|0.]",
  "]Checking up ID CARD[025.[025.[025.[025.", "]OK!]Hall side doors lock released.".
- After a text it waits for the action button (`0x800c5f4c & 0x1000`). The card: the status
  screen's question (`FUN_8002f2bc(0, 0x100, 7, …)`); yes: flag 4:`0x46`, the card's slot cleared.
- Released (state 7): camera 9, 60 frames, the lock (`FUN_80057364(0x2120000)`, room sound
  `0x12`), 60 frames; camera 2 the same (and camera 12 when `0x800c77d4 & 0x40000000`), then
  back to the screen camera.

## 3. Voices (xa_on)

- `xa_on kind, n` (`0x59` → `FUN_8001260c`) sets `0x800c77d4` bit `0x20`; `wsleeping` waits for
  it.
- Kind 0: the stage's clip table, its address stored to `0x800c6184` by
  `COMMON/BIN/STAGE<s>.BIN` (loaded at `0x8011a000`). Kind 1: the common table at `0x80092838`
  (19 entries, 13 used). Entries: u16 channel, u16 start, u16 length (sectors of that channel).
- Audio: `PL0/VOICE/VOICE0<s>.XAS` (stage s; `VOICE00` = common), 16 channels interleaved
  (file sector = block * 16 + channel), XA ADPCM 37.8 kHz mono. `tools/xa2ds.py`.
- When Leon dies (HP < 0 sets `0x800c77b4` bit 4), the death sequence plays common clip 0.
- DS: `voice_stop` kills the clip's channel only while the clip still plays (`tickGetCount`
  against its end, a frame early). It used to kill it two frames after the end, when the DS
  had often given the channel to a new music note.

**Sound effects** (`FUN_80057364`): the u32 of a bank's sound table gives the program
(byte 1 & 0x7F), the tone (byte 2 >> 4), the first SPU voice (byte 3 & 0x1F) and how many of
the next tones sound with it on the next voices (byte 3 >> 5). Every gun's shot (sound 0 of
`ARMSnn`) is two tones: the same sample dry and with the room's reverb (tone mode 4; the
reverb is on when the room header's byte 6 is not 0, `0x800bc060`); the custom shotgun's
second one is six semitones lower. 34 sounds in 25 room banks and one enemy bank are layered
too. `vab2ds.py` mixes the layers into one sample, each at its tone volume (vol / 127) and
saturated as the SPU's 16-bit mix (the VP70's shot: 2 × 90 / 127, about 3 dB louder than one
tone at full); the reverb is not made. The weapon banks are kept at 16 bits (`--16bit`),
which keeps the shots' fading boom that 8 bits turned to noise after 0.2 s.

## 4. Room objects (items lying around, gates)

- RDT header byte 2 = model count; section 10 = (TIM offset, MD1 offset) per model. The TIM
  is 8 bpp and shared; each face's CLUT field picks the palette row.
- `obj_model_set` (`0x2d`, 38 bytes): ins[1] = model (255 = none), position at +14, rotation at
  +20.
- `item_aot_set`: data[6] = the model of the item; the PS1 skips items already taken and hides
  their model. `tools/obj2ds.py`.

### Collision, pushing, crates (observed, November 2026)

`obj_model_set` (handler `0x800515dc`; the object is `0x800c7f20 + 504 * ins[1]`): ins[2] the
type (`obj+8`; 2 = a crate), ins[7] what it hangs on, ins[10] its flags (`obj[0]`, | 1),
ins[12] the attribute (`obj+0x10`; `0x20` takes flag 1 off: an item's model, off until its
item AOT turns it on), ins[26]/[28]/[30] the box's offset x / y / z and ins[32]/[34]/[36]
half its size x / y / z. `FUN_800349b0` puts the box's middle at position + offset, with x
and z swapped while the object is turned a quarter (`obj+0x76 & 0x400`). Flags: `0x02` no
box, `0x08` the room's boxes do not stop it, `0x40` no characters, `0x80` can be pushed,
`0x100` can be climbed. Only 15 objects of disc 1 have a box: the statues of 10C, the
shelves of 112, the crates of 203, 306 and 603, 216, 403.

Every frame `FUN_80033b10` (not with flag 2:4):

- the box of every enemy and of Leon against every object's (`FUN_80034a78`: a character's
  box is `obj+0x9a` to each side, 1800 high; mode 0 moves the character out, mode 1 the
  object, mode 2 only tells). Which way is taken from where the character stood a frame
  before (`obj+0x4a` / `0x4e`): the side he came from, else the nearer side.
- Pushing: with exactly one object touched on Leon's floor, which he faces square on
  (`FUN_80034fd0`: the point 1.2 × half his width + 128 before him lies in the box) and
  which has flag `0x80`, a counter (`obj+0x144`, only in states 1 and 10) runs. From 6 on the
  object is put before him (mode 1); if it then touches a room box with flag `0x4000` on its
  floor (`FUN_80048e00`), an enemy or another object (`FUN_80034dd8`) it goes back and the
  count starts again, else `0x800c77d4 |= 0x2000`: walking (`0x80039b04`) goes to **state
  10** (`pl_move_0a` `0x8003bf54`). There Leon turns to the nearest quarter with the walking
  move going on, leans on with PLD move 7, then walks move 8 (568 units in 25 frames) while
  up is held and the flag comes again, with room sound 4 on key 1, and lets go with move 7
  backwards and room sound 0.
- A crate (type 2) whose box reaches a room box of shape 10 with flag `0x4000` tips over the
  ledge away from Leon (`FUN_800347dc`, `FUN_80034290`): it slides on at 13, 26, ... a frame,
  turns a quarter (the turn growing by 8), sinks a third of its speed for 12 frames, then
  falls with 160 more a frame to the floor below and bounces once by 30. A crate is drawn
  900 above its place (`0x8003400c`) - the DS had them half in the floor.
- Leon climbs onto an object with flag `0x100` like onto a climb box (`FUN_80035148`:
  `FUN_80035a2c` / `FUN_800358c8`, the point 1.35 × half his width + 128 before him in the
  box; `0x800c79d8` = the object), walks on its top (`FUN_80035ae0`: not off it, but over
  to another such object standing against it) and jumps down where there is room.
- The AOT loop runs for every object too (`FUN_8004eddc`: SAT bit 0 Leon, 1 the enemies, 2
  the **room objects**, 3 the partner): the AOT's number goes to `obj+0x0b` (member 9) and
  an event starts. Room 10C: `aot_set 03 05 44 …` starts function 4 while something stands
  on the plate, which asks `member_cmp 00 09 00 03 00` of statue 1, takes flag `0x80` off it
  (`member_copy 10 00`, `calc … 7f ff`, `member_set2 00 10`) and sets it in place.
- Members of an object (`work_set 04 n`; table `0x80010f00`): 0 flags, 6 type, 9 the AOT,
  `0x0a` attribute, `0x0b`–`0x0d` position, `0x0e`–`0x10` rotation, `0x11` floor, `0x1b` the
  push counter, `0x20`–`0x25` the box.
- The event AOT's handler (`0x8004e128`) starts nothing while flag 2:6 is set.

**Parents** (`obj+0x80`, a matrix): ins[7] of `obj_model_set` and byte 2 of `super_set`
(`0x48`, `0x80051888`: pad, parent, part, s16 place, s16 turn; on the work object): `0x00`
the room, `0x40` Leon, `0x80 | k` a character (0 Leon, 1 the partner, 2 + slot an enemy),
`0xc0 | n` object n; with byte 3 & `0x80` that part of the character - what somebody holds
in a scene (113, 115, 301, 304, 30B, 60C, 611, 702 …). `super_reset` (`0x49`,
`0x80051c2c`: pad, s16 turn) leaves the object in the room where it is. The boards of 205
hang on the window frames (`0xc0`, `0xc2` …) and fall with `super_reset`.

**DS port:** `objects_tick`, `objects_aots`, `push_tick`, `object_matrix`,
`game_draw_attached` (`source/main.c`; `ST_PUSH`, `player.on_object`); enemies are stopped
at the boxes by `game_blocked` instead of being pushed out. Not ported: the splash of a
crate, the water carrying crates (306 moves them by script), flag `0x200`.

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

### Shadows and blood pools

Every character has a shadow (`obj+0x16c`, placed each frame by `FUN_80016254`): a flat
quad on the floor under one of its parts, turned with it, that the PS1 draws by subtracting a
colour from the picture. Its fields (shorts): 0 / 1 offset (turned by the facing), 2 / 3 size
along / across the facing, 4-6 world position at floor height, 7 flags, 8 facing, 10 the
part it follows. The room scripts set it with `kage_set` (op `0x60`, `scd_op_60`: work 1 / 2 /
3 + index, colour, u16 sizes, s16 offsets): room 100's zombies `bf bf bf` (grey) 400 x 600,
the corpses of 113, 119 and 301 `10 bf bf` 800-2000 - a subtraction that takes little red:
blood. A zombie that dies (routine 7, `0x8010a380`) gets that colour, and its shadow grows by
5 a frame on both sides for 120 frames; Leon's (`0x8003ce98`) by 8 for 60.
**DS** (`enemy.c`): zombies start with the grey 400 x 600, `kage_set` sets any enemy's; the
DS has no subtractive blending, so it is a translucent soft disc (a 32 x 32 A5I3 texture) of
what the subtraction leaves (dark red or black) at the strongest subtracted part's opacity.
3D pixels with alpha only blend with the background picture when it is the mixer's second
target: `REG_BLDCNT = BLEND_DST_BG3` (before, the effects' alpha was ignored over the picture).
Leon has no shadow yet.

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

### The ending (ENDING.BIN, observed November 2026)

The last rooms fade out and set system flag 0:`0x0a` (700 after movie 12 R700L, 703 after its
scene; also 11C), then loop. The main loop (`0x80025ed4`) sees it, blanks the screen
(`FUN_8002b1f4(2)`) and runs sub-program 5 (`FUN_80031244`; table `0x8009a4d4`, 12 bytes:
file, entry, load address pointer): file 2 of `COMMON/BIN` = `ENDING.BIN` at `0x8011a000`,
entry `0x8011a0c4`. Its main states (`0x8011c904` by `0x8011f0ac`: fade in, the script,
fade out, the end) run the script `0x8011a5b8` - 19 steps (jump table `0x8011a024`, step
`0x8011f01c`, frame counter `0x8011f0b0`). Flags 1:0 = Claire, 1:1 = scenario B.

- Steps 0/1: A plays MAIN26 (`FUN_80056410` with `0xff26`: main `0x26`, no sub) for 300 frames,
  then movie 17 (R704L); B movie 14 at once.
- Step 4: picture `END10` (Leon) / `END11` (Claire) (`FUN_8011c81c`: raw 320 × 240 16 bpp files
  `COMMON/DATA/END10..14.TIM`), MAIN2A, fade in.
- Step 5: after 480 frames (START: at once) the results fade in over it: texts 13-17 of the
  table `0x8011cb50` (CONGRATULATIONS!!, RANKING, CLEAR GAME, TOTAL TIME, NUMBER OF SAVES), the
  letter (text 4 + rank) and the scenario (texts 9-12), and ten glyphs (`FUN_8011b704`) for
  hh:mm:ss and the saves. 1200 frames later (START after 300) the picture fades out.
- Steps 6-18: the reward (special weapons, "YOUR REWARD … CHECK ITEM BOX!!"), the clear data
  for the other scenario (`FUN_8011c1bc` sets its start: character, HP, room; MEM_CARD.BIN via
  `FUN_800311c4`), the extra modes' pictures; then `0x8011a334`: 60 frames, the new-game reset,
  the title (sub-program 0).

**Rank** (`0x8011b9dc`): the play time (`0x800cc2a0` + VSyncs since the start / load; 216000 an
hour) costs 10 points for every started hour over 2.5 (at most 10); saves (`0x800cc2c0`) 5 each
(20 or more: all 100); first aid sprays used (`0x800cc2be`) 15 each (6 or more: all 100). The
sum of the three; a special weapon (flag 1:8) takes 50 off. Rank = how many of 269, 199, 144,
99, 0 the sum is under: A (269 and more) … E. The rewards, each with rank A or B and unless
already owned (`FUN_8011c0d8`): all time points (2.5 h) → item `0x11`; scenario B with no
spray → `0x0F`; no save → `0x12`.

Texts: a glyph table at `0x8011c914` (10 bytes: tpage, clut, u, v, w, h, screen w, h; `END00.TIM`
sits at VRAM x 512, its palettes from y 480), strings of glyph numbers at `0x8011ccc4` (0xFF
ends one; A = 0 … Y = 22, digits 23-32, `:` 36, space 38). **DS port:** `ending.c` (steps 0-5
and the end; no rewards, clear data or extra modes), `tools/ending2ds.py` (`ending/pic0..4.bin`,
`ending/text.bin`), play time and sprays in the saves (version 5), the debug menu's X starts it.

## 9. Item use, reloading, effects of a shot

**Poison** (observed, November 2026): an effect with bit 8 of its byte 30 that hits a
player (`0x8001fae8`: the spiders' spit; two more places, `0x8002181c`, `0x80023500`) sets
`0x800c7a0c` = 5400 and the counter `0x800c7a0e` = 30. Nothing counts `0x800c7a0c` down:
while it is not 0 Leon's routine (`0x80039294`) takes the counter down every frame and at 0
one HP - never the last - and starts it at 30 again; he walks as in Caution (`obj+0x151` =
1, `0x8003979c`), his model gets a tint (`0x800ac371`, up to 64) and the status screen says
Poison (`0x80064a28`, before Fine / Caution / Danger). The blue herb and its mixes, `plc_heal`
(`0x83`: full HP too) and `poison_clr` (`0x87`) end it; `poison_ck` (`0x86`) asks. **DS:**
`player.poison`, `poison_tick`, `game_player_poison` (the spiders); save version 6. The
tint is left out.

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
  against one that can move (section 4, "Collision, pushing, crates").

**DS port:** slopes rise one floor up to the floor in the type field (rooms 409: 5 and 6;
it used to be taken as the rise). Stairs are walked like slopes rising ((type >> 6) & 31)
floors from where Leon steps on. Leon's
collision there counts only boxes on both floors his height at the next step lies between
(`player_floors`, `source/main.c`): the walls beside the stairs are on all their floors, a
wall of one floor reaching over the steps is not (room 103: a floor 2 wall over the top
steps stopped him going up, not going down). The step onto the stairs already counts as on
them, with their bottom as seen from where Leon stands: room 200's floor 0 box under the
entrance landing reaches over the top steps and stopped him at the bottom step whenever a
step went from before the stairs past their edge. (A walk over every stairs box of disc 1,
5 lines x 8 step offsets each way: 99 of 1280 walks stuck before, 64 after; the 64 are on
the stairs of 111 and 118 and stop the same way before and after, not looked into yet.)

The DS measures these shapes (`penetration`: triangles and the diamond as polygons, the
stadiums by the distance to their axis); it used to take 1-5 as whole boxes (invisible walls
in their empty halves, e.g. room 103's walkway) and 7 / 8 as ellipses (corners cut).
The DS blocks a step that leads into a box deeper than the circle already was (the PS1
pushes out): whoever stands in a box may walk out and along it (room 101: the script puts a
zombie into the shop window, box 11, and makes the window solid again; with "not deeper or
equal" it was stuck there). Lines of sight (shots, enemies spotting Leon) ignore a box one end
stands in. Enemies keep off each other (`crowded`) but not off people (Kendo's body wedged
that zombie in). An enemy is drawn only while it stands in the view area of the shown camera
(PS1 `FUN_80026598` draws Leon and the enemies with the room objects' test `FUN_8002bc00`;
`camera_view_area`): room 101 parks the zombies that break through the shop window at
-32000, where camera 2 looks. Besides, an enemy is hidden when a tall solid wall stands both
between the camera and it and between Leon and it (`camera_occluded`; many cameras look
through outer walls), never behind a box the room script switches (`sca_id_set`: windows,
gates).

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
SUB), start modes, fade, live volume and pan, 14 shared channels. Rooms 100, 103, 109 and
the police station's piece 07 would hold 14-16 notes with their tails; with 12 channels a note
took a sounding one every few seconds (103: 54 held notes and 431 tails cut in 2 minutes), with
14 and the rules below 18 / 251 (109: 2 / 99, piece 07: 0 / 56). A new note takes a free
channel, else the quietest one fading out (its end is least heard), else the oldest held
one. When no DS channel is left, the effects and voice clips take the music's quietest note
(`music_yield`, `sound_play` / `voice_play` retry) and the music its own. `soundPlaySample`
picks the channel on the ARM9 from `soundGetActiveChannels`, which synchronises with the ARM7
(calico, auto-update off), so notes started together do not share one. A sample played once
counts as over a frame early (and sooner when bent up), never late: a channel the DS freed may
already play another sound. A stop keeps the bank (a start replays from the beginning). A sub sequence takes a
program its own bank lacks from the main bank. `tools/bgm2ds.py --sub` writes `RMU2` files.
The sequencer steps at 60 Hz by a tick task (`music_frame` catches up on up to 6 missed
frames): driven by the main loop it slowed down whenever a pass took two frames and stalled
with held notes during loads. `tests/music_host.c` plays a room's music on the host and
counts voices, steals, sound messages and the summed channel volume.
Loudness: the DS mixer adds up all channels and clips once a side gets more than about one
channel at full volume. At a master volume of 64 the police station's and the city's pieces
summed 160-390 (room 100 peaked at 730) and crackled; the PS1 SPU (24 voices, 16-bit sum) has
far more room. The music now plays at 48 and keeps the summed volume of its channels under 160
(`MIX_LIMIT`): after the frame's new notes start, every voice is scaled down at once if the sum
is over, and the gain comes back by 2/256 a frame. Room 100 averages 144 instead of 394.
The voices' volumes go out with 11 bits (`soundChSetVolume`, 0..2047: 0..127 times 1, 1/2,
1/4 or 1/16; a note starts with `soundPlaySample`'s 0..127 and is set at once): at the
music's level a voice had only 20-40 of the 0..127 steps, and a fading note came to 0 and
ended at about −25 dB - in the main hall's sparse piece (MAIN05) the notes broke off.
Pitch bends reach as far as the tone says (VAB tone bytes 12 / 13, `pbmin` down and `pbmax`
up, 0 to 12 semitones; `bgm2ds.py` stores them as `pbmin << 4 | pbmax` in the tone's 8th
byte); all were taken as ±2 semitones, so 16 pieces bent wrong (piece 07 by 1, MAIN00 6 / 12,
MAIN08 1-3). A camera cut stops the main loop for a few frames (reading and unpacking the
picture and the masks); `show_camera_view` calls `music_frame` between the steps so the notes
do not bunch up after it.
Voices follow the SPU envelope of their VAB tone (ADSR1/2, written by `bgm2ds.py`): attack,
exponential decay to the sustain level, sustain rising or falling while held (607 tones of the
music fade out on their own: bells, plucks - without it SUB_01's bell in the city rang on like
a siren), release after the note off (up to ~6 s). Step every 2^max(0, shift − 11) samples of
step << max(0, 11 − shift) (psx-spx), summed over 735 samples a frame; a fading voice ends
when its DS volume (0..2047) comes to 0, or below 1/128.

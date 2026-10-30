# re2ds – handoff for a new Claude instance

This file is a complete briefing. Read it top to bottom once before touching anything. It
describes the project, the rules you must follow, the way the user works, everything that
has been built so far (with the reasons behind the decisions), how to build, test and
debug, how the reverse-engineering workflow works, what is known to be wrong or untested,
and what to do next. Later parts are reference tables (symbols, files, functions, rooms,
enemies, formats). Skim those, search them when needed.

Date of writing: 2026-10-20. Repository state: `develop` at the merge of PR #20
(`feature/enemies`). `main` is older (last release = PR #19).

---

## 0. Ten-line summary

1. **Project:** re2ds = a Nintendo DS port of *Resident Evil 2* (PS1, European disc 1,
   SLES-00972, Leon A). Local folder `/Users/eray1605/Downloads/OpenBiohazard2-master/re2ds`,
   GitHub `eray1605/re2ds`.
2. **It is not an emulator.** The engine in `source/` is new C code for the DS (libnds,
   devkitPro). The game data comes from the user's own disc and is converted by the scripts
   in `tools/` into `nitrofiles/`, which `make` packs into `re2ds.nds`.
3. **The PS1 program was reverse engineered** (Ghidra through the REA tools) so the new engine
   follows the original rules: enemy AI, damage, scripts, doors, camera, sound.
4. **Never commit or share Capcom data:** `build/`, `nitrofiles/`, `*.nds`, the disc, the
   icon made from the disc. They are in `.gitignore` or generated locally.
5. **Language:** talk to the user in **German**, write repository docs and code comments in
   **English**.
6. **Commit only after the user tested and confirmed.** One feature branch from `develop`
   plus one PR into `develop` per commit. Release = PR `develop` -> `main`, then delete and
   recreate `develop` from `main`.
7. **The user tests on a real DS flashcart (R4) and in melonDS** and reports in short German
   messages, often with screenshots. They test "later"; they asked for autonomous work.
8. **Everything on disc 1 for Leon A is playable in principle**: rooms, doors, items, scripts,
   zombies, dogs, crows, lickers, spiders, Mr. X (first version), weapons, saving, map,
   documents, movies, music, title screen. Missing: bosses (alligator, G forms, plants,
   moth, cockroach), grenade launcher, bowgun, rocket launcher, options, ending/ranking,
   Claire's disc.
9. **Build:** `make DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM` in `re2ds/`.
   Host tests (no DS needed) are in `tests/`.
10. **Open on the user's side:** they have not yet tested the dog / crow / licker / spider /
    Mr. X animations, the live map, the hall terminal, message pages, Ada's scenes on
    hardware beyond the screenshots described below.

---

## 1. The user and how to work with them

### 1.1 Who

The user (GitHub `eray1605`, e-mail eraybolel70@gmail.com — use it only for identification)
is a hobbyist who wants a playable RE2 on the DS. They are not a C expert; they are learning
to read the code (they just set up VS Code). They speak German, write fast, often with typos,
and expect results rather than discussion.

### 1.2 Hard rules from the user (memory files in
`/Users/eray1605/.claude/projects/-Users-eray1605/memory/`)

- **Always answer in German.** Repository documents (README, docs/, code comments, commit
  messages, PR texts) are English.
- **re2ds git workflow:** work on `develop`; every commit gets its own feature branch and a
  PR into `develop`. Never commit the ROM or Capcom data.
- **Commit/push only after the user tested and confirmed** ("nicht immer alles sofort
  pushen"). Autonomous implementation without commits is the default until they say
  otherwise. The last confirmation was: "Ja, Feature-Branch committen und PR nach develop
  öffnen" followed by "Ja, merge PR #20 in develop."
- Downloads need approval (state filename, source, size). sudo is done by the user.
- Releases: PR `develop` -> `main`, merge, then **delete `develop` (remote and local) and
  recreate it from `main`**. The user asked for this several times.

### 1.3 How the user communicates

- Short messages: "die hunde machen keine geräusche", "ok klappt mache bitte weiter".
- They post screenshots from melonDS. Read them carefully; they usually show a real bug.
- They sometimes ask conceptual questions (how does the converter work, how do I set up
  VS Code). Answer plainly in German, step by step, no jargon dumps.
- They get annoyed by: things that "must be done in pause", unreadable long texts, features
  that freeze the game, extra confirmation questions. They like: fast fixes, clear lists of
  what to test.
- At the end of every work block give **a German list of what to test**, numbered, concrete
  ("Raum 212: hört man Bellen?").
- When something cannot be verified on the host, say so honestly ("nur am PC geprüft").

### 1.4 The Claude Code environment (desktop app)

- The task list tool (`mcp__ccd_turn__update_plan`) must be kept up to date, and every turn
  that edits files or runs commands ends with `mcp__ccd_turn__wrap_up` (recap, optional
  question + buttons). After `wrap_up` with a question, write no further text.
- A usage limit can be reached mid-turn. When the system says so, finish the current piece and
  tell the user what is done and what is left.
- Use the scratchpad directory for temporary files:
  `/private/tmp/claude-501/-Users-eray1605-Downloads-OpenBiohazard2-master/<session>/scratchpad`.
  The repository itself must stay free of temporary material.
- macOS quirks met repeatedly: there is **no `timeout` command** (use
  `perl -e 'alarm N; exec @ARGV'`), `sed -i` needs `''`, `awk` has no `strtonum`, AddressSanitizer
  binaries produced hangs/no output (use `-fsanitize=undefined` only), and shell variables
  in `rm` were blocked by a safety check (write into a new folder instead).
- A `.vscode/` folder exists locally (include paths, build task); it is excluded through
  `.git/info/exclude`, do not commit it.

---

## 2. The project in detail

### 2.1 Goal

A complete RE2 for the DS (both characters eventually, but disc 1 / Leon A first). ROM size
limit 512 MB for the card; the current ROM is about 437 MB. The DS has 4 MB RAM, a 2D/3D
chip with 2048 polygons per frame, 6144 vertices, 512 KB texture memory, 16 sound channels,
ARM9 67 MHz. These limits shape everything (see ROADMAP.md).

### 2.2 Why not an emulator

The PS1 uses a MIPS R3000, different graphics and sound hardware. Instead the engine is
rewritten and the game's **data and rules** are reused. Room scripts (SCD bytecode) are
interpreted unchanged by `source/script.c`, so puzzles/doors/events behave as in the original.

### 2.3 Data flow

```
disc .bin ──extract_psx_iso.py──> build/disc1/ (files)
   │                                   │
   │                SLES_009.72 ──REA/Ghidra──> knowledge (docs/, symbols)
   ▼                                   │
tools/build_assets.sh + convert_room.sh ──> build/ds/ ──copied──> nitrofiles/
                                                  │
source/*.c ──make (devkitARM)──> re2ds.elf ──ndstool + nitrofiles──> re2ds.nds
```

Disc image: `/Users/eray1605/Downloads/Resident Evil 2 (Europe) (Disc 1)/Resident Evil 2 (Europe)
(Disc 1)/Resident Evil 2 (Europe) (Disc 1) (Track 1).bin`. Extracted tree: `re2ds/build/disc1/`
(`COMMON/`, `PL0/` (Leon), `ZMOVIE/`, `SLES_009.72`). Only the European disc 1 works because
many tables are read from fixed addresses of that executable.

### 2.4 Repository layout

| Path | Purpose |
|---|---|
| `source/` | the DS engine (C, libnds). ~11,700 lines. |
| `tools/` | converters (Python 3, two small C tools `bss2ppm`, `lz77`), `research/` helpers |
| `tests/` | host tests (compile on the Mac, no DS): `enemy_host`, `cutscene_host`, `script_host`, `door_host` |
| `docs/` | knowledge about the original engine, written in English |
| `README.md`, `ROADMAP.md` | user-facing: setup for Windows/WSL/Mac, controls, status, plan |
| `start.command` | macOS helper: closes melonDS, copies the ROM to the SD sync folder, starts it |
| `Makefile` | builds with devkitARM; Docker fallback; icon hook |
| `build/` | intermediate files (ignored) incl. the extracted disc |
| `nitrofiles/` | converted game data (ignored; packed into the ROM) |

### 2.5 Environment on the user's Mac

- devkitPro at `/opt/devkitpro` (libnds, calico, devkitARM). Build command above.
- melonDS 1.1; config `~/Library/Preferences/melonDS/melonDS.toml` with DLDI enabled,
  `ImageSize 3` (1 GB SD image because the ROM is written there), FolderSync on
  `~/Documents/re2ds-sd`. `start.command` copies the new `re2ds.nds` there when its header
  differs. melonDS has no argv, so the engine reads NitroFS from the SD copy of the ROM
  ("sd_files" fallback in `main.c`).
- The user also uses a real DS with an R4 flashcart (icon request came from there).
- Python 3 with no extra packages (no PIL). `ffmpeg` is used by the converters
  (`bg2ds.quantise`, movies). `sips` (macOS) is handy to turn PPM/BMP into PNG for viewing.
- REA (Reverse Engineering Assistant) tools via MCP, Ghidra 12.1.4. See section 8.

---

## 3. Chronological history of the work

The project had many sessions; the earlier ones are only summarised. All of it is merged into
`main` (PR #1-#19) except the last block (PR #20 in `develop`).

### 3.1 Phase 1: rooms walkable (done before this handoff)
- Extractor for the PS1 ISO (`extract_psx_iso.py`), 125 rooms converted (`rdt2json.py` ->
  `room2ds.py`), backgrounds (`bss2ppm`, `bg2ds.py`: 256x192, 255 colours, LZ77), masks
  (`bssmask.py`, `mask2ds.py`), doors with their animations (`do2ds.py`, `door.c`), floors,
  stairs, slopes, collision, debug warp menu (SELECT; Leon arrives where a neighbouring room's
  door into the room puts him, the converter's spawn point only in 11C, 11D, 40C, 40F, 500, 509
  and 600).

### 3.2 Phase 2: script system
- `script.c` interprets the SCD opcodes with the sizes taken from the PS1 handlers. Threads,
  flags (64 arrays of 256 bits), variables, AOTs (area objects: doors, items, messages,
  events, typewriters, item boxes), cutscenes with `plc_*` commands, camera cuts, voices.

### 3.3 Phase 3-4: player and zombies
- Tank controls with health-dependent animations, aiming, firing, reload, auto-aim, grabs and
  struggling, death sequence ("YOU DIED"), zombies with the original AI, path zones
  (`blk2ds.py`), people moved by scripts (Kendo), shotgun impact (thrown back, heavy recoil).

### 3.4 Phase 5-7 pieces (earlier in this long session)
- **Music**: sequencer (`music.c`) with the PS1 sequence data, instruments as 8-bit samples,
  room music table that scripts change (`sce_bgmtbl_set`, op 0x57), volume/pan semantics from
  `sce_bgm_control`, saved in the save file. Later: start modes of the table (bits 6-7), fade
  out, live cc 7/10, 12 channels, and the fine tune sign (it raises the pitch; it was
  lowering it, which put the police station music out of tune). SUB_xx: both sequences,
  programs a sub lacks from the main bank (the fight at Kendo's: SUB_00 with MAIN01's tones).
  See [engine-notes.md](engine-notes.md) section 11.
- **Sound effects** (`sound.c`, banks keyed by note), enemy banks per voice kind pair
  (`enemse2ds.py`), voices (`xa2ds.py`, IMA-ADPCM), the movie player (`movie.c`, RMV1/RMV2,
  FFmpeg-decoded 256x128 frames, 15 fps opening / 10 fps in-game, 1 kHz TickTask clock).
- **Menus** (`menu.c`): inventory on the lower screen, equip/use/check/combine, item pictures,
  item box (64 slots), documents ("files", `files2ds.py`), saving with ink ribbons into
  12 slots (`save.c`, `re2ds.sav` on the SD card), title screen with NEW GAME / LOAD GAME
  (`title2ds.py`), opening movie after NEW GAME, SELECT at boot skips movies.
- **Windows/WSL setup docs**, Docker fallback for devkitPro (devkitpro.org returned 403 at one
  point), `docs/how-re2ds-works.md` with diagrams for laypeople.

### 3.5 This session's block (all in PR #20)

In order of work:

1. **Enemy survey**: which enemy types occur in which rooms of disc 1 (table in section 12).
2. **Dog** (type 0x20): decompiled overlay `CDEMD0.EMS + 0x25b000`, states idle / circle /
   hunt / attack (leap with bite, bounce-off) / thrown back by hits / death. Per-weapon damage.
3. **Leon hurt/killed outside a grab**: `game_player_hurt(kind)` and `game_player_killed()`
   with PLD animations 3/4/5 and 2, state `ST_HURT`, voice calls.
4. **Crow** (0x21): perched, takeoff, flying band over Leon, dive, peck while sitting on him,
   shake-off with key presses, hit -> falls and dies.
5. **Licker** (0x22/0x24): hears noise (3000 any step, 9000 run/shot), turns, crawls, claw
   (keys 17-24), tongue (keys 1-5), jump attack (80/100 damage, survive with >= 40).
6. **SMG and flamethrower** (continuous fire): PLW moves 10-19, ammo accounting every 8th call.
7. **Map** (`map2ds.py`, `MAPS.PIX` + tables from the executable): 20 pages, 137 rooms; first
   as R-screen, then, on the user's request, **live on the lower screen**; START opens the
   inventory, Y toggles map/status, touch on the map opens the inventory, touch on the title
   strip flips pages; visited rooms are saved (save version 4). Leon's marker was removed on
   request (only the current room blinks).
8. **Spider** (0x25) and small spiders (0x26), **Mr. X** (0x2a) first version.
9. **Non-zombie fallback**: types without own AI no longer use the zombie *grab* (their models
   lack set 2 = Leon's grab animations, which would crash); they strike instead.
10. **Partner object** (Ada/Sherry): `sce_em_set` slot 0xff, `work_set 2`, RBJ mask bit 1. Fixed
    the **freeze in room 301** (Ben scene waits for Ada's `plc_dest`). The user's screenshot
    of Ada's odd arms turned out to be the original animation data (rendered the same on the PC
    from the raw disc files); the user confirmed in-game matches.
11. **Door skip**: B during a door animation.
12. **Enemy sound bank fix**: bank chosen when enemies spawn after room entry (dogs were
    silent because they are spawned by the exec script after `enter_room`). Dog footsteps
    from EDD flag 0x40000.
13. **Message pages and yes/no**: converter keeps `0xFD` (new page -> `\f`) and `0xFB`
    (question -> `\v`); `menu.c` pages, YES/NO, answer in flag 11:0x1F.
14. **`aot_on` opcode** (scd_op_47) implemented -> calls the AOT handler; many rooms use it for
    puzzle rewards and scripted door use.
15. **Hall terminal** (AOT type 14, `terminal_door_lock_service`): "DOOR LOCK SERVICE", needs the
    Blue Card Key (item 0x35), sets flag 4:0x46, key is consumed.
16. **ROM icon** from the disc's own title art (eye + red "2") via `tools/icon2ds.py`, built
    into `build/icon.bmp`; Makefile uses it when present.
17. **Docs** updated (`enemy-others.md`, `player-control.md`, `engine-notes.md`, ROADMAP).
18. **Host-test sweep** with UBSan over all rooms: clean.

---
## 4. Engine architecture (source/)

### 4.1 Timing and the main loop (`main.c`, function `main`)

- The DS runs at 60 Hz. The **game logic runs at 25 Hz** (`GAME_HZ 25`): the PS1 PAL game waits
  two vsyncs per gameplay frame. The loop accumulates `game_tick_acc += GAME_HZ` per DS frame
  and runs ticks while `game_tick_acc >= DS_HZ`. One PS1 frame = one game tick here, so every
  PS1 constant (speeds per frame, timers in frames) is used 1:1 per tick.
- Per DS frame: `scanKeys`, L opens the documents, Y toggles the lower screen, `menu_frame`
  (inventory / messages; returns "paused"), the live map (`map_live`), `update_menu` (debug),
  `update_player_frame` (input, turning, aiming), then the tick loop:
  `update_player_tick` -> `enemy_tick` -> `update_pose` -> `die_tick` -> `effect_tick` ->
  `roomfx_tick` -> AOT checks -> special moves -> `script_tick` -> `map_mark_room`.
- The tick loop does not run while a message is open (`message_text`) or the hall terminal
  runs (`term.active`). `terminal_step` runs once per DS frame when no message is open.
- `pmMainLoop()` (calico) is called every frame (lid, reset).

### 4.2 The player (`main.c`)

- Struct `player` (position x/y/z in PS1 units, dir in DS units = RE2 units * 8, i.e. 32768 =
  360 degrees), state `PlayerState`:
  `ST_IDLE, ST_WALK, ST_RUN, ST_BACK, ST_TURN, ST_AIM, ST_FIRE, ST_RELOAD, ST_CLIMB, ST_JUMP,
  ST_FALL, ST_LAND, ST_HURT, ST_SCRIPT, ST_GRABBED, ST_DEAD`.
  Order matters: many checks use `>= ST_CLIMB` (no control) / `>= ST_GRABBED` (enemy.c moves
  him) / `< ST_HURT` (AOT checks allowed).
- Model `leon.bin`: 10 PLD animations (0-9), then the weapon set PLW (from 10 on;
  `PLW_FIRST 10`). Health levels Fine/Caution/Danger pick walk 0/3/6, run 1/4/7, stand 2/5/8.
  PLD 2 = death fall (113 keys), 3 = hurt from front, 4 = hurt from behind, 5 = light hit.
- HP 200 (PS1 `obj+0x156`); damage function `game_player_damage(d)` = PS1 `FUN_8003cf38`:
  damage below 41 counts x1.5; the first drop below 0 by at most 14 leaves 0 HP once
  (`saved_once`), otherwise death.
- `game_player_hurt(kind)` (0 light, 1 front, 2 back): PLD animation, pushed +-200 for 3 ticks
  (`hurt_tick`). `game_player_killed()`: PLD 2, then `ST_DEAD` starts the death sequence.
- Weapons: table `weapons[]` (item, range, damage near/mid/far from zombie table `0x8009c470`,
  lower key from `0x8009eed8`, reload events from PLW EDD flags, pump keys). Items: 1 knife,
  2 VP70, 3 Browning, 4 custom handgun, 5 magnum, 6 custom magnum, 7 shotgun, 8 custom
  shotgun, 0x0F SMG, 0x10 flamethrower. Frame handlers (muzzle flash, casings, shells) as
  data `shot_fx[]`. Continuous fire for 0x0F/0x10: `auto_fire_tick`.
- Special moves: climbing/jumping down via the room's RBJ animations 6-9.
- Death sequence (`die_tick`): DIEDEMO.BIN reimplementation (white flash, orbit camera,
  YOU DIED picture, fade) then a new game / load.

### 4.3 Enemies (`enemy.c`, ~3,200 lines)

Common:
- `MAX_ENEMIES 33`: script slots 0-31 like the PS1 (room 40B uses 24) plus `PARTNER_SLOT 32` (sce_em_set slot 0xff); room animation sets only for slots < `RBJ_SLOTS` 16. `MAX_TYPES 4`
  models loaded at once (texture memory). Models `model/emXX.bin`, `_tex.bin`, `_sets.bin`
  (animation sets: set 0 = first EMD EDD/EMR pair, set 1 = second, set 2 = third).
- `enemy_spawn` (sce_em_set): flags = variant (byte 4) | status (byte 5) << 8. Status bits:
  0x4000 hold, 0x8000 off (not processed/drawn), 0x2000 on ground, 0x1000 direct target.
- `enemy_tick` per tick: distance, sight check every 8 ticks (`game_sight_blocked`), path goal
  (`update_goal` with zones), push/bleed, then the type dispatch:
  `is_person` -> stands / plc; `routine == 8` -> zombie script modes; then `is_dog`,
  `is_crow`, `is_licker`, `is_spider`, `is_tyrant` -> own tick functions; else zombie states.
- `enemy_hit` (Leon's shot or stab): nearest living enemy in a +-22.5 degree cone in sight;
  damage by distance box (<3000, <6000, farther); per type: crow (any hit kills), dog table,
  licker table (0x22 / 0x24), spider, tyrant (just bleeds); zombies: magnum head burst,
  shotgun throw-back / heavy recoil, stagger, fall.
- Helpers added this session: `turn_delta` (FUN_80014fb4), `move_dir` (move along a direction
  with room collision and crowding), `dog_enter` (state + sub reset, used by all new AIs),
  `same_floor`, `strike` (fallback attack for types without AI).
- Animation: `set_anim(e, set, n, key)`; `anim_step(e, &fwd)` gives root motion; `update_pose`
  eases into new animations over 7 ticks like the PS1 (`FUN_80028a4c`).
- Drawing (`enemy_draw`): nearest first within the GX vertex/polygon budget
  (`VERTEX_LIMIT 5900`, `POLYGON_LIMIT 1980`).

Per type (details and PS1 addresses in `docs/enemy-zombie.md` and `docs/enemy-others.md`):

| Type | Name | States here | Notes |
|---|---|---|---|
| 0x10-0x1F | zombies | LURK, CHASE, LUNGE, GRAB, LYING, EAT, DOWN, DYING, DEAD | original AI, grabs with Leon's grab animations (set 2) |
| 0x20 | dog | IDLE, CIRCLE, HUNT, ATTACK, HURT(9), DYING | speed-driven (`obj+0x144`), leap 280/280, bite 30 HP, step sound 8 |
| 0x21 | crow | PERCH, TAKEOFF, FLY, DIVE(13), ON(14), FALL/DYING | 10 HP, one attacker at a time (`crow_lock`), peck 7 HP |
| 0x22/0x24 | licker | IDLE, TURN, STALK, CLAW, TONGUE, JUMP, HURT | blind (noise), claw lock, always on the floor (ceiling variants drop) |
| 0x25 | spider | IDLE, WANDER, TURN, CHARGE, SPIT, EVADE, HURT | wall/ceiling variants drop when Leon is near |
| 0x26 | small spider | crawl, nip 2 HP | 1 HP |
| 0x2a | Mr. X | STAND, WALK, ATTACK | 20000+ HP, three attacks (anim 12/15/3), first version |
| 0x2d | zombie arms | HIDDEN, WAVE, REACH, HOLD, RETRACT, JOIN, GONE | room 205; hold Leon, no damage; `hide_bones` hides the model's other arm |
| 0x3a | giant moth | PERCH, TAKEOFF, IDLE, DECIDE, HOVER, APPROACH, SPORE, CLING, FLYOFF | room 613; clings: 3 HP / 4 ticks |
| 0x3b | larvae | CRAWL, SPIT, DROP, SIT, CIRCLE, SPAWNER | 1 HP; spawners not ported |
| 0x2e/0x39 | Ivy | WAIT, WALK, SWIPE, SPIT, GRAB, HURT | damage values guessed |
| 0x2f | vines | SWAY | Ivy's chain objects not built |
| 0x31 | Birkin G2 | ENTER, WALK, LUNGE, SWIPE, CLOSE, SWEEP, FLINCH, PUPPET | room 509; entrance via member 0x17 |
| 0x34 | Birkin's last form | WAIT, DROP, WALK, CLAW, CHANGE, FLINCH | room 617; signals 1/2/4/8/0x10 via member 0x17 |
| 0x29 | cockroaches | IDLE, CRAWL, JUMP, CLING, FALL | room 40B pipe (axis built in) |
| 0x3e/0x3f | lab machines | ST_PROP | only animate |
| 0x23 | alligator | WAIT, CRAWL, TURN, END, CYLINDER, FLINCH, AWAY | room 40A chase; gas cylinder ends it (flag 5:0x1e) |
| 0x40+ | people | stand / plc | Ada (0x41) as partner, Ben (0x46), Kendo (0x48) ... |
| 0x27 0x28 0x2b 0x2c 0x30 0x33 0x36 0x37 | scene actors / plain fights | ACT_STAND (no set flag), ACT_IDLE/WALK/ATTACK/FLINCH | stand-ins, table actor_types |
| others | none left in Leon's rooms | (zombie AI + `strike` as a fallback) | |

**Enemies stay dead:** `sce_em_set` byte 9 is a flag bit (array 6 in stages 1-3, array 7 later);
`script.c` skips the spawn when it is set and passes array/bit in spawn flags bits 16-31;
`enemy.c` sets it when the enemy's HP drops below 0 (`kill_flag`).

### 4.4 Scripts (`script.c`, `script.h`)

- SCD bytecode of the room (init script once, exec script function 0 on entry, events start
  other functions). `NUM_THREADS` threads, each with `pc`, sleep, block stack, call stack,
  `work_type`/`work_index` (work_set: 1 player, 2 partner, 3 enemy, 4 object).
- Flags: `flag_get(array, bit)`, `flag_set`. 64 arrays * 256 bits. Known arrays: 0-2 system,
  4 (`0x800cc450`) scenario/story flags, 5 (`0x800cc470`) room/event flags incl. plc arrival
  (`done`), 8 picked items, 9 (`0x800cc508`) visited rooms on the PS1 map, 11 (`0x800de0c4`)
  message choice (bit 0x1F = "no"), 32 (`0x800cc51c`) rooms cleared, 33 (`0x800cc520`) map
  found. The PS1 pointer table of arrays is `0x8009f94c` (index -> address, see section 11).
- Opcodes implemented: control flow (if/else/while/for/switch, gosub, evt_exec, evt_next,
  sleep), ck/set/cmp/calc/member_*, AOT set/reset/on, door_aot_set, item_aot_set, obj_model_set,
  sce_em_set, plc_* (via `plc.c`), cut_chg/cut_replace, message_on, xa_on, se_on, bgm control,
  bgmtbl_set, movie_on, sca_id_set, pos_set/dir_set, speed_set/add_speed, espr_on/kill,
  work_copy (0x1d: a variable into the next instruction's parameters, 42 rooms) ...
  Skipped: sce_scr_move (0x6d, the background pan of rooms 200 and 216), see
  engine-notes.md section 1.
  Unknown opcodes are skipped (counted in `script_unknown_ops`).
- `OP_AOT_ON` (71) now calls `game_aot_on(n)` and yields (the PS1 runs the handler first).
- `OP_MESSAGE_ON` shows the room message and the thread waits until it is closed.
- `OP_MOVIE_ON` plays the movie synchronously and clears flag 0:0x16.

### 4.5 plc (`plc.c`)

Scripted movement of characters (`plc_dest`, `plc_motion`, `plc_rot`, `plc_neck` (ignored),
`plc_ret`, `plc_cnt`, `plc_stop`): mode 0 animations from the room's RBJ set, other modes from
the character's own sets. Arrival sets the `done` bit in flag array 5 -> scripts wait for it.
This is why a missing character (e.g. the partner before this session) freezes scenes.

### 4.6 Menu / lower screen (`menu.c`, `menu.h`)

- Lower screen = 8 bpp bitmap BG (sub engine, `fb[256*192]`), palette from `menu/items.bin`.
- States: `focus` (inventory open, pauses the game), `message` (text box; paged), `picture`
  (CHECK picture), `lower_status` (Y: status screen instead of map during play),
  `status_shown`.
- `menu_lower_map()` tells main.c when the lower screen belongs to the map; `menu_overlay`
  draws other pictures there (the map); `menu_overlay_end` gives it back.
- Text: `text_wrapped` stops at `\f` (page) and `\v` (question); `draw_message` shows a page
  (4 lines; 3 + YES/NO for questions). Left/right selects; A confirms, B = no; the answer is
  written to flag 11:0x1F before the message closes (scripts check it right after).
- Inventory: 8 slots, commands EQUIP/USE/CHECK/COMBINE, combining weapon + ammo and herbs.
- Item box: `menu_item_box`. Choice lists: `menu_choose` (save slots, files).

### 4.7 Map (`main.c`, section "map", data from `tools/map2ds.py`)

- `map_page(stage0, room)` = PS1 `FUN_80069cbc`; `map_piece(page, room)` = `FUN_80069fb4`.
- `map_seen[20]` bits = visited pieces, saved in the save (version 4), set by `map_mark_room`
  every tick.
- `map_live(redraw)`: loads the page image (64 KB, 4bpp values expanded to 8 bpp), composes
  the title strip and all pieces at 4/5 scale into a 256x192 buffer, blinks the current room
  via palette (`map_palette`), redraws only when the key (page, piece, seen bits) changes.
- Colours are our own (the PS1 makes its CLUTs at run time). Pixel values in pieces are kinds:
  1 floor, 4 walls, 2/3/5 stairs/furniture, 8 doors, 14/15 highlights.
- Touch on the title strip (y < 30) flips to the next page with a visited room; other touches
  open the inventory (handled in `menu_frame`).

### 4.8 Saving (`save.c`, `save.h`)

- `re2ds.sav` on the SD card (libfat), header "RSV1", version, slot count, slot size.
  `SAVE_VERSION 4`. Older versions are read as a prefix (new fields stay zero).
- `SaveSlot`: stage, room, camera, position, HP, inventory, item box, door locks, script
  state (flags + variables), music table, documents (`files`), `map_seen[20]`.

### 4.9 Sound, music, voices, movies

- `sound.c`: banks "RBS1" (`SoundBankHeader`, entries by note range, 8-bit when
  `reserved == 1`). Banks: room bank (footsteps, room effects), enemy bank (chosen by
  `pick_enemy_bank()` from the voice kinds of spawned enemies), weapon bank (ARMSnn), voice
  bank (CORE00), door banks.
- `music.c`: sequencer for the BGM files, room table (`music_table_*`), 8-bit instruments.
- Voices: `xa2ds.py` per stage (`VOICE0s.XAS`), `voice_play(kind, n)`.
- `movie.c`: RMV3 files (groups of up to 30 frames with one palette: a key frame, then only the changed 8x8 tiles; `tools/rmvdelta.c`) at 15/10 fps, audio ring buffer, TickTask clock; still plays RMV1/RMV2.

### 4.10 Doors, effects, room objects

- `door.c`: DOORnn.DO2 programs (door model, camera, sound), played after the next room was
  loaded behind a black screen; B skips (`use_door`).
- `effect.c`: blood sprites (`game_blood`), `roomfx.c`: the room's ESP effects plus CORE00
  effects (muzzle flash, smoke, casings via `roomfx_throw`).
- Room objects: `obj2ds.py` models placed by `obj_model_set`, drawn only when inside the
  current camera's view area (PS1 `FUN_80033f70` / `FUN_8002bc00`).

### 4.11 The hall terminal (`main.c`, "terminals")

`SCE_TERMINAL = 14`. `terminal_start(a)`: program `data[0] & 3`, screen camera `data[2]` (the
hall: camera 15, the computer's desktop); 0 = door lock service (implemented), 1 = fingerprint
check (`terminal_rodem_fingerprint 0x800384dc`, "The computer does not respond." for now).
`terminal_tick` runs at the game's rate while `term.active` (the game ticks stop): it cuts to
the screen camera, opens a window over 12 ticks, types the PS1's texts into it a character a
tick (sound 0x10; `|n` colour, `]` line, `[nnn` pause), waits for A/B after each, asks for the
Blue Card Key in the message box, then shows the doors (camera 9 and 2, 60 + 60 ticks with the
lock sound 0x12 between) and closes the window. The text is drawn into the background bitmap
on five palette entries taken from the picture's least used ones (`terminal_palette`, the
picture reloads afterwards); the font is libnds' 8 x 8 squeezed to 6 columns. Details:
`docs/engine-notes.md` (computer programs).

---

## 5. Data formats (summary; full definitions in `source/formats.h`)

| Magic | File | Content |
|---|---|---|
| RBG1 (`BgHeader`) | `bg/roomXXX_camNN.bin`, `files/*.bin`, `title/*.bin` | u32 magic, u16 palette[256], LZ77 8 bpp 256x256 (or 256x192) |
| RMK1 (`MaskHeader`) | `mask/*.bin` | foreground mask atlas + quads |
| room (`RoomHeader`) | `room/roomSRR0.bin` | cameras, switches, boxes, doors, scripts, messages (ASCII + `\n \f \v`), floor sounds |
| RMD1 (`ModelHeader`) | `model/*.bin`, `room/*_rbj*.bin` | bones, display lists, frames (offset, speed, rotations x8), animations (pool entries frame \| flags<<12) |
| `_sets.bin` | `model/emXX_sets.bin` | u8 count, u16 first animation per set |
| RBS1 | `sound/*.bin` | sound banks |
| RMU1 | `music/*.bin` | music (format 1 = 8-bit) |
| RMV3 (RMV1/RMV2) | `movie/*.bin` | movies: key frames + tile deltas, 8-bit audio (older: whole frames) |
| RIT1 | `menu/items.bin` | icons, palette, portrait, item names |
| RMP1 | `menu/maps.bin` | 20 map pages: table, 137 pieces (16 bytes), LZ77 images |
| RSV1 | `re2ds.sav` | save slots |
| RVC1 | `voice/*.bin` | IMA-ADPCM voice clips |
| text | `subs/subs.txt` | subtitles from `tools/subs2ds.py` (whisper.cpp over the voices and movies; optional) |
| `room/roomSRR0_rbj.bin`, `_rbj_eN.bin`, `_rbj_p.bin` | | the room's animations for Leon, enemy slot N, the partner |

Coordinates: PS1 units; y negative is up; a floor level is 1800 (`LEVEL_HEIGHT`). Directions:
RE2 units 4096 = 360 deg; the DS engine uses 32768 (RE2 * 8). Facing vector = (cos, -sin).

---
## 6. Converters and the build pipeline (tools/)

### 6.1 Overview

All converters are Python 3 stdlib only (run with `python3 -I`), plus two small C tools:
`tools/bss2ppm.c` (PS1 MDEC background decoder, needs FFmpeg libavcodec/libavutil/libswscale)
and `tools/lz77.c` (LZ77 compressor compatible with the DS BIOS). `build_assets.sh` builds
both on first use.

```
tools/build_assets.sh <extracted disc dir> [ROOM1000 ROOM1010 ...]
```

- Without room arguments, every `PL0/RDT/ROOM****.RDT` is converted (Leon A = 125 rooms).
- Rooms are converted in parallel with `xargs -P <cpus>` by `tools/convert_room.sh`.
- Output goes to `build/ds/...`, then everything is copied to `nitrofiles/` (which `rm -rf`s
  `nitrofiles/` first!). `nitrofiles/` and `build/` must never be committed.
- The disc is extracted with `tools/extract_psx_iso.py` (writes movies/XA as `.raw2352` raw
  sectors which `str2ds.py`/`xa2ds.py` need).

### 6.2 Per-room steps (`convert_room.sh ROOMsrr0`)

| Step | Tool | Output |
|---|---|---|
| decode BSS backgrounds (MDEC) | `bss2ppm` | `build/bg/roomsrr_camNN.ppm` |
| room data to JSON | `rdt2json.py` | cameras, switches, collision, doors, scripts, messages (keeps `\f` page and `\v` question codes) |
| foreground masks | `bssmask.py` + `mask2ds.py` | `mask/roomsrr_*.bin` (RMK1) |
| backgrounds | `bg2ds.py` | `bg/roomsrr_camNN.bin` (RBG1, LZ77) |
| room binary | `room2ds.py` | `room/roomsrr0.bin` (spawn camera argument: 2 for ROOM1000) |
| room sound bank | `vab2ds.py --rdt` | `sound/roomsrr0.bin` |
| room animations (RBJ) | `rbj2ds.py` | `room/roomsrr0_rbj.bin` (Leon), `_rbj_eN.bin` (enemy slots), `_rbj_p.bin` (partner, RBJ mask bit 1) |
| path zones | `blk2ds.py` | `room/roomsrr0_blk.bin` |
| object models | `obj2ds.py` | `room/roomsrr0_obj.bin` |
| room effects (ESP) | `resp2ds.py` | `room/roomsrr0_esp.bin` |

### 6.3 Global steps (`build_assets.sh`)

| Data | Tool | Source on disc | Output |
|---|---|---|---|
| Leon model | `pld2json.py` + `model2ds.py` | PL0/PLD/PL00.PLD (+ W00) | `model/leon.bin`, `_tex.bin` |
| Leon per weapon | `plw2ds.py` | PL00Wnn.PLW | `model/leon_wNN.bin` |
| enemies/characters | `em2ds.py` | SLES_009.72 + CDEMD0.EMS | `model/emXX.bin`, `_tex`, `_sets` |
| doors | `vab2ds.py`, `do2ds.py` | COMMON/DOOR/DOORnn.DO2 | `sound/doorNN.bin`, `door/doorNN_*` |
| enemy sounds | `enemse2ds.py` | COMMON/SOUND/ENEMSE.VBS | `sound/enemseNN.bin` |
| core sounds/voice | `vab2ds.py --vb` | CORE00.EDH/VB | `sound/core00.bin` |
| weapon sounds | `vab2ds.py` | ARMSnn.EDH/VB | `sound/armsNN.bin` |
| music | `bgm2ds.py` (+ `--table`) | MAINxx.BGM, INIT_TBL.DAT | `music/*.bin`, `bgm_table.bin` |
| inventory | `menu2ds.py`, `itps2ds.py` | ST0.TIM, ITEMALL.PIX, ITPS.ITP | `menu/items.bin`, `pictures.bin` |
| effects | `esp2ds.py`, `resp2ds.py --core` | CORE00.ESP, CORE01.TIM, TEX_PAL.TIM | `effect/core.bin`, `core_fx.bin` |
| title screen | `title2ds.py` | TIT_BG.TIM, TMOJI.TIM | `title/*` |
| map | `map2ds.py` | SLES_009.72 + MAPS.PIX | `menu/maps.bin` (RMP1) |
| ROM icon | `icon2ds.py` | TIT_BG.TIM (eye) + TMOJI.TIM ("2") | `build/icon.bmp` (32x32, 16 colours) |
| documents | `files2ds.py` | FILES.TIM | `files/*` |
| YOU DIED | `die2ds.py` | DIED00.DIE | `die/died00.bin` |
| voices | `xa2ds.py` | STAGEn.BIN + VOICE0n.XAS.raw2352 | `voice/*` |
| movies | `str2ds.py --fps` | *.STR.raw2352 | `movie/*.bin` (15 fps opening, 10 fps in-game, to fit 512 MB) |

The `Makefile` picks up `build/icon.bmp` as `GAME_ICON` if it exists (path built from the
Makefile's own directory: `_ICON := $(dir $(abspath $(firstword $(MAKEFILE_LIST))))build/icon.bmp`),
otherwise the libnds default icon is used. The icon is disc-derived -> never committed.

### 6.4 Debug/preview tools (not in the pipeline)

`scd_disasm.py` (room script disassembler — the most useful one), `room_map.py` (top-down
collision map), `check_projection.py`, `ds_preview.py` (renders a DS frame from converted
files), `preview_model.py`, `preview_anims.py` (contact sheet of animations),
`door_preview.py`, `reachability.py` (collision rule check from every door), `tim2png.py`.

Typical:
```
python3 -I tools/scd_disasm.py <disc>/PL0/RDT/ROOM1010.RDT | less
```

### 6.5 Building the ROM

```
make DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
```
Result `re2ds.nds` (~ 450-500 MB with all movies). The Makefile rebuilds the ROM when
`nitrofiles/` changes. For melonDS on the Mac: `start.command` syncs the SD folder
`~/Documents/re2ds-sd` (DLDI SD image, saves) and starts melonDS. VS Code: the user has local
`.vscode/c_cpp_properties.json` and `tasks.json` (excluded via `.git/info/exclude`, NOT
committed); `code` CLI was installed via the VS Code command palette.

---

## 7. Testing

The user tests on real hardware (R4 flashcart, DSi XL) and in melonDS. Claude cannot see the
DS screen; only host tests. **Host tests run the real engine C files with stubbed graphics.**

### 7.1 Host tests (`tests/`)

| Test | Build | Run / purpose |
|---|---|---|
| `script_host.c` | `cc -O1 -g -fsanitize=undefined -Isource tests/script_host.c -o build/script_host` | `build/script_host $(ls nitrofiles/room/room????.bin)` (not `room*0.bin`: that also matches the `_rbj_e0.bin` files, counted as "bad header") — runs every room's scripts, reports unknown opcodes, hangs, out-of-range access. `SHOW_SPAWNS=1` prints sce_em_set, `AOTS=1` every AOT (type, data bytes), `ITEMS=1` the item AOTs. |
| `enemy_host.c` | `cc -O1 -g -fsanitize=undefined -Itests/stubs -Isource tests/enemy_host.c source/script.c source/enemy.c source/plc.c -o build/enemy_host -lm` | one room's enemies vs a fake Leon. Env: `TICKS` (default 100), `PX`/`PZ`/`PY` (Leon position), `NOISE` (from tick `NOISE_AT` on), `SHOOT` (Leon shoots every 15 ticks), `MASH` (button mashing when grabbed), `TRACE` (per-tick enemy dump with y and surface, sounds, blood), `ZONES=nitrofiles/room/room1020_blk.bin`, `WALLS` (use room collision; the lickers' wall crawl reads the boxes always), `EVENT=n` (start event function n at tick 5, e.g. `EVENT=3` lets the arms of room 205 out), `MEMBERS=tick:slot:member:value,...`, `LOOK=<Leon's facing>` (what his head turns to, every 45 ticks), `SPAWN=slot:type:flags:x:y:z:dir,...` (an enemy the script only sets behind flags, e.g. room 10A's wall licker `0:0x22:0x0101:-27000:-3000:-12400:1024`). |
| `cutscene_host.c` | `cc -Itests/stubs -Isource tests/cutscene_host.c source/script.c source/enemy.c source/plc.c -o build/cutscene_host` | `./build/cutscene_host nitro:/room/room3010.bin <event> [ticks]` — runs a scene (plc, messages, cuts, voices, spawns); ends when no thread runs. Loads `_rbj_eN.bin` and `_rbj_p.bin`. `TRACE_MEMBERS=1` prints the script's enemy member sets (e.g. when room 201's event 4 signals its licker). |
| `door_host.c` | `cc -Itests/stubs -Isource tests/door_host.c source/script.c -o build/door_host` | `build/door_host 0` — door program positions; `DOOR_DUMP=30,120` dumps frames. **Stale:** misses many `game_*` stubs and does not link. |
| `music_host.c` | `cc -O1 -g -fsanitize=undefined -Itests/stubs -Isource tests/music_host.c -o build/music_host -lm` (`-DMAX_VOICES=n` to try other limits) | `./build/music_host <stage> <room hex> [seconds] [frame:id:op,...] [slow]` — plays a room's music: voices busy, notes that took a sounding voice (held / fading), DS channels missing (one-shots free theirs at their end), sound messages per frame, summed channel volume (peak/average; the DS clips past about 127, the music's limit is 160). |

`tests/stubs/` holds a fake `nds.h` etc. **When adding a new `game_*` hook called from
script.c/enemy.c, add a stub to all four host tests** (this session added
`game_player_hurt`, `game_player_killed`, `game_aot_on`, `game_busy`).

### 7.2 Sanitizers on macOS

- ASAN-built host binaries hang on this macOS (known issue) -> use `-fsanitize=undefined` only.
- `timeout` is not installed. Use perl alarm:
  ```
  perl -e 'alarm 60; exec @ARGV' build/script_host nitrofiles/room/room*0.bin
  ```
- Sweep that was clean at the end of this session: script_host over all 125 rooms with UBSan;
  enemy_host for 1020 (zombies), dog rooms, crow room, licker rooms, 301 partner scene.

### 7.3 Compile check of the DS build

`make ...` must build with no warnings (the Makefile uses `-Wall`). Unused static functions
produce warnings -> remove them.

### 7.4 What only the user can test

Everything visual and timing-related: animations, camera, map look, message layout, the
terminal flow, sounds, the icon on the R4 menu. After a test round the user reports in German
with short sentences / screenshots; wait for "klappt"/"passt" before committing.

---

## 8. Reverse-engineering workflow

### 8.1 Files

- `SLES_009.72` (PS1 executable, loaded at 0x80010000, file offset = addr - 0x80010000 +
  0x800). Enemy AI is NOT in it: it lives in overlays in `PL0/PLD/CDEMD0.EMS`.
- `tools/research/psxexe_to_elf.py` wraps the EXE (+ overlays) into an ELF for Ghidra/REA.
- `tools/research/overlay_symbols.py SLES CDEMD0.EMS <type hex> <slot> <prefix>` prints the
  `--overlay FILE:OFFSET:SIZE:ADDR` argument (stderr) and the overlay's function-table symbols
  (stdout). Slot 0 code at 0x80100000, slot 1 at 0x8010d000. Loader table 0x8009328c,
  index `((type - 0x10) * 4 + 1) - slot`.
- Symbol files: `re2_symbols.txt` (main EXE names found so far), `re2_symbols_zombie.txt`,
  `re2_symbols_all.txt`. A symbol line `0xADDR name` makes Ghidra create a function there —
  this is how functions only reached through tables are found (e.g. 0x8004edb0, the terminal
  AOT handler).

Example (dog = type 0x20, slot 0):
```
cd tools/research
python3 -I overlay_symbols.py $DISC/SLES_009.72 $DISC/PL0/PLD/CDEMD0.EMS 20 0 dog > /tmp/dog.txt 2> /tmp/dog.arg
cat re2_symbols.txt /tmp/dog.txt > /tmp/sym.txt
python3 -I psxexe_to_elf.py $DISC/SLES_009.72 /tmp/re2_dog.elf /tmp/sym.txt $(cat /tmp/dog.arg)
```
Then open in REA (`mcp__rea__open_binary`), `mcp__rea__batch_decompile` the table entries,
`procedure_pseudo_code` for single functions.

When REA times out, Ghidra headless does the same (`support/analyzeHeadless <proj dir> <proj>
-import x.elf -processor MIPS:LE:32:default -preScript NoRet.java`, then `-process x.elf
-noanalysis -readOnly -postScript <a script that decompiles given addresses>`). Two traps:
without `-processor` Ghidra picks MIPS 64 R6 for these ELFs and every function decompiles to
`halt_baddata`; and its non-returning-function discovery marks `rand` (0x80015988) and
others as not returning, which cuts functions short ("Subroutine does not return"), so the
pre-script turns the option "Non-Returning Functions - Discovered" off.

### 8.2 Important PS1 addresses (main EXE)

| Address | Meaning |
|---|---|
| 0x800d... `obj` layout | enemy/player object; +0x14c anim select (0x0F0000 + key<<8 + anim), +0x144 speed, +0x156 HP |
| 0x80014c68 | move object by its speed along its direction |
| 0x80014fb4 | turn delta to a target (our `turn_delta`) |
| 0x800150f8 | cone check: returns 0 when INSIDE the cone |
| 0x80028a4c | animation blending (7 ticks) |
| 0x8002bc00 / 0x80033f70 | object culling by camera view area |
| 0x8003cf38 | player damage (x1.5 under 41, survive-once rule) |
| 0x8004edb0 | AOT type 14 handler (terminal) -> mode 7 -> 0x80036a90 |
| 0x80037c6c | terminal_door_lock_service |
| 0x800384dc | terminal_rodem_fingerprint (not ported) |
| 0x80069cbc / 0x80069fb4 | map page for a room / piece for a room |
| 0x800695ec / 0x8006a4a4 | map drawing / map screen |
| 0x8009328c | enemy code loader table |
| 0x8009c470 | weapon damage table (zombies) |
| 0x8009eed8 | weapon lower-key table |
| 0x8009f490 | AOT handler table (by AOT type) |
| 0x8009f94c | flag array pointer table |
| 0x800a13e0 | map image table |
| 0x800a2a08 | map page table |
| 0x800a0118 | voice/core file table per character |

Flag arrays (pointer table 0x8009f94c, PS1 bit order `0x80000000 >> (bit & 31)`):
array 4 = 0x800cc450 (story), 9 = 0x800cc508 (map visited), 11 = 0x800de0c4 (message answer,
bit 0x1F set = NO), 32 = 0x800cc51c (cleared), 33 = 0x800cc520 (map found).

### 8.3 Message codes (room message text)

`0xFC` newline, `0xFD arg` new page (we write `\f`), `0xFB arg` yes/no question follows (we write
`\v`), `0xFA arg` start code. Converter `rdt2json.py` keeps `\f` and `\v`; `menu.c` pages them.

### 8.4 Rules for porting

- Port behaviour, never copy data blobs into the repo; tables of numbers (speeds, damage,
  animation numbers) are fine in C with the PS1 address in a comment.
- Write a doc section (docs/enemy-*.md, engine-notes.md) for each ported routine: address,
  what it does, what we simplified.
- Mark guesses as such in comments ("not verified").

---

## 9. Known issues / untested state (end of this session)

Everything from PR #20 (develop) is **not yet tested by the user on the DS**, except: dog
sounds (fixed after report), map look (iterated), 301 Ben scene no freeze (fix done; user to
confirm), icon (made from disc).

Specific risks:
1. Dog / crow / licker / spider / Mr X AIs are first versions: speeds and distances from the
   PS1 code but state transitions simplified; may look jumpy. Mr X has no scripted
   appearances logic beyond sce_em_set; his knockdown/stagger missing.
2. SMG / flamethrower continuous fire: flamethrower has no flame effect sprite yet (only sound
   and damage); SMG casing rate maybe too high.
3. Map: unvisited rooms are always drawn dimmed (the PS1 hides them until the map is found;
   flag 33 mechanism not understood). Page flipping by touch only.
4. Terminal: only the door-lock service program; fingerprint program says "does not respond".
5. Message paging: question boxes use 3 text lines + YES/NO; long questions may be cut.
6. Partner (Ada/Sherry/Ben in slot 0xff) only follows plc; no free AI following Leon (Ada
   walking with Leon in the B1 parking area is not done).
7. Alligator (0x23, room 40A0): ported (course, cylinder, explosion -> flag 5:0x1e); its camera
   cuts and the exact bite are not.
8. G (Birkin 0x30-0x36), cockroaches (0x29): placeholder AI; Ivy's vines and damage values are approximations.
   Moth spores, moth modes 2/6/7/9/12 and the larva spawners (room 614) are not ported.
9. Lickers and spiders on walls and ceilings are ported (`docs/enemy-others.md` sections 4
   and 5) but not tested on the DS yet; the spiders' poison hits at once instead of flying.
10. Grenade launcher, bowgun, rocket launcher not in `weapons[]`.
11. Options menu, ending, ranking/results screen, Claire/B scenario: none.
12. Ada model looks odd — confirmed identical to disc data; no change needed.

---

## 10. Next steps (suggested order)

1. Wait for the user's test report of PR #20 content; fix what they find.
2. Release: PR develop -> main, then delete develop (remote + local) and recreate from main.
3. Alligator scene (room 40A0): RE the overlay of type 0x23, port the scripted sequence
   (gas tank shot), flags 5:0a/0b.
4. Partner following AI (Ada in B1/sewers): PS1 partner routine in the type 0x41 overlay.
5. G-type enemies and plants/moths (stage 4-6): overlays, then AIs like the dog's.
6. Remaining weapons (grenade launcher with ammo kinds, bowgun, rocket launcher) — PLW sets
   exist (`leon_wNN.bin`); effects from CORE00.ESP.
7. Map found flag (array 33) to hide unknown rooms; fingerprint terminal.
8. Ending + ranking; options (sound volume, controls).
9. Performance on real DS (polygon budget in rooms with many enemies; VERTEX_LIMIT).

---
## Appendix A. Functions per source file (generated)

Non-static and static function definitions, `file:line name`. Use as an index.

### source/assets.c (     112 lines)

```
9   asset_load    28  asset_free  33  room_load
60  room_message  68  model_load  85  masks_load
```

### source/door.c (     483 lines)

```
65   setup_camera      78   rd16          79   ru16
80   var               82   func_start    88   start_thread
100  base              102  push          108  unwind
118  condition_failed  140  compare       153  calc
168  work_part         174  member_store  186  member_load
197  run_thread        378  door_begin    419  apply_part
429  render            450  door_frame    467  door_end
```

### source/effect.c (     211 lines)

```
45   effect_load   70   effect_clear  75   find_anim
83   effect_spawn  113  effect_blood  119  effect_tick
149  effect_count  157  effect_draw
```

### source/enemy.c (    3236 lines)

```
88    game_player_voice    89    game_player_damage  92    game_player_died
93    game_player_hurt     94    game_player_killed  206   is_person
219   anim_model           221   enemy_set_room_set  233   free_type
244   enemy_room_reset     259   model_gx_cost       289   load_type
364   rand                 366   set_anim            378   enter_state
409   enemy_spawn          487   enemy_sound_set     507   sound_at
512   moan                 525   enemy_member_get    550   enemy_member_set
576   enemy_count          585   present             588   crowded
605   enemy_blocks_player  619   frame_speed         625   angle_to
632   turn_toward          651   in_cone             660   anim_step
673   walk                 690   lurk                736   can_grab
746   is_zombie            747   strike              767   turned_offset
776   grab_place           783   start_grab          810   grab_player
874   grab_zombie          946   chase               1019  lunge
1040  lying                1068  start_script_mode   1080  script_mode
1169  is_dog               1171  set_anim            1175  dog_run_anim
1182  dog_speed_up         1188  dog_slow_down       1195  turn_delta
1203  move_dir             1219  dog_fall            1231  dog_enter
1237  dog_spawn            1249  abs                 1254  dog_idle
1304  dog_circle           1337  dog_can_attack      1351  dog_hunt
1414  dog_bites            1435  dog_attack          1521  dog_shot
1547  dog_thrown           1598  dog_step_sound      1608  dog_tick
1651  is_crow              1653  set_anim            1655  crow_spawn
1663  crow_release         1671  crow_move           1678  crow_tick
1884  crow_shot            1938  is_licker           1939  lick_kind
1940  set_anim             1942  licker_spawn        1954  licker_hears
1963  licker_hits          1978  licker_step         1986  licker_tick
2161  licker_shot          2204  is_spider           2205  is_spiderling
2207  spider_spawn         2221  spider_bites        2232  spiderling_tick
2252  spider_tick          2419  spider_shot         2464  is_tyrant
2466  tyrant_spawn         2474  tyrant_tick         2544  update_pose
2585  enemy_body           2599  enemy_body_back     2610  enemy_plc
2628  plc_step             2644  enemy_set_zones     2651  zone_at
2669  portal               2689  idist8              2701  path_goal
2778  update_goal          2796  enemy_tick          2882  enemy_grabbing
2884  present              2889  on_ground           2894  enemy_aim_target
2926  fall                 2941  push_tick           2953  blood_burst
2964  enemy_hit            3096  fallen              3124  draw_bone
3155  enemy_draw           3227  enemy_debug_print
```

### source/main.c (    3551 lines)

```
179   health_level            225   texture_write         247   reserve_mask_texture
267   fatal                   275   upload_leon_textures  295   show_camera_view
341   set_camera              348   free_objects          361   texture_size
363   rd32                    365   load_objects          389   object_entry
391   upload_object_texture   416   is_item_model         425   upload_objects
435   game_object_set         450   roomfx_on             451   roomfx_kill
453   game_floor_sound_set    461   game_object_hide      467   game_object_member
480   game_object_member_set  492   in_view_area          508   draw_objects
537   load_room               599   load_room_index       608   build_hierarchy
627   setup_camera            665   view_camera           670   camera_sees
702   setup_projection        719   update_pose           746   draw_bone
772   draw_player             785   draw_masks            823   stores_test_triangle
841   calibrate_culling       855   die_target            864   die_camera_tick
886   die_orbit_start         903   die_show_picture      921   die_picture_level
933   die_tick                1020  model_colour          1035  render_frame
1062  fade                    1075  point_in_quad         1091  player_floor
1097  is_slope                1107  penetration           1135  blocked_for
1149  blocked                 1158  update_height         1189  camera_has_zones
1199  check_camera_switch     1237  set_anim              1246  set_special_anim
1253  place_player            1277  terminal_start        1288  game_busy
1291  terminal_step           1335  game_show_message     1341  menu_message_open
1343  game_cut_change         1349  game_set_player_pos   1357  game_set_player_dir
1359  game_voice              1364  voice_playing         1366  game_player_member
1377  game_player_member_set  1389  player_body           1398  player_body_back
1408  game_plc                1428  script_move_tick      1440  game_spawn_enemy
1448  game_enemy_sound        1456  game_player_voice     1463  game_player_damage
1477  game_player_grabbed     1488  game_player_pose      1509  game_player_hurt
1528  game_player_killed      1539  hurt_tick             1555  game_player_died
1560  game_bgm_control        1575  game_movie            1587  game_bgmtbl_set
1593  game_menu_sound         1598  game_blood            1603  game_enemy_member
1608  game_enemy_member_set   1614  segment_hits_box      1645  sight_box_hit_flags
1660  sight_box_hit           1665  game_sight_blocked    1690  stands_in
1696  tall_wall               1703  update_occlusion      1716  camera_occluded
1735  game_blocked            1744  game_play_sound       1750  game_set_box_flag
1778  sd_rom_read             1807  sd_rom_close          1819  nitro_init
1861  pick_enemy_bank         1877  enter_room            1890  new_game
1920  choice_frame            1933  file_pages            1941  show_file_page
1959  show_file               2018  map_page              2077  map_piece
2121  map_mark_room           2132  map_colour            2147  map_palette
2178  map_load                2201  map_draw              2232  map_live
2269  open_files              2287  any_save              2298  pick_slot
2320  use_typewriter          2379  load_game             2412  show_title
2432  title_screen            2474  start_game            2493  warp_to
2506  use_door                2567  trigger               2625  game_aot_on
2633  check_aots              2664  move_forward          2732  is_auto
2738  weapon_in_hand          2751  apply_weapon_texture  2775  update_weapon_model
2803  leon_point              2843  throw_casing          2885  shot_effects
2904  is_auto                 2906  start_reload          2914  reload_tick
2941  fire                    2969  auto_fire_tick        3037  update_aim
3087  update_player_frame     3118  floor_sound           3131  step_forward
3139  special_root_motion     3156  special_move_tick     3208  box_in_front
3224  try_special_move        3252  update_player_tick    3328  aot_name
3339  print_status            3366  update_menu           3408  main
```

### source/menu.c (    1036 lines)

```
58   col                60   fill                  68   frame
76   text               92   text_end              96   text_wrapped
120  text_box           127  icon                  138  item_name
145  is_weapon          147  slot_pos              153  draw_slot
172  draw_commands      186  ecg_wave              209  draw_condition
239  draw_portrait      248  draw_weapon           266  draw_picture
280  redraw             297  show_screen           310  pause_init
353  pause_show         364  menu_init             393  menu_debug
406  menu_debug_on      408  menu_message          418  draw_message
442  menu_message_open  448  draw_choice           462  menu_choose
517  shows_count        519  box_inv_pos           525  draw_item_box
576  menu_item_box      683  menu_overlay          693  menu_overlay_end
700  menu_picture       706  menu_focus            713  menu_has_focus
718  menu_lower_map     720  menu_lower_toggle     722  set_info
750  spare_ammo         767  inventory_spare_ammo  774  inventory_reload
791  combine            834  command               869  combine_with
876  touched_slot       887  menu_frame
```

### source/movie.c (     172 lines)

```
40   clock_tick  46  clock_start   52  clock_ticks
54   clock_stop  56  clear_screen  62  movie_play
166  movie_end
```

### source/music.c (     430 lines)

```
56   set_tempo        64   varlen             76   voice_stop
83   music_duck       85   music_stop         95   music_playing
97   music_play       138  note_rate          145  note_off
154  play_tone        191  note_on            198  bend
213  event            280  music_frame        316  table_load
323  table_pairs      332  music_table_reset  339  music_table_get
349  music_table_put  358  music_table_set    370  music_room
400  music_control
```

### source/plc.c (     271 lines)

```
31   rd16         33   table      36   angle_to
41   angle_diff   44   turn_to    51   turn_toward
56   start_anim   66   loop_anim  76   forward
82   plc_command  144  play       168  facing
173  arrived      185  plc_tick
```

### source/roomfx.c (     417 lines)

```
64   parse         93   roomfx_load   142  upload
184  find          195  rd16          197  roomfx_spawn
233  roomfx_throw  264  roomfx_on     272  roomfx_kill
279  roomfx_tick   338  roomfx_count  347  roomfx_draw
```

### source/save.c (      95 lines)

```
28  save_init   54  save_available  56  save_read
85  save_write
```

### source/script.c (     819 lines)

```
95   rd16                96   ru16                98   function_start
107  flag_get            114  flag_set            124  cut_replace
125  cut_auto_enabled    127  inventory_has       136  stacks
138  inventory_add       156  script_state_get    162  script_state_set
168  script_state_reset  174  inventory_reset     181  aot_rect
190  aot_quad            199  aot_head            212  op_aot
252  op_aot_reset        265  work_member         276  work_member_set
287  start_thread        307  script_start_event  313  call_base
318  push_block          333  unwind              350  condition_failed
374  compare             387  calc                405  var
408  run_thread          753  script_tick         764  script_camera_changed
770  script_room_enter
```

### source/sound.c (     105 lines)

```
9    sound_init   14  sound_bank_load  28  sound_bank_free
33   find         45  sound_play       55  sound_frames
67   voice_stop   77  voice_play       99  voice_playing
101  voice_frame
```

## Appendix B. Named PS1 functions (tools/research/re2_symbols.txt)

```
# Startup / main loop
0x8002a338 main
0x8003673c pad_read_and_map
0x800310d0 task_scheduler
0x800396e4 player_control_dispatch
# Player control handlers (table 0x8009c398, indexed by obj+5)
0x80039760 pl_ctrl_00_stand
0x800399a4 pl_ctrl_01_walk
0x80039e10 pl_ctrl_02_run
0x8003a324 pl_ctrl_03_back
0x8003abf8 pl_ctrl_04_turn
0x8003f790 pl_ctrl_05_aim
0x8003afdc pl_ctrl_06
0x8003b11c pl_ctrl_07
0x8003b6d4 pl_ctrl_08
0x8003ba68 pl_ctrl_09
0x8003bee4 pl_ctrl_0a
0x8003c1d0 pl_ctrl_0b
# Player motion handlers (table 0x8009c3c8)
0x800398e8 pl_move_00_stand
0x80039bc8 pl_move_01_walk
0x8003a02c pl_move_02_run
0x8003a4a4 pl_move_03_back
0x8003adfc pl_move_04_turn
0x80040800 pl_move_05_aim
0x8003afe4 pl_move_06
0x8003b124 pl_move_07
0x8003b6e4 pl_move_08
0x8003ba78 pl_move_09
0x8003bf54 pl_move_0a
0x8003c1d8 pl_move_0b
# Terminals
0x80037c6c terminal_door_lock_service
0x800384dc terminal_rodem_fingerprint

# SCD opcode handlers (table 0x8009f560, used by room and door scripts)
0x8004fd7c scd_op_00
0x8004fd94 scd_op_01
0x8004fdf8 scd_op_02
0x8004fe10 scd_op_03
0x8004fe3c scd_op_04
0x8004fe74 scd_op_05
0x8004febc scd_op_06
0x8004fefc scd_op_07
0x8004ff38 scd_op_08
0x8004ff74 scd_op_09
0x8004ffbc scd_op_0a
0x80050024 scd_op_0b
0x80050058 scd_op_0c
0x800500b4 scd_op_0d
0x80050254 scd_op_0e
0x800502d4 scd_op_0f
0x800503a4 scd_op_10
0x800503e8 scd_op_11
0x8005045c scd_op_12
0x800505b8 scd_op_13
0x80050690 scd_op_14
0x800506a8 scd_op_15
0x800506c0 scd_op_16
0x800506f4 scd_op_17
0x80050740 scd_op_18
0x800507a8 scd_op_19
0x80050800 scd_op_1a
0x8005016c scd_op_1b
0x8004fd64 scd_op_1c
0x8005084c scd_op_1d
0x800508ec scd_op_21
0x8005094c scd_op_22
0x80050a0c scd_op_23
0x80050ab4 scd_op_24
0x80050ae4 scd_op_25
0x80050b24 scd_op_26
0x80050b70 scd_op_27
0x80050cf8 scd_op_28
0x80050d24 scd_op_29
0x80050db4 scd_op_2a
0x80051024 scd_op_2b
0x8005108c scd_op_2c
0x800515dc scd_op_2d
0x80051c80 scd_op_2e
0x80051e00 scd_op_2f
0x80051e2c scd_op_30
0x80051ea8 scd_op_31
0x80051f1c scd_op_32
0x80051f4c scd_op_33
0x80051f7c scd_op_34
0x80051fcc scd_op_35
0x800527a4 scd_op_36
0x800528d8 scd_op_37
0x800549c4 scd_op_38
0x80052764 scd_op_39
0x80052920 scd_op_3a
0x8005117c scd_op_3b
0x80050e60 scd_op_3c
0x800521b4 scd_op_3d
0x80052208 scd_op_3e
0x80052f08 scd_op_3f
0x80052f54 scd_op_40
0x800531d8 scd_op_41
0x80053318 scd_op_42
0x800533b4 scd_op_43
0x800534c8 scd_op_44
0x80054dc0 scd_op_45
0x800514d0 scd_op_46
0x80051544 scd_op_47
0x80051888 scd_op_48
0x80051c2c scd_op_49
0x8005313c scd_op_4a
0x80050ec8 scd_op_4b
0x80052c00 scd_op_4c
0x800146bc scd_op_4d
0x8005126c scd_op_4e
0x80053a90 scd_op_4f
0x80053acc scd_op_50
0x80053b08 scd_op_51
0x80052d50 scd_op_52
0x80053bd8 scd_op_53
0x80052a6c scd_op_54
0x80052504 scd_op_55
0x80052570 scd_op_56
0x80053b80 scd_op_57
0x80053438 scd_op_58
0x80053d08 scd_op_59
0x800543e4 scd_op_5a
0x8005349c scd_op_5b
0x80053e44 scd_op_5c
0x80053fd0 scd_op_5d
0x800541d0 scd_op_5e
0x80053d70 scd_op_5f
0x80054578 scd_op_60
0x80050fac scd_op_61
0x800542c4 scd_op_62
0x80053174 scd_op_63
0x800529c0 scd_op_64
0x80052c64 scd_op_65
0x8005336c scd_op_66
0x800510f8 scd_op_67
0x800511e8 scd_op_68
0x80051398 scd_op_69
0x800546b4 scd_op_6a
0x8005476c scd_op_6b
0x800508a8 scd_op_6c
0x80054998 scd_op_6d
0x80051d4c scd_op_6e
0x80054a44 scd_op_6f
0x80054a8c scd_op_70
0x80054b28 scd_op_71
0x800519fc scd_op_72
0x80054bd8 scd_op_73
0x80053cb8 scd_op_74
0x80052b30 scd_op_75
0x80054498 scd_op_76
0x80053ef4 scd_op_77
0x80053f40 scd_op_78
0x80053f98 scd_op_79
0x80054c58 scd_op_7a
0x80054d10 scd_op_7b
0x800547b8 scd_op_7c
0x80054820 scd_op_7d
0x800548dc scd_op_7e
0x8005492c scd_op_7f
```

## Appendix C. Enemies spawned by the init/entry scripts per room (generated)

From `SHOW_SPAWNS=1 build/script_host nitrofiles/room/room*0.bin` (type hex x count; only scripts that run without events/flags, so story spawns may be missing).

| room | types |
|---|---|
| 100 | 1fx6 |
| 101 | 48x1 1fx4 |
| 102 | 1fx4 |
| 103 | 11x1 1fx3 |
| 104 | 1fx6 |
| 105 | 10x3 |
| 106 | 12x1 13x1 |
| 107 | 10x3 |
| 109 | 21x6 |
| 10a | 10x2 |
| 110 | 10x3 |
| 113 | 1fx5 |
| 114 | 10x3 |
| 115 | 51x1 |
| 118 | 13x1 1fx2 |
| 119 | 1fx5 |
| 11a | 12x1 13x1 |
| 11b | 1fx6 |
| 11d | 1fx7 |
| 201 | 22x1 |
| 202 | 4ax1 |
| 204 | 22x1 1fx1 |
| 205 | 2dx10 |
| 207 | 13x1 1fx3 |
| 209 | 1fx6 |
| 20a | 1fx6 |
| 20b | 1fx6 |
| 20c | 13x1 1fx3 |
| 210 | 10x1 |
| 211 | 20x2 |
| 212 | 20x2 |
| 215 | 12x5 |
| 216 | 41x1 |
| 219 | 41x1 |
| 21a | 20x2 |
| 300 | 40x1 |
| 301 | 41x1 46x1 |
| 302 | 25x2 |
| 304 | 41x1 |
| 305 | 20x2 |
| 30b | 50x1 |
| 401 | 18x1 |
| 403 | 41x1 |
| 404 | 41x1 47x1 50x1 |
| 405 | 25x2 |
| 407 | 25x2 |
| 408 | 41x1 |
| 409 | 41x1 47x1 |
| 40a | 23x1 41x2 |
| 40b | 41x1 |
| 40d | 41x1 |
| 40e | 41x1 |
| 410 | 41x1 54x1 |
| 500 | 41x1 31x1 |
| 501 | 41x1 |
| 502 | 41x1 1fx5 |
| 503 | 41x1 |
| 504 | 41x1 |
| 505 | 41x1 |
| 507 | 2ax1 |
| 508 | 41x1 1fx4 |
| 509 | 31x1 |
| 603 | 16x1 |
| 605 | 24x2 |
| 607 | 43x1 |
| 608 | 3fx1 |
| 609 | 24x2 |
| 60a | 2fx6 |
| 60b | 2ex2 2fx1 |
| 60d | 3ex1 |
| 60e | 2ex1 2fx8 |
| 60f | 24x3 |
| 610 | 15x3 16x2 |
| 611 | 3fx1 |
| 613 | 3ax1 3bx6 |
| 614 | 3bx1 |
| 615 | 16x5 |
| 616 | 24x2 |
| 617 | 34x1 |
| 702 | 41x1 2bx1 |
| 703 | 4fx1 55x1 |

Type key: 0x10-0x1F zombies, 0x20 dog, 0x21 crow, 0x22 licker, 0x23 alligator, 0x24 licker (red), 0x25 spider, 0x26 small spider, 0x27 plant?, 0x28 moth?, 0x29 ?, 0x2a Mr. X, 0x2b-0x2f G forms / Ivy, 0x40+ people.

## Appendix D. Existing docs

- `docs/damage.md` (     263 lines): RE2 (PS1, SLES-00972) – hits and weapon damage
- `docs/enemy-others.md` (     292 lines): RE2 (PS1, SLES-00972) – dog, crow, licker
- `docs/enemy-zombie.md` (     258 lines): RE2 (PS1, SLES-00972) – zombie AI
- `docs/engine-notes.md` (     144 lines): RE2 (PS1, SLES-00972) – scripts, cutscenes, effects, media
- `docs/how-re2ds-works.md` (     164 lines): How re2ds works – from the disc image to the DS ROM
- `docs/player-control.md` (     311 lines): RE2 (PS1, SLES-00972) – player control
- `ROADMAP.md` (     109 lines): re2ds – roadmap to the complete game
- `README.md` (     298 lines): re2ds – Resident Evil 2 (PS1) on the Nintendo DS

End of handoff. Keep this file updated at the end of each larger session.

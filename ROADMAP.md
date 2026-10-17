# re2ds – roadmap to the complete game

Goal: Resident Evil 2 (PS1, PAL; disc 1 = SLES-00972) complete as a native DS ROM – both
characters, both scenarios, with menus, sound and movies. The ROM is always built locally from
your own discs and is **never shared** (it contains Capcom's data).

Effort per phase: S (days), M (1–3 weeks), L (1–2 months), XL (several months).

## What is on the disc (disc 1, Leon)

| Contents | Files | Size on PS1 | Notes |
|---|---|---|---|
| rooms (RDT) | 125 in 7 stages | 20 MB | cameras, collision, scripts, texts, masks, effects, objects |
| backgrounds (BSS) | 128 files, 1285 camera slots | 80 MB | 320×240 MDEC each |
| movies (STR) | 15 | 380 MB | about 10 minutes |
| voices (XA) | 8 | 41 MB | |
| music (BGM) | 108 | 13 MB | sequences + samples |
| sound effects (EDH/VB) | 28 pairs | 1.6 MB | VAB-like |
| enemies (EMS) | 2 packs | 13 MB | models, animations, AI overlays |
| door animations (DO2) | 55 | 3.4 MB | |
| menu graphics (TIM/PIX) | ~50 | 11 MB | inventory, items, maps, files |

Disc 2 (Claire) has the same structure.

## DS limits and what follows from them

- **ROM at most 512 MB.** Today about 290 MB for disc 1 with two movies; the remaining movies
  must stay small (≈ 128 KB/s or less).
- **4 MB of main memory.** Only one room, its models and the current camera are loaded;
  backgrounds and movies are streamed from NitroFS.
- **3D: at most 2048 polygons per frame.** Leon alone has 454; enemies are drawn nearest first
  within the budget left after masks and effects.
- **Texture memory 512 KB** (banks A–D): Leon 96 KB, masks 64 KB, effects, room objects and
  room effects within budgets (96 KB each), the rest for enemies.
- **Sound: 16 channels** (PS1: 24) → a sequencer with channel priorities.
- **The second screen** as an advantage: inventory, map and status on the lower screen with
  touch.

## Phases

### Phase 1 – every room walkable (L) – done
- Batch converter for all 125 rooms; 8-bit backgrounds with LZ77, loaded per camera.
- Doors with the door animations (DO2), floors, stairs and slopes, collision.
- Debug menu: warp to any room.

### Phase 2 – script system (XL) – done for disc 1
- SCD interpreter (`source/script.c`): control flow, flags, variables, threads, AOTs (doors
  with locks, items, messages, events), every opcode used on disc 1 with its size taken from
  the PS1 handlers.
- Cutscenes: `plc_*` (characters moved by the scripts), camera cuts, voices, room objects and
  their movement (`speed_set` / `add_speed`), effects (`sce_espr_on`).
- Open: `0x1b` (a loop with a variable count, not used on disc 1), head turning (`plc_neck`),
  room lighting (`0x7c`–`0x7f`).

### Phase 3 – the player (M) – mostly done
- Done: tank controls, health animations, pose blending, aiming / firing / reloading for the
  handguns and the knife, auto-aim, grab and struggle, death sequence.
- Open: the other weapons (magnum, shotgun, grenade launcher, …) and their frame handlers,
  pushing objects, the death scenes of other enemies.

### Phase 4 – enemies (XL) – zombies done
- Done: zombies with the original AI, path zones, hits, falling, dying, eating, people moved
  by scripts.
- Open: dog, crow and licker (analysis in [docs/enemy-others.md](docs/enemy-others.md)),
  spider, moth, plant, alligator, G (all stages), Mr. X; head shots and severed parts as in
  [docs/damage.md](docs/damage.md).

### Phase 5 – menus and saving (L) – started
- Done: inventory on the lower screen, equip / check (with the item pictures) / combine;
  saving at the typewriters with ink ribbons (12 slots on the SD card), loading at the start
  and after dying; the item box (64 slots, shared by all save rooms, kept in the save).
- Open: map, files, title screen, options, ending and ranking.

### Phase 6 – sound (L) – done for disc 1
- Room sound effects, enemy voices, footsteps, weapon sounds, music sequencer, voice clips.
- Open: the remaining one-off sounds (e.g. the PS1's death sound effect).

### Phase 7 – movies (L) – started
- Done: player and converter (`tools/str2ds.py`, `source/movie.c`); the opening (two movies).
- Open: the in-game movies (`ZMOVIE`, `PL0/ZMOVIE`) at their trigger points, the ending.

### Phase 8 – all scenarios (M)
- Disc 2 (Claire) through the same pipeline.
- Scenarios A/B, the zapping system.
- Optional: 4th Survivor, Tofu, extra costumes.

### Phase 9 – polish (M)
- Performance and loading times, bugs, real hardware (flashcart), the ripple effect of the
  YOU DIED picture, the glass shards' exact physics.

## Biggest risks

1. **Polygon budget** with several enemies on screen – measure early in phase 4.
2. **Movies** – ARM9 time (67 MHz) and ROM size.
3. **Script edge cases** – every wrong opcode can block progress; the host tests run every
   room's scripts.

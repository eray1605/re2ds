# RE2 (PS1, SLES-00972) – zombie AI

Source: `SLES_009.72` + the zombie code from `PL0/PLD/CDEMD0.EMS` (disc 1, Europe),
analysed with REA + Ghidra 12.1.4. Addresses are PS1 RAM addresses, zombie code in slot 0
(`0x80100000`). Legend as in [player-control.md](player-control.md):
**observed** = in the code as written, **assumed** = interpretation.

---

## 1. Where the enemy code lives

The enemy AI is **not** in the executable; it is loaded from the disc per room
(observed, `FUN_8001b284`):

- Two code slots of `0xD000` bytes: **slot 0 = `0x80100000`**, slot 1 = `0x8010D000`.
  Slot 0 overwrites the 2 KB placeholder at the end of the EXE.
- All zombie types `0x10`–`0x1F` share **one** code block.
- The block is in `PL0/PLD/CDEMD0.EMS` twice, linked once per slot:
  - file offset `0x0000`, 52,592 bytes → linked for slot 1 (`0x8010D000`)
  - file offset `0xD000`, 52,592 bytes → linked for slot 0 (`0x80100000`)
- The load table in the executable is at `0x8009328c` (sector offset, size).

The gameplay loop (`0x80025058`) calls `type_table_0x800c7a58[obj->type](obj)` per frame for
every active object and computes the **distance to the player** before
(`obj+0x1f0 = sqrt(dx² + dz²)`, player position `0x800c782c`/`0x800c7834`).

**DS port:** no code loading needed; the zombie AI is plain C in the DS engine
(`source/enemy.c`).

---

## 2. Structure

Main table at the end of the zombie code (`0x8010c654`, indexed with routine 0 = `obj+4`):

| Routine 0 | Address      | Meaning                                    |
|-----------|--------------|--------------------------------------------|
| 0         | `0x80100674` | initialisation (once)                      |
| 1         | `0x801010b4` | **AI per frame**                           |
| 2         | `0x80104e80` | taking a hit (assumed)                     |
| 3         | `0x80108190` | death (assumed)                            |
| 4         | `0x80061488` | shared routine in the executable (also the player's): moved by the room script (`plc_*`, see [engine-notes.md](engine-notes.md)) |
| 8         | `0x80109c3c` | the room script's zombie modes (modes table `0x8010cd3c`) |

The AI routine first dispatches on the kind (`obj+0x10e & 0x3f`, table `0x8010c678`):
even values → `0x801010f4` (standing zombie), odd → `0x80101178` (crawler).
Then, as for the player, every state (`obj+5`) has a **decision** and a **movement**
routine:

| State | Decision     | Movement     | Meaning (assumed unless marked)              |
|-------|--------------|--------------|----------------------------------------------|
| 0     | `0x801011fc` | `0x8010135c` | **lurking / standing** (observed)            |
| 1     | `0x8010167c` | `0x801019d0` | **shuffling towards the player** (observed)  |
| 2     | `0x80101f0c` | `0x80102218` | lunge with raised arms                       |
| 3     | `0x8010259c` | `0x801025a4` | **grab and bite** (observed)                 |
| 4–6   | …            | …            | still open                                   |
| 5     | `0x801030b4` | `0x801030bc` | lying on the ground (set after a fall)       |
| 7     | `0x801036ac` | `0x801036b4` | spawned lying, gets up                       |
| 8     | `0x80103aa8` | `0x80103ab0` | eats (observed): sub 1 while hold flag `0x4000`, then sub 3 plays animation `0x15` once and goes to state 1 (chases Leon); sub 6/7 eats the dead player, turning to him |
| 9     | `0x80103d7c` | `0x80103d84` | knocked over by another zombie               |
| 10    | `0x801040b0` | `0x801040b8` | staggering / recoil                          |
| 12    | `0x80104548` | `0x80104680` | turns round to a noise behind it             |
| 14    | `0x80104cac` | `0x80104cb4` | attack on another level / another target     |

Routine 8 (room script modes, observed): mode 0 stand, 1 walk to the destination (turn rate
0x24, own gait), 2 lunge (animation 0x0A, holds at key 0x11 for 0x18 ticks), 3 eat (0x15).

---

## 3. Initialisation (`0x80100674`, observed)

**Hit points:** a random value from a table of 16 entries. The index is
`(rand() >> (rand() & 3)) & 15`, so the values are not evenly distributed:

| Table                             | Values                                                          |
|-----------------------------------|-----------------------------------------------------------------|
| normal (`0x8010c494`)             | 80, 94, 128, 75, 60, 95, 58, 75, 50, 83, 79, 66, 80, 65, 82, 65 |
| EASY (`0x800c77b4 & 0x20`, flag 0:`0x1a`, `0x8010c4b4`) | 54, 28, 54, 54, 70, 40, 54, 100, 40, 54, 70, 54, 54, 70, 54, 40 |

- On NORMAL, if `0x800c77ef < 4` there are **+15 HP**. `0x800c77ef` counts the objects the
  room has activated (`FUN_8001a8b0` adds 1): a zombie among few gets 15 more. EASY skips it.
- Zombie type `0x11`: fixed **250 HP**.
- **Clothes** (observed): the textures of types `0x12` and `0x1f` hold four 128-pixel pages
  (0 heads and hands, 1-3 whole bodies). `FUN_800169f4` retargets every face of a mesh to a
  page and its palette (CLUT row `0x1e0` + page). Type `0x1f`: all bones but 8, 11, 14 take
  page `obj+0x1cf & 0x3f` (byte 8 of `sce_em_set`), or a random 1-3 when that is 0
  (`(rand + (rand << (rand & 3))) % 3 + 1`): 1 a light shirt, 2 a jacket, 3 a police
  uniform. Type `0x12`: half get the second head (bone 8 → page 2), half the third body
  (page 3, hands 11 / 14 on page 2). DS: `enemy_set_skin`, `Enemy.bone_page`.
- **Crawler** (`obj+0x10e & 1`): HP halved, smaller collision box (200 instead of 500), and
  status bit `0x2000` (a lower priority target for the auto-aim). The kind table
  `0x8010c678` (by `obj+0x10e & 0x3f`) sends **every odd variant** to the crawler routine
  `0x80101178` (observed); disc 1 sets them in rooms 11A (3), 204 (7), 20B (1) and 603 (5).
  That routine runs by `obj+5` (tables `0x8010c730` / `0x8010c73c`, observed):
  - 0 crawls (`0x80102e14` / `0x80102f58`): set 1 animation 5 (blend 15, random key 0-15),
    turning `0x18` a frame to its goal while the root motion is over 20, 7-14 frames apart
    once more; it grabs Leon's leg within `0x514` when he is in one of its arm cones
    (facing ± `0x100`, half `0x100`) on its floor and nobody holds him;
  - 1 grabs (`0x801025a4` with the crawler's animations: `0x0e` grab, `0x0f` bite at key 1
    for 5 (×1.5), `0x10` let go). Shaken off, at key 22 Leon stamps its head off (part 8,
    blood, sound 2) and at the end of `0x10` it is dead (sub 6: routine 7, HP −1). Leon's
    side is `FUN_8010ae98` (table `0x8010cd50`, variants 2 / 3): set 2 animations 6 / 10
    grab, + 1 struggle, + 2 break free, then 9, the stamp (from behind turned round);
  - 2 lies (`0x801039a4` / `0x80103a84`: animation `0x17`) until Leon comes near in front.
  Hit (`0x801077c8`): animation 6 from a random key 0-7, groan `0xc`, then on. Death
  (`0x80108954`): animation 7, blood, groan `0xb` / `0xd`.
  **DS:** `is_crawler`, `crawler_tick` (`ST_CRAWL`, `ST_CR_HIT`, `ST_CR_LIE`), the grab with
  `grab.crawler`; variant 3 crawls, 1 lies in wait, 5 / 7 lie as dead (HP −1, no target).
  Shots pass over a crawler unless Leon aims down (as over a lying zombie).
- A third of the zombies (`(rand >> (rand & 3)) % 3 == 0`), and type `0x11` always, get
  flag `0x8000` in `obj+0x21a` and a stagger counter of 32–47 instead of 16–31: **fast
  zombies** (observed). In chase (`0x801019d0`) and lunge (`0x80102218`) they skip a keyframe
  whenever the key % 3 == 2 (`FUN_80029e00`, also with status bit `0x80` of `obj+0x10e`);
  the chase adds a turn of 8 there. Root motion is the difference to the last frame read
  (`FUN_8001581c`, `obj+0x20c`), so the skipped key's distance is kept: they walk a third
  faster.
- Gait (`obj+0x218`): animation **0 or 2** of set 0 (table `0x80100004` =
  `0,2,0,2,0,2,2,0`, index `(rand >> (rand & 3)) & 7`). The faster variants 1/3 and the
  gaits 4–7 are not used here.

**Spawn variants** (`obj+0x10e & 0x3f`, from the room script):

| Value | Start                                                   |
|-------|---------------------------------------------------------|
| 1     | state 2 at once                                         |
| 2, 4  | lying (state 7, animation `0x16`/`0x17`)                |
| 3     | state 0                                                 |
| 5, 7  | lies as if dead (HP = −1), gets up later                |
| 6     | state 1 at once (starts walking)                        |
| 8     | state 8                                                 |
| 10    | state 15, a special collision mode                      |
| odd   | crawler                                                 |

---

## 4. Perception (observed)

`FUN_80014fb4(obj, x, z, half_angle)` returns **0** if the target is inside the cone
`±half_angle` around the facing. Angles in 4096 = 360°:

| Value         | Cone   |
|---------------|--------|
| `0x200`       | ±45°   |
| `0x320` (800) | ±70°   |
| `0x400`       | ±90°   |
| `0x52c`       | ±116°  |

The player's noises are bits in `0x800c77f2` (set by the player control):
`0x01` shot, `0x02` walking, `0x04` running (assumed), `0x08`/`0x10` aiming.

Distances in game units. For comparison: 500 is about the collision width of a zombie.

### State 0 – lurking (`0x801011fc`)

```c
d = obj->dist;                                          // obj+0x1f0
front90 = in_cone(player, 0x400);
front70 = in_cone(player, 800);
if (d < 5000 && front90 && obj->awake)                  → state 1 (chase)
if (d < 3000 && !front70 && noise & 0x15 && 50 %)       → state 12 (turn round)
if (d < 2000 && !front90 && noise & 0x17 && 50 %)       → state 12
```

`obj->awake` = flag `0x800` in `obj+0x154` (line of sight, see below).

### State 1 – chasing (`0x8010167c`)

```c
if (d < 1200 && nobody_grabbing && same_level
    && ((left_arm  && grabbable(facing + 22.5°))
     || (right_arm && grabbable(facing - 22.5°))))      → state 3 (grab), set the grab flag
if (d < 3500 && !front90 && noise & 0x15 && 25 %)       → state 12
if (d < 2500 && !front90 && noise & 0x17 && 50 %)       → state 12
if (player_hp == -32768 && front45 && d < 1000)         → state 8 (eat)
```

`left_arm`/`right_arm` = flags `0x20`/`0x40` in `+0x21a` not set (which side is left is
assumed). `grabbable` = `FUN_800150f8`.
"Nobody grabbing" is the global flag `0x80` in `0x800c79c7`, so only **one zombie at a
time** can bite.

---

## 5. Movement (state 1, `0x801019d0`, observed)

- Animation = the chosen gait. The **speed comes from the animation** (`FUN_8001581c`, as
  for the player).
- Course: the zombie turns towards its target (`obj+0x1c4/0x1c6`) every frame with
  - **8** units/frame if `d < 5000`,
  - **16** if `d < 3000` (called in addition),
  - otherwise after a "weaving" pattern from table `0x8010c748` (32 entries, duration + a bit
    for turning faster) with random ±16 jerks.
- **Moaning:** every 30–45 frames a 1/32 chance of sound 10 or 11, then a 150 frame pause.
- Every 100–131 frames: 25 % chance of state 2 if `d < 5000`.
- State 2 at once if `d < 2000`, the player is in the ±45° cone and nobody is grabbing.
- The grab test (`d < 1200`, two ±22.5° arm cones from `FUN_800150f8`, one grabber at a
  time) is the same in the lunge (`0x80101f0c`).
- State 2 at once if the player runs (`0x04`), `d < 2500` and in the ±116° cone.
- With flag `0x1000` in `obj+0x21a` the zombie loses 1 HP every 8 frames and staggers
  (assumed: burning or acid). It dies below 0 HP.

The target is found through the room's path zones (RDT section 12, `FUN_80046fb0` /
`FUN_80047308`): a depth-first search through the zones with portal midpoints, an 800 unit
pull-back and a shortcut test; status bit `0x1000` targets the player directly.

---

## 6. Grab and bite (state 3, `0x801025a4`, observed)

Sequence over the sub state `obj+6`:

1. **Grab:** animation `0x0b` (crawler `0x0e`). The zombie turns to the player at once.
   The player goes into his routine 5 ("grabbed"), the global grab flag is set, moaning.
2. **Struggle:** a counter starts at **148**. −2 per frame, and −5 more per newly pressed
   button from `0x34f` (D-pad, aim, fire, run). Below 0 → **broken free** (step 4).
3. **Bite:** when the animation reaches the bite frame, damage is dealt:

   | Variant                       | Bite frame | Base value | Taken by the player |
   |-------------------------------|-----------:|-----------:|--------------------:|
   | standing                      | 16         | 20         | **30 HP**           |
   | crawler                       | 1          | 5          | 7 HP                |
   | type `0x17`/`0x11`            | 16         | 30         | 45 HP               |
   | type `0x17`/`0x11` crawling   | 1          | 10         | 15 HP               |

   Damage function `0x8003cf38` (executable, observed):
   ```c
   if (dmg < 41) dmg += dmg / 2;     // ×1.5
   player_hp -= dmg;                 // 0x800c794a, maximum 200
   if (player_hp < 0) {
       if (player_hp < -14 || saved_once) → the player dies (returns 2)
       else { player_hp = 0; saved_once = 1; → breaks free (returns 1) }
   }
   ```
   A player with few HP thus survives **once, narrowly** with 0 HP if the bite would take
   him at most 14 points below zero.
4. **Pushed away:** the zombie staggers back. With a 1/16 chance it loses an arm (flag
   `0x40` or `0x20` in `+0x21a`, see [damage.md](damage.md)). It can knock over another
   zombie, which then goes to state 9.

---

### Additions (observed, October 2026)

- **Animation selection** `obj+0x14c` = `0x0F0000 + start key·0x100 + number`. Set 0 (EMD
  sections 1/2, `obj+0x108`/`obj+0x17c`) = gaits; set 1 = the rest: 0 standing (59 keys),
  8/9 getting up, 0x0B grab, 0x12 eat, 0x16/0x17 lying.
- **Spawn** (`FUN_800534c8`, opcode 0x44): `obj+0x10e` = u16 from script bytes 4/5
  (variant | status << 8), `obj+0x158`/`0x15a` = the script u16 at +0x12/+0x14,
  `obj+0x154` = 0. Status bit `0x1000`: the target is the player directly (no path search,
  `FUN_80046fb0`); `0x2000`: on the ground (set when knocked down, spawned lying or
  crawling; cleared when it gets up) – the auto-aim takes it only if nothing else is in
  sight (`FUN_8003ed74`); `0x4000`: held lying; `0x8000`: the object is not processed or
  drawn.
- **State 0 movement** (`0x8010135c`): step 0 = standing animation from a random key 0–31,
  timer `0x158` = rand+150, `0x15a` = rand+300 (rand = 0..255, `FUN_80015988`). When `0x15a`
  runs out: moan (sound 11, 50 %), 50 % step 2 = gait without start offset, turns with 16
  towards the target for rand+150 frames, then step 0 again. Chases if `d < 3000` or
  (`d < 7500` and line of sight).
- **State 7** (`0x801036b4`): lying animation 0x17 (0x16 with `0x21a & 4`), waits until
  `0x10e & 0x4000` is cleared, then gets up with 8 (9) from key rand&15 moaning (10/11/12),
  then state 1. In room 100, exec function 2 clears the bit for zombie 0 after 50 frames
  (`member_copy` → `calc AND 0xBFFF` → `member_set2`).
- **State 1 turning**: target turn 8/frame below 5001, otherwise after table `0x8010c748`
  (32 u16: frames, bit 15 = 8/frame **away** from the target, i.e. weaving); every 30–45
  frames +16 and a moan chance; below 3000 another +16. A ±16 random jerk on each table
  change. `FUN_80014ef8(obj, x, z, rate)`: turns by at most `rate` (negative = away).
- **Line of sight** `obj+0x154 & 0x800` (`FUN_80046fb0`, every 8 frames): a line from part 8
  of the zombie to a part of the player crosses no room box with flag `0x2000`
  (`FUN_8004d000`; shapes 1–5 and 13 do not count, table `0x8009f480`). Box height: from
  `-1800 · (lowest floor bit)` to `-((type>>11)·100 + ((type>>6)&31)·1800)` – cars (1800
  high) do not block the view.
- **Members** (`FUN_8005202c`, for `member_set`/`member_copy`/`member_cmp`): 0 = `obj+0`
  (bit 0 = exists), 2/3/4/5 = `obj+4..7` (routines), 7 = `obj+0x10e`, 9 = `obj+0xb`,
  0x0B–0x0D = position, 0x0E–0x10 = rotation (0x0F = facing), 0x14/0x15 = destination,
  0x17 = `obj+0x1d4`, 0x1A = `obj+0x1da` (target).

## 7. Open points

- States 2, 4, 6, 10–15 and the crawler tables in detail.
- Hits, weapon damage, death and head shots: see [damage.md](damage.md).

---

## Tools

Building an ELF with the executable and the zombie code:

```bash
python3 -I tools/research/psxexe_to_elf.py SLES_009.72 sles_zombie.elf tools/research/re2_symbols_all.txt \
    --overlay "PL0/PLD/CDEMD0.EMS:0xD000:0xCD70:0x80100000"
```

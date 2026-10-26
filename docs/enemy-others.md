# RE2 (PS1, SLES-00972) – dog, crow, licker

Source: `SLES_009.72` + the enemy code from `PL0/PLD/CDEMD0.EMS` (disc 1, Europe).
**observed** = in the code as written, **assumed** = interpretation. Basics (code slots,
damage, hits) in [enemy-zombie.md](enemy-zombie.md) and [damage.md](damage.md).

---

## 1. Load table of all enemy types (observed)

`0x8009328c`, index `(type − 0x10) * 4 + 1 − slot`, 8 bytes each: sector offset in
`CDEMD0.EMS` (× 2048) and size. Slot 0 is linked at `0x80100000`, slot 1 at `0x8010D000`.

| Type   | Enemy (after the known RE2 ids, ✔ = confirmed in the code) | Slot 0 sector | Size    |
|--------|--------------------------------------------------|--------------:|--------:|
| `0x10`–`0x1F` | zombies ✔                                 | `0x001a`      | 52,592  |
| `0x20` | Cerberus (dog) ✔                                 | `0x04b6`      | 21,790  |
| `0x21` | crow ✔                                           | `0x0528`      | 18,980  |
| `0x22` | licker ✔                                         | `0x0562`      | 30,000  |
| `0x23` | alligator (room 40A) ✔                           | `0x05f1`      | 18,236  |
| `0x24` | licker, tougher variant ✔                        | `0x06a5`      | 23,592  |
| `0x25` | spider                                           | `0x071f`      | 25,964  |
| `0x26` | small spider                                     | `0x076b`      | 4,298   |
| `0x27` | G embryo                                         | `0x078a`      | 13,852  |
| `0x28` | G adult                                          | `0x07c3`      | 26,208  |
| `0x29` | cockroach (room 40B) ✔                           | `0x0872`      | 7,460   |
| `0x2a`, `0x2b` | Mr. X                                    | `0x0898`/`0x0911` | ~19,800 |
| `0x2d` | zombie arms through the windows (room 205) ✔    | `0x09af`      | 5,276   |
| `0x2e`, `0x39` | Ivy (green / purple, one code) ✔             | `0x09db`      | 22,478  |
| `0x2f` | Ivy's vines (segments, own objects) ✔            | `0x0a76`      | 9,216   |
| `0x3e`, `0x3f` | lab machines (60D; 608, 611): props ✔   | …             | …       |
| `0x3a` | giant moth (room 613) ✔                          | `0x0eed`      | 19,752  |
| `0x3b` | moth larvae (rooms 613, 614) ✔                   | `0x0f31`      | 5,286   |
| `0x31` | Birkin G2 (room 509) ✔                           | `0x0b39`      | 31,624  |
| `0x34` | Birkin's last form (room 617) ✔                  | `0x0c9f`      | 44,364  |
| `0x30`, `0x33`, `0x36` | Birkin, other forms                  | …             | …       |

`tools/research/overlay_symbols.py` prints the complete list.

People (types `0x40` and up, e.g. Kendo `0x48`) are not enemies: the room scripts move them
(routine 4, `plc_*`), see [engine-notes.md](engine-notes.md).

### Entry (observed)

Every block has an entry function that the executable calls per frame. It dispatches on
`obj+4` through a main table (init, AI, hit, death, …):

| Enemy  | Entry        | Main table   |
|--------|--------------|--------------|
| zombie | `0x801002c0` | `0x8010c654` |
| dog    | `0x80100004` | `0x8010525c` |
| crow   | `0x8010d13c` (slot 1) | `0x801118a4` |
| licker `0x22` | `0x80102e5c` | `0x80107468` |

For the dog and the crow the entry lowers the hit lock `+0x1d3` by 1 every frame (observed).
That settles an open point of [damage.md](damage.md).

---

## 2. Dog (Cerberus, type `0x20`)

**HP** (observed, init `0x801000f4`): table `0x801051d4` + random 0–3:
129, 95, 95, 95, 129, 80, 95, 95, 80, 95, 69, 80, 69, 95, 69, 80.
With mode flag `0x20`: 67, 67, 92, 75, 92, 75, 67, 69, 75, 67, 40, 67, 40, 67, 40, 79.

Collision box 600, start state by spawn variant (table `0x8010527c`).

### States (observed)

| State | Routine      | Behaviour |
|-------|--------------|-----------|
| 1 | `0x801007a8` | **Circling**: runs to a random point ±512/±1024 around the player (table `0x801052cc`), turns ≤ 16/frame. An **aggression counter** rises by 1 every frame while the player is closer than 3000; **+21** if the player runs. Above 64, on a player shot or a global flag → state 2. |
| 2 | `0x80100aa4` | **Hunting** with sub states (`0x801052ec`): run animation, turning up to **96/frame** (for comparison: zombie 8–16). After too many misses (counter > 450) it runs a **U-turn** and tries again. Sub state 3: patrols between the room's waypoints. |
| 3 | `0x80101170` | **Jump / bite attack**, sub states `0x8010530c`. |

**Attack condition** (`FUN_80104b6c`): distance < 4000, same level, a free path within
±22.5°, the player not in a locking animation right now.

**Bite** (`FUN_80104c84`, observed):
- A hit if the player is within **1000** units of a body point of the dog (model part at
  `+0x30c`, assumed the head) (only **700** if the player has ≤ 20 HP; assumed a fairness
  rule).
- Damage: base value 20 → **30 HP**.
- If the bite would kill and the dog is behind the player, its own **kill scene** starts
  (the player is torn down).
- The dog can attack another object instead of the player (`+0x227`).

### Movement, animations and reactions (observed, October 2026)

All animations are in the EMD's first set (27); `obj+0x14c` = `0x0F0000 + start key·0x100 +
number` as for the zombie. The dog moves by its own speed `obj+0x144` along its facing
(`FUN_80014c68`), not by the animation's root motion.

| Anim | Use |
|------|-----|
| `0x00` | standing, sniffing (state 0, 90 frames) |
| `0x01` | walking (states 0 and 1, speed 30) |
| `0x02` / `0x0D` / `0x0C` | running straight / turning left / right (`0x801054cc`, by the sign of the turn) |
| `0x07` | getting up after a hit |
| `0x11` | thrown back by a hit (also the death) |
| `0x12` | lying |
| `0x14` | crouching and leaping |
| `0x15` | landing after a missed leap |
| `0x16` | bouncing off after a bite |

- **State 0** (`0x80100708` / `0x801004cc`): walks between the room's waypoints at 30 a
  frame; 1/32 at a waypoint: sniffs. To state 1 when the player is on its floor and closer
  than 4000, or another dog attacked (`0x800c77f0 & 0x20`); to state 2 at a shot.
- **State 1** circling: target = player + offset from `0x801052cc` (x and z each one of
  1024, 0, −512, 0, −1024, 0, 512, 0), turning 16. Within 3000: if the player is within ±11°
  ahead (`FUN_800150f8(…, 0x80)` returns 0 **inside** the cone) and farther than 600 → leap.
- **State 2** hunting: speed +40 a frame up to 240, turning `0x60` (or from `0x801052fc`);
  every frame the player is not ahead adds 10 to `obj+0x21a`; above 450 a U-turn (sub state 2:
  slows by 6 to 10, turns `0x60` while drifting on, speeds up by 6). After a bite it waits
  `0x80105244[rand & 7]` frames (7, 31, 15, 15, 15, 15, 7, 31) before turning to the player.
- **Leap** (`0x801011d0` ff.): 8 frames crouching at speed 65, then 280 forward and 280 up
  (vertical speed −280, gravity +40 a frame, `FUN_80014cf0`); bites only while rising. Missed:
  +20 to `obj+0x21a`, lands (sound 10). Bit: bounces off backward (direction + `0x800`) at a
  quarter of the speed, turning `0x80` a frame up to `0x800`.
- **Hit** (`0x801031d8`): thrown along the player's facing plus `0x801053c0[rand & 7]`
  (`0x80`, −`0xC0`, −`0x80`, `0xC0`, −`0x100`, `0x100`, `0x100`, −`0x100`) at 240, 10 slower
  a frame (half and falling first when hit in the air); then lies (`0x12`), gets up (`0x07`)
  and hunts again. While thrown, further hits only bleed (`0x80103754`). Sound 1.
- **Death** (`0x8010400c`): the same throw, sound 7, then lies for good (routine 7).

The player's side of a bite (`FUN_80105050`): player routine 2 (damage), state 2 + side
(`FUN_800152b0`): `0x8003c528` plays PLD animation 3 (front, pushed back 200 a frame while the
animation eases in) or 4 (from behind, pushed forward), voice 1 / 2. A light hit (states 0 /
1, `0x8003c408`) plays PLD 5. Killed outside a grab (`FUN_80105098`, routine 3,
`0x8003ccd0`): PLD 2, voice 3.

### Damage against the dog (near / middle / far)

Handgun 18/16/15, shotgun 59/40/30, custom shotgun 300/75/65, magnum 200, custom magnum 300,
SMG 10, bowgun 26, knife 10.
A dog with 95 HP thus needs **6 handgun shots** or 2–4 shotgun shots.

---

## 3. Crow (type `0x21`)

**HP: fixed 10** (observed, init `0x8010d2fc`). Small collision box (300), hovers above the
floor (floor height via `FUN_8004c348`). Spawn flags: `0x4000` = starts in a fixed state
(assumed: perched), `0x40` = inactive at first.

**Control** (`0x8010d4e4`, observed): with a 1/32 chance per frame a random waypoint is chosen
for 0–127 frames (`FUN_800471f8`), otherwise the crow flies towards the player
(`FUN_80046fb0`). Once the player is dead it only circles a little. The line of sight to the
player is checked every frame (flag `2` in `+0x22a`). Below a distance of 900 the collision
box is lowered (assumed: a dive).

**Attack – pecking** (`0x8010f5b0`, observed):
- When the crow sits on the player (flag `4`) it pecks: blood, sound 2, damage 5 → **7 HP**
  per peck.
- The next peck follows after 2–9 frames.
- **Shaking off:** a counter drops by 1 per frame and by 3 more per new button from `0x34f`.
  Below 0 the crow flies off.

**States** (think / move tables `0x801118c4` / `0x80111900` by `obj+5`, observed):

| State | Behaviour |
|-------|-----------|
| 0 | perched: idle animations 0–4; wakes when the player is closer than 1800, runs within 5400, or shoots |
| 1 | waits 0–7 frames, then 2 |
| 2 | takes off: anim 7, speed 100–131 rising by 1 to 300, vertical speed −10, −80, −60, −100, −80 in steps, turning `0x20` |
| 4 | flies: anim 3 (glide) or 4 (flap) for 0–31 frames, toward the player or a waypoint (turning `0x20`); 1/16: climb (5, −40) or sink (6, +40) to stay 1800–4800 above the player's feet |
| 8 | sharp turn (`0x60`), slowing down, after circling wide for 10 checks |
| 13 | dive (`0x8010f244`): at the player's head height (feet − `0xb40`), turning `0x180`, +12 a frame; within 650 and ±99 in height → 14 |
| 14 | on the player (`0x8010f4ec`): anim 5, pecks every 2–9 frames (blood, sound 2, 5 → 7 HP), shaken off as the counter (100) runs out |
| 10 | hit (`0x8010f900`): anim 9, knocked away from the player at 100, falls (gravity 20); on the floor sound 5, anim `0x0b` for 11 frames, then dead |

Attack decision (`FUN_801111bc`): 901–8100 away, in sight, the player within ±22.5° ahead
and no other crow attacking (`0x800c77f0 & 1`, also the player's grab lock).

**Damage against the crow:** every weapon does at least 15 (handgun 15, SMG 15, knife 15
near), so **every hit** kills a crow.

---

## 4. Licker (type `0x22`, tougher variant `0x24`)

**HP type `0x22`** (observed, init `0x80103158`): table `0x80107124`:
97, 111, 91, 85, 120, 125, 109, 111, 111, 109, 91, 111, 125, 120, 97, 111.
With mode flag `0x20`: 75, 75, 80, 80, 85, 79, 72, 100, 90, 85, 92, 107, 99, 111, 100, 110.
With story flag `0xa4` set there are −10 or −20 HP.

**Set by a scene** (observed, init `0x80103158`): with status bit `0x4000` the start state
comes from `0x801073bc[variant & 3]` instead of `0x801073b8`: 16, 17, 18, 22. All of them
wait for the room script's signal (member 0x17, `obj+0x1d4`) before anything happens:
16/17 hang on the ceiling (animation 3) and then drop: 16 (`0x80104d14`) lets go with animation
`0x12`, then turns over and falls with `0x13`; 17 (`0x80104f74`) falls with `0x13` at once.
Both fall at +72 a frame more each frame (`obj+0x146`) and play `0x13` to its end on the
floor; 17 hisses (sound 2) on landing. 22 plays animation `0xe` and then
backs off (state 5), 18 (`0x80105114`) plays animation 0 once and on the signal crawls on
by it until the next signal - room 201's licker passing the window outside on the first
visit, which never comes in. **DS port:** `LICK_SCRIPT` (`licker_script`): waits, not
aimed at nor in the way; on the signal the crawler crawls, the others hunt.

**Ceiling and walls** (observed): the spawn variant `(+0x10e >> 8) & 0xF` sets the
orientation. 1 = ceiling, 2–5 = different walls or corners (gravity vector in
`0x80107524`/`0x80107528`). The helper `0x8010029c` finds the floor or ceiling height for it.

### Attacks (observed)

| Attack | Routine | Range / cone | Damage to the player |
|--------|---------|--------------|----------------------|
| **Claw swipe** | state 9, `0x801020bc` | frames 17–24, ±22.5°, range from table `0x80107382` per frame | 25 → **37 HP** |
| **Tongue lash** | state 10, `0x801023c4` | frames 1–5, ±30°, **2800** | 15 → **22 HP** |
| **Jump attack** | `0x80103e8c` | frames 11–14 in the air, distance < 2200, height −199…+710 | **80**, or **100** if the player has > 110 HP |

About the **jump attack**:
- If the player survives, he is set to **at least 40 HP** and knocked down.
- If he dies, its own kill scene starts (flags `0x4000100`).
- In practice the jump kills every player with **less than 80 HP**; everybody else ends up
  with 40 HP or more.

Only **one licker at a time** attacks with the claw (lock `0x8010752c`).

### Perception and movement (observed, October 2026)

`FUN_80100db0`: the licker is blind. It reacts within 3000 to any noise of the player
(`0x800c77f0 & 0x170000`) and within 9000 to running or shots (`0x800c77f2 & 5`).

| State | Routine | Animation | Behaviour |
|-------|---------|-----------|-----------|
| 0 | `0x801017c8` | 3 | listens |
| 2 | `0x801019a4` | 2, then 4 | turns to the sound (`0x60`) |
| 5 | `0x80101c90` | 4 / 2 | backs off a little and turns (`0x40` within 4000, else `0x10`) |
| 6 / 20 | `0x80101efc` / `0x8010291c` | 1 | crawls at the player (root motion); within 2000 → claw (state 9) if ±67° ahead, else turn; beyond 4000 → tongue (10), 20 or 8 at random |
| 9 | `0x801020bc` | 7 | claw: keys 17–24, reach by key from `0x80107382` (2500, 3500, 4600, 4500, 4200, 3500, 3000, 2500) |
| 10 | `0x801023c4` | `0x15` | tongue: keys 1–5, ±30°, 2800 |
| 11 | `0x80103cec` | 6, `0x1f`, `0x20`, `0x21` | jump: crouch, take off, in the air (550 forward −8 a frame, 245 up, gravity 35), land; hits on keys 11–14 within 2200 |

Hit (`0x8010574c`): pushed along the player's facing (speed −6 a frame), sound 7; the next
state by distance (`FUN_801055cc`). Death (`0x80106a20`): anim `0x1c`, then `0x0b`, sound 8.
Type `0x24` has fewer animations: its tongue is 15, hit 16, death 20, jump 23 / 24 / 25.

### Type `0x24`

The same attack values (tongue 15, jump 80/100, observed), but much less damage from weapons
(see below). Its HP table has not been assigned with certainty yet.

### Damage against lickers (near / middle / far)

| Weapon         | `0x22`        | `0x24`       |
|----------------|---------------|--------------|
| handgun        | 14 / 13 / 12  | 10 / 9 / 8   |
| shotgun        | 50 / 45 / 40  | 30 / 27 / 25 |
| custom shotgun | 110 / 95 / 80 | 50 / 47 / 45 |
| magnum         | 200           | **40**       |
| GL acid        | 130 / 60 / 10 | 65 / 65 / 15 |
| SMG            | 11            | 8            |

A normal licker with 111 HP thus needs 8 handgun shots or 3 shotgun shots from near.

---

## 5. Spider (type `0x25`) and the small ones (`0x26`)

Overlay slot 1 (`0x8010d000`), main table `0x801132b8`; init `0x8010d1ec`. **HP** `0x801131cc`
+ rand & 3: 109, 109, 109, 109, 129, 109, 109, 129, 109, 109, 109, 129, 109, 99, 109, 109.
Spawn variant `& 0xf`: 0 floor, 2–3 ceiling, 4–11 walls (orientation `(variant − 4) >> 1`
quarter turns); the floor state table is `0x801132e8`. No root motion: the spider moves by
`obj+0x144`.

| State | Routine | Anim | Behaviour |
|-------|---------|------|-----------|
| 0 | `0x8010d708` | 0 | decides: not facing (±90°) and within 8000 → 5; a shot → 9 or 5; facing (±11°): 3000–7500 → 7, 1000–6000 → 8 (3/16) or 7 |
| 1–3 | `0x8010d850` | 2 / 3 | walks (table `0x80113310`: anim, turn, speed step) |
| 4 | `0x8010da00` | 3 | wanders, turning ±`0x18` for 10–63 frames |
| 5 / 6 | `0x8010db04` | 2 / 3 | turns on the spot (`0x80` / `0x20`) for 60–91 frames |
| 7 | `0x8010dc64` | 4, 5, 5, 7 | crouches (speed 100), aims (`0x40`), rushes at 300 turning `0x10` for 10–41 frames, stops; bites within 1500 and `0x10cc` in height (`FUN_80112a64`): 20 → 30 HP, sound 1 |
| 8 | `0x8010df0c` | `0x0b` | spits (`FUN_80112828` spawns the projectile) |
| 9 | `0x8010e198` | 8 | jumps aside: 400–463 toward the player's direction + rand (+ `0x800`) + `0x380` |

Hit (`0x8010fcd8`): anim 1, then state 9 or a random one (`0x80113410`). Death (`0x80110d10`):
anim `0x0c` (206 keys), then dead.

## 6. Mr. X (type `0x2a`)

Overlay slot 0, main table `0x80104c98`, states `0x80104cb4`; init `0x80101324`. HP
`DAT_80104660[...] + 20000` (the room decides which entry, e.g. rooms 0x117, 0x10d, 0x507,
0x109, 0x20d, 0x20c, 0x602 have their own): shots hardly matter. Walking (`0x80101d6c`): within
`0x157d` (`0x1965` with status `0x100`) he turns up to `0x30` a frame toward the target
(waypoint when not in view) and walks with the animation's motion; otherwise state 2.
Attacks (hit check `FUN_801002c0(obj, anim, key0, key1, range)`, damage by `FUN_801005fc`):
state 25 anim 12 keys `0x2f`-`0x32` range 550 damage 40; 26 anim 15 keys 14-19 range 950
damage 10; 27 anim 3 keys 21-25 range 750 damage 30. Spawn status `0x4000` holds him.
The port uses these; the animation numbers are those of the state routines and unchecked
on screen.

## 7. Zombie arms (type `0x2d`)

Room 205 sets ten of them (slots 0–9, so the DS keeps eleven slots: 0–9 and the partner).
The room script parks them (status `0x80`) and lets them out from the camera events 3–6.
Code: CDEMD0.EMS sector `0x9af`, size `0x149c`, at `0x80100000`.

- **Model:** one model, two arms: parts 1–7 and 8–14 (table `0x80101387`). Spawn variant
  bit 0 picks the arm; the other one's parts are never drawn (`0x801000f8`). The hand is part
  3 / 10 (`0x8010138a + variant * 7`): reach checks measure from it. HP 250, any hit sends the
  arm back (routine 2 `0x80100f30`, routine 3 `0x80100fc0`: blood at the arm, HP back to 250).
- **Modes** (obj+5, check table `0x801013c0`, action table `0x801013e0`):

| Mode | Check / action | What it does |
|---|---|---|
| 0 hidden | `0x8010050c` / `0x80100674` | 400 back inside the wall, waits `rand & 15` ticks; Leon < 2500 in the front half → out at once (mode 1); Leon < 3000 within ±`0x2f8` → slides out (anim 0, +50 per tick up to key 10). Sound 4, at most every 60 ticks. |
| 1 wave | `0x801007a8` / `0x80100830` | anim 1 or 2, `rand & 31 + 60` ticks, turns `0x20` toward Leon but at most ±`0x180` from its set direction; hand < 900 from Leon → mode 3; time up → mode 5. |
| 3 reach | `0x801009a0` / `0x80100a90` | anim 3, turns `0x30`; from key 5 hand < 600 and no cooldown: holds him (mode 4), or joins the arm that already does (mode 6). End → mode 5. |
| 4 hold | `0x80100b68` | anim 5; Leon (player routine 5, `0x801011fc`) plays set 2 anim 0 facing the arm (turned round if it got him from behind). Counter 150, −1 per tick and −2 per key pressed; then he plays set 2 anim 1 and is free, 120 ticks no new hold (`0x800c77f0`). **No damage.** |
| 5 retract | `0x80100d34` | anim 4, sound 6, back 30 per tick; then hidden for good (mode 7 has no code). |
| 6 join | `0x80100e40` | anim 5 until the hold ends, then mode 1. |

## 8. Giant moth (type `0x3a`) and larvae (`0x3b`)

Room 613: the moth (slot 1, variant `0x10`: on the wall) and six larvae (variant 8). Their set
flags (`sce_em_set` byte 9: `0x10`, `0x21`–`0x25`, `0x2e`, array 7 in stage 6) tell the room
script when all are dead (function 3 checks them before it lets Leon go on). Room 614 has one
larva spawner (variant `0x6002`).

**Moth** (CDEMD0.EMS sector `0xeed`, size `0x4d28`). HP 250 (200 with system flag `0x20` of
`0x800c77b4`). Modes in obj+5 dispatch through `0x80104a5c` (so mode *m* is the table's entry
*m* + 1):

| Mode | Code | What it does | DS |
|---|---|---|---|
| 0 | `0x801004a0` | anim 0, waits 30–61 ticks (20 after a cling) | `MO_IDLE` |
| 1 | `0x80100558` | decides: Leon < 6000 and no cooldown → table `0x80104a98` (mode 8 one time in eight, else 4); otherwise table `0x80104a90` (modes 5 / 4 alternately) | `MO_DECIDE` |
| 2 | `0x80100738` | take-off / landing (sub-table `0x80104aa0`, per distance class `0x80104ab0`) | folded into the others |
| 3 | `0x80100c50` | clings to Leon (sub-table `0x80104ae0`): flies (dist − 1000, or − 500 from behind) / 26 a tick onto him, anim 9, rising `0x37` a tick; then anim 10: counter 20 minus keys pressed, every 4 ticks 3 HP (`FUN_8003cf38(3)`) and its own hold − 3; then flies off (`0x80100f84`: back, up, 60 ticks no clinging) | `MO_CLING`, `MO_FLYOFF` |
| 4 | `0x8010117c` | anim 3, turns `0x30` a tick toward Leon (away when he is dead), wobbles (`0x80104ad0`), up to 60 ticks; clings when Leon < 1850 within ±`0x180` (`FUN_80104448`) | `MO_HOVER` |
| 5 | `0x801014d8` | anim 2, flies 50 a tick (75 when not flying) for half the distance, turning `0x10` | `MO_APPROACH` |
| 6 | `0x80101674` | turns round toward Leon in steps of `0x30` | — |
| 7 | `0x80101808` | lands on a wall (`0x80104af0`) | — |
| 8 | `0x80101b9c` | anim 4, rises `0x6f` a tick by `0x30f`, flutters six times 15 ticks (sound 5), dusts spores (effect 5) | `MO_SPORE` (no spores yet) |
| 9 | `0x80101e04` | crawls on the floor toward Leon (speed `0x4b`) | — |
| 10 | `0x801020f0` | on the wall (variant `0x10`, anim 8, sub-table `0x80104b04`) | `MO_PERCH`, `MO_TAKEOFF` |
| 12 | `0x801026fc` | flies away for up to 450 ticks or until Leon < 1700 | — |

The DS version flies at 800 above the floor; the PS1 keeps y in obj+0x3c against the floor
height obj+0x1c2 with several speeds. Dying: falls to the floor (no death animation found).

**Larvae** (sector `0xf31`, size `0x14a6`). HP 1. Mode by `variant & 15` (`0x80101418`):
0 crawl (speed 2, turns 2 toward Leon every 4 ticks, spits when Leon < 1800 within ±`0x100`,
one at a time for 90 ticks: effect `0x2b`, damage not found, 4 HP here), 1 falls from the
ceiling (+40 a tick up to 600, sound 1), 2 / 7 spawners (release larvae with variant `0x6001`
at Leon, at most `0x18`/`0x14`/`0x0d` by moth count `0x80101424`, triggered by obj+0x1d4 – not
ported), 3–6 sit on walls, 8 crawls in circles (turn ±2 a tick, reversed every 2048 ticks).
Killed: shrinks away (`0x80100ebc`, scale − `0x200` a tick).

## 9. Enemies stay dead (set flag)

`scd_op_44` (`0x800534c8`) skips an `sce_em_set` whose byte 9 is not `0xff` when that bit is
set in array 6 (`0x800cc478`, stages 1–3) or array 7 (`0x800cc498`, stages 4–7, or with
system flag 1:6); the enemy keeps the bit in obj+0x1ce and sets it when it dies. The DS
version: `script.c` checks and passes array/bit in the upper bits of the spawn flags,
`enemy.c` sets the flag when the enemy's HP drops below 0.

## 10. NORMAL / EASY

`COMMON/BIN/TITLE.BIN` asks NORMAL / EASY after NEW GAME and sets `0x800c77b4 |= 0x20000`, plus
bit `0x20` (flag 0:`0x1a`) for EASY. Only enemy overlays read that bit: they take their HP
from a second table. Items and Leon are the same.

| Type | Normal | Easy |
|---|---|---|
| zombie `0x10`-`0x1f` | `0x8010c494` (50–128), +15 while fewer than 4 objects are active (`0x800c77ef`) | `0x8010c4b4` (28–100), no +15 |
| dog `0x20` | `0x801051d4` (69–129) | `0x801051f4` (40–92) |
| licker `0x22` | `0x80107124` (85–125), −10 after flag 4:`0xa4` | `0x80107144` (72–111), −20 |
| licker `0x24` | `0x80105860` (87–126), −30 after flag 4:`0xa4` | `0x80105840` (100–140), −40 |
| spider `0x25` | `0x801061cc` (99–129) | `0x801061ec` (49–99) |
| Mr. X `0x2a` | 20000 + `0x80104660`[case] | 20000 + `0x80104674`[case] (not ported: the case comes from status bits and stage) |
| moth `0x3a` | 250 | 200 |
| alligator, plants, G (`0x23`, `0x2b`, `0x2e`, `0x31`, `0x33`, `0x34`, `0x39`) | own tables | own tables (not ported with them yet) |

## 11. Ivy (types `0x2e` / `0x39`) and its vines (`0x2f`)

Rooms 60A, 60B, 60E. Code: CDEMD0.EMS (sector of `0x2e`), init `0x80100158`, routine 1
`0x80100660`, modes `0x80105640`.

- **HP:** `0x80105568` (easy `0x80105588`) + `rand & 3`; after flag 4:`0xa4` 50 less (easy 20).
- **Start mode** by `variant & 15` (`0x80105638`: 0, 1, 0, 1, 8, 8, 9, 11, …). Variant 7 (room
  60B) is mode 11: it stands among the plants; the DS wakes it at 3000 (assumed).
- **Vines:** the init makes 4 (3 with variant bit `0x10`) objects of type `0x2f` from table
  `0x801055b8` and hangs them on itself (`obj+0x1b4`); they swing and grab. The DS draws Ivy's
  own animations only and hides a `0x2f` with variant 0 (it builds such a chain). Other
  `0x2f` (variant `0x20`: 1 HP, `0x21`: cannot be cut) sway where the room sets them.
- **Modes:** 0 wait (anim 0) until Leon < 10000; 1 walk (anim 1, root motion, turn
  `FUN_80105114(.., 6)` – too slow as plain units here, the DS turns `0x20`); from walking
  (`FUN_80100c7c`) within 3000 and ±`0x200`: 4 swipe (anim 2, hit test on vine parts `0x11` /
  `0x17` at 40), 5 spit acid (effect `0x1d000c00` from part `0x7ac`, sound 3), 6 grab (Leon in
  player routine 6, pulled in to 1500 at 40 a tick; anims 8, 9, 10 with Leon's set 2 anims
  0, 1, 2; effect 8 on him), 12 is the collapse.
- **Not found:** the damage of swipe, acid and bite (Leon's routine 6): the DS uses 20, 10
  and 15 (x1.5) as guesses; mashing frees Leon like a zombie's grab.

## 12. Alligator (type `0x23`)

Room 40A (the sewer). Overlay at `0x80100000`: init `0x801004e0`, routine 1 `0x80100738`
(check table `0x80104490`, action table `0x801044b0`, mode *m* = entries *m*+1 / *m*+9),
hit routine `0x80101f84` (types by `FUN_80101e40`), death routine `0x80102bf8`.

- **HP** 25000 + 240 (`0x80104442`; easy `0x80104440`, also 240).
- **Start:** mode 0 waits for `obj+0x1d4` (member `0x17`), which the room script's function
  11 sets (`member_copy 10 17; calc or 1`). Then anim 6 (surfacing), anim 3 and a quarter
  turn (`+0x400`).
- **The chase** (modes 1 / 2, anims 0 crawl / 1 lunge, root motion): at x < −`0x45d5`
  (mode 3: sound 4, anims 2 and 3, `+0x400`) and again at z > −`0x1c9b`; after the second
  turn mode 7 plays anim 0 to key `0x42` and mode 4 (`0x80100f2c`) eats Leon (HP −1, player
  routine 5).
- **The gas cylinder:** script function 10 lowers it (object 1, flags 5:00 and 5:01, 4:`0x9b`);
  hanging at z ≈ −9500. With 5:00 set and the cylinder `0x2199`–`0x2327` ahead
  (`0x80100938`), the alligator takes it in its jaws (`obj+0x226` 1 … 7).
- **Hits** (`FUN_80101e40`): `obj+0x226` = 7 → type 1 (`0x801020ec`): the explosion, anim 9,
  camera cuts, then flag 5:`0x1e` and status `0x4000` – the script (function 11) waits for
  5:`0x1e`. HP below 25000 with `obj+0x218` bit `0x10` (past the second turn) in camera 8 →
  types 2 / 3: it swims away (anims 7 / 1 / 8). Else one hit in four: type 4, anim 10.
- **DS:** the course, the turns, the cylinder bite and the three hit outcomes as above;
  a bite on the way (60 HP, a guess) instead of `FUN_801012d0` / `obj+0x224`, whose effect is
  in Leon's routine 5; the camera cuts of the death and escape scenes are not played.

## 13. Birkin G2 (type `0x31`)

Room 509 (Leon A; with flag 1:1 the room sets `0x33` instead). Overlay `0x59c800` in CDEMD0.EMS,
init `0x801004c0`, routine 1 `0x801009b4` dispatches by `variant & 0x3f` (`0x80107a94`).

- **HP** 700 (easy 600). Variants 2 / 3: 5000 HP, puppets of the scenes (room 500).
- **Entrance** (mode 7, `0x80103aa0`): pose anim `0x13`; `obj+0x1d4` bit 1 (script member
  `0x17` = 1) → anims `0x14`, `0x15`, `0x16`; then he sets bit 2 – the script waits for
  member `0x17` ≥ 2 – and on bit 4 (`member_set 17 04`) roars (anim `0x0f`) and fights.
- **Choice** (mode 1 check `0x80100c4c`): below 100 HP (or `obj+0x224` 0) variant 1, mode 6;
  within 1500 and ±`0x400`: mode 5 (anim `0x0e`); 2501–3399 in front, no cooldown: mode 2,
  the lunge (anims 6, 7; claw test at 1200: `FUN_8003cf38(20)`, sound 3; cooldown
  `obj+0x21a` = 20); within 2500 and ±`0x4c8`: mode 4, the swipe (15; with Leon held the
  500 grab, `0x80101b78`); Leon running within 3400, ±`0x270`, 1 in 4: mode 8 (anim 9,
  30). With Leon below 60 HP an attack is held back up to two times (mode 3).
- **Walk** (mode 1, `0x80100f64`): anim 1 (variant 1: anim 2), turns `0x20` (`0x30` when off by
  more than `0x338`), root motion, sounds 13 / 9.
- **DS:** the entrance, the choice and the four attacks as above; mode 5's damage (20) and
  the flinch (anim 3, one hit in eight) are guesses; death: anim 10, then the kill flag
  (7:`0x17`) the room script checks.

## 14. Birkin's last form (type `0x34`)

Room 617 (the lab). Overlay `0x64f800` (size `0xad4c`): init `0x8010062c`, routine 1 `0x80100a08`
(`variant & 0x3f` → `0x8010aa90`; both variants use the check table `0x8010aa98` and the action
table `0x8010aae0`, mode *m* = entries *m*+2 / *m*+20), death `0x801072e8`.

- **HP** 1000 (easy 900); after the change 700 (easy 500).
- **Start** (mode 13, `0x80104910`): anim `0x17` until `obj+0x1d4` bit 1 (script function 7:
  `member_set 0c` −`0x2328`, then `member_set 17 01`); it falls (speed + 30 a tick) to the
  floor, anims `0x19` and 9, then mode 1 with `obj+0x21e` = 30.
- **Walk** (mode 1, `0x80101670`): anim 2, turns `0x10` (`0x20` when off by more than `0x338`).
  Check `0x8010157c`: ±`0x200`, < 4000, no cooldown → mode 2; ±`0x400`, < 2000 → mode 8;
  HP < 700 → mode 0.
- **Claw** (mode 2, `0x80101874`): anim 3 or 4 (4 within 2500), the claw sphere 1700 / 1500
  (`FUN_800151d4`) → 15 HP; if that kills Leon, the grab (player routine 6). Cooldown 10.
- **Change** (mode 0, `0x80100bd4`): anim `0x0b` (HP reset, `obj+0x1d4` bit 2), `0x0c`, `0x0a`
  (variant 1), bit 8, then waits for bit `0x10` – the room script (function 3: bit test 2 →
  event 13, whose end sets member `0x17` = `0x10`).
- **Death** (`0x801072e8`): anim `0x11`, bit 4 (script function 3 → event 14), anim `0x1c`.
- **DS:** as above; mode 8 is the claw too, the flinch (mode 6, anim `0x0f`, one hit in eight)
  is simplified; the first form cannot be killed (it changes at 700).

## 15. Cockroaches (type `0x29`) and the lab machines (`0x3e`, `0x3f`)

**Cockroaches:** room 40B sets 24 (slots 0–23 – the DS keeps the PS1's 33 object slots,
`0x800c7a14`, `MAX_ENEMIES` 33). 10 HP. Init `0x801002c0`, modes via `0x80101b24` / `0x80101b38`.

- They live on the inside of the sewer pipe; the overlay has it built in: axis along x at
  z −`0x6270`, y −`0x708`, radius `0x708`; `obj+0x218` is the angle round it (0 = the top), and
  z / y follow from it. The model is turned about x accordingly (DS: `Enemy.roll`).
- Mode 0 (`0x8010051c`): anims 1 / 0 for a while; mode 1 (`0x80100608`): crawls, speed up to
  `0x8c` (+`0x14`), heading turned `0x20` a tick to a random aim; x along the axis, the
  sideways part (× `0x64000` / `0x113fa0`) round it. A new decision one time in four: from the
  upper half (y < −`0x708`) one time in four mode 2, else mode 0.
- Mode 2 (`0x8010088c`): sound 3, flies at a part of Leon (table `0x80101b54`, 14 bytes each)
  and clings within 200 (Leon in player routine 5); gives up after 90 ticks.
  Mode 3 (`0x80100e58`): clings `0x5a` + rand / 16 ticks, −8 a tick while Leon shakes
  (`0x800c5f46`); mode 4 (`0x8010105c`): falls, sound 1, crawls again. More than two on him
  for `0xe1` ticks: he dies (`0x800c77f8` = 3, HP −1).
- **DS:** as above; the part Leon is held by is approximated (a random point on his body),
  the damage of player routine 5 is a guess (1 HP every 32 ticks per cockroach).

**Machines:** `0x3e` (a column, 60D) and `0x3f` (an apparatus, 608 / 611) are set like enemies
but are machines: the DS plays their animation and never aims at them or lets them act.

## 16. The G forms and the second tyrant of the scenes

Types `0x27` (G embryo), `0x28` (G adult), `0x2b` (the second tyrant, 702), `0x2c` (603), `0x30`
(G1: 219, 303), `0x33` (G2 of the other scenario, 509), `0x36` / `0x37` (704: a body and four
tentacles). Their overlays are not ported. Where they appear the room scripts move them with
`plc_*` (which works for every enemy) and set member `0x17`; none of these scripts waits for
an answer from them (no `member_cmp 17`), so the scenes run through.

- **Without a set flag** (byte 9 `0xff`: 301, 219, 603, 704; the embryo next to 303's adult)
  they are actors (`ACT_STAND`): their idle animation, never aimed at.
- **With one** (303's G adult `0x62` and embryos `0x63`/`0x81`/`0x82`, 702's tyrant `0x28`,
  509's `0x33` `0x17`): a plain fight with their own animations, chosen by root motion and
  length (table `actor_types` in `enemy.c`), not while a scene has Leon (flag 2:07).
  HP from their inits (`sh 0x156`): embryo 1, G adult 450, tyrant 10000, G1 400, `0x33` 750,
  `0x36` 400, `0x37` −1 (cannot be hurt). Damage values are guesses.

## 17. Open points

- The lickers' perception (do they react to noise?) and the switch ceiling ↔ floor.
- The licker on ceilings and walls (the gravity vectors `0x80107524` / `0x80107528`); the
  crows perched in routine 4 (status `0x4000`).
- The alligator's bite on the way (`obj+0x224`, Leon's routine 5) and the camera cuts of its
  death and escape.
- The real AIs of `0x27`, `0x28`, `0x2b`, `0x2c`, `0x30`, `0x33`, `0x36`, `0x37` (section 16 is a
  stand-in); mode 8 of `0x34`, the G2 second manner (variant 1);
  `0x27` / `0x28` (G embryo / adult, rooms 301 / 303), `0x2b` (702), `0x2c` (603), `0x30`, `0x33`,
  `0x36` / `0x37` (704): their load information is above.
- Ivy: the vine objects' own grabbing, the real damage values, mode 11.
- Moth: modes 2, 6, 7, 9, 12 and the spores; larva spawners (what sets obj+0x1d4) and the
  spit's damage.

## Tools

```bash
# symbols + the overlay argument for an enemy type (type in hex, slot 0/1, prefix)
python3 -I tools/research/overlay_symbols.py SLES_009.72 PL0/PLD/CDEMD0.EMS 20 0 em20 > sym_em20.txt 2> ovl_em20.arg
python3 -I tools/research/psxexe_to_elf.py SLES_009.72 dog.elf sym_em20.txt $(cat ovl_em20.arg)
```

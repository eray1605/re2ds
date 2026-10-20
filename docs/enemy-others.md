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
| `0x23` | alligator                                        | `0x05f1`      | 18,236  |
| `0x24` | licker, tougher variant ✔                        | `0x06a5`      | 23,592  |
| `0x25` | spider                                           | `0x071f`      | 25,964  |
| `0x26` | small spider                                     | `0x076b`      | 4,298   |
| `0x27` | G embryo                                         | `0x078a`      | 13,852  |
| `0x28` | G adult                                          | `0x07c3`      | 26,208  |
| `0x29` | cockroach                                        | `0x0872`      | 7,460   |
| `0x2a`, `0x2b` | Mr. X                                    | `0x0898`/`0x0911` | ~19,800 |
| `0x2e`, `0x2f`, `0x37`, `0x39` | plants / Ivy / tentacles | …             | …       |
| `0x30`, `0x31`, `0x33`, `0x34`, `0x36` | Birkin G1–G5     | …             | …       |
| `0x3a`, `0x3b` | moth, larva                              | …             | …       |

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

## 7. Open points

- The lickers' perception (do they react to noise?) and the switch ceiling ↔ floor.
- The HP table of type `0x24`.
- The licker on ceilings and walls (the gravity vectors `0x80107524` / `0x80107528`); the
  crows perched in routine 4 (status `0x4000`).
- The alligator (`0x23`, room 40A0): a choreographed chase (init `0x801004e0`, 25000 HP, corner
  thresholds x < −`0x45d5` and z > −`0x1c9b` in `0x80100938`, the gas cylinder via flags
  `0x800cc470` bits 0/1, its kill scene in `0x80100f2c`); the room script waits for flags
  5:`0x0a` / 5:`0x0b` it sets.
- G (all stages), Mr. X, plants, moths, cockroaches: their load information is above.

## Tools

```bash
# symbols + the overlay argument for an enemy type (type in hex, slot 0/1, prefix)
python3 -I tools/research/overlay_symbols.py SLES_009.72 PL0/PLD/CDEMD0.EMS 20 0 em20 > sym_em20.txt 2> ovl_em20.arg
python3 -I tools/research/psxexe_to_elf.py SLES_009.72 dog.elf sym_em20.txt $(cat ovl_em20.arg)
```

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

## 5. Open points

- The lickers' perception (do they react to noise?) and the switch ceiling ↔ floor.
- The HP table of type `0x24`.
- Death and hit animations of the dog, crow and licker.
- The other enemies (alligator, spiders, G, Mr. X, plants): their load information is above.

## Tools

```bash
# symbols + the overlay argument for an enemy type (type in hex, slot 0/1, prefix)
python3 -I tools/research/overlay_symbols.py SLES_009.72 PL0/PLD/CDEMD0.EMS 20 0 em20 > sym_em20.txt 2> ovl_em20.arg
python3 -I tools/research/psxexe_to_elf.py SLES_009.72 dog.elf sym_em20.txt $(cat ovl_em20.arg)
```

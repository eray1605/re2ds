# RE2 (PS1, SLES-00972) – hits and weapon damage

Sources as in [player-control.md](player-control.md) and [enemy-zombie.md](enemy-zombie.md).
**observed** = in the code as written, **assumed** = interpretation.

---

## 1. Sequence of a shot

1. **Pick the target** – `FUN_8003ed74` when the weapon is raised (observed):
   - walks all active enemies (HP ≥ 0, no flags `0xc000` in `+0x10e`),
   - checks the line of sight (`FUN_8004d000`),
   - takes the **nearest** enemy, optionally only up to a maximum distance,
   - enemies with flag `0x2000` (lying or crawling) are only taken if there is no other,
   - without a target `+0x1b4` points at the player himself.
2. **Classify the aim height** – `FUN_8003ef98` (observed): compares the target's height with
   the aim posture and sets the upper 3 bits of the player flag word. This picks one of
   3 shot shapes per weapon (table `0x8009ea60 + weapon*0x18 + shape*8`).
3. **Use up a round** – the handler from `0x8009f070[weapon]`, mostly just `FUN_80065594`.
4. **Hit check** – `FUN_8003dd8c`, in the frames the shot shape gives (a list of pairs
   `(shape, frames)`, `0xFF` = end, `0xFE` = loop for continuous fire).

### Hit check (`0x8003dd8c`, observed)

For every enemy that
- is active and has HP ≥ 0,
- **has no hit lock right now** (`+0x1d3 == 0`, see recoil below),
- is in the line of sight,

up to **3 test boxes** of the shot shape are tested (`FUN_8003e914`). The boxes are grown by
the enemy's size (`+0x9a`, `+0x1ee`).

- Each box stands for a **distance band**: box 0 = near, box 1 = middle, box 2 = far
  (assumed; fits the shotgun's damage falloff).
- In addition the **body zone** of the hit is found (height comparison, `FUN_8003e750`,
  masks at `0x8009ef14`).
- Of all enemies hit, **only the nearest** takes the damage.

Then (observed):

```c
entry = damage_table[enemy->type][weapon - 1];           // 20 bytes, see 2.
dmg   = (entry.w0 >> (box * 10)) & 0x3ff;
enemy->hp -= dmg;                                         // +0x156
enemy->previous_routine = enemy->routine;                 // +0x1fc
enemy->routine0 = (enemy->hp < 0) ? 3 /*death*/ : 2 /*hit*/;

zone_counter[zone] -= (entry.w1 >> (box * 3)) & 7;        // +0x153/+0x152/+0x151
if (zone_counter[zone] < 0) zone_counter[zone] = 0xFF;    // zone destroyed

enemy->hit_kind = box * 3 + zone;                         // +0x1d2
enemy->routine1 = weapon;                                 // +5: the reaction depends on the weapon
enemy->lock     = (entry.w1 >> (box * 7 + 9)) & 0x7f;     // +0x1d3, recoil / lock time
direction_flags = 0x20 / 0x40 / 0x80 by the angle player → enemy
```

---

## 2. Damage tables

Pointer table per enemy type: `0x8009ebe8[type]` (observed). Zombies:

| Enemy types                  | Table        |
|------------------------------|--------------|
| `0x10`–`0x14`, `0x18`–`0x1F` | `0x8009c470` |
| `0x15`–`0x17`                | `0x8009c5d8` |

Each entry: 20 bytes per weapon (weapon id 1–18):
`w0` = 3 × 10 bits of damage (near/middle/far), `w1` = 3 × 3 bits of zone damage + 3 × 7 bits
of lock time, then 6 × s16 (assumed: height / range limits of the zones).

### Zombie (standard, `0x8009c470`) – observed

| Id | Weapon             | Damage near / middle / far | Zone near/middle/far | Lock |
|---:|--------------------|----------------------------|----------------------|-----:|
| 1  | knife              | 3 / 0 / 0                  | 4 / 2 / 0            | 15   |
| 2  | H&K VP70           | 16 / 15 / 14               | 4 / 2 / 0            | 5    |
| 3  | Browning           | 16 / 15 / 14               | 4 / 2 / 0            | 5    |
| 4  | custom handgun     | 16 / 15 / 14               | 4 / 2 / 0            | 1    |
| 5  | magnum             | 900 / 900 / 900            | 2 / 1 / 0            | 15   |
| 6  | custom magnum      | 900 / 900 / 900            | 2 / 1 / 0            | 15   |
| 7  | shotgun            | **200 / 60 / 40**          | 2 / 1 / 0            | 15   |
| 8  | custom shotgun     | **300 / 80 / 60**          | 2 / 1 / 0            | 15   |
| 9  | grenade launcher   | 200 / 50 / 10              | 2 / 1 / 0            | 15   |
| 10 | GL flame rounds    | 200 / 50 / 5               | 2 / 1 / 0            | 15   |
| 11 | GL acid rounds     | 200 / 50 / 10              | 2 / 1 / 0            | 15   |
| 12 | bowgun             | 30 / 30 / 30               | 2 / 1 / 0            | 0    |
| 13 | Colt SAA           | 16 / 15 / 14               | 4 / 2 / 0            | 5    |
| 14 | spark shot         | 60 / 60 / 60               | 2 / 1 / 0            | 15   |
| 15 | SMG                | 4 / 4 / 4                  | 2 / 1 / 0            | 1    |
| 16 | flamethrower       | 15 / 15 / 15               | 2 / 1 / 0            | 5    |
| 17 | rocket launcher    | 900 / 900 / 900            | 2 / 1 / 0            | 15   |
| 18 | gatling            | 8 / 8 / 8                  | 2 / 1 / 0            | 2    |

### Zombie types `0x15`–`0x17` (`0x8009c5d8`) – differences

These types are much tougher against handguns and shotguns:

| Weapon               | Damage near / middle / far |
|----------------------|----------------------------|
| handguns, Colt       | 11 / 10 / 9                |
| shotgun              | 60 / 40 / 30               |
| custom shotgun       | 80 / 60 / 50               |
| grenade / flame      | 80 / 50 / 10 (or 5)        |
| bowgun               | 25                         |
| spark shot           | 41                         |
| SMG                  | 3                          |
| flamethrower         | 12                         |
| gatling              | 6                          |

Magnum, rocket launcher and GL acid stay the same.

### Shots to kill (standard zombie, derived)

Zombie HP 50–128, mostly about 80 (see [enemy-zombie.md](enemy-zombie.md)); a hit kills as
soon as the HP drop **below 0**:

| Weapon                | 50 HP | 80 HP | 128 HP |
|-----------------------|------:|------:|-------:|
| handgun (near, 16)    | 4     | 6     | 9      |
| shotgun near          | 1     | 1     | 1      |
| shotgun far           | 2     | 3     | 4      |
| SMG                   | 13    | 21    | 33     |
| magnum                | 1     | 1     | 1      |

---

## 3. The zombie's hit reaction (`0x80104e80`, observed)

1. **Let go of the player:** if exactly this zombie holds the player, the player is released
   at once (player routine back to 1, grab flag off).
2. **Lying zombie** (`+0x21a & 2`): state 5/6, plus effects per weapon:
   - GL flame (10): the zombie burns (`FUN_80106068`, flag `0x800`),
   - GL acid (11): `FUN_80106250`,
   - grenade (9): `FUN_8010634c`,
   - spark shot (14): `FUN_80106450`,
   - flamethrower (16): burns from the 9th hit on.
   Burning is the HP loss every 8 frames from the walking routine.
3. **Falling over:** a counter `+0x223` (start 16–31, for a third of the zombies 32–47)
   decides whether a hit knocks the zombie down (then state 5). If the counter is 0 (or ≤ 23
   with flag `0x40` in `+0x10e` and not the knife), the first such hit only sets a mark
   (`+0x222 = 1`); the **next** one knocks it down. With the **knife** it falls on the first.
   Who counts the counter down is still open.
4. **Shooting off an arm:** if zone 1 (`+0x152`) is destroyed and the hit came from the side
   (`0x40`/`0x80`), an arm falls off: flag `0x20` (one side) or `0x40` (the other side),
   sound 9, the severed part as an effect. **Without that arm the zombie cannot grab on that
   side any more.** The chase state checks this, observed.
5. Then a jump into the actual hit animation:
   `0x8010c764[weapon*9 + hit_kind]` (standing) or `0x8010ca0c[weapon]` (crawler).

### Body zones

Derived from the model parts that fall off when the zone is hit (see 4.):

| Zone | Counter  | Start | Meaning                                                 |
|------|----------|------:|---------------------------------------------------------|
| 0    | `+0x153` | 13    | **legs** (parts 3, 5, 6 fly off → crawler)              |
| 1    | `+0x152` | 13    | **upper body**: arms (parts 9/12) and head (part 8)     |
| 2    | `+0x151` | 13    | cannot be hit on zombies (mask bit missing)             |

With a handgun from near (4 per hit) a zone is destroyed after **4 hits**, with the shotgun
(2) after 7.

### Hit lock

`+0x1d3` gets the "lock" value from the table. While it is not 0, the hit check ignores this
enemy (observed). Rapid-fire weapons therefore have low values (SMG 1, gatling 2), single
shots higher ones (handgun 5, shotgun 15). It drops by 1 per frame in the enemy's entry
routine (observed for the dog and the crow, see [enemy-others.md](enemy-others.md)).

---

## 4. Hit animations, death and head shots

### Dispatch (observed)

Both routines (hit `0x80104e80`, death `0x80108190`) end in a jump through a table
`[weapon * 9 + box * 3 + zone]`:

- hit, standing: `0x8010c764` – crawler: `0x8010ca0c[weapon]` (always `0x801077c8`)
- death, standing: `0x8010ca48` – crawler: `0x8010ccf0[weapon]` (always `0x80108954`)

The death routine also counts every killed enemy once in `0x800cc2bc` (assumed: statistics
for the final ranking) and releases a grabbed player.

### Important handlers

| Address      | Effect (observed)                                                                       |
|--------------|------------------------------------------------------------------------------------------|
| `0x80105378` | normal flinch                                                                            |
| `0x80107e30` | flinch under continuous fire (SMG, gatling)                                              |
| `0x80108470` | **normal death**: falls forward / backward, blood. **25 %**: does not get up again but becomes a **crawler with 1 HP** (only if the legs are intact, on the ground, not headless) |
| `0x80107378` | **legs off**: 50 % leg part 3 flies off (then another 50 % 5 and 6), a blood fountain. Then a **crawler** (with 10 HP after a deadly hit, except 25 % if the legs were off already → dead) |
| `0x80109204` | **head bursts**: part 8 flies away from the player, blood, splatter sound. **50 %**: the zombie walks on headless for 30–93 frames (1–3 s), then dies |
| `0x80108b2c` | **upper body torn**: head, left arm and with 50 % the right arm fly off; then perhaps briefly on headless (15–46 frames) |
| `0x8010663c` | **shotgun point blank into the upper body**: the zombie is thrown over and crawls on (crawler, 10 HP after a deadly hit) |
| `0x80109550` | **thrown backward** (impulse 400, −50 per frame); with the bowgun the bolt stays in the part hit |

### When does the head burst? (observed)

Only with strong weapons on the **upper body (zone 1)**:

| Weapon                    | Condition                                                         |
|---------------------------|-------------------------------------------------------------------|
| magnum, custom magnum     | **always** (every hit is deadly, death handler `0x80109204`)      |
| shotgun                   | near (box 0) **and** the aim flag "high" set → 100 %, even if the HP would last |
| custom shotgun            | near: aim flag "high" **or** 25 % chance → upper body torn (with the head) |
| rocket launcher           | near: **always** upper body torn (with the head)                  |
| grenade launcher (grenade)| near, zone 1/2: like the custom shotgun, but only a 1/4 chance    |
| handguns, SMG, knife, …   | **never** – only the normal death `0x80108470`                    |

Hits on the **legs (zone 0)** lead to `0x80107378` even with the magnum, rocket launcher and
shotgun. The zombie then mostly survives as a **crawler with 10 HP** although the damage was
deadly.

"Aim flag high" = the top bit of the player flag word (`0x800c77f4 < 0`). `FUN_8003ef98` sets
it on the shot when the weapon hand is above the enemy's target window (assumed: the player
aims up).

### Zombie model parts (derived)

From the parts that fall off and the clothing variants in the initialisation:

| Part     | Meaning (derived)                  |
|----------|------------------------------------|
| 3, 5, 6  | legs (lower / upper leg)           |
| 8        | head                               |
| 9–11     | one arm (upper arm, forearm, hand) |
| 12–14    | the other arm                      |

Parts 8, 11 and 14 (head and hands) get their own skin model for type `0x12`, the others the
clothing, which fits this assignment.

### More details

- A crawler that grabs the player's leg loses its head (part 8) in frame 22 of the kick-off
  animation – the well-known head stomp (observed in state 3, sub state 5).
- Lying zombies (`+0x21a & 2`) die without an animation, with a fire / acid / grenade effect
  depending on the weapon.

---

## 5. For the DS port

- The damage tables can be taken over 1:1 as C arrays. A converter can read them straight
  from `SLES_009.72` (file offset = address − `0x80010000` + `0x800`).
- The hit check only needs 2D boxes along the facing plus a height comparison, which is
  cheap on the DS.
- Instead of the original's line-of-sight test, a ray against the room's collision boxes is
  enough to start with.

---

## 6. Open points

- The exact shape of the test boxes (table `0x8009ea60`) and of the zone limits.
- Who counts down `+0x223` (the fall counter).
- Meaning of `+0x10c` (condition "== 0" for crawling on; assumed: height above the floor).
- Cross-check the model part assignment with the EMD skeleton (e.g. with OpenBiohazard2's
  `modeldumper`).
- `0x800c77ef`: used here as the condition "enemies active in the room" (without it no hits,
  no aiming). In [enemy-zombie.md](enemy-zombie.md) it gives +15 HP if `< 4`. Assumed: the
  number of active enemies, so zombies in empty rooms would be tougher. Still to check.

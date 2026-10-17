# RE2 (PS1, SLES-00972) – player control

Source: `SLES_009.72` from disc 1 (Europe), build "CAPCOM-Mar 14 1998", analysed with
REA + Ghidra 12.1.4 (MIPS:LE:32). All addresses are PS1 RAM addresses.
Function names like `pl_ctrl_01_walk` are ours (see `tools/research/re2_symbols.txt`).

Legend: **observed** = in the decompiled code as written. **assumed** = interpretation, not
cross-checked yet (e.g. in an emulator).

---

## 1. Input → action bits

`pad_read_and_map` (`0x8003673c`) runs once per frame:

- The raw pad data is inverted (PS1 pads report pressed = 0).
- A table (`0x8009aa0c + control type * 0x20`, 16 entries of 16 bits) maps the buttons to
  **16 action bits**. The control type is in `0x800cc2ae` (options menu).
- Result:
  - `0x800c5f4c` = actions **held**
  - `0x800c5f50` = actions **newly pressed** (edge)
  - `0x800c5f3c` / `0x800c5f40` = raw buttons held / newly pressed
- Demo mode: with flag `0x40000000` in `0x800c77b4` the buttons come from a recording
  (`0x800d6e7e`).

Action bits (observed from their use):

| Bit      | Meaning                                                     |
|----------|-------------------------------------------------------------|
| `0x001`  | forward                                                     |
| `0x002`  | turn right (angle +)                                        |
| `0x004`  | backward                                                    |
| `0x008`  | turn left (angle −)                                         |
| `0x010`  | aim up/down (assumed: one direction)                        |
| `0x020`  | aim up/down (assumed: the other direction)                  |
| `0x040`  | fire                                                        |
| `0x080`  | action / examine                                            |
| `0x100`  | draw / aim weapon (held)                                    |
| `0x200`  | run (held)                                                  |
| `0x1000` | confirm (used in terminals)                                 |

**DS port:** only the mapping is replaced: D-pad = 1/2/4/8, B = run, R = aim,
A = fire / action. Everything else works with the action bits.

---

## 2. Player object (excerpt)

The player object is a typical Capcom "work" structure. Observed fields:

| Offset   | Type | Meaning                                                            |
|----------|------|--------------------------------------------------------------------|
| `+0x04`  | u8   | routine 0 (1 = normal control)                                     |
| `+0x05`  | u8   | **routine 1 = player state** (index into the tables below)         |
| `+0x06`  | u8   | routine 2 = sub state (when aiming: weapon phase)                  |
| `+0x07`  | u8   | routine 3 = init flag / step within the sub state                  |
| `+0x76`  | s16  | **facing Y** (4096 = 360°)                                         |
| `+0x10c` | s16  | current animation frame (assumed)                                  |
| `+0x10e` | u16  | **equipped weapon** (`& 0xfff`, 0 = none)                          |
| `+0x14c` | u32  | animation selection: `0x70000 + animation number` (7 = blend ticks)|
| `+0x14d` | u8   | keyframe counter of the running animation                          |
| `+0x151` | u8   | **health level** 0 = Fine, 1 = Caution, 2 = Danger                 |
| `+0x154` | u16  | aim posture in bits 13–15 (`0x2000`, `0x4000` = straight, `0x8000`)|
| `+0x156` | s16  | **hit points**                                                     |
| `+0x178` | ptr  | the current keyframe's EDD entry (frame \| flags << 12)            |
| `+0x1b4` | ptr  | current target (enemy) of the auto-aim                             |

Health level (observed, recomputed in every state):

```c
level = 0;
if (hp < 101 || poisoned) level = 1;   // Caution (poison flag: 0x800c7a0c)
if (hp < 21)              level = 2;   // Danger
```

---

## 3. State machine

`player_control_dispatch` (`0x800396e4`) calls two tables per frame, indexed with `+0x05`:

1. **Control** `0x8009c398[state](obj, held, pressed)` – reads the buttons, picks the next
   state and turns the character.
2. **Movement** `0x8009c3c8[state](obj, ...)` – plays the animation, moves the character,
   checks collision.

A state change writes `*(u32*)(obj+4) = 0x0001 | (state << 8)`, i.e. routine 0 = 1,
routine 1 = new state, routines 2 and 3 = 0.

| State   | Control      | Movement     | Meaning                          |
|---------|--------------|--------------|----------------------------------|
| 0       | `0x80039760` | `0x800398e8` | standing                         |
| 1       | `0x800399a4` | `0x80039bc8` | walking                          |
| 2       | `0x80039e10` | `0x8003a02c` | running                          |
| 3       | `0x8003a324` | `0x8003a4a4` | walking backward                 |
| 4       | `0x8003abf8` | `0x8003adfc` | turning on the spot              |
| 5       | `0x8003f790` | `0x80040800` | **aiming / firing** (see 5.)     |
| 6–9, B  | …            | …            | not analysed yet                 |
| A (10)  | –            | –            | set from "walking" with flag `0x2000` in `0x800c77d4` (assumed: stairs / scene) |

Transitions (observed, the same in states 0–4):

```c
if (held & (2|8))  state = 4;            // turn
if (held & 4)      state = 3;            // backward
if (held & 1) {    state = 1;            // walk
    if (held & 0x200) state = 2;         // run
}
if ((held | pressed) & 0x80) examine();  // FUN_80035148 or FUN_8004bc44 (special case)
if ((held & 0x100) && weapon != 0) state = 5;  // aim
// no matching button → back to 0 (standing)
```

Running only lasts while **forward and run** are held (`(held & 0x201) == 0x201`).
While running, turning only starts once `+0x144 > 50` (assumed: run-up).

---

## 4. Movement and turning

### Turn speed per frame (observed, unit 4096 = 360°)

| State              | Fine | Caution | Danger | Table        |
|--------------------|------|---------|--------|--------------|
| walking            | 40   | 32      | 16     | `0x80010bc8` |
| running            | 56   | 48      | 24     | `0x80010bd4` |
| backward           | 40   | 16      | 16     | `0x80010bdc` |
| on the spot        | 80   | 48      | 32     | `0x80010be4` |
| raising the weapon | 32   | 32      | 32     | constant     |
| aiming             | 64   | 64      | 64     | constant     |
| while firing       | 8    | 8       | 8      | constant     |

**Game tick:** the main loop waits at the end of a frame with `VSync(DAT_800d76ca)`
(`0x8002ada8`); `0x80013c94` sets the value to **2** in game. One game frame = 2 vblanks, i.e.
**25 Hz on PAL** (SLES-00972) and 30 Hz on NTSC. In menus the value is 0 (full frame rate;
text timers are doubled then, `0x8002f670`). All "per frame" values here are per game frame.

On PAL, Leon turns on the spot at 80/4096 × 360° × 25 ≈ **176°/s** (NTSC: 211°/s).

### Moving forward

`pl_move_01_walk` (`0x80039bc8`) uses no fixed speed. Instead:

- It picks the animation `0 / 3 / 6` by health level (table `0x80010bcc`): normal walk,
  limp, heavy limp.
- `FUN_8001581c` plays the keyframe and **takes the displacement from the animation data**
  (root motion).
- `FUN_800289d4(obj, …, 0x200)` checks collision with the room geometry.
- Footstep sound when the frame flag `0x4000` is set (`FUN_8005797c`).

**DS port:** the walking speed lives in the animation data of the PLD file
(Leon = `PL0/PLD/PL00.PLD`). The DS engine applies the displacement per keyframe.

### Pose blending

Every animation change (`obj+0x14c = 0x70000 | anim`, counter 7 in byte `0x14e`) eases from
the shown pose into the new one (`FUN_80028a4c`): per game tick
`shown = new + (shown - new) * counter / 8`, angles the short way round, root offset too,
then counter − 1. The DS engine does this for Leon and for all enemies / people.

---

## 5. Aiming and firing

In state 5 the control routine is empty. The logic is in `pl_move_05_aim` (`0x80040800`):

```c
handler = weapon_table_0x8009f01c[weapon];   // per weapon
handler(obj, ..., held);
```

Weapon table (handlers observed; weapon names after the known RE2 item ids. This fits the
code: the special cases for ids 9–11 are the three grenade launcher ammunition types):

| Weapon id                 | Handler      | Weapon                                      |
|---------------------------|--------------|---------------------------------------------|
| 1                         | `0x8003f798` | knife                                       |
| 2, 3, 5–13, 17, 19        | `0x8003fd44` | standard: handguns, magnum, shotgun, grenade launcher, bowgun, Colt SAA, rocket launcher, Beretta |
| 4                         | `0x80044ff0` | custom handgun (burst)                      |
| 14 (`0x0E`)               | `0x80044b7c` | spark shot                                  |
| 15, 16 (`0x0F`, `0x10`)   | `0x800442cc` | SMG and flamethrower (continuous)           |
| 18 (`0x12`)               | `0x8004539c` | gatling                                     |

Knife and guns dispatch further on sub state `+0x06`
(tables `0x8009f10c` knife, `0x8009f128` guns):

| Phase | Handler                            | Meaning                                    |
|-------|------------------------------------|--------------------------------------------|
| 0     | `0x8003fd80`                       | raise the weapon, find a target            |
| 1     | `0x80040128`                       | **aim** (held), pick the height, fire      |
| 2     | `0x800403ec` / knife `0x8003f7d4`  | **shot** or knife slash                    |
| 3     | `0x800421e8`                       | lower the weapon (assumed)                 |
| 4     | `0x8004237c`                       | **reload** (observed, see below)           |
| 5     | `0x800424d0`                       | unknown (triggered by raw button `0x4`)    |

### Phase 0 – raising the weapon (`0x8003fd80`)

- Animation 9, posture "straight" (`0x4000`).
- `FUN_8003ed74(obj, range)` picks the auto-aim target: the nearest enemy with hit points
  ≥ 0 that is in sight. Enemies whose status (`obj+0x10e`) has bit `0x2000` – set while a
  zombie lies on the ground or crawls – are only chosen if no other one is in sight. Range 0
  for guns, 5000 for the knife (30000 in two particular rooms).
- With a target (`+0x1b4`) `FUN_80014ef8` turns the player towards it (rate `0xd8`,
  weapon 13: `0x120`).
- Newly pressed `0x20` → search again with range 3000.
- After a minimum number of frames (table `0x8009eed8`, 3 bytes per weapon) `0x10`/`0x20`
  switch directly to the up / down posture.

### Phase 1 – aiming (`0x80040128`)

```c
if (!(held & 0x100)) { phase = 3; return; }        // released → lower
if (held & 0x10 && posture != 0x8000) posture = 0x8000;
if (held & 0x20 && posture != 0x2000) posture = 0x2000;
if (!(held & 0x30) && posture != 0x4000) posture = 0x4000;
if (held & 0x40) {                                   // fire
    if (rounds_loaded(weapon)) { phase = 2; return; }            // FUN_8006541c
    if (pressed & 0x40) {
        if (!spare_ammo() || weapon in 9..11)                    // FUN_80065708
            FUN_80057364(0x1010001, pos);   // weapon bank (ARMS) sound 1: the click
        else phase = 4;                                          // reload
    }
}
turn(±64);
collision();
```

For weapon 17 (`0x11`, rocket launcher) aiming up/down is locked; when raising, this also
applies to weapon 14 (`0x0E`). The grenade launcher ids 9–11 never reload automatically,
they only click.

### Phase 2 – the shot (`0x800403ec`)

On the first frame:
- animation by posture (table `0x80010d04`),
- the weapon's start handler from `0x8009f070[weapon]`,
- `FUN_8003ef98` (assumed: damage) and `FUN_8003dd58` (resets the hit-check sequence).

Per frame:
- `FUN_8003dd8c` – the **hit check** (observed): walks all enemies, tests the shot ray
  against their body parts, picks the nearest hit, subtracts damage from the tables at
  `0x8009ebe8` and sets the hit direction bits. Not run for weapons 9–12.
- Turning ±8.
- Weapons 6 and 8 (custom magnum and custom shotgun) take recoil from the animation
  (`FUN_8001581c`): the character slides back.
- With aim released and the recoil time over (table `0x8009eeda`) → phase 1.
- Then the weapon's frame handler from `0x8009f0bc[weapon]`. For the VP70 (`0x80040b94`)
  it spawns effects with `FUN_8001b8fc(effect id << 24 | sub << 16 | ..., dir, bone matrix,
  offset)` on the hand bone (matrix `obj+0x198 + 0x7ac`):
  - keyframe 1: `0x030a0e00` and `0x01000e00` at (138, 615, 8) – muzzle flash and fire
    (CORE00.ESP effects 3 and 1),
  - keyframe 3: `0x03080900` twice at (350, 40, -15) – smoke, and `0x02000700` with the
    facing + 1300 – the casing (effect 2).

### Phase 4 – reloading (`0x8004237c`)

- Animation `0x70010` = PLW move 16.
- On each keyframe the EDD flags of the entry at `obj+0x178` are tested:
  - `0x100000`: play weapon bank sound `(flags & 0xf0000) >> 16`,
  - `0x200000` (and `+0x154` bit 1): drop the magazine (`FUN_8003f050`),
  - `0x400000`: load the magazine (`FUN_80065780`: moves rounds from the ammunition item
    into the weapon, up to its capacity; an empty ammunition item is removed).
- At the end of the animation → phase 1.

VP70 (`PL00W02.PLW` move 16, 32 keyframes): key 7 sound 4 + magazine, key 22 sound 3 + load.
Custom handgun (`PL00W04.PLW`): key 7 sound 3 + magazine, key 23 sound 3 + load.

---

## 6. Open points

- Which of the actions `0x10`/`0x20` is "up" and which "down": check in an emulator.
- States 6–11 (probably stairs, pushing, damage, death, grabbed).
- Contents of the table `0x8009eed8` (frames per weapon) and the animation tables.
- The R3000 has a one-instruction load delay that Ghidra does not model. Cross-check the
  assembly where it matters.

---

## Tools

- `tools/extract_psx_iso.py` – extracts the file system from the `.bin`.
- `tools/research/psxexe_to_elf.py` – wraps `SLES_009.72` as a MIPS ELF for REA/Ghidra,
  optionally with `tools/research/re2_symbols.txt` for function names, and with overlays
  (`--overlay FILE:OFFSET:SIZE:ADDR`, e.g. `COMMON/BIN/DIEDEMO.BIN` at `0x80190000`).

```bash
python3 -I tools/extract_psx_iso.py "<Track 1>.bin" /path/to/output
python3 -I tools/research/psxexe_to_elf.py /path/to/output/SLES_009.72 sles.elf tools/research/re2_symbols.txt
```

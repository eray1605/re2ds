# re2ds – Resident Evil 2 auf dem Nintendo DS (Prototyp)

Ziel des ersten Meilensteins: **ein Raum** (Stage 1, Raum 00 – die Straße am Anfang) mit
vorgerendertem Hintergrund, Leon als 3D-Modell und Tank-Steuerung.

Die Spieldaten kommen aus der eigenen PS1-Disc und werden **nicht** mitgeliefert. Alles unter
`build/` ist aus der Disc erzeugt und wird nicht eingecheckt.

Analyse-Notizen zur Original-Engine: [../re2ds-notes](../re2ds-notes).

## Konverter (Stand)

| Werkzeug | Eingabe | Ausgabe | Status |
|----------|---------|---------|--------|
| `tools/bss2ppm` (C, nutzt FFmpegs MDEC-Decoder) | `COMMON/BSS/ROOMsrr.BSS` | 320×240-Hintergrund pro Kamera | ✔ funktioniert, Raum 100: 9 Kameras |
| `tools/rdt2json.py` | `PL0/RDT/ROOMsrrp.RDT` | Kameras, Kamerawechsel, Kollision als JSON | ✔ Raum 100: 9 Kameras, 25 Wechselzonen, 21 Kollisionsboxen |
| `tools/check_projection.py` | JSON + Hintergründe | Kollisionsboxen in die Hintergründe projiziert | ✔ Kamera-Konvention bestätigt |
| `tools/pld2json.py` | `PL0/PLD/PL00.PLD` (+ `--plw PL00W00.PLW`) | Mesh, Skelett, Animationen als JSON, Textur als PNG | ✔ Leon: 15 Knochen, 187 Dreiecke + 267 Vierecke, 10 + 17 Animationen |
| `tools/preview_model.py`, `tools/preview_anims.py` | JSON + Textur | Vorschau (vorn/seitlich), Animations-Übersicht | ✔ |
| `tools/bg2ds.py` | `bss2ppm`-Ausgabe | `build/ds/bg/<raum>_camNN.bin`: 256×192 BGR555 (96 KB) | ✔ |
| `tools/room2ds.py` | `rdt2json`-Ausgabe | `build/ds/room/<raum>.bin`: Kameras, Zonen (mit Sichtbereich-Flag), Kollision, Startpunkt | ✔ Raum 100: 1200 Byte |
| `tools/model2ds.py` | `pld2json`-Ausgabe + PLD | `build/ds/model/leon.bin` (Display Lists, Skelett, Keyframes, Animationen), `leon_tex.bin` (3 × 128×256, 8 Bit) | ✔ 88 KB + 97 KB |
| `tools/bssmask.py` | `COMMON/BSS/ROOMsrr.BSS` | Masken-Atlas pro Kamera (TIM + PNG) | ✔ Raum 100: 7 von 9 Kameras, Größen stimmen exakt |
| `tools/mask2ds.py` | `rdt2json`-Ausgabe + Masken-TIMs | `build/ds/mask/<raum>_camNN.bin`: Atlas (256×2ⁿ, 8 Bit, Index 0 transparent) + Welt-Vierecke | ✔ 238 Rechtecke |
| `tools/tim2png.py` | beliebige Datei | TIM-Bilder als PNG | ✔ |
| `tools/ds_preview.py` | **nur** `build/ds/` | DS-Bild 256×192 mit Hintergrund, Leon und Masken | ✔ prüft die Formate |

Die DS-Formate sind in [source/formats.h](source/formats.h) beschrieben (gilt für Konverter und Engine).

## Erkenntnisse

- **Hintergründe und Masken:** Jeder 64-KB-Block eines `.BSS` gehört zu einer Kamera
  (gefunden über den Hintergrund-Lader `0x8002b240` in `SLES_009.72`):
  ```
  0x0000   MDEC-Bild „BS v3“, 320×240 (Hintergrund)
  h        u32 entpackte Größe, u16 0xFFFF, LZ-Datenstrom  → Masken-TIM (8 Bit, 256×N)
  o − 8    u32 h        (Zeiger auf den Masken-Block, liest der Lader)
  o − 4    u32 ≠ 0      (Kamera hat Masken)
  ```
  Entpacker (handgeschriebener Assembler, `0x80010000`–`0x800104b0`, als Zustandsmaschine
  über 1-KB-Stücke im Scratchpad):
  ```
  c = Byte;  c == 0xFF            → Ende
  c & 0x10 == 0  → Kopie:   b = Byte; Abstand = ((c >> 5) << 8 | b) − 2048
                            Länge = (c & 0x0F) + 3   (bei 0x0F: + nächstes Byte)
  sonst          → Literale: Anzahl = 32 − (c & 0x1F)  (bei 16: + nächstes Byte)
  ```
  Der Masken-Atlas enthält ausgeschnittene Stücke des Hintergrunds; PS1-Farbe `0x0000` ist
  transparent. Das Längenfeld dieser TIMs zählt nur die Pixel (ohne 12-Byte-Kopf).
  Die Masken-Rechtecke stehen im RDT (PRI-Liste pro Kamera, `mask_offset`); Tiefe × 32 =
  Abstand entlang der Blickrichtung (wie OpenBiohazard2 `render/camera.go`).
- **Kamera-Konvention** (verifiziert mit Raum 100, Kamera 2):
  ```
  forward = normalize(to - from)
  right   = normalize(cross(forward, (0, -1, 0)))   // RE2: -Y ist oben
  down    = cross(forward, right)
  screen  = (RID.distance >> 7)                     // ≈ 207,8 → 60° vertikal
  sx = 160 + screen * dot(p - from, right) / dot(p - from, forward)
  sy = 120 + screen * dot(p - from, down)  / dot(p - from, forward)
  ```
  Für den DS (256×192) einfach mit 0,8 skalieren: `screen * 0.8`, Mitte (128, 96).
- **Leon (`PL00.PLD`)**: gleicher Aufbau wie am PC – EDD-Animationen, EMR-Skelett,
  MD1-Mesh (15 Körperteile, je Dreiecke + Vierecke), TIM 8 Bit 384×256 mit 3 Paletten.
  Die Textur enthält drei 128 px breite Seiten; Seite 3 sind die blutigen Varianten.
  Vierecke werden als Streifen `(0,1,2) + (1,3,2)` gezeichnet.
- **Skelett** (Knochen → Körperteil, aus Hierarchie und Positionen abgeleitet):
  0 Rumpf, 1 Hüfte, 2–4 rechtes Bein (Ober-/Unterschenkel, Fuß), 5–7 linkes Bein,
  8 Kopf, 9–11 rechter Arm (Ober-/Unterarm, Hand), 12–14 linker Arm.
  Pose: `M = M_parent · T(rel_pos) · Rx · Ry · Rz`, Winkel 12 Bit (4096 = 360°).
- **Animationen**: Die PLD enthält nur Sonderbewegungen (Sterben, Wegstoßen, Stolpern,
  Aufheben, Gepackt-Werden …). Laufen/Rennen/Stehen stehen in der **Waffen-Datei `.PLW`**
  (Animationsbank 7 in der Spielersteuerung). `PL00W00.PLW` (ohne Waffe):

  | PLW-Anim | Keyframes | Bedeutung |
  |---------:|----------:|-----------|
  | 0 / 3 / 6 | 30 / 34 / 45 | Gehen – Fine / Caution / Danger |
  | 1 / 4 / 7 | 20 / 20 / 22 | Rennen – Fine / Caution / Danger |
  | 2 / 5 / 8 | 50 / 30 / 30 | Stehen – Fine / Caution / Danger |
  | 9  | 10 | Waffe heben (`0x70009` im Code) |
  | 10–15 | 23–24 bzw. 1 | Zielen/Schießen hoch, gerade, runter (je Bewegung + Haltepose) |
  | 16 | 32 | Waffe senken (vermutet) |

  Das passt genau zu `player-control.md`: Gehen wählt Animation `0x70000 + {0,3,6}` je nach
  Gesundheit.

## Bauen und ausführen (Konverter)

```bash
clang -O2 -o tools/bss2ppm tools/bss2ppm.c -I/opt/homebrew/include -L/opt/homebrew/lib -lavcodec -lavutil -lswscale
./tools/bss2ppm <disc>/COMMON/BSS/ROOM100.BSS build/bg/room100
python3 -I tools/rdt2json.py <disc>/PL0/RDT/ROOM1000.RDT build/room/room1000.json
python3 -I tools/check_projection.py build/room/room1000.json build/bg/room100 build/check/room100
python3 -I tools/pld2json.py <disc>/PL0/PLD/PL00.PLD build/leon/leon --plw <disc>/PL0/PLD/PL00W00.PLW
python3 -I tools/preview_model.py build/leon/leon.json build/leon/leon_tex.png build/leon/leon.png 10 0

# DS-Formate
python3 -I tools/bg2ds.py build/bg/room100 build/ds/bg build/ds/preview
python3 -I tools/room2ds.py build/room/room1000.json build/ds/room/room1000.bin 2
python3 -I tools/model2ds.py build/leon/leon.json <disc>/PL0/PLD/PL00.PLD build/ds/model/leon
python3 -I tools/bssmask.py <disc>/COMMON/BSS/ROOM100.BSS build/mask/room100
python3 -I tools/mask2ds.py build/room/room1000.json build/mask/room100 build/ds/mask/room100
python3 -I tools/ds_preview.py build/ds room1000 room100 leon build/ds/preview/frame.png 10 0
```

### Konventionen (in `ds_preview.py` geprüft)

- Weltkoordinaten = RE2-Spieleinheiten = DS-Festkomma 20.12 (4096 Einheiten = 1,0).
  Leons Knochen-Vertices passen direkt in `VTX_16`.
- Modelle schauen nach **+X** (Gehen bewegt die Wurzel um ~80 Einheiten/Frame entlang +X).
  Drehung um Y wie PS1 `RotMatrixY`: Blickrichtung `(cos a, 0, −sin a)`.
- Knochen-Pose: `M = M_parent · T(pos) · Rx · Ry · Rz`; die Wurzel nimmt die
  Position aus dem Keyframe (`offset`, Hüfthöhe ≈ −1800).
- DS-Projektion: Bildschirmabstand = `screen · 256/320`, Mitte (128, 96); Sichtfeld wie
  am PS1 (60° vertikal), Seitenverhältnis 4:3 bleibt.

`<disc>` ist das mit `../re2ds-notes/tools/extract_psx_iso.py` entpackte Disc-Dateisystem.

## DS-Engine (erster Stand, noch nicht kompiliert)

`source/main.c` + `source/assets.c`, Makefile nach devkitPro-Vorlage (libnds, NitroFS):

- **Bildschirm oben:** BG3 = 16-Bit-Hintergrund der aktuellen Kamera, BG0 = 3D-Ebene mit
  transparentem Hintergrund (Leon + Masken-Vierecke, Tiefentest durch die Hardware).
- **VRAM:** A = Haupt-BG, B + D = Texturen (Leon 96 KB, Masken ≤ 64 KB), E = Textur-Paletten,
  C = Textkonsole unten. Texturbänke nur in ihren Standard-Slots (B = 1, D = 3): libnds
  berechnet die Texturadresse aus der Bank-Nummer, andere Zuordnungen ergeben schwarze bzw.
  unsichtbare Texturen.
- Texturdaten vor `glTexImage2D`/`glColorTableEXT` mit `DC_FlushRange` aus dem Cache schreiben
  (die Uploads laufen per DMA).
- **Steuerung:** Steuerkreuz hoch/runter = gehen/rückwärts, links/rechts = drehen, B = rennen.
  Drehgeschwindigkeiten aus `player-control.md`, Fortbewegung aus der Root-Motion der Animation.
- **Kollision:** Kreis (Radius 400) gegen die Boxen des Raums, getrennt nach X und Z (Gleiten).
- **Kamerawechsel:** Zonen der aktuellen Kamera ohne Sichtbereich-Flag; beim Wechsel werden
  Hintergrund und Masken neu geladen.

```bash
tools/build_assets.sh <disc> 1 00 2   # füllt nitrofiles/ (Raum 100, Start in Kamera 2)
make                                  # braucht devkitPro (DEVKITARM gesetzt) → re2ds.nds
```

## Nächste Schritte

1. ~~PLD-Konverter für Leon~~ ✔. Offen: Waffen-Meshes aus der PLW (ersetzen Hand/Arm).
2. ~~DS-Formate erzeugen~~ ✔, inklusive Masken: Auf dem DS werden sie als texturierte
   Vierecke in der richtigen Tiefe gezeichnet, der Z-Puffer erledigt die Verdeckung.
3. DS-Toolchain (devkitPro) und Emulator (melonDS) einrichten.
4. DS-Engine: Hintergrund als Bitmap-BG, Leon auf der 3D-Ebene, Steuerung nach
   `../re2ds-notes/player-control.md`, Kollision und Kamerawechsel.

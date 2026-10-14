# re2ds – Roadmap zum kompletten Spiel

Ziel: Resident Evil 2 (PS1, PAL; Disc 1 = SLES-00972) komplett als native DS-ROM – beide
Charaktere, beide Szenarien, mit Menüs, Ton und Videos. Die ROM wird immer lokal aus den
eigenen Discs gebaut und **nie weitergegeben** (sie enthält Capcom-Daten).

Aufwand pro Phase: S (Tage), M (1–3 Wochen), L (1–2 Monate), XL (mehrere Monate).

## Stand heute (Phase 0 – erledigt)

- Analyse mit REA/Ghidra: Spielersteuerung, Zombie-KI, Treffer/Schaden, Tod/Kopfschüsse,
  Hund/Krähe/Licker (`../re2ds-notes/`).
- Konverter: BSS-Hintergründe (MDEC), Masken (LZ + TIM), RDT-Raumdaten, Leon (PLD/PLW).
- DS-Engine: Raum 100 mit Hintergrund, Masken-Verdeckung über den Tiefenpuffer, Leon mit
  Texturen und Animation, Gehen/Rennen/Drehen, Kollision, Kamerawechsel.

## Was auf der Disc steckt (Disc 1, Leon)

| Inhalt | Dateien | Größe PS1 | Bemerkung |
|---|---|---|---|
| Räume (RDT) | 125 in 7 Stages | 20 MB | Kameras, Kollision, Skripte, Texte, Masken-Infos |
| Hintergründe (BSS) | 128 Dateien, 1285 Kamera-Slots | 80 MB | je 320×240 MDEC |
| Videos (STR) | 15 | 380 MB | ca. 10 min Laufzeit |
| Sprachausgabe (XA) | 8 | 41 MB | |
| Musik (BGM) | 108 | 13 MB | Sequenzen + Samples |
| Soundeffekte (EDH/VB) | 28 Paare | 1,6 MB | VAB-ähnlich |
| Gegner (EMS) | 2 Pakete | 13 MB | Modelle, Animationen, KI-Overlays |
| Türanimationen (DO2) | 55 | 3,4 MB | |
| Menügrafiken (TIM/PIX) | ~50 | 11 MB | Inventar, Items, Karten, Files |

Disc 2 (Claire) hat denselben Aufbau.

## Grenzen des DS und was daraus folgt

- **ROM max. 512 MB.** Grobe Schätzung pro Charakter: Hintergründe ~40 MB (8 Bit mit
  Palette + LZ statt heute 96 KB roh pro Kamera), Räume/Modelle ~35 MB, Ton ~30 MB,
  Videos ~80 MB → zusammen ~380 MB. Das passt, aber **die Videos sind der Hebel**: sie
  müssen auf ~128 KB/s.
- **4 MB Hauptspeicher.** Immer nur einen Raum, die Modelle darin und die aktuelle Kamera
  laden; Hintergründe pro Kamerawechsel aus NitroFS streamen.
- **3D: max. 2048 Polygone pro Bild.** Leon allein hat 454 (187 Dreiecke + 267 Vierecke;
  der DS zählt ein Viereck als ein Polygon), Masken kommen dazu. Bei 4–5 Zombies im Bild
  wird es eng → Gegner-Modelle vereinfachen (LOD) und die Polygonzahl messen.
- **Textur-Speicher heute 256 KB** (Bänke B + D; A hält den Hintergrund, C die Textkonsole).
  Leon belegt 96 KB →
  Texturen pro Raum budgetieren, evtl. auf 4 Bit reduzieren.
- **Ton: 16 Kanäle** (PS1: 24) → Sequenzer auf dem ARM7 mit Kanal-Priorität.
- **Zweiter Bildschirm** als Vorteil: Inventar, Karte und Status unten mit Touch.

## Phasen

### Phase 1 – Alle Räume begehbar (L) – erledigt bis auf Türanimation und genaue Kollisionsformen
- Batch-Konverter für alle 125 Räume; Hintergründe 8 Bit + LZ, beim Kamerawechsel laden.
- Räume wechseln über Türen inkl. Türanimation (DO2) und Ladezeit dahinter verstecken.
- Bodenhöhen, Treppen und Rampen (RDT-Höhendaten), Speicherverwaltung für Raumwechsel.
- Debug-Menü: in jeden Raum springen.
- **Fertig, wenn:** Leon jeden Raum von Szenario A betreten kann, ohne Absturz und mit
  passenden Kameras und Masken.

### Phase 2 – Skriptsystem (XL) – begonnen

Stand: Interpreter (`source/script.c`) mit Kontrollfluss, Flags, Variablen, Threads und
AOTs (Türen inkl. Schlösser, Items, Texte, Events); Texte der englischen PAL-Disc;
Host-Test `tests/script_host.c` über alle Räume. Offen: 12 Räume mit seltenen unbekannten
Opcodes, Figuren-/Kamera-Befehle für Cutscenes (plc_*, sce_em_set, obj_model_set), Flag-Startwerte.

- Interpreter für die Raumskripte (SCD-Bytecode). Vorlage: `script/` in OpenBiohazard2
  (Go, in diesem Ordner), fehlende Opcodes per REA aus der PS1-EXE.
- Auslöser (AOT): Türen, verschlossene Türen, Items aufheben, Untersuchen, Ereignisse.
- Spiel-Flags, Texte aus den RDT-Nachrichten mit eigener Schrift, Skript-Kamerafahrten.
- **Fertig, wenn:** in Szenario A alle Türen, Schlösser, Items und Rätsel per Skript
  funktionieren.

### Phase 3 – Spieler komplett (M)
- Zielen und Schießen (Analyse liegt vor), Waffenmodelle aus PLW, Nachladen, Munition.
- Gesundheit (Fine/Caution/Danger mit eigenen Animationen), Treffer, Tod, Game Over.
- Objekte schieben, Klettern, Treppen-Animationen.

### Phase 4 – Gegner (XL)
- Gegner-Konverter (EMD aus den EMS-Paketen) mit Polygon-Reduktion.
- Zombie, Hund, Krähe, Licker nach den vorhandenen Analysen nachbauen.
- Noch analysieren und bauen: Spinne, Motte, Pflanze (Ivy), Alligator, G (Birkin, alle
  Stufen), Mr. X (Szenario B).
- Treffer-Reaktionen, Kopfschüsse und Tod nach `damage.md`.
- **Fertig, wenn:** jeder Gegner in seinem Raum spielbar ist und das Bild flüssig bleibt.

### Phase 5 – Menüs und Speichern (L)
- Inventar, Item-Kiste, Kombinieren und Untersuchen, Karte, Files – auf dem unteren
  Bildschirm.
- Schreibmaschine und Farbbänder; Spielstände auf SD-Karte (libfat) bzw. melonDS.
- Titelbild, Optionen, Game Over, Abspann, Ranking.

### Phase 6 – Ton (L)
- Soundeffekte (EDH/VB) in DS-Samples umwandeln.
- Musik: das BGM-Sequenzformat analysieren und einen eigenen Sequenzer auf dem ARM7
  schreiben (16 Kanäle).
- Sprachausgabe: XA → IMA-ADPCM, gestreamt aus NitroFS.

### Phase 7 – Videos (L)
- 15 Videos pro Disc auf 256×192 umrechnen; DS-Codec mit Decoder auf dem ARM9
  (Entscheidung offen: eigener einfacher Codec oder einen vorhandenen portieren).
- Ziel ~15 Bilder/s und ~128 KB/s inkl. Ton.

### Phase 8 – Alle Szenarien (M)
- Disc 2 (Claire) durch dieselbe Pipeline schicken.
- Szenario A/B, Zapping-System (Entscheidungen in A wirken sich auf B aus).
- Optional: 4th Survivor, Tofu, Extra-Kostüme.

### Phase 9 – Feinschliff (M)
- Leistung und Ladezeiten, Bugs, Test auf echter Hardware (Flashcart), Spielstand-
  Kompatibilität.

## Größte Risiken

1. **Polygonbudget** bei mehreren Gegnern – früh in Phase 4 messen.
2. **Skriptsystem** – viele Opcodes, jeder Fehler blockiert den Spielfortschritt.
3. **Video-Codec** – Rechenleistung des ARM9 (67 MHz) und ROM-Größe.
4. **Musik** – das BGM-Format ist noch nicht analysiert.

## Nächster Schritt

Phase 1 beginnen: Batch-Konverter für alle Räume und komprimierte Hintergründe, danach
Türen und Raumwechsel.

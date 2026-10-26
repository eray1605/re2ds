# re2ds selber weiterentwickeln – eine Anleitung von Null an

Diese Anleitung soll dich in die Lage versetzen, re2ds ohne Hilfe weiterzuentwickeln:
Fehler finden, im PS1-Original nachsehen, wie es dort gemacht wird, den C-Code ändern,
bauen, testen und per Pull Request einreichen. Sie ist bewusst ausführlich und setzt
**keine** Programmierkenntnisse voraus.

Arbeite sie der Reihe nach durch. Teil 2 (C-Grundlagen) ist der wichtigste – nimm dir
dafür ruhig mehrere Abende Zeit und tippe die Beispiele selbst ab, statt sie nur zu lesen.

**Inhalt**

1. [Das große Bild: wie re2ds funktioniert](#1-das-große-bild-wie-re2ds-funktioniert)
2. [C-Grundlagen](#2-c-grundlagen)
3. [Werkzeuge einrichten](#3-werkzeuge-einrichten)
4. [Bauen, starten, testen](#4-bauen-starten-testen)
5. [Rundgang durch den Code](#5-rundgang-durch-den-code)
6. [Die Spielwelt in Zahlen](#6-die-spielwelt-in-zahlen)
7. [Raumskripte (SCD) lesen](#7-raumskripte-scd-lesen)
8. [Reverse Engineering: im PS1-Original nachsehen](#8-reverse-engineering-im-ps1-original-nachsehen)
9. [Drei echte Fälle Schritt für Schritt](#9-drei-echte-fälle-schritt-für-schritt)
10. [Der Arbeitsablauf für einen Fehler](#10-der-arbeitsablauf-für-einen-fehler)
11. [Git und Pull Requests](#11-git-und-pull-requests)
12. [Übungen](#12-übungen)
13. [Spickzettel](#13-spickzettel)

---

## 1. Das große Bild: wie re2ds funktioniert

re2ds ist **kein Emulator**. Es ist ein neu geschriebenes Programm für den Nintendo DS,
das die **Daten** der PS1-Disc (Räume, Modelle, Sounds, Skripte, Filme) benutzt und das
**Verhalten** des PS1-Spiels nachbaut. Das Verhalten lesen wir aus dem Maschinencode der
PS1 (`SLES_009.72` und die Gegner-Overlays) heraus.

```
PS1-Disc (.bin)
   │  tools/extract_psx_iso.py        Dateien von der Disc holen
   ▼
build/disc1/...  (RDT, PLD, EMD, VB, STR, ...)   ← Capcom-Daten, NIE committen
   │  tools/build_assets.sh           ruft viele Python-Konverter auf (*2ds.py)
   ▼
nitrofiles/...   (room/*.bin, model/*.bin, sound/*.bin, movie/*.bin)  ← auch Capcom-Daten
   │  make                            kompiliert source/*.c mit devkitARM
   ▼
re2ds.nds        (eine Datei, Code + nitrofiles/ darin)  ← NIE committen
```

Drei Arten von Arbeit kommen ständig vor:

| Art | Wo | Sprache |
|-----|----|---------|
| Spiel-Logik (Leon, Gegner, Skripte, Sound) | `source/*.c` | C |
| Daten umwandeln (PS1-Format → DS-Format) | `tools/*.py` | Python |
| Herausfinden, wie das Original es macht | Ghidra/REA auf der PS1-EXE | MIPS-Assembler / decompiliertes C |

In dieser Anleitung geht es vor allem um die erste und dritte Art.

Wichtige Grundregel: **Bevor du etwas „verbesserst“, schau, wie das Original es macht.**
Die meisten Fehler in re2ds entstehen, weil eine Annahme nicht stimmte (siehe Teil 9).

---

## 2. C-Grundlagen

C ist eine alte, kleine Sprache. Der Computer tut in C genau das, was dasteht – nicht mehr.
Das macht C schnell (wichtig für den DS mit 67 MHz und 4 MB RAM), aber auch gnadenlos:
Fehler stürzen nicht immer sofort ab, sondern machen manchmal „nur“ seltsame Dinge.

### 2.1 Dein erstes Programm (am Mac, nicht auf dem DS)

Lege irgendwo (nicht im Projekt) eine Datei `hallo.c` an:

```c
#include <stdio.h>          // holt printf herein

int main(void)              // hier beginnt jedes C-Programm
{
    printf("Hallo Leon!\n"); // \n = neue Zeile
    return 0;               // 0 = alles gut
}
```

Im Terminal:

```bash
cc hallo.c -o hallo
```

```bash
./hallo
```

`cc` ist der C-Compiler deines Macs. Er übersetzt den Text in ein Programm. Jede Zeile mit
einer Anweisung endet mit `;`. Alles hinter `//` ist ein Kommentar.

### 2.2 Variablen und Typen

Eine Variable ist ein benannter Speicherplatz mit einem **Typ**. Der Typ sagt, wie groß
der Platz ist und welche Werte hineinpassen.

```c
int hp = 200;            // ganze Zahl (auf DS und Mac 32 Bit: ca. ±2 Milliarden)
char c = 'A';            // 8 Bit
float zeit = 1.5f;       // Kommazahl – auf dem DS LANGSAM, sparsam benutzen
```

In re2ds benutzen wir fast immer Typen mit **fester Größe** aus `<stdint.h>`, weil die
PS1-Daten genau so gebaut sind:

| Typ | Bits | Wertebereich | typisch für |
|-----|-----:|--------------|-------------|
| `uint8_t` | 8 | 0 … 255 | Zustände, Flags, Keyframe-Nummern |
| `int8_t` | 8 | −128 … 127 | kleine Versätze |
| `uint16_t` | 16 | 0 … 65535 | Flag-Felder, Größen |
| `int16_t` | 16 | −32768 … 32767 | Winkel, Positionen in Skripten |
| `uint32_t` | 32 | 0 … 4 Milliarden | Distanzen, Adressen |
| `int32_t` | 32 | ±2 Milliarden | Weltkoordinaten x/y/z |

**Überlauf:** Ein `uint8_t` mit 255 plus 1 wird 0. Ein `int16_t` mit 32767 plus 1 wird
−32768. Das ist bei Winkeln sogar praktisch (siehe 6.2), sonst eine häufige Fehlerquelle.

### 2.3 Rechnen

```c
int a = 7 / 2;     // 3   – ganze Zahlen schneiden ab!
int b = 7 % 2;     // 1   – Rest
int c = -7 / 2;    // -3  – schneidet Richtung 0 ab
a += 5;            // a = a + 5
a++;               // a = a + 1
```

Weil der DS keine schnelle Kommazahl-Einheit hat, rechnet re2ds wie die PS1 mit
**Festkomma**: Die Zahl 4096 steht für 1,0. Beispiel: `(30 * cosLerp(dir)) >> 12`
heißt „30 mal Kosinus“, denn `cosLerp` liefert −4096 … 4096 und `>> 12` teilt durch 4096.

### 2.4 Hexadezimal und Bits – unbedingt lernen

Im Reverse Engineering ist fast alles hexadezimal (Basis 16, Ziffern 0–9 und A–F):

| Dezimal | Hex | Binär |
|--------:|----:|------:|
| 1 | `0x1` | `0001` |
| 8 | `0x8` | `1000` |
| 15 | `0xF` | `1111` |
| 16 | `0x10` | `0001 0000` |
| 255 | `0xFF` | `1111 1111` |
| 4096 | `0x1000` | eine 1 und 12 Nullen |

Eine Hex-Ziffer sind genau 4 Bits. `0x4510` sind also die Bits
`0100 0101 0001 0000`. Deshalb kann man Flags so gut in Hex lesen.

Bit-Operatoren:

```c
x & 0xFF      // UND: nur die unteren 8 Bits behalten ("maskieren")
x | 0x80      // ODER: Bit 7 setzen
x & ~0x80     // Bit 7 löschen (~ dreht alle Bits um)
x ^ 0x80      // Bit 7 umschalten
x >> 8        // 8 Bits nach rechts schieben = durch 256 teilen
x << 4        // 4 Bits nach links = mal 16
(x >> 8) & 0xF   // die Bits 8..11 als Zahl 0..15 herausholen
```

Beispiel aus `source/enemy.c`:

```c
e->airborne = ((e->flags >> 8) & 0xF) != 0;
```

Die Spawn-Flags des Lickers im Raum 2040 sind `0x4510`. `>> 8` ergibt `0x45`,
`& 0xF` ergibt `5`, also „nicht 0“ → der Licker startet in der Luft (an der Decke).
Und `e->flags & 0x4000` prüft Bit 14: „von einem Skript gesteuert“.

Übe das, bis du es im Kopf kannst. Ein Taschenrechner im „Programmierer“-Modus (macOS:
Rechner → Darstellung → Programmierer) hilft am Anfang.

### 2.5 Entscheidungen und Schleifen

```c
if (hp < 21) {
    printf("Danger\n");
} else if (hp < 101) {
    printf("Caution\n");
} else {
    printf("Fine\n");
}

for (int i = 0; i < 6; i++) {      // i = 0,1,2,3,4,5
    printf("Gegner %d\n", i);
}

while (y < floor_y) {              // solange die Bedingung stimmt
    y += 72;
}

switch (state) {                   // wie viele ifs auf einen Wert
case 0:
    printf("lauert\n");
    break;                         // ohne break läuft es in den nächsten case weiter!
case 1:
    printf("verfolgt\n");
    break;
default:
    printf("anderes\n");
}
```

Vergleiche: `==` gleich (NICHT `=`, das ist Zuweisung!), `!=` ungleich, `<`, `<=`, `>`, `>=`.
Verknüpfen: `&&` und, `||` oder, `!` nicht. In C ist 0 „falsch“ und alles andere „wahr“.

**Klassiker-Fehler:** `if (x = 5)` setzt x auf 5 und ist immer wahr. Gemeint war `if (x == 5)`.

### 2.6 Funktionen

```c
// gibt die größere von zwei Zahlen zurück
static int max_of(int a, int b)
{
    if (a > b)
        return a;
    return b;
}

int main(void)
{
    printf("%d\n", max_of(3, 9));   // 9
    return 0;
}
```

- `int` vor dem Namen ist der Rückgabetyp, `void` heißt „gibt nichts zurück“.
- `static` heißt: nur in dieser .c-Datei sichtbar. In re2ds ist fast alles `static`.
- Eine Funktion muss **vor** ihrer Benutzung bekannt sein – entweder weiter oben stehen
  oder vorher „deklariert“ werden (nur die erste Zeile mit `;` am Ende):

```c
static void walk(Enemy *e, int i, int32_t step_len);   // Deklaration
```

### 2.7 Arrays (Felder)

```c
static const uint8_t hp_table[16] = { 80, 94, 128, 75, 60, 95, 58, 75,
                                      50, 83, 79, 66, 80, 65, 82, 65 };
int hp = hp_table[3];      // 75 – gezählt wird ab 0!
```

`const` heißt: wird nie verändert (landet dann im ROM statt im knappen RAM).
**Achtung:** C prüft keine Grenzen. `hp_table[16]` liest einfach irgendetwas dahinter.

Eine Besonderheit, die re2ds oft nutzt, sind **benannte Startwerte**:

```c
static const char *const movie_names[] = {
    [4] = "r10b", [5] = "r200", [10] = "r204l",
};
```

Hier ist `movie_names[10]` der Text `"r204l"`, alle nicht genannten Einträge sind `NULL`.

### 2.8 Strukturen (struct)

Eine `struct` fasst zusammengehörige Werte zusammen – wie ein Formular:

```c
typedef struct {
    int32_t x, y, z;     // Position
    int16_t dir;         // Blickrichtung
    int16_t hp;
    uint8_t state;       // was er gerade tut
} Enemy;

Enemy e;
e.hp = 80;
e.state = 1;
```

Die echte `Enemy`-Struktur steht in `source/enemy.c` und hat Dutzende Felder. Hinter
vielen steht als Kommentar, wo das Feld im PS1-Speicher liegt, z. B.
`int16_t hp; // obj+0x156`. Das ist die Brücke zum Reverse Engineering (Teil 8).

### 2.9 Zeiger – das schwierigste Thema

Ein Zeiger ist eine Variable, die eine **Speicheradresse** enthält – „wo etwas liegt“
statt „was es ist“.

```c
int hp = 200;
int *p = &hp;      // & = "Adresse von"; p zeigt jetzt auf hp
*p = 150;          // * = "das, worauf p zeigt"; hp ist jetzt 150
```

Warum braucht man das? Damit eine Funktion etwas **verändern** kann, das ihr nicht gehört:

```c
static void heile(int *wert)
{
    *wert += 50;
}

int main(void)
{
    int hp = 100;
    heile(&hp);         // hp ist jetzt 150
    return 0;
}
```

Bei Strukturen schreibt man statt `(*e).hp` kürzer `e->hp`. Deshalb siehst du in re2ds
überall `Enemy *e` und `e->hp`, `e->state` usw.: Die Funktion bekommt einen Zeiger auf
einen der Gegner im Feld `enemies[]` und ändert ihn direkt.

`NULL` ist ein Zeiger, der auf nichts zeigt. Wenn du `*p` liest und `p` ist `NULL`, stürzt
das Programm ab (auf dem DS meist ein eingefrorener Bildschirm). Prüfe deshalb oft:

```c
if (!w)          // w ist NULL
    return;
```

### 2.10 Header und Quelldateien

- `.h` (Header): sagt anderen Dateien, **was es gibt** (Typen, Funktionsnamen).
- `.c` (Quelle): enthält **wie es gemacht wird**.

Beispiel: `source/roomfx.h` deklariert `void roomfx_tick(void);`, `source/main.c` macht
`#include "roomfx.h"` und darf es dann aufrufen. Der eigentliche Code steht in
`source/roomfx.c`.

Wenn eine Datei etwas von einer anderen braucht, die sie nicht einbinden soll (z. B.
`enemy.c` will einen Sound abspielen, der Sound-Code steht aber in `main.c`), benutzt
re2ds **Rückruf-Funktionen** mit dem Präfix `game_`: `enemy.c` ruft
`game_enemy_sound(...)`, und `main.c` implementiert sie. Das hält die Teile getrennt und
erlaubt die Host-Tests (Teil 4.3), die eigene, einfache `game_`-Funktionen mitbringen.

### 2.11 #define und enum

```c
#define LICK_GRAVITY 72          // ein fester Wert mit Namen
enum { ST_IDLE, ST_WALK, ST_RUN };   // ST_IDLE = 0, ST_WALK = 1, ST_RUN = 2
```

Regel im Projekt: **keine „magischen Zahlen“** mitten im Code. Gib Werten einen Namen und
schreib dazu, woher sie kommen (`// state 17, 0x80104f74`).

### 2.12 printf – dein wichtigstes Debug-Werkzeug

```c
printf("x=%d y=%d\n", (int)x, (int)y);      // %d ganze Zahl
printf("flags=%04x\n", flags);               // %x hex, 04 = 4 Stellen mit Nullen
printf("name=%s\n", "Leon");                 // %s Text
```

Auf dem DS erscheint `printf` auf dem unteren Bildschirm (siehe `print_status` in
`source/main.c`, SELECT öffnet das Menü). In den Host-Tests erscheint es im Terminal.

### 2.13 Speicher

Der DS hat 4 MB RAM. Große Daten (Raum, Modelle, Sounds) werden mit `asset_load` geladen
und mit `asset_free` wieder freigegeben. Vergisst du das Freigeben, ist nach einigen
Räumen der Speicher voll. Gibst du etwas frei und benutzt es danach weiter, passiert
Chaos. Merke: **Wer lädt, gibt frei.** Schau dir an, wie bestehender Code es macht,
und mach es genauso.

### 2.14 Typische C-Fehler und wie man sie findet

| Fehler | Symptom | Gegenmittel |
|--------|---------|-------------|
| Array-Grenze überschritten | zufällige Werte, Absturz | Grenzen prüfen, UBSan (4.3) |
| `=` statt `==` | Bedingung immer wahr | Compiler-Warnungen lesen |
| Ganzzahl-Division | 7/2 = 3 | erst multiplizieren, dann teilen |
| Überlauf (`int16_t` > 32767) | Werte springen ins Negative | größeren Typ, `& 32767` bei Winkeln |
| `NULL`-Zeiger benutzt | eingefrorener DS | vorher `if (!p) return;` |
| fehlendes `break` im `switch` | zwei Fälle laufen nacheinander | immer `break;` |
| Variable nicht initialisiert | mal geht's, mal nicht | jede Variable mit Startwert |

**Lies die Warnungen des Compilers!** `make` zeigt `warning:`-Zeilen. Der Projektstand hat
null Warnungen – eine neue Warnung ist fast immer ein echter Fehler.

### 2.15 Zum Weiterlernen

- „C Programming: A Modern Approach“ (K. N. King) – das beste Lehrbuch, Kapitel 1–17.
- Online kostenlos: https://www.learn-c.org (interaktiv, kurz).
- cppreference.com (Abschnitt C) zum Nachschlagen einzelner Funktionen.
- Übe mit kleinen Programmen am Mac (`cc datei.c -o prog && ./prog`), nicht gleich im
  großen Projekt.

---

## 3. Werkzeuge einrichten

Auf deinem Mac ist alles schon installiert. Zur Kontrolle oder für einen neuen Rechner:

| Werkzeug | Wozu | Prüfen mit |
|----------|------|-----------|
| Xcode Command Line Tools | `cc`, `git`, `make` | `cc --version` |
| devkitPro mit devkitARM + libnds | Compiler für den DS | `ls /opt/devkitpro/devkitARM` |
| Python 3 | alle `tools/*.py` | `python3 --version` |
| Homebrew `ffmpeg` | Filme und Bilder umwandeln | `ffmpeg -version` |
| melonDS | DS-Emulator zum Testen | App starten |
| GitHub CLI `gh` | Pull Requests | `gh auth status` |
| Ghidra (kostenlos, ghidra-sre.org) | PS1-Code lesen (Teil 8) | App starten |
| Ein PS1-Emulator mit Debugger: DuckStation oder PCSX-Redux | dem Original live zuschauen | App starten |
| Editor: VS Code mit der Erweiterung „C/C++“ | Code lesen und schreiben | – |

Die Installation von devkitPro steht in `README.md` (und `tools/setup_wsl.sh` für
Windows/WSL). `sudo`-Befehle musst du selbst eingeben.

**Tipp für VS Code:** Öffne den Ordner `re2ds`. Mit Cmd+Klick auf einen Funktionsnamen
springst du zur Definition, mit Cmd+Shift+F suchst du im ganzen Projekt. Das ist die
Fähigkeit, die du am meisten brauchst: **Code finden**.

---

## 4. Bauen, starten, testen

### 4.1 Daten umwandeln (nur einmal, oder nach Änderungen an `tools/`)

```bash
tools/build_assets.sh build/disc1
```

Das dauert lange (alle Räume, Filme). Für einzelne Räume:

```bash
tools/build_assets.sh build/disc1 ROOM2040
```

### 4.2 Die ROM bauen

Im Ordner `re2ds`:

```bash
make DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
```

Am Ende steht `built ... re2ds.nds`. Steht dort `error:`, lies die **erste** Fehlermeldung –
sie nennt Datei und Zeile, z. B. `source/enemy.c:2150:5: error: ...`. Spätere Fehler sind
oft nur Folgefehler.

Dann `re2ds.nds` in melonDS öffnen. Im Spiel: SELECT öffnet das Raum-Menü und den
Debug-Bildschirm (Raum, Position, Kamera, Gegner, Fehlercodes).

### 4.3 Host-Tests: Spiel-Logik ohne DS ausprobieren

Das ist ein Riesenvorteil: Teile der Spiel-Logik laufen auch am Mac, ohne Grafik.
Die Tests stehen in `tests/`. Sie bringen eigene, einfache Versionen der
`game_`-Funktionen mit (siehe 2.10) und drucken, was passiert.

**Gegner simulieren** (`tests/enemy_host.c`):

```bash
cc -O1 -Itests/stubs -Isource tests/enemy_host.c source/script.c source/enemy.c source/plc.c -o build/enemy_host -lm
```

```bash
TICKS=300 TRACE=1 ./build/enemy_host nitro:/room/room1000.bin
```

Steuerung über Umgebungsvariablen (vor den Befehl schreiben):

| Variable | Wirkung |
|----------|---------|
| `TICKS=600` | so viele Spiel-Ticks laufen |
| `TRACE=1` | jeden Tick Zustand, Animation, Position jedes Gegners |
| `PX=5899 PZ=0` | wo Leon steht |
| `NOISE=4` | Leon macht Geräusche (1 Schuss, 2 Gehen, 4 Rennen) |
| `SHOOT=1` | Leon schießt alle 15 Ticks auf den nächsten Gegner |
| `MEMBERS=10:1:0x17:1` | in Tick 10 bei Gegner 1 das Skript-Feld 0x17 auf 1 setzen |
| `EVENT=3` | Ereignis-Funktion 3 des Raums starten |

Die Ausgabezeilen sehen so aus:

```
t17 e1 st 0/0 anim 19/23 key 0 pos 32000 32000 dir 8192 verts 1940
```

= Tick 17, Gegner 1, Zustand 0 / Unterzustand 0, Animation 19 (von 23 Keyframes), Keyframe 0,
Position x/z, Richtung.

**Zwischensequenzen simulieren** (`tests/cutscene_host.c`):

```bash
cc -Itests/stubs -Isource tests/cutscene_host.c source/script.c source/enemy.c source/plc.c -o build/cutscene_host -lm
```

```bash
TRACE=1 ./build/cutscene_host nitro:/room/room2040.bin 9 400
```

Startet die Ereignis-Funktion 9 von Raum 2040 und druckt Nachrichten, Kameraschnitte,
Filme, Leons Bewegungen und Gegner.

**Fehler aufspüren mit dem Sanitizer:** Baue einen Test zusätzlich mit
`-g -fsanitize=undefined`. Dann meldet das Programm Überläufe, falsche Verschiebungen
usw. als `runtime error:` mit Datei und Zeile:

```bash
cc -O1 -g -fsanitize=undefined -Itests/stubs -Isource tests/enemy_host.c source/script.c source/enemy.c source/plc.c -o build/enemy_host_ub -lm
```

(`-fsanitize=address` hängt in dieser Umgebung leider – nimm `undefined`.)

**Skripte aller Räume prüfen:**

```bash
cc -O1 -g -fsanitize=undefined -Isource tests/script_host.c -o build/script_host
```

```bash
./build/script_host nitrofiles/room/room*.bin
```

### 4.4 Testen in melonDS – eine Checkliste

1. Raum über SELECT anspringen (spart Zeit).
2. Genau das machen, was der Fehler beschreibt.
3. Debug-Bildschirm lesen (Gegnerzahl, `fehler`, Position).
4. Zum Vergleich ein Video vom Original ansehen (YouTube „Resident Evil 2 Leon A
   longplay PS1“) oder es in DuckStation selbst spielen. **Das Original ist die
   Wahrheit**, nicht die Erinnerung.

---

## 5. Rundgang durch den Code

| Datei | Zeilen ca. | Was darin passiert |
|-------|-----------:|--------------------|
| `source/main.c` | 3800 | Hauptschleife, Leon (Laufen, Zielen, Schießen, Nachladen), Kamera, Kollision, Räume laden, Türen, Inventar, Debug-Anzeige, alle `game_`-Rückrufe |
| `source/enemy.c` | 5500 | alle Gegner: Zombies, Hunde, Licker, Krähen, Spinnen, Tyrant, Birkin … |
| `source/script.c` | 900 | Interpreter der Raumskripte (SCD-Opcodes) |
| `source/plc.c` | – | Bewegungen, die Skripte Figuren befehlen („geh dorthin“, Animation X) |
| `source/music.c` | – | Musik (PS1-Sequenzen nachgespielt) |
| `source/sound.c` | – | Soundeffekte abspielen |
| `source/roomfx.c`, `effect.c` | – | Sprites: Mündungsfeuer, Blut, Hülsen, Feuer |
| `source/movie.c`, `subs.c` | – | Filme und Untertitel |
| `source/menu.c` | – | Inventar, Karte, Dateien, Titelbild |
| `source/door.c` | – | Tür-Animationen |
| `source/save.c` | – | Speichern/Laden |
| `source/formats.h` | – | Aufbau der umgewandelten Dateien |

**Wie man sich zurechtfindet:**

1. Suche nach einem Wort aus dem Problem: „licker“, „casing“, „movie“, „reload“.
2. Lies den Kommentar über der Funktion. Fast jede nennt die PS1-Adresse, die sie nachbaut
   (z. B. `// state 17, 0x80104f74`).
3. Lies `docs/` – dort steht, was über das Original schon bekannt ist:
   - `docs/how-re2ds-works.md` – Überblick in einfacher Sprache
   - `docs/engine-notes.md` – Kamera, Skripte, Sound, Musik, Kollision, Filme
   - `docs/player-control.md` – Leon, Waffen
   - `docs/enemy-zombie.md`, `docs/enemy-others.md` – Gegner
   - `docs/damage.md` – Schaden
   - `docs/HANDOFF.md` – Stand des Projekts, offene Punkte, Funktionsliste

**Der Spiel-Takt:** Die PS1-Version (PAL) rechnet 25-mal pro Sekunde (`GAME_HZ`). Der DS
zeichnet 60-mal pro Sekunde. In `main.c` sammelt `game_tick_acc` die Zeit und ruft pro
Spiel-Tick einmal die Logik auf (`update_player_tick`, `enemy_tick`, `script_tick`,
`roomfx_tick`). Ein Keyframe einer Animation = ein Tick.

---

## 6. Die Spielwelt in Zahlen

### 6.1 Koordinaten

- x und z sind der Boden, y ist die Höhe. **y ist nach oben negativ**: Die Decke liegt
  bei etwa −5700, der Boden bei 0 (bzw. bei −1800 pro Stockwerk höher).
- 1800 Einheiten ≈ eine Etagenhöhe, Leon ist etwa 1800 groß. 500 ≈ Breite eines Zombies.

### 6.2 Winkel – zwei Systeme!

| System | Voller Kreis | Wo |
|--------|-------------:|----|
| RE2/PS1 | 4096 (`0x1000`) | PS1-Code, Skripte, Dokumentation |
| DS (libnds) | 32768 | `e->dir`, `player.dir`, `cosLerp`, `sinLerp` |

Umrechnung: DS = RE2 × 8. Deshalb steht oft `* 8` im Code. Weil der Kreis bei 32768
endet, schreibt man `& 32767`, damit der Winkel „herumläuft“ (32770 → 2).

Halber Kegel ±45° = `0x200` in RE2-Einheiten, in `in_cone(e, x, z, 0x200)` steckt das `* 8`.

### 6.3 Festkomma

4096 = 1,0. Schwerkraft, Geschwindigkeiten und Skalierungen sind oft so gespeichert.

### 6.4 Ticks und Zeit

25 Ticks = 1 Sekunde (PAL). Ein Timer von 150 ≈ 6 Sekunden.

---

## 7. Raumskripte (SCD) lesen

Jeder Raum (`ROOMsrrp.RDT`: s = Stufe, rr = Raum hex, p = Spieler 0 Leon) hat Skripte:
`init_script` (beim Betreten) und `exec_script` (laufende Ereignisse). Sie bestimmen
Zwischensequenzen, Gegner, Türen, Flags.

**Zerlegen:**

```bash
python3 -I tools/scd_disasm.py build/disc1/PL0/RDT/ROOM2040.RDT > /tmp/r2040.txt
```

Ausschnitt:

```
exec_script function 9:
    120  movie_on 0a
    123  work_set 03 01            # "Gegner 1 auswählen"
    126  pos_set 00 eb c5 bc e9 69 f2   # Position x=0xc5eb, y=0xe9bc, z=0xf269
    134  member_set 17 01 00       # Feld 0x17 = 1 ("los!")
```

Zahlen sind **Little Endian**: `bc e9` ist der 16-Bit-Wert `0xe9bc` = −5700 (die Decke).

Wichtige Befehle (vollständige Liste: `NAMES` in `tools/scd_disasm.py`, umgesetzt in
`source/script.c`):

| Befehl | Bedeutung |
|--------|-----------|
| `ck a b v` | prüfe Flag b in Flag-Feld a gleich v |
| `set a b v` | setze Flag |
| `if … else … endif` | Bedingung |
| `sleep`, `evt_exec`, `evt_end` | warten, Ereignis starten, Ende |
| `sce_em_set` | Gegner/Figur platzieren (Slot, Typ, Flags, Position) |
| `work_set 03 n` / `member_set` | Gegner n auswählen / ein Feld setzen |
| `plc_dest`, `plc_motion` | Figur hingehen / Animation spielen lassen |
| `cut_chg` | Kamera wechseln |
| `movie_on n` | Film n abspielen |
| `se_on` | Sound |
| `sce_bgm_control` | Musik |

Alle Räume nach einem Befehl durchsuchen:

```bash
for f in build/disc1/PL0/RDT/ROOM*.RDT; do python3 -I tools/scd_disasm.py "$f" | grep -q movie_on && echo "$f"; done
```

---

## 8. Reverse Engineering: im PS1-Original nachsehen

Hier liegt die eigentliche Kunst. Ziel: Zu einem Verhalten die Stelle im PS1-Code finden
und verstehen, was sie tut.

### 8.1 Was du vor dir hast

- `SLES_009.72` ist das PS1-Hauptprogramm (MIPS R3000, wird bei Adresse `0x80010000`
  geladen). Gegner-Code liegt in Overlays (`PL0/PLD/CDEMD0.EMS`), die zur Laufzeit bei
  `0x80100000` eingeblendet werden.
- Der Decompiler (Ghidra) macht aus Maschinencode C-ähnlichen Text. Namen gibt es keine:
  `FUN_80057364` heißt „Funktion bei Adresse 0x80057364“, `DAT_800c77b4` heißt „Daten bei
  Adresse 0x800c77b4“.
- In `docs/HANDOFF.md` und `tools/research/re2_symbols*.txt` stehen Adressen, deren
  Bedeutung schon bekannt ist (z. B. `scd_op_6f` = Skriptbefehl `movie_on`).

### 8.2 Ghidra vorbereiten

1. Ein ELF bauen, das Ghidra versteht (mit den bekannten Namen):

```bash
python3 -I tools/research/psxexe_to_elf.py build/disc1/SLES_009.72 build/re2.elf tools/research/re2_symbols.txt
```

   Für einen Gegner mit seinem Overlay (Beispiel Zombie, Typ 0x1f, Slot 0): erst mit
   `tools/research/overlay_symbols.py` die `--overlay`-Angabe und Symbole erzeugen
   (siehe Kopf der Datei), dann an `psxexe_to_elf.py` anhängen.
2. Ghidra starten → neues Projekt → `build/re2.elf` importieren → Prozessor
   „MIPS:LE:32:default“ ist erkannt → analysieren lassen (dauert ein paar Minuten).
3. Mit **G** (Go to) zu einer Adresse springen, z. B. `80054a44`.
4. Rechts steht der decompilierte Code. **L** benennt Dinge um – benenne alles, was du
   verstanden hast. **;** setzt Kommentare. Das ist dein Notizbuch.
5. **Referenzen**: Rechtsklick auf eine Funktion/Variable → „References → Show References
   to“. So findest du, wer etwas aufruft oder schreibt.

### 8.3 Decompilierten Code lesen

Typischer Ausschnitt (Licker fällt von der Decke, `0x80104f74`):

```c
case 1:
    sVar3 = *(short *)((int)param_1 + 0x146) + 0x48;
    *(short *)((int)param_1 + 0x146) = sVar3;
    param_1[0xf] = param_1[0xf] + (int)sVar3;
    if (-1 < (int)(param_1[0xf] + (int)*(short *)((int)param_1 + 0x146) + 0x48)) {
        FUN_800576a8(2, param_1);
        ...
```

So übersetzt man das:

- `param_1` ist der Zeiger auf das Gegner-Objekt („obj“).
- `*(short *)((int)param_1 + 0x146)` = das 16-Bit-Feld bei Versatz 0x146 → in re2ds
  `e->vspeed` (steht im Kommentar der `Enemy`-Struktur).
- `param_1[0xf]`: `param_1` ist hier ein Zeiger auf 4-Byte-Werte, also Versatz 0xf × 4 =
  0x3c → die y-Position.
- Zeile 1–3: `vspeed += 72; y += vspeed;` → **Schwerkraft**.
- `if (y + vspeed + 72 >= 0)` → am Boden angekommen → `FUN_800576a8(2, obj)` =
  Gegner-Sound 2.

**Vorgehen beim Lesen:**

1. Erst grob: Welche Funktionen werden aufgerufen? Welche Felder geschrieben?
2. Felder mit schon bekannten Versätzen abgleichen (`docs/`, `Enemy`-Struktur).
3. Konstanten notieren (0x48 = 72) und in Dezimal umrechnen.
4. Eine Vermutung aufschreiben – und dann **an echten Daten prüfen** (Teil 9!).

### 8.4 Wichtige Grundbausteine im PS1-Code

| Adresse | Was | Parameter |
|---------|-----|-----------|
| `FUN_80057364` | Sound abspielen | `bank << 24 \| sound << 16 \| …` (Bank 1 = Waffe, 2 = Raum, 3 = Gegner) |
| `FUN_800576a8` | Gegner-Sound | Nummer, obj |
| `FUN_8001b8fc` | Effekt (Sprite) erzeugen | `effekt << 24 \| sub << 16 \| …` |
| `FUN_8001581c` | Animation: Wurzelbewegung berechnen | obj |
| `FUN_80015988` | Zufallszahl 0..255 (`rnd()` in re2ds) | – |
| `FUN_80014fb4` | „liegt Ziel im Kegel?“ (0 = ja) | obj, x, z, halber Winkel |
| `0x800c782c / 0x800c7834` | Leons x / z | – |
| `0x800c77b4 & 0x20` | Schwierigkeit EASY | – |
| `scd_op_XX` | Skriptbefehl XX | – |

### 8.5 Daten statt Code anschauen

Oft steckt die Antwort in **Tabellen** oder **Dateien**, nicht im Code. Python ist dafür
ideal:

```python
import struct
d = open('build/disc1/COMMON/DATA/CORE00.ESP', 'rb').read()
print(d[:16].hex(' '))                       # die ersten 16 Bytes in Hex
wert = struct.unpack_from('<H', d, 0x10)[0]  # 16-Bit-Wert bei Versatz 0x10 (< = Little Endian)
```

Format-Zeichen für `struct`: `B` u8, `b` s8, `H` u16, `h` s16, `I` u32, `i` s32.

### 8.6 Dem Original live zuschauen

DuckStation und PCSX-Redux haben einen Debugger: Haltepunkt auf eine Adresse setzen,
Speicher ansehen, Werte live beobachten. Damit kannst du z. B. sehen, welchen Wert
`obj+0x12` beim Aufprall wirklich hat. Das ist der sicherste Beweis, den es gibt.

---

## 9. Drei echte Fälle Schritt für Schritt

Diese drei Fehler wurden in diesem Projekt so gelöst. Sie zeigen typische Fallen.

### Fall 1: Falscher Film beim Licker

**Meldung:** „Beim Licker kommt die Szene, wo William erschossen wird.“

1. Wo werden Filme gestartet? Suche nach `movie` in `source/script.c` → Befehl
   `OP_MOVIE_ON` ruft `game_movie(ins[1])`. In `source/main.c` steht die Tabelle
   `movie_names[]` (Nummer → Datei).
2. Welcher Raum spielt welchen Film? Alle Skripte zerlegen (Befehl aus Teil 7) →
   `ROOM2040: movie_on 0a`, `ROOM4090: movie_on 0b`, `ROOM10B0: movie_on 04`, …
3. Was zeigen die Filme? Mit ffmpeg Vorschaubilder machen:

```bash
ffmpeg -f psxstr -i build/disc1/PL0/ZMOVIE/R204L.STR.raw2352 -an -vf "select='not(mod(n\,40))',scale=160:80,tile=6x1" -frames:v 1 /tmp/R204L.png
```

   → R204L zeigt den Licker, der von der Decke fällt.
4. **Das Muster erkennen:** Jeder Film heißt wie sein Raum: R204L ↔ ROOM2040,
   R409 ↔ ROOM4090, R10B ↔ ROOM10B0. Raum 2040 ruft 10 auf, also muss 10 = R204L sein.
   In der Tabelle stand aber `[9] = "r204l"`, `[10] = "r409"` – alles um eins verschoben.
5. Korrigieren, bauen, testen.

**Lehre:** Wenn ein Fehler „um eins daneben“ ist, prüfe die Zählung (ab 0 oder ab 1?).
Und: Namen in Daten sind oft der beste Beweis.

### Fall 2: Der Hülsensound

**Meldung:** „Falscher Sound für die Patronenhülse.“

1. Im PS1-Code die Aufprall-Funktion der Hülse gefunden (`FUN_8002038c`). Sie ruft
   `FUN_80057364(*(ushort *)(obj + 0x12) << 16 | 1)` – die Soundnummer steht also im
   Effekt-Objekt bei `+0x12`.
2. **Der Fehler (beim ersten Versuch):** Es wurde angenommen, `obj` beginne dort, wo die
   Erzeuger-Funktion `FUN_8001b8fc` „Basis“ schreibt, und `+0x12` sei ein Feld aus dem
   Kopf der Effektdatei (Wert 0x1f). Daraus folgte „Bank 0, Sound 0x1f“ → stumm. Das war
   falsch, und der Nutzer hatte ein Video des Originals, das das Gegenteil zeigte.
3. **Die Prüfung, die gefehlt hatte:** Die Aufprall-Funktion liest die Position bei
   `+0x34/+0x36/+0x38`. An der angenommenen Basis stand dort eine Drehmatrix – unmöglich.
   Also war die Basis falsch: Das Objekt beginnt 0x18 Bytes früher, mit einer Kopie der
   24-Byte-Bewegungsdaten aus der Effektdatei. Dort ist `+0x0c/+0x0e` die
   Startgeschwindigkeit (140 / −50 – passt!) und `+0x12` = `0x0102`.
4. `0x0102 << 16 | 1` = `0x01020001` → Bank 1 (Waffe), Sound 2. Für Schrotpatronen
   `0x0109` → Sound 9.

**Lehre:** Prüfe jede Annahme an einem **zweiten** Merkmal (hier: passen die anderen
Felder an diesem Versatz?). Und wenn das Original-Video etwas anderes zeigt als dein
Code-Verständnis, hat das Video recht.

### Fall 3: Der schwebende Licker

**Meldung:** „Der Licker schwebt nach unten statt zu fallen.“

1. In `enemy.c` gesucht, was mit `airborne` passiert: `e->y += 120;` pro Tick –
   gleichmäßig, also „schweben“.
2. Im PS1-Code die Zustände 16/17 des Lickers gelesen (Ausschnitt in 8.3):
   Geschwindigkeit wächst jeden Tick um 72 → beschleunigtes Fallen. Dazu Animation
   `0x12` (loslassen), `0x13` (fallen/landen) und Sound 2 beim Aufprall.
3. Welche Variante setzt Raum 2040? Flags `0x4510`: `& 3` = 0 → Zustand 16 (loslassen,
   dann fallen).
4. Neue Funktion `licker_drop()` geschrieben, mit `enemy_host` geprüft
   (`MEMBERS=10:1:0x17:1 TRACE=1`): Animation 18, dann 19, Landung nach ~11 Ticks.

**Lehre:** „Sieht komisch aus“ heißt oft: Eine Bewegung ist linear statt beschleunigt,
oder eine Animation fehlt. Schau, welche Animationen das Original in dem Zustand setzt.

---

## 10. Der Arbeitsablauf für einen Fehler

1. **Genau beschreiben:** Raum, was passiert, was passieren sollte. Original-Video suchen.
2. **Reproduzieren:** in melonDS (SELECT → Raum) oder im Host-Test.
3. **Code finden:** Suche (Cmd+Shift+F) nach Stichworten, Kommentare und `docs/` lesen.
4. **Original prüfen:** PS1-Adresse aus dem Kommentar in Ghidra öffnen, lesen, Annahmen
   gegen Daten prüfen (Teil 8, 9).
5. **Kleinste Änderung machen**, die das Original nachbaut. Kommentar mit PS1-Adresse
   dazuschreiben. Stil des umgebenden Codes übernehmen.
6. **Host-Test** (wenn möglich) mit `-fsanitize=undefined`.
7. **ROM bauen**, null Warnungen, in melonDS testen.
8. **Doku aktualisieren** (`docs/…`), wenn du etwas Neues über das Original gelernt hast.
9. **Commit und Pull Request** (Teil 11).

Wenn du nicht weiterkommst: Schreib auf, was du weißt und was nicht. Oft löst sich das
Problem beim Aufschreiben. Mach eine Pause und lies am nächsten Tag noch einmal.

---

## 11. Git und Pull Requests

Regeln des Projekts:

- Gearbeitet wird auf `develop`. Jede Änderung bekommt einen eigenen Zweig
  (`feature/...`) von `develop` und einen Pull Request nach `develop`.
- Ein Release ist ein Pull Request `develop` → `main`. Danach `develop` neu aus `main`
  erstellen.
- **Niemals committen:** `re2ds.nds`, `build/`, `nitrofiles/`, Untertitel-Texte – das sind
  Capcom-Daten (und die ROM ist zu groß für GitHub). `.gitignore` schützt davor, aber
  prüfe trotzdem mit `git status`.

Ablauf:

```bash
git fetch origin
```

```bash
git switch -c feature/mein-fix origin/develop
```

… Dateien ändern, bauen, testen …

```bash
git status
```

```bash
git diff
```

```bash
git add source/enemy.c docs/enemy-others.md
```

```bash
git commit -m "Licker: fall with gravity like the PS1 (state 16/17)"
```

```bash
git push -u origin feature/mein-fix
```

```bash
gh pr create --base develop --title "Licker: fall with gravity" --body "Was und warum, wie getestet."
```

Gute Commit-Nachricht: erste Zeile kurz, was sich ändert (auf Englisch wie der Rest des
Repos); darunter, warum, und welche PS1-Adresse es belegt.

Wenn etwas schiefgeht:

```bash
git restore source/enemy.c
```

(verwirft deine ungespeicherten Änderungen an der Datei – vorsichtig!)

```bash
git log --oneline -10
```

(zeigt die letzten Commits)

---

## 12. Übungen

Mach sie der Reihe nach. Jede baut auf der vorherigen auf.

1. **Bits:** Was ist `(0x4510 >> 8) & 0xF`? Was ist `0x4510 & 3`? Was ist
   `0x4510 & 0x4000`? (Lösung: 5, 0, 0x4000)
2. **printf:** Füge in `print_status` (`source/main.c`) eine Zeile hinzu, die Leons
   Blickrichtung in Grad zeigt: `player.dir * 360 / 32768`. Bauen, ansehen, wieder
   entfernen.
3. **Konstante ändern:** Setze in `enemy.c` `LICK_GRAVITY` testweise auf 20 und
   beobachte im `enemy_host` (`MEMBERS=10:1:0x17:1 TRACE=1 TICKS=60`), wie viel später
   der Licker landet. Danach zurücksetzen.
4. **Skript lesen:** Zerlege `ROOM1000.RDT` und finde alle `sce_em_set`. Wie viele Zombies
   setzt der Raum, in welchen Slots?
5. **Ghidra:** Öffne `scd_op_6f` (`0x80054a44`) und `FUN_80031494`. Finde die Tabelle
   `0x8009a578` und lies die Einträge (32 Bytes pro Eintrag).
6. **Daten lesen:** Schreib ein Python-Skript, das aus `CORE00.ESP` für Effekt 2 die
   Bewegungsdaten ausgibt und `0x0102` findet (Hinweis: Fall 2).
7. **Kleiner eigener Fix:** Such dir in `docs/HANDOFF.md` einen offenen Punkt aus, der
   klein klingt, und arbeite ihn mit dem Ablauf aus Teil 10 ab.

---

## 13. Spickzettel

```
Bauen:        make DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
Gegner-Test:  cc -O1 -Itests/stubs -Isource tests/enemy_host.c source/script.c source/enemy.c source/plc.c -o build/enemy_host -lm
              TRACE=1 TICKS=300 ./build/enemy_host nitro:/room/room1000.bin
Szene-Test:   TRACE=1 ./build/cutscene_host nitro:/room/roomXXXX.bin <event> [ticks]
Skript:       python3 -I tools/scd_disasm.py build/disc1/PL0/RDT/ROOMXXXX.RDT
Raumkarte:    python3 -I tools/rdt2json.py ROOM.RDT room.json && python3 -I tools/room_map.py room.json map.png

Winkel:       RE2 4096 = 360°, DS 32768 = 360°, DS = RE2 * 8, & 32767
Festkomma:    4096 = 1.0, (a * b) >> 12
y:            nach oben negativ, Etage = 1800
Takt:         25 Ticks/s (PAL), 1 Keyframe pro Tick
Hex:          0x10 = 16, 0x100 = 256, 0x1000 = 4096
Bits:         (x >> n) & maske holt Bits heraus, x | bit setzt, x & ~bit löscht

Sound-Banken: 0 Tür, 1 Waffe (ARMSnn), 2 Raum, 3 Gegner, 4 Leon (CORE)
Schaden:      PS1 nimmt Werte < 41 mal 1,5 (FUN_8003cf38)
```

Viel Erfolg! Fang klein an, ändere eine Sache auf einmal und teste nach jeder Änderung.

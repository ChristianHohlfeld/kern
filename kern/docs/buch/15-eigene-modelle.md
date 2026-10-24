# 15 Eigene Modelle trainieren

Dieses Kapitel ist die Anleitung: ein Netz mit dem Lehrer trainieren, seinen Fortschritt lesen, das
beste auswählen, im Studio benutzen und eine neue Aufgabe hinzufügen. Wie der Lehrer intern rechnet
(MLP, PPO, DAgger, CMA-ES, Determinismus), steht in [Kapitel 14](14-lehrer.md); die allgemeinen
Grundlagen neuronaler Netze und des Reinforcement Learning in
[Anhang A](A-grundlagen-neuronale-netze.md). Hier verweisen die Abschnitte jeweils auf die Stelle im
Code und in Kapitel 14, die erklärt, was ein Schalter bewirkt.

## 15.1 Das Werkzeug bauen

```
kern/tools/lehrer.sh              # baut kern/build/kern_lehrer
kern/tools/lehrer.sh /pfad/out    # baut /pfad/out/kern_lehrer
```

Das Skript läuft auf Linux und macOS, auf x86 und ARM. Es übersetzt Box3D, die deterministische libm
(`third_party/musl_math`) und alle Quellen unter `src/` außer `studio/` und `app/` mit
`-O3 -DNDEBUG -ffp-contract=off`, Box3D zusätzlich mit `-DBOX3D_DISABLE_SIMD`, und linkt sie mit
`tools/kern_lehrer.cpp`. Es baut nur neu, was älter ist als seine Quelle oder der neueste Header.

| Umgebungsvariable | Wirkung |
|---|---|
| `CC`, `CXX` | Compiler (Standard `cc`, `c++`) |
| `KERN_JOBS` | parallele Compiler (Standard: Zahl der Kerne) |
| `KERN_NATIVE` | 1 (Standard): `-march=native` bzw. `-mcpu=native`, wenn der Compiler es kennt; 0: ohne |

Die CPU-Schalter wählen nur Befehle aus, sie ändern kein Ergebnis: Das Netz bleibt Bit für Bit
dasselbe wie im Browser und im Studio ([Kapitel 14](14-lehrer.md#147-determinismus)). Wie Kern selbst
gebaut wird, steht in [Kapitel 2](02-bauen-und-testen.md).

Beim Start lädt `kern_lehrer` Pollens Netze aus `third_party/microduck/` (oder `--policies DIR`); sie
werden für Warmstart (velstand), Vorbild und Spielschicht (Kick, Aufheben) gebraucht.

## 15.2 Alle Befehle

| Befehl | Zweck |
|---|---|
| `--task jump\|stand [Optionen]` | PPO-Training einer Bewegung (mit Rezept, Abschnitt 15.3) |
| `--eval netz.onnx [--episodes 10]` | Sprung messen: sauber und mit voller Variation |
| `--trace netz.onnx\|builtin\|demo.json [--task jump] [--randomized]` | eine Episode Takt für Takt: Höhe, Kontakte, Belohnungsterme |
| `--vorbild-suchen demo.json [--iterations 120] [--vorbild start.json]` | Sprung-Vorbild mit CMA-ES suchen |
| `--vorbild-pruefen builtin\|demo.json [--episodes 10]` | wie ein Vorbild springt |
| `--spielen [--nachahmen 8] [--episoden 64] [--out play.onnx]` | Spielschicht durch Nachahmen (DAgger) |
| `--spielen-pruefen vorbild\|play.onnx [--episodes 10]` | wie Vorbild oder Netz spielen |
| `--spielen-ueben play.onnx [--iterations 300] [--envs N] [--uebungs-std 0.1] [--out play_geuebt.onnx]` | Spielschicht mit PPO üben |
| `--spielen-spur [--seed N] [--randomized] [--netz play.onnx]` | eine Spielepisode Entscheidung für Entscheidung |
| `--biss-fenster` | wo der Schnabel eine Socke greift (Gitter) |
| `--halt-messen` | wie weit die Ente nach „Stopp“ noch geht |
| `--demo-spur [--netz play.onnx] [--seed SEKUNDEN]` | das Beispiel-Level wie im Studio, einmal pro Sekunde eine Zeile |
| `--fingerprint 400` | Determinismus-Fingerabdruck |
| `--kalibrieren`, `--abgleich`, `--beispiele`, `--profil` | Kalibrierung ([Kapitel 16](16-kalibrierung.md)) |

Allgemeine Optionen: `--threads N` (Standard: alle Kerne), `--seed N` (Standard 1).

## 15.3 Den Sprung trainieren

### Der Standardlauf

```
kern/build/kern_lehrer --task jump --iterations 3000 \
    --out jump.onnx --log jump.csv --checkpoint run.ckpt --keep-every 250
```

Das ist der Weg, auf dem `policies/jump.onnx` entstand (32 Enten auf 2 Kernen, Seed 1,
3000 Iterationen; `docs/lehrer.md`). Was passiert:

1. **Rezept laden** (`recipe_for("jump")`): Warmstart von velstand, Nachahmung des eingebauten
   Vorbilds (8 Runden à 96 Episoden), danach Üben mit Aktionsrauschen 0,1, Lernrate 10⁻⁴,
   Nachahmungsbelohnung 10. Kommandozeilenoptionen überschreiben einzelne Werte.
2. **Enten anlegen:** `--envs` (Standard 16 je Thread) Enten in `--threads` Gruppen
   (`ThreadBackend`). Die Zahl wird auf ein Vielfaches der Threads abgerundet
   (`envs = (envs / threads) · threads`).
3. **Nachahmen** (DAgger, [Kapitel 14.6](14-lehrer.md#146-nachahmen-vorbilder-und-cma-es)). Danach
   schreibt das Werkzeug `jump_nachgeahmt.onnx` (was die Nachahmung allein ergibt) und mit
   `--checkpoint` den Lerner nach `run.ckpt.nachgeahmt`.
4. **Üben** (PPO, [Kapitel 14.5](14-lehrer.md#145-ppo-schritt-für-schritt-ppocpp)): die ersten
   30 Iterationen lernt nur der Kritiker, danach beide. Alle `--eval-every` (10) Iterationen springt
   das Netz 5 Episoden auf dem sauberen Roboter.
5. **Speichern:** alle `--save-every` (25) Iterationen und am Ende das ONNX nach `--out` und den
   Lerner nach `--checkpoint`; alle `--keep-every` Iterationen zusätzlich einen Schnappschuss
   `jump_it00250.onnx` usw.

Bei der ersten Bewertung mit mindestens 4 von 5 echten Sprüngen erscheint
`FIRST REAL JUMP at iteration …` und das Netz wird sofort gespeichert. Mit `--stop-after-jump` endet
der Lauf dort.

### Fortsetzen

```
kern/build/kern_lehrer --task jump --iterations 6000 --resume run.ckpt --checkpoint run.ckpt --out jump.onnx
```

`--resume` lädt den ganzen Lerner (Netze, Normalisierer, σ, Adam-Zustände, Lernrate, Iteration) und
überspringt die Nachahmung. Die Iterationen zählen weiter; `--iterations` ist das Ziel, nicht die
Zahl der zusätzlichen Iterationen. Die Log-Datei wird dann fortgeschrieben statt neu angelegt.
Mit `run.ckpt.nachgeahmt` als `--resume` beginnt das Üben neu von der Nachahmung aus.

### Ohne Vorbild

```
kern/build/kern_lehrer --task jump --vorbild none --iterations 3000
```

Reines PPO; die Belohnung bekommt dann den Term `track_height` (eine Referenzhöhe über die Phase).
`docs/lehrer.md` dokumentiert, dass dieser Weg den Absprung nicht fand
([Kapitel 14.6](14-lehrer.md#146-nachahmen-vorbilder-und-cma-es)). Für Experimente:
`--vorbild demo.json` nimmt ein eigenes Vorbild, `--nachahmen 5 --episoden 64` ändert die DAgger-
Runden, `--uebungs-std 0.2` das Rauschen beim Üben, `--nachahmungs-lohn 0` schaltet die
Nachahmungsbelohnung ab.

### Ein eigenes Vorbild suchen

```
kern/build/kern_lehrer --vorbild-suchen demo.json --iterations 120
kern/build/kern_lehrer --vorbild-suchen demo2.json --vorbild demo.json --iterations 120   # weiter von demo.json
kern/build/kern_lehrer --vorbild-pruefen demo2.json --episodes 20
kern/build/kern_lehrer --task jump --vorbild demo2.json --out jump2.onnx
```

Je Generation erscheint eine Zeile mit der Bewertung und der Messung des Suchzentrums auf dem
sauberen Roboter (Höhe, Steigen, Flugzeit, Neigung, „a real jump“ / „no real jump“ / „fell“). Das
eingebaute Vorbild entstand in drei Stufen mit zusammen 370 Generationen; die erste Stufe dauerte
auf 2 Kernen rund 18 Minuten für rund 2 Millionen Entenschritte (`docs/lehrer.md`).

### Im Browser

Die Seite `/lehrer/` trainiert dasselbe Rezept mit Web Workern
([Kapitel 14.11](14-lehrer.md#1411-der-lehrer-im-browser)). Gleicher Seed und gleich viele Enten
insgesamt (Worker × Enten je Worker) ergeben dasselbe Netz wie nativ.

## 15.4 Fortschritt lesen

Jede Übungsiteration schreibt eine Zeile in diesem Format (Zahlen hier als Platzhalter):

```
it <N>  <Sekunden> s  return <R>  len <L>  value <V>  std <σ>  lr <lr>  <S> steps/s (collect <c>s, learn <l>s)  | jumps <j>/5 up <h> mm rise <r> mm flight <f> s tilt <t> falls <k>
```

Der Teil ab `|` erscheint nur in Iterationen mit Bewertung.

| Feld | Bedeutung | Worauf achten |
|---|---|---|
| `return` | mittlere Rückgabe der Episoden, die in dieser Iteration endeten | soll über Hunderte Iterationen steigen; schwankt von Iteration zu Iteration |
| `len` | ihre mittlere Länge in Regeltakten | Sprung: 100 = 2 s ohne Sturz; kürzer heißt, Episoden enden mit Sturz oder Bodenkontakt |
| `value` | Wertverlust des Kritikers | hoch nach der Nachahmung (der Kritiker kennt die Erträge noch nicht), soll dann fallen |
| `std` | mittleres Aktionsrauschen σ | sinkt, wenn die Policy sicherer wird; 0,001 ist die Untergrenze |
| `lr` | Lernrate nach der KL-Anpassung | fällt sie dauerhaft auf 1e-5, ändert sich die Policy je Schritt zu stark |
| `steps/s` | Entenschritte je Sekunde (Sammeln + Lernen) | Gerätegeschwindigkeit |
| `jumps n/5 …` | Bewertung auf dem sauberen Roboter | das eigentliche Ziel |

Die Nachahmung schreibt je Runde:

```
  round <r> (<Sekunden> s): <n> samples, error <mse> | the network: <j> of 5 episodes a real jump, ...
```

`samples` sind alle bisher beschrifteten Takte, `error` der mittlere quadratische Fehler je Aktion
der letzten Epoche.

Mit `--log jump.csv` steht jede Iteration als CSV-Zeile mit den Spalten

```
iteration,steps,seconds,mean_reward,mean_return,mean_length,episodes,value_loss,entropy,mean_std,learning_rate,steps_per_second,jumps,height_mm,flight_s,falls
```

`jumps` ist −1 in Iterationen ohne Bewertung. Die Datei lässt sich direkt in eine Tabellenkalkulation
oder ein Plotprogramm laden.

**Wenn etwas schiefgeht,** zeigt `--trace` eine Episode Takt für Takt:

```
kern/build/kern_lehrer --trace jump_it01000.onnx
kern/build/kern_lehrer --trace builtin                 # das Vorbild selbst
kern/build/kern_lehrer --trace jump.onnx --randomized  # mit voller Variation
```

Jede Zeile: Takt, Rumpfhöhe über dem Start (mm), welche Sohle Kontakt hat, ob ein anderes Teil den
Boden berührt, Aufrecht-Wert, Neigung x/y und alle Belohnungsterme ungleich 0. So sieht man, welcher
Term die Rückgabe trägt – und ob die Policy die Belohnung ausnutzt, statt zu springen.

## 15.5 Das beste Netz auswählen

Die zuletzt gespeicherte Datei ist nicht immer die beste. PPO kann nach einem guten Stand wieder
schlechter werden; die Bewertung während des Laufs hat nur 5 saubere Episoden.

1. Mit `--keep-every N` Schnappschüsse behalten.
2. Jeden Kandidaten mit mehr Episoden messen, sauber **und** variiert:

   ```
   for f in jump_it*.onnx; do kern/build/kern_lehrer --eval "$f" --episodes 40; done
   ```

   `--eval` gibt zwei Zeilen aus: „clean robot“ (Seed 1000) und „randomised: build, servos, sensors,
   start“ (Seed 5000, volle Variation).
3. Wählen nach: echte Sprünge mit Variation, dann Stürze (0), dann Steigen, Flugzeit und Neigung.
4. Den Kandidaten im Studio prüfen (Abschnitt 15.8).

Für `policies/jump.onnx` ergab diese Messung 40 von 40 sauber und 36 von 40 mit voller Variation,
ohne Sturz ([Kapitel 14.9](14-lehrer.md#149-ergebnisse-des-sprungs)).

## 15.6 Hyperparameter wählen

### Eine Iteration in Zahlen

Um zu sehen, was ein Schalter bewirkt, hilft es, eine Iteration des Sprung-Rezepts mit 32 Enten
durchzurechnen (alle Schritte in [Kapitel 14.5](14-lehrer.md#145-ppo-schritt-für-schritt-ppocpp)):

| Schritt | Rechnung |
|---|---|
| Sammeln | 32 Enten × 48 Takte = 1536 Zeilen; je Zeile a = μ + σ·ε mit σ = 0,1 nach der Nachahmung |
| Normalisierer | der Kritiker-Normalisierer nimmt die 1536 Zeilen auf; der Actor-Normalisierer ist eingefroren (Warmstart) |
| GAE | je Ente rückwärts über 48 Takte mit γ = 0,99, λ = 0,95; danach Vorteile auf Mittel 0, Std 1 |
| Minibatches | 1536 / 4 = 384 Zeilen je Minibatch, jede in 8 feste Teilstücke à 48 Zeilen |
| Updates | 5 Epochen × 4 Minibatches = 20 Adam-Schritte für Actor (mit σ) und Kritiker |
| Lernrate | vor jedem der 20 Schritte: KL messen, bei KL > 0,02 durch 1,5, bei KL < 0,005 mal 1,5 |

`--steps` und `--envs` ändern die Zahl der Zeilen und damit die Minibatch-Größe; `--epochs` und
`--minibatches` die Zahl der Schritte je Iteration; `--kl` die Größe, die jeder Schritt höchstens
erreichen soll. Die Nachahmung rechnet dagegen überwacht: 8 Runden à 96 Episoden zu höchstens
100 Takten ergeben beim Sprung höchstens 76 800 beschriftete Beispiele; gemessen waren es 49 936,
weil eine Episode früher endet, sobald die Ente fällt oder ein anderes Teil als die Sohlen den Boden
berührt. Nach jeder Runde wird der Actor in 10 Epochen mit Minibatches zu 256 an alle bisherigen
Beispiele angepasst.

### Die Schalter

Alle PPO-Werte stehen im Rezept; die Kommandozeile überschreibt sie einzeln.

| Option | Feld | Sprung-Rezept | Wirkung (Kapitel 14.5) |
|---|---|---|---|
| `--envs N` | Enten insgesamt | 16 je Thread | mehr Daten je Iteration, kleineres Rauschen im Gradienten; **bestimmt das Ergebnis** |
| `--threads N` | Threads | alle Kerne | nur die Geschwindigkeit; das Ergebnis hängt nicht davon ab |
| `--steps N` | `steps_per_env` | 48 | Takte je Ente und Iteration; Zeilen je Iteration = Enten × Schritte |
| `--epochs N`, `--minibatches N` | | 5, 4 | Durchläufe über die Daten, Teilung in Minibatches |
| `--lr X` | `learning_rate` | 10⁻⁴ | Startlernrate; die adaptive Regel passt sie danach an |
| `--adaptive-lr 0\|1`, `--kl X` | | an, 0,01 | Ziel-KL je Minibatch |
| `--init-std X` | `init_std` | 0,3 | Aktionsrauschen am Anfang (vor der Nachahmung) |
| `--uebungs-std X` | `practice_std` | 0,1 | Aktionsrauschen beim Üben nach der Nachahmung |
| `--entropy X` | `entropy_coef` | 0,005 | hält das Rauschen davon ab, zu schnell zu fallen |
| `--actor 512,256,128` | `actor_hidden` | 512, 256, 128 | Netzgröße des Actors; beim Warmstart muss sie zu velstand passen |
| `--critic 256,128` | `critic_hidden` | 256, 128 | Netzgröße des Kritikers |
| `--warm-start velstand\|none` | `warm_start` | `velstand` | Start vom velstand-Netz |
| `--substeps N` | Box3D-Sub-Steps | 16 | nicht ändern (siehe unten) |
| `--profil servo.json` | Servoprofil | XL330 | Training mit kalibrierten Servos ([Kapitel 16](16-kalibrierung.md)) |

Hinweise aus den Läufen in `docs/lehrer.md`:

- **Nach der Nachahmung vorsichtig:** kleine Lernrate (10⁻⁴) und kleines Rauschen (0,1). Ein Actor,
  der schon gut handelt, wird durch große Schritte und großes Rauschen leicht zerstört; deshalb auch
  „Kritiker zuerst“.
- **Größeres Rauschen hilft nicht beim Finden.** Mit großem Rauschen und ohne Warmstart lernte die
  Ente zu stehen, nicht zu springen.
- **Wenige Enten bremsen.** Mit wenigen Dutzend Enten sind die Minibatches klein, der Gradient
  verrauscht, und die KL-Grenze hält die Lernrate unten.
- **Sub-Steps nicht senken.** 8 statt 16 Sub-Steps wären 1,6-mal schneller, verändern aber Gang und
  Bewegungen messbar. Ein Netz, das mit anderen Sub-Steps lernt, lernt eine andere Physik als die des
  Studios.

Zu `--warm-start`: `make_learner` wertet nur den Wert `velstand` aus; jeder andere Wert startet mit
zufällig initialisierten Gewichten. Einen Warmstart von einer Datei gibt es nur für die Spielschicht
(`--spielen-ueben`).

## 15.7 Die Spielschicht trainieren

### 1. Messen, was das Vorbild braucht

```
kern/build/kern_lehrer --biss-fenster
kern/build/kern_lehrer --halt-messen
```

`--biss-fenster` legt eine Socke auf ein Gitter von 11 × 9 Stellen vor eine stehende Ente, lässt
Pollens Aufheben mit offenem Schnabel im Fenster der Phase [0,04, 0,30) laufen und druckt je Stelle,
ob die Socke hochkam und ob sie nach 3 s Wegtragen noch im Schnabel war, dazu die beste Stelle:

```
sock centre (ahead, left, real m): held at the end of the pick / still held after carrying 3 s
+0.045 -0.020  0 0
...
best: +0.0xx ahead, +0.0xx left (n kept around it)
```

Dieselbe Messung macht das Training selbst (`bite_spot`), einmal pro Prozess vor den ersten
Episoden; `--spielen` druckt das Ergebnis. Der Befehl ist zum Nachsehen da, etwa nachdem sich Reibung,
Sockenmodell oder Schnabel geändert haben.

`--halt-messen` lässt die Ente aus 0,10, 0,12, 0,15 und 0,20 m/s je viermal anhalten und druckt, wie
weit sie danach noch voraus und seitlich ging.

### 2. Nachahmen

```
kern/build/kern_lehrer --spielen --nachahmen 12 --episoden 32 --out play.onnx --checkpoint play.ckpt
```

Standard sind 8 Runden à 64 Episoden; `policies/play.onnx` vor der Überarbeitung entstand mit
12 Runden à 32 Episoden zu je 60 s (`docs/lehrer.md`). Je Runde misst das Werkzeug das Netz in
16 Episoden auf dem sauberen Roboter:

```
  round <r> (<Sekunden> s): <n> samples, error <mse> | the network (clean robot): ball kicked away <k> of <b> (<t> s), sock picked up <p> / carried off and spat out <d> of <s> (<t> s; <h> of <a> picks held), fell <f> of 16
```

**Achtung:** `--out` wird nach **jeder** Runde überschrieben. Wer eine frühere Runde behalten will,
kopiert die Datei während des Laufs (für `play.onnx` vor der Überarbeitung war Runde 10 von 12 die
beste). `--checkpoint` speichert den Lerner nach jeder Runde.

### 3. Prüfen

```
kern/build/kern_lehrer --spielen-pruefen vorbild --episodes 48
kern/build/kern_lehrer --spielen-pruefen play.onnx --episodes 32
```

Zwei Zeilen, sauber und variiert: Bälle weggekickt (und mittlere Zeit), Socken aufgehoben, Socken
weggetragen und ausgespuckt (Zeit), wie viele Griffe hielten, Stürze. Das Vorbild zu messen zeigt,
was das Netz höchstens erreichen kann, wenn es nur nachahmt.

### 4. Eine Episode ansehen

```
kern/build/kern_lehrer --spielen-spur --seed 3
kern/build/kern_lehrer --spielen-spur --seed 3 --randomized --netz play.onnx
```

Je Entscheidung (25 Hz) eine Zeile: Ball und Socke, wie der Tracker sie kennt, und wo sie wirklich
liegen; linker Fuß, hält, Aufheben und Phase, Kick, getragene Strecke; die neun Aktionen des Vorbilds
und mit `--netz` die des Netzes daneben (dann fährt das Netz); Stillstand und Gehzeit; wie weit die
Socke bewegt wurde, Bodenkontakt, welches Netz fährt, Zustand, Schwerkraft. Am Ende das Ergebnis der
Episode. Weichen Vorbild und Netz an einer Stelle ab, sieht man hier, wo.

### 5. Üben

```
kern/build/kern_lehrer --spielen-ueben play.onnx --iterations 300 --envs 32 --out play_geuebt.onnx
```

Warmstart von `play.onnx`, Aktionsrauschen 0,1 (`--uebungs-std`), 20 Iterationen nur Kritiker, dann
PPO mit der Belohnung der Spielschicht ([Kapitel 14.8](14-lehrer.md#148-die-spielschicht-lernen)). Alle
20 Iterationen misst es das Netz in 16 sauberen Episoden und schreibt `play_geuebt_it0020.onnx` usw.
sowie `--out`.

### 6. Im Beispiel-Level prüfen

```
kern/build/kern_lehrer --demo-spur --netz play_geuebt.onnx --seed 120
```

`--demo-spur` spielt das Micro-Duck-Level des Studios mit 60-Hz-Frames, wie das Studio es spielt (hier
120 s; `--seed` ist bei diesem Befehl die Dauer in Sekunden), mit `kern/policies/jump.onnx` und dem
angegebenen Spielnetz (sonst `kern/policies/play.onnx`). Einmal pro Sekunde: Position, Zustand,
fahrendes Netz, was der Tracker über Ball und Socke weiß, hält, Stillstand, Gehzeit und die Antwort
des Spielnetzes auf diese Beobachtung (`net fwd`, `turn`, `kick`, `pick`). Mit der
Umgebungsvariable `KERN_JUMP_AT=SEKUNDE` drückt es in dieser Sekunde Taste 7 (Sprung).

## 15.8 Im Studio benutzen

Kerns eigene Netze haben feste Dateinamen: `jump.onnx` und `play.onnx` (`KERN_POLICIES` in
`web/studio/game.mjs`). So kommt ein eigenes Netz ins Studio:

1. **Ins Repository:** die Datei nach `kern/policies/jump.onnx` bzw. `kern/policies/play.onnx`
   kopieren. Die Entwicklerseite lädt Kerns Netze aus `kern/policies/`, eine ausgelieferte Seite aus
   `policies/` neben der Seite. Die Engine fragt fehlende Netze an, sobald eine Ente sie braucht.
2. **Aus dem Browser-Lehrer:** Die Lehrer-Seite speichert ihr Netz auf demselben Gerät (IndexedDB
   „kern-lehrer“). Das Studio lädt es beim Start und übernimmt jedes neue sofort, auch in offenen
   Tabs. Das gilt für den Sprung (die Seite trainiert keine Spielschicht).
3. **Prüfen:** Micro-Duck-Level öffnen, Play, Taste **7** (Sprung) bzw. die Ente in Ruhe lassen
   (Spielschicht: Autonomous und Kern extras an, Ball oder Socke in Sicht). Die Browser-Konsole meldet
   „Kern: trained network jump.onnx loaded“ für ein Netz aus dem Lehrer und eine Warnung, wenn eine
   Datei nicht lädt.

Das Netz muss die Form eines rsl_rl-Actors haben (Sub, Div, Gemm/Elu …, Gemm); der Lehrer exportiert
genau so. Die Spielschicht prüft zusätzlich die Größen: Ein Netz, das nicht 23 Eingänge und
9 Ausgänge hat, wird nicht gerechnet (die Aktion bleibt 0).

## 15.9 Eine neue Aufgabe hinzufügen

Eine neue Bewegung mit dem Layout von Pollens Netzen (61 Beobachtungen → 14 Aktionen) braucht vier
Dinge: eine `Task`, einen Eintrag in den Rezepten, eine Messung und die Verdrahtung im Studio. Ein
Vorbild ist optional, aber für schwere Bewegungen oft nötig.

### 1. Die `Task` schreiben

In `src/lehrer/tasks.cpp` (oder einer eigenen Datei unter `src/lehrer/`, die `lehrer.sh` automatisch
mit übersetzt) eine Klasse von `Task` ableiten. Gerüst nach dem Muster von `JumpTask`:

```cpp
class BowTask : public Task {
 public:
  std::string name() const override { return "bow"; }
  f32 episode_seconds() const override { return 2.0f; }
  i32 privileged() const override { return 4; }      // nur der Kritiker sieht diese

  Start reset(Rng& rng) override {
    Start s;
    s.yaw = rng.uniform(-180, 180);
    s.roll = rng.uniform(-3, 3);
    s.pitch = rng.uniform(-3, 3);
    s.foot_friction = rng.uniform(0.7f, 1.3f);
    return s;
  }
  void command(f32 o[kObs], i32 step) override {      // Befehlsplätze 48..60
    for (i32 i = 48; i < kObs; ++i) o[i] = 0;
    const f32 a = 6.28318531f * step * kControlDt / episode_seconds();
    o[55] = kern::dm::cos(a);                           // Phase in den freien Plätzen 55/56
    o[56] = kern::dm::sin(a);
  }
  void privileged(const game::GameWorld::WalkerProbe& p, f32* out) override {
    out[0] = p.position.z; out[1] = p.velocity.z;
    out[2] = p.foot_contact[0] ? 1.0f : 0.0f; out[3] = p.foot_contact[1] ? 1.0f : 0.0f;
  }
  void progress(i32 iteration) override {             // Lehrpläne
    fall_ = -2.0f - 18.0f * clamp(iteration / 300.0f, 0.0f, 1.0f);
  }
  f32 reward(const Tick& t, bool& terminal) override {
    Terms r(*this);                                     // weight x term x dt, mit Namen für --trace
    const auto& p = t.probe;
    r.add("upright", 2, upright(p));
    r.add("action_rate", -0.1f, action_rate(t));
    // ... die eigentliche Aufgabe ...
    terminal = fell(p) || p.body_contact;
    if (terminal) r.add("fall", fall_, 1, false);       // einmalig, nicht x dt
    return r.sum();
  }
 private:
  f32 fall_ = -2;
};
```

`Terms`, `upright`, `pose_match`, `action_rate` und `fell` sind Hilfsfunktionen in `tasks.cpp`.
Eine Fabrikfunktion `make_bow_task()` und ihre Deklaration in `env.h` machen die Klasse außerhalb
nutzbar.

### 2. Rezept und Namen

In `src/lehrer/recipe.cpp`:

```cpp
std::unique_ptr<Task> make_task(const std::string& name) {
  if (name == "jump") return make_jump_task();
  if (name == "stand") return make_stand_task();
  if (name == "bow") return make_bow_task();          // neu
  return nullptr;
}

Recipe recipe_for(const std::string& task) {
  ...
  if (task == "bow") {
    r.warm_start = "velstand";
    r.ppo.init_std = 0.3f;
    r.ppo.learning_rate = 1e-4f;
  }
  return r;
}
```

Damit kennen `kern_lehrer --task bow` und die C-Schnittstelle des Browsers die Aufgabe; auf der
Lehrer-Seite fehlt nur ein `<option>` in `web/lehrer/index.html`. Nutzt die Aufgabe Beobachtungsplätze,
die velstand nie gesehen hat, müssen sie beim Warmstart als neue Eingänge angegeben werden: In
`make_learner` übergibt Kern dafür derzeit die Phasenplätze 55 und 56 (`game::kJumpPhaseSlot` und
der nächste).

### 3. Messen

Ohne eine harte Messung lässt sich kein Netz auswählen. `JumpMeter` und `evaluate_jump` zeigen das
Muster: eine Episode Takt für Takt beobachten (`tick(probe)`), am Ende entscheiden, ob es gelang
(`finish`). Die Messung soll prüfen, was die Bewegung **ist**, nicht was die Belohnung zählt (siehe
Fallstricke). `--eval` und die Bewertung während des Laufs rufen derzeit `evaluate_jump` auf; für eine
neue Aufgabe kommt eine eigene Bewertung dazu.

### 4. Ein Vorbild (optional)

Findet PPO die Bewegung nicht, hilft ein Vorbild wie beim Sprung:

- eine kleine **Parametrisierung** (beim Sprung 15 Zahlen: drei Gelenke zu fünf Zeitpunkten, das
  andere Bein gespiegelt, Ziele über `amplitude · tanh`),
- eine **Bewertung**, die CMA-ES minimieren kann (`score_jump_demo` als Muster: belohnt die Bewegung,
  bestraft Neigung und Sturz, auf sauberen und variierten Robotern),
- ein **Lehrer-Objekt** mit `act(observation, probe, step, action)` (`JumpTeacher`), das für jeden
  Zustand eine Antwort hat – Voraussetzung für DAgger.

`search_jump_demo`, `dagger_episodes`, `JumpImitation` und `Recipe::demo` sind derzeit auf
`JumpDemo` und `JumpTeacher` zugeschnitten. Für eine neue Aufgabe mit Vorbild schreibt man die
entsprechenden Funktionen nach diesem Muster; `cmaes_minimise` und `Ppo::imitate` sind allgemein.

### 5. Im Studio verdrahten

Eine neue Bewegung läuft im Studio wie Pollens Kicks und der Sprung: harter Wechsel auf ihr Netz,
nach der Dauer zurück. Nötig sind:

| Wo | Was |
|---|---|
| `policy_set()` in `src/game/walker.cpp` | ein `SkillDef` wie `jump`: `name`, `file`, `duration`, `action_scale`, `phase_slot` und `period`, wenn das Netz eine Phase sieht |
| `PlayInput::robot` und `drive_walkers` | ein freies Bit und `walker_skill(w.actor, "bow")` beim Drücken |
| `src/studio/viewport.cpp` | eine Taste (wie `Digit7` → 256) |
| `src/game/touch.cpp` | ein Touch-Knopf (wie „HOP“) |
| `src/game/logic_nodes.cpp` | die Beschreibung von **Robot Move** um den Namen ergänzen; der Knoten ruft `walker_skill` mit dem Namen |
| `web/studio/game.mjs` | den Dateinamen in `KERN_POLICIES`, damit das Studio ihn aus `policies/` lädt |
| `kern/policies/` | die Datei selbst, dazu eine Zeile in `policies/README.md` |

Die Beobachtung, die das Studio dem Netz gibt, baut `run_controller` genau wie im Training: Kopf- und
Körperplätze 0, die Phase cos/sin in `phase_slot` und dem nächsten Platz.

### Eine Aufgabe mit anderem Layout

Die Spielschicht zeigt den Weg für ein Netz, das **nicht** 61 → 14 ist: eine eigene Ebene in der
Laufzeit (`walker_play.cpp`: Beobachtung, Ausführung, externer Zugang für den Lehrer), eine eigene
`Env`-Klasse (`PlayEnv`), ein eigener Lerner (`make_play_learner`), eigene Metadaten
(`play_metadata`) und eigene Bewertung (`evaluate_play`). Die PPO- und Nachahmungsmaschinerie
(`Ppo`, `Collector`, `ThreadBackend`, `Ppo::imitate`) ist dieselbe; sie kennt nur `Env`.

## 15.10 Beobachtung, Aktion und Belohnung entwerfen

Was die Netze in Kern vorleben:

- **Nur, was der Roboter wirklich hat.** Der Actor sieht Sensorwerte (IMU, Gelenke, Kamera-Detektor,
  Mund-Encoder), nie wahre Positionen. Die Spielschicht kennt Ball und Socke nur aus dem Detektor mit
  Rauschen und Aussetzern. Wahre Größen gehören in die privilegierte Beobachtung des Kritikers.
- **Zeit sichtbar machen.** Eine Bewegung mit Ablauf bekommt ein Phasensignal (cos, sin), keinen
  Zähler. Ein Netz ohne Gedächtnis braucht Zeitgrößen in der Beobachtung: Die Spielschicht sieht, wie
  lange sie steht (`still_s`), geht (`walking_s`) und trägt (`carried_s`).
- **Messen statt befehlen.** „Steht still“ heißt in der Spielschicht „beide Sohlen am Boden, keine
  Bewegung läuft“ – was die Beine tun, nicht was befohlen wurde. Der Gang setzt auf kleine Befehle
  manchmal nicht an; das ist Stehen.
- **Aktionen wie Bedienelemente.** Laufbefehle mit Totzone wie ein Stick (unter 20 % steht die Ente),
  Bewegungen als Schwelle (> 0,5 startet), Grenzen für Kopf und Nacken.
- **Belohnung aus Termen mit Namen.** Jeder Term einzeln sichtbar (`--trace`), mit Gewicht × Term ×
  dt. Regularisierer wie bei Pollen: Aktionsänderung, Drehrate, Eigenkontakt. Lehrpläne über
  `progress(iteration)`: Strafen und Variationsbereiche wachsen, wenn die Bewegung sich bildet.
- **Episodenende hart.** Sturz und Bodenkontakt anderer Teile beenden die Episode mit einem Malus;
  sonst wird Abstützen zur sicheren Wette.
- **Das Vorbild ohne verstecktes Wissen.** Soll das Netz ein Vorbild genau lernen, darf das Vorbild
  nur sehen, was das Netz sieht, und kein Gedächtnis haben, das sich nicht aus der Beobachtung ergibt.
  Ein Wunsch, der aus einer Dauer folgt, steigt als Rampe an statt als einzelner Impuls.

## 15.11 Fallstricke aus den echten Läufen

Aus `docs/lehrer.md`:

- **Die Belohnung wird ausgenutzt.** In den ersten PPO-Läufen saß die Ente auf dem Schwanz, machte
  einen Hechtsprung oder stützte sich ab. Jedes Mal half nur eine strengere Regel: Bodenkontakt eines
  anderen Teils beendet die Episode, Neigung kostet, die Belohnung gilt nur aufrecht.
- **Die Messung war zu locker.** Die erste Sprungmessung (Rumpf irgendwann 15 mm über Standhöhe,
  60 ms ohne Bodenkontakt) ließ eine Variante durch, bei der die Ente sich aufrichtete und die Füße
  anzog, mit 45° Kippen. Seitdem zählt nur ein echter Sprung: Steigen in der Luft über die
  Abhebehöhe, Flug, höchstens 30° Neigung, Landen und Stehen.
- **PPO allein findet seltene Bewegungen nicht.** Was alle Servos gleichzeitig am Anschlag braucht,
  entsteht durch zufälliges Erkunden praktisch nie. Ein Vorbild aus CMA-ES und Nachahmung lösten es.
- **Ein guter Actor und ein ahnungsloser Kritiker.** Ohne „Kritiker zuerst“ zerstören die Vorteile
  eines Kritikers, der die Erträge noch nicht kennt, einen nachgeahmten Actor.
- **Normalisierer wandern.** Bei wenigen Dutzend Umgebungen ändern sich die Normalisierer zwischen
  zwei Iterationen merklich; die alte Policy wird deshalb vor dem Update mit den aktuellen
  Normalisierern neu bewertet.
- **Fehler in der Welt statt im Netz.** Beim Training der Spielschicht kamen drei Fehler der Welt ans
  Licht: Die Socke kroch von selbst über den Boden (runde Stoffklumpen rollten; jetzt Rollwiderstand),
  der Ball rollte ewig (jetzt Rollwiderstand wie Gummi auf Holz), und das Gähnen des Leerlaufs
  öffnete beim Aufheben den Schnabel (eine laufende Bewegung zählt nicht mehr als Stillstehen). Ein
  Netz, das an einer Stelle nicht lernt, kann auf einen solchen Fehler zeigen; `--spielen-spur`
  und `--trace` zeigen ihn.
- **Zahlen von Hand veralten.** Die Bissposition stand früher als gemessene Zahl in der Dokumentation;
  jetzt misst der Lehrer sie bei jedem Lauf in der aktuellen Physik (`bite_spot`).
- **Determinismus brechen.** Ein `std::sin` statt `kern::dm::sin`, eine Summe in Thread-Reihenfolge
  oder `-ffast-math` – und das Netz ist auf einem anderen Gerät ein anderes
  ([Kapitel 14.7](14-lehrer.md#147-determinismus)). `kern_lehrer --fingerprint 400` gegen
  `tests/determinism_fingerprint.txt` zeigt es.
- **Andere Physik als das Studio.** Weniger Sub-Steps, ein anderer Boden, ein anderes Servoprofil:
  Das Netz lernt dann eine andere Welt. Der Lehrer nutzt deshalb dieselbe `GameWorld` und dieselben
  Einstellungen wie das Studio.

---

© 2026 Christian Heinrich Hohlfeld · Alle Rechte vorbehalten · [Impressum und Rechte](00-impressum.md)

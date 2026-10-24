# Der Lehrer: eigene Netze für den Micro Duck

Kern trainiert Policies für den Micro Duck selbst. Alles läuft in C++ auf Kerns eigener Engine und
Box3D: Simulation, Lernverfahren, Export. Es gibt keine Python- oder PyTorch-Abhängigkeit. Gelernt
wird beim Nutzer: nativ auf allen Kernen (auch headless auf einem Server), im Browser auf Desktop
und Handy und später direkt im Studio.

Die Ente im Lehrer ist dieselbe wie im Studio: dieselbe GameWorld, dieselben Servos (Gerätemodell,
`docs/kalibrierung.md`), dieselben Sensoren, derselbe 50-Hz-Regler, dieselbe Physik (16 Substeps).
Ein Netz, das hier lernt, läuft deshalb unverändert im Studio. Es wird im Format von Pollens Netzen
exportiert (61 Beobachtungen, 14 Aktionen) und kann wie diese auf den Roboter.

## Stand

| Schritt | Stand |
|---|---|
| 1. Headless-Simulation: Ente ohne GPU, extern gesteuert, Episoden | fertig |
| 2. MLP mit Backprop und Adam | fertig |
| 3. PPO (wie rsl_rl: GAE, Clip, adaptive Lernrate, asymmetrischer Critic) | fertig |
| 4. ONNX-Export im Format von Pollens Netzen | fertig |
| 5. Variation (Domain Randomization) wie Pollens Rezepte, nativ auf allen Kernen | fertig |
| 6. Lernen im Browser (Web Worker, Desktop und Handy) | fertig |
| 7. Der Sprung: Vorbild → Nachahmen → Üben; als Bewegung im Studio | fertig |
| 8. Determinismus: Bit für Bit auf jeder Plattform und mit jeder Zahl Worker | fertig |
| 9. Digitaler Zwilling: Gerätemodelle, Kalibrierung, Abgleich (`docs/kalibrierung.md`) | fertig (ohne echte Messdaten) |
| 10. Spielen: Ball kicken, Socke aufheben, wegtragen, ausspucken – ein gelerntes Netz (Abschnitt 8) | Ball fertig; Socke gelernt, Erfolgsquote niedrig |

## 1. Die Umgebung (`src/lehrer`)

- **`DuckEnv`:** eine Ente auf ebenem Boden in einer eigenen GameWorld.
  - Der Renderer bleibt headless (`Renderer::set_headless`): Meshes bleiben auf der CPU, GL wird nie
    angefasst.
  - `reset()`, dann im Takt `observation()` / `step(action)`.
  - Ein `step` ist ein Regeltakt: 20 ms Roboterzeit, 4 feste Schritte der Welt.
- **Externe Steuerung:** `set_walker_external` und `set_walker_action`. Der Regler nimmt die Aktion
  des Lehrers statt eines Netzes. Die Servos bekommen sie wie trainiert: Grundstellung + Aktion,
  Skalierung 1, kein Tiefpass, mit der Totzeit des Servoprofils.
- **Beobachtung:** `walker_observation` liefert zwischen zwei Takten genau, was der Regler beim
  nächsten Takt sieht (Test `micro_duck_observation_between_ticks_is_what_its_controller_sees`).
  Die Befehlsplätze 48–60 schreibt die Aufgabe (einheitliches Layout, wie Pollens Fertigkeiten).
- **Messwerte für Belohnung und Critic:** `walker_probe`, in Robotereinheiten (nicht in der
  4-fachen Weltgröße): Rumpf-Lage und -Geschwindigkeit, Sohlen am Boden, Fußhöhe, Eigenkontakt,
  Bodenkontakt anderer Teile.
- **Prüfung:** Pollens velstand-Netz hält die Ente über diesen Weg stehend; gleicher Seed, gleiche
  Episode, Bit für Bit (Test `lehrer_duck_env_runs_episodes_headless`).

## 2. Netze, PPO, Nachahmung (`nn.cpp`, `ppo.cpp`)

- **`Mlp`:** ELU zwischen den Schichten, lineare Ausgabe, wie rsl_rls Actor und Critic. Gewichte
  eingangsweise, Initialisierung wie PyTorchs `nn.Linear`. Test: Gradient gegen finite Differenzen.
- **`Adam`** mit PyTorchs Standardwerten, Gradient vorher auf Norm 1 begrenzt.
- **`Normalizer`** wie rsl_rls EmpiricalNormalization; der Export schreibt ihn als Sub/Div ins ONNX.
- **PPO wie rsl_rl:** Gauß-Actor mit lernbarer Standardabweichung, asymmetrischer Critic, GAE
  (γ 0,99, λ 0,95), geclippter Surrogate und Wertverlust, Entropie, adaptive Lernrate über die KL.
  Vor dem Update wird die alte Policy mit den aktuellen Normalisierern neu bewertet (bei einigen
  Dutzend Umgebungen wandern die Normalisierer sonst merklich).
- **Nachahmung (`Ppo::imitate`):** Verhaltensklonen des Actors (mittlerer quadratischer Fehler,
  Adam, Minibatches in fester Reihenfolge).
- **Kritiker zuerst (`set_actor_frozen`):** Nach der Nachahmung lernt in den ersten 30 Iterationen
  nur der Critic. Sonst steuern Vorteile eines Critics, der die Erträge noch nicht kennt, einen Actor,
  der schon gut handelt, und zerstören ihn.
- **Warmstart:** Der Actor beginnt als Pollens velstand-Netz (Gewichte und eingefrorener
  Normalisierer). Die Plätze des Sprung-Phasensignals bekommen Gewicht 0.

## 3. Der Sprung

Pollen hat kein Sprung-Netz; dies ist das erste eigene. Die Aufgabe: auf das Kommando aus dem Stand
mit dem ganzen Körper abheben, landen und wieder stehen.

### Das Phasensignal

Der Sprung ist ein Befehl in den Beobachtungs-Befehlsplätzen, wie Pollens Ground-Pick: cos und sin
der Phase t / 2 s in den Plätzen 55 und 56 (`kJumpPhaseSlot`), die Pollens Netze nie nutzen. Sonst
nichts:

- kein Regler, keine Rückkopplung, kein Skript, keine IK, keine Posen-Vorgabe zur Laufzeit,
- keine Kräfte, Impulse oder Physik-Eingriffe.

Die Bewegung erzeugt allein das Netz über die 14 Servoziele, wie bei jeder anderen Fertigkeit.

### Was als echter Sprung zählt (`JumpMeter`)

- **Flug:** alle Teile der Ente mindestens 60 ms ohne Bodenkontakt.
- **Abheben:** Der Rumpf steigt in der Luft mindestens 5 mm über die Höhe beim Abheben. Nur die
  Füße anziehen ist kein Sprung.
- **Aufrecht:** nie mehr als 30° Neigung.
- **Landen:** kein Sturz (über 60° oder ein anderes Teil als die Sohlen am Boden), am Ende auf beiden
  Sohlen stehend.

Die erste, lockerere Messung (Rumpf irgendwann 15 mm über Standhöhe, 60 ms ohne Bodenkontakt) ließ
eine „Sprung“-Variante durch, bei der die Ente sich aufrichtete und dann die Füße anzog, mit 45°
Kippen. Das war kein Sprung; seitdem gilt die strenge Messung.

### Warum PPO allein nicht reicht

Die ersten Läufe (PPO, auch mit Lehrplan für den Sturz-Malus, Phasensignal, Warmstart von velstand,
Referenzhöhe in der Belohnung, verschiedenen Netzgrößen und Lernraten) fanden den Absprung nicht:

- Der Absprung braucht alle Beinservos gleichzeitig am Anschlag. Die Servos sind weich (0,554 N·m/rad,
  Höchstmoment bei 1,7 rad Abweichung): Das Netz muss Ziele weit hinter den Gelenkgrenzen setzen.
  Zufälliges Erkunden um eine stehende Policy erzeugt das praktisch nie.
- Stehen ist sicher und bringt Belohnung; ein missglückter Versuch kostet den Sturz.
- Mit wenigen Dutzend Enten sind die Minibatches klein und der Gradient verrauscht; die KL-Grenze
  hält die Lernrate unten.

### Vorbild → Nachahmen → Üben (`vorbild.h`)

1. **Vorbild suchen** (`--vorbild-suchen`): CMA-ES sucht in derselben Physik einen Sprung, den die
   Ente kann. Gesucht werden die Servoziele des linken Beins (Hüfte, Knie, Knöchel; das rechte
   spiegelt) zu fünf Zeitpunkten (Hocke, Abdruck, Anziehen, Landen, Abfedern). Vor der Hocke und ab
   der Hälfte der Phase hält Pollens velstand die Balance.
   Bewertet wird Steigen in der Luft, Flugzeit, Neigung, Sturz und Stehen, auf dem sauberen und auf
   variierten Robotern, jede Generation auf anderen. Nichts ist von Hand gesetzt: Die Physik
   entscheidet, was geht. Das Ergebnis ist eingebaut (`builtin_jump_demo`).
2. **Nachahmen** (DAgger, 8 Runden à 96 Episoden): In Runde 0 führt das Vorbild, danach das Netz
   selbst; das Vorbild sagt in jedem Takt, was es getan hätte. Das Netz lernt aus allen Beispielen.
   Die Episoden einer Runde lassen sich auf Threads oder Web Worker verteilen (`dagger_episodes`),
   in Episodenfolge zusammengefügt ergibt das dieselbe Runde.
3. **Üben** (PPO): mit der Sprung-Belohnung, einer Nachahmungs-Belohnung (Nähe zur Aktion des
   Vorbilds), kleinem Aktionsrauschen (0,1), vorsichtiger Lernrate und der vollen Variation, bis der
   Sprung robust ist.

Zur Laufzeit gibt es nur das Netz. Das Vorbild existiert nur, während es lernt.

### Belohnung (Übungsphase)

| Term | Gewicht | Was |
|---|---|---|
| airborne | 20 × aufrecht | im Flugfenster in der Luft und steigend (Rumpf über Abhebehöhe oder Steiggeschwindigkeit > 0) |
| lift | 20 × aufrecht | Steiggeschwindigkeit des Rumpfs im Absprungfenster (0–1 m/s) |
| landed | 6 × aufrecht | nach dem Sprung wieder auf beiden Sohlen |
| extra_hop | −4 | ein zweiter Hüpfer |
| imitate | 10 | exp(−mittlerer quadratischer Abstand zur Aktion des Vorbilds / 0,3²) |
| upright / tilt | 2 / −30 | aufrecht (exp(−g_xy²/0,05)); Neigung g_xy² kostet |
| pose_legs / pose_neck | 1 / 1 | vor und nach dem Sprung in Grundstellung |
| drift | −2 | horizontale Geschwindigkeit |
| action_rate | −0,1 → −0,5 | Änderung der Aktion (Lehrplan ab Iteration 200) |
| body_ang_vel, self_collisions | −0,05, −1 | wie Pollens Rezepte |
| fall | −2 → −20 (einmalig) | Lehrplan über 300 Iterationen; Sturz oder Bodenkontakt eines anderen Teils beendet die Episode |

Ohne Vorbild folgt der Rumpf zusätzlich einer Referenzhöhe (nur in der Belohnung).

### Ergebnis

`kern/policies/play.onnx` (12 Runden Nachahmen mit je 32 Episoden zu 60 s, halb sauberer Roboter,
halb volle Variation; ausgewählt ist Runde 8 von 12 nach Prüfung über 32 Episoden, rund 50 min auf
2 Kernen). Gemessen über 32 Episoden je Zeile; Ball und Socke 0,12–1,2 m entfernt, oft beide dicht
beieinander:

| | Ball weggekickt | Socke aufgehoben | aufgehoben, weggetragen, ausgespuckt | Griffe, die hielten | Stürze |
|---|---|---|---|---|---|
| Netz, sauberer Roboter | 9 von 11 (im Mittel nach 17 s) | 4 von 21 | 1 von 21 | 2 von 14 | 2 von 32 |
| Netz, volle Variation | 7 von 11 | 13 von 21 | 0 von 21 | 1 von 15 | 10 von 32 |

- **Ball:** gelernt. Das Netz geht zum näheren Objekt, stellt den linken Fuß hinter den Ball und
  startet Pollens Kick. Passt es nicht, geht es zurück und kommt neu; es bleibt nicht mehr stehen.
- **Socke:** Das Netz geht hin, stellt sich hin, startet das Aufheben und beißt. Mit der gemessenen
  Reibung der TPU-Polster auf Baumwolle hält der Biss aber selten, und beim Hochheben oder Losgehen
  rutscht die Socke oft heraus. Das ist der Engpass, nicht die Entscheidung: Der Griff selbst ist in
  dieser Physik schwer. Das Netz versucht es immer wieder. Nächster Schritt: das Sockenmodell (ein
  Stoff, der sich in den Schnabel legt, statt starrer Klumpen) und Üben mit Belohnung
  (`--spielen-ueben`).
- **Variation:** Mit steiferen Servos und rutschigeren Sohlen fällt Pollens Gang beim Drehen auf der
  Stelle öfter.
- Tests: `micro_duck_plays_with_a_ball_on_its_own` (Ball 30 cm vor der Ente: gekickt, rollt weg) und
  `micro_duck_goes_for_a_sock_and_bites` (geht zur Socke, steht, hebt auf, beißt) – Studio-Physik,
  Bild für Bild, nur das Netz entscheidet.

Befehle:

```
kern_lehrer --spielen [--nachahmen 12] [--episoden 32] [--out play.onnx]
kern_lehrer --spielen-pruefen vorbild|play.onnx [--episodes 32]
kern_lehrer --spielen-spur --seed 3 [--randomized] [--netz play.onnx]
kern_lehrer --spielen-ueben play.onnx [--iterations 300]   # danach üben, nur mit Belohnung
kern_lehrer --demo-spur [--netz play.onnx] [--seed SEKUNDEN]   # das Beispiel-Level wie im Studio
kern_lehrer --biss-fenster      # wo der Schnabel eine Socke greift
kern_lehrer --halt-messen       # wie weit die Ente nach „Stopp“ noch geht
```

## 9. Die Läufe bis hierher

- **PPO allein** (Läufe 1–9): erst Ausnutzen der Belohnung (auf dem Schwanz sitzen, Hechtsprung,
  sich abstützen; jeweils durch strengere Regeln beseitigt), dann stabiles Stehen ohne Sprung; mit
  großem Rauschen und ohne Warmstart lernte die Ente zu stehen, nicht zu springen.
- **Erstes Vorbild:** Die Ente richtete sich auf und zog die Füße an (45° Kippen) – bestand die
  lockere Messung, ist aber kein Sprung. Daraus entstand die strenge Messung.
- **Vorbild mit strenger Bewertung, Nachahmen, Üben** (Lauf 15): das Netz in `kern/policies/`.

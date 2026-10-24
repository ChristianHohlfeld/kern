# Kerns eigene Netze

Netze, die Kern selbst trainiert hat (Kern Lehrer, `docs/lehrer.md`), im Format von Pollens Netzen
(ONNX, Bewegungen mit 61 Beobachtungen → 14 Aktionen wie Pollens). Das Studio lädt sie wie Pollens
Netze aus `third_party/pollen/`.

| Datei | Bewegung | Wie entstanden |
|---|---|---|
| `jump.onnx` | Sprung (Logik-Knoten Robot Move, Bewegung „jump“) | Vorbild (CMA-ES) → Nachahmen (DAgger) → Üben (PPO), siehe `docs/lehrer.md` |
| `play.onnx` | Spielschicht: Ball kicken, Socke aufheben, wegtragen, ausspucken (23 Beobachtungen → 9 Aktionen, 25 Hz, über Pollens Netzen; im Level mit `kernExtras`) | Vorbild → Nachahmen (DAgger, 12 Runden), siehe `docs/lehrer.md`, Abschnitt 8 |

Ein im Browser trainiertes Netz (Seite `/lehrer/`) ersetzt auf demselben Gerät das mitgelieferte.

# Kerns eigene Netze

Netze, die Kern selbst trainiert hat (Kern Lehrer, `docs/lehrer.md`), im Format von Pollens Netzen
(ONNX, 61 Beobachtungen → 14 Aktionen). Das Studio lädt sie wie Pollens Netze aus
`third_party/microduck/`.

| Datei | Bewegung | Wie entstanden |
|---|---|---|
| `jump.onnx` | Sprung (Logik-Knoten Robot Move, Bewegung „jump“) | Vorbild (CMA-ES) → Nachahmen (DAgger) → Üben (PPO), siehe `docs/lehrer.md` |

Ein im Browser trainiertes Netz (Seite `/lehrer/`) ersetzt auf demselben Gerät das mitgelieferte.

# Kern Net

Der Netzwerk-Stack von Kern ist C++ im Engine-Modul (`kern/src/net`, `kern/include/kern/net`) und folgt den Artikeln von Glenn Fiedler (Gaffer On Games) und dem Aufbau von yojimbo / reliable.io / netcode.io. Er läuft über jede Transportschicht, die Datagramme verschicken kann – im Browser über BroadcastChannel (Tabs auf einem Rechner) oder einen WebSocket-Relay (verschiedene Rechner), nativ und in Tests über ein In-Process-Netz mit Netzwerksimulator.

## Schichten

| Schicht | Datei | Aufgabe |
|---|---|---|
| Serialisierung | `serialize.h` | Bitpacker (32-Bit-Wörter, Little Endian), **eine** `serialize()`-Funktion je Typ für Lesen, Schreiben und Messen; Integer im Wertebereich, Bools, Floats, quantisierte Floats und Vektoren, Quaternionen „smallest three“, Strings, relative Sequenznummern. Leser prüfen alles: falsche Werte, abgeschnittene Pakete und Müll liefern `false`, nie einen Absturz. |
| Zuverlässigkeit | `reliable.h` | Jede Sendung trägt Sequenz, Ack und 32 Ack-Bits (komprimierter Header). Liefert Acks, RTT, Paketverlust und Bandbreite; Pakete über 1 KB werden fragmentiert und wieder zusammengesetzt. |
| Kanäle | `connection.h` | **Reliable ordered**: genau einmal, in Reihenfolge, Neuversand nach ~1,25 × RTT, große Nachrichten als Blöcke in 1-KB-Fragmenten. **Unreliable unordered**: einmal, im nächsten Paket. Nachrichten sind typisierte Bytefolgen; eigene Structs serialisieren sich selbst. |
| Verbindung | `client_server.h` | Handshake mit Client-Salt, Challenge-Salt und XOR-Verbindungs-ID (gepolsterte Requests: kein Verstärkungsangriff), Slots, Keep-Alive, Timeouts, Disconnect, „Server voll“. Jedes Datagramm beginnt mit CRC32 über Protokoll-ID und Inhalt: Fremdverkehr und andere Spielversionen fallen vor dem Parsen heraus. |
| Transport | `transport.h` | `LoopbackNetwork` (In-Process), `HostTransport` (Datagramme mit der Browser-Seite), `Simulator` für Latenz, Jitter, Verlust und Duplikate mit festem Seed. |
| Spiel | `kern/game/net_game.h` | Autoritativer Server, Snapshot-Interpolation, Eingaben, Ereignisse (siehe unten). |

## Spiele im Netz

`NetGame` macht aus jedem Kern-Level ein Mehrspieler-Spiel, ohne dass das Level dafür vorbereitet sein muss:

- **Server** simuliert alles (Box3D, Regeln, Pickups, Trigger). Jeder Spieler, der beitritt, bekommt eine Kopie des Spieler-Actors des Levels als Pawn.
- **Clients** schicken pro Netz-Tick (30 Hz) ihre Eingabe, die letzten vier redundant – ein verlorenes Paket kostet nichts, ein Sprung geht nie verloren.
- **Snapshots** enthalten HUD und Posen bewegter Actors (Position auf 1 mm, Rotation mit 11 Bit je Komponente). Ein Prioritätsakkumulator hält jeden Snapshot unter einem Paket: Was sich bewegt, steigt schnell, Ruhendes wird selten gesendet, der eigene Pawn immer.
- **Interpolation**: Clients zeichnen 100 ms in der Vergangenheit und interpolieren zwischen Snapshots (Jitter-Puffer); bei Lücken extrapolieren sie bis zu 250 ms. Die Uhr gleicht sich an den Server an.
- **Ereignisse**, die ankommen müssen (Willkommen, Pawns, gesammelte Pickups), laufen zuverlässig; Sounds unzuverlässig.
- Beide Seiten brauchen dasselbe Level; das Willkommen enthält eine Prüfsumme, ein anderes Level wird abgewiesen.

## Relay

Browser können kein UDP senden. Für Spieler auf verschiedenen Rechnern leitet `tools/net_relay.mjs` (Node, Paket `ws`) Datagramme über WebSockets weiter – ohne eigene Logik: Kern Net darüber verhält sich wie über UDP.

```sh
npm install ws
node tools/net_relay.mjs 8790
```

## Tests

`kern/tests/run.sh` prüft Serialisierung (Rundreise, Grenzwerte, abgeschnittene Daten, 2000 Zufallspuffer), Acks und Fragmentierung, den zuverlässigen Kanal unter 25 % Verlust, 60 ± 30 ms Latenz und 10 % Duplikaten (alle Nachrichten und Blöcke in Reihenfolge), Client/Server (Handshake, volle Server, Disconnect, Timeout, 500 Müll-Datagramme, Wiederverbinden) und Spiele im Netz.

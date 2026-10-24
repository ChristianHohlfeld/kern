# Kapitel 7 – Audio

Kern hat eine eigene Sound-Engine in C++. Sie läuft im selben WebAssembly-Modul wie Renderer und Physik, mischt jede Stimme Sample für Sample selbst und übergibt dem Browser nur noch fertiges Stereo-PCM. Web Audio dient lediglich als Lautsprecherausgang und als Dekoder für importierte Dateien.

Dieses Kapitel beschreibt den Mixer mit 3D-Klang, Bussen, Hall und Limiter, den Synthesizer, aus dem alle eingebauten Sounds entstehen, die Stimme der Micro Duck, die Schicht, mit der Spiele und Levels Klang benutzen, und die Anbindung an den Browser.

| Teil | Dateien | Aufgabe |
|---|---|---|
| Mixer | `include/kern/audio/audio.h`, `src/audio/audio.cpp` | Clips, Stimmen, 3D, Busse, Hall, Limiter |
| Synthesizer | `include/kern/audio/synth.h`, `src/audio/synth.cpp` | Patches, Musik-Sequencer, eingebaute Sound-Bibliothek |
| Duck-Stimme | `include/kern/audio/duck_voice.h`, `src/audio/duck_voice.cpp` | Lautäußerungen der Micro Duck |
| Spiel-Sound | `include/kern/game/sound.h`, `src/game/sound.cpp` | `SoundBank` und `GameAudio` |
| Browser | `web/studio/game.mjs` (`KernAudio`) | AudioWorklet/ScriptProcessor, Dekodieren |

## 7.1 Warum ein eigener Mixer?

Die Kopfzeilen von `audio.h` nennen die Gründe zwischen den Zeilen: Alles passiert „sample by sample and deterministically“. Ein eigener Mixer bringt drei Dinge, die ein Graph aus Web-Audio-Knoten nicht liefert:

- **Determinismus.** Dieselben Ereignisse erzeugen auf jeder Plattform dieselben Samples. Eingebaute Sounds werden mit festen Seeds synthetisiert, Ereignisse entstehen in den festen Physikschritten.
- **Ein Modell für alle Hosts.** Studio, exportierte Spiele und native Tests verwenden denselben C++-Code. Der Browser muss nur Samples abspielen.
- **Volle Kontrolle über Lautstärke-Rampen, Stimmenverwaltung und Ausgabepegel.** Kein Klick beim Stoppen, kein Übersteuern: Die Ausgabe überschreitet nie ±1.

## 7.2 Der Mixer

### Initialisierung

```cpp
audio::Mixer mixer;
mixer.init(48000);          // Abtastrate des Ausgabegeräts, 8000..192000; max_voices Standard 64
```

`init(sample_rate, max_voices)` löscht alles und legt einen festen Pool von Stimmen an (1 bis `kMaxVoices` = 128). Kern Studio startet den Mixer mit der Rate des `AudioContext` und 64 Stimmen. Die Ausgabe ist immer Stereo (`kOutputChannels = 2`), interleaved.

### Clips

Ein Clip ist PCM in der Rate des Mixers, mono oder stereo.

- `create_clip(samples, frames, channels, rate, name)` kopiert Samples beliebiger Rate (3000 bis 384 000 Hz) und bringt sie **einmal** auf die Mixer-Rate. Clips über 60 Sekunden werden abgelehnt, nicht-endliche Samples durch 0 ersetzt.
- `adopt_clip(samples, channels, name, loopable)` übernimmt Samples, die bereits in Mixer-Rate vorliegen – so kommen die synthetisierten Sounds hinein.
- `destroy_clip` stoppt alle Stimmen des Clips. `clip_info` liefert Name, Frames, Kanäle, Dauer, Spitzenwert und ob der Clip ein nahtloser Loop ist.

Das Resampling beim Import ist ein **Windowed-Sinc-Filter**: Sinc-Kern mit Blackman-Fenster und 16 Nulldurchgängen pro Seite. Beim Herunterrechnen wird die Grenzfrequenz auf die niedrigere Nyquist-Frequenz gesenkt (`cutoff = min(1, ratio)`), damit keine Spiegelfrequenzen entstehen. Die Gewichte werden pro Ausgabesample normiert. Das ist rechenintensiv, geschieht aber nur einmal beim Laden; beim Abspielen genügt danach eine billige Interpolation.

### Stimmen und `PlayParams`

`play(clip, params)` startet eine Stimme und gibt eine `VoiceId` zurück: `generation << 32 | slot`. Die Generation steigt mit jedem Start. Eine alte Id, deren Slot inzwischen neu belegt ist, passt deshalb nicht mehr, und Aufrufe mit ihr laufen ins Leere statt eine fremde Stimme zu verändern.

| Feld | Standard | Bereich | Bedeutung |
|---|---|---|---|
| `volume` | 1 | 0–16 | lineare Verstärkung |
| `pitch` | 1 | 0,05–8 | Abspielgeschwindigkeit |
| `pan` | 0 | −1–1 | Stereoposition (nur 2D) |
| `loop` | aus | | Clip wiederholen |
| `bus` | `sfx` | | Ziel-Bus (`master` wird zu `sfx`) |
| `fade_in` | 0 | s | Einblenden |
| `start` | 0 | s | Startposition im Clip |
| `priority` | 0 | | höhere Priorität überlebt Stimmenknappheit |
| `spatial` | aus | | 3D-Stimme |
| `position`, `velocity` | 0 | m, m/s | Ort und Geschwindigkeit der Quelle |
| `min_distance` | 1 | ≥ 0,01 | innerhalb volle Lautstärke |
| `max_distance` | 50 | > min | Stille (linear) bzw. Endpunkt (inverse, exponential) |
| `rolloff` | `inverse` | | `inverse`, `linear`, `exponential` |
| `spread` | 0 | 0–1 | 0 Punktquelle, 1 umhüllend ohne Panning |
| `doppler` | 1 | 0–4 | 0 schaltet Doppler für diese Stimme ab |

Ungültige Werte werden geklemmt, nicht abgelehnt.

**Voice Stealing.** Ist kein Slot frei, sucht `play()` die unwichtigste Stimme mit höchstens gleicher Priorität: zuerst die niedrigste Priorität, dann die leiseste (Hörbarkeit des letzten Blocks), dann die älteste. Gibt es nur Stimmen höherer Priorität, scheitert der Start (`"all voices busy with higher priority"`). `stats().stolen` zählt die Übernahmen.

**Steuerung laufender Stimmen:** `stop(voice, fade)` blendet aus – mindestens 5 ms (`kStopGuard`), denn ein hartes Abschneiden mitten in einer Schwingung erzeugt einen hörbaren Klick. `set_volume(voice, v, ramp)` fährt die Lautstärke linear über `ramp` Sekunden (Standard 30 ms). Weiter gibt es `set_pitch`, `set_pan`, `set_position`, `set_paused`, `set_all_paused` (Spielpause, Positionen bleiben erhalten), `stop_all(bus, fade)`, `playing`, `position_of`.

### Der Mischvorgang

`render(out, frames)` arbeitet in Blöcken zu 128 Frames (`kBlock`). Pro Block und Stimme berechnet `spatialize()` die Zielverstärkungen links und rechts, einen Doppler-Faktor und eine Tiefpass-Grenzfrequenz. Innerhalb des Blocks werden die Verstärkungen **linear vom alten zum neuen Wert interpoliert**. Bewegt sich eine Quelle, springt der Pegel also nicht von Block zu Block, sondern gleitet – ohne „Zipper“-Geräusche.

Pro Frame dann:

1. Hüllkurve (Einblenden oder Ausblenden beim Stoppen) und Lautstärke-Rampe fortschreiben.
2. Den Clip an der gebrochenen Position lesen, mit **4-Punkt-Hermite-Interpolation** (Catmull-Rom):
   ```cpp
   inline f32 hermite(f32 x0, f32 x1, f32 x2, f32 x3, f32 t) {
     const f32 c1 = 0.5f * (x2 - x0);
     const f32 c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3;
     const f32 c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
     return ((c3 * t + c2) * t + c1) * t + x1;
   }
   ```
   Bei Loops wird der Index modulo der Clip-Länge gefaltet; bei One-Shots liefert alles außerhalb Stille.
3. Einen einpoligen Tiefpass anwenden (Luftdämpfung, siehe 7.3): `y += k · (x − y)` mit `k = 1 − exp(−2π f_c / rate)`.
4. Mit Hüllkurve × Lautstärke × Kanalverstärkung in den Puffer des Busses addieren.
5. Die Position um `pitch × doppler` weiterschieben (pausiert: 0).

Mono-Clips werden auf beide Kanäle verteilt.

**Virtuelle Stimmen.** Liegt die Lautheit einer 3D-Stimme unter 10⁻⁵ (`kSilence`) – etwa weil sie jenseits von `max_distance` ist –, wird sie nicht gemischt. Ihre Position läuft trotzdem weiter. Kommt der Hörer zurück, ist eine Maschine an der Stelle, an der sie gewesen wäre, statt wieder von vorn zu beginnen. `stats().virtual_voices` zählt solche Stimmen.

## 7.3 3D-Klang

`Mixer::set_listener({position, forward, up, velocity})` setzt den Hörer. Die Spiel-Laufzeit stellt ihn an die Kamera (7.7). Für jede 3D-Stimme rechnet `spatialize()`:

### Entfernung

Mit `d = clamp(Abstand, min_distance, max_distance)`:

| `rolloff` | Verstärkung g |
|---|---|
| `inverse` | `min_distance / d` |
| `linear` | `1 − (d − min) / (max − min)` |
| `exponential` | `(d / min_distance)^−1,5` |

Inverse und exponentielle Kurven erreichen nie ganz null. Deshalb blendet der Mixer sie über die letzten 20 % vor `max_distance` zusätzlich linear auf null aus. So wird jede Quelle hinter `max_distance` tatsächlich still und kann virtuell werden.

### Panning

Die Richtung zur Quelle wird auf die Rechtsachse des Hörers projiziert (`cross(forward, up)`), das ergibt einen Pan-Wert in [−1, 1]. Nahe Quellen und breite Quellen panen weniger:

```
proximity = clamp(dist / max(0.25, min_distance / 2), 0, 1)
pan      *= (1 − spread) · proximity
```

Eine Quelle direkt am Kopf springt so nicht hart von links nach rechts. Dann folgt **Equal-Power-Panning**, normiert auf Einheitsverstärkung in der Mitte:

```
a     = (pan + 1) · π/4
left  = cos(a) · √2 · g
right = sin(a) · √2 · g
```

Equal Power heißt: `left² + right²` ist für alle Pan-Werte gleich. Eine Quelle, die sich von links nach rechts bewegt, wird in der Mitte nicht leiser, wie es bei linearem Überblenden passieren würde. Dieselbe Formel (ohne g) gilt für 2D-Stimmen mit `pan`.

### Luftdämpfung und Richtung

Hohe Frequenzen werden über Distanz stärker gedämpft als tiefe. Kern nähert das mit einem Tiefpass, dessen Grenzfrequenz mit dem Abstand sinkt; hinter dem Hörer wird der Klang zusätzlich dumpfer:

```
f_c = 20000 / (1 + dist / 120)
falls front < 0:  f_c *= 1 + 0,6 · front        // gerade hinter dem Hörer: 40 %
```

`front` ist das Skalarprodukt aus Richtung zur Quelle und Blickrichtung. Ohne HRTF ist dieser Unterschied das wichtigste Merkmal, an dem man „vorn“ von „hinten“ unterscheidet.

### Doppler

```
f' = f · (c − v_l) / (c − v_s)
```

`c` ist `speed_of_sound` (Standard 343 m/s), `v_l` und `v_s` sind die Geschwindigkeiten von Hörer und Quelle entlang der Linie Quelle → Hörer, jeweils mit `doppler_factor × PlayParams::doppler` skaliert und auf ±0,9 c begrenzt. Der Faktor wird auf 0,5–2 geklemmt und pro Block mit 25 % zum neuen Wert geglättet, damit schnelle Vorbeifahrten nicht stufig klingen.

## 7.4 Busse, Hall und Master

### Busse

Jede Stimme spielt auf einem Bus: `music`, `sfx`, `ambience`, `ui` oder `voice`. Alle Busse münden in `master`.

```cpp
struct BusSettings {
  f32 volume = 1;
  bool muted = false;
  f32 lowpass_hz = 0;     // 0 = aus
  f32 reverb_send = 0;    // 0..1 zum Master-Hall
};
```

`init()` setzt Ausgangswerte: Musik 0,7, Ambience 0,8, Hall-Send von `sfx` 0,12 und von `voice` 0,08. Spiele überschreiben das über die Level-Einstellungen (7.7).

Pro Block und Bus:

- Lautstärke und Stummschaltung werden **über den Block gerampt** (von der Verstärkung des letzten Blocks zur neuen). Stummschalten klickt deshalb nicht.
- Optional ein Tiefpass: Biquad nach RBJ (Audio-EQ-Cookbook), Güte 0,7071 (Butterworth), Grenzfrequenz zwischen 10 Hz und 0,45 × Rate.
- Das Signal geht in den Master und, mit `reverb_send` gewichtet, in den Hall-Eingang.
- Spitzen- und RMS-Pegel werden pro Bus gemessen.

### Hall (Freeverb)

Der Master-Hall ist Freeverb von Jezar at Dreampoint: pro Kanal **acht parallele, gedämpfte Kammfilter**, gefolgt von **vier Allpässen in Reihe**.

- Ein Kammfilter ist eine Verzögerungsleitung mit Rückkopplung. Er erzeugt eine Folge immer leiserer Echos im Abstand seiner Länge. Die Dämpfung im Rückkopplungszweig (`store = out · (1 − damp) + store · damp`, ein Tiefpass) lässt hohe Frequenzen schneller abklingen – wie in einem echten Raum.
- Acht Kämme mit leicht verschiedenen Längen (1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 Samples bei 44,1 kHz) überlagern sich zu einer dichten Echo-Wolke.
- Die Allpässe (556, 441, 341, 225) verwischen die verbleibende Struktur, ohne den Frequenzgang zu färben.
- Der rechte Kanal hat um 23 Samples längere Leitungen. Das entkoppelt die Kanäle und macht den Hall breit.

Die Längen werden auf die tatsächliche Rate skaliert. `set_reverb(room_size, damping, wet)` setzt `feedback = 0,7 + 0,28 · room_size` und `damp = 0,4 · damping`; der Eingang bekommt Freeverbs feste Verstärkung 0,015 auf die Summe beider Kanäle, das Hall-Signal wird mit `wet × 3` zum Master addiert.

### Limiter und Soft-Clip

Am Ende steht ein Spitzenbegrenzer:

```
peak   = max(|L|, |R|)
needed = peak · gain > 0,92 ? 0,92 / peak : 1
gain   = needed < gain ? needed : 1 − (1 − gain) · release
```

Der Angriff ist sofort: Die Verstärkung fällt, bevor eine Spitze die Schwelle 0,92 überschreitet. Das Loslassen folgt einer Exponentialkurve mit 120 ms Zeitkonstante (`release = exp(−1 / (0,12 · rate))`). Danach folgt ein weiches Clipping mit `tanh` oberhalb der Schwelle. Mit der Verstärkung davor wird es normalerweise nicht erreicht; es fängt aber jeden Rest ab, auch NaN. So bleibt jedes Ausgabesample in [−1, 1].

`stats()` liefert Stimmen (aktiv, hörbar, virtuell, gestohlen), Clips, gerenderte Frames, die stärkste Absenkung des Limiters in dB, Spitzen- und RMS-Pegel von Master und Bussen sowie einen Spitzenwert mit Haltefunktion, der mit 20 dB pro Sekunde abfällt wie die Nadel eines Pegelmessers. Die Statusleiste des Studios zeigt daraus die Pegelanzeige.

## 7.5 Der Synthesizer

Kern bringt keine Sound-Dateien mit. Alle eingebauten Sounds entstehen beim Laden aus Code: ein kleiner subtraktiver Synthesizer in der Tradition von sfxr, erweitert um resonante Filter, Rauschfarben, FM und Schichtung, dazu ein Step-Sequencer für Musik-Loops (`synth.h`). Alles wird offline in Clips gerendert, mit festen Seeds und damit auf jeder Plattform gleich.

### `SynthPatch`

| Gruppe | Felder |
|---|---|
| Wellenform | `wave`: `square`, `saw`, `sine`, `triangle`, `noise`, `pink`, `pulse`; `volume` |
| Hüllkurve (s) | `attack`, `sustain`, `punch`, `release` |
| Tonhöhe | `frequency`, `slide` (Oktaven/s), `slide_accel` (Oktaven/s²), `min_frequency`, `vibrato_depth` (Halbtöne), `vibrato_rate`, `arp_ratio`, `arp_time` |
| Pulsbreite | `duty`, `duty_sweep` |
| FM | `fm_ratio`, `fm_depth` (nur Sinus) |
| Filter | `lowpass` (Hz), `lowpass_sweep` (Oktaven/s), `resonance` (0–0,95), `highpass` (Hz) |
| Modulation | `tremolo_depth`, `tremolo_rate`, `repeat` (s), `bitcrush` (0–1) |
| Rauschen | `seed` |

### Wie `synthesize()` rechnet

- **Modulation mit Kontrollrate.** Slide, Vibrato, Tremolo und Filter-Sweep werden nur alle 16 Samples neu berechnet. Laut Kommentar würden `exp2`/`sin` pro Sample bei langen Noten die Kosten dominieren; 16 Samples liegen weit unter hörbaren Stufen.
- **Oszillatoren** über eine Phase in [0, 1): Rechteck und Puls mit ±0,6, Sägezahn, Dreieck, Sinus (mit optionaler Phasenmodulation durch einen zweiten Sinus im Verhältnis `fm_ratio`).
- **Rauschen** wird wie bei sfxr mit der Oszillatorfrequenz neu gewürfelt: tiefe Noten klingen dunkler. Rosa Rauschen entsteht aus weißem mit Paul Kellets sparsamem Filter (drei einpolige Tiefpässe).
- **Tiefpass:** Chamberlin-State-Variable-Filter mit Resonanz (`q = 1 − resonance`), Grenzfrequenz-Koeffizient `2 · sin(π · f / rate)`. **Hochpass:** einpolig.
- **Bitcrush** als Sample-and-Hold.
- **Hüllkurve:** linearer Anstieg, Haltephase mit optionalem „Punch“ (Überhöhung, die linear auf 1 abfällt), linearer Abfall.
- `repeat` startet die Tonhöhenhüllkurve periodisch neu (Power-up-Effekt), `arp_time` springt einmal um `arp_ratio` (Pickup-Effekt).

Hilfsfunktionen: `mix_into` (Schichten mit Zeitversatz), `make_seamless` (die letzten Millisekunden werden mit Equal-Power-Überblendung über den Anfang gelegt, danach ist der Puffer ein nahtloser Loop), `normalize`.

### Modalsynthese für Anschläge

Für Aufprallgeräusche kleiner fester Körper hat die Bibliothek eine zweite Methode, `struck()`: einige exponentiell abklingende Sinus-Teiltöne (Moden des Körpers) plus ein kurzer, tiefpassgefilterter Rauschimpuls für den eigentlichen Kontakt, mit 2 ms weichem Einsatz. Die Kommentare im Code begründen die Wahl der Moden: Ein massives Kunststoffteil klingt weder tief noch lang – ein tiefer, lange klingender Teilton wirkt wie eine hohle Kiste. Deshalb liegen die Moden von `plastic` im mittleren Bereich (760, 1650, 3100 Hz) und klingen innerhalb weniger Millisekunden ab.

### Musik

Die beiden Musik-Loops sind Partituren: Jeder Takt stellt seine Noten in eine Warteschlange; gleiche Noten werden nur einmal gerendert und wiederverwendet.

- `music_drive`: 120 BPM, Am–F–C–G, Four-on-the-Floor-Bassdrum, Snare auf 2 und 4, Achtel-Bass, Sechzehntel-Arpeggio, Flächen. 8 Takte zu 2 s.
- `music_calm`: 80 BPM, Fmaj7–Cmaj7–Am7–G6, Flächen, Bass und eine pentatonische Glockenmelodie. 8 Takte zu 3 s.

Beide sind Stereo und nahtlos.

### `BuiltinJob`: Rendern in Zeitscheiben

Musik zu rendern dauert einige hundert Millisekunden. `BuiltinJob` verteilt das: `start(name, rate)`, dann `step(budget_ms)` – bei Musik etwa ein Takt bzw. so viele Noten, wie ins Budget passen –, bis `done()`, dann `take(channels)`. Kurze Effekte werden in einem Schritt fertig. Laut `docs/studio.md` ruckelt ein Level mit Musik deshalb beim Laden nicht.

### Die eingebaute Bibliothek

Sounds werden mit `"builtin:<name>"` referenziert. `builtin_sounds()` listet in Anzeigereihenfolge:

| Kategorie | Namen |
|---|---|
| Gameplay | `pickup`, `powerup`, `jump`, `land`, `goal`, `hazard`, `checkpoint`, `lose`, `laser` |
| Combat | `gunshot`, `smg`, `pistol`, `sniper`, `launcher`, `reload`, `hit`, `down` |
| Physics | `explosion`, `impact`, `impact_light`, `break`, `step`, `plastic`, `plastic_step` |
| Vehicle | `engine`*, `hover`*, `boost` |
| Ambience | `wind`*, `city`*, `hum`* |
| Robot | `servo`*, `quack`, `beep` |
| UI | `click`, `confirm`, `error` |
| Music | `music_drive`*, `music_calm`* |

Mit * markierte Einträge sind Loops. Dazu kommen die Laute der Micro Duck (7.6), die nicht in dieser Liste stehen, aber über ein Namensschema erreichbar sind.

Zwei Beispiele aus `render_builtin`, um zu zeigen, wie die Sounds gebaut sind:

```cpp
} else if (name == "pickup") {
  SynthPatch a = patch(Wave::pulse, 988, 0.002f, 0.05f, 0.28f, 0.42f);
  a.duty = 0.35f;
  a.arp_ratio = 1.5f;          // nach 65 ms eine Quinte höher
  a.arp_time = 0.065f;
  out = synthesize(a, rate);
  add(out, bell(rate, 100, 0.35f), 0.35f);
} else if (name == "impact" || name == "impact_light") {
  // A body landing on something: a thud, not a crash - the noise is kept below 2 kHz.
  ...
```

## 7.6 Die Stimme der Micro Duck

Die Micro Duck spricht mit einem eigenen kleinen Synthesizer (`duck_voice.h`), einer Portierung des `sounds`-Crates aus pollen-robotics/microduck (Rust, Apache-2.0). Es gibt keine Samples.

![Die Micro Duck im Level: ihre Laute kommen aus ihrem Kopf, auf dem Voice-Bus](bilder/duck-level.png)

### Persönlichkeit aus einem Seed

Ein einziger `u32`-Seed bestimmt deterministisch eine `DuckPersonality`: Tonlage und Register, Glissando-Neigung, Obertonneigung, Nasalität, Formant, Vibrato, Jitter, Atem, „Quakigkeit“ (Amplitudenmodulation mit 18–55 Hz), Triller und Tempo (0,82–1,22). Zwei Seeds klingen wie zwei verschiedene Tiere; derselbe Seed klingt immer gleich.

Der Seed entsteht wie auf dem Roboter aus einer Kennung: die ersten vier Bytes von SHA-256 der Kennung, Big-Endian (`duck_seed_from_id`; Kern enthält dafür eine kleine eigene SHA-256-Implementierung). Der Roboter nimmt seine Seriennummer, Kern den Namen und die Actor-Id (`name + "#" + id`, in `src/game/walker.cpp`).

### Wie ein Laut entsteht

- **Klangkörper:** sieben Harmonische mit Gewichten aus der Persönlichkeit:
  ```
  base      = 1 / n^tilt
  high_lift = brightness · (n/7)^1,5
  nasal     = nasal · 0,6       (nur 2. und 3. Harmonische)
  skew      = gerade/ungerade Betonung nach harmonic_skew
  formant   = formant_gain · base · 1,5   (nur bei n = formant_n)
  w_n = max(base + high_lift + nasal + skew + formant, 0),  w_1 ≥ 0,7
  ```
- Die Tonhöhenkurve jedes Lauts wird mit Vibrato und Jitter multipliziert und zur Phase integriert.
- **Quaken:** Amplitudenmodulation `1 − am_d · (0,5 + 0,5 · sin(2π · am_rate · t))`.
- **Atem:** rosa Rauschen, beigemischt mit `breath`.

### Laute und Varianten

Sieben Tags: `alarm`, `greet`, `inquire`, `peck`, `chirp`, `coo`, `wheee`. `greet` und `chirp` haben je 12 Varianten, `wheee` 6, alle anderen 10 (`duck_variant_count`). Gespielt wird eine zufällige Variante, damit die Ente nicht wie eine hängende Aufnahme klingt. `wheee` ist in Start, Schleife und Ende geteilt (`render_duck_wheee`), damit ein Spieler die Schleife so lange halten kann, wie eine Fahrt dauert.

### Bitgenau wie der Roboter

Der Port ist auf Sample-Gleichheit bei 48 kHz ausgelegt: derselbe xoshiro256++/splitmix64-Strom einschließlich der Box-Muller-Reserve, dieselbe Reihenfolge der Zufallszüge, dieselben Konstanten, `f32` und `f64` genau dort, wo das Rust-Original sie verwendet. Fused Multiply-Add ist per Pragma abgeschaltet, damit `a*b + c` zweimal rundet wie in Rust. Laut `docs/walker.md` ergibt ein Vergleich aller 328 Puffer gegen das Rust-Crate dieselbe Länge und eine maximale Abweichung von 0.

Wie auf dem Roboter liegt jeder Laut danach als 16-Bit-Puffer vor (× 32767, begrenzt, abgeschnitten). Erst dann wird er kubisch auf die Mixer-Rate gebracht. Zuletzt folgt ein Hochpass zweiter Ordnung bei 300 Hz: Der kleine Lautsprecher des Roboters gibt darunter fast nichts wieder (`SPEAKER_ROLLOFF_HZ`), und Kern bildet das nach.

### Einbindung

Ein Laut ist eine gewöhnliche Sound-Referenz nach dem Schema

```
builtin:duck_<tag>_<seed>_<variant>
builtin:duck_wheee-start_<seed>_<variant>    (ebenso -loop, -end)
```

`find_builtin` und `render_builtin` erkennen diese Namen. Die Laufzeit sendet sie als `SoundCue` mit `voice = true` auf dem Bus `voice`, mit dem Kopf der Ente als Position (dort sitzt der Lautsprecher). Ein neuer Laut schneidet den laufenden ab – wie ein einzelner Lautsprecher. Während `wheee` werden andere Laute verworfen.

Weil die Synthese auf einem Telefon zehn Millisekunden pro Laut kosten kann (lang genug, um die Audioausgabe auszuhungern), wärmt `GameWorld::warm_walker_voices()` die Laute jeder Ente vorab: eine Synthese pro Frame, häufigste zuerst (`duck_voice_warm`, prozessweit gecacht). Wann die Ente welchen Laut ruft, steht in [Kapitel 13](13-micro-duck.md) und in `docs/walker.md`.

## 7.7 Klang im Spiel: `SoundBank` und `GameAudio`

Die Spiel-Laufzeit benutzt den Mixer über zwei Klassen aus `include/kern/game/sound.h`. Der Kopfkommentar zeigt die Verwendung:

```cpp
audio::Mixer mixer;  mixer.init(48000);
SoundBank bank(mixer);
GameAudio audio(mixer, bank);
world.start(level, SimMode::play);  audio.begin(world);
// jeden Frame:
bank.prepare(4); audio.update(world, camera, dt);   // der Host zieht mixer.render()
world.stop();  audio.end();
```

### Sound-Referenzen

Überall, wo ein Level einen Sound nennt, steht eine Zeichenkette:

- `"builtin:<name>"` – ein synthetisierter Sound,
- die Id eines importierten `SoundAsset` – eine Datei, die im Level als Data-URL liegt (WAV, MP3, OGG, M4A/AAC, FLAC – was der Browser dekodiert),
- `""`, `"none"` – still; `"auto"` – der Standard der jeweiligen Aktion.

`sound_reference_ok(level, ref)` prüft eine Referenz auf Gültigkeit.

### `SoundBank`

Die Bank macht aus Referenzen Mixer-Clips.

- `clip(ref)` gibt den Clip zurück, oder 0, solange er nicht bereit ist – und fordert ihn dabei an.
- **Eingebaute Sounds** kommen in eine Warteschlange. `prepare(budget_ms)` rendert sie mit `BuiltinJob` in Zeitscheiben und übergibt das Ergebnis mit `adopt_clip`.
- **Importierte Sounds** werden „angefragt“. Der Host holt sich die Liste mit `pending_imports()`, die Data-URL mit `source(id)`, dekodiert sie und gibt das PCM mit `set_pcm(id, samples, frames, channels, rate)` zurück; die Bank legt daraus mit `create_clip` (Windowed-Sinc-Resampling) einen Clip an. Scheitert das Dekodieren, meldet der Host `set_failed(id, reason)`.
- `request_level(level)` fordert vorab alles an, was ein Level brauchen kann: Musik, Ambience, Aufprall- und Bruchgeräusche, die Motor-, Sprung- und Boost-Sounds der Controller, Schritte und Landungen je nach Bewegungsart, Waffen- und Treffer-Sounds, Pickup- und Trigger-Sounds (mit den Standards `goal`, `hazard`, `checkpoint`, `confirm`), Audio Sources, `goal`/`lose`, wenn das Level gewonnen oder verloren werden kann, sowie `impact_light`, wenn irgendwo `impact` benutzt wird. Play beginnt dann mit allem an seinem Platz.
- `sync_assets(level)` verwirft Clips von Assets, die entfernt oder ersetzt wurden (erkannt an der Identität der Quelle).

### `GameAudio`

`GameAudio` spielt eine laufende `GameWorld` ab.

**`begin(world)`** übernimmt Musik und Ambience aus den World Settings, bestimmt den Motor-Loop des Spielers und sammelt alle Audio Sources mit `autoplay`.

**`apply_settings(world)`** bildet die Level-Einstellungen auf den Mixer ab:

| Level | Mixer |
|---|---|
| `master_volume` | Master-Bus |
| `music_volume`, `ambience_volume` | Busse `music`, `ambience` |
| `reverb` r (0–1) | Hall-Send: `sfx` 0,7 r, `voice` 0,5 r, `ambience` 0,3 r, `music`/`ui` 0; `set_reverb(0,55 + 0,4 r, 0,45, 0,12 + 0,5 r)` |
| `doppler` | `Mixer::doppler_factor` |

**`update(world, camera, dt)`** jeden Frame:

1. **Pause** folgt der Welt (`set_all_paused`).
2. **Hörer** an der Kamera; seine Geschwindigkeit aus der Bewegung des Auges seit dem letzten Frame. Über 200 m/s gilt es als Kameraschnitt, und die Geschwindigkeit wird 0 – sonst gäbe es bei jedem Schnitt einen absurden Doppler-Sprung.
3. **Musik und Ambience** starten als 2D-Loops (Priorität 100, 0,6 s Einblenden), sobald ihre Clips bereit sind.
4. **Motor des Spielers:** ein 3D-Loop am Spieler, ohne Doppler (die Kamera folgt dem Spieler; Eigen-Doppler wäre falsch), Priorität 90. Lautstärke und Tonhöhe folgen Tempo und Gas, mit exponentieller Glättung:
   ```
   speed   = clamp(hud.speed / Höchstgeschwindigkeit, 0, 1,2)
   volume → 0,32 + 0,45 · speed + 0,18 · throttle     (max. 0,95; nicht spielend: 0,12)
   pitch   = 0,72 + 0,95 · speed + 0,08 · throttle    (0,5–2,2)
   ```
   Bei der laufenden Ente (`walker`) ist der „Motor“ das Servo-Geräusch: leise, lauter mit der Arbeit der Motoren (Drehmoment × Geschwindigkeit), höher mit ihrer Drehzahl.
5. **Audio Sources** folgen ihren Actors; ihre Geschwindigkeit wird aus der Positionsänderung geschätzt (für Doppler). Loops starten mit Priorität 40 und 0,3 s Einblenden, One-Shots mit Priorität 20, beide nach ihrer Startverzögerung. Mit `pitch_by_speed` steigt die Tonhöhe mit der Geschwindigkeit des Actors: `pitch · (1 + pitch_by_speed · speed · 0,1)`. Stirbt der Actor, wird die Stimme in 0,15 s ausgeblendet.
6. **Cues der Simulation** (7.8) werden abgespielt. Ist der Clip eines Cues noch nicht fertig, wartet er bis zu 0,25 s und wird danach verworfen – ein Geräusch, das zu spät kommt, ist schlimmer als keins. Mehr als 64 wartende Cues werden abgeschnitten.

Cue-Stimmen haben `min_distance` 3 m, `max_distance` 80 m und Priorität 50 (Bus `ui`) oder 10 (alles andere). Wichtige Dauerklänge (Musik, Motor, Loops) verdrängen also bei Stimmenknappheit nie einen Effekt – umgekehrt schon. `cues_played()` und `cues_dropped()` zählen mit.

**`end(fade)`** blendet alles aus dem Spiel aus. `set_muted` schaltet den Master stumm und bleibt über `begin`/`end` erhalten (Stummschalter des Editors).

### Welche Sounds Controller benutzen

`engine_sound_for(controller)` wählt bei `engine_sound = "auto"` den Loop nach Bewegungsart:

| Controller | Loop |
|---|---|
| Hover Car, Flyer | `builtin:hover` |
| Vehicle | `builtin:engine` |
| Walker (Micro Duck) | `builtin:servo` |
| Rolling, Character, Soldier | keiner |

Weitere Felder am Controller: `jump_sound` (`builtin:jump`), `boost_sound` (`builtin:boost`). Waffen haben `fire_sound` (`builtin:smg`) und `reload_sound` (`builtin:reload`), Lebenspunkte `hit_sound` (`builtin:hit`) und `death_sound` (`builtin:down`), Pickups `sound` (`builtin:pickup`), Trigger `sound` (`"auto"`: passend zur Aktion).

## 7.8 Cues und Aufprallgeräusche

### `SoundCue`

Die Simulation spielt nichts selbst ab. Sie erzeugt während der festen Physikschritte **Cues** und sammelt sie; `GameAudio` leert die Liste jeden Frame:

```cpp
struct SoundCue {
  std::string sound;              // "builtin:<name>" oder importierte Id
  Vec3 position;
  f32 volume = 1, pitch = 1;
  bool spatial = true;
  audio::Bus bus = audio::Bus::sfx;
  ActorId actor = 0;
  bool voice = false, silence = false;   // Stimme des Actors: schneidet ab / verstummt nur
};
```

Weil Cues in den festen Schritten entstehen, sind sie deterministisch. Ohne Abnehmer (headless, etwa im Lehrer) behält `GameWorld::cue` nur die neuesten 64. Im Netzwerk werden Sounds unzuverlässig übertragen, Ereignisse wie gesammelte Pickups dagegen zuverlässig ([Kapitel 10](10-netzwerk.md)).

### Aufprallgeräusche

Jeder Rigid Body hat `impact_sound` (Standard `builtin:impact`) und `impact_threshold` (2,5 m/s Annäherungsgeschwindigkeit). Jeder Collider hat zusätzlich `impact_sound`, Standard `"auto"` = der Sound seines Körpers. So kann ein Teil eines Körpers anders klingen als ein anderes: Die Sohle der Ente macht `plastic_step`, ihr Gehäuse `plastic`.

Der Ablauf in `GameWorld` (`src/game/runtime.cpp`):

1. Nur Shapes, die einen Sound haben (oder vom Spieler oder der Logik gebraucht werden), melden Box3D-Hit-Events. Die globale Schwelle von Box3D wird auf die kleinste Schwelle aller klingenden Körper gesetzt, mindestens 0,05 m/s (`kMinImpactThreshold`).
2. Pro Physikschritt höchstens 6 Aufprallgeräusche – mehr würde der Mixer ohnehin stehlen.
3. Unter der Schwelle des Körpers bleibt es still.
4. **Entprellen:** Eine Sohle rollt von der Ferse zur Spitze und setzt sich in kleinen Hüpfern, die Box3D als einzelne Treffer meldet. Ein Schritt (Sound mit „step“ im Namen) klingt deshalb höchstens alle 0,5 s pro Actor, ein Klopfen alle 0,09 s.
5. **Stärke:** `clamp((v − threshold) / (5 · threshold) + 0,2, 0,2, 1)` wird zur Lautstärke.
6. **Leichter Aufprall:** Bei `builtin:impact` unter dem 2,2-Fachen der Schwelle wird `builtin:impact_light` gespielt.
7. **Tonhöhe nach Masse:** `clamp(1,05 − log10(max(0,1, Masse)) · 0,1, 0,7, 1,15)`. Schwere Dinge klingen tiefer; leichte nur wenig höher, weil stark hochgepitchte Klopfer gläsern klingen.

Die Micro Duck setzt ihre Schwelle auf 0,12 m/s, damit schon leichte Berührungen hörbar sind. Brechende Gelenke spielen `break_sound` (`builtin:break`) an der Position ihres Actors.

## 7.9 Level-Einstellungen und die Audio Source

### World Settings

```cpp
f32 master_volume = 1;
std::string music;            // 2D-Loop auf dem Musik-Bus ("" = keiner)
f32 music_volume = 0.6f;
std::string ambience;         // 2D-Loop auf dem Ambience-Bus
f32 ambience_volume = 0.6f;
f32 reverb = 0.25f;           // Master-Hall 0..1
f32 doppler = 1;              // Doppler-Skala für alle 3D-Sounds
```

### `AudioComponent`

Die Komponente „Audio Source“ macht einen Actor zur Schallquelle – Maschinenbrummen, Radio, Wind, ein einmaliger Sound:

| Feld | Standard | Bedeutung |
|---|---|---|
| `sound` | `builtin:hum` | Referenz |
| `volume` | 1 | linear, 0–4 |
| `pitch` | 1 | Abspielrate |
| `loop` | an | |
| `autoplay` | an | startet mit Play/Simulate |
| `spatial` | an | 3D am Actor; aus: 2D auf dem Bus |
| `min_distance`, `max_distance` | 2 m, 40 m | volle Lautstärke innen, still jenseits |
| `rolloff` | `inverse` | |
| `bus` | `sfx` | |
| `doppler` | 1 | 0 = kein Doppler |
| `pitch_by_speed` | 0 | zusätzliche Tonhöhe pro m/s des Körpers |
| `delay` | 0 | Sekunden nach dem Start |

Der Studio-Katalog enthält fertige Varianten: Audio Source (Maschinenbrummen), Ambient Wind (überall hörbar), City Ambience (mit Abfall) und Radio (spielt `music_calm` in 3D). Bedienung im Editor: siehe [Kapitel 11](11-studio.md).

## 7.10 Die Browser-Anbindung

Der Mixer rendert PCM; der Browser spielt es ab. Diese Aufgabe übernimmt `KernAudio` in `web/studio/game.mjs`, das Studio und exportierte Spiele gemeinsam nutzen. (Der Kopfkommentar von `audio.h` nennt noch `kern/web/audio.mjs`; im Repository liegt der Code in `game.mjs`.)

### Starten

```js
export function startAudio(engine) {
  if (!KernAudio.supported()) return null;
  const audio = new KernAudio(frames => engine.audioRender(frames));
  const rate = audio.create();
  if (!rate || !engine.audioInit(rate)) return null;
  setInterval(() => { audio.pump(); engine.audioPrepare(5); }, 20);
  return audio;
}
```

- `create()` legt den `AudioContext` an (`latencyHint: "interactive"`) und setzt, wo vorhanden, `navigator.audioSession.type = "playback"`. Auf iOS stellt das sicher, dass der Stummschalter Spiele nicht stumm macht.
- Browser starten Audio erst nach einer Nutzergeste. Die erste `pointerdown`-, `keydown`-, `touchend`- oder `mousedown`-Aktion setzt den Kontext fort und verbindet die Ausgabe. Wird die Seite nach einer Unterbrechung wieder sichtbar, setzt `KernAudio` einen „interrupted“-Kontext fort.
- Der Mixer bekommt die Rate des Geräts (`kern_studio_audio_init(rate)`). So muss im Browser nichts mehr umgerechnet werden.
- Ein Timer alle 20 ms füllt die Ausgabe nach und rendert eingebaute Sounds mit 5 ms Budget (`kern_studio_audio_prepare`). Ton fließt so weiter, wenn Frames langsam sind oder der Tab im Hintergrund liegt, und Musik wird auch zwischen Frames fertig.

### Der AudioWorklet-Weg

Bevorzugt läuft die Ausgabe über ein AudioWorklet. Der Prozessor-Quelltext steht als String in `game.mjs` und wird als Blob-URL geladen, ersatzweise als `data:`-URL (falls eine Content Security Policy Blobs sperrt).

- Der Hauptthread rendert Stücke von 128 bis 2048 Frames (`kern_studio_audio_render`, höchstens 16 384 Frames pro Aufruf), kopiert sie und schickt sie per `postMessage` mit übertragenem Puffer an das Worklet.
- Das Worklet spielt die Warteschlange ab und meldet alle 4 Blöcke zurück, wie viele Frames es verbraucht hat und wie oft es leer lief.
- `pump()` hält die Warteschlange `latency` Sekunden voraus, gerundet auf 128 Frames. Der Startwert ist 70 ms. **Adaptiv:** Läuft die Warteschlange leer (der Hauptthread hing länger, als die Warteschlange reichte), kommen 30 ms dazu, bis höchstens 200 ms. Nach 15 Sekunden ohne Aussetzer sinkt der Vorlauf wieder um 5 ms pro Schritt zum Startwert.
- **Aussetzer ohne Klick:** Ist die Warteschlange leer, springt das Worklet nicht auf Stille. Das letzte Sample gleitet über etwa 2 ms (96 Samples) auf null, und wenn wieder Daten kommen, werden sie eingeblendet. Eine Lücke bleibt hörbar, aber ohne Knacken.
- Nach einem sehr langen Stillstand (mehr als eine Sekunde Rückstand) beginnt die Zählung neu.

Wo kein AudioWorklet verfügbar ist, nimmt `KernAudio` einen `ScriptProcessor` mit 1024 Frames, der in seinem Callback direkt rendert.

### Dekodieren importierter Sounds

Importierte Sounds liegen als Data-URL im Level. `pollSounds(audio)` fragt die Engine nach ausstehenden Imports (`kern_studio_pending_sounds`), holt die Quelle (`kern_studio_sound_source`), dekodiert sie mit `decodeAudioData` und übergibt das PCM mit `kern_studio_sound_pcm` (interleaved `f32`, 1 oder 2 Kanäle, beliebige Rate). Mehr als zwei Kanäle werden vorher auf Stereo heruntergemischt (gerade Kanäle links, ungerade rechts). Schlägt etwas fehl, meldet `kern_studio_sound_failed` den Grund; Cues mit diesem Sound werden dann verworfen statt ewig zu warten.

## 7.11 Hinweise für eigene Spiele

- **Referenzen statt Dateien.** Die eingebauten Sounds kosten keinen Download und sind sofort deterministisch. Eigene Dateien lohnen sich für Musik und charakteristische Geräusche.
- **Busse nutzen.** Musik auf `music`, Dauergeräusche auf `ambience`, Effekte auf `sfx`, Oberfläche auf `ui`, Stimmen auf `voice`. Dann wirken Lautstärke-Regler und Hall wie erwartet; `music` und `ui` bekommen keinen Hall.
- **`max_distance` ehrlich setzen.** Jenseits davon werden Stimmen virtuell und kosten keine Mischzeit.
- **Prioritäten.** Wichtige Dauerklänge bekommen eine hohe Priorität, kurze Effekte eine niedrige. Bei 64 Stimmen wird sonst bei viel Action das Falsche gestohlen.
- **Schwellen für Aufprall.** Eine zu niedrige `impact_threshold` lässt rollende oder ruhende Körper ständig klopfen; die Entprellung hilft nur begrenzt.
- **Kein Ton ohne Geste.** Im Browser ist bis zur ersten Interaktion nichts zu hören. Das ist eine Regel des Browsers, kein Fehler.

Der Mixer selbst ist von der Spiel-Laufzeit unabhängig. Wer in einer eigenen `kern::App` ([Kapitel 12](12-eigene-spiele.md)) Klang möchte, kann `audio::Mixer` und `audio::render_builtin` direkt verwenden. Für die Browser-Ausgabe muss die App dann allerdings selbst eine Render-Funktion exportieren und eine Ausgabe wie `KernAudio` anbinden; `web/app.mjs` enthält keine Audio-Anbindung.

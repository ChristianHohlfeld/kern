# Kern – Das Buch

**Die Engine, Kern Studio, ein Laufroboter und der Lehrer: verstehen, benutzen, weiterentwickeln**

## Impressum und Rechte

**Autor, Herausgeber und Rechteinhaber (Selbstverlag):** Christian Heinrich Hohlfeld\
Gerhart-Hauptmann-Weg 5, 78464 Konstanz, Deutschland\
ORCID: [0009-0003-6634-9045](https://orcid.org/0009-0003-6634-9045)

© 2026 Christian Heinrich Hohlfeld. Alle Rechte vorbehalten.

**Das Buch.** Dieses Buch und seine Abbildungen sind urheberrechtlich geschützt. Jede Verwertung
außerhalb der engen Grenzen des Urheberrechtsgesetzes ist ohne schriftliche Zustimmung des
Rechteinhabers unzulässig. Das gilt insbesondere für Vervielfältigung, Übersetzung, Bearbeitung,
Verbreitung, öffentliche Zugänglichmachung sowie die Einspeicherung und Verarbeitung in
elektronischen Systemen. Die Nutzung für das Training, Feinabstimmen oder Bewerten von Modellen des
maschinellen Lernens (Text- und Data-Mining) behält sich der Rechteinhaber nach § 44b Abs. 3 UrhG
ausdrücklich vor. Kurze Code-Beispiele aus dem Buch dürfen unter der Lizenz der Software genutzt
werden.

**Die Software.** Kern (Engine, Kern Studio, der Lehrer, die trainierten Netze, Werkzeuge und
Beispiele) ist Open Source unter der Apache License 2.0 und kostenlos erhältlich:
<https://christianhohlfeld.github.io/kern/>, Quellcode unter
<https://github.com/ChristianHohlfeld/kern>. Ausgenommen sind die in Kern Studio enthaltenen
3D-Modelle der Micro Duck von Pollen Robotics: Sie stehen unter CC BY-NC-SA 4.0 und dürfen nur nicht
kommerziell genutzt werden. Ein kommerzieller Build (`kern/build.sh --commercial`) lässt sie weg. Die
Namen „Kern", „Kern Studio", „Kern Lehrer" und das Kern-Logo sind von der Lizenz ausgenommen. Kurse
und Integrationslizenzen für Schulen und Unternehmen bietet der Rechteinhaber gesondert an;
Integrationslizenzen umfassen Pollens 3D-Modelle nicht.

**Bestandteile Dritter.** Kern verwendet und dieses Buch beschreibt Box3D (Erin Catto, MIT), Earcut
(Mapbox, ISC), musl-Mathematik (MIT), Verfahren aus Intel XeGTAO (MIT) und three.js (MIT) sowie
Robotermodell, Policies und portierte Software der Micro Duck von Pollen Robotics (Apache 2.0). Sie
bleiben unter ihren eigenen Lizenzen; die Lizenztexte stehen in [Anhang D](D-lizenzen.md). Kern
Studio zeigt die Ente mit Pollens 3D-Modellen (CC BY-NC-SA 4.0, nicht kommerziell); diese sind in
diesem Buch nicht enthalten und nicht abgebildet. Die Abbildungen der Ente in diesem Buch, auch die
Bildschirmfotos aus Kern Studio, entstanden mit einer eigenen Gestaltung des Autors und zeigen
Pollens 3D-Modelle nicht.

**Marken.** „Micro Duck" (auch „Microduck") bezeichnet den Roboter von Pollen Robotics (Hugging
Face), den Kern simuliert; der Name wird nur zur Beschreibung verwendet. Dieses Buch und Kern sind
keine Produkte von Pollen Robotics oder Hugging Face und werden von ihnen weder unterstützt noch
geprüft. Weitere Marken- und Produktnamen (unter anderem Box3D, Unreal Engine, WebAssembly, WebGL,
ONNX, GitHub, Amazon, Dynamixel (ROBOTIS), MuJoCo, Intel, Xbox) gehören ihren Inhabern und werden nur zur Beschreibung genannt.

**Entstehung.** Software und Text hat zum größten Teil ein KI-System (Claude von Anthropic) unter der
Leitung des Autors geschrieben. Idee, Architektur, Auswahl der Komponenten, Prüfung und
Verantwortung liegen beim Autor; Näheres in [Über den Autor](E-ueber-den-autor.md).

**Haftung.** Alle Angaben, Programme und Anleitungen wurden sorgfältig erstellt und geprüft, ohne
Gewähr für Richtigkeit und Vollständigkeit. Wer nach diesem Buch Roboter baut, kalibriert oder
betreibt, tut das auf eigenes Risiko; Autor und Rechteinhaber haften nur für Vorsatz und grobe
Fahrlässigkeit, bei Verletzung von Leben, Körper oder Gesundheit sowie nach zwingenden
gesetzlichen Vorschriften, insbesondere dem Produkthaftungsgesetz.

## Inhalt

| | |
|---|---|
| **Teil I – Die Engine** | [1 Einführung](01-einfuehrung.md) · [2 Bauen und Testen](02-bauen-und-testen.md) · [3 Grundlagen](03-grundlagen.md) · [4 Grafik](04-grafik.md) · [5 Szene und Geometrie](05-szene-und-geometrie.md) · [6 Physik](06-physik.md) · [7 Audio](07-audio.md) · [8 Spiel-Laufzeit](08-spiel-laufzeit.md) · [9 Logik](09-logik.md) · [10 Netzwerk](10-netzwerk.md) |
| **Teil II – Benutzen** | [11 Kern Studio](11-studio.md) · [12 Eigene Spiele](12-eigene-spiele.md) |
| **Teil III – Roboter und Lernen** | [13 Die Micro Duck](13-micro-duck.md) · [14 Der Lehrer](14-lehrer.md) · [15 Eigene Modelle trainieren](15-eigene-modelle.md) · [16 Kalibrierung](16-kalibrierung.md) · [17 Referenz](17-referenz.md) |
| **Anhänge – Grundlagen** | [A Neuronale Netze und Reinforcement Learning](A-grundlagen-neuronale-netze.md) · [B Echtzeit-Rendering](B-grundlagen-rendering.md) · [C Physiksimulation](C-grundlagen-physik.md) · [D Lizenzen](D-lizenzen.md) · [Über den Autor](E-ueber-den-autor.md) |

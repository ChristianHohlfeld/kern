# Über den Autor

**Christian Heinrich Hohlfeld** ist Softwareentwickler in Konstanz. Er programmiert seit 2010 – in
C++, Java, C#/.NET, TypeScript und Python – und hat an der HTWG Konstanz Software Engineering
studiert (B.Sc.). Sein Ansatz: erst verstehen, wie eine Arbeit wirklich abläuft, dann das ganze
System bauen, bis es live ist. Routine darf Software übernehmen; die Entscheidungen bleiben bei
Menschen.

Spiele und 3D begleiten ihn von Anfang an. Seine Android-App MMA News, allein gebaut und ab 2010
veröffentlicht, erreichte rund 220.000 Downloads. 2015 erschien Alien Star Racing, ein Spiel mit
prozedural erzeugten Leveln auf einem eigenen Toolkit über cocos2d-x (C++); es kam in einem von
Microsoft gesponserten Wettbewerb unter die besten zehn. Mit VoxelShaper entwickelt er ein Voxel-CAD
für den 3D-Druck im Browser, mit kern3d einen WebGL-Kern ohne Abhängigkeiten – den Vorläufer von
Kern.

Beruflich hat er Software für Abrechnung und Finanzierung (ines GmbH), eine Handwerksplattform
(wirsindhandwerk.de) und als Freiberufler unter anderem für die Robert Bosch GmbH entwickelt. Heute
ist er bei der WOBAK, der städtischen Wohnungsbaugesellschaft in Konstanz, Referent für Digitale
Transformation und Prozessoptimierung und interner Datenschutzbeauftragter; er leitet dort die
Projektgruppe Digitalisierung und die interne KI-Pilotgruppe. Daneben baut und betreibt er eigene
Produkte, darunter digitalisierungsplanung.de, ein Werkzeug, das echte Arbeitsabläufe im Browser
aufzeichnet und KI-Agenten über einen MCP-Server zugänglich macht.

<https://christianhohlfeld.com> · <https://www.linkedin.com/in/christian-hohlfeld/> ·
<https://github.com/ChristianHohlfeld>

## Wie Kern und dieses Buch entstanden sind

Kern und dieses Buch sind mit künstlicher Intelligenz entstanden, und das soll niemand zwischen den
Zeilen herausfinden müssen. Den größten Teil des Quellcodes und der Texte hat ein KI-System
geschrieben (Claude von Anthropic), unter der Leitung des Autors.

**Beim Autor liegt:**

- **Die Idee und das Ziel:** eine Engine in reinem C++, die im Browser auf Telefon und Desktop gleich
  schnell und gleich aussehend läuft, mit einem Editor, einer Robotersimulation und einem eigenen
  Trainingssystem für neuronale Netze.
- **Die Architektur und die Auswahl der Bausteine:** C++20 nach WebAssembly und WebGL2, Box3D als
  Physik, die Micro Duck mit den Original-Netzen von Pollen Robotics statt nachgebauter Bewegungen,
  Treue zum echten Roboter vor eigenen Tricks, das Lizenzmodell.
- **Der Maßstab und die Entscheidungen:** was gut genug ist, was verworfen wird und was live geht.
  Jede Funktion hat er auf echten Geräten geprüft; viele Verbesserungen sind aus diesem Prüfen
  entstanden.
- **Die Verantwortung** für Software und Buch.

**Bei der KI liegt:** die Umsetzung in Code und Tests, die Recherche in Quellen und
Fachveröffentlichungen, das Schreiben und Übersetzen der Kapitel – in einem Tempo, das ein Einzelner
ohne solche Werkzeuge nicht erreicht.

Das Ergebnis zeigt, was heute möglich ist: Ein erfahrener Entwickler, der weiß, was er bauen will,
und die richtigen Entscheidungen trifft, baut mit KI in kurzer Zeit, wofür früher ein Team nötig
war. Die KI schreibt; was entsteht und ob es taugt, entscheidet der Mensch.

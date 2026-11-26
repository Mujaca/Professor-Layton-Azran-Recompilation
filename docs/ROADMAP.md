# ROADMAP

> Ziel und Meilensteine ändert **nur der Mensch**. Agenten tragen nur den
> gemessenen Stand in die Tabelle „Frontier-Verlauf“ ein.

## Ziel

*Professor Layton and the Azran Legacy* (EUR) läuft als **rein statisch
recompilierte** native PC-Anwendung: eigener Recompiler, eigene Runtime
(Kernel, Services, GPU, Audio, Input). Azahar dient nur als Referenz zum Lesen
und Vergleichen.

## Messgröße: Frontier

`python tools/frontier.py` baut nichts. Es startet `out/port.exe` im
**natürlichen Boot** (unveränderter Einstieg `0x00100000`, keine
Verhaltens-Schalter), vergleicht die Dienst- und Ereignisfolge mit der
Azahar-Referenz (`reports/azahar_trace.txt`) und meldet:

* `services`: wie viele Dienste der Referenzfolge in richtiger Reihenfolge
  erreicht werden (aktuell 10 in der Referenz),
* `first_divergence`: das erste fehlende bzw. abweichende Ereignis,
* `milestones`: welche der folgenden Meilensteine im Log belegt sind,
* `workarounds`: Anzahl der aktiven Code-Patches (soll sinken).

## Meilensteine (in dieser Reihenfolge)

| # | Meilenstein | Beleg im Log |
|---|---|---|
| M1 | Alle 10 Referenzdienste werden natürlich geöffnet | `services == 10` |
| M2 | Spiel ruft selbst `gsp::Gpu SetBufferSwap` mit eigenem Framebuffer | `[gsp] SetBufferSwap` ohne Host-Eingriff |
| M3 | Spiel schickt selbst eine nicht leere PICA-Liste (`SetCommandList`/`0x000C`) | `[gpu] list ... draws>0` |
| M4 | Erster vom Spiel gerenderter Frame ist nicht einfarbig | `tools/frontier.py --shot` |
| M5 | PICA-Vertex-Shader + TEV: Titelbildschirm erkennbar | Sichtvergleich mit `reports/azahar_topscreen.png` |
| M6 | Input (hid), Menü bedienbar, Spielstart | manueller Test |
| M7 | Audio (DSP/BCSTM), Video (MOFLEX), Saves | manueller Test |
| M8 | `tools/workarounds/code_patches.json` ist leer | `workarounds == 0` |
| M9 | Hardware-Renderer: PICA-Kommandolisten werden in eine native Grafik-API (D3D11/Vulkan/OpenGL) übersetzt (Details: `docs/IDEAS.md`). Frühestens nach M4/M5, darf parallel zu M6–M8 laufen | Bildvergleich Software- gegen Hardware-Pfad auf denselben Gast-Frames |
| M10 | Ton klingt korrekt: Titelmusik ohne Aussetzer und Verzerrung | Hörtest + Vergleich mit einer Azahar-Aufnahme derselben Szene |
| M11 | Neues Spiel startet: Namenseingabe → erste Spielszene | Screenshots aus dem natürlichen Boot (nur echte Host-Eingabe) |
| M12 | Titel und Hauptmenü laufen mit ≥ 30 fps (Gast-Frames) | fps-Anzeige im Fenstertitel bzw. `[port] gast=`-Log |
| M13 | Bild flackert nicht: jeder angezeigte Frame ist vollständig (kein Wechsel zwischen halbfertigen/alten Puffern) | Sichttest im Fenster + Frame-Folge ohne Ausreißer |
| M14 | Erstes Rätsel spielbar: Eingabe, Lösung, Speichern | manueller Test + Speicherstand lädt erneut |
| M15 | Filme farbtreu: Logo- und Intro-Filme (MOFLEX/Mobiclip) stimmen mit einer unabhängigen Dekodierung überein | mittlere Abweichung < 3 je Kanal gegen FFmpeg-Referenzframes, keine Blockstreifen |
| M16 | Soundeffekte und Spielstand: Effekte bei Eingaben hörbar, Speichern und erneutes Laden funktionieren (konkretisiert M7) | Effekt-Quellen im `HLE_AUDIO_LOG` aktiv + Hörtest; Speicherstand überlebt Neustart |
| M17 | Hardware-Renderer als Standard: Bilder bleiben auf der GPU, Ausgabe über D3D-Swapchain, wählbare höhere interne Auflösung; Software-Pfad bleibt Referenz | Bildvergleich SW/HW besteht (siehe M18), Gast-fps HW ≥ SW, Sichttest |
| M18 | Reproduzierbare Testläufe: Eingaben an Gastzeit statt Host-Frames gekoppelt, zwei Läufe liefern dieselben Gast-Frames | zwei Läufe → identische Bildfolge (Grundlage für M9-/M15-Vergleiche und A/B-Messungen) |

Ab M4 sind Bildvergleiche zulässig, vorher nicht.

## Stand und nächste Arbeitspakete (Stand 2026-11-26, master 4a442a1)

| # | Stand | Beleg |
|---|---|---|
| M1–M5, M8 | erreicht | Frontier 10/10, 0 Patches, Titel |
| M6 | erreicht | Menü, Touch, Namenseingabe, Rätsel-Eingabe (Nutzer) |
| M7 | erreicht | Ton, Filme, Speichern/Laden, Soundeffekte |
| M9 | erreicht | Nutzer-Entscheid 2026-11-26 nach Bildvergleich; `--renderer hw` läuft; Bildvergleich (`tools/rendercmp.py`, 2026-11-26, m9-compare): froenborg 89 Bilder oben MAD 0,60, max 0,03 % Pixel > 8, 0 Cluster; unten MAD 0,002; Rätsel-Lauf 150 Bilder oben MAD 0,48, Cluster > 32 nur Titelkarte (max 286 px) und Luftschiff (max 353 px) - Texelgrenzen-Gleichstände, SW-Rundung (docs/GAME_NOTES.md) |
| M10 | erreicht | Aussetzer, Hänger und Echtzeit gelöst; Nutzer-Entscheid 2026-11-26: Hörtest („sauber“) genügt als Abnahme, Azahar-Aufnahme entfällt |
| M11 | erreicht | Namenseingabe → Eröffnungsszene (`--press`-Folge in `docs/GAME_NOTES.md`) |
| M12 | erreicht | 2026-11-26, master 7b1cf2f, `--renderer hw`, Echtzeit, 2 Läufe: Titel (g2100–2700) und Slot-Menü (ab g2900) je 59,8–60,0 Gast-fps (`[port] gast=` je 300 Host-Frames); 3D-Dialog Froenborg 30 Gast-fps |
| M13 | erreicht | Sichttest Nutzer (Doppelpufferung) |
| M14 | erreicht | Nutzer: erstes Rätsel gespielt, gespeichert, „Continue“ lädt in Froenborg |
| M15 | erreicht | Logo gegen FFmpeg mittlere Abweichung 0,91; übrige Filme: Nutzer-Sichttest 2026-11-26 ohne Farbfehler, als Abnahme bestätigt |
| M16 | erreicht | Speichern/Laden („Continue“) und Soundeffekte vom Nutzer bestätigt |
| M17 | offen | – |
| M18 | erreicht | `port.exe --deterministic` (virtuelle Zeit, VBlank im Gleichschritt); `tools/scene.py froenborg raetsel1 --repro`: je 89/89 Bilder gleich (alle 50 Gast-Frames bis g4450), hw und soft gleiche VBlank→Gast-Frame-Folge |

1. ~~Bildrate Titel/Hauptmenü~~ (M12 erreicht: 60 Gast-fps).
2. ~~Bildvergleich SW gegen HW~~ (M9 erreicht: drei HW-Fehler behoben,
   Restunterschiede an Texelgrenzen-Gleichständen).
3. **HW-Renderer als Standard** (M17): Bilder auf der GPU, D3D-Ausgabe,
   höhere interne Auflösung.
4. Offen, niedrige Priorität: seltener Hänger in Rätsel 001 (Nutzer meldet
   beim Spielen), Speicherpfad mit echter Titel-ID.
5. **Modding** (vom Nutzer aufgenommen 2026-11-26, noch kein Meilenstein):
   A. RomFS-Overlay `mods/<name>/romfs/` (Texte, Grafiken, Ton, Filme);
   B. Lua-Skripte (Frame-/Eingabe-Callbacks, Gastspeicher lesen/schreiben,
   Overlay-Zeichnen); C. Funktions-Hooks ueber Recompiler-Einstiegspruefung.
   Mods standardmaessig aus; Gate, Frontier und Vergleiche laufen ohne Mods;
   Schnittstelle generisch, keine Spieladressen in der Runtime.
   QoL-Ideen (als Beispiel-Mods, nicht in der Runtime):
   - Lupenmodus: Lupe folgt dem Mauszeiger, Klick untersucht; eigener
     Cursor nur in diesem Modus (braucht Spielzustand -> Lua, Stufe B).
6. **Builder-Paket fuer Endnutzer** (vom Nutzer aufgenommen 2026-11-26):
   ein Download (ZIP mit kleinem Starter), der Python (Embeddable), die
   Build-Abhaengigkeiten (pyctr, capstone, ziglang) und die Quellen buendelt;
   Nutzer waehlt seinen entschluesselten Dump, der Builder prueft ihn und
   baut port.exe ohne weitere Installation oder Downloads. Enthaelt keine
   Spielinhalte; Lizenztexte der gebuendelten Teile liegen bei. Bau des
   Pakets reproduzierbar (Skript, spaeter GitHub-Release per CI).
7. **Ruhiger Standardstart** (nach M17): Debug-Overlay/fps im Titel nur mit
   `--debug` bzw. `[debug] overlay = 1` (Teil des M17-Branches); port.exe
   ohne Konsole (`--console` bzw. `[debug] console = 1` zeigt sie);
   Konsolenausgabe immer zusaetzlich in `out/logs/port-<Zeit>.log`, die
   letzten 10 bleiben. Tools (check, frontier, scene.py) muessen weiter die
   Ausgabe lesen koennen.
8. **Performance** (vom Nutzer aufgenommen 2026-11-26 nach dem M17-Sichttest):
   3D-Dialog Froenborg faellt bei interner Aufloesung x2/x3 auf ~21-24
   Gast-fps (x1: 28-30, Spiel begrenzt auf 30); sichtbar mit `--fps`.
   Bekannt: GPU-Worker nur ~66 % ausgelastet (Ursache offen: CPU-Seite,
   zweiter Zeichendurchgang, Synchronisation?); exaktes Shader-Blending
   kostet ~1 Gast-fps (Snapshot-Kopien serialisieren die GPU, vier
   allgemeine Alternativen gemessen und verworfen, docs/GAME_NOTES.md).
   Vorgehen: erst profilieren (`[hwtime]`, Host-Profiler), dann je Ursache
   ein Experiment; Bildvergleich (M9-Werte) darf sich nicht verschlechtern.
   Zielwerte legt der Mensch fest.

## Frontier-Verlauf

| Datum | Commit | services | erste Divergenz | Patches |
|---|---|---|---|---|
| 2026-11-24 | cleanup | 7/10 | y2r:u fehlt | 23 |
| 2026-11-24 | ipc-layouts | 7/10 | y2r:u fehlt | 23 |
| 2026-11-24 | recomp-bxret | 4/10 | ptm:u | 23 |
| 2026-11-24 | gsp: InterruptRelayQueue + GX 0x00-0x05 | 9/10 | boss:U | 23 |
| 2026-11-24 | kernel-sync | 9/10 | boss:U | 23 |
| 2026-11-24 | cfg-eur | 9/10 | boss:U | 22 |
| 2026-11-24 | workarounds | 9/10 | boss:U | 0 |
| 2026-11-24 | arbiter-apt (9 war Schein) | 3/10 | hid:USER | 0 |
| 2026-11-24 | romfs-lookup | 3/10 | hid:USER | 0 |
| 2026-11-24 | fs-session-init | 9/10 | boss:U | 0 |
| 2026-11-24 | review-faelschungen | 9/10 | boss:U | 0 |
| 2026-11-24 | pica-cmdlist | 3/10 | hid:USER | 0 |
| 2026-11-24 | pica-shader | 3/10 | hid:USER | 0 |
| 2026-11-24 | pica-raster-tev | 3/10 | hid:USER | 0 |
| 2026-11-24 | pica-texturen-blend | 3/10 | hid:USER | 0 |
| 2026-11-24 | pica-luecken | 3/10 | hid:USER | 0 |
| 2026-11-24 | pica-review-fixes | 9/10 | boss:U | 0 |
| 2026-11-24 | pica-lighting | 9/10 | boss:U | 0 |
| 2026-11-24 | pica-dphi-format | 9/10 | boss:U | 0 |
| 2026-11-24 | srv-namenslaenge | 9/10 | boss:U | 0 |
| 2026-11-24 | naechste-divergenz-1 | 9/10 | boss:U | 0 |
| 2026-11-24 | recomp-difftest | 9/10 | boss:U | 0 |
| 2026-11-24 | naechste-divergenz-2 | 10/10 | None | 0 |
| 2026-11-24 | recomp-kontrollfluss | 9/10 | boss:U | 0 |
| 2026-11-24 | ipc-folge-1 | 10/10 | None | 0 |
| 2026-11-24 | hid-shared-memory | 9/10 | boss:U | 0 |
| 2026-11-24 | bildausgabe | 9/10 | boss:U | 0 |
| 2026-11-24 | y2r-umwandlung | 9/10 | boss:U | 0 |
| 2026-11-24 | io-review-fixes | 10/10 | None | 0 |
| 2026-11-24 | audio-ausgabe | 10/10 | None | 0 |
| 2026-11-24 | speicherstand-archive | 10/10 | None | 0 |
| 2026-11-24 | io-review-fixes-2 | 10/10 | None | 0 |
| 2026-11-24 | m3-draws | 10/10 | None | 0 |
| 2026-11-24 | pica-lighting-fixes | 10/10 | None | 0 |
| 2026-11-24 | pica-geometry-shader | 10/10 | None | 0 |
| 2026-11-24 | recomp-vfp | 9/10 | boss:U | 0 |
| 2026-11-24 | exklusiv-monitor | 9/10 | boss:U | 0 |
| 2026-11-24 | pica-proctex-gas | 10/10 | None | 0 |
| 2026-11-24 | recomp-abdeckung | 10/10 | None | 0 |
| 2026-11-24 | recomp-fuzz-breit | 10/10 | None | 0 |
| 2026-11-24 | pica-performance | 10/10 | None | 0 |
| 2026-11-24 | frame-pacing | 10/10 | None | 0 |
| 2026-11-24 | m3-erster-draw | 10/10 | None | 0 |
| 2026-11-24 | ipc-folge-2 | 10/10 | None | 0 |
| 2026-11-24 | kernel-scheduler | 10/10 | None | 0 |
| 2026-11-24 | gpu-mmio-m3 | 10/10 | None | 0 |
| 2026-11-24 | pica-lighting-perf | 10/10 | None | 0 |
| 2026-11-24 | listen-flut | 10/10 | None | 0 |
| 2026-11-24 | pica-azahar-abgleich | 10/10 | None | 0 |
| 2026-11-24 | m3-gpu-2 | 10/10 | None | 0 |
| 2026-11-24 | m3-hang-dateilesen | 10/10 | None | 0 |
| 2026-11-24 | m3-erster-draw-2 | 10/10 | None | 0 |
| 2026-11-24 | listen-flut | 10/10 | None | 0 |
| 2026-11-24 | m3-erster-draw-3 | 10/10 | None | 0 |
| 2026-11-24 | ipc-folge-3 | 10/10 | None | 0 |
| 2026-11-24 | m3-hang-timing | 10/10 | None | 0 |
| 2026-11-24 | ringpuffer-wartestelle | 10/10 | None | 0 |
| 2026-11-24 | m3-demux-paketlaenge | 10/10 | None | 0 |
| 2026-11-24 | ringpuffer-wartestelle-2 | 10/10 | None | 0 |
| 2026-11-24 | m4-filmtextur | 10/10 | None | 0 |
| 2026-11-24 | m4-gl-init-cosim | 10/10 | None | 0 |
| 2026-11-24 | m3-intraframe-bedingung | 10/10 | None | 0 |
| 2026-11-24 | m3-intraframe-bedingung-2 | 10/10 | None | 0 |
| 2026-11-24 | m4-nach-film | 10/10 | None | 0 |
| 2026-11-24 | m4-zweite-wiedergabe | 10/10 | None | 0 |
| 2026-11-24 | m4-audio-stream | 10/10 | None | 0 |
| 2026-11-24 | m5-titel-eingabe | 10/10 | None | 0 |
| 2026-11-24 | ipc-folge-4 | 10/10 | None | 0 |
| 2026-11-24 | m4-draw-aufrufer | 10/10 | None | 0 |
| 2026-11-24 | m4-layer-registry | 10/10 | None | 0 |
| 2026-11-24 | m5-ablauf-hauptmenue | 10/10 | None | 0 |
| 2026-11-24 | recomp-performance | 10/10 | None | 0 |
| 2026-11-24 | listen-flut | 10/10 | None | 0 |
| 2026-11-24 | decoder-difftest | 10/10 | None | 0 |
| 2026-11-24 | cosim-unicorn | 10/10 | None | 0 |
| 2026-11-24 | recomp-fuzz-thumb | 10/10 | None | 0 |
| 2026-11-24 | recomp-abdeckung-2 | 10/10 | None | 0 |
| 2026-11-24 | cosim-sweep | 10/10 | None | 0 |
| 2026-11-24 | m4-szenenwechsel | 10/10 | None | 0 |
| 2026-11-24 | m5-bildqualitaet | 10/10 | None | 0 |
| 2026-11-25 | m9-hw-tev-texturen | 10/10 | None | 0 |
| 2026-11-25 | m10-dsp-abgleich | 10/10 | None | 0 |
| 2026-11-25 | m7-effekte-filmton | 10/10 | None | 0 |
| 2026-11-25 | m9-hw-blend-titel | 10/10 | None | 0 |
| 2026-11-25 | m9-hw-review-fixes | 10/10 | None | 0 |
| 2026-11-25 | m5-logo-farben | 10/10 | None | 0 |
| 2026-11-25 | m9-hw-restfehler | 10/10 | None | 0 |
| 2026-11-25 | m9-hw-texturcache | 10/10 | None | 0 |
| 2026-11-25 | scheduler-staffelstab | 10/10 | None | 0 |
| 2026-11-26 | m18-determinism | 10/10 | None | 0 |
| 2026-11-26 | m9-compare | 10/10 | None | 0 |

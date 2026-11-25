# Spätere Ausbaustufen

Hier stehen festgehaltene Vorhaben, die **noch nicht** dran sind. Ziele und
Meilensteine selbst stehen in `docs/ROADMAP.md` und werden nur vom Menschen
geändert. Ein Eintrag hier ist kein Arbeitsauftrag, sondern eine Erinnerung
mit Zeitpunkt.

## Hardware-Renderer: PICA-Befehle in eine native Grafik-API übersetzen

**Offizieller ROADMAP-Meilenstein M9 (vom Nutzer am 2026-11-24 festgelegt).**

Der CPU-Code ist statisch rekompiliert. Der PICA-Software-Rasterizer
(`runtime/gpu`) und das HLE-DSP (`runtime/audio`) sind dagegen funktional eine
Nachbildung der Hardware. Die übliche nächste Stufe bei Recomp-Projekten ist,
die GPU-Arbeit auf die Host-GPU zu bringen:

* PICA-Kommandolisten in D3D11, Vulkan oder OpenGL übersetzen.
* Vertex- und Geometry-Shader-Programme der PICA in HLSL, GLSL oder SPIR-V
  übersetzen (Vorbild: Azahar `src/video_core/shader/generator`).
* TEV-Stufen, Fog, Lighting und Alpha-/Stencil-/Depth-Tests in Fragment-Shader
  umsetzen.
* Texturen, Farb- und Tiefenpuffer als GPU-Ressourcen verwalten.
  Synchronisation mit Gastspeicher-Zugriffen über DisplayTransfer,
  TextureCopy, MemoryFill und CPU-Lesezugriffe: Cache mit Invalidierung,
  wie im Azahar-Rasterizer-Cache.

**Zeitpunkt:** erst nach M4/M5, also wenn das Menü im Software-Pfad korrekt
gezeichnet wird. Bis dahin ist der Software-Renderer die Referenz für
korrekte Bilder.

**Abnahme später:** Bildvergleich Software-Pfad gegen Hardware-Pfad auf
denselben Gast-Frames. Der Software-Pfad bleibt als Referenz und
Rückfallebene erhalten.

**Stand 2026-11-25 (m9-hw-texturen-tev):** Der D3D11-Pfad
(`runtime/gpu/hw_d3d11.c`, `--renderer hw`) deckt jetzt zusätzlich ab:

* Texturen der Einheiten 0–2 (`Texture2D` und `Disabled`) in allen 14
  PICA-Formaten. Die Dekodierung nutzt exakt den Software-Dekoder
  (`gpu_pica_decode_texel`, inkl. ETC1/ETC1A4); der Upload-Cache ist je
  (Adresse, Format, Größe) geschlüsselt und prüft bei jeder neuen
  Kommandoliste sowie nach MemoryFill/DisplayTransfer/TextureCopy einen
  FNV-1a-Hash der Quellbytes (`gpu_pica_tex_generation`).
* Wrap-Modi 0–7 inklusive Border-Farbe und Nearest/Linear (PICA mag/min-Bits;
  der Software-Pfad liest weiterhin nur den Nachbartexel, deshalb sind
  Linear-Szenen nicht pixelgleich).
* TEV als ein HLSL-Über-Shader: 6 Stufen mit Quellen, Modifern aller 16
  Quellen, Kombinierern Replace…AddThenMultiply inkl. Dot3, Skalierung und
  Combiner-Buffer (0xE0/0xFD) mit derselben Integer-Semantik wie `p_tev`.
* Alpha-Test auf dem TEV-Alpha.

`tests/hw_selftest.c` vergleicht zusätzlich texturierte TEV-Szenen
(RGBA8-Modulate, zwei Texturen mit Dot3_RGBA, ClampToBorder + Alpha-Test,
I8-Textur) pixelweise gegen den Software-Pfad; alle Szenen sind identisch.

**Offen im Hardware-Pfad:** Fragment-Lighting, Fog, Blending/LogicOps,
W-Buffer, Stencil, prozedurale Textur, Cube-/Shadow-/Projection-Texturen und
Mipmaps. Solange diese Zustände aktiv sind, zeichnet weiterhin der
Software-Rasterizer (`p_hw_supported` in `runtime/gpu/gpu.c`).


## Scheduler nach Azahar-Vorbild (Entwurf 2026-11-25, für M12/M18)

**Befund.** Alle Gast-Threads sind Host-Threads unter einer globalen
`CRITICAL_SECTION`. Fairness entsteht nur über `Sleep(1)` beim
Wiedereintritt (`guest_lock_reacquire`), Prioritäten wirken nur teilweise
(`kwait`). Am Titel sind ~95 % der Thread-Zeit Warten; der Software-Gastcode
selbst ist kaum sichtbar. Jede lokale Änderung am Timing (Sleep(0),
Übergabe per Zähler, Pump-Intervall, präziser SleepThread, GPU-Worker,
SysCore ohne Sperre) war in A/B-Messungen schlechter oder neutral
(Zahlen in `docs/GAME_NOTES.md`). Das Spiel ist an VBlanks gekoppelt:
HW-Renderer 20 sichtbare fps (3 VBlanks/Frame), Ziel 30 (2 VBlanks).

**Idee.** Die Sperre durch einen expliziten Staffelstab je 3DS-Kern ersetzen,
wie Azahars `ThreadManager`:

* Jeder Gast-Thread behält seinen Host-Thread (Stacks des rekompilierten
  Codes), wartet aber auf ein **eigenes** Ereignis.
* Ein Scheduler wählt an definierten Punkten (SVC-Rückkehr, Warten,
  Schlafen, Zeitscheibe in `hle_pump`) den **höchstpriorisierten bereiten**
  Thread des Kerns und übergibt direkt (Ereignis setzen, selbst warten) – keine
  Rennen um eine Sperre, kein `Sleep(1)`, Übergabe in Mikrosekunden.
* Bereitschaft explizit: Objekt signalisiert (`kwait`), Schlaf-Deadline
  erreicht (Deadline-Tabelle), Arbiter geweckt. Externe Quellen (VBlank,
  DSP, GPU-Interrupts, Timer) markieren Threads bereit und wecken einen
  untätigen Kern.
* Kern 0 und Kern 1 (SysCore) bekommen je einen Stab; Kernel-Zustand bleibt
  unter einer kurzen Kernel-Sperre, `ldrex/strex` per CAS (ein Prototyp
  „SysCore ohne Sperre + CAS-Monitor“ lief am 2026-11-25 stabil, brachte ohne
  Scheduler aber keinen Gewinn und wurde nicht übernommen).

**Nutzen.** Prioritäten wie auf Hardware (Render-/Sound-Thread kommen sofort
dran), keine verlorenen Millisekunden, und mit fester Zeitscheibe nahezu
deterministische Abläufe (Grundlage für M18).

**Aufwand und Risiko.** Groß: `hs_wait`, Arbiter, `SleepThread`, Yield,
Thread-Start/-Ende, DSP-/GPU-/VBlank-Anbindung. Vorgehen in Stufen auf einem
Branch, jede Stufe per A/B (Gast-Frames, Musiklücken, Titel/Namenseingabe)
gegen master; erst mergen, wenn besser und stabil.

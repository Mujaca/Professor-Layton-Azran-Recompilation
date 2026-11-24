# Spätere Ausbaustufen

Hier stehen festgehaltene Vorhaben, die **noch nicht** dran sind. Ziele und
Meilensteine selbst stehen in `docs/ROADMAP.md` und werden nur vom Menschen
geändert. Ein Eintrag hier ist kein Arbeitsauftrag, sondern eine Erinnerung
mit Zeitpunkt.

## Hardware-Renderer: PICA-Befehle in eine native Grafik-API übersetzen

**Wunsch des Nutzers (2026-11-24).**

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

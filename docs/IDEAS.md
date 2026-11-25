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
  (`gpu_pica_decode_texel`, inkl. ETC1/ETC1A4); der Upload-Cache haelt bis
  zu 32 Texturen (LRU, 64 MiB) je (Adresse, Format, Größe) und
  prüft den Inhalt per FNV-1a einmal pro Kommandoliste je Textur sowie nach
  GX-Schreiboperationen in den Bereich (`gpu_pica_tex_generation`,
  `hw_invalidate_texture_range`).
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


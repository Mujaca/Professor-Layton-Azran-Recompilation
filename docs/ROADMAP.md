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

Ab M4 sind Bildvergleiche zulässig, vorher nicht.

## Nächste Arbeitspakete (priorisiert)

0. **IPC-Reply-Layouts prüfen** (docs/WORKAROUNDS.md §5): jede Antwort in
   `runtime/kernel/hle.c` gegen Azahar (`IPC::RequestBuilder`) abgleichen,
   zuerst hid GetIPCHandles (direkt vor dem Hänger). Danach
   `wa-fs-client-slot` entfernen und Frontier messen.
1. **Recompiler-Korrektheit absichern**: Differenztest pro Funktion
   (recompilierter C-Code vs. Referenz-Interpreter, z. B. Unicorn als reines
   Testwerkzeug) für Hash-, String- und Locale-Routinen. Mehrere
   Workarounds (RomFS-Lookup, `mblen`) deuten auf Semantikfehler hin.
2. **cfg:u korrekt** (Sprache/Region EUR) → Workaround `patch_lang_uk` entfällt.
3. **Kernel-Semantik**: Mutex/Semaphore/Event/ArbitrateAddress, Thread-
   Prioritäten, IPC-Reply-Größen sauber nach Azahar-Vorbild implementieren →
   Recompiler-Hacks (`0x1079D0`, `0x12E1E4`, `0x287DD4`) entfallen.
4. **M1**: y2r:u, dsp::DSP, boss:U erreichen.
5. **GSP-Interrupts** (PDC0/PDC1/P3D) zeitlich korrekt → M2/M3.
6. **PICA-Shader + TEV** in `runtime/gpu` → M4/M5.

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
| 2026-11-24 | pica-performance | 10/10 | None | 0 |
| 2026-11-24 | ipc-folge-2 | 10/10 | None | 0 |
| 2026-11-24 | kernel-scheduler | 10/10 | None | 0 |

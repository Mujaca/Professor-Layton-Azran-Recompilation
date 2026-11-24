---
name: 3ds-port
description: Arbeitsablauf für den statischen 3DS-Port (Layton 6): bauen, natürlichen Boot messen (Frontier), nächste Divergenz zur Azahar-Referenz finden und an der Ursache (Recompiler/Kernel/Service) beheben. Laden, bevor irgendetwas gebaut, gemessen oder geändert wird.
---

# 3ds-port: Arbeitsablauf

Regeln: `AGENTS.md`. Ziel/Meilensteine: `docs/ROADMAP.md`. Schulden:
`docs/WORKAROUNDS.md`. Fakten: `docs/REFERENCE.md` (Skill `3ds-hle`).

## Die Schleife (immer genau so)

```powershell
python tools/build.py            # erster Build dauert Minuten; danach --no-gen fuer Runtime-Aenderungen
python tools/frontier.py         # misst den natuerlichen Boot gegen Azahar
```

1. **Divergenz lesen**: `frontier.py` nennt das erste fehlende Ereignis
   (z. B. `y2r:u`). Log: `out/logs/frontier.txt`.
2. **Ursache eingrenzen** – nur mit Belegen:
   * `python tools/g3ds.py trace-diff reports/azahar_trace.txt out/logs/frontier.txt --table`
   * Wo hängt der Gast? `python tools/g3ds.py where --run --thread <n>`
   * Wer schreibt Adresse X? `python tools/g3ds.py guest-refs 0x...` /
     `guest-watch --write 0x...`
   * Was macht der echte Dienst? Azahar-Quelle lesen:
     `tools/azahar/src/core/hle/service/<dienst>/`, Kernel:
     `tools/azahar/src/core/hle/kernel/`.
3. **An der Ursache beheben**, in dieser Rangfolge:
   Recompiler-Semantik (`tools/recomp`) → Kernel/SVC (`runtime/kernel`) →
   Dienst (`runtime/kernel/hle.c`) → GPU (`runtime/gpu`).
   Niemals Spielcode patchen oder Gastspeicher fälschen (AGENTS.md §2).
4. **Messen**: `frontier.py` erneut. `services` darf nicht sinken.
   Wenn ein Workaround dadurch überflüssig wird: entfernen
   (`python tools/build.py` nutzt nur `tools/workarounds/*`) und erneut messen.
5. **Prüfen + committen**: `python tools/check.py`, dann ein Commit mit
   Frontier-Zahl in der Nachricht; Zeile in `docs/ROADMAP.md` ergänzen.

## Workaround abbauen (gute Einstiegsaufgabe)

```powershell
python tools/build.py --pristine   # ganz ohne Code-Patches
python tools/frontier.py
```

Für einen einzelnen Patch: Eintrag in `tools/workarounds/code_patches.json`
vorübergehend per `python tools/workarounds/apply.py --skip 0x...` weglassen,
bauen, messen. Fällt `services`, zeigt die neue Divergenz die echte Ursache.

## Fallstricke

* `getenv()` nie im Hot Path (Block-Hooks laufen 10⁸-mal); Werte cachen.
* `GetTickCount64()` hat ~15,6 ms Auflösung; für Ticks
  `QueryPerformanceCounter` nehmen.
* Der erste Lauf nach einem Rebuild ist langsamer (Page-Ins); bei
  Timeouts nicht vorschnell Schlüsse ziehen.
* Unbekannte Aufrufziele werden im generierten Code als No-op behandelt und in
  `out/missing_calls.txt` protokolliert. Echte Ziele darin als Seed in
  `tools/recomp/seeds.txt` eintragen, nicht wegpatchen.

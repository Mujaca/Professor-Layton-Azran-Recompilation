# PORT STATE (kompakt)

Stand: Engine-Submit-Kette bis `0x13F2D4` belegt; echte PICA-Liste fehlt noch.

## Belegt

- Ring-Entry = `{vtable, objekt}`; Renderer ruft `[[entry]]+8` (`fn_001ED8FC`).
- Menue-Vtable `0x3C6FE4`, `+8` = `0x0011DB84` (ARM). Im RAM steht `0x0011DB86`
  (entsteht zur Laufzeit); erzwungenes `0x11DB84` laesst die Methode laufen.
- Kette: `0x12493C -> 0x12FC24 (Emitter) -> 0x12FD30 -> 0x139914 -> 0x13F2D4`.
  `0x139914`/`0x13F2D4` laufen (18-20x) mit `r0=CommandBuffer`, `r2=Liste`.
- `0x12FC24`-Size-Gate: `needed = 4 + 4*count(0x140140(menu+0x1a4))`.
  `0x140140` folgt `[menu+0x1a4+4] -> +0x3C -> count`; `[0x3EE2D4]=0` => count 0.
- Ring-Manager `0x42B7AC`, Init `0x126D28(ctx, base, size)`:
  setzt `+0x17C=base`, `+0x180=size>>2`, ruft 2x `0x26E180` (Pools).
  Lazy-Gate: `fn_00275994` ruft `0x13038C` nur, wenn `0x28AAF0 != 0`.
- Mit Init: Ring belegt (`+17C=0D500000`, `+180=0x400`, `+18D=1`);
  CommandBuffer `0x3EE680` wird vom Submit-Body gefuellt.
- `[gspq] id=1 list=003EE680` wird gelesen, aber `ops=0`:
  Engine-Format ist kein `(value,header)`-PICA-Format.

## Sackgassen (nicht erneut verfolgen)

- `0x27A3B4`/`0x28EF3C` = Unlink-Pass; Callback `0x192D4C` ist `bx lr`.
- Feste Allokation per `HLE_RETFORCE 284AA8=<addr>` erzeugt zyklische Listen.
- `132FF8` hart auf einen Wert zwingen bricht das Size-Gate bei count!=4.
- `0x275994` ist kein Loop (kurze Funktion).

## Reproduzierbare Laeufe

- Profil: `tools/profiles/chain.json` (alle Env-Schalter gebuendelt).
- `python tools\run_chain_direct.py`
- `python tools\watch_chain.py --write 0x42B888 --timeout 45`
- `HLE_PRECALL=addr,r0,r1,r2,r3` (einmaliger Vor-Aufruf, z. B. Ring-Init).

## Belegt (neu): qops=4 aus Engine-Descriptor

- `guest-watch 0x3EE680`: Wert `0x3ECB68` von `fn_002771EC` geschrieben.
- Descriptor `003ECB68 0 0x18 0 0 4 ...`: Count 4 bei `+0x14`.
- `gsp_engine_desc()` dekodiert dieses Format -> `ops=4`, `qops=4` (20x),
  ohne `gsp_engine_list`-Fallback.

## Naechster Schritt

Die 4 Engine-Eintraege auf echte PICA-Kommandos abbilden (bzw.
`gpu_run_pica_list`-Format erweitern), damit der GPU sie ausfuehrt und
der Menue-Frame aus der Engine kommt; Top/Bottom neu messen.

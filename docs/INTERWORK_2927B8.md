# Thumb/ARM-Interworking: 0x002927B8

## Befund

`fn_0011E494` (Archiv-Dateilader) haengt im Free-Pfad. Die Kette laeuft ueber
`0x292736` (Thumb) -> `0x2927B8` -> `0x2922FC` -> `0x292370` ->
`fn_00292650(r0 = 0x0FFEF809)` -> Fehlerpfad `fn_001009E4` bzw.
Vergleichsschleife `0x292678`.

## Ursache (statisch belegt)

An `0x2927B8` steht **ARM-Code**:

```
0x002927B8  E59F0000   ldr r0, [pc]      ; pc = 0x2927C0
0x002927BC  E12FFF1E   bx  lr
0x002927C0  00455D40   (Literal)
```

Der Aufrufer `0x292736` setzt vor dem Aufruf explizit `c->t = 0` (ARM-BLX).
Im Recompilat ist `fn_002927B8` jedoch als **Thumb** erzeugt:

```
L_002927B8_002927B8:
  arm_trace_block(c, 0x002927B8, 1);       // thumb = 1
  arm_dp(DP_MOV, 0, 0, c->r[0], 1, -1);    // mov r0,r0  (Thumb 0x0000)
  goto L_002927B8_002922FC;                // landet mitten im Thumb-Code
```

Dadurch liefert die Funktion nicht das Literal `0x00455D40`, sondern springt mit
unveraendertem `r0`/`r4` nach `0x2922FC`. `r4` ist dort ein Zufallswert
(Stack-Zeiger `0xFFEF808|1`), was den Free-Pfad und die memcmp-Schleife
`0x292678` ausloest.

## Fix-Optionen

1. Recompiler: an Adressen, die per ARM-BLX erreicht werden, ARM-Semantik
   erzeugen (Mode-Map korrigieren).
2. Runtime-Shim: bei `pc == 0x2927B8` direkt `r0 = 0x00455D40` setzen und zum
   LR zurueckkehren (Register/Thumb-Bit aus LR).
3. Binaerpatch an `0x2927B8` (Thumb-kompatibler Stub), z. B.
   `4800 4770` + Literal - Achtung: ueberschreibt `0x2927BC..0x2927C3`.

# RomFS-Lookup der Engine

## Befund (Subagent, statisch + gemessen)

Die Engine baut **keine** Dateiliste auf; jeder Lookup parst die Roh-Metadaten
im Speicher. Die drei Regionen liegen als Stream-Deskriptoren vor:

```
[3D69B0] = 09000000 / 000001E0   Master-Hash
[3D69C0] = 090001E0 / 00000DF8   IVFC-Level-1
[3D69D0] = 09000FD8 / 0000DD80   Metadaten
```

Lookup-Kette:
`0026F130` -> `002808A8` -> vtable `0032AF28` -> `00287D10` (Wide-String bis ':')
-> `[[0x3D6940]]` = `0033D8B8` -> `003991DC` -> `00212CD0` (Dir, 0x18) /
`00212E3C` (File, 0x20).

Hash: `h = seed ^ 0x75BCD15; h = c ^ ror(h,5)` pro UTF-16-Zeichen; Seed ist der
Parent-Dir-Offset.

## Warum Dateien nicht gefunden werden

Die File-Hash-Buckets zeigen in eine **einzige verkettete Liste**; Eintraege,
deren Offset **vor** dem Bucket-Kopf liegt, sind unerreichbar. Betroffen sind
5 von 610 Dateien, u. a. `product.bcsar` (Metadaten-Offset 0x948):

```
H("product.bcsar", parent=0xD4) = 0x8196F34D
Bucket 90 Kopf = 0x2060  >  0x948   ->  nicht in der Kette
```

## Fix

`tools/patch_romfs_lookup.py` biegt die Bucket-Woerter direkt auf die
betroffenen Records um (Image-Offsets = RomFS-Offsets):

| Datei | Offset | alt -> neu |
|---|---|---|
| product.bcsar | 0x12C8 | 0x2060 -> 0x0948 |
| lt6_a.fa | 0x117C | 0x8CD8 -> 0x0000 |
| m001.moflex | 0x1A74 | 0x7248 -> 0x01D8 |
| BGM_001 | 0x1504 | 0x2D70 -> 0x0984 |
| BGM_002 | 0x1640 | 0x5498 -> 0x09D0 |

## Offener Punkt: Sprache

Die Engine fragt `rom:/lt6/snd/jp/product.bcsa`, das EUR-RomFS hat nur
`/lt6/snd/uk/`. Der Language-Index kommt nicht aus `cfg:u 0x000A0002`
(wird nie gelesen); die Tabelle liegt bei `0x3D8EEC` (Getter `0x12B60C`).
Solange Index 0 = "jp" gilt, schlaegt der Lookup unabhaengig von den
Ketten-Patches fehl.

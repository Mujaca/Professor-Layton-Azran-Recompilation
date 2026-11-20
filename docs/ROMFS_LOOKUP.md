# RomFS-Lookup der Engine

## Layout, das die Engine erwartet

Die Engine liest den RomFS-Header an **Stream-Offset 0** und danach die vier
Tabellen aus den Header-Feldern (u32-Folge, 0x28 Bytes):

| Feld | Wert |
|---|---|
| header_size | 0x28 |
| dir_hash_off / size | 0x28 / 0x24 (9 Buckets) |
| dir_meta_off / size | 0x4C / 0x114 |
| file_hash_off / size | 0x160 / 0x994 (613 Buckets) |
| file_meta_off / size | 0xAF4 / 0xD284 |
| file_data_off | 0xDD80 |

Records:

```
Dir : parent+0, sibling+4, childDir+8, childFile+0xC,
      HASHNEXT+0x10, nameLenBytes+0x14, name@+0x18
File: parent+0, sibling+4, dataOffset(u64)+8, dataSize(u64)+0x10,
      HASHNEXT+0x18, nameLenBytes+0x1C, name@+0x20
```

Hash: `h = seed ^ 0x75BCD15; h = c ^ ror(h,5)` pro UTF-16-Zeichen,
Seed = Parent-Dir-Offset, Bucket = `h % (tabellenGroesse/4)`.

## Fix 1: IVFC-Praefix entfernen (erledigt)

Das SelfNCCH-Image `out/romfs_image.bin` beginnt mit einem 0x1000-Byte
IVFC-Block. Die Engine hielt diesen fuer den RomFS-Header und bekam dadurch
die Tabellengroessen 0x1E0 / 0xDF8 / 0x0 / 0xDD80 (Datei-Cursor: 0 Buckets
-> Division durch 0 -> Lookup immer "not found").

`runtime/hle/hle.c` addiert bei Reads aus `out/romfs_image.bin` **+0x1000**
auf den Datei-Offset (abschaltbar mit `HLE_ROMFS_NO_STRIP`).

Beleg (Read-Liste nach dem Fix):

```
off=0x0    len=0x28   -> 0x09000000+  (Header)
off=0x28   len=0x24   -> 0x09000000   (dir hash, 9 Buckets)
off=0x4C   len=0x114  -> 0x09000024   (dir meta)
off=0x160  len=0x994  -> 0x09000138   (file hash, 613 Buckets)
off=0xAF4  len=0xD284 -> 0x09000ACC   (file meta)
```

Cursor danach: DIR 9 Buckets / FILE 613 Buckets, Streams korrekt
(0x09000000 / 0x09000024 / 0x09000138 / 0x09000ACC).

## Fix 2: 5 unerreichbare Records (erledigt)

Die Buckets zeigen in EINE Kette; Eintraege vor dem Bucket-Kopf waren
unerreichbar. `tools/patch_romfs_lookup.py` setzt das Bucket-Wort auf den
Record und den HASHNEXT des Records auf den alten Kopf (sibling +4 bleibt).
Offline-Check: 610/610 Dateien erreichbar.

## Offen: leere erste Pfadkomponente

Der Pfad kommt als `/lt6/snd/uk/product.bcsar` (nach "rom:") an. Der
Komponenten-Splitter liefert fuer das fuehrende `/` eine Komponente mit
`lenChars = 0`, dadurch wird `""` gehasht und die Kette degeneriert.
Messung an `0x212E3C`: Deskriptor `{seed=0, lenChars=0, ptr=/lt6/...}`,
Vergleich mit leerem Key (`r0=0`), Record-Puffer = Muell.

In-place-Kuerzen des Strings an `0x33D8B8` (Sonde `HLE_PATH_FIX`) hilft
nicht, weil das String-Objekt vorher gebaut wird.

## Sprache

Die Engine baute Pfade mit `jp` (Literal @0x003CBEFE). `tools/patch_lang_uk.py`
schreibt es auf `uk`; Beleg: `[key] 'rom:/lt6/snd/uk/product.bcsar'`.

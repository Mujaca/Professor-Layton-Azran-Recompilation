/* RomFS-Overlay fuer Mods (docs/MODDING.md, Stufe A).
 *
 * Das Spiel liest sein RomFS ueber fs:USER OpenFileDirectly (SelfNCCH) nur
 * als Offsets in die Level-3-Ebene und wertet Hash-/Metatabellen selbst aus
 * (docs/REFERENCE.md, "RomFS"). Sind Mods aktiv, baut dieses Modul beim Start
 * eine virtuelle Level-3-Ebene (LayeredFS-Prinzip, Azahar
 * core/file_sys/layered_fs.cpp nur als Referenz gelesen):
 *
 *   [Header + Hash-/Metatabellen neu]  (Speicher)
 *   [Datenbereich des Originals]       (unveraendert, gleiche Relativ-Offsets)
 *   [ersetzte/neue Dateien]            (je 16 Bytes ausgerichtet)
 *
 * Quellen je Mod (mods/<name>/):
 *   romfs/<pfad>                 ersetzt bzw. ergaenzt eine RomFS-Datei
 *   fa/<archivpfad>/<eintrag>    ersetzt einen Eintrag eines Level-5-ARC0-
 *                                Archivs; das Archiv wird virtuell neu
 *                                zusammengesetzt (Dateitabelle unkomprimiert,
 *                                Daten des Originals unveraendert, neue
 *                                Eintraege angehaengt)
 *   mod.ini                      priority = N (hoeher gewinnt), name, ...
 *
 * Generisch: keine Dateinamen oder Adressen des Spiels. Ohne wirksame
 * Mod-Datei bleibt das Overlay inaktiv und die Runtime liest das Original.
 */
#ifndef ROMFS_OVERLAY_H
#define ROMFS_OVERLAY_H

#include <stddef.h>
#include <stdint.h>

/* image_path: Host-Abbild (out/romfs_image.bin), l3_base: Offset der
 * Level-3-Ebene darin (0x1000), mods_dir: Wurzel der Mods. Liefert 1, wenn
 * mindestens eine Mod-Datei wirkt (virtuelles Abbild aktiv), sonst 0. */
int rfo_init(const char *image_path, uint64_t l3_base, const char *mods_dir);
int rfo_active(void);
/* Groesse der virtuellen Level-3-Ebene. */
uint64_t rfo_size(void);
/* Liest ab Level-3-Offset; Bytes hinter dem Ende werden nicht geliefert.
 * Rueckgabe: gelesene Bytes. Thread-sicher. */
size_t rfo_read(uint64_t off, void *dst, size_t len);
void rfo_shutdown(void);

/* Level-5-Dekompression (u32-Kopf = Rohgroesse << 3 | Methode; 0 = keine,
 * 1 = LZ10, 2/3 = Huffman 4/8 Bit, 4 = RLE). Liefert malloc-Puffer oder
 * NULL; *out_len = Rohgroesse. Fuer Selbsttests exportiert. */
uint8_t *rfo_l5_decompress(const uint8_t *src, size_t n, size_t *out_len);

#endif

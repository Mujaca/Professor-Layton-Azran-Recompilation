#ifndef FA_INDEX_H
#define FA_INDEX_H

#include <stdint.h>

/* Loest `name` (z. B. "seq/menu_logo.xq") im ARC0-Archiv `archive_path` auf.
 * Rueckgabe 1 bei Erfolg, dann absoluter Offset und Groesse in *out_off /
 * *out_size. 0 = nicht gefunden. */
int fa_index_lookup(const char *archive_path, const char *name,
                    uint32_t *out_off, uint32_t *out_size);

#endif

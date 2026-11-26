/* RomFS-Overlay fuer Mods - siehe romfs_overlay.h und docs/MODDING.md.
 *
 * Referenzen (nur gelesen): Azahar core/file_sys/layered_fs.cpp (Aufbau der
 * Level-3-Tabellen), 3dbrew "RomFS" (Hash, Bucket-Anzahl), Kuriimu2
 * Level5Compressor / ARC0 (Kompressionskopf, Tabellenlayout).
 *
 * Level-3-Header (u32): header_len, dir_hash{off,len}, dir_meta{off,len},
 * file_hash{off,len}, file_meta{off,len}, file_data_off. Verzeichnis-Eintrag
 * 0x18 + Name, Datei-Eintrag 0x20 + Name (UTF-16, auf 4 Bytes aufgefuellt).
 *
 * ARC0 (Level-5): Kopf 0x48 Bytes, u32 bei 0x04..0x14 = Offsets der
 * Verzeichnistabelle, Verzeichnis-Hashtabelle, Dateitabelle, Namen, Daten.
 * Jede Tabelle ist einzeln Level-5-komprimiert. Verzeichnis-Eintrag 20 Bytes
 * {crc, u16 erstes Unterverz., u16 Anzahl, u16 erste Datei, u16 Anzahl,
 * u32 Namensbasis Dateien, u32 Name des Verzeichnisses}, Datei-Eintrag
 * 16 Bytes {crc, Namensoffset relativ zur Basis, Datenoffset relativ zum
 * Datenbeginn, Groesse}. Das Overlay aendert nur Offset/Groesse ersetzter
 * Eintraege; Namen, CRCs und Reihenfolge bleiben, damit die Suche des
 * Spiels unveraendert trifft. Die Dateitabelle wird unkomprimiert
 * (Methode 0) geschrieben; Methode 0 verwendet das Original selbst fuer die
 * Verzeichnis-Hashtabelle (Befund docs/MODDING.md).
 */
#include "romfs_overlay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#error "romfs_overlay: nur Windows-Host (FindFirstFileW)"
#endif

#define RFO_END 0xFFFFFFFFu

/* ------------------------------------------------------------------ */
/* Hilfen                                                              */

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (q == NULL) {
        printf("[mods] kein Speicher\n");
        exit(2);
    }
    return q;
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void wr64(uint8_t *p, uint64_t v) { wr32(p, (uint32_t)v); wr32(p + 4, (uint32_t)(v >> 32)); }
static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

static uint16_t wlow(uint16_t c) { return (c >= 'A' && c <= 'Z') ? (uint16_t)(c + 32) : c; }

/* UTF-16 -> UTF-8 fuer Logausgaben (statischer Ringpuffer). */
static const char *u8s(const uint16_t *s, int n) {
    static char ring[4][1024];
    static int k;
    char *o = ring[k++ & 3];
    int m = WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)s, n, o, 1023, NULL, NULL);
    o[m > 0 ? m : 0] = 0;
    return o;
}

/* ------------------------------------------------------------------ */
/* Level-5-Dekompression (Kopf: Rohgroesse << 3 | Methode)             */

static size_t l5_lz10(const uint8_t *s, size_t n, uint8_t *o, size_t raw) {
    size_t i = 4, w = 0;
    unsigned flags = 0, mask = 0;
    while (w < raw && i < n) {
        mask >>= 1;
        if (mask == 0) {
            flags = s[i++];
            mask = 0x80;
        }
        if (!(flags & mask)) {
            if (i >= n) break;
            o[w++] = s[i++];
        } else {
            unsigned pos, len, back;
            if (i + 1 >= n) break;
            pos = ((unsigned)s[i] << 8) | s[i + 1];
            i += 2;
            len = (pos >> 12) + 3;
            back = (pos & 0xFFF) + 1;
            if (back > w) return 0;
            while (len-- && w < raw) {
                o[w] = o[w - back];
                w++;
            }
        }
    }
    return w;
}

/* Nintendo-/cuecomp-Huffman mit Level-5-Kopf: Baum ab Byte 4, Bitstrom in
 * u32-Woertern (MSB zuerst). Huff4: niedriges Nibble zuerst. */
static size_t l5_huff(const uint8_t *s, size_t n, uint8_t *o, size_t raw, int bits) {
    size_t tree = 4, pak, w = 0, nxt = 0;
    uint32_t code = 0, mask = 0;
    unsigned pos, cur = 0, nb = 0;
    if (n < 6) return 0;
    pak = tree + ((size_t)s[tree] + 1) * 2;
    pos = s[tree + 1];
    while (w < raw) {
        unsigned ch;
        mask >>= 1;
        if (mask == 0) {
            if (pak + 4 > n) break;
            code = rd32(s + pak);
            pak += 4;
            mask = 0x80000000u;
        }
        nxt += ((pos & 0x3F) + 1) * 2;
        if (!(code & mask)) {
            ch = pos & 0x80;
            pos = tree + nxt < n ? s[tree + nxt] : 0;
        } else {
            ch = pos & 0x40;
            pos = tree + nxt + 1 < n ? s[tree + nxt + 1] : 0;
        }
        if (ch) {
            cur |= (pos & ((1u << bits) - 1)) << nb;
            nb += (unsigned)bits;
            if (nb == 8) {
                o[w++] = (uint8_t)cur;
                cur = 0;
                nb = 0;
            }
            pos = s[tree + 1];
            nxt = 0;
        }
    }
    return w;
}

static size_t l5_rle(const uint8_t *s, size_t n, uint8_t *o, size_t raw) {
    size_t i = 4, w = 0;
    while (w < raw && i < n) {
        unsigned f = s[i++];
        if (f & 0x80) {
            unsigned c = (f & 0x7F) + 3;
            if (i >= n) break;
            while (c-- && w < raw) o[w++] = s[i];
            i++;
        } else {
            unsigned c = (f & 0x7F) + 1;
            while (c-- && w < raw && i < n) o[w++] = s[i++];
        }
    }
    return w;
}

uint8_t *rfo_l5_decompress(const uint8_t *src, size_t n, size_t *out_len) {
    uint32_t head;
    size_t raw, got = 0;
    uint8_t *o;
    if (n < 4) return NULL;
    head = rd32(src);
    raw = head >> 3;
    o = (uint8_t *)malloc(raw ? raw : 1);
    if (o == NULL) return NULL;
    switch (head & 7) {
        case 0: got = n - 4 < raw ? n - 4 : raw; memcpy(o, src + 4, got); break;
        case 1: got = l5_lz10(src, n, o, raw); break;
        case 2: got = l5_huff(src, n, o, raw, 4); break;
        case 3: got = l5_huff(src, n, o, raw, 8); break;
        case 4: got = l5_rle(src, n, o, raw); break;
        default: break;
    }
    if (got != raw) {
        free(o);
        return NULL;
    }
    *out_len = raw;
    return o;
}

/* ------------------------------------------------------------------ */
/* Datenquellen                                                        */

enum { SEG_ZERO, SEG_IMAGE, SEG_HOST, SEG_MEM };

typedef struct {
    uint64_t voff;   /* nur in der globalen Extent-Liste */
    uint64_t len;
    int kind;
    uint64_t src;    /* Offset im Abbild bzw. in der Host-Datei */
    int host;        /* Index in g.host */
    const uint8_t *mem;
} Seg;

typedef struct {
    wchar_t *path;
    uint64_t size;
} HostFile;

typedef struct {
    char *entry;     /* Eintragspfad im Archiv (Bytes, '/' getrennt) */
    int host;
    int mod;
} FaOv;

typedef struct {
    uint16_t *name;
    int nlen;
    int parent;
    int *dirs, ndirs, cdirs;
    int *files, nfiles, cfiles;
    uint32_t meta;
} Dir;

typedef struct {
    uint16_t *name;
    int nlen;
    int parent;
    uint64_t size;
    uint64_t orig_rel;   /* Datenoffset im Original (relativ) */
    int changed;         /* 0 = Originaldaten an Originalstelle */
    int mod;             /* Mod, die die Datei ersetzt hat (-1 = keine) */
    Seg *segs;
    int nsegs;
    FaOv *fa;
    int nfa, cfa;
    uint32_t meta;
    uint64_t data_rel;
} File;

typedef struct {
    wchar_t *dir;
    char name[128];
    int prio;
} Mod;

static struct {
    int active;
    FILE *img;
    uint64_t l3_base;
    Dir *dirs;
    int ndirs, cdirs;
    File *files;
    int nfiles, cfiles;
    HostFile *host;
    int nhost, chost;
    Mod *mods;
    int nmods;
    Seg *ext;
    int next, cext;
    uint64_t vsize;
    uint8_t *meta;
    uint8_t **mems;
    int nmems, cmems;
    SRWLOCK lock;
    /* kleiner Cache offener Host-Dateien */
    FILE *hfp[8];
    int hidx[8];
    int hnext;
} g;

static const uint8_t *keep_mem(uint8_t *p) {
    if (g.nmems == g.cmems) {
        g.cmems = g.cmems ? g.cmems * 2 : 16;
        g.mems = (uint8_t **)xrealloc(g.mems, sizeof(*g.mems) * (size_t)g.cmems);
    }
    g.mems[g.nmems++] = p;
    return p;
}

static FILE *host_fp(int h) {
    int i;
    for (i = 0; i < 8; i++)
        if (g.hfp[i] != NULL && g.hidx[i] == h) return g.hfp[i];
    i = g.hnext++ & 7;
    if (g.hfp[i] != NULL) fclose(g.hfp[i]);
    g.hfp[i] = _wfopen(g.host[h].path, L"rb");
    g.hidx[i] = h;
    return g.hfp[i];
}

static void seg_fill(const Seg *s, uint64_t rel, uint8_t *dst, size_t n) {
    FILE *fp = NULL;
    size_t got = 0;
    switch (s->kind) {
        case SEG_MEM: memcpy(dst, s->mem + rel, n); return;
        case SEG_ZERO: memset(dst, 0, n); return;
        case SEG_IMAGE: fp = g.img; break;
        case SEG_HOST: fp = host_fp(s->host); break;
    }
    if (fp != NULL && _fseeki64(fp, (long long)(s->src + rel), SEEK_SET) == 0)
        got = fread(dst, 1, n, fp);
    if (got < n) memset(dst + got, 0, n - got);   /* Host-Datei kuerzer geworden */
}

/* Liest aus einer Segmentliste (Inhalt einer Datei). */
static size_t segs_read(const Seg *segs, int nsegs, uint64_t off, uint8_t *dst, size_t len) {
    size_t done = 0;
    uint64_t base = 0;
    int i;
    for (i = 0; i < nsegs && done < len; i++) {
        const Seg *s = &segs[i];
        if (off < base + s->len) {
            uint64_t rel = off - base;
            size_t n = (size_t)(s->len - rel < len - done ? s->len - rel : len - done);
            seg_fill(s, rel, dst + done, n);
            done += n;
            off += n;
        }
        base += s->len;
    }
    return done;
}

static int host_add(const wchar_t *path) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &a)) return -1;
    if (g.nhost == g.chost) {
        g.chost = g.chost ? g.chost * 2 : 16;
        g.host = (HostFile *)xrealloc(g.host, sizeof(*g.host) * (size_t)g.chost);
    }
    g.host[g.nhost].path = _wcsdup(path);
    g.host[g.nhost].size = ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    return g.nhost++;
}

/* ------------------------------------------------------------------ */
/* Baum                                                                */

static int dir_new(const uint16_t *name, int nlen, int parent) {
    Dir *d;
    if (g.ndirs == g.cdirs) {
        g.cdirs = g.cdirs ? g.cdirs * 2 : 64;
        g.dirs = (Dir *)xrealloc(g.dirs, sizeof(*g.dirs) * (size_t)g.cdirs);
    }
    d = &g.dirs[g.ndirs];
    memset(d, 0, sizeof(*d));
    d->name = (uint16_t *)xrealloc(NULL, sizeof(uint16_t) * (size_t)(nlen + 1));
    memcpy(d->name, name, sizeof(uint16_t) * (size_t)nlen);
    d->nlen = nlen;
    d->parent = parent < 0 ? g.ndirs : parent;
    if (parent >= 0) {
        Dir *p = &g.dirs[parent];
        if (p->ndirs == p->cdirs) {
            p->cdirs = p->cdirs ? p->cdirs * 2 : 4;
            p->dirs = (int *)xrealloc(p->dirs, sizeof(int) * (size_t)p->cdirs);
        }
        p->dirs[p->ndirs++] = g.ndirs;
    }
    return g.ndirs++;
}

static int file_new(const uint16_t *name, int nlen, int parent) {
    File *f;
    Dir *p;
    if (g.nfiles == g.cfiles) {
        g.cfiles = g.cfiles ? g.cfiles * 2 : 256;
        g.files = (File *)xrealloc(g.files, sizeof(*g.files) * (size_t)g.cfiles);
    }
    f = &g.files[g.nfiles];
    memset(f, 0, sizeof(*f));
    f->name = (uint16_t *)xrealloc(NULL, sizeof(uint16_t) * (size_t)(nlen + 1));
    memcpy(f->name, name, sizeof(uint16_t) * (size_t)nlen);
    f->nlen = nlen;
    f->parent = parent;
    f->mod = -1;
    p = &g.dirs[parent];
    if (p->nfiles == p->cfiles) {
        p->cfiles = p->cfiles ? p->cfiles * 2 : 8;
        p->files = (int *)xrealloc(p->files, sizeof(int) * (size_t)p->cfiles);
    }
    p->files[p->nfiles++] = g.nfiles;
    return g.nfiles++;
}

static int name_eq(const uint16_t *a, int na, const uint16_t *b, int nb) {
    int i;
    if (na != nb) return 0;
    for (i = 0; i < na; i++)
        if (wlow(a[i]) != wlow(b[i])) return 0;
    return 1;
}

static int find_child_dir(int d, const uint16_t *name, int n) {
    int i;
    for (i = 0; i < g.dirs[d].ndirs; i++) {
        Dir *c = &g.dirs[g.dirs[d].dirs[i]];
        if (name_eq(c->name, c->nlen, name, n)) return g.dirs[d].dirs[i];
    }
    return -1;
}

static int find_child_file(int d, const uint16_t *name, int n) {
    int i;
    for (i = 0; i < g.dirs[d].nfiles; i++) {
        File *c = &g.files[g.dirs[d].files[i]];
        if (name_eq(c->name, c->nlen, name, n)) return g.dirs[d].files[i];
    }
    return -1;
}

typedef struct {
    uint32_t dmeta_off, dmeta_len, fmeta_off, fmeta_len, data_off;
    uint8_t *dmeta, *fmeta;
} OrigL3;

static int load_name(const uint8_t *tab, uint32_t len, uint32_t at, uint32_t nbytes,
                     uint16_t *out, int cap) {
    int n = (int)(nbytes / 2), i;
    if (n > cap || (uint64_t)at + nbytes > len) return -1;
    for (i = 0; i < n; i++) out[i] = (uint16_t)(tab[at + 2 * i] | (tab[at + 2 * i + 1] << 8));
    return n;
}

static int load_files(const OrigL3 *o, int dir, uint32_t off) {
    int guard = 0;
    while (off != RFO_END) {
        uint16_t name[512];
        const uint8_t *m;
        int n, fi;
        if ((uint64_t)off + 0x20 > o->fmeta_len || ++guard > 1000000) return 0;
        m = o->fmeta + off;
        n = load_name(o->fmeta, o->fmeta_len, off + 0x20, rd32(m + 0x1C), name, 512);
        if (n < 0) return 0;
        fi = file_new(name, n, dir);
        g.files[fi].orig_rel = rd64(m + 8);
        g.files[fi].size = rd64(m + 0x10);
        off = rd32(m + 4);
    }
    return 1;
}

static int load_dir(const OrigL3 *o, uint32_t off, int parent, int depth, uint32_t *sibling) {
    uint16_t name[512];
    const uint8_t *m;
    int n, d;
    uint32_t c;
    if ((uint64_t)off + 0x18 > o->dmeta_len || depth > 64) return 0;
    m = o->dmeta + off;
    n = load_name(o->dmeta, o->dmeta_len, off + 0x18, rd32(m + 0x14), name, 512);
    if (n < 0) return 0;
    d = dir_new(name, n, parent);
    if (!load_files(o, d, rd32(m + 0xC))) return 0;
    c = rd32(m + 8);
    while (c != RFO_END) {
        uint32_t next;
        if (!load_dir(o, c, d, depth + 1, &next)) return 0;
        c = next;
    }
    *sibling = rd32(m + 4);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Mods einlesen                                                       */

typedef struct {
    wchar_t **v;
    int n, c;
} WList;

static int wcmp_sort(const void *a, const void *b) {
    return wcscmp(*(wchar_t *const *)a, *(wchar_t *const *)b);
}

/* Eintraege eines Verzeichnisses (ohne . / ..), sortiert. want_dirs: 1 =
 * nur Verzeichnisse, 0 = nur Dateien. */
static WList list_dir(const wchar_t *dir, int want_dirs) {
    WList l = {0};
    WIN32_FIND_DATAW fd;
    wchar_t pat[1024];
    HANDLE h;
    _snwprintf(pat, 1024, L"%ls\\*", dir);
    pat[1023] = 0;
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return l;
    do {
        int is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (is_dir != want_dirs) continue;
        if (l.n == l.c) {
            l.c = l.c ? l.c * 2 : 16;
            l.v = (wchar_t **)xrealloc(l.v, sizeof(wchar_t *) * (size_t)l.c);
        }
        l.v[l.n++] = _wcsdup(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    qsort(l.v, (size_t)l.n, sizeof(wchar_t *), wcmp_sort);
    return l;
}

static void wlist_free(WList *l) {
    int i;
    for (i = 0; i < l->n; i++) free(l->v[i]);
    free(l->v);
}

static void read_mod_ini(Mod *m) {
    wchar_t p[1024];
    char line[512];
    FILE *f;
    _snwprintf(p, 1024, L"%ls\\mod.ini", m->dir);
    p[1023] = 0;
    f = _wfopen(p, L"rb");
    if (f == NULL) return;
    while (fgets(line, sizeof(line), f)) {
        char *k = line, *v, *e;
        while (*k == ' ' || *k == '\t') k++;
        if (*k == '#' || *k == ';' || *k == '[') continue;
        v = strchr(k, '=');
        if (v == NULL) continue;
        *v++ = 0;
        for (e = v + strlen(v); e > v && (e[-1] == '\r' || e[-1] == '\n' || e[-1] == ' ' || e[-1] == '\t');) *--e = 0;
        while (*v == ' ' || *v == '\t') v++;
        for (e = k + strlen(k); e > k && (e[-1] == ' ' || e[-1] == '\t');) *--e = 0;
        if (_stricmp(k, "priority") == 0) m->prio = atoi(v);
    }
    fclose(f);
}

static int mod_cmp(const void *a, const void *b) {
    const Mod *x = (const Mod *)a, *y = (const Mod *)b;
    if (x->prio != y->prio) return x->prio < y->prio ? -1 : 1;
    return wcscmp(x->dir, y->dir);
}

static const char *mod_label(int m) {
    static char buf[4][160];
    static int k;
    char *o = buf[k++ & 3];
    if (m < 0) return "Original";
    snprintf(o, 160, "%s (priority %d)", g.mods[m].name, g.mods[m].prio);
    return o;
}

/* romfs/<pfad>: Datei ersetzen oder anlegen. comps = Pfadteile. */
static void apply_romfs_file(int mod, wchar_t **comps, int ncomp, const wchar_t *host) {
    int d = 0, i, fi, h;
    for (i = 0; i < ncomp - 1; i++) {
        int n = (int)wcslen(comps[i]);
        int c = find_child_dir(d, (const uint16_t *)comps[i], n);
        if (c < 0) c = dir_new((const uint16_t *)comps[i], n, d);
        d = c;
    }
    h = host_add(host);
    if (h < 0) return;
    {
        int n = (int)wcslen(comps[ncomp - 1]);
        fi = find_child_file(d, (const uint16_t *)comps[ncomp - 1], n);
        if (fi < 0) {
            fi = file_new((const uint16_t *)comps[ncomp - 1], n, d);
            printf("[mods] neu: %ls (%s)\n", host, mod_label(mod));
        } else if (g.files[fi].mod >= 0) {
            printf("[mods] Konflikt %ls: %s ersetzt %s\n", host, mod_label(mod),
                   mod_label(g.files[fi].mod));
        } else {
            printf("[mods] ersetzt: %ls (%s)\n", host, mod_label(mod));
        }
    }
    {
        File *f = &g.files[fi];
        f->changed = 1;
        f->mod = mod;
        f->size = g.host[h].size;
        free(f->segs);
        f->segs = (Seg *)xrealloc(NULL, sizeof(Seg));
        memset(f->segs, 0, sizeof(Seg));
        f->segs[0].kind = SEG_HOST;
        f->segs[0].host = h;
        f->segs[0].len = f->size;
        f->nsegs = 1;
    }
}

/* fa/<archivpfad>/<eintrag>: das laengste Praefix, das eine RomFS-Datei
 * ist, bezeichnet das Archiv. */
static void apply_fa_file(int mod, wchar_t **comps, int ncomp, const wchar_t *host) {
    int d = 0, i, fi = -1, h;
    char entry[1024];
    size_t el = 0;
    for (i = 0; i < ncomp - 1; i++) {
        int n = (int)wcslen(comps[i]);
        fi = find_child_file(d, (const uint16_t *)comps[i], n);
        if (fi >= 0) break;
        d = find_child_dir(d, (const uint16_t *)comps[i], n);
        if (d < 0) break;
    }
    if (fi < 0) {
        printf("[mods] fa: kein Archiv im Pfad %ls (%s), ignoriert\n", host, mod_label(mod));
        return;
    }
    for (i = i + 1; i < ncomp; i++) {
        const wchar_t *c = comps[i];
        if (el) entry[el++] = '/';
        for (; *c; c++) {
            if (*c > 0xFF || el + 2 >= sizeof(entry)) {
                printf("[mods] fa: Eintragsname nicht darstellbar: %ls\n", host);
                return;
            }
            entry[el++] = (char)*c;
        }
    }
    entry[el] = 0;
    h = host_add(host);
    if (h < 0) return;
    {
        File *f = &g.files[fi];
        for (i = 0; i < f->nfa; i++) {
            if (_stricmp(f->fa[i].entry, entry) == 0) {
                printf("[mods] Konflikt %ls: %s ersetzt %s\n", host, mod_label(mod),
                       mod_label(f->fa[i].mod));
                f->fa[i].host = h;
                f->fa[i].mod = mod;
                return;
            }
        }
        if (f->nfa == f->cfa) {
            f->cfa = f->cfa ? f->cfa * 2 : 4;
            f->fa = (FaOv *)xrealloc(f->fa, sizeof(FaOv) * (size_t)f->cfa);
        }
        f->fa[f->nfa].entry = _strdup(entry);
        f->fa[f->nfa].host = h;
        f->fa[f->nfa].mod = mod;
        f->nfa++;
    }
}

typedef void (*ApplyFn)(int mod, wchar_t **comps, int ncomp, const wchar_t *host);

static void walk(int mod, const wchar_t *dir, wchar_t **comps, int depth, ApplyFn fn) {
    WList fl, dl;
    int i;
    wchar_t p[1024];
    if (depth >= 32) return;
    fl = list_dir(dir, 0);
    for (i = 0; i < fl.n; i++) {
        _snwprintf(p, 1024, L"%ls\\%ls", dir, fl.v[i]);
        p[1023] = 0;
        comps[depth] = fl.v[i];
        fn(mod, comps, depth + 1, p);
    }
    dl = list_dir(dir, 1);
    for (i = 0; i < dl.n; i++) {
        _snwprintf(p, 1024, L"%ls\\%ls", dir, dl.v[i]);
        p[1023] = 0;
        comps[depth] = dl.v[i];
        walk(mod, p, comps, depth + 1, fn);
    }
    wlist_free(&fl);
    wlist_free(&dl);
}

/* ------------------------------------------------------------------ */
/* ARC0 neu zusammensetzen                                             */

static void seg_push(Seg **v, int *n, int *c, Seg s) {
    if (s.len == 0) return;
    if (*n == *c) {
        *c = *c ? *c * 2 : 8;
        *v = (Seg *)xrealloc(*v, sizeof(Seg) * (size_t)*c);
    }
    (*v)[(*n)++] = s;
}

/* Teilbereich [off, off+len) einer Segmentliste als Segmente anhaengen. */
static void seg_slice(const Seg *src, int nsrc, uint64_t off, uint64_t len, Seg **v, int *n, int *c) {
    uint64_t base = 0;
    int i;
    for (i = 0; i < nsrc && len > 0; i++) {
        const Seg *s = &src[i];
        if (off < base + s->len) {
            uint64_t rel = off - base;
            uint64_t take = s->len - rel < len ? s->len - rel : len;
            Seg t = *s;
            t.voff = 0;
            t.len = take;
            if (t.kind == SEG_IMAGE || t.kind == SEG_HOST) t.src += rel;
            else if (t.kind == SEG_MEM) t.mem += rel;
            seg_push(v, n, c, t);
            off += take;
            len -= take;
        }
        base += s->len;
    }
}

static const char *cstr_at(const uint8_t *names, size_t n, uint32_t at) {
    if (at >= n || memchr(names + at, 0, n - at) == NULL) return NULL;
    return (const char *)names + at;
}

static void fa_rebuild(File *f) {
    uint8_t hdr[0x48];
    uint32_t t1, t2, t3, tn, td;
    uint8_t *tabs = NULL, *dirs = NULL, *ents = NULL, *names = NULL, *out;
    size_t ndirs = 0, nents = 0, nnames = 0;
    int *match = NULL, i, nmatch = 0;
    uint64_t data_len, cur;
    Seg *segs = NULL;
    int nsegs = 0, csegs = 0;
    const char *label = u8s(f->name, f->nlen);

    if (f->nsegs == 0 || f->size < 0x48 ||
        segs_read(f->segs, f->nsegs, 0, hdr, 0x48) != 0x48 || memcmp(hdr, "ARC0", 4) != 0) {
        printf("[mods] fa: %s ist kein ARC0-Archiv, Eintraege ignoriert\n", label);
        return;
    }
    t1 = rd32(hdr + 4); t2 = rd32(hdr + 8); t3 = rd32(hdr + 12); tn = rd32(hdr + 16); td = rd32(hdr + 20);
    if (!(0x48 <= t1 && t1 <= t2 && t2 <= t3 && t3 <= tn && tn <= td && td <= f->size)) {
        printf("[mods] fa: %s: Tabellenoffsets ungueltig\n", label);
        return;
    }
    tabs = (uint8_t *)xrealloc(NULL, td);
    segs_read(f->segs, f->nsegs, 0, tabs, td);
    dirs = rfo_l5_decompress(tabs + t1, t2 - t1, &ndirs);
    ents = rfo_l5_decompress(tabs + t3, tn - t3, &nents);
    names = rfo_l5_decompress(tabs + tn, td - tn, &nnames);
    if (dirs == NULL || ents == NULL || names == NULL) {
        printf("[mods] fa: %s: Tabellen nicht dekomprimierbar\n", label);
        goto done;
    }
    match = (int *)xrealloc(NULL, sizeof(int) * (size_t)f->nfa);
    for (i = 0; i < f->nfa; i++) match[i] = -1;
    for (size_t k = 0; k + 20 <= ndirs; k += 20) {
        const uint8_t *de = dirs + k;
        unsigned ffi = (unsigned)(de[8] | (de[9] << 8)), fc = (unsigned)(de[10] | (de[11] << 8));
        const char *dn = cstr_at(names, nnames, rd32(de + 16));
        uint32_t fno = rd32(de + 12);
        if (dn == NULL) continue;
        for (unsigned e = ffi; e < ffi + fc && (size_t)(e + 1) * 16 <= nents; e++) {
            const char *fn = cstr_at(names, nnames, fno + rd32(ents + (size_t)e * 16 + 4));
            size_t dl = strlen(dn);
            if (fn == NULL) continue;
            for (i = 0; i < f->nfa; i++) {
                const char *want = f->fa[i].entry;
                if (match[i] < 0 && _strnicmp(want, dn, dl) == 0 && _stricmp(want + dl, fn) == 0)
                    match[i] = (int)e;
            }
        }
    }
    for (i = 0; i < f->nfa; i++) {
        if (match[i] < 0)
            printf("[mods] fa: %s: Eintrag %s nicht im Archiv (%s), ignoriert\n", label,
                   f->fa[i].entry, mod_label(f->fa[i].mod));
        else
            nmatch++;
    }
    if (nmatch == 0) goto done;

    data_len = f->size - td;
    cur = align_up(data_len, 16);
    for (i = 0; i < f->nfa; i++) {
        uint8_t *fe;
        if (match[i] < 0) continue;
        fe = ents + (size_t)match[i] * 16;
        wr32(fe + 8, (uint32_t)cur);
        wr32(fe + 12, (uint32_t)g.host[f->fa[i].host].size);
        printf("[mods] fa: %s/%s <- %s\n", label, f->fa[i].entry, mod_label(f->fa[i].mod));
        cur += align_up(g.host[f->fa[i].host].size, 16);
    }
    {
        uint32_t ntn = t3 + (uint32_t)align_up(4 + nents, 4);
        uint32_t ntd = ntn + (uint32_t)align_up(td - tn, 4);
        Seg s;
        out = (uint8_t *)xrealloc(NULL, ntd);
        memset(out, 0, ntd);
        memcpy(out, tabs, t3);
        wr32(out + t3, (uint32_t)(nents << 3));   /* Methode 0 */
        memcpy(out + t3 + 4, ents, nents);
        memcpy(out + ntn, tabs + tn, td - tn);
        wr32(out + 16, ntn);
        wr32(out + 20, ntd);
        keep_mem(out);
        memset(&s, 0, sizeof(s));
        s.kind = SEG_MEM;
        s.mem = out;
        s.len = ntd;
        seg_push(&segs, &nsegs, &csegs, s);
        seg_slice(f->segs, f->nsegs, td, data_len, &segs, &nsegs, &csegs);
        s.kind = SEG_ZERO;
        s.mem = NULL;
        s.len = align_up(data_len, 16) - data_len;
        seg_push(&segs, &nsegs, &csegs, s);
        for (i = 0; i < f->nfa; i++) {
            uint64_t sz;
            if (match[i] < 0) continue;
            sz = g.host[f->fa[i].host].size;
            memset(&s, 0, sizeof(s));
            s.kind = SEG_HOST;
            s.host = f->fa[i].host;
            s.len = sz;
            seg_push(&segs, &nsegs, &csegs, s);
            memset(&s, 0, sizeof(s));
            s.kind = SEG_ZERO;
            s.len = align_up(sz, 16) - sz;
            seg_push(&segs, &nsegs, &csegs, s);
        }
        free(f->segs);
        f->segs = segs;
        f->nsegs = nsegs;
        f->size = 0;
        for (i = 0; i < nsegs; i++) f->size += segs[i].len;
        f->changed = 1;
    }
done:
    free(tabs);
    free(dirs);
    free(ents);
    free(names);
    free(match);
}

/* ------------------------------------------------------------------ */
/* Level-3 neu aufbauen (Reihenfolge wie Azahar LayeredFS)             */

static uint32_t name_size(int nlen) { return (uint32_t)align_up((uint64_t)nlen * 2, 4); }

static uint32_t calc_hash(const uint16_t *name, int n, uint32_t parent) {
    uint32_t h = parent ^ 123456789u;
    int i;
    for (i = 0; i < n; i++) {
        h = (h >> 5) | (h << 27);
        h ^= name[i];
    }
    return h;
}

static uint32_t hash_table_size(uint32_t n) {
    uint32_t c;
    if (n < 3) return 3;
    if (n < 19) return n | 1;
    c = n;
    while (c % 2 == 0 || c % 3 == 0 || c % 5 == 0 || c % 7 == 0 || c % 11 == 0 || c % 13 == 0 ||
           c % 17 == 0)
        c++;
    return c;
}

static int *g_dlist, g_ndl, *g_flist, g_nfl;
static uint32_t g_dcur, g_fcur;

static void prep_dir(int d) {
    g.dirs[d].meta = g_dcur;
    g_dlist[g_ndl++] = d;
    g_dcur += 0x18 + name_size(g.dirs[d].nlen);
}

static void prep(int d) {
    Dir *p = &g.dirs[d];
    int i;
    for (i = 0; i < p->nfiles; i++) {
        File *f = &g.files[p->files[i]];
        f->meta = g_fcur;
        g_flist[g_nfl++] = p->files[i];
        g_fcur += 0x20 + name_size(f->nlen);
    }
    for (i = 0; i < p->ndirs; i++) prep_dir(p->dirs[i]);
    for (i = 0; i < p->ndirs; i++) prep(p->dirs[i]);
}

static void put_name(uint8_t *dst, const uint16_t *name, int n) {
    int i;
    memset(dst, 0, name_size(n));
    for (i = 0; i < n; i++) {
        dst[2 * i] = (uint8_t)name[i];
        dst[2 * i + 1] = (uint8_t)(name[i] >> 8);
    }
}

static void ext_push(Seg s) {
    if (s.len == 0) return;
    if (g.next == g.cext) {
        g.cext = g.cext ? g.cext * 2 : 64;
        g.ext = (Seg *)xrealloc(g.ext, sizeof(Seg) * (size_t)g.cext);
    }
    s.voff = g.vsize;
    g.ext[g.next++] = s;
    g.vsize += s.len;
}

static void rebuild(const OrigL3 *o) {
    uint32_t nd = (uint32_t)g.ndirs, nf = (uint32_t)g.nfiles, dh, fh, i;
    uint32_t *dhash, *fhash;
    uint8_t *dmeta, *fmeta, *hdr;
    uint32_t hdr_len = 0x28, o_dh, o_dm, o_fh, o_fm, data_off;
    uint64_t orig_end = 0, cur;
    Seg s;

    g_dlist = (int *)xrealloc(NULL, sizeof(int) * nd);
    g_flist = (int *)xrealloc(NULL, sizeof(int) * (nf ? nf : 1));
    g_ndl = g_nfl = 0;
    g_dcur = g_fcur = 0;
    prep_dir(0);
    prep(0);

    dh = hash_table_size(nd);
    fh = hash_table_size(nf);
    dhash = (uint32_t *)xrealloc(NULL, 4u * dh);
    fhash = (uint32_t *)xrealloc(NULL, 4u * fh);
    memset(dhash, 0xFF, 4u * dh);
    memset(fhash, 0xFF, 4u * fh);
    dmeta = (uint8_t *)xrealloc(NULL, g_dcur);
    fmeta = (uint8_t *)xrealloc(NULL, g_fcur ? g_fcur : 1);

    /* Datenoffsets: unveraenderte Dateien behalten ihre Originalstelle. */
    for (i = 0; i < nf; i++) {
        File *f = &g.files[i];
        if (!f->changed && f->orig_rel + f->size > orig_end) orig_end = f->orig_rel + f->size;
    }
    orig_end = align_up(orig_end, 16);
    cur = orig_end;
    for (i = 0; i < (uint32_t)g_nfl; i++) {
        File *f = &g.files[g_flist[i]];
        if (!f->changed) {
            f->data_rel = f->orig_rel;
        } else {
            f->data_rel = cur;
            cur += align_up(f->size, 16);
        }
    }

    for (i = 0; i < (uint32_t)g_ndl; i++) {
        Dir *d = &g.dirs[g_dlist[i]];
        uint8_t *m = dmeta + d->meta;
        uint32_t parent = g.dirs[d->parent].meta, sib = RFO_END, b;
        if (d->parent != g_dlist[i]) {
            Dir *p = &g.dirs[d->parent];
            int k;
            for (k = 0; k + 1 < p->ndirs; k++)
                if (p->dirs[k] == g_dlist[i]) sib = g.dirs[p->dirs[k + 1]].meta;
        }
        wr32(m + 0, parent);
        wr32(m + 4, sib);
        wr32(m + 8, d->ndirs ? g.dirs[d->dirs[0]].meta : RFO_END);
        wr32(m + 0xC, d->nfiles ? g.files[d->files[0]].meta : RFO_END);
        b = calc_hash(d->name, d->nlen, parent) % dh;
        wr32(m + 0x10, dhash[b]);
        dhash[b] = d->meta;
        wr32(m + 0x14, (uint32_t)d->nlen * 2);
        put_name(m + 0x18, d->name, d->nlen);
    }
    for (i = 0; i < (uint32_t)g_nfl; i++) {
        File *f = &g.files[g_flist[i]];
        Dir *p = &g.dirs[f->parent];
        uint8_t *m = fmeta + f->meta;
        uint32_t parent = p->meta, sib = RFO_END, b;
        int k;
        for (k = 0; k + 1 < p->nfiles; k++)
            if (p->files[k] == g_flist[i]) sib = g.files[p->files[k + 1]].meta;
        wr32(m + 0, parent);
        wr32(m + 4, sib);
        wr64(m + 8, f->data_rel);
        wr64(m + 0x10, f->size);
        b = calc_hash(f->name, f->nlen, parent) % fh;
        wr32(m + 0x18, fhash[b]);
        fhash[b] = f->meta;
        wr32(m + 0x1C, (uint32_t)f->nlen * 2);
        put_name(m + 0x20, f->name, f->nlen);
    }

    o_dh = hdr_len;
    o_dm = o_dh + 4 * dh;
    o_fh = o_dm + g_dcur;
    o_fm = o_fh + 4 * fh;
    data_off = (uint32_t)align_up((uint64_t)o_fm + g_fcur, 16);
    hdr = (uint8_t *)xrealloc(NULL, data_off);
    memset(hdr, 0, data_off);
    wr32(hdr + 0x00, hdr_len);
    wr32(hdr + 0x04, o_dh); wr32(hdr + 0x08, 4 * dh);
    wr32(hdr + 0x0C, o_dm); wr32(hdr + 0x10, g_dcur);
    wr32(hdr + 0x14, o_fh); wr32(hdr + 0x18, 4 * fh);
    wr32(hdr + 0x1C, o_fm); wr32(hdr + 0x20, g_fcur);
    wr32(hdr + 0x24, data_off);
    for (i = 0; i < dh; i++) wr32(hdr + o_dh + 4 * i, dhash[i]);
    memcpy(hdr + o_dm, dmeta, g_dcur);
    for (i = 0; i < fh; i++) wr32(hdr + o_fh + 4 * i, fhash[i]);
    memcpy(hdr + o_fm, fmeta, g_fcur);
    g.meta = hdr;

    g.vsize = 0;
    memset(&s, 0, sizeof(s));
    s.kind = SEG_MEM;
    s.mem = hdr;
    s.len = data_off;
    ext_push(s);
    memset(&s, 0, sizeof(s));
    s.kind = SEG_IMAGE;
    s.src = g.l3_base + o->data_off;
    s.len = orig_end;
    ext_push(s);
    for (i = 0; i < (uint32_t)g_nfl; i++) {
        File *f = &g.files[g_flist[i]];
        int k;
        uint64_t pad;
        if (!f->changed) continue;
        for (k = 0; k < f->nsegs; k++) ext_push(f->segs[k]);
        pad = align_up(f->size, 16) - f->size;
        memset(&s, 0, sizeof(s));
        s.kind = SEG_ZERO;
        s.len = pad;
        ext_push(s);
    }
    printf("[mods] RomFS virtuell: %u Verzeichnisse, %u Dateien, Metadaten 0x%X, Groesse 0x%llX\n",
           nd, nf, data_off, (unsigned long long)g.vsize);

    free(g_dlist);
    free(g_flist);
    free(dhash);
    free(fhash);
    free(dmeta);
    free(fmeta);
}

/* ------------------------------------------------------------------ */
/* Oeffentliche Schnittstelle                                          */

static wchar_t *widen(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    UINT cp = CP_UTF8;
    wchar_t *w;
    if (n <= 0) {
        cp = CP_ACP;
        n = MultiByteToWideChar(cp, 0, s, -1, NULL, 0);
    }
    w = (wchar_t *)xrealloc(NULL, sizeof(wchar_t) * (size_t)(n > 0 ? n : 1));
    if (n <= 0 || MultiByteToWideChar(cp, 0, s, -1, w, n) <= 0) w[0] = 0;
    return w;
}

int rfo_init(const char *image_path, uint64_t l3_base, const char *mods_dir) {
    uint8_t h[0x28];
    OrigL3 o;
    uint32_t sib;
    wchar_t *wmods;
    WList ml;
    wchar_t *comps[40];
    int i, any = 0;

    rfo_shutdown();
    InitializeSRWLock(&g.lock);
    g.l3_base = l3_base;
    g.img = fopen(image_path, "rb");
    if (g.img == NULL) {
        printf("[mods] RomFS-Abbild %s fehlt, Mods aus\n", image_path);
        return 0;
    }
    memset(&o, 0, sizeof(o));
    if (_fseeki64(g.img, (long long)l3_base, SEEK_SET) != 0 || fread(h, 1, 0x28, g.img) != 0x28 ||
        rd32(h) != 0x28) {
        printf("[mods] Level-3-Kopf ungueltig, Mods aus\n");
        rfo_shutdown();
        return 0;
    }
    o.dmeta_off = rd32(h + 0x0C); o.dmeta_len = rd32(h + 0x10);
    o.fmeta_off = rd32(h + 0x1C); o.fmeta_len = rd32(h + 0x20);
    o.data_off = rd32(h + 0x24);
    o.dmeta = (uint8_t *)xrealloc(NULL, o.dmeta_len);
    o.fmeta = (uint8_t *)xrealloc(NULL, o.fmeta_len);
    _fseeki64(g.img, (long long)(l3_base + o.dmeta_off), SEEK_SET);
    if (fread(o.dmeta, 1, o.dmeta_len, g.img) != o.dmeta_len) goto bad;
    _fseeki64(g.img, (long long)(l3_base + o.fmeta_off), SEEK_SET);
    if (fread(o.fmeta, 1, o.fmeta_len, g.img) != o.fmeta_len) goto bad;
    if (!load_dir(&o, 0, -1, 0, &sib)) goto bad;
    for (i = 0; i < g.nfiles; i++) {
        File *f = &g.files[i];
        f->segs = (Seg *)xrealloc(NULL, sizeof(Seg));
        memset(f->segs, 0, sizeof(Seg));
        f->segs[0].kind = SEG_IMAGE;
        f->segs[0].src = l3_base + o.data_off + f->orig_rel;
        f->segs[0].len = f->size;
        f->nsegs = 1;
    }

    wmods = widen(mods_dir);
    ml = list_dir(wmods, 1);
    g.mods = (Mod *)xrealloc(NULL, sizeof(Mod) * (size_t)(ml.n ? ml.n : 1));
    g.nmods = 0;
    for (i = 0; i < ml.n; i++) {
        Mod *m = &g.mods[g.nmods++];
        wchar_t p[1024];
        _snwprintf(p, 1024, L"%ls\\%ls", wmods, ml.v[i]);
        p[1023] = 0;
        m->dir = _wcsdup(p);
        snprintf(m->name, sizeof(m->name), "%s", u8s((const uint16_t *)ml.v[i], (int)wcslen(ml.v[i])));
        m->prio = 0;
        read_mod_ini(m);
    }
    wlist_free(&ml);
    qsort(g.mods, (size_t)g.nmods, sizeof(Mod), mod_cmp);
    printf("[mods] %d Mod(s) in %s\n", g.nmods, mods_dir);
    for (i = 0; i < g.nmods; i++) {
        wchar_t p[1024];
        printf("[mods]   %s\n", mod_label(i));
        _snwprintf(p, 1024, L"%ls\\romfs", g.mods[i].dir);
        p[1023] = 0;
        walk(i, p, comps, 0, apply_romfs_file);
    }
    for (i = 0; i < g.nmods; i++) {
        wchar_t p[1024];
        _snwprintf(p, 1024, L"%ls\\fa", g.mods[i].dir);
        p[1023] = 0;
        walk(i, p, comps, 0, apply_fa_file);
    }
    free(wmods);
    for (i = 0; i < g.nfiles; i++)
        if (g.files[i].nfa > 0) fa_rebuild(&g.files[i]);
    for (i = 0; i < g.nfiles; i++)
        if (g.files[i].changed) any = 1;
    if (!any) {
        printf("[mods] keine wirksame Mod-Datei, Original-RomFS\n");
        free(o.dmeta);
        free(o.fmeta);
        rfo_shutdown();
        return 0;
    }
    rebuild(&o);
    free(o.dmeta);
    free(o.fmeta);
    g.active = 1;
    return 1;
bad:
    printf("[mods] Level-3-Tabellen nicht lesbar, Mods aus\n");
    free(o.dmeta);
    free(o.fmeta);
    rfo_shutdown();
    return 0;
}

int rfo_active(void) { return g.active; }

uint64_t rfo_size(void) { return g.active ? g.vsize : 0; }

size_t rfo_read(uint64_t off, void *dst, size_t len) {
    uint8_t *o = (uint8_t *)dst;
    size_t done = 0;
    int lo, hi;
    if (!g.active || off >= g.vsize) return 0;
    if (len > g.vsize - off) len = (size_t)(g.vsize - off);
    AcquireSRWLockExclusive(&g.lock);
    lo = 0;
    hi = g.next - 1;
    while (lo < hi) {   /* letztes Extent mit voff <= off */
        int mid = (lo + hi + 1) / 2;
        if (g.ext[mid].voff <= off) lo = mid;
        else hi = mid - 1;
    }
    while (done < len && lo < g.next) {
        const Seg *s = &g.ext[lo];
        uint64_t rel = off - s->voff;
        size_t n = (size_t)(s->len - rel < len - done ? s->len - rel : len - done);
        seg_fill(s, rel, o + done, n);
        done += n;
        off += n;
        lo++;
    }
    ReleaseSRWLockExclusive(&g.lock);
    return done;
}

void rfo_shutdown(void) {
    int i;
    for (i = 0; i < 8; i++) {
        if (g.hfp[i] != NULL) fclose(g.hfp[i]);
        g.hfp[i] = NULL;
    }
    if (g.img != NULL) fclose(g.img);
    for (i = 0; i < g.ndirs; i++) {
        free(g.dirs[i].name);
        free(g.dirs[i].dirs);
        free(g.dirs[i].files);
    }
    for (i = 0; i < g.nfiles; i++) {
        int k;
        free(g.files[i].name);
        free(g.files[i].segs);
        for (k = 0; k < g.files[i].nfa; k++) free(g.files[i].fa[k].entry);
        free(g.files[i].fa);
    }
    for (i = 0; i < g.nhost; i++) free(g.host[i].path);
    for (i = 0; i < g.nmods; i++) free(g.mods[i].dir);
    for (i = 0; i < g.nmems; i++) free(g.mems[i]);
    free(g.dirs);
    free(g.files);
    free(g.host);
    free(g.mods);
    free(g.ext);
    free(g.meta);
    free(g.mems);
    memset(&g, 0, sizeof(g));
}

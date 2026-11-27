/* fs: Archive - Host-Abbildung (siehe fs_archive.h).
 *
 * Vorbild ist Azahar (nur als Spezifikation gelesen):
 *   core/hle/service/fs/archive.cpp, file_sys/archive_savedata.cpp,
 *   file_sys/archive_extsavedata.cpp, file_sys/archive_systemsavedata.cpp,
 *   file_sys/savedata_archive.cpp, file_sys/path_parser.cpp.
 *
 * Host-Layout (bewusst unter out/savedata, damit der Port nicht in ein
 * Benutzerverzeichnis schreibt):
 *   savedata/          <program-id:016x>/data/00000001/        (+ .metadata)
 *   extsavedata/       <save_high:08X>/<save_low:08X>/{user,boss}/ (+ metadata)
 *   sharedextsavedata/ 00048000/<save_low:08X>/{user,boss}/   (+ metadata)
 *   systemsavedata/    <save_high:08X>/<save_low:08X>/         (+ metadata)
 */
#include "fs_archive.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define FS_MKDIR(p) _mkdir(p)
#define FS_RMDIR(p) _rmdir(p)
#define FS_UNLINK(p) _unlink(p)
#else
#include <dirent.h>
#include <unistd.h>
#define FS_MKDIR(p) mkdir(p, 0777)
#define FS_RMDIR(p) rmdir(p)
#define FS_UNLINK(p) unlink(p)
#endif

#define ARCHIVE_SELFNCCH 0x00000003u
#define ARCHIVE_SAVEDATA 0x00000004u
#define ARCHIVE_EXTDATA  0x00000006u
#define ARCHIVE_SHARED   0x00000007u
#define ARCHIVE_SYSTEM   0x00000008u
#define SHARED_EXTDATA_HIGH 0x00048000u

#define MAX_ARCHIVES 24
#define MAX_DIRS 16

typedef struct {
    int used;
    uint64_t handle;
    uint32_t id;
    char root[FS_ARCHIVE_PATH_MAX];
} FsArchive;

static FsArchive g_archives[MAX_ARCHIVES];
static uint64_t g_next_archive_handle = 1;
static char g_base[FS_ARCHIVE_PATH_MAX] = "out/savedata";
static uint64_t g_program_id = 0;

/* ------------------------------------------------------------------ */
/* Host-Hilfen                                                         */

static int path_exists(const char *p) {
    struct stat st;
    return p != NULL && stat(p, &st) == 0;
}

static int path_is_dir(const char *p) {
    struct stat st;
    return p != NULL && stat(p, &st) == 0 && (st.st_mode & S_IFDIR) != 0;
}

static int path_is_file(const char *p) {
    struct stat st;
    return p != NULL && stat(p, &st) == 0 && (st.st_mode & S_IFDIR) == 0;
}

static void mkdir_parents(const char *path) {
    char tmp[FS_ARCHIVE_PATH_MAX];
    size_t len;
    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char save = tmp[i];
            tmp[i] = 0;
            FS_MKDIR(tmp);
            tmp[i] = save;
        }
    }
    FS_MKDIR(tmp);
}

static void remove_tree(const char *path) {
#ifdef _WIN32
    if (!path_exists(path)) return;
    if (!path_is_dir(path)) {
        FS_UNLINK(path);
        return;
    }
    {
        char pattern[FS_ARCHIVE_PATH_MAX + 8];
        intptr_t fh;
        struct _finddata_t fd;
        snprintf(pattern, sizeof(pattern), "%s/*", path);
        fh = _findfirst(pattern, &fd);
        if (fh != -1) {
            do {
                char child[FS_ARCHIVE_PATH_MAX];
                if (strcmp(fd.name, ".") == 0 || strcmp(fd.name, "..") == 0) continue;
                snprintf(child, sizeof(child), "%s/%s", path, fd.name);
                remove_tree(child);
            } while (_findnext(fh, &fd) == 0);
            _findclose(fh);
        }
    }
    FS_RMDIR(path);
#else
    {
        DIR *d;
        struct dirent *de;
        if (!path_exists(path)) return;
        d = opendir(path);
        if (d != NULL) {
            while ((de = readdir(d)) != NULL) {
                char child[FS_ARCHIVE_PATH_MAX];
                if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
                snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
                remove_tree(child);
            }
            closedir(d);
            FS_RMDIR(path);
        } else {
            FS_UNLINK(path);
        }
    }
#endif
}

static int dir_is_empty(const char *path) {
#ifdef _WIN32
    char pattern[FS_ARCHIVE_PATH_MAX + 8];
    intptr_t fh;
    struct _finddata_t fd;
    int empty = 1;
    snprintf(pattern, sizeof(pattern), "%s/*", path);
    fh = _findfirst(pattern, &fd);
    if (fh != -1) {
        do {
            if (strcmp(fd.name, ".") != 0 && strcmp(fd.name, "..") != 0) {
                empty = 0;
                break;
            }
        } while (_findnext(fh, &fd) == 0);
        _findclose(fh);
    }
    return empty;
#else
    DIR *d = opendir(path);
    struct dirent *de;
    int empty = 1;
    if (d == NULL) return 1;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0) {
            empty = 0;
            break;
        }
    }
    closedir(d);
    return empty;
#endif
}

/* ------------------------------------------------------------------ */
/* Gastpfade                                                           */

int fs_archive_decode_path(const uint8_t *mem, uint32_t mem_size, uint32_t type, uint32_t size,
                           uint32_t addr, uint8_t *out, size_t out_cap) {
    uint32_t off = addr & 0x1FFFFFFFu;
    if (out_cap == 0) return -1;
    out[0] = 0;
    if (type == 1) return 0; /* Empty */
    if (mem == NULL) return -1;
    if ((uint64_t)off + (uint64_t)size > (uint64_t)mem_size) return -1;
    if (type == 2) { /* Binary: rohe Bytes (z. B. ExtSaveDataInfo) */
        if (size > out_cap) return -1;
        memcpy(out, mem + off, size);
        return (int)size;
    }
    if (type == 3) { /* Char */
        uint32_t i, n = 0;
        for (i = 0; i < size && n + 1 < out_cap; i++) {
            uint8_t c = mem[off + i];
            if (c == 0) break;
            out[n++] = c;
        }
        out[n] = 0;
        return (int)n;
    }
    if (type == 4) { /* Wchar (UTF-16LE, nur ASCII relevant) */
        uint32_t i, n = 0;
        for (i = 0; i + 1 < size && n + 1 < out_cap; i += 2) {
            uint16_t c = (uint16_t)mem[off + i] | (uint16_t)(mem[off + i + 1] << 8);
            if (c == 0) break;
            out[n++] = (c < 0x80) ? (uint8_t)c : (uint8_t)'?';
        }
        out[n] = 0;
        return (int)n;
    }
    return -1;
}

/* Normalisiert einen Gastpfad relativ zum Archivwurzelverzeichnis.
 * Regeln wie Azahar path_parser.cpp: beginnt mit '/', leere und "."-Teile
 * werden verworfen, ".." darf nicht ueber die Wurzel hinaus. */
static int guest_rel(const char *in, char *out, size_t out_size) {
    size_t o = 0;
    int depth = 0;
    size_t starts[64];
    const char *p;

    if (in == NULL || out_size == 0) return 0;
    out[0] = 0;
    if (in[0] != '/') return 0;
    for (p = in; *p; p++) {
        if (strchr("<>\\|:\"*?", *p) != NULL) return 0;
    }
    p = in;
    while (*p) {
        const char *e;
        size_t len;
        while (*p == '/') p++;
        if (*p == 0) break;
        e = p;
        while (*e && *e != '/') e++;
        len = (size_t)(e - p);
        if (len == 1 && p[0] == '.') {
            p = e;
            continue;
        }
        if (len == 2 && p[0] == '.' && p[1] == '.') {
            if (depth == 0) return 0;
            depth--;
            o = starts[depth];
            while (o > 0 && out[o - 1] == '/') o--;
            out[o] = 0;
            p = e;
            continue;
        }
        if (len >= 128 || depth >= 64) return 0;
        if (o > 0) out[o++] = '/';
        starts[depth++] = o;
        if (o + len + 1 > out_size) return 0;
        memcpy(out + o, p, len);
        o += len;
        out[o] = 0;
        p = e;
    }
    return 1;
}

static void build_host(const char *root, const char *rel, char *out, size_t out_size) {
    if (rel[0] == 0) snprintf(out, out_size, "%s", root);
    else snprintf(out, out_size, "%s/%s", root, rel);
}

/* ------------------------------------------------------------------ */
/* Metadaten                                                           */

static int write_format_meta(const char *path, const FsFormatInfo *info) {
    uint8_t buf[16];
    FILE *f;
    memset(buf, 0, sizeof(buf));
    memcpy(buf + 0, &info->total_size, 4);
    memcpy(buf + 4, &info->number_directories, 4);
    memcpy(buf + 8, &info->number_files, 4);
    buf[12] = info->duplicate_data ? 1 : 0;
    f = fopen(path, "wb");
    if (f == NULL) return 0;
    if (fwrite(buf, 1, sizeof(buf), f) != sizeof(buf)) {
        fclose(f);
        return 0;
    }
    fclose(f);
    return 1;
}

static int read_format_meta(const char *path, FsFormatInfo *info) {
    uint8_t buf[16];
    FILE *f = fopen(path, "rb");
    if (f == NULL) return 0;
    memset(buf, 0, sizeof(buf));
    if (fread(buf, 1, sizeof(buf), f) < 12) {
        fclose(f);
        return 0;
    }
    fclose(f);
    memset(info, 0, sizeof(*info));
    memcpy(&info->total_size, buf + 0, 4);
    memcpy(&info->number_directories, buf + 4, 4);
    memcpy(&info->number_files, buf + 8, 4);
    info->duplicate_data = buf[12];
    return 1;
}

/* ------------------------------------------------------------------ */
/* Archivpfade                                                         */

static void savedata_paths(uint64_t program_id, char *root, size_t root_n, char *meta,
                           size_t meta_n) {
    snprintf(root, root_n, "%s/savedata/%016llx/data/00000001", g_base,
             (unsigned long long)program_id);
    snprintf(meta, meta_n, "%s/savedata/%016llx/data/00000001.metadata", g_base,
             (unsigned long long)program_id);
}

typedef struct {
    uint32_t media_type;
    uint32_t save_low;
    uint32_t save_high;
} ExtPath;

static int ext_paths(uint32_t archive_id, const uint8_t *raw, uint32_t raw_len, char *parent,
                     size_t parent_n, char *user, size_t user_n, char *meta, size_t meta_n) {
    ExtPath p;
    const char *dir;
    if (raw == NULL || raw_len < sizeof(ExtPath)) return 0;
    memcpy(&p, raw, sizeof(p));
    if (archive_id == ARCHIVE_SHARED) p.save_high = SHARED_EXTDATA_HIGH;
    dir = (archive_id == ARCHIVE_SHARED) ? "sharedextsavedata" : "extsavedata";
    snprintf(parent, parent_n, "%s/%s/%08X/%08X", g_base, dir, p.save_high, p.save_low);
    snprintf(meta, meta_n, "%s/%s/%08X/%08X/metadata", g_base, dir, p.save_high, p.save_low);
    if (user != NULL) snprintf(user, user_n, "%s/user", parent);
    return 1;
}

static void system_paths(uint32_t high, uint32_t low, char *root, size_t root_n, char *meta,
                         size_t meta_n) {
    snprintf(root, root_n, "%s/systemsavedata/%08X/%08X", g_base, high, low);
    snprintf(meta, meta_n, "%s/systemsavedata/%08X/%08X/metadata", g_base, high, low);
}

static FsArchive *archive_by_handle(uint64_t handle) {
    for (int i = 0; i < MAX_ARCHIVES; i++) {
        if (g_archives[i].used && g_archives[i].handle == handle) return &g_archives[i];
    }
    return NULL;
}

static int64_t register_archive(uint32_t id, const char *root) {
    int slot = -1;
    for (int i = 0; i < MAX_ARCHIVES; i++) {
        if (!g_archives[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return -1;
    memset(&g_archives[slot], 0, sizeof(g_archives[slot]));
    g_archives[slot].used = 1;
    g_archives[slot].id = id;
    g_archives[slot].handle = g_next_archive_handle++;
    if (g_next_archive_handle == 0) g_next_archive_handle = 1;
    snprintf(g_archives[slot].root, sizeof(g_archives[slot].root), "%s", root);
    return (int64_t)g_archives[slot].handle;
}

void fs_archive_init(const char *base_dir) {
    if (base_dir != NULL && base_dir[0] != 0) {
        snprintf(g_base, sizeof(g_base), "%s", base_dir);
    }
    memset(g_archives, 0, sizeof(g_archives));
    mkdir_parents(g_base);
}

void fs_archive_set_program_id(uint64_t program_id) {
    g_program_id = program_id;
}

uint64_t fs_archive_program_id(void) {
    return g_program_id;
}

int64_t fs_archive_open(uint32_t archive_id, const uint8_t *raw, uint32_t raw_len,
                        uint64_t program_id, uint32_t *result) {
    char root[FS_ARCHIVE_PATH_MAX];
    char meta[FS_ARCHIVE_PATH_MAX];

    if (result) *result = FS_RESULT_SUCCESS;
    switch (archive_id) {
        case ARCHIVE_SELFNCCH:
            /* Azahar ArchiveFactory_SelfNCCH::Open: das laufende Programm
             * selbst (RomFS + ExeFS-Abschnitte), kein Host-Verzeichnis. Dateien
             * oeffnet hle.c ueber binaere SelfNCCH-Pfade; alle anderen
             * Operationen lehnt resolve() ab. */
            return register_archive(archive_id, "");
        case ARCHIVE_SAVEDATA:
            savedata_paths(program_id, root, sizeof(root), meta, sizeof(meta));
            if (!path_is_dir(root)) {
                if (result) *result = FS_RESULT_NOT_FORMATTED;
                return -1;
            }
            break;
        case ARCHIVE_EXTDATA:
        case ARCHIVE_SHARED: {
            char parent[FS_ARCHIVE_PATH_MAX];
            if (!ext_paths(archive_id, raw, raw_len, parent, sizeof(parent), root, sizeof(root),
                           meta, sizeof(meta))) {
                if (result) *result = FS_RESULT_INVALID_PATH;
                return -1;
            }
            if (!path_is_dir(root)) {
                /* Azahar ArchiveFactory_ExtSaveData::Open: ExtSaveData liefert
                 * NotFound/InvalidState (120), SharedExtSaveData NotFormatted
                 * (340).  Das Systemarchiv wird NICHT hier angelegt. */
                if (result) *result = (archive_id == ARCHIVE_SHARED)
                                          ? FS_RESULT_NOT_FORMATTED
                                          : FS_RESULT_NOT_FOUND_INVALID_STATE;
                return -1;
            }
            break;
        }
        case ARCHIVE_SYSTEM: {
            ExtPath p;
            if (raw == NULL || raw_len < 8) {
                if (result) *result = FS_RESULT_INVALID_PATH;
                return -1;
            }
            memcpy(&p.save_high, raw + 0, 4);
            memcpy(&p.save_low, raw + 4, 4);
            system_paths(p.save_high, p.save_low, root, sizeof(root), meta, sizeof(meta));
            if (!path_is_dir(root)) {
                if (result) *result = FS_RESULT_NOT_FOUND;
                return -1;
            }
            break;
        }
        default:
            if (result) *result = FS_RESULT_NOT_FOUND;
            return -1;
    }
    return register_archive(archive_id, root);
}

uint32_t fs_archive_close(uint64_t handle) {
    FsArchive *a = archive_by_handle(handle);
    if (a == NULL) return FS_RESULT_INVALID_ARCHIVE_HANDLE;
    memset(a, 0, sizeof(*a));
    return FS_RESULT_SUCCESS;
}

uint32_t fs_archive_id(uint64_t handle) {
    FsArchive *a = archive_by_handle(handle);
    return (a != NULL) ? a->id : 0u;
}

int fs_archive_root(uint64_t handle, char *out, size_t out_size) {
    FsArchive *a = archive_by_handle(handle);
    if (a == NULL) return 0;
    snprintf(out, out_size, "%s", a->root);
    return 1;
}

uint32_t fs_archive_control(uint64_t handle, uint32_t action, uint32_t input_size,
                            uint32_t output_size) {
    (void) handle;
    (void) action;
    (void) input_size;
    (void) output_size;
    /* Azahar FileSys::ArchiveBackend::Control (Default): Success.  Commit und
     * Rollback haben beim Host-Dateisystem nichts zu tun; offene Dateien
     * werden bei jedem Write geflusht. */
    return FS_RESULT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Datei-/Verzeichnisoperationen                                       */

static uint32_t resolve(uint64_t handle, const char *guest, char *host, size_t host_n, char *rel,
                        size_t rel_n) {
    FsArchive *a = archive_by_handle(handle);
    if (a == NULL) return FS_RESULT_INVALID_ARCHIVE_HANDLE;
    /* Azahar SelfNCCHArchive: Delete/Rename/Create/OpenDirectory ->
     * UnsupportedOpenFlags; OpenFile mit Textpfad -> InvalidPath. */
    if (a->id == ARCHIVE_SELFNCCH) return FS_RESULT_UNSUPPORTED_OPEN_FLAGS;
    if (!guest_rel(guest, rel, rel_n)) return FS_RESULT_INVALID_PATH;
    build_host(a->root, rel, host, host_n);
    return FS_RESULT_SUCCESS;
}

uint32_t fs_archive_open_file(uint64_t handle, const char *guest_path, uint32_t mode,
                              uint32_t attributes, char *host_out, size_t host_size,
                              int *fixed_size) {
    char host[FS_ARCHIVE_PATH_MAX];
    char rel[FS_ARCHIVE_PATH_MAX];
    char parent[FS_ARCHIVE_PATH_MAX];
    uint32_t res = resolve(handle, guest_path, host, sizeof(host), rel, sizeof(rel));
    FsArchive *a = archive_by_handle(handle);
    int is_ext;
    (void) attributes;
    if (fixed_size) *fixed_size = 0;
    if (res != FS_RESULT_SUCCESS) return res;
    is_ext = (a != NULL && (a->id == ARCHIVE_EXTDATA || a->id == ARCHIVE_SHARED));
    /* Azahar SaveDataArchive::OpenFile: leerer Modus und Create ohne Write
     * werden abgelehnt; ExtSaveData verbietet das Create-Flag generell. */
    if (mode == 0) return FS_RESULT_UNSUPPORTED_OPEN_FLAGS;
    if ((mode & 4u) != 0 && (mode & 2u) == 0) return FS_RESULT_UNSUPPORTED_OPEN_FLAGS;
    if (is_ext && (mode & 4u) != 0) return FS_RESULT_UNSUPPORTED_OPEN_FLAGS;
    if (path_is_dir(host)) return FS_RESULT_UNEXPECTED_FILE_OR_DIR;
    if (!path_is_file(host)) {
        char *slash;
        FILE *f;
        if (rel[0] == 0) return FS_RESULT_FILE_NOT_FOUND;
        snprintf(parent, sizeof(parent), "%s", host);
        slash = strrchr(parent, '/');
        if (slash == NULL) return FS_RESULT_PATH_NOT_FOUND;
        *slash = 0;
        if (!path_is_dir(parent)) return FS_RESULT_PATH_NOT_FOUND;
        if ((mode & 4u) == 0) return FS_RESULT_FILE_NOT_FOUND;
        f = fopen(host, "wb");
        if (f == NULL) return FS_RESULT_FILE_NOT_FOUND;
        fclose(f);
    }
    snprintf(host_out, host_size, "%s", host);
    if (fixed_size && is_ext) *fixed_size = 1;
    return FS_RESULT_SUCCESS;
}

uint32_t fs_archive_create_file(uint64_t handle, const char *guest_path, uint64_t size,
                                uint32_t attributes) {
    char host[FS_ARCHIVE_PATH_MAX];
    char rel[FS_ARCHIVE_PATH_MAX];
    char parent[FS_ARCHIVE_PATH_MAX];
    char *slash;
    uint32_t res = resolve(handle, guest_path, host, sizeof(host), rel, sizeof(rel));
    FsArchive *a = archive_by_handle(handle);
    FILE *f;
    (void) attributes;
    if (res != FS_RESULT_SUCCESS) return res;
    if (rel[0] == 0) return FS_RESULT_INVALID_PATH;
    if (path_exists(host)) return FS_RESULT_FILE_ALREADY_EXISTS;
    /* Azahar SaveDataArchive::CreateFile mit allow_zero_size_create=false
     * (ExtSaveData/SharedExtSaveData): Groesse 0 -> UnsupportedOpenFlags. */
    if (size == 0 && a != NULL && (a->id == ARCHIVE_EXTDATA || a->id == ARCHIVE_SHARED))
        return FS_RESULT_UNSUPPORTED_OPEN_FLAGS;
    snprintf(parent, sizeof(parent), "%s", host);
    slash = strrchr(parent, '/');
    if (slash == NULL) return FS_RESULT_PATH_NOT_FOUND;
    *slash = 0;
    if (!path_is_dir(parent)) return FS_RESULT_PATH_NOT_FOUND;
    f = fopen(host, "wb");
    if (f == NULL) return FS_RESULT_FILE_NOT_FOUND;
    if (size > 0) {
        if (size > 0xFFFFFFFFull) size = 0xFFFFFFFFull;
#ifdef _WIN32
        _chsize_s(_fileno(f), (long long)size);
#else
        if (ftruncate(fileno(f), (off_t)size) != 0) { /* ignore */ }
#endif
    }
    fclose(f);
    return FS_RESULT_SUCCESS;
}

uint32_t fs_archive_delete_file(uint64_t handle, const char *guest_path) {
    char host[FS_ARCHIVE_PATH_MAX];
    char rel[FS_ARCHIVE_PATH_MAX];
    uint32_t res = resolve(handle, guest_path, host, sizeof(host), rel, sizeof(rel));
    if (res != FS_RESULT_SUCCESS) return res;
    if (!path_exists(host)) return FS_RESULT_FILE_NOT_FOUND;
    if (path_is_dir(host)) return FS_RESULT_FILE_NOT_FOUND;
    return (FS_UNLINK(host) == 0) ? FS_RESULT_SUCCESS : FS_RESULT_FILE_NOT_FOUND;
}

uint32_t fs_archive_rename_file(uint64_t src_handle, const char *src_path, uint64_t dst_handle,
                                const char *dst_path) {
    char src[FS_ARCHIVE_PATH_MAX], dst[FS_ARCHIVE_PATH_MAX];
    char src_rel[FS_ARCHIVE_PATH_MAX], dst_rel[FS_ARCHIVE_PATH_MAX];
    uint32_t res;
    if (src_handle != dst_handle) {
        /* Azahar ArchiveManager::RenameFileBetweenArchives: Unimplemented. */
        return FS_RESULT_UNSUPPORTED_OPEN_FLAGS;
    }
    res = resolve(src_handle, src_path, src, sizeof(src), src_rel, sizeof(src_rel));
    if (res != FS_RESULT_SUCCESS) return res;
    res = resolve(dst_handle, dst_path, dst, sizeof(dst), dst_rel, sizeof(dst_rel));
    if (res != FS_RESULT_SUCCESS) return res;
    if (!path_exists(src)) return FS_RESULT_FILE_NOT_FOUND;
    if (path_exists(dst)) return FS_RESULT_FILE_ALREADY_EXISTS;
    if (rename(src, dst) != 0) return FS_RESULT_FILE_NOT_FOUND;
    return FS_RESULT_SUCCESS;
}

/* Azahar SaveDataArchive::RenameDirectory (eigene Funktion, nicht RenameFile). */
uint32_t fs_archive_rename_directory(uint64_t src_handle, const char *src_path,
                                     uint64_t dst_handle, const char *dst_path) {
    char src[FS_ARCHIVE_PATH_MAX], dst[FS_ARCHIVE_PATH_MAX];
    char src_rel[FS_ARCHIVE_PATH_MAX], dst_rel[FS_ARCHIVE_PATH_MAX];
    uint32_t res;
    if (src_handle != dst_handle) {
        /* ArchiveManager::RenameDirectoryBetweenArchives: Unimplemented. */
        return FS_RESULT_UNSUPPORTED_OPEN_FLAGS;
    }
    res = resolve(src_handle, src_path, src, sizeof(src), src_rel, sizeof(src_rel));
    if (res != FS_RESULT_SUCCESS) return res;
    res = resolve(dst_handle, dst_path, dst, sizeof(dst), dst_rel, sizeof(dst_rel));
    if (res != FS_RESULT_SUCCESS) return res;
    if (src_rel[0] == 0) return FS_RESULT_UNEXPECTED_FILE_OR_DIR;
    if (!path_is_dir(src)) return FS_RESULT_PATH_NOT_FOUND;
    if (path_exists(dst)) return FS_RESULT_DIR_ALREADY_EXISTS;
    if (rename(src, dst) != 0) return FS_RESULT_PATH_NOT_FOUND;
    return FS_RESULT_SUCCESS;
}

uint32_t fs_archive_get_free_bytes(uint64_t handle, uint64_t *bytes) {
    /* Azahar SaveDataArchive::GetFreeBytes: "Stubbed to return 32MiB". */
    if (archive_by_handle(handle) == NULL) {
        if (bytes) *bytes = 0;
        return FS_RESULT_INVALID_ARCHIVE_HANDLE;
    }
    /* SelfNCCHArchive::GetFreeBytes: 0. */
    if (bytes) *bytes = (fs_archive_id(handle) == ARCHIVE_SELFNCCH) ? 0ull
                                                                     : 32ull * 1024ull * 1024ull;
    return FS_RESULT_SUCCESS;
}

uint32_t fs_archive_create_directory(uint64_t handle, const char *guest_path, uint32_t attributes) {
    char host[FS_ARCHIVE_PATH_MAX];
    char rel[FS_ARCHIVE_PATH_MAX];
    char parent[FS_ARCHIVE_PATH_MAX];
    char *slash;
    uint32_t res = resolve(handle, guest_path, host, sizeof(host), rel, sizeof(rel));
    (void) attributes;
    if (res != FS_RESULT_SUCCESS) return res;
    if (rel[0] == 0) return FS_RESULT_DIR_ALREADY_EXISTS;
    if (path_exists(host)) return FS_RESULT_DIR_ALREADY_EXISTS;
    snprintf(parent, sizeof(parent), "%s", host);
    slash = strrchr(parent, '/');
    if (slash == NULL) return FS_RESULT_PATH_NOT_FOUND;
    *slash = 0;
    if (!path_is_dir(parent)) return FS_RESULT_PATH_NOT_FOUND;
    return (FS_MKDIR(host) == 0) ? FS_RESULT_SUCCESS : FS_RESULT_PATH_NOT_FOUND;
}

uint32_t fs_archive_delete_directory(uint64_t handle, const char *guest_path, int recursive) {
    char host[FS_ARCHIVE_PATH_MAX];
    char rel[FS_ARCHIVE_PATH_MAX];
    uint32_t res = resolve(handle, guest_path, host, sizeof(host), rel, sizeof(rel));
    if (res != FS_RESULT_SUCCESS) return res;
    if (rel[0] == 0) return FS_RESULT_DIRECTORY_NOT_EMPTY;
    if (!path_exists(host)) return FS_RESULT_PATH_NOT_FOUND;
    if (!path_is_dir(host)) return FS_RESULT_UNEXPECTED_FILE_OR_DIR;
    if (recursive) {
        remove_tree(host);
        return FS_RESULT_SUCCESS;
    }
    if (!dir_is_empty(host)) return FS_RESULT_DIRECTORY_NOT_EMPTY;
    return (FS_RMDIR(host) == 0) ? FS_RESULT_SUCCESS : FS_RESULT_DIRECTORY_NOT_EMPTY;
}

uint32_t fs_archive_open_directory(uint64_t handle, const char *guest_path, char *host_out,
                                   size_t host_size) {
    char host[FS_ARCHIVE_PATH_MAX];
    char rel[FS_ARCHIVE_PATH_MAX];
    uint32_t res = resolve(handle, guest_path, host, sizeof(host), rel, sizeof(rel));
    if (res != FS_RESULT_SUCCESS) return res;
    if (!path_exists(host)) return FS_RESULT_PATH_NOT_FOUND;
    if (!path_is_dir(host)) return FS_RESULT_UNEXPECTED_FILE_OR_DIR;
    snprintf(host_out, host_size, "%s", host);
    return FS_RESULT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Formatierung                                                        */

uint32_t fs_archive_get_format_info(uint32_t archive_id, const uint8_t *raw, uint32_t raw_len,
                                    uint64_t program_id, FsFormatInfo *out) {
    char root[FS_ARCHIVE_PATH_MAX], meta[FS_ARCHIVE_PATH_MAX];
    switch (archive_id) {
        case ARCHIVE_SAVEDATA:
            savedata_paths(program_id, root, sizeof(root), meta, sizeof(meta));
            break;
        case ARCHIVE_EXTDATA:
        case ARCHIVE_SHARED: {
            char parent[FS_ARCHIVE_PATH_MAX];
            if (!ext_paths(archive_id, raw, raw_len, parent, sizeof(parent), root, sizeof(root),
                           meta, sizeof(meta)))
                return FS_RESULT_INVALID_PATH;
            break;
        }
        case ARCHIVE_SYSTEM: {
            ExtPath p;
            if (raw == NULL || raw_len < 8) return FS_RESULT_INVALID_PATH;
            memcpy(&p.save_high, raw + 0, 4);
            memcpy(&p.save_low, raw + 4, 4);
            system_paths(p.save_high, p.save_low, root, sizeof(root), meta, sizeof(meta));
            break;
        }
        default:
            return FS_RESULT_NOT_FOUND;
    }
    if (!read_format_meta(meta, out)) return FS_RESULT_NOT_FORMATTED;
    return FS_RESULT_SUCCESS;
}

uint32_t fs_archive_format(uint32_t archive_id, const uint8_t *raw, uint32_t raw_len,
                           uint64_t program_id, const FsFormatInfo *info) {
    char root[FS_ARCHIVE_PATH_MAX], meta[FS_ARCHIVE_PATH_MAX];
    switch (archive_id) {
        case ARCHIVE_SAVEDATA:
            savedata_paths(program_id, root, sizeof(root), meta, sizeof(meta));
            remove_tree(root);
            mkdir_parents(root);
            if (!write_format_meta(meta, info)) return FS_RESULT_NOT_FORMATTED;
            return FS_RESULT_SUCCESS;
        case ARCHIVE_EXTDATA:
        case ARCHIVE_SHARED:
            return fs_archive_create_ext(archive_id, raw, raw_len, info, program_id);
        case ARCHIVE_SYSTEM: {
            ExtPath p;
            if (raw == NULL || raw_len < 8) return FS_RESULT_INVALID_PATH;
            memcpy(&p.save_high, raw + 0, 4);
            memcpy(&p.save_low, raw + 4, 4);
            return fs_archive_create_system(p.save_high, p.save_low, info);
        }
        default:
            return FS_RESULT_INVALID_PATH;
    }
}

uint32_t fs_archive_create_ext(uint32_t archive_id, const uint8_t *raw, uint32_t raw_len,
                               const FsFormatInfo *info, uint64_t program_id) {
    char parent[FS_ARCHIVE_PATH_MAX], user[FS_ARCHIVE_PATH_MAX], meta[FS_ARCHIVE_PATH_MAX];
    char boss[FS_ARCHIVE_PATH_MAX];
    (void) program_id;
    if (!ext_paths(archive_id, raw, raw_len, parent, sizeof(parent), user, sizeof(user), meta,
                   sizeof(meta)))
        return FS_RESULT_INVALID_PATH;
    remove_tree(parent);
    mkdir_parents(user);
    snprintf(boss, sizeof(boss), "%s/boss", parent);
    mkdir_parents(boss);
    if (!write_format_meta(meta, info)) return FS_RESULT_NOT_FORMATTED;
    return FS_RESULT_SUCCESS;
}

uint32_t fs_archive_create_system(uint32_t high, uint32_t low, const FsFormatInfo *info) {
    char root[FS_ARCHIVE_PATH_MAX], meta[FS_ARCHIVE_PATH_MAX];
    system_paths(high, low, root, sizeof(root), meta, sizeof(meta));
    remove_tree(root);
    mkdir_parents(root);
    if (!write_format_meta(meta, info)) return FS_RESULT_NOT_FORMATTED;
    return FS_RESULT_SUCCESS;
}

void fs_archive_provision_shared(uint32_t save_low) {
    ExtPath p;
    char parent[FS_ARCHIVE_PATH_MAX], user[FS_ARCHIVE_PATH_MAX], meta[FS_ARCHIVE_PATH_MAX];
    char boss[FS_ARCHIVE_PATH_MAX];
    FsFormatInfo info;

    memset(&p, 0, sizeof(p));
    p.save_low = save_low;
    p.save_high = SHARED_EXTDATA_HIGH;
    /* ext_paths setzt save_high fuer ARCHIVE_SHARED selbst auf 0x00048000. */
    if (!ext_paths(ARCHIVE_SHARED, (const uint8_t *)&p, sizeof(p), parent, sizeof(parent), user,
                   sizeof(user), meta, sizeof(meta)))
        return;
    if (path_is_dir(user)) return; /* bereits vorhanden (Spielstand) */
    mkdir_parents(user);
    snprintf(boss, sizeof(boss), "%s/boss", parent);
    mkdir_parents(boss);
    memset(&info, 0, sizeof(info));
    info.total_size = 0x100000;
    info.number_directories = 512;
    info.number_files = 512;
    write_format_meta(meta, &info);
}

/* ------------------------------------------------------------------ */
/* Directory-Streams                                                   */

typedef struct {
    int used;
    char host[FS_ARCHIVE_PATH_MAX];
#ifdef _WIN32
    intptr_t find;
    struct _finddata_t fd;
    int has_current;
#else
    DIR *dir;
#endif
} FsDir;

static FsDir g_dirs[MAX_DIRS];

int fs_archive_dir_valid(int slot) {
    return slot >= 0 && slot < MAX_DIRS && g_dirs[slot].used;
}

int fs_archive_dir_open(const char *host_path, uint32_t *result) {
    int slot = -1;
    if (result) *result = FS_RESULT_SUCCESS;
    for (int i = 0; i < MAX_DIRS; i++) {
        if (!g_dirs[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (result) *result = FS_RESULT_NOT_FOUND;
        return -1;
    }
    memset(&g_dirs[slot], 0, sizeof(g_dirs[slot]));
    snprintf(g_dirs[slot].host, sizeof(g_dirs[slot].host), "%s", host_path);
#ifdef _WIN32
    {
        char pattern[FS_ARCHIVE_PATH_MAX + 8];
        snprintf(pattern, sizeof(pattern), "%s/*", host_path);
        g_dirs[slot].find = _findfirst(pattern, &g_dirs[slot].fd);
        if (g_dirs[slot].find == -1) {
            if (result) *result = FS_RESULT_PATH_NOT_FOUND;
            return -1;
        }
        g_dirs[slot].has_current = 1;
        g_dirs[slot].used = 1;
        return slot;
    }
#else
    g_dirs[slot].dir = opendir(host_path);
    if (g_dirs[slot].dir == NULL) {
        if (result) *result = FS_RESULT_PATH_NOT_FOUND;
        return -1;
    }
    g_dirs[slot].used = 1;
    return slot;
#endif
}

static void entry_set_filename(uint8_t *entry, const char *name) {
    size_t n = strlen(name);
    if (n > 255) n = 255;
    for (size_t i = 0; i < n; i++) {
        entry[i * 2u] = (uint8_t)name[i];
        entry[i * 2u + 1u] = 0;
    }
}

static void entry_set_83(uint8_t *entry, const char *name, int is_dir) {
    char base[9];
    char ext[4];
    size_t bi = 0, ei = 0;
    const char *dot = strrchr(name, '.');
    size_t base_len = dot ? (size_t)(dot - name) : strlen(name);
    memset(base, ' ', 8);
    base[8] = 0;
    memset(ext, ' ', 3);
    ext[3] = 0;
    for (size_t i = 0; i < base_len && bi < 8; i++) {
        char c = name[i];
        if (c == ' ') continue;
        base[bi++] = (char)((c >= 'a' && c <= 'z') ? c - 32 : c);
    }
    if (!is_dir && dot != NULL) {
        for (size_t i = 1; dot[i] && ei < 3; i++) {
            char c = dot[i];
            ext[ei++] = (char)((c >= 'a' && c <= 'z') ? c - 32 : c);
        }
    }
    memcpy(entry + 0x20C, base, 8);
    entry[0x20C + 8] = 0;
    memcpy(entry + 0x216, ext, 3);
    entry[0x216 + 3] = 0;
}

static void entry_fill(uint8_t *entry, const char *name, int is_dir, uint64_t size) {
    memset(entry, 0, 0x228);
    entry_set_filename(entry, name);
    entry_set_83(entry, name, is_dir);
    entry[0x21C] = is_dir ? 1 : 0;    /* is_directory */
    entry[0x21D] = (name[0] == '.') ? 1 : 0;
    entry[0x21E] = is_dir ? 0 : 1;    /* archive bit wie Azahar DiskDirectory */
    entry[0x21F] = 0;                 /* read-only */
    memcpy(entry + 0x220, &size, 8);
}

uint32_t fs_archive_dir_read(int slot, uint8_t *entries, uint32_t count, uint32_t *read) {
    uint32_t got = 0;
    if (read) *read = 0;
    if (!fs_archive_dir_valid(slot)) return FS_RESULT_FILE_NOT_FOUND;
    if (entries == NULL && count > 0) return FS_RESULT_INVALID_PATH;
    while (got < count) {
        const char *name = NULL;
        int is_dir = 0;
        uint64_t size = 0;
#ifdef _WIN32
        FsDir *d = &g_dirs[slot];
        while (d->has_current) {
            d->has_current = 0;
            name = d->fd.name;
            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
                if (_findnext(d->find, &d->fd) == 0) d->has_current = 1;
                name = NULL;
                continue;
            }
            is_dir = (d->fd.attrib & _A_SUBDIR) != 0;
            size = (uint64_t)d->fd.size;
            if (_findnext(d->find, &d->fd) == 0) d->has_current = 1;
            break;
        }
        if (name == NULL) break;
#else
        FsDir *d = &g_dirs[slot];
        struct dirent *de;
        struct stat st;
        char child[FS_ARCHIVE_PATH_MAX];
        do {
            de = readdir(d->dir);
        } while (de != NULL && (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0));
        if (de == NULL) break;
        name = de->d_name;
        snprintf(child, sizeof(child), "%s/%s", d->host, name);
        if (stat(child, &st) == 0) {
            is_dir = (st.st_mode & S_IFDIR) != 0;
            size = (uint64_t)st.st_size;
        }
#endif
        entry_fill(entries + (size_t)got * 0x228u, name, is_dir, size);
        got++;
    }
    if (read) *read = got;
    return FS_RESULT_SUCCESS;
}

void fs_archive_dir_close(int slot) {
    if (slot < 0 || slot >= MAX_DIRS || !g_dirs[slot].used) return;
#ifdef _WIN32
    if (g_dirs[slot].find != -1) _findclose(g_dirs[slot].find);
#else
    if (g_dirs[slot].dir != NULL) closedir(g_dirs[slot].dir);
#endif
    memset(&g_dirs[slot], 0, sizeof(g_dirs[slot]));
}

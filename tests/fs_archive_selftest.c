/* Selbsttest fuer runtime/kernel/fs_archive.c.
 *
 * Prueft die Archive ohne Spielcode: SaveData-Erststart (NotFormatted) und
 * FormatSaveData/GetFormatInfo, Ext-/SharedExt-/SystemSaveData, Datei- und
 * Verzeichnisoperationen inklusive echter Directory-Eintraege (0x228 Byte)
 * und die Azahar-Result-Codes.  Basisverzeichnis kommt als argv[1]. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "fs_archive.h"

static int g_fail = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FEHLER %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_fail++; \
    } \
} while (0)

#define CHECK_RES(expr, want) do { \
    uint32_t _r = (expr); \
    if (_r != (want)) { \
        printf("FEHLER %s:%d: %s -> %08X, erwartet %08X\n", __FILE__, __LINE__, #expr, \
               _r, (uint32_t)(want)); \
        g_fail++; \
    } \
} while (0)

static int file_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static uint64_t file_size(const char *p) {
    struct stat st;
    if (stat(p, &st) != 0) return 0xFFFFFFFFFFFFFFFFull;
    return (uint64_t)st.st_size;
}

int main(int argc, char **argv) {
    const char *base = (argc > 1) ? argv[1] : "tools/out/fs_archive_test/savedata";
    uint32_t res = 0;
    int64_t h;
    FsFormatInfo info, got;
    char host[FS_ARCHIVE_PATH_MAX];
    uint8_t extpath[12];
    uint32_t low = 0xF000000Bu, high = 0x00048000u, media = 1u;
    uint8_t wbuf[64];
    const char *s = "/gamecoin.dat";
    uint8_t syspath[8];
    int fixed = 0;

    fs_archive_init(base);
    fs_archive_set_program_id(0x00040000000F7E00ull);

    /* --- Azahar-Result-Codes (core/file_sys/errors.h, Modul FS=17) ----- */
    CHECK(FS_RESULT_INVALID_PATH == 0xE0E046BEu);
    CHECK(FS_RESULT_UNSUPPORTED_OPEN_FLAGS == 0xE0C046F8u);
    CHECK(FS_RESULT_UNEXPECTED_FILE_OR_DIR == 0xE0C04702u);
    CHECK(FS_RESULT_WRITE_BEYOND_END == 0xE0E046C1u);
    CHECK(FS_RESULT_NOT_FORMATTED == 0xC8A04554u);
    CHECK(FS_RESULT_NOT_FOUND_INVALID_STATE == 0xC8A04478u);

    /* --- Self-Test Pfaddekodierung ------------------------------------ */
    memset(wbuf, 0, sizeof(wbuf));
    for (int i = 0; s[i]; i++) wbuf[2 * i] = (uint8_t)s[i];
    CHECK(fs_archive_decode_path(wbuf, sizeof(wbuf), 4, 28, 0, (uint8_t *)host,
                                 sizeof(host)) == 13);
    CHECK(strcmp(host, s) == 0);
    CHECK(fs_archive_decode_path((const uint8_t *)s, 14, 3, 14, 0, (uint8_t *)host,
                                 sizeof(host)) == 13);
    CHECK(strcmp(host, s) == 0);
    /* Grenzpruefung: Zugriff hinter mem_size wird abgelehnt. */
    CHECK(fs_archive_decode_path((const uint8_t *)s, 4, 3, 14, 0, (uint8_t *)host,
                                 sizeof(host)) == -1);

    /* --- SaveData: erster Start ist nicht formatiert ------------------- */
    h = fs_archive_open(4, NULL, 0, fs_archive_program_id(), &res);
    CHECK(h == -1);
    CHECK_RES(res, FS_RESULT_NOT_FORMATTED);
    CHECK_RES(fs_archive_get_format_info(4, NULL, 0, fs_archive_program_id(), &got),
              FS_RESULT_NOT_FORMATTED);

    memset(&info, 0, sizeof(info));
    info.total_size = 0x20000;
    info.number_directories = 128;
    info.number_files = 64;
    info.duplicate_data = 1;
    CHECK_RES(fs_archive_format(4, NULL, 0, fs_archive_program_id(), &info), FS_RESULT_SUCCESS);

    h = fs_archive_open(4, NULL, 0, fs_archive_program_id(), &res);
    CHECK(h > 0);
    CHECK_RES(res, FS_RESULT_SUCCESS);
    CHECK(fs_archive_root((uint64_t)h, host, sizeof(host)) == 1);
    CHECK(strstr(host, "/savedata/00040000000f7e00/data/00000001") != NULL);

    CHECK_RES(fs_archive_get_format_info(4, NULL, 0, fs_archive_program_id(), &got),
              FS_RESULT_SUCCESS);
    CHECK(got.total_size == info.total_size);
    CHECK(got.number_directories == info.number_directories);
    CHECK(got.number_files == info.number_files);
    CHECK(got.duplicate_data == 1);

    /* Verzeichnis anlegen, doppelt anlegen, Datei anlegen. */
    CHECK_RES(fs_archive_create_directory((uint64_t)h, "/sav", 0), FS_RESULT_SUCCESS);
    CHECK_RES(fs_archive_create_directory((uint64_t)h, "/sav", 0),
              FS_RESULT_DIR_ALREADY_EXISTS);
    CHECK_RES(fs_archive_create_directory((uint64_t)h, "/nix/tief", 0),
              FS_RESULT_PATH_NOT_FOUND);
    CHECK_RES(fs_archive_create_file((uint64_t)h, "/sav/a.bin", 16, 0), FS_RESULT_SUCCESS);
    CHECK_RES(fs_archive_create_file((uint64_t)h, "/sav/a.bin", 16, 0),
              FS_RESULT_FILE_ALREADY_EXISTS);
    CHECK_RES(fs_archive_create_file((uint64_t)h, "/fehlt/b.bin", 4, 0),
              FS_RESULT_PATH_NOT_FOUND);

    CHECK_RES(fs_archive_open_file((uint64_t)h, "/sav/a.bin", 1, 0, host, sizeof(host), &fixed),
              FS_RESULT_SUCCESS);
    CHECK(file_size(host) == 16);
    CHECK_RES(fs_archive_open_file((uint64_t)h, "/sav/fehlt.bin", 1, 0, host, sizeof(host), &fixed),
              FS_RESULT_FILE_NOT_FOUND);
    /* SaveData erlaubt O_CREAT (Mode-Bit 2). */
    CHECK_RES(fs_archive_open_file((uint64_t)h, "/sav/neu.bin", 7, 0, host, sizeof(host), &fixed),
              FS_RESULT_SUCCESS);
    CHECK(file_exists(host));
    CHECK_RES(fs_archive_open_file((uint64_t)h, "/sav", 1, 0, host, sizeof(host), &fixed),
              FS_RESULT_UNEXPECTED_FILE_OR_DIR);
    /* Azahar SaveDataArchive::OpenFile: Mode 0 und Create ohne Write. */
    CHECK_RES(fs_archive_open_file((uint64_t)h, "/sav/a.bin", 0, 0, host, sizeof(host), &fixed),
              0xE0C046F8u);
    CHECK_RES(fs_archive_open_file((uint64_t)h, "/sav/mit4.bin", 4, 0, host, sizeof(host), &fixed),
              0xE0C046F8u);

    /* Rename/Delete */
    CHECK_RES(fs_archive_rename_file((uint64_t)h, "/sav/neu.bin", (uint64_t)h, "/sav/neu2.bin"),
              FS_RESULT_SUCCESS);
    CHECK_RES(fs_archive_rename_file((uint64_t)h, "/sav/fehlt", (uint64_t)h, "/sav/x"),
              FS_RESULT_FILE_NOT_FOUND);
    CHECK_RES(fs_archive_delete_file((uint64_t)h, "/sav/neu2.bin"), FS_RESULT_SUCCESS);
    CHECK_RES(fs_archive_delete_file((uint64_t)h, "/sav/neu2.bin"), FS_RESULT_FILE_NOT_FOUND);

    /* Directory-Objekt: Eintrag a.bin mit Groesse 16 und is_directory=0. */
    {
        int slot;
        uint8_t entries[8 * 0x228];
        uint32_t read = 0;
        uint32_t found = 0;
        CHECK_RES(fs_archive_open_directory((uint64_t)h, "/sav", host, sizeof(host)),
                  FS_RESULT_SUCCESS);
        slot = fs_archive_dir_open(host, &res);
        CHECK(slot >= 0);
        CHECK_RES(res, FS_RESULT_SUCCESS);
        CHECK_RES(fs_archive_dir_read(slot, entries, 8, &read), FS_RESULT_SUCCESS);
        for (uint32_t i = 0; i < read; i++) {
            const uint8_t *e = entries + i * 0x228;
            char name[256];
            int n = 0;
            while (n < 255 && (e[2 * n] || e[2 * n + 1])) {
                name[n] = (char)e[2 * n];
                n++;
            }
            name[n] = 0;
            if (strcmp(name, "a.bin") == 0) {
                uint64_t sz = 0;
                memcpy(&sz, e + 0x220, 8);
                CHECK(e[0x21C] == 0);
                CHECK(sz == 16);
                found = 1;
            }
        }
        CHECK(read >= 1);
        CHECK(found == 1);
        fs_archive_dir_close(slot);
        CHECK(!fs_archive_dir_valid(slot));
    }

    CHECK_RES(fs_archive_delete_directory((uint64_t)h, "/sav", 0),
              FS_RESULT_DIRECTORY_NOT_EMPTY);
    CHECK_RES(fs_archive_delete_directory((uint64_t)h, "/sav", 1), FS_RESULT_SUCCESS);
    CHECK_RES(fs_archive_open_directory((uint64_t)h, "/sav", host, sizeof(host)),
              FS_RESULT_PATH_NOT_FOUND);

    /* Ungueltige Pfade und Handles. */
    CHECK_RES(fs_archive_create_file((uint64_t)h, "ohne_slash", 4, 0), FS_RESULT_INVALID_PATH);
    CHECK_RES(fs_archive_create_file((uint64_t)h, "/../ausbruch", 4, 0), FS_RESULT_INVALID_PATH);
    CHECK_RES(fs_archive_create_file((uint64_t)h, "/a:b", 4, 0), FS_RESULT_INVALID_PATH);
    CHECK_RES(fs_archive_open_file(9999, "/x", 1, 0, host, sizeof(host), &fixed),
              FS_RESULT_INVALID_ARCHIVE_HANDLE);
    CHECK_RES(fs_archive_close((uint64_t)h), FS_RESULT_SUCCESS);
    CHECK_RES(fs_archive_close((uint64_t)h), FS_RESULT_INVALID_ARCHIVE_HANDLE);

    /* --- ExtSaveData (0x6): fehlt -> NotFound/InvalidState ------------- */
    memcpy(extpath + 0, &media, 4);
    memcpy(extpath + 4, &low, 4);
    memcpy(extpath + 8, &high, 4);
    h = fs_archive_open(6, extpath, sizeof(extpath), 0, &res);
    CHECK(h == -1);
    CHECK_RES(res, FS_RESULT_NOT_FOUND_INVALID_STATE);
    CHECK_RES(fs_archive_create_ext(6, extpath, sizeof(extpath), &info, 0), FS_RESULT_SUCCESS);
    h = fs_archive_open(6, extpath, sizeof(extpath), 0, &res);
    CHECK(h > 0);
    CHECK_RES(res, FS_RESULT_SUCCESS);
    CHECK(fs_archive_root((uint64_t)h, host, sizeof(host)) == 1);
    CHECK(strstr(host, "/extsavedata/00048000/F000000B/user") != NULL);
    CHECK_RES(fs_archive_create_file((uint64_t)h, "/x.bin", 8, 0), FS_RESULT_SUCCESS);
    CHECK_RES(fs_archive_create_file((uint64_t)h, "/leer.bin", 0, 0), 0xE0C046F8u);
    /* ExtSaveData verbietet O_CREAT. */
    CHECK_RES(fs_archive_open_file((uint64_t)h, "/y.bin", 5, 0, host, sizeof(host), &fixed),
              0xE0C046F8u);
    fixed = 0;
    CHECK_RES(fs_archive_open_file((uint64_t)h, "/x.bin", 3, 0, host, sizeof(host), &fixed),
              FS_RESULT_SUCCESS);
    CHECK(fixed == 1);   /* FixSizeDiskFile */
    CHECK_RES(fs_archive_open_file((uint64_t)h, "/x.bin", 0, 0, host, sizeof(host), &fixed),
              0xE0C046F8u);
    CHECK_RES(fs_archive_close((uint64_t)h), FS_RESULT_SUCCESS);

    /* --- SharedExtSaveData (0x7): fehlt -> NotFormatted, Start legt an -- */
    h = fs_archive_open(7, extpath, sizeof(extpath), 0, &res);
    CHECK(h == -1);
    CHECK_RES(res, 0xC8A04554u);           /* 340, InvalidState, Status */
    snprintf(host, sizeof(host), "%s/sharedextsavedata", base);
    CHECK(!file_exists(host));
    fs_archive_provision_shared(low);
    h = fs_archive_open(7, extpath, sizeof(extpath), 0, &res);
    CHECK(h > 0);
    CHECK_RES(res, FS_RESULT_SUCCESS);
    CHECK(fs_archive_root((uint64_t)h, host, sizeof(host)) == 1);
    CHECK(strstr(host, "/sharedextsavedata/00048000/F000000B/user") != NULL);
    /* RenameDirectory ist eine eigene Operation (nicht RenameFile). */
    CHECK_RES(fs_archive_create_directory((uint64_t)h, "/d1", 0), FS_RESULT_SUCCESS);
    CHECK_RES(fs_archive_rename_directory((uint64_t)h, "/d1", (uint64_t)h, "/d2"),
              FS_RESULT_SUCCESS);
    CHECK_RES(fs_archive_rename_directory((uint64_t)h, "/d1", (uint64_t)h, "/d3"),
              FS_RESULT_PATH_NOT_FOUND);
    /* Azahar SaveDataArchive::GetFreeBytes: 32 MiB. */
    {
        uint64_t free_bytes = 0;
        CHECK_RES(fs_archive_get_free_bytes((uint64_t)h, &free_bytes), FS_RESULT_SUCCESS);
        CHECK(free_bytes == 0x02000000ull);
    }
    CHECK_RES(fs_archive_close((uint64_t)h), FS_RESULT_SUCCESS);

    /* --- SystemSaveData (0x8) ----------------------------------------- */
    {
        uint32_t shigh = 0, slow = 0x00020000u;
        memcpy(syspath + 0, &shigh, 4);
        memcpy(syspath + 4, &slow, 4);
        h = fs_archive_open(8, syspath, sizeof(syspath), 0, &res);
        CHECK(h == -1);
        CHECK_RES(res, FS_RESULT_NOT_FOUND);
        CHECK_RES(fs_archive_create_system(shigh, slow, &info), FS_RESULT_SUCCESS);
        h = fs_archive_open(8, syspath, sizeof(syspath), 0, &res);
        CHECK(h > 0);
        CHECK_RES(res, FS_RESULT_SUCCESS);
        CHECK(fs_archive_root((uint64_t)h, host, sizeof(host)) == 1);
        CHECK(strstr(host, "/systemsavedata/00000000/00020000") != NULL);
        CHECK_RES(fs_archive_create_directory((uint64_t)h, "/d", 0), FS_RESULT_SUCCESS);
        CHECK_RES(fs_archive_create_file((uint64_t)h, "/d/s.bin", 4, 0), FS_RESULT_SUCCESS);
        CHECK_RES(fs_archive_control((uint64_t)h, 0, 0, 0), FS_RESULT_SUCCESS);
        CHECK_RES(fs_archive_close((uint64_t)h), FS_RESULT_SUCCESS);
    }

    if (g_fail == 0) printf("fs_archive_selftest: OK\n");
    return g_fail ? 1 : 0;
}

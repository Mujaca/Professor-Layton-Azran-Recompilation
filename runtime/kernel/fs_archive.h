/* fs: Archive (SaveData, ExtSaveData, SharedExtSaveData, SystemSaveData).
 *
 * Bildet die Azahar-Archive (core/hle/service/fs/archive.cpp,
 * file_sys/archive_savedata.cpp, archive_extsavedata.cpp,
 * archive_systemsavedata.cpp, savedata_archive.cpp) auf
 * Host-Verzeichnisse unter out/savedata/<archiv>/... ab.
 *
 * Die Archive selbst sind in 3DS-Code 64-Bit-Werte (ArchiveHandle), keine
 * Kernel-Handles.  Directory- und Datei-Objekte sind dagegen echte Sessions;
 * die Anbindung an die Kernel-Handletabelle macht hle.c.
 */
#ifndef FS_ARCHIVE_H
#define FS_ARCHIVE_H

#include <stddef.h>
#include <stdint.h>

#define FS_ARCHIVE_PATH_MAX 640

/* Azahar core/file_sys/errors.h: description | module(FS=17)<<10 |
 * summary<<21 | level<<27. */
#define FS_RESULT_SUCCESS                 0x00000000u
#define FS_RESULT_NOT_FOUND               0xC8804478u  /* 120, NotFound, Status */
#define FS_RESULT_FILE_NOT_FOUND          0xC8804470u  /* 112 */
#define FS_RESULT_PATH_NOT_FOUND          0xC8804471u  /* 113 */
#define FS_RESULT_NOT_FORMATTED           0xC8A04554u  /* 340, InvalidState */
#define FS_RESULT_NOT_FOUND_INVALID_STATE  0xC8A04478u /* 120, InvalidState */
#define FS_RESULT_INVALID_ARCHIVE_HANDLE  0xC8804465u  /* 101, ArchiveNotMounted */
#define FS_RESULT_INVALID_PATH            0xE0E042BEu  /* 702, InvalidArgument, Usage */
#define FS_RESULT_FILE_ALREADY_EXISTS     0xC8A044B4u  /* 180, NothingHappened */
#define FS_RESULT_DIR_ALREADY_EXISTS      0xC8A044B9u  /* 185, NothingHappened */
#define FS_RESULT_DIRECTORY_NOT_EMPTY     0xC92044F0u  /* 240, Canceled */
#define FS_RESULT_UNEXPECTED_FILE_OR_DIR  0xE0C04302u  /* 770, NotSupported, Usage */
#define FS_RESULT_UNSUPPORTED_OPEN_FLAGS  0xE0C042F8u  /* 760, NotSupported, Usage */
#define FS_RESULT_WRITE_BEYOND_END        0xE0E042C1u  /* 705, InvalidArgument, Usage */

typedef struct {
    uint32_t total_size;
    uint32_t number_directories;
    uint32_t number_files;
    uint8_t duplicate_data;
} FsFormatInfo;

/* Basisverzeichnis (normalerweise "out/savedata"). Legt es bei Bedarf an. */
void fs_archive_init(const char *base_dir);

/* Program-ID der FS-Session.  Der Port bildet fs:REG nicht ab; Azahar nimmt
 * ohne Registrierung 0 (ClientSlot::program_id). */
void fs_archive_set_program_id(uint64_t program_id);
uint64_t fs_archive_program_id(void);

/* Dekodiert einen Gastpfad (LowPathType: 1 Empty, 2 Binary, 3 Char nested,
 * 4 Wchar) aus dem Gastpuffer.  Liefert die Laenge in Bytes oder -1. */
int fs_archive_decode_path(const uint8_t *mem, uint32_t type, uint32_t size, uint32_t addr,
                           uint8_t *out, size_t out_cap);

/* Oeffnet ein Archiv.  raw/raw_len ist der dekodierte Archivpfad (binary/char).
 * Liefert einen ArchiveHandle > 0 oder -1; *result traegt den Azahar-Code. */
int64_t fs_archive_open(uint32_t archive_id, const uint8_t *raw, uint32_t raw_len,
                        uint64_t program_id, uint32_t *result);
uint32_t fs_archive_close(uint64_t handle);

/* Kopiert den Host-Wurzelpfad eines offenen Archivs. 0 bei Erfolg. */
int fs_archive_root(uint64_t handle, char *out, size_t out_size);

/* ControlArchive: Azahar SaveDataArchive erbt den Default (Success).  Der
 * Port quittiert Commit/Rollback ebenso; offene Dateien werden geflusht. */
uint32_t fs_archive_control(uint64_t handle, uint32_t action, uint32_t input_size,
                            uint32_t output_size);

/* Datei-/Verzeichnisoperationen (Pfade relativ, mit fuehrendem '/'). */
uint32_t fs_archive_open_file(uint64_t handle, const char *guest_path, uint32_t mode,
                              uint32_t attributes, char *host_out, size_t host_size);
uint32_t fs_archive_create_file(uint64_t handle, const char *guest_path, uint64_t size,
                                uint32_t attributes);
uint32_t fs_archive_delete_file(uint64_t handle, const char *guest_path);
uint32_t fs_archive_rename_file(uint64_t src_handle, const char *src_path, uint64_t dst_handle,
                                const char *dst_path);
uint32_t fs_archive_create_directory(uint64_t handle, const char *guest_path, uint32_t attributes);
uint32_t fs_archive_delete_directory(uint64_t handle, const char *guest_path, int recursive);
uint32_t fs_archive_open_directory(uint64_t handle, const char *guest_path, char *host_out,
                                   size_t host_size);

/* Formatierung/Format-Info. */
uint32_t fs_archive_get_format_info(uint32_t archive_id, const uint8_t *raw, uint32_t raw_len,
                                    uint64_t program_id, FsFormatInfo *out);
uint32_t fs_archive_format(uint32_t archive_id, const uint8_t *raw, uint32_t raw_len,
                           uint64_t program_id, const FsFormatInfo *info);
uint32_t fs_archive_create_ext(uint32_t archive_id, const uint8_t *raw, uint32_t raw_len,
                               const FsFormatInfo *info, uint64_t program_id);
uint32_t fs_archive_create_system(uint32_t high, uint32_t low, const FsFormatInfo *info);

/* Directory-Stream (echtes Handle-Objekt in hle.c).  fs_archive_dir_open
 * liefert einen Slot >= 0 oder -1. */
int fs_archive_dir_open(const char *host_path, uint32_t *result);
uint32_t fs_archive_dir_read(int slot, uint8_t *entries, uint32_t count, uint32_t *read);
void fs_archive_dir_close(int slot);
int fs_archive_dir_valid(int slot);

#endif

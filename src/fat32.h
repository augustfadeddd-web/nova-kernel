#ifndef NOVA_FAT32_H
#define NOVA_FAT32_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Minimal FAT32 implementation on top of the ATA driver: mount, format a
 * blank disk, read/write/seek files, mkdir/remove/rename.  On-disk names are
 * 8.3 (long names map deterministically to their 8.3 truncation, so the
 * volume stays standard and host-readable). */
bool fat32_mount(void);
bool fat32_mounted(void);

void *fat32_open(const char *path, const char *mode);
size_t fat32_read(void *handle, void *buffer, size_t size, size_t count);
size_t fat32_write(void *handle, const void *buffer, size_t size, size_t count);
int fat32_seek(void *handle, long offset, int whence);
long fat32_tell(void *handle);
uint32_t fat32_length(void *handle);
int fat32_close(void *handle);

bool fat32_exists(const char *path);
bool fat32_mkdir(const char *path);
int fat32_remove(const char *path);
int fat32_rename(const char *old_path, const char *new_path);

/* Directory listing for the file manager.  Names are reconstructed from the
 * on-disk 8.3 entries ("NAME.EXT"); dot entries are skipped. */
#define FAT32_NAME_MAX 13
typedef struct {
    char name[FAT32_NAME_MAX];
    uint32_t size;
    bool is_dir;
} fat32_dirent;
/* Lists the entries of the directory at path ("/" is the root).  Returns the
 * number of entries written (at most capacity). */
int fat32_list(const char *path, fat32_dirent *entries, int capacity);
/* Total sectors, total data clusters and the number of unused clusters. */
void fat32_disk_usage(uint32_t *total_sectors, uint32_t *total_clusters,
                      uint32_t *free_clusters);

#endif

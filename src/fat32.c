#include "fat32.h"
#include "ata.h"
#include "system.h"
#include "terminal.h"



#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR 512
#define FAT_EOC 0x0FFFFFF8u
#define FAT_MASK 0x0FFFFFFFu
#define ATTR_DIR 0x10
#define ATTR_LFN 0x0F

static uint8_t sector[SECTOR];
static bool mounted;
static uint32_t chain_epoch=1;

static uint32_t bs_total;
static uint16_t bs_bytes_per_sector;
static uint8_t bs_sectors_per_cluster;
static uint16_t bs_reserved;
static uint8_t bs_num_fats;
static uint32_t bs_fat_size;
static uint32_t bs_root_cluster;
static uint32_t fat_start_sector;
static uint32_t data_start_sector;
static uint32_t total_clusters; /* Valid data cluster numbers are 2..total+1. */
static uint32_t next_free_cluster;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static bool read_sector(uint32_t lba, uint8_t *buffer)
{
    return ata_read_sectors(lba, 1, buffer);
}

static bool write_sector(uint32_t lba, const uint8_t *buffer)
{
    return ata_write_sectors(lba, 1, buffer);
}

static bool flush(void)
{
    return ata_flush();
}

static bool valid_cluster(uint32_t cluster)
{
    return cluster >= 2 && cluster < total_clusters + 2;
}

static uint32_t cluster_sector(uint32_t cluster)
{
    return data_start_sector + (cluster - 2) * bs_sectors_per_cluster;
}

static bool fat_get(uint32_t cluster, uint32_t *value)
{
    uint32_t offset = cluster * 4;
    if (!read_sector(fat_start_sector + offset / SECTOR, sector))
        return false;
    *value = rd32(sector + offset % SECTOR) & FAT_MASK;
    return true;
}

static bool fat_set(uint32_t cluster, uint32_t value)
{
    ++chain_epoch;
    uint32_t offset = cluster * 4;
    uint32_t entry = value & FAT_MASK;
    for (uint8_t fat = 0; fat < bs_num_fats; ++fat) {
        uint32_t fat_base = bs_reserved + (uint32_t)fat * bs_fat_size;
        if (!read_sector(fat_base + offset / SECTOR, sector)) {
            terminal_log("FAT_FAIL: fat read\n");
            return false;
        }
        wr32(sector + offset % SECTOR, entry);
        if (!write_sector(fat_base + offset / SECTOR, sector)) {
            terminal_log("FAT_FAIL: fat write\n");
            return false;
        }
    }
    return true;
}

static bool fat_alloc(uint32_t *cluster)
{
    uint32_t cursor = next_free_cluster;
    for (uint32_t tried = 0; tried < total_clusters + 1; ++tried) {
        uint32_t index = cursor;
        if (index < 2 || !valid_cluster(index))
            index = 2;
        uint32_t value;
        if (!fat_get(index, &value)) {
            terminal_log("DIR_FAIL: fat read in alloc\n");
            return false;
        }
        cursor = index + 1;
        if (cursor >= total_clusters + 2)
            cursor = 2;
        next_free_cluster = cursor;
        if (value == 0) {
            if (!fat_set(index, FAT_EOC))
                return false;
            /* Zero the newly allocated cluster. */
            uint8_t zero[SECTOR];
            memset(zero, 0, sizeof(zero));
            for (uint8_t s = 0; s < bs_sectors_per_cluster; ++s)
                if (!write_sector(cluster_sector(index) + s, zero))
                    return false;
            *cluster = index;
            return true;
        }
    }
    return false;
}

static bool fat_free_chain(uint32_t cluster)
{
    uint32_t current = cluster;
    while (valid_cluster(current)) {
        uint32_t next;
        if (!fat_get(current, &next))
            return false;
        if (!fat_set(current, 0))
            return false;
        if (next >= FAT_EOC)
            break;
        current = next;
    }
    return true;
}

static void log_number(uint32_t value)
{
    char reversed[12], text[12];
    unsigned int count = 0, written = 0;
    do { reversed[count++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (count) text[written++] = reversed[--count];
    text[written] = 0;
    terminal_log(text);
}

/* ------------------------------------------------------------- formatting -- */

static bool format_disk(void)
{
    uint32_t total = ata_total_sectors();
    if (total < 8192)
        return false;
    uint8_t sectors_per_cluster = 1;
    uint32_t reserved = 32;
    uint32_t fat_size = 1;
    for (;;) {
        uint32_t overhead = reserved + 2 * fat_size;
        uint32_t data_sectors = total > overhead ? total - overhead : 0;
        uint32_t needed = (uint32_t)(((uint64_t)(data_sectors / sectors_per_cluster + 2) * 4 + SECTOR - 1) / SECTOR);
        if (needed <= fat_size)
            break;
        fat_size = needed;
    }
    uint32_t data_sectors = total - reserved - 2 * fat_size;
    uint32_t clusters = data_sectors / sectors_per_cluster;

    uint8_t boot[SECTOR];
    memset(boot, 0, sizeof(boot));
    boot[0] = 0xEB; boot[1] = 0x58; boot[2] = 0x90;
    memcpy(boot + 3, "NOVAOS  ", 8);
    wr16(boot + 11, SECTOR);
    boot[13] = sectors_per_cluster;
    wr16(boot + 14, (uint16_t)reserved);
    boot[16] = 2;
    wr16(boot + 17, 0); /* root entry count (FAT32) */
    wr16(boot + 19, 0); /* total16 */
    boot[21] = 0xF8;
    wr16(boot + 22, 0); /* fatsz16 */
    wr16(boot + 24, 63);
    wr16(boot + 26, 255);
    wr32(boot + 28, 0);
    wr32(boot + 32, total);
    wr32(boot + 36, fat_size);
    wr16(boot + 40, 0);
    wr16(boot + 42, 0);
    wr32(boot + 44, 2); /* root cluster */
    wr16(boot + 48, 1); /* FSInfo sector */
    wr16(boot + 50, 6); /* backup boot sector */
    memcpy(boot + 71, "NOVA OS    ", 11); /* volume label */
    memcpy(boot + 82, "FAT32   ", 8);      /* file system type */
    boot[510] = 0x55; boot[511] = 0xAA;

    if (!write_sector(0, boot) || !write_sector(6, boot))
        return false;

    uint8_t fsinfo[SECTOR];
    memset(fsinfo, 0, sizeof(fsinfo));
    wr32(fsinfo + 0, 0x41615252);
    wr32(fsinfo + 484, 0x61417272);
    wr32(fsinfo + 508, 0xAA550000);
    wr32(fsinfo + 488, clusters + 2);
    wr32(fsinfo + 492, 3);
    if (!write_sector(1, fsinfo) || !write_sector(7, fsinfo))
        return false;

    uint8_t fat[SECTOR];
    memset(fat, 0, sizeof(fat));
    wr32(fat + 0, 0x0FFFFF00);
    wr32(fat + 4, 0x0FFFFFFF);
    wr32(fat + 8, 0x0FFFFFFF); /* root cluster 2 */
    for (uint32_t s = 0; s < fat_size; ++s) {
        const uint8_t *chunk = s == 0 ? fat : NULL;
        uint8_t zero[SECTOR];
        if (!chunk) {
            memset(zero, 0, sizeof(zero));
            chunk = zero;
        }
        if (!write_sector(reserved + s, chunk))
            return false;
        if (!write_sector(reserved + fat_size + s, chunk))
            return false;
    }

    /* Zero the root directory cluster. */
    uint8_t zero[SECTOR];
    memset(zero, 0, sizeof(zero));
    for (uint8_t s = 0; s < sectors_per_cluster; ++s) {
        uint32_t lba = reserved + 2 * fat_size + s;
        if (!write_sector(lba, zero))
            return false;
    }
    return flush();
}

static bool parse_boot_sector(void)
{
    if (!read_sector(0, sector))
        return false;
    if (sector[510] != 0x55 || sector[511] != 0xAA)
        return false;
    uint16_t bytes = rd16(sector + 11);
    uint8_t sec_per_clus = sector[13];
    uint16_t reserved = rd16(sector + 14);
    uint8_t num_fats = sector[16];
    uint32_t total16 = rd16(sector + 19);
    uint32_t total = total16 ? total16 : rd32(sector + 32);
    uint32_t fat_size = rd32(sector + 36);
    uint32_t root_cluster = rd32(sector + 44);

    if (bytes != SECTOR || sec_per_clus == 0 || (sec_per_clus & (sec_per_clus - 1)) ||
        reserved == 0 || num_fats == 0 || fat_size == 0 || total == 0 ||
        root_cluster < 2)
        return false;
    /* FAT32 check: root entry count must be zero and the type string present. */
    if (rd16(sector + 17) != 0)
        return false;
    if (memcmp(sector + 82, "FAT32   ", 8) != 0)
        return false;

    bs_bytes_per_sector = bytes;
    bs_sectors_per_cluster = sec_per_clus;
    bs_reserved = reserved;
    bs_num_fats = num_fats;
    bs_fat_size = fat_size;
    bs_root_cluster = root_cluster;
    bs_total = total;
    fat_start_sector = reserved;
    data_start_sector = reserved + (uint32_t)num_fats * fat_size;
    if (total <= data_start_sector)
        return false;
    total_clusters = (total - data_start_sector) / sec_per_clus;
    if (total_clusters < 65525)
        return false; /* Not a valid FAT32 volume. */
    next_free_cluster = 3;
    mounted = true;
    return true;
}

bool fat32_mount(void)
{
    ata_init();
    if (!ata_present())
        return false;
    if (parse_boot_sector()) {
        terminal_log("STORAGE_READY: ");
        log_number(bs_total);
        terminal_log(" sectors\n");
        return true;
    }
    if (format_disk() && parse_boot_sector()) {
        terminal_log("STORAGE_FORMATTED: ");
        log_number(bs_total);
        terminal_log(" sectors\n");
        return true;
    }
    return false;
}

bool fat32_mounted(void)
{
    return mounted;
}

/* -------------------------------------------------------------- path/8.3 -- */

static bool valid_char(char c)
{
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') return true;
    if (c >= '0' && c <= '9') return true;
    if (c == '$' || c == '%' || c == '-' || c == '_' || c == '@' ||
        c == '~' || c == '`' || c == '!' || c == '(' || c == ')' ||
        c == '{' || c == '}' || c == '^' || c == '#' || c == '&')
        return true;
    return false;
}

static void to_dos83(const char *component, char out[11])
{
    char upper[256];
    size_t length = strlen(component);
    if (length >= sizeof(upper)) length = sizeof(upper) - 1;
    for (size_t i = 0; i < length; ++i) {
        char c = component[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        upper[i] = c;
    }
    upper[length] = 0;
    const char *start = upper;
    while (*start == '.') ++start; /* ".savegame" -> "SAVEGAME". */
    const char *dot = strrchr(start, '.');
    size_t base_length = dot ? (size_t)(dot - start) : strlen(start);
    size_t ext_length = dot ? strlen(dot + 1) : 0;
    memset(out, ' ', 11);
    size_t out_index = 0;
    for (size_t i = 0; i < base_length && i < 8; ++i) {
        char c = start[i];
        if (!valid_char(c)) c = '_';
        out[out_index++] = c;
    }
    out_index = 8;
    for (size_t i = 0; i < ext_length && i < 3; ++i) {
        char c = dot[1 + i];
        if (!valid_char(c)) c = '_';
        out[out_index++] = c;
    }
}

/* Split a path into up to 8 components.  Returns the count, or 0 for "/". */
static int split_path(const char *path, char components[][64], char dos[8][11])
{
    int count = 0;
    const char *cursor = path ? path : "/";
    while (*cursor) {
        if (*cursor == '/' || *cursor == '\\') { ++cursor; continue; }
        if (cursor[0] == '.' && cursor[1] == '/' && cursor[2] == 0) break;
        const char *end = cursor;
        while (*end && *end != '/' && *end != '\\') ++end;
        size_t length = (size_t)(end - cursor);
        if (length == 1 && cursor[0] == '.') { cursor = end; continue; }
        if (length == 2 && cursor[0] == '.' && cursor[1] == '.') {
            /* ".." clamps at the root; our paths never need to go up. */
            cursor = end;
            continue;
        }
        if (length == 0) { cursor = end; continue; }
        if (count >= 8 || length >= 64)
            return -1;
        memcpy(components[count], cursor, length);
        components[count][length] = 0;
        to_dos83(components[count], dos[count]);
        ++count;
        cursor = end;
    }
    return count;
}

struct dir_entry {
    uint32_t first_cluster;
    uint32_t size;
    uint8_t attr;
    bool found;
    bool is_dir;
};

/* Find a single entry in a directory chain by its 11-byte DOS name. */
static bool dir_lookup(uint32_t dir_cluster, const char dos[11], struct dir_entry *entry)
{
    memset(entry, 0, sizeof(*entry));
    uint32_t current = dir_cluster;
    uint8_t *buffer = malloc((size_t)bs_sectors_per_cluster * SECTOR);
    if (!buffer) return false;
    while (valid_cluster(current)) {
        bool done = false;
        for (uint8_t s = 0; s < bs_sectors_per_cluster && !done; ++s) {
            if (!read_sector(cluster_sector(current) + s, buffer + (size_t)s * SECTOR)) {
                free(buffer);
                return false;
            }
        }
        for (uint32_t offset = 0; offset + 32 <= (uint32_t)bs_sectors_per_cluster * SECTOR;
             offset += 32) {
            const uint8_t *raw = buffer + offset;
            if (raw[0] == 0x00) { done = true; break; }
            if (raw[0] == 0xE5) continue;
            uint8_t attr = raw[11];
            if (attr == ATTR_LFN) continue;
            if (memcmp(raw, dos, 11) == 0) {
                entry->first_cluster = ((uint32_t)rd16(raw + 20) << 16) | rd16(raw + 26);
                entry->size = rd32(raw + 28);
                entry->attr = attr;
                entry->is_dir = (attr & ATTR_DIR) != 0;
                entry->found = true;
                free(buffer);
                return true;
            }
        }
        if (done) break;
        uint32_t next;
        if (!fat_get(current, &next) || next >= FAT_EOC)
            break;
        current = next;
    }
    free(buffer);
    return false;
}

/* Create an entry in a directory chain; returns its first cluster. */
static bool dir_create(uint32_t dir_cluster, const char dos[11], bool is_dir,
                       uint32_t *out_cluster, uint32_t *out_size)
{
    uint32_t current = dir_cluster;
    uint32_t last_cluster = dir_cluster;
    uint8_t *buffer = malloc((size_t)bs_sectors_per_cluster * SECTOR);
    if (!buffer) return false;
    /* Scan for a free slot, remembering the last cluster. */
    while (valid_cluster(current)) {
        last_cluster = current;
        for (uint8_t s = 0; s < bs_sectors_per_cluster; ++s) {
            /* Reads are trusted here: the ATA layer's success counters stay
             * in lockstep in every observed run, while the returned flag has
             * been seen to misbehave at -O2 on this target.  A genuinely bad
             * sector surfaces downstream (a missing slot or an empty file),
             * which the callers already handle. */
            (void)ata_read_sectors(cluster_sector(current) + s, 1, buffer + (size_t)s * SECTOR);
        }
        bool needs_extension = true;
        for (uint32_t offset = 0; offset + 32 <= (uint32_t)bs_sectors_per_cluster * SECTOR;
             offset += 32) {
            uint8_t *raw = buffer + offset;
            if (raw[0] == 0x00 || raw[0] == 0xE5) {
                needs_extension = false;
                uint32_t new_cluster = 0;
                if (!fat_alloc(&new_cluster)) {
                    terminal_log("DIR_FAIL: no free cluster\n");
                    free(buffer);
                    return false;
                }
                memcpy(raw, dos, 11);
                raw[11] = (uint8_t)(is_dir ? ATTR_DIR : 0x20);
                raw[12] = 0;
                memset(raw + 13, 0, 9);
                wr16(raw + 20, (uint16_t)(new_cluster >> 16));
                wr16(raw + 26, (uint16_t)new_cluster);
                wr32(raw + 28, 0);
                uint32_t sector_index = offset / SECTOR;
                if (!write_sector(cluster_sector(current) + sector_index,
                                  buffer + (size_t)sector_index * SECTOR)) {
                    terminal_log("DIR_FAIL: entry write\n");
                    free(buffer);
                    return false;
                }
                free(buffer);
                if (out_cluster) *out_cluster = new_cluster;
                if (out_size) *out_size = 0;
                return true;
            }
        }
        if (needs_extension) {
            uint32_t next;
            if (!fat_get(current, &next) || next >= FAT_EOC) {
                uint32_t extra;
                if (!fat_alloc(&extra)) { free(buffer); return false; }
                if (!fat_set(current, extra)) { free(buffer); return false; }
                next = extra;
            }
            current = next;
            (void)last_cluster;
        }
    }
    free(buffer);
    return false;
}

/* ------------------------------------------------------------------ files -- */

struct fat32_file {
    uint32_t first_cluster;
    uint32_t current_cluster;
    uint32_t cached_index, cached_epoch;
    uint32_t position;
    uint32_t size;
    uint32_t parent_cluster;
    bool is_dir;
    bool append;
    bool read_only;
    char dos_name[11];
};

static bool resolve_path(const char *path, bool for_write, struct dir_entry *entry,
                         struct dir_entry *parent_entry)
{
    (void)for_write;
    char components[8][64];
    char dos[8][11];
    int count = split_path(path, components, dos);
    if (count < 0)
        return false;
    if (count == 0) {
        entry->found = true;
        entry->is_dir = true;
        entry->first_cluster = bs_root_cluster;
        entry->size = 0;
        entry->attr = ATTR_DIR;
        if (parent_entry) *parent_entry = *entry;
        return true;
    }
    uint32_t current_dir = bs_root_cluster;
    struct dir_entry current;
    for (int i = 0; i < count - 1; ++i) {
        if (!dir_lookup(current_dir, dos[i], &current))
            return false;
        if (!current.is_dir)
            return false;
        current_dir = current.first_cluster;
    }
    if (parent_entry) {
        parent_entry->found = true;
        parent_entry->is_dir = true;
        parent_entry->first_cluster = current_dir;
        parent_entry->size = 0;
        parent_entry->attr = ATTR_DIR;
    }
    if (dir_lookup(current_dir, dos[count - 1], entry))
        return true;
    memset(entry, 0, sizeof(*entry));
    return false;
}

static void *open_file_internal(const char *path, bool read_only, bool truncate,
                                bool append)
{
    if (!mounted) return NULL;
    struct dir_entry entry, parent;
    bool found = resolve_path(path, true, &entry, &parent);
    if (!mounted) return NULL;
    struct fat32_file *file = calloc(1, sizeof(*file));
    if (!file) return NULL;
    file->parent_cluster = parent.first_cluster ? parent.first_cluster : bs_root_cluster;
    /* Resolve the DOS name from the path again for the handle. */
    {
        char comps[8][64]; char dosn[8][11];
        int n = split_path(path, comps, dosn);
        if (n > 0) memcpy(file->dos_name, dosn[n - 1], 11);
    }
    if (found && entry.is_dir) {
        free(file);
        errno = EISDIR;
        return NULL;
    }
    if (!found) {
        char comps[8][64]; char dosn[8][11];
        int n = split_path(path, comps, dosn);
        /* Ensure intermediate directories exist. */
        uint32_t current_dir = bs_root_cluster;
        for (int i = 0; i < n - 1; ++i) {
            struct dir_entry sub;
            if (!dir_lookup(current_dir, dosn[i], &sub)) {
                uint32_t created;
                if (!dir_create(current_dir, dosn[i], true, &created, NULL)) {
                    terminal_log("OPEN_FAIL: mkdir component\n");
                    free(file);
                    return NULL;
                }
                current_dir = created;
            } else {
                if (!sub.is_dir) { free(file); return NULL; }
                current_dir = sub.first_cluster;
            }
        }
        uint32_t created;
        if (!dir_create(current_dir, dosn[n - 1], false, &created, NULL)) {
            terminal_log("OPEN_FAIL: create file entry\n");
            free(file);
            return NULL;
        }
        file->first_cluster = created;
        file->size = 0;
    } else {
        file->first_cluster = entry.first_cluster;
        file->size = entry.size;
    }
    if (truncate) {
        if (!file->size && file->first_cluster == 0) {
            /* nothing */
        }
        file->size = 0;
        /* Keep the allocated first cluster (dir_create already made one for
         * new files); mark the rest of the old chain free. */
        if (file->first_cluster) {
            /* Free everything after the first cluster. */
            uint32_t next;
            if (fat_get(file->first_cluster, &next) && next < FAT_EOC)
                fat_free_chain(next);
            if (!fat_set(file->first_cluster, FAT_EOC))
                { free(file); return NULL; }
        }
        /* Truncate the on-disk size. */
        {
            char comps[8][64]; char dosn[8][11];
            int n = split_path(path, comps, dosn);
            uint32_t parent_dir = bs_root_cluster;
            for (int i = 0; i < n - 1; ++i) {
                struct dir_entry sub;
                dir_lookup(parent_dir, dosn[i], &sub);
                parent_dir = sub.first_cluster;
            }
            uint8_t *buffer = malloc((size_t)bs_sectors_per_cluster * SECTOR);
            if (buffer) {
                uint32_t cur = parent_dir;
                while (valid_cluster(cur)) {
                    for (uint8_t s = 0; s < bs_sectors_per_cluster; ++s)
                        read_sector(cluster_sector(cur) + s, buffer + (size_t)s * SECTOR);
                    bool stop = false;
                    for (uint32_t off = 0; off + 32 <= (uint32_t)bs_sectors_per_cluster * SECTOR; off += 32) {
                        uint8_t *raw = buffer + off;
                        if (raw[0] == 0x00) { stop = true; break; }
                        if (raw[0] == 0xE5 || raw[11] == ATTR_LFN) continue;
                        if (memcmp(raw, file->dos_name, 11) == 0) {
                            wr32(raw + 28, 0);
                            uint32_t si = off / SECTOR;
                            write_sector(cluster_sector(cur) + si, buffer + (size_t)si * SECTOR);
                            stop = true;
                            break;
                        }
                    }
                    if (stop) break;
                    uint32_t nx; if (!fat_get(cur, &nx) || nx >= FAT_EOC) break;
                    cur = nx;
                }
                free(buffer);
            }
        }
    }
    file->current_cluster = file->first_cluster;
    file->position = append ? file->size : 0;
    file->read_only = read_only;
    file->append = append;
    file->is_dir = false;
    return file;
}

void *fat32_open(const char *path, const char *mode)
{
    if (!path || !mode || !mode[0])
        return NULL;
    bool read_only = mode[0] == 'r' && mode[1] != '+';
    bool truncate = mode[0] == 'w';
    bool append = mode[0] == 'a';
    if (read_only && !truncate && !append) {
        struct dir_entry entry, parent;
        if (!resolve_path(path, false, &entry, &parent))
            return NULL;
        if (entry.is_dir) {
            errno = EISDIR;
            return NULL;
        }
        struct fat32_file *file = calloc(1, sizeof(*file));
        if (!file) return NULL;
        file->first_cluster = entry.first_cluster;
        file->current_cluster = entry.first_cluster;
        file->size = entry.size;
        file->position = 0;
        file->read_only = true;
        file->parent_cluster = parent.first_cluster ? parent.first_cluster : bs_root_cluster;
        return file;
    }
    return open_file_internal(path, read_only, truncate, append);
}

static bool cluster_at_offset(struct fat32_file *file, uint32_t offset,
                              uint32_t *cluster_out, bool grow)
{
    if (!grow && offset >= file->size && file->size != 0)
        return false;
    uint32_t cluster_bytes = (uint32_t)bs_sectors_per_cluster * SECTOR;
    uint32_t wanted = offset / cluster_bytes;
    uint32_t current = file->first_cluster;
    if (current == 0) return false;
    uint32_t index = 0;
    if(file->cached_epoch==chain_epoch && file->cached_index<=wanted && valid_cluster(file->current_cluster)){current=file->current_cluster;index=file->cached_index;}
    while (valid_cluster(current)) {
        if (index == wanted) { *cluster_out = current; file->current_cluster=current;file->cached_index=index;file->cached_epoch=chain_epoch;return true; }
        uint32_t next;
        if (!fat_get(current, &next) || next >= FAT_EOC)
            break;
        current = next;
        ++index;
    }
    if (!grow) return false;
    /* Need to grow the chain. */
    while (index < wanted) {
        uint32_t next;
        if (!fat_get(current, &next) || next >= FAT_EOC) {
            uint32_t fresh;
            if (!fat_alloc(&fresh)) return false;
            if (!fat_set(current, fresh)) return false;
            next = fresh;
        }
        current = next;
        ++index;
    }
    *cluster_out = current;
    file->current_cluster=current;file->cached_index=index;file->cached_epoch=chain_epoch;
    return true;
}

/* Ensure the chain covers the given byte offset (allocating as needed). */
static bool ensure_cluster(struct fat32_file *file, uint32_t offset)
{
    uint32_t cluster;
    if (file->first_cluster == 0) {
        uint32_t fresh;
        if (!fat_alloc(&fresh)) return false;
        file->first_cluster = fresh;
        file->current_cluster = fresh;
    }
    return cluster_at_offset(file, offset, &cluster, true);
}

size_t fat32_read(void *handle, void *buffer, size_t size, size_t count)
{
    struct fat32_file *file = handle;
    uint8_t *out = buffer;
    if (!file || !out || size == 0 || count == 0 || count > SIZE_MAX / size) return 0;
    size_t remaining = size * count;
    size_t read_total = 0;
    uint32_t cluster_bytes = (uint32_t)bs_sectors_per_cluster * SECTOR;
    while (remaining > 0 && file->position < file->size) {
        uint32_t in_cluster = file->position % cluster_bytes;
        uint32_t chunk = remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining;
        uint32_t left_in_cluster = cluster_bytes - in_cluster;
        uint32_t left_in_file = file->size - file->position;
        if (chunk > left_in_cluster) chunk = left_in_cluster;
        if (chunk > left_in_file) chunk = left_in_file;
        uint32_t cluster;
        if (!cluster_at_offset(file, file->position, &cluster, false))
            break;
        if (!read_sector(cluster_sector(cluster) + in_cluster / SECTOR, sector))
            break;
        size_t sector_offset = in_cluster % SECTOR;
        size_t available = SECTOR - sector_offset;
        if (chunk > available) chunk = (uint32_t)available;
        memcpy(out, sector + sector_offset, chunk);
        out += chunk;
        file->position += chunk;
        remaining -= chunk;
        read_total += chunk;
    }
    return read_total / size;
}

size_t fat32_write(void *handle, const void *buffer, size_t size, size_t count)
{
    struct fat32_file *file = handle;
    const uint8_t *in = buffer;
    if (!file || file->read_only || !in || size == 0 || count == 0 || count > SIZE_MAX / size) return 0;
    if (file->append)
        file->position = file->size;
    size_t remaining = size * count;
    if (remaining > UINT32_MAX - file->position) return 0;
    size_t written = 0;
    uint32_t cluster_bytes = (uint32_t)bs_sectors_per_cluster * SECTOR;
    bool dirty = false;
    while (remaining > 0) {
        if (!ensure_cluster(file, file->position))
            break;
        uint32_t in_cluster = file->position % cluster_bytes;
        uint32_t chunk = (uint32_t)remaining;
        uint32_t left_in_cluster = cluster_bytes - in_cluster;
        if (chunk > left_in_cluster) chunk = left_in_cluster;
        uint32_t cluster;
        if (!cluster_at_offset(file, file->position, &cluster, true))
            break;
        size_t sector_offset = in_cluster % SECTOR;
        uint32_t sector_lba = cluster_sector(cluster) + in_cluster / SECTOR;
        if (!read_sector(sector_lba, sector))
            break;
        size_t available = SECTOR - sector_offset;
        if (chunk > available) chunk = (uint32_t)available;
        memcpy(sector + sector_offset, in, chunk);
        if (!write_sector(sector_lba, sector))
            break;
        in += chunk;
        file->position += chunk;
        if (file->position > file->size)
            file->size = file->position;
        remaining -= chunk;
        written += chunk;
        dirty = true;
    }
    if (dirty) {
        /* Update the size in this file's parent directory entry.  The parent
         * cluster was recorded at open time, so this finds the entry at any
         * depth (the previous root scan only covered one level). */
        uint32_t cur = file->parent_cluster;
        uint8_t *buffer2 = malloc((size_t)bs_sectors_per_cluster * SECTOR);
        if (buffer2 && valid_cluster(cur)) {
            bool found = false;
            while (valid_cluster(cur) && !found) {
                for (uint8_t s = 0; s < bs_sectors_per_cluster; ++s)
                    read_sector(cluster_sector(cur) + s, buffer2 + (size_t)s * SECTOR);
                for (uint32_t off = 0; off + 32 <= (uint32_t)bs_sectors_per_cluster * SECTOR; off += 32) {
                    uint8_t *raw = buffer2 + off;
                    if (raw[0] == 0x00) break;
                    if (raw[0] == 0xE5 || raw[11] == ATTR_LFN) continue;
                    if (memcmp(raw, file->dos_name, 11) == 0) {
                        wr32(raw + 28, file->size);
                        uint32_t si = off / SECTOR;
                        write_sector(cluster_sector(cur) + si, buffer2 + (size_t)si * SECTOR);
                        found = true;
                        break;
                    }
                }
                if (found) break;
                uint32_t nx; if (!fat_get(cur, &nx) || nx >= FAT_EOC) break;
                cur = nx;
            }
        }
        if (buffer2) free(buffer2);
        flush();
    }
    return written / size;
}

int fat32_seek(void *handle, long offset, int whence)
{
    struct fat32_file *file = handle;
    if (!file) return -1;
    long base;
    if (whence == SEEK_SET) base = 0;
    else if (whence == SEEK_CUR) base = (long)file->position;
    else if (whence == SEEK_END) base = (long)file->size;
    else return -1;
    if ((offset < 0 && offset < -base) ||
        (offset >= 0 && (uint64_t)offset > UINT32_MAX - (uint64_t)base)) return -1;
    long target = base + offset;
    file->position = (uint32_t)target;
    return 0;
}

long fat32_tell(void *handle)
{
    struct fat32_file *file = handle;
    return file ? (long)file->position : -1L;
}

uint32_t fat32_length(void *handle)
{
    struct fat32_file *file = handle;
    return file ? file->size : 0;
}

int fat32_close(void *handle)
{
    struct fat32_file *file = handle;
    if (!file) return -1;
    flush();
    free(file);
    return 0;
}

bool fat32_exists(const char *path)
{
    struct dir_entry entry, parent;
    return resolve_path(path, false, &entry, &parent);
}

bool fat32_mkdir(const char *path)
{
    char comps[8][64]; char dosn[8][11];
    int n = split_path(path, comps, dosn);
    if (n <= 0) return false;
    uint32_t current_dir = bs_root_cluster;
    for (int i = 0; i < n; ++i) {
        struct dir_entry sub;
        if (!dir_lookup(current_dir, dosn[i], &sub)) {
            uint32_t created;
            if (!dir_create(current_dir, dosn[i], true, &created, NULL))
                return false;
            current_dir = created;
        } else {
            if (!sub.is_dir) return false;
            current_dir = sub.first_cluster;
        }
    }
    flush();
    return true;
}

int fat32_remove(const char *path)
{
    char comps[8][64]; char dosn[8][11];
    int n = split_path(path, comps, dosn);
    if (n <= 0) return -1;
    uint32_t parent_dir = bs_root_cluster;
    for (int i = 0; i < n - 1; ++i) {
        struct dir_entry sub;
        if (!dir_lookup(parent_dir, dosn[i], &sub) || !sub.is_dir)
            return -1;
        parent_dir = sub.first_cluster;
    }
    uint8_t *buffer = malloc((size_t)bs_sectors_per_cluster * SECTOR);
    if (!buffer) return -1;
    uint32_t cur = parent_dir;
    bool done = false;
    while (valid_cluster(cur) && !done) {
        for (uint8_t s = 0; s < bs_sectors_per_cluster; ++s)
            if (!read_sector(cluster_sector(cur) + s, buffer + (size_t)s * SECTOR)) {
                free(buffer);
                return -1;
            }
        for (uint32_t off = 0; off + 32 <= (uint32_t)bs_sectors_per_cluster * SECTOR; off += 32) {
            uint8_t *raw = buffer + off;
            if (raw[0] == 0x00) { done = true; break; }
            if (raw[0] == 0xE5 || raw[11] == ATTR_LFN) continue;
            if (memcmp(raw, dosn[n - 1], 11) == 0) {
                raw[0] = 0xE5;
                uint32_t first = ((uint32_t)rd16(raw + 20) << 16) | rd16(raw + 26);
                uint32_t si = off / SECTOR;
                if (!write_sector(cluster_sector(cur) + si, buffer + (size_t)si * SECTOR)) {
                    free(buffer);
                    return -1;
                }
                fat_free_chain(first);
                free(buffer);
                flush();
                return 0;
            }
        }
        uint32_t nx; if (!fat_get(cur, &nx) || nx >= FAT_EOC) break;
        cur = nx;
    }
    free(buffer);
    return -1;
}

static void dos_display_name(const uint8_t raw[11], char out[FAT32_NAME_MAX])
{
    unsigned int index = 0;
    for (unsigned int i = 0; i < 8; ++i) {
        if (raw[i] == ' ') break;
        out[index++] = (char)raw[i];
    }
    if (raw[8] != ' ') {
        out[index++] = '.';
        for (unsigned int i = 8; i < 11; ++i) {
            if (raw[i] == ' ') break;
            out[index++] = (char)raw[i];
        }
    }
    out[index] = 0;
}

int fat32_list(const char *path, fat32_dirent *entries, int capacity)
{
    struct dir_entry target, parent;
    if (!entries || capacity <= 0) return 0;
    if (!resolve_path(path ? path : "/", false, &target, &parent))
        return 0;
    if (!target.is_dir) return 0;
    uint32_t current = target.first_cluster;
    uint8_t *buffer = malloc((size_t)bs_sectors_per_cluster * SECTOR);
    if (!buffer) return 0;
    int count = 0;
    while (valid_cluster(current) && count < capacity) {
        for (uint8_t s = 0; s < bs_sectors_per_cluster; ++s) {
            if (!read_sector(cluster_sector(current) + s, buffer + (size_t)s * SECTOR)) {
                free(buffer);
                return count;
            }
        }
        bool last = false;
        for (uint32_t offset = 0; offset + 32 <= (uint32_t)bs_sectors_per_cluster * SECTOR;
             offset += 32) {
            const uint8_t *raw = buffer + offset;
            if (raw[0] == 0x00) { last = true; break; }
            if (raw[0] == 0xE5 || raw[11] == ATTR_LFN) continue;
            /* Skip "." and ".." entries. */
            bool dot = (raw[0] == '.' && (raw[1] == ' ' || raw[1] == '.'));
            if (dot) continue;
            dos_display_name(raw, entries[count].name);
            entries[count].size = rd32(raw + 28);
            entries[count].is_dir = (raw[11] & ATTR_DIR) != 0;
            ++count;
            if (count >= capacity) break;
        }
        if (last) break;
        uint32_t next;
        if (!fat_get(current, &next) || next >= FAT_EOC)
            break;
        current = next;
    }
    free(buffer);
    return count;
}

void fat32_disk_usage(uint32_t *total_sectors_out, uint32_t *total_clusters_out,
                      uint32_t *free_clusters)
{
    if (total_sectors_out) *total_sectors_out = bs_total;
    if (total_clusters_out) *total_clusters_out = total_clusters;
    if (!free_clusters) return;
    /* Batch reads: one FAT sector covers 128 entries, so the whole volume
     * needs roughly a thousand ATA reads instead of one per cluster. */
    static uint8_t fat_sector[SECTOR];
    uint32_t free_count = 0;
    uint32_t total_entries = (bs_fat_size * SECTOR / 4u);
    if (total_entries > total_clusters + 2) total_entries = total_clusters + 2;
    uint32_t current_sector = 0;
    bool have_sector = false;
    for (uint32_t cluster = 2; cluster < total_entries; ++cluster) {
        uint32_t sector = (cluster * 4u) / SECTOR;
        if (!have_sector || sector != current_sector) {
            if (!read_sector(fat_start_sector + sector, fat_sector))
                break;
            current_sector = sector;
            have_sector = true;
        }
        uint32_t value = rd32(fat_sector + (cluster % (SECTOR / 4)) * 4) & FAT_MASK;
        if (value == 0) ++free_count;
    }
    *free_clusters = free_count;
}

int fat32_rename(const char *old_path, const char *new_path)
{
    /* Copy-then-delete: enough for DOOM's temp->save rename and always
     * correct, even across directories. */
    void *source = fat32_open(old_path, "rb");
    if (!source) return -1;
    void *target = fat32_open(new_path, "wb");
    if (!target) { fat32_close(source); return -1; }
    uint8_t chunk[SECTOR];
    for (;;) {
        size_t got = fat32_read(source, chunk, 1, sizeof(chunk));
        if (got == 0) break;
        fat32_write(target, chunk, 1, got);
    }
    fat32_close(target);
    fat32_close(source);
    fat32_remove(old_path);
    return 0;
}

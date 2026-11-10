#ifndef COAST_DISK_INVENTORY_H
#define COAST_DISK_INVENTORY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define DISK_LIMIT 64
/* INFO_LSBLK's bounded text snapshot, not raw disk reads or guessed host state. */
typedef struct {
    char name[32], fs[24], mount[160];
    uint64_t bytes;
    uint32_t sector;
    int parent;
    bool partition, readonly;
} DiskEntry;
typedef struct {
    DiskEntry entries[DISK_LIMIT];
    int count;
    bool incomplete;
} DiskInventory;
static inline bool disk_equal(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}
static inline bool disk_number(const char *s, uint64_t *out) {
    uint64_t n = 0;
    if (!*s)
        return false;
    while (*s) {
        if (*s < '0' || *s > '9' || n > (UINT64_MAX - (unsigned)(*s - '0')) / 10)
            return false;
        n = n * 10 + (unsigned)(*s++ - '0');
    }
    *out = n;
    return true;
}
static inline bool disk_token(const char **cursor, const char *end, char *out, size_t cap) {
    const char *p = *cursor;
    while (p < end && (*p == ' ' || *p == '\t'))
        p++;
    size_t n = 0;
    bool fits = true;
    while (p < end && *p != ' ' && *p != '\t' && *p != '\r') {
        unsigned char c = *p++;
        if (c < 32 || c > 126)
            fits = false;
        if (n + 1 < cap)
            out[n++] = c;
        else
            fits = false;
    }
    out[n] = 0;
    *cursor = p;
    return n && fits;
}
static inline void disk_parse(DiskInventory *inventory, const char *text, size_t bytes) {
    *inventory = (DiskInventory){0};
    int parent = -1;
    const char *p = text, *end = text + bytes;
    while (p < end) {
        const char *line = p;
        while (p < end && *p != '\n')
            p++;
        if (p == end) {
            inventory->incomplete = true;
            break;
        }
        const char *finish = p++;
        if (finish == line)
            continue;
        const char *q = line;
        char fields[7][160];
        bool valid = true;
        for (int i = 0; i < 7; i++)
            if (!disk_token(&q, finish, fields[i], sizeof(fields[i])))
                valid = false;
        while (q < finish && (*q == ' ' || *q == '\t' || *q == '\r'))
            q++;
        if (q != finish)
            valid = false;
        if (disk_equal(fields[0], "NAME"))
            continue;
        DiskEntry entry = {.parent = -1};
        uint64_t sector = 0, ro = 0;
        bool part = disk_equal(fields[1], "part");
        if (!valid || (!part && !disk_equal(fields[1], "disk")) ||
            !disk_number(fields[2], &entry.bytes) || !disk_number(fields[3], &sector) ||
            !disk_number(fields[4], &ro) || sector == 0 || sector > UINT32_MAX || ro > 1 ||
            entry.bytes % sector || inventory->count == DISK_LIMIT) {
            inventory->incomplete = true;
            parent = -1;
            continue;
        }
        size_t sizes[] = {sizeof(entry.name), sizeof(entry.fs), sizeof(entry.mount)};
        char *dest[] = {entry.name, entry.fs, entry.mount};
        int source[] = {0, 5, 6};
        for (int i = 0; i < 3; i++) {
            size_t n = 0;
            while (fields[source[i]][n])
                n++;
            if (n >= sizes[i]) {
                valid = false;
                break;
            }
            for (size_t j = 0; j <= n; j++)
                dest[i][j] = fields[source[i]][j];
        }
        for (int i = 0; i < inventory->count; i++)
            if (disk_equal(inventory->entries[i].name, entry.name))
                valid = false;
        if (!valid) {
            inventory->incomplete = true;
            parent = -1;
            continue;
        }
        entry.sector = sector;
        entry.readonly = ro;
        entry.partition = part;
        if (part && line[0] == ' ')
            entry.parent = parent;
        if (!part)
            parent = inventory->count;
        inventory->entries[inventory->count++] = entry;
    }
}
#endif

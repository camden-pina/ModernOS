#include <fs/vfs.h>
#include <fs/file_time.h>
#include <drivers/block.h>
#include <mm/pmm.h>
#include <string.h>

struct fat_fs {
    uint32_t sector_size, cluster_sectors, reserved, fat_sectors, fats, root, clusters, next_free;
    uint32_t active_fat, fsinfo;
    uint64_t data_sector, total_sectors;
    bool mirrored, cache_valid, cache_dirty, fsinfo_invalidated;
    uint64_t cache_sector;
    uint8_t fat_cache[4096];
};

static bool valid_cluster(struct fat_fs *f, uint32_t c) {
    return c >= 2 && c < f->clusters + 2;
}

static uint64_t cluster_sector(struct fat_fs *f, uint32_t c) {
    return f->data_sector + (uint64_t)(c - 2) * f->cluster_sectors;
}

static int flush_fat(struct mount *m) {
    struct fat_fs *f = m->data;
    if (!f->cache_valid || !f->cache_dirty)
        return 0;

    uint64_t relative = f->cache_sector - f->reserved - (uint64_t)f->active_fat * f->fat_sectors;
    unsigned first = f->mirrored ? 0 : f->active_fat, last = f->mirrored ? f->fats : first + 1;
    for (unsigned i = first; i < last; i++) {
        int e = block_write(m->device, f->reserved + (uint64_t)i * f->fat_sectors + relative, 1,
                            f->fat_cache);
        if (e) {
            m->readonly = true;
            return e;
        }
    }

    f->cache_dirty = false;
    return 0;
}

static int cache_fat(struct mount *m, uint64_t sector) {
    struct fat_fs *f = m->data;
    if (!f->cache_valid || f->cache_sector != sector) {
        int e = flush_fat(m);
        if (e)
            return e;
        e = block_read(m->device, sector, 1, f->fat_cache);
        if (e) {
            f->cache_valid = false;
            return e;
        }
        f->cache_sector = sector;
        f->cache_valid = true;
    }
    return 0;
}

static int fat_get(struct mount *m, uint32_t c, uint32_t *out) {
    struct fat_fs *f = m->data;
    if (!valid_cluster(f, c))
        return -E_IO;

    uint64_t off = (uint64_t)c * 4;
    int e =
        cache_fat(m, f->reserved + (uint64_t)f->active_fat * f->fat_sectors + off / f->sector_size);
    if (e)
        return e;

    *out = get_le32(f->fat_cache + off % f->sector_size) & 0x0fffffff;
    return 0;
}

static int fat_set(struct mount *m, uint32_t c, uint32_t value) {
    struct fat_fs *f = m->data;
    if (!valid_cluster(f, c))
        return -E_IO;

    uint64_t off = (uint64_t)c * 4;
    int e =
        cache_fat(m, f->reserved + (uint64_t)f->active_fat * f->fat_sectors + off / f->sector_size);
    if (e)
        return e;

    uint8_t b[4096];
    uint8_t *entry = f->fat_cache + off % f->sector_size;
    put_le32(entry, (get_le32(entry) & 0xf0000000) | (value & 0x0fffffff));
    f->cache_dirty = true;

    /* Invalidate optional counters once. FAT entries remain allocation truth. */
    if (!f->fsinfo_invalidated && f->fsinfo && f->fsinfo < f->reserved) {
        e = block_read(m->device, f->fsinfo, 1, b);
        if (e)
            return e;
        if (get_le32(b) == 0x41615252 && get_le32(b + 484) == 0x61417272 &&
            get_le32(b + 508) == 0xaa550000) {
            put_le32(b + 488, 0xffffffff);
            put_le32(b + 492, 0xffffffff);
            e = block_write(m->device, f->fsinfo, 1, b);
            if (e)
                return e;
        }
        f->fsinfo_invalidated = true;
    }
    return 0;
}

static int next_cluster(struct mount *m, uint32_t c, uint32_t *next) {
    int e = fat_get(m, c, next);
    if (e)
        return e;
    if (*next >= 0x0ffffff8) {
        *next = 0;
        return 0;
    }
    return valid_cluster(m->data, *next) ? 0 : -E_IO;
}

static int allocate_cluster(struct mount *m, uint32_t *out) {
    struct fat_fs *f = m->data;
    uint32_t c = valid_cluster(f, f->next_free) ? f->next_free : 2;
    for (uint32_t scanned = 0; scanned < f->clusters; scanned++) {
        uint32_t value;
        int e = fat_get(m, c, &value);
        if (e)
            return e;
        if (!value) {
            /* Zero before linking; a failed write never exposes stale data. */
            uint8_t zero[4096] = {0};
            for (unsigned s = 0; s < f->cluster_sectors; s++) {
                e = block_write(m->device, cluster_sector(f, c) + s, 1, zero);
                if (e)
                    return e;
            }

            f->next_free = c + 1;
            e = fat_set(m, c, 0x0fffffff);
            if (e)
                return e;
            *out = c;
            return 0;
        }

        if (++c == f->clusters + 2)
            c = 2;
    }

    return -E_NOSPC;
}

static int free_chain(struct mount *m, uint32_t c) {
    struct fat_fs *f = m->data;
    for (uint32_t visited = 0; c && visited < f->clusters; visited++) {
        uint32_t next;
        int e = next_cluster(m, c, &next);
        if (e)
            return e;
        e = fat_set(m, c, 0);
        if (e)
            return e;
        c = next;
    }
    return c ? -E_IO : 0;
}

static int chain_at(struct mount *m, uint32_t start, uint64_t index, bool extend, uint32_t *out) {
    struct fat_fs *f = m->data;
    if (!valid_cluster(f, start) || index >= f->clusters)
        return -E_IO;

    uint32_t c = start;
    while (index--) {
        uint32_t next;
        int e = next_cluster(m, c, &next);
        if (e)
            return e;
        if (!next) {
            if (!extend)
                return -E_NOENT;
            e = allocate_cluster(m, &next);
            if (e)
                return e;
            e = fat_set(m, c, next);
            if (e) {
                free_chain(m, next);
                return e;
            }
        }
        if (next == c)
            return -E_IO;
        c = next;
    }
    *out = c;
    return 0;
}

static int file_cluster(struct vnode *v, uint64_t index, bool extend, uint32_t *out) {
    uint32_t start = v->aux[0];
    uint64_t steps = index;
    if (v->cache[1] && index >= v->cache[0]) {
        start = v->cache[1];
        steps -= v->cache[0];
    }
    int e = chain_at(v->mount, start, steps, extend, out);
    if (!e) {
        v->cache[0] = index;
        v->cache[1] = *out;
    }
    return e;
}

static uint32_t entry_cluster(const uint8_t *e) {
    return get_le16(e + 26) | ((uint32_t)(get_le16(e + 20) & 0x0fff) << 16);
}

static void set_entry_cluster(uint8_t *e, uint32_t c) {
    put_le16(e + 26, c);
    put_le16(e + 20, c >> 16);
}

static int dir_record(struct mount *m, uint32_t dir, uint64_t offset, uint8_t record[32],
                      bool write, bool extend, uint64_t *where) {
    struct fat_fs *f = m->data;
    uint64_t cluster_bytes = (uint64_t)f->sector_size * f->cluster_sectors;
    uint32_t c;
    int e = chain_at(m, dir, offset / cluster_bytes, extend, &c);
    if (e)
        return e;
    uint64_t sector = cluster_sector(f, c) + (offset % cluster_bytes) / f->sector_size;
    uint8_t b[4096];
    e = block_read(m->device, sector, 1, b);
    if (e)
        return e;
    if (write) {
        memcpy(b + offset % f->sector_size, record, 32);
        e = block_write(m->device, sector, 1, b);
    } else
        memcpy(record, b + offset % f->sector_size, 32);
    if (where)
        *where = sector * f->sector_size + offset % f->sector_size;
    return e;
}

static uint8_t short_checksum(const uint8_t *s) {
    uint8_t sum = 0;
    for (unsigned i = 0; i < 11; i++)
        sum = ((sum & 1) ? 128 : 0) + (sum >> 1) + s[i];
    return sum;
}

static int short_name(const uint8_t *s, char *out) {
    unsigned n = 0;
    for (unsigned i = 0; i < 8 && s[i] != ' '; i++) {
        unsigned char c = s[i];
        if (!i && c == 5)
            c = 0xe5;
        if ((s[12] & 8) && c >= 'A' && c <= 'Z')
            c += 32;
        out[n++] = c;
    }
    if (s[8] != ' ') {
        out[n++] = '.';
        for (unsigned i = 8; i < 11 && s[i] != ' '; i++) {
            char c = s[i];
            if ((s[12] & 16) && c >= 'A' && c <= 'Z')
                c += 32;
            out[n++] = c;
        }
    }
    out[n] = 0;
    return n;
}

static bool names_equal(const char *a, const char *b) {
    while (*a && *b) {
        unsigned char x = *a++, y = *b++;
        if (x >= 'a' && x <= 'z')
            x -= 32;
        if (y >= 'a' && y <= 'z')
            y -= 32;
        if (x != y)
            return false;
    }
    return *a == *b;
}

static void node_times(struct vnode *v, const uint8_t *r) {
    v->atime = fs_time_from_fat(get_le16(r + 18), 0);
    v->mtime = fs_time_from_fat(get_le16(r + 24), get_le16(r + 22));
    /* FAT stores creation time, not Unix inode-change time. */
    v->ctime = 0;
}

static void creation_times(uint8_t *r) {
    uint16_t date, time;
    if (fs_time_to_fat(fs_time_now(), &date, &time)) {
        put_le16(r + 14, time);
        put_le16(r + 16, date);
        put_le16(r + 18, date);
        put_le16(r + 22, time);
        put_le16(r + 24, date);
    }
}

static void make_node(struct mount *m, const uint8_t *record, uint64_t where, uint32_t parent,
                      uint64_t offset, unsigned lfn_count, struct vnode *v) {
    uint32_t cluster = entry_cluster(record);
    *v = (struct vnode){.mount = m,
                        .id = where + 2,
                        .size = get_le32(record + 28),
                        .mode = ((record[11] & 16) ? S_IFDIR : S_IFREG) |
                                ((record[11] & 1) ? 0555 : 0777),
                        .aux = {cluster, parent, offset, lfn_count}};
    node_times(v, record);
}

static int scan(struct vnode *dir, uint64_t *offset, struct directory_entry *out,
                struct vnode *node) {
    struct fat_fs *f = dir->mount->data;
    char longname[OS_NAME_MAX + 1] = {0};
    unsigned expected = 0, total = 0;
    uint8_t checksum = 0;
    for (uint64_t visited = 0;
         visited < (uint64_t)f->clusters * f->cluster_sectors * f->sector_size / 32; visited++) {
        uint8_t r[32];
        uint64_t where;
        int e = dir_record(dir->mount, dir->aux[0], *offset, r, false, false, &where);
        if (e == -E_NOENT)
            return 0;
        if (e)
            return e;
        uint64_t position = *offset;
        *offset += 32;
        if (!r[0])
            return 0;
        if (r[0] == 0xe5) {
            expected = total = 0;
            continue;
        }
        if (r[11] == 15) {
            unsigned seq = r[0] & 31;
            if (r[0] & 0x40) {
                memset(longname, 0, sizeof(longname));
                expected = total = seq;
                checksum = r[13];
            }
            if (!seq || seq != expected || seq > 20 || r[12] || get_le16(r + 26) ||
                r[13] != checksum) {
                expected = total = 0;
                continue;
            }
            static const uint8_t positions[] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
            for (unsigned i = 0; i < 13; i++) {
                unsigned index = (seq - 1) * 13 + i;
                uint16_t ch = get_le16(r + positions[i]);
                if (index > OS_NAME_MAX) {
                    if (ch && ch != 0xffff)
                        total = 0;
                    continue;
                }
                /* UTF-16 non-ASCII names are visible through their 8.3 alias. */
                if (ch > 127 && ch != 0xffff)
                    total = 0;
                longname[index] = (ch == 0xffff) ? 0 : (char)ch;
            }
            expected--;
            continue;
        }
        if (r[11] & 8) {
            total = expected = 0;
            continue;
        }
        bool valid = total && !expected && short_checksum(r) == checksum && longname[0];
        if (valid)
            strcpy(out->name, longname);
        else
            short_name(r, out->name);
        make_node(dir->mount, r, where, dir->aux[0], position, valid ? total : 0, node);
        out->id = node->id;
        out->mode = node->mode;
        return 1;
    }
    return -E_IO;
}

static int fat_root(struct mount *m, struct vnode *v) {
    struct fat_fs *f = m->data;
    *v = (struct vnode){.mount = m, .id = 1, .mode = S_IFDIR | 0777, .aux = {f->root, 0, 0, 0}};
    return 0;
}

static int fat_lookup(const struct vnode *dir, const char *name, struct vnode *out) {
    if ((dir->mode & 0170000) != S_IFDIR)
        return -E_NOTDIR;
    struct vnode d = *dir, node;
    struct directory_entry entry;
    uint64_t offset = 0;
    int e;
    while ((e = scan(&d, &offset, &entry, &node)) > 0)
        if (names_equal(name, entry.name)) {
            *out = node;
            return 0;
        }
    return e < 0 ? e : -E_NOENT;
}

static int update_node(struct vnode *v) {
    int flushed = flush_fat(v->mount);
    if (flushed)
        return flushed;
    struct fat_fs *f = v->mount->data;
    uint64_t where = v->id - 2;
    if (v->id == 1)
        return 0;
    uint8_t b[4096];
    int e = block_read(v->mount->device, where / f->sector_size, 1, b);
    if (e)
        return e;
    uint8_t *record = b + where % f->sector_size;
    if (!record[0] || record[0] == 0xe5 || record[11] == 15)
        return -E_NOENT;
    set_entry_cluster(record, v->aux[0]);
    put_le32(record + 28, v->size);
    uint16_t date, time;
    if (fs_time_to_fat(v->mtime, &date, &time)) {
        put_le16(record + 22, time);
        put_le16(record + 24, date);
    }
    return block_write(v->mount->device, where / f->sector_size, 1, b);
}

static int fat_stat(struct vnode *v) {
    if (v->id == 1)
        return 0;
    struct fat_fs *f = v->mount->data;
    uint8_t b[4096];
    uint64_t where = v->id - 2;
    int e = block_read(v->mount->device, where / f->sector_size, 1, b);
    if (e)
        return e;
    uint8_t *record = b + where % f->sector_size;
    if (!record[0] || record[0] == 0xe5)
        return -E_NOENT;
    if (v->aux[0] != entry_cluster(record))
        v->cache[0] = v->cache[1] = 0;
    v->aux[0] = entry_cluster(record);
    v->size = get_le32(record + 28);
    node_times(v, record);
    return 0;
}

static long fat_read(struct vnode *v, uint64_t off, void *data, size_t size) {
    struct fat_fs *f = v->mount->data;
    if ((v->mode & 0170000) != S_IFREG)
        return -E_ISDIR;
    if (off >= v->size)
        return 0;
    size = min((uint64_t)size, v->size - off);
    size_t done = 0;
    uint8_t sector[4096];
    uint64_t cluster_bytes = (uint64_t)f->sector_size * f->cluster_sectors;
    uint32_t c;
    int e = file_cluster(v, off / cluster_bytes, false, &c);
    if (e)
        return e;
    while (done < size) {
        unsigned skip = off % f->sector_size;
        size_t n = min(size - done, f->sector_size - skip);
        e = block_read(v->mount->device,
                       cluster_sector(f, c) + (off % cluster_bytes) / f->sector_size, 1, sector);
        if (e)
            return done ? (long)done : e;
        memcpy((uint8_t *)data + done, sector + skip, n);
        done += n;
        off += n;
        if (done < size && !(off % cluster_bytes)) {
            e = next_cluster(v->mount, c, &c);
            if (e || !c)
                return done ? (long)done : -E_IO;
            v->cache[0] = off / cluster_bytes;
            v->cache[1] = c;
        }
    }
    return done;
}

static long fat_write(struct vnode *v, uint64_t off, const void *data, size_t size) {
    if (!size)
        return 0;
    if ((v->mode & 0170000) != S_IFREG)
        return -E_ISDIR;
    if (!(v->mode & 0222))
        return -E_ACCES;
    if (off > UINT32_MAX || size > UINT32_MAX - off)
        return -E_FBIG;
    /* Fill sparse gaps explicitly: FAT has no sparse extent representation. */
    if (off > v->size) {
        uint8_t zero[4096] = {0};
        while (v->size < off) {
            size_t n = min((uint64_t)sizeof(zero), off - v->size);
            long e = fat_write(v, v->size, zero, n);
            if (e != (long)n)
                return e < 0 ? e : -E_NOSPC;
        }
    }
    struct fat_fs *f = v->mount->data;
    if (!v->aux[0]) {
        uint32_t c;
        int e = allocate_cluster(v->mount, &c);
        if (e)
            return e;
        v->aux[0] = c;
        e = update_node(v);
        if (e) {
            free_chain(v->mount, c);
            v->aux[0] = 0;
            return e;
        }
    }
    uint64_t cluster_bytes = (uint64_t)f->sector_size * f->cluster_sectors;
    uint32_t c;
    int e = file_cluster(v, off / cluster_bytes, true, &c);
    if (e)
        return e;
    size_t done = 0;
    uint8_t sector[4096];
    while (done < size) {
        unsigned skip = off % f->sector_size;
        size_t n = min(size - done, f->sector_size - skip);
        uint64_t lba = cluster_sector(f, c) + (off % cluster_bytes) / f->sector_size;
        if (skip || n < f->sector_size) {
            e = block_read(v->mount->device, lba, 1, sector);
            if (e)
                break;
        }
        memcpy(sector + skip, (const uint8_t *)data + done, n);
        e = block_write(v->mount->device, lba, 1, sector);
        if (e)
            break;
        done += n;
        off += n;
        if (done < size && !(off % cluster_bytes)) {
            uint32_t next;
            e = next_cluster(v->mount, c, &next);
            if (e)
                break;
            if (!next) {
                e = allocate_cluster(v->mount, &next);
                if (e)
                    break;
                e = fat_set(v->mount, c, next);
                if (e) {
                    free_chain(v->mount, next);
                    break;
                }
            }
            c = next;
            v->cache[0] = off / cluster_bytes;
            v->cache[1] = c;
        }
    }
    if (done) {
        v->size = max(v->size, off);
        v->mtime = fs_time_now();
        int metadata = update_node(v);
        if (metadata)
            return metadata;
    }
    return done ? (long)done : e;
}

static int fat_truncate(struct vnode *v) {
    uint32_t old = v->aux[0];
    v->aux[0] = 0;
    v->size = 0;
    v->cache[0] = v->cache[1] = 0;
    v->mtime = fs_time_now();
    int e = update_node(v);
    if (e) {
        fat_stat(v);
        return e;
    }
    e = old ? free_chain(v->mount, old) : 0;
    return e ? e : flush_fat(v->mount);
}

static int fat_resize(struct vnode *v, uint64_t length) {
    if (length > UINT32_MAX)
        return -E_FBIG;
    if (!(v->mode & 0222))
        return -E_ACCES;
    int e = fat_stat(v);
    if (e)
        return e;
    if (length == v->size)
        return 0;
    if (!length)
        return fat_truncate(v);
    if (length > v->size) {
        const uint8_t zero = 0;
        long n = fat_write(v, length - 1, &zero, 1);
        return n == 1 ? 0 : n < 0 ? (int)n : -E_IO;
    }
    struct fat_fs *f = v->mount->data;
    uint64_t cluster_bytes = (uint64_t)f->sector_size * f->cluster_sectors;
    uint32_t last, next;
    e = file_cluster(v, (length - 1) / cluster_bytes, false, &last);
    if (e)
        return e;
    e = next_cluster(v->mount, last, &next);
    if (e)
        return e;
    /* Publish the shorter length before freeing its now-unreachable tail. */
    v->size = length;
    v->mtime = fs_time_now();
    e = update_node(v);
    if (e) {
        fat_stat(v);
        return e;
    }
    v->cache[0] = v->cache[1] = 0;
    if (next) {
        e = fat_set(v->mount, last, 0x0fffffff);
        if (e)
            return e;
        e = flush_fat(v->mount);
        if (e)
            return e;
        e = free_chain(v->mount, next);
        if (e)
            return e;
    }
    return flush_fat(v->mount);
}

static int fat_set_times(struct vnode *v, int64_t atime, int64_t mtime, unsigned changed) {
    if (v->id == 1)
        return -E_NOTSUP; /* The root has no directory timestamp record. */
    uint16_t ad = 0, at = 0, md = 0, mt = 0;
    if (((changed & 1) && !fs_time_to_fat(atime, &ad, &at)) ||
        ((changed & 2) && !fs_time_to_fat(mtime, &md, &mt)))
        return -E_INVAL;
    struct fat_fs *f = v->mount->data;
    uint64_t where = v->id - 2;
    uint8_t b[4096];
    int e = block_read(v->mount->device, where / f->sector_size, 1, b);
    if (e)
        return e;
    uint8_t *r = b + where % f->sector_size;
    if (!r[0] || r[0] == 0xe5 || r[11] == 15)
        return -E_NOENT;
    if (changed & 1)
        put_le16(r + 18, ad);
    if (changed & 2) {
        put_le16(r + 22, mt);
        put_le16(r + 24, md);
    }
    e = block_write(v->mount->device, where / f->sector_size, 1, b);
    if (!e)
        node_times(v, r);
    return e;
}

static bool legal_name(const char *name) {
    size_t n = strlen(name);
    if (!n || n > OS_NAME_MAX || name[n - 1] == ' ' || name[n - 1] == '.' || !strcmp(name, ".") ||
        !strcmp(name, ".."))
        return false;
    for (size_t i = 0; i < n; i++) {
        unsigned c = (unsigned char)name[i];
        if (c < 32 || c > 126 || c == '"' || c == '*' || c == '/' || c == ':' || c == '<' ||
            c == '>' || c == '?' || c == '\\' || c == '|')
            return false;
    }
    return true;
}

static bool short_exists(const struct vnode *dir, const uint8_t name[11]) {
    struct fat_fs *f = dir->mount->data;
    for (uint64_t off = 0; off < (uint64_t)f->clusters * f->cluster_sectors * f->sector_size;
         off += 32) {
        uint8_t r[32];
        int e = dir_record(dir->mount, dir->aux[0], off, r, false, false, NULL);
        if (e || !r[0])
            return false;
        if (r[0] != 0xe5 && r[11] != 15 && !memcmp(r, name, 11))
            return true;
    }
    return true;
}

static int create_record(const struct vnode *dir, const char *name, uint32_t cluster, uint64_t size,
                         uint8_t attr, struct vnode *out) {
    if (!legal_name(name))
        return -E_INVAL;

    struct vnode existing;
    int e = fat_lookup(dir, name, &existing);
    if (!e)
        return -E_EXIST;
    if (e != -E_NOENT)
        return e;

    uint8_t alias[11];
    bool chosen = false;
    size_t len = strlen(name), dot = len;
    for (size_t i = 0; i < len; i++)
        if (name[i] == '.')
            dot = i;

    for (unsigned sequence = 1; sequence < 1000000; sequence++) {
        memset(alias, ' ', 11);
        char suffix[8];
        snprintf(suffix, sizeof(suffix), "~%u", sequence);
        unsigned base = 8 - strlen(suffix), n = 0;
        for (size_t i = 0; i < dot && n < base; i++) {
            char c = name[i];
            if (c == ' ' || c == '.')
                continue;
            if (c >= 'a' && c <= 'z')
                c -= 32;
            alias[n++] =
                (c == '+' || c == ',' || c == ';' || c == '=' || c == '[' || c == ']') ? '_' : c;
        }
        if (!n)
            alias[n++] = '_';
        memcpy(alias + n, suffix, strlen(suffix));
        if (dot < len)
            for (unsigned i = 0; i < 3 && dot + 1 + i < len; i++) {
                char c = name[dot + 1 + i];
                if (c >= 'a' && c <= 'z')
                    c -= 32;
                alias[8 + i] = c;
            }
        if (!short_exists(dir, alias)) {
            chosen = true;
            break;
        }
    }
    if (!chosen)
        return -E_NOSPC;

    unsigned lfn = (len + 12) / 13, needed = lfn + 1, run = 0;
    uint64_t start = 0, off = 0;
    bool end = false;
    struct fat_fs *f = dir->mount->data;
    for (; off < (uint64_t)f->clusters * f->cluster_sectors * f->sector_size; off += 32) {
        uint8_t r[32] = {0};
        e = dir_record(dir->mount, dir->aux[0], off, r, false, true, NULL);
        if (e)
            return e;
        if (!r[0])
            end = true;
        if (end || r[0] == 0xe5) {
            if (!run)
                start = off;
            if (++run == needed)
                break;
        } else
            run = 0;
    }

    if (run != needed)
        return -E_NOSPC;
    if (end) {
        uint8_t zero[32] = {0};
        e = dir_record(dir->mount, dir->aux[0], start + needed * 32, zero, true, true, NULL);
        if (e)
            return e;
    }

    uint8_t sum = short_checksum(alias);
    static const uint8_t positions[] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    for (unsigned i = 0; i < lfn; i++) {
        unsigned seq = lfn - i;
        uint8_t r[32];
        memset(r, 0xff, sizeof(r));
        r[0] = seq | (!i ? 0x40 : 0);
        r[11] = 15;
        r[12] = 0;
        r[13] = sum;
        put_le16(r + 26, 0);
        for (unsigned j = 0; j < 13; j++) {
            size_t index = (seq - 1) * 13 + j;
            put_le16(r + positions[j],
                     index < len ? (unsigned char)name[index] : (index == len ? 0 : 0xffff));
        }
        e = dir_record(dir->mount, dir->aux[0], start + i * 32, r, true, false, NULL);
        if (e)
            return e;
    }

    uint8_t r[32] = {0};
    memcpy(r, alias, 11);
    r[11] = attr;
    creation_times(r);
    set_entry_cluster(r, cluster);
    put_le32(r + 28, size);

    e = flush_fat(dir->mount);
    if (e)
        return e;

    uint64_t where;
    e = dir_record(dir->mount, dir->aux[0], start + lfn * 32, r, true, false, &where);
    if (!e)
        make_node(dir->mount, r, where, dir->aux[0], start + lfn * 32, lfn, out);
    return e;
}

static int fat_create(const struct vnode *dir, const char *name, unsigned mode, struct vnode *out) {
    if ((dir->mode & 0170000) != S_IFDIR)
        return -E_NOTDIR;
    uint32_t cluster = 0;
    int e;
    if ((mode & 0170000) == S_IFDIR) {
        e = allocate_cluster(dir->mount, &cluster);
        if (e)
            return e;
        uint8_t r[32] = {0};
        memset(r, ' ', 11);
        r[0] = '.';
        r[11] = 16;
        set_entry_cluster(r, cluster);
        creation_times(r);
        e = dir_record(dir->mount, cluster, 0, r, true, false, NULL);
        if (!e) {
            r[1] = '.';
            struct fat_fs *f = dir->mount->data;
            set_entry_cluster(r, dir->aux[0] == f->root ? 0 : dir->aux[0]);
            e = dir_record(dir->mount, cluster, 32, r, true, false, NULL);
        }
        if (e) {
            free_chain(dir->mount, cluster);
            return e;
        }
    }
    e = create_record(dir, name, cluster, 0, cluster ? 16 : 32, out);
    if (e && cluster)
        free_chain(dir->mount, cluster);
    return e;
}

static int delete_record(const struct vnode *v) {
    for (unsigned i = 0; i <= v->aux[3]; i++) {
        uint64_t off = v->aux[2] - (uint64_t)i * 32;
        uint8_t r[32];
        int e = dir_record(v->mount, v->aux[1], off, r, false, false, NULL);
        if (e)
            return e;
        r[0] = 0xe5;
        e = dir_record(v->mount, v->aux[1], off, r, true, false, NULL);
        if (e)
            return e;
    }
    return 0;
}

static int fat_unlink(const struct vnode *dir, const char *name, bool directory) {
    struct vnode v;
    int e = fat_lookup(dir, name, &v);
    if (e)
        return e;
    if (((v.mode & 0170000) == S_IFDIR) != directory)
        return directory ? -E_NOTDIR : -E_ISDIR;
    if (!(v.mode & 0222))
        return -E_ACCES;
    if (directory) {
        uint64_t off = 0;
        struct directory_entry entry;
        struct vnode node;
        while ((e = scan(&v, &off, &entry, &node)) > 0)
            if (strcmp(entry.name, ".") && strcmp(entry.name, ".."))
                return -E_NOTEMPTY;
        if (e)
            return e;
    }
    e = delete_record(&v);
    if (e)
        return e;
    e = v.aux[0] ? free_chain(v.mount, v.aux[0]) : 0;
    return e ? e : flush_fat(v.mount);
}

static int fat_rename(const struct vnode *old, const char *oldname, const struct vnode *new,
                      const char *newname) {
    struct vnode source, dest;
    int e = fat_lookup(old, oldname, &source);
    if (e)
        return e;
    if (!(source.mode & 0222))
        return -E_ACCES;
    e = create_record(new, newname, source.aux[0], source.size,
                      (source.mode & 0170000) == S_IFDIR ? 16 : 32, &dest);
    if (e)
        return e;
    /* A rename preserves the actual on-disk timestamps, including creation. */
    uint8_t source_record[32], dest_record[32];
    e = dir_record(source.mount, source.aux[1], source.aux[2], source_record, false, false, NULL);
    if (!e)
        e = dir_record(dest.mount, dest.aux[1], dest.aux[2], dest_record, false, false, NULL);
    if (!e) {
        memcpy(dest_record + 13, source_record + 13, 7);
        memcpy(dest_record + 22, source_record + 22, 4);
        e = dir_record(dest.mount, dest.aux[1], dest.aux[2], dest_record, true, false, NULL);
    }
    if (e) {
        delete_record(&dest);
        return e;
    }
    if ((source.mode & 0170000) == S_IFDIR && old->aux[0] != new->aux[0]) {
        uint8_t dotdot[32];
        e = dir_record(source.mount, source.aux[0], 32, dotdot, false, false, NULL);
        if (!e) {
            struct fat_fs *f = source.mount->data;
            set_entry_cluster(dotdot, new->aux[0] == f->root ? 0 : new->aux[0]);
            e = dir_record(source.mount, source.aux[0], 32, dotdot, true, false, NULL);
        }
        if (e) {
            delete_record(&dest);
            return e;
        }
    }
    e = delete_record(&source);
    if (e) {
        delete_record(&dest);
        return e;
    }
    return 0;
}

static int fat_readdir(const struct vnode *v, uint64_t *offset, struct directory_entry *out) {
    if ((v->mode & 0170000) != S_IFDIR)
        return -E_NOTDIR;
    struct vnode d = *v, node;
    return scan(&d, offset, out, &node);
}

static int fat_chmod(struct vnode *v, unsigned mode) {
    struct fat_fs *f = v->mount->data;
    uint64_t where = v->id - 2;
    if (v->id == 1)
        return 0;
    uint8_t b[4096];
    int e = block_read(v->mount->device, where / f->sector_size, 1, b);
    if (e)
        return e;
    uint8_t *record = b + where % f->sector_size;
    if (mode & 0222)
        record[11] &= ~1u;
    else
        record[11] |= 1;
    e = block_write(v->mount->device, where / f->sector_size, 1, b);
    if (!e)
        v->mode = (v->mode & 0170000) | ((mode & 0222) ? 0777 : 0555);
    return e;
}

static int fat_sync(struct mount *m) {
    int e = flush_fat(m);
    return e ? e : block_flush(m->device);
}

static const struct fs_ops operations = {.root = fat_root,
                                         .lookup = fat_lookup,
                                         .read = fat_read,
                                         .write = fat_write,
                                         .truncate = fat_truncate,
                                         .create = fat_create,
                                         .unlink = fat_unlink,
                                         .rename = fat_rename,
                                         .readdir = fat_readdir,
                                         .stat = fat_stat,
                                         .chmod = fat_chmod,
                                         .sync = fat_sync,
                                         .resize = fat_resize,
                                         .set_times = fat_set_times};

int fat32_mount(struct mount *m) {
    uint8_t boot[4096];
    int e = block_read(m->device, 0, 1, boot);
    if (e)
        return e;
    if (get_le16(boot + 510) != 0xaa55 || get_le16(boot + 17) || get_le16(boot + 22))
        return -E_NOENT;

    unsigned sector = get_le16(boot + 11), clusters = boot[13], reserved = get_le16(boot + 14),
             fats = boot[16];
    uint64_t total = get_le16(boot + 19);
    if (!total)
        total = get_le32(boot + 32);
    uint32_t fat_sectors = get_le32(boot + 36), root = get_le32(boot + 44);
    uint64_t data = reserved + (uint64_t)fats * fat_sectors;
    if (sector != m->device->sector_size || !clusters || (clusters & (clusters - 1)) ||
        clusters > 128 || !reserved || !fats || fats > 2 || !fat_sectors || data >= total ||
        total > m->device->sectors || get_le16(boot + 42))
        return -E_NOENT;

    uint64_t count = (total - data) / clusters;
    if (count < 65525 || count >= 0x0ffffff5 || root < 2 || root >= count + 2 ||
        (uint64_t)fat_sectors * sector / 4 < count + 2)
        return -E_NOENT;

    struct fat_fs *f = kmallocz(sizeof(*f));
    if (!f)
        return -E_NOMEM;
    *f = (struct fat_fs){.sector_size = sector,
                         .cluster_sectors = clusters,
                         .reserved = reserved,
                         .fat_sectors = fat_sectors,
                         .fats = fats,
                         .root = root,
                         .clusters = count,
                         .next_free = 2,
                         .fsinfo = get_le16(boot + 48),
                         .mirrored = !(get_le16(boot + 40) & 0x80),
                         .active_fat = (get_le16(boot + 40) & 0x80) ? get_le16(boot + 40) & 15 : 0,
                         .data_sector = data,
                         .total_sectors = total};
    if (f->active_fat >= fats) {
        kfree(f);
        return -E_IO;
    }

    m->data = f;
    m->ops = &operations;
    strcpy(m->type, "fat32");

    uint32_t next;
    e = next_cluster(m, root, &next);
    if (e) {
        kfree(f);
        m->data = NULL;
        return e;
    }
    return 0;
}

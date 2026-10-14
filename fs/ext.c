/* Read-only ext2/ext4. Clean filesystems, legacy block maps and extent trees.
   Unsupported incompatible features and journals needing replay are refused. */
#include <fs/vfs.h>
#include <drivers/block.h>
#include <mm/pmm.h>
#include <string.h>

struct ext_fs {
    uint32_t block_size, inode_size, inodes_per_group, blocks_per_group, desc_size;
    uint32_t inode_count, incompat, rocompat, checksum_seed;
    uint64_t blocks, groups, descriptor_offset;
};

struct ext_inode {
    uint8_t bytes[4096];
    uint32_t seed;
};

static uint32_t crc32c(uint32_t crc, const void *data, size_t len) {
    const uint8_t *b = data;
    while (len--) {
        crc ^= *b++;
        for (unsigned i = 0; i < 8; i++)
            crc = (crc >> 1) ^ ((0 - (crc & 1)) & 0x82f63b78u);
    }
    return crc;
}

static int read_block(struct mount *m, uint64_t block, void *data) {
    struct ext_fs *f = m->data;
    if (!block || block >= f->blocks)
        return -E_IO;
    return block_read_bytes(m->device, block * f->block_size, data, f->block_size);
}

static int read_inode(struct mount *m, uint64_t number, struct ext_inode *inode) {
    struct ext_fs *f = m->data;
    if (!number || number > f->inode_count)
        return -E_IO;
    uint64_t group = (number - 1) / f->inodes_per_group, index = (number - 1) % f->inodes_per_group;
    if (group >= f->groups)
        return -E_IO;
    uint8_t descriptor[64];
    int e = block_read_bytes(m->device, f->descriptor_offset + group * f->desc_size, descriptor,
                             f->desc_size);
    if (e)
        return e;
    uint64_t table = get_le32(descriptor + 8);
    if (f->incompat & 0x80)
        table |= (uint64_t)get_le32(descriptor + 40) << 32;
    if (f->rocompat & 0x400) {
        uint16_t stored = get_le16(descriptor + 30);
        put_le16(descriptor + 30, 0);
        uint8_t g[4];
        put_le32(g, group);
        uint32_t c = crc32c(f->checksum_seed, g, 4);
        if ((uint16_t)crc32c(c, descriptor, f->desc_size) != stored)
            return -E_IO;
    }
    if (!table || table >= f->blocks ||
        index * f->inode_size > (f->blocks - table) * f->block_size - f->inode_size)
        return -E_IO;
    e = block_read_bytes(m->device, table * f->block_size + index * f->inode_size, inode->bytes,
                         f->inode_size);
    if (e)
        return e;
    uint8_t n[4];
    put_le32(n, number);
    inode->seed = crc32c(crc32c(f->checksum_seed, n, 4), inode->bytes + 100, 4);
    if (f->rocompat & 0x400) {
        uint32_t stored = get_le16(inode->bytes + 124);
        put_le16(inode->bytes + 124, 0);
        bool high = f->inode_size >= 132 && get_le16(inode->bytes + 128) >= 4;
        if (high) {
            stored |= (uint32_t)get_le16(inode->bytes + 130) << 16;
            put_le16(inode->bytes + 130, 0);
        }
        uint32_t calculated = crc32c(inode->seed, inode->bytes, f->inode_size);
        if (high ? calculated != stored : (uint16_t)calculated != stored)
            return -E_IO;
    }
    unsigned flags = get_le32(inode->bytes + 32);
    if (flags & (0x10000000u | 0x800u))
        return -E_NOTSUP; /* inline data, encryption */
    return 0;
}

static int inode_node(struct mount *m, uint64_t id, struct vnode *out) {
    struct ext_inode inode;
    int e = read_inode(m, id, &inode);
    if (e)
        return e;
    unsigned mode = get_le16(inode.bytes);
    uint64_t size = get_le32(inode.bytes + 4);
    if ((mode & S_IFREG) == S_IFREG || (mode & S_IFDIR) == S_IFDIR || (mode & S_IFLNK) == S_IFLNK)
        size |= (uint64_t)get_le32(inode.bytes + 108) << 32;
    *out = (struct vnode){.mount = m,
                          .id = id,
                          .size = size,
                          .mode = mode,
                          .atime = get_le32(inode.bytes + 8),
                          .ctime = get_le32(inode.bytes + 12),
                          .mtime = get_le32(inode.bytes + 16)};
    return 0;
}

static int ext_root(struct mount *m, struct vnode *out) {
    return inode_node(m, 2, out);
}

static int extent_block(struct mount *m, const struct ext_inode *inode, uint32_t logical,
                        uint64_t *physical) {
    struct ext_fs *f = m->data;
    uint8_t buffer[4096];
    const uint8_t *node = inode->bytes + 40;
    size_t bytes = 60;
    unsigned expect = UINT32_MAX;
    for (unsigned step = 0; step < 6; step++) {
        unsigned count = get_le16(node + 2), maximum = get_le16(node + 4),
                 depth = get_le16(node + 6);
        if (get_le16(node) != 0xf30a || depth > 5 || count > maximum ||
            maximum > (bytes - 12) / 12 || (expect != UINT32_MAX && expect != depth))
            return -E_IO;
        uint32_t previous = 0;
        for (unsigned i = 0; i < count; i++) {
            uint32_t start = get_le32(node + 12 + i * 12);
            if (i && start <= previous)
                return -E_IO;
            previous = start;
        }
        if (!depth) {
            *physical = 0;
            for (unsigned i = 0; i < count; i++) {
                const uint8_t *e = node + 12 + i * 12;
                uint32_t start = get_le32(e);
                unsigned raw = get_le16(e + 4);
                unsigned length = raw <= 32768 ? raw : raw - 32768;
                if (!length || (uint64_t)start + length > UINT32_MAX + 1ULL)
                    return -E_IO;
                if (logical >= start && logical - start < length) {
                    if (raw > 32768)
                        return 0; /* unwritten extent reads as zero */
                    uint64_t base = get_le32(e + 8) | ((uint64_t)get_le16(e + 6) << 32);
                    if (!base || base >= f->blocks || length > f->blocks - base)
                        return -E_IO;
                    *physical = base + logical - start;
                    return 0;
                }
            }
            return 0; /* sparse */
        }
        const uint8_t *chosen = NULL;
        for (unsigned i = 0; i < count; i++) {
            const uint8_t *e = node + 12 + i * 12;
            if (get_le32(e) > logical)
                break;
            chosen = e;
        }
        if (!chosen) {
            *physical = 0;
            return 0;
        }
        uint64_t child = get_le32(chosen + 4) | ((uint64_t)get_le16(chosen + 8) << 32);
        int e = read_block(m, child, buffer);
        if (e)
            return e;
        if (f->rocompat & 0x400) {
            unsigned max_entries = get_le16(buffer + 4);
            size_t tail = 12 + (size_t)max_entries * 12;
            if (tail > f->block_size - 4 ||
                crc32c(inode->seed, buffer, tail) != get_le32(buffer + tail))
                return -E_IO;
        }
        node = buffer;
        bytes = f->block_size;
        expect = depth - 1;
    }
    return -E_IO;
}

static int inode_block(struct mount *m, const struct ext_inode *inode, uint64_t logical,
                       uint64_t *physical) {
    struct ext_fs *f = m->data;
    if (logical > UINT32_MAX)
        return -E_FBIG;
    if (get_le32(inode->bytes + 32) & 0x80000)
        return extent_block(m, inode, logical, physical);
    unsigned pointers = f->block_size / 4, depth;
    uint64_t block;
    if (logical < 12) {
        *physical = get_le32(inode->bytes + 40 + logical * 4);
        return 0;
    }
    logical -= 12;
    if (logical < pointers) {
        depth = 1;
        block = get_le32(inode->bytes + 88);
    } else {
        logical -= pointers;
        if (logical < (uint64_t)pointers * pointers) {
            depth = 2;
            block = get_le32(inode->bytes + 92);
        } else {
            logical -= (uint64_t)pointers * pointers;
            if (logical >= (uint64_t)pointers * pointers * pointers)
                return -E_FBIG;
            depth = 3;
            block = get_le32(inode->bytes + 96);
        }
    }
    uint8_t buffer[4096];
    while (depth) {
        if (!block) {
            *physical = 0;
            return 0;
        }
        int e = read_block(m, block, buffer);
        if (e)
            return e;
        uint64_t stride = 1;
        for (unsigned i = 1; i < depth; i++)
            stride *= pointers;
        block = get_le32(buffer + (logical / stride) * 4);
        logical %= stride;
        depth--;
    }
    if (block >= f->blocks)
        return -E_IO;
    *physical = block;
    return 0;
}

static long ext_read(struct vnode *v, uint64_t off, void *data, size_t size) {
    struct ext_fs *f = v->mount->data;
    if (off >= v->size)
        return 0;
    size = min((uint64_t)size, v->size - off);
    struct ext_inode inode;
    int e = read_inode(v->mount, v->id, &inode);
    if (e)
        return e;
    if ((v->mode & 0170000) == S_IFLNK && v->size <= 60 && !get_le32(inode.bytes + 28)) {
        memcpy(data, inode.bytes + 40 + off, size);
        return size;
    }
    size_t done = 0;
    uint8_t buffer[4096];
    while (done < size) {
        uint64_t block;
        e = inode_block(v->mount, &inode, off / f->block_size, &block);
        if (e)
            return e;
        size_t n = min(size - done, f->block_size - off % f->block_size);
        if (block) {
            e = read_block(v->mount, block, buffer);
            if (e)
                return e;
        } else
            memset(buffer, 0, f->block_size);
        memcpy((uint8_t *)data + done, buffer + off % f->block_size, n);
        done += n;
        off += n;
    }
    return done;
}

static int ext_readdir(const struct vnode *v, uint64_t *offset, struct directory_entry *out) {
    if ((v->mode & 0170000) != S_IFDIR)
        return -E_NOTDIR;
    struct ext_fs *f = v->mount->data;
    while (*offset < v->size) {
        uint8_t header[8];
        struct vnode dir = *v;
        long n = ext_read(&dir, *offset, header, 8);
        if (n != 8)
            return n < 0 ? n : -E_IO;
        uint32_t id = get_le32(header);
        unsigned len = get_le16(header + 4);
        unsigned namesize = (f->incompat & 2) ? header[6] : get_le16(header + 6);
        if (len < 8 || (len & 3) || len > f->block_size - *offset % f->block_size ||
            len > v->size - *offset || namesize > OS_NAME_MAX || namesize > len - 8)
            return -E_IO;
        uint64_t position = *offset;
        *offset += len;
        if (!id)
            continue; /* includes htree interior blocks and csum tails */
        n = ext_read(&dir, position + 8, out->name, namesize);
        if (n != (long)namesize)
            return n < 0 ? n : -E_IO;
        out->name[namesize] = 0;
        struct vnode child;
        int e = inode_node(v->mount, id, &child);
        if (e)
            return e;
        out->id = id;
        out->mode = child.mode;
        return 1;
    }
    return 0;
}

static int ext_lookup(const struct vnode *v, const char *name, struct vnode *out) {
    uint64_t off = 0;
    struct directory_entry entry;
    int e;
    while ((e = ext_readdir(v, &off, &entry)) > 0)
        if (!strcmp(entry.name, name))
            return inode_node(v->mount, entry.id, out);
    return e < 0 ? e : -E_NOENT;
}

static int ext_stat(struct vnode *v) {
    return inode_node(v->mount, v->id, v);
}

static const struct fs_ops operations = {.root = ext_root,
                                         .lookup = ext_lookup,
                                         .read = ext_read,
                                         .readdir = ext_readdir,
                                         .stat = ext_stat};

int ext_mount(struct mount *m) {
    uint8_t sb[1024];
    if (block_read_bytes(m->device, 1024, sb, sizeof(sb)))
        return -E_IO;
    if (get_le16(sb + 56) != 0xef53)
        return -E_NOENT;

    unsigned log = get_le32(sb + 24), rev = get_le32(sb + 76);
    uint32_t incompat = rev ? get_le32(sb + 96) : 0, ro = rev ? get_le32(sb + 100) : 0;
    if (log > 2 || incompat & ~(0x2c2u | 0x2000u) || (ro & (0x200u | 0x10000u)) ||
        !(get_le16(sb + 58) & 1)) {
        pr_warn("%s: ext filesystem needs recovery or has unsupported features (%x)\n",
                m->device->name, incompat);
        return -E_NOTSUP;
    }
    if ((ro & 0x400) && (sb[0x175] != 1 || crc32c(~0u, sb, 1020) != get_le32(sb + 1020)))
        return -E_IO;

    struct ext_fs *f = kmallocz(sizeof(*f));
    if (!f)
        return -E_NOMEM;
    f->block_size = 1024u << log;
    f->inode_size = rev ? get_le16(sb + 88) : 128;
    f->inodes_per_group = get_le32(sb + 40);
    f->blocks_per_group = get_le32(sb + 32);
    f->inode_count = get_le32(sb);
    f->blocks = get_le32(sb + 4);
    f->incompat = incompat;
    f->rocompat = ro;
    if (incompat & 0x80)
        f->blocks |= (uint64_t)get_le32(sb + 336) << 32;
    f->desc_size = (incompat & 0x80) ? get_le16(sb + 254) : 32;
    f->checksum_seed = (incompat & 0x2000) ? get_le32(sb + 624) : crc32c(~0u, sb + 104, 16);

    uint64_t first = get_le32(sb + 20);
    if (f->inode_size < 128 || f->inode_size > f->block_size ||
        (f->inode_size & (f->inode_size - 1)) || !f->blocks_per_group || !f->inodes_per_group ||
        f->inodes_per_group > f->block_size * 8 || f->blocks <= first || f->desc_size < 32 ||
        f->desc_size > 64 ||
        f->blocks > m->device->sectors * m->device->sector_size / f->block_size) {
        kfree(f);
        return -E_IO;
    }

    f->groups = (f->blocks - first + f->blocks_per_group - 1) / f->blocks_per_group;
    f->descriptor_offset = (first + 1) * f->block_size;
    if (f->groups * f->desc_size > f->blocks * f->block_size - f->descriptor_offset) {
        kfree(f);
        return -E_IO;
    }

    m->data = f;
    m->ops = &operations;
    m->readonly = true;
    strcpy(m->type, "ext");

    struct vnode root;
    int e = ext_root(m, &root);
    if (e || (root.mode & 0170000) != S_IFDIR) {
        kfree(f);
        m->data = NULL;
        return e ? e : -E_IO;
    }
    return 0;
}

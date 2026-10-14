#include <fs/memfd.h>
#include <mm/pmm.h>
#include <mm/user.h>
#include <string.h>
#include <sched.h>

#define OBJECT_LIMIT (128ULL * 1024 * 1024)
#define TOTAL_PAGES 131072 /* Bounded 512 MiB backing store across all objects. */
#define BUCKETS 1024
#define SEAL_SEAL 1
#define SEAL_SHRINK 2
#define SEAL_GROW 4
#define SEAL_WRITE 8

struct memory_page {
    uint64_t physical;
    struct memory_object *object;
    struct memory_page *next;
};

struct memory_account {
    unsigned references, pages, limit;
};

struct memory_object {
    unsigned references, mappings, writable, seals, domain, renderer;
    uint64_t size, count;
    struct memory_page *pages;
    struct memory_account *account;
};

static spinlock_t memory_lock;
static struct memory_page *buckets[BUCKETS];
static unsigned allocated_pages;

static unsigned bucket(uint64_t physical) {
    return (physical / PAGE_SIZE) % BUCKETS;
}

static struct memory_page *page_at(uint64_t physical) {
    for (struct memory_page *p = buckets[bucket(physical)]; p; p = p->next)
        if (p->physical == physical)
            return p;
    return NULL;
}

struct memory_object *memfd_create(unsigned flags, unsigned domain) {
    struct task_context *context = this_cpu()->current->context;
    if (!context->memory_account) {
        context->memory_account = kmallocz(sizeof(*context->memory_account));
        if (!context->memory_account)
            return NULL;
        context->memory_account->references = 1;
        context->memory_account->limit = context->web_role == OS_WEB_RENDERER ? 32768 : TOTAL_PAGES;
    }

    struct memory_object *m = kmallocz(sizeof(*m));
    if (m) {
        m->references = 1;
        m->seals = (flags & 2) ? 0 : SEAL_SEAL;
        m->domain = domain;
        m->account = context->memory_account;
        uint64_t irq = spin_lock_irqsave(&memory_lock);
        m->account->references++;
        spin_unlock_irqrestore(&memory_lock, irq);
    }

    return m;
}

void memfd_account_release(struct memory_account *a) {
    if (!a)
        return;
    uint64_t irq = spin_lock_irqsave(&memory_lock);
    bool last = !--a->references;
    spin_unlock_irqrestore(&memory_lock, irq);
    if (last)
        kfree(a);
}

/* Allocation and resize run under vfs_mutex. PTE references use memory_lock;
   object destruction removes the registry before releasing physical storage. */
static void destroy(struct memory_object *m) {
    uint64_t irq = spin_lock_irqsave(&memory_lock);
    for (uint64_t i = 0; i < m->count; i++) {
        struct memory_page **p = &buckets[bucket(m->pages[i].physical)];
        while (*p && *p != &m->pages[i])
            p = &(*p)->next;
        if (*p)
            *p = (*p)->next;
    }
    allocated_pages -= m->count;
    m->account->pages -= m->count;
    spin_unlock_irqrestore(&memory_lock, irq);

    for (uint64_t i = 0; i < m->count; i++)
        pmm_free_pages(m->pages[i].physical, 1);

    memfd_account_release(m->account);
    kfree(m->pages);
    kfree(m);
}

void memfd_release(struct memory_object *m) {
    uint64_t irq = spin_lock_irqsave(&memory_lock);
    bool last = !--m->references;
    spin_unlock_irqrestore(&memory_lock, irq);
    if (last)
        destroy(m);
}

unsigned memfd_domain(struct memory_object *m) {
    return m->domain;
}

bool memfd_authorize(struct memory_object *m, unsigned domain, unsigned renderer) {
    uint64_t irq = spin_lock_irqsave(&memory_lock);
    bool ok = m->domain == domain && (!m->renderer || m->renderer == renderer);
    if (ok)
        m->renderer = renderer;
    spin_unlock_irqrestore(&memory_lock, irq);
    return ok;
}

uint64_t memfd_size(struct memory_object *m) {
    uint64_t irq = spin_lock_irqsave(&memory_lock), size = m->size;
    spin_unlock_irqrestore(&memory_lock, irq);
    return size;
}

long memfd_seals(struct memory_object *m, bool add, unsigned seals) {
    uint64_t irq = spin_lock_irqsave(&memory_lock);
    long result = m->seals;
    if (add) {
        if (seals & ~15u)
            result = -E_INVAL;
        else if (m->seals & SEAL_SEAL)
            result = -E_PERM;
        else if ((seals & SEAL_WRITE) && m->writable)
            result = -E_BUSY;
        else {
            m->seals |= seals;
            result = 0;
        }
    }
    spin_unlock_irqrestore(&memory_lock, irq);
    return result;
}

long memfd_resize(struct memory_object *m, uint64_t size) {
    if (size > OBJECT_LIMIT)
        return -E_FBIG;
    uint64_t count = align(size, PAGE_SIZE) / PAGE_SIZE;
    uint64_t irq = spin_lock_irqsave(&memory_lock);
    long error = 0;
    if ((size < m->size && (m->seals & SEAL_SHRINK)) || (size > m->size && (m->seals & SEAL_GROW)))
        error = -E_PERM;
    /* No cross-CPU invalidation/SIGBUS support for truncating mapped objects.
       Reject it honestly; never leave stale writable aliases to freed pages. */
    else if (size < m->size && m->mappings)
        error = -E_BUSY;
    else if (count > m->count && (count - m->count > TOTAL_PAGES - allocated_pages ||
                                  count - m->count > m->account->limit - m->account->pages))
        error = -E_NOMEM;
    if (!error && count > m->count) {
        allocated_pages += count - m->count;
        m->account->pages += count - m->count;
    }
    spin_unlock_irqrestore(&memory_lock, irq);
    if (error)
        return error;

    struct memory_page *next = count ? kmallocz(count * sizeof(*next)) : NULL;
    if (count && !next)
        error = -E_NOMEM;

    uint64_t done = min(count, m->count);
    if (!error)
        for (; done < count; done++) {
            next[done].physical = pmm_alloc_pages(1);
            if (!next[done].physical) {
                error = -E_NOMEM;
                break;
            }
            memset(phys_ptr(next[done].physical), 0, PAGE_SIZE);
        }

    if (error) {
        if (next)
            for (uint64_t i = m->count; i < done; i++)
                pmm_free_pages(next[i].physical, 1);
        kfree(next);
        irq = spin_lock_irqsave(&memory_lock);
        if (count > m->count) {
            allocated_pages -= count - m->count;
            m->account->pages -= count - m->count;
        }
        spin_unlock_irqrestore(&memory_lock, irq);
        return error;
    }

    irq = spin_lock_irqsave(&memory_lock);
    for (uint64_t i = 0; i < m->count; i++) {
        struct memory_page **p = &buckets[bucket(m->pages[i].physical)];
        while (*p && *p != &m->pages[i])
            p = &(*p)->next;
        if (*p)
            *p = (*p)->next;
        if (i < count)
            next[i].physical = m->pages[i].physical;
    }

    for (uint64_t i = 0; i < count; i++) {
        next[i].object = m;
        unsigned b = bucket(next[i].physical);
        next[i].next = buckets[b];
        buckets[b] = &next[i];
    }
    if (count < m->count) {
        allocated_pages -= m->count - count;
        m->account->pages -= m->count - count;
    }

    struct memory_page *previous = m->pages;
    uint64_t old_count = m->count;
    m->pages = next;
    m->count = count;
    m->size = size;

    /* Clear a truncated partial tail, so later growth cannot expose old data. */
    if (count && size % PAGE_SIZE && size < old_count * PAGE_SIZE)
        memset((uint8_t *)phys_ptr(next[count - 1].physical) + size % PAGE_SIZE, 0,
               PAGE_SIZE - size % PAGE_SIZE);
    spin_unlock_irqrestore(&memory_lock, irq);

    for (uint64_t i = count; i < old_count; i++)
        pmm_free_pages(previous[i].physical, 1);
    kfree(previous);
    return 0;
}

long memfd_transfer(struct file *f, void *data, size_t size, bool write) {
    if (!size)
        return 0;
    struct memory_object *m = f->memory;
    if (write && (int64_t)f->offset < 0)
        return -E_INVAL;
    if (write && (f->offset > OBJECT_LIMIT || size > OBJECT_LIMIT - f->offset))
        return -E_FBIG;
    if (write && (m->seals & SEAL_WRITE))
        return -E_PERM;
    if (write && f->offset + size > m->size) {
        long error = memfd_resize(m, f->offset + size);
        if (error)
            return error;
    }
    if (!write)
        size = f->offset >= m->size ? 0 : min((uint64_t)size, m->size - f->offset);
    size_t done = 0;
    while (done < size) {
        uint64_t offset = f->offset + done;
        size_t part = min(size - done, PAGE_SIZE - offset % PAGE_SIZE);
        void *p = (uint8_t *)phys_ptr(m->pages[offset / PAGE_SIZE].physical) + offset % PAGE_SIZE;
        if (write)
            memcpy(p, (uint8_t *)data + done, part);
        else
            memcpy((uint8_t *)data + done, p, part);
        done += part;
    }
    f->offset += done;
    return done;
}

bool memfd_map_page(struct memory_object *m, uint64_t offset, bool write, uint64_t *physical) {
    uint64_t irq = spin_lock_irqsave(&memory_lock);
    bool ok = offset / PAGE_SIZE < m->count && (!write || !(m->seals & SEAL_WRITE));
    if (ok) {
        *physical = m->pages[offset / PAGE_SIZE].physical;
        m->references++;
        m->mappings++;
        m->writable += write;
    }
    spin_unlock_irqrestore(&memory_lock, irq);
    return ok;
}

bool memfd_retain_page(uint64_t physical, bool write) {
    uint64_t irq = spin_lock_irqsave(&memory_lock);
    struct memory_page *p = page_at(physical);
    bool ok = p && (!write || !(p->object->seals & SEAL_WRITE));
    if (ok) {
        p->object->references++;
        p->object->mappings++;
        p->object->writable += write;
    }
    spin_unlock_irqrestore(&memory_lock, irq);
    return ok;
}

bool memfd_write_enable(uint64_t physical) {
    uint64_t irq = spin_lock_irqsave(&memory_lock);
    struct memory_page *p = page_at(physical);
    bool ok = p && !(p->object->seals & SEAL_WRITE);
    if (ok)
        p->object->writable++;
    spin_unlock_irqrestore(&memory_lock, irq);
    return ok;
}

void memfd_write_disable(uint64_t physical) {
    uint64_t irq = spin_lock_irqsave(&memory_lock);
    struct memory_page *p = page_at(physical);
    if (p)
        p->object->writable--;
    spin_unlock_irqrestore(&memory_lock, irq);
}

void memfd_unmap_page(uint64_t physical, bool write) {
    uint64_t irq = spin_lock_irqsave(&memory_lock);
    struct memory_page *p = page_at(physical);
    struct memory_object *m = p ? p->object : NULL;
    bool last = false;
    if (m) {
        m->mappings--;
        m->writable -= write;
        last = !--m->references;
    }
    spin_unlock_irqrestore(&memory_lock, irq);
    if (last)
        destroy(m);
}

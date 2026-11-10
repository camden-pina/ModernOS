#ifndef USER_MEMORY_H
#define USER_MEMORY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define USER_MIN 0x10000ULL
#define USER_TOP 0x0000800000000000ULL
#define USER_STACK_TOP 0x00007ffffff00000ULL
#define USER_STACK_SIZE (8 * 1024 * 1024)
#define USER_MAX_IMAGE (128 * 1024 * 1024)
#define USER_MAX_ALLOC (512ULL * 1024 * 1024)
#if CONFIG_LARGE_BUILDS
/* Per-operation bounds, not additional resident or sandbox budget grants. */
#define USER_NATIVE_MAX_ALLOC (32ULL * 1024 * 1024 * 1024)
#define USER_MAX_FILE (8ULL * 1024 * 1024 * 1024)
#else
#define USER_NATIVE_MAX_ALLOC USER_MAX_ALLOC
#define USER_MAX_FILE (256ULL * 1024 * 1024)
#endif

/* Mapping metadata belongs to the VM, including while vfork or kernel workers
   borrow it. Live user threads stay on one CPU until TLB shootdowns exist. */
struct user_budget;
struct user_range;
struct user_vm {
    uint64_t brk_base, brk_end, mmap_next, generation, pages, page_limit, peak_pages;
    unsigned references;
    struct user_budget *budget; /* shared aggregate across descendant VMs */
    struct user_range *ranges;
    uint64_t virtual_pages, range_tag;
    bool range_restricted;
};

struct address_space {
    uint64_t root;
    struct user_vm *vm;
};

static inline uint64_t user_alloc_limit(const struct address_space *space) {
    return space->vm->budget ? USER_MAX_ALLOC : USER_NATIVE_MAX_ALLOC;
}

void *phys_ptr(uint64_t p);
uint64_t kernel_cr3(void);
void user_cpu_init(void);
bool user_space_create(struct address_space *s);
struct user_budget *user_budget_create(uint64_t page_limit);
void user_budget_get(struct user_budget *);
void user_budget_put(struct user_budget *);
bool user_budget_charge(struct user_budget *, uint64_t pages);
void user_budget_uncharge(struct user_budget *, uint64_t pages);
uint64_t user_budget_pages(const struct user_budget *);
uint64_t user_budget_limit(const struct user_budget *);
uint64_t user_budget_peak(const struct user_budget *);
/* Attach only to an empty VM, before any user image/stack/mapping is made. */
bool user_space_budget_attach(struct address_space *, struct user_budget *, uint64_t limit);
void user_space_limit(struct address_space *, uint64_t limit);
bool user_space_clone(struct address_space *out, const struct address_space *source);
void user_space_share(struct address_space *out, const struct address_space *source);
void user_space_destroy(struct address_space *s);
bool user_map(struct address_space *s, uint64_t va, unsigned flags);
/* Private snapshots/images are marked separately: discard cannot reload them. */
bool user_map_file(struct address_space *, uint64_t va, unsigned flags);
bool user_file_snapshot(const struct address_space *, uint64_t va);
long user_discard(struct address_space *, uint64_t address, uint64_t bytes, unsigned advice);
/* Checked demand-zero recovery only. Never handles protection/EOF faults. */
bool user_zero_fault(const struct address_space *, uint64_t address, uint64_t error);
/* Eager snapshot EOF pages retain allocated/charged backing but never become
   hardware-present. Intended access faults deliver BUS_ADRERR. */
bool user_map_file_eof(struct address_space *, uint64_t va, unsigned flags);
bool user_file_eof(const struct address_space *, uint64_t va);
bool user_file_eof_fault(const struct address_space *, uint64_t va, uint64_t error);
struct memory_object;
bool user_map_memory(struct address_space *, uint64_t va, unsigned flags, struct memory_object *,
                     uint64_t offset, bool readonly);
struct vnode;
bool user_map_shared_file(struct address_space *, uint64_t va, unsigned flags,
                          const struct vnode *, uint64_t offset, uint64_t write_origin);
bool user_shared(const struct address_space *, uint64_t va);
bool user_protect(struct address_space *s, uint64_t va, unsigned flags);
bool user_unmap(struct address_space *s, uint64_t va);
bool user_access(const struct address_space *s, uint64_t va, size_t len, bool write);
/* Includes allocated PROT_NONE pages; used to classify a user page fault. */
bool user_mapped(const struct address_space *s, uint64_t va);
bool user_mapping_flags(const struct address_space *, uint64_t va, unsigned *flags);
bool user_copy_in(const struct address_space *s, void *dst, uint64_t src, size_t len);
/* Internal only: the entire source range MUST have passed user_access(). */
bool user_space_direct(const struct address_space *s);
bool user_copy_in_validated(const struct address_space *s, void *dst, uint64_t src, size_t len);
bool user_copy_video_validated(const struct address_space *s, void *dst, uint64_t src, size_t len);
bool user_copy_out(const struct address_space *s, uint64_t dst, const void *src, size_t len);
bool user_futex_atomic32(const struct address_space *,uint64_t,unsigned,uint32_t,uint32_t *);
bool user_load_bytes(const struct address_space *s, uint64_t dst, const void *src, size_t len);
int user_string(const struct address_space *s, char *dst, uint64_t src, size_t cap);
int user_string_size(const struct address_space *s, uint64_t src, size_t cap, size_t *size);
#endif

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
#define USER_MAX_ALLOC (512 * 1024 * 1024)

/* Mapping metadata belongs to the VM, including while vfork or kernel workers
   borrow it. Live user threads stay on one CPU until TLB shootdowns exist. */
struct user_vm {
    uint64_t brk_base, brk_end, mmap_next, generation, pages, page_limit, peak_pages;
    unsigned references;
};

struct address_space {
    uint64_t root;
    struct user_vm *vm;
};

void *phys_ptr(uint64_t p);
uint64_t kernel_cr3(void);
void user_cpu_init(void);
bool user_space_create(struct address_space *s);
bool user_space_clone(struct address_space *out, const struct address_space *source);
void user_space_share(struct address_space *out, const struct address_space *source);
void user_space_destroy(struct address_space *s);
bool user_map(struct address_space *s, uint64_t va, unsigned flags);
struct memory_object;
bool user_map_memory(struct address_space *, uint64_t va, unsigned flags, struct memory_object *,
                     uint64_t offset, bool readonly);
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
bool user_load_bytes(const struct address_space *s, uint64_t dst, const void *src, size_t len);
int user_string(const struct address_space *s, char *dst, uint64_t src, size_t cap);
int user_string_size(const struct address_space *s, uint64_t src, size_t cap, size_t *size);
#endif

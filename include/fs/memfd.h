#ifndef MODERNOS_MEMFD_H
#define MODERNOS_MEMFD_H
#include <fs/vfs.h>

struct memory_object;
struct memory_account;
void memfd_account_release(struct memory_account *);
struct memory_object *memfd_create(unsigned flags, unsigned domain);
void memfd_release(struct memory_object *);
long memfd_resize(struct memory_object *, uint64_t size);
long memfd_transfer(struct file *, void *, size_t, bool write);
uint64_t memfd_size(struct memory_object *);
long memfd_seals(struct memory_object *, bool add, unsigned seals);
unsigned memfd_domain(struct memory_object *);
bool memfd_authorize(struct memory_object *, unsigned domain, unsigned renderer);
/* Each shared PTE owns a reference, including after close, fork and exec.
   Writable counts prevent sealing a page which a CPU can still write. */
bool memfd_map_page(struct memory_object *, uint64_t offset, bool write, uint64_t *physical);
bool memfd_retain_page(uint64_t physical, bool write);
bool memfd_write_enable(uint64_t physical);
void memfd_write_disable(uint64_t physical);
void memfd_unmap_page(uint64_t physical, bool write);
#endif

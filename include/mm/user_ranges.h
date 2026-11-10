#ifndef USER_RANGES_H
#define USER_RANGES_H
#include <mm/user.h>
/* Default-off, exact-hash Electron admission is the only production source of
   this tree budget. These are virtual bounds, not permission/commit grants;
   user_ranges_sparse rejects smaller inherited or restricted contexts. */
#define USER_RESERVE_MAX (1380ULL << 30)
/* Language servers plus a Markdown webview fill the former 16 TiB tree
   reservation ceiling before search can fork its helper. Allow 32 TiB of
   sparse virtual space across the admitted tree, including fork/exec overlap.
   Physical backing, per-request size, records and task limits are unchanged.
   The shared counters and checked arithmetic are uint64_t, including pages. */
#define USER_VIRTUAL_MAX (32768ULL << 30)
#define USER_RANGE_MAX 16384
/* Per-VM callers retain the existing serialized address-space mutation rules.
   Shared tree counters are atomic across different VMs/CPUs. */
/* backed means a leaf is installed, including an anonymous demand-zero
   marker after discard. Actual charged PFNs are counted by user_vm.pages. */
struct user_range { uint64_t first, end, tag; bool backed, sparse; struct user_range *next; };
bool user_budget_virtual_take(struct user_budget *, uint64_t);
void user_budget_virtual_give(struct user_budget *, uint64_t);
bool user_budget_record_take(struct user_budget *, unsigned);
void user_budget_record_give(struct user_budget *, unsigned);
uint64_t user_budget_virtual_pages(const struct user_budget *);
uint64_t user_budget_records(const struct user_budget *);
bool user_ranges_add(struct address_space *, uint64_t, uint64_t, bool backed, bool sparse);
bool user_ranges_clone(struct address_space *, const struct address_space *);
void user_ranges_destroy(struct address_space *);
bool user_ranges_occupied(const struct address_space *, uint64_t);
bool user_ranges_free(const struct address_space *, uint64_t, uint64_t);
bool user_ranges_select(const struct address_space *, uint64_t, uint64_t, uint64_t *);
bool user_ranges_sparse(const struct address_space *);
bool user_ranges_new_mapping(struct address_space *);
bool user_ranges_reserve(struct address_space *, uint64_t, uint64_t, bool fixed);
bool user_ranges_unmap(struct address_space *, uint64_t, uint64_t);
bool user_ranges_has_sparse(const struct address_space *, uint64_t, uint64_t);
long user_ranges_protect(struct address_space *, uint64_t, uint64_t, unsigned);
long user_ranges_remap(struct address_space *, uint64_t, uint64_t, uint64_t);
/* Backing-only primitives: no virtual ledger mutation. Internal MM use only. */
bool user_commit_backing(struct address_space *, uint64_t, unsigned);
bool user_unmap_backing(struct address_space *, uint64_t);
bool user_protect_backing(struct address_space *, uint64_t, unsigned);
#endif

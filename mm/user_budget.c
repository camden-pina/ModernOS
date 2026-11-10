#include <mm/user.h>
#include <mm/user_ranges.h>
#include <mm/pmm.h>
#include <panic.h>
#if CONFIG_ELECTRON_PROFILE
#include <printk.h>
#endif

/* One reference per distinct VM, plus any explicit in-flight owner. Shared
   threads/vfork/idle CR3 borrowing retain the VM, not another page charge. */
struct user_budget {
    uint64_t limit, pages, peak;
    uint64_t virtual_pages, virtual_peak, records, record_peak;
    unsigned references;
};

struct user_budget *user_budget_create(uint64_t limit) {
    if (!limit) return NULL;
    struct user_budget *b = kmallocz(sizeof(*b));
    if (!b) return NULL;
    b->limit = limit;
    b->references = 1;
    return b;
}
void user_budget_get(struct user_budget *b) {
    if (b) {
        unsigned before = __atomic_fetch_add(&b->references, 1, __ATOMIC_ACQ_REL);
        kassert(before && before != (unsigned)-1);
    }
}
void user_budget_put(struct user_budget *b) {
    if (!b) return;
    unsigned before = __atomic_fetch_sub(&b->references, 1, __ATOMIC_ACQ_REL);
    kassert(before);
    if (before == 1) {
#if CONFIG_ELECTRON_PROFILE
        if(user_budget_pages(b))pr_warn("ELECTRON_BUDGET_FINAL_NONZERO pages=%llu virtual=%llu records=%llu caller=%p\n",
            (unsigned long long)user_budget_pages(b),(unsigned long long)b->virtual_pages,
            (unsigned long long)b->records,__builtin_return_address(0));
#endif
        kassert(!__atomic_load_n(&b->pages, __ATOMIC_ACQUIRE));
        kassert(!b->virtual_pages && !b->records);
#if CONFIG_ELECTRON_PROFILE
        pr_info("electron-budget: released peak=%llu limit=%llu pages (4096 bytes each)\n",
                (unsigned long long)user_budget_peak(b), (unsigned long long)b->limit);
#endif
#if CONFIG_ELECTRON_PROFILE
        pr_info("electron-virtual: released peak_pages=%llu records_peak=%llu\n",
                (unsigned long long)b->virtual_peak, (unsigned long long)b->record_peak);
#endif
        kfree(b);
    }
}
bool user_budget_charge(struct user_budget *b, uint64_t pages) {
    if (!b || !pages) return true;
    uint64_t before = __atomic_load_n(&b->pages, __ATOMIC_ACQUIRE);
    do {
        if (before > b->limit || pages > b->limit - before) {
#if CONFIG_ELECTRON_PROFILE
            pr_warn("electron-budget: denied current=%llu request=%llu limit=%llu pages\n",
                    (unsigned long long)before, (unsigned long long)pages,
                    (unsigned long long)b->limit);
#endif
            return false;
        }
    } while (!__atomic_compare_exchange_n(&b->pages, &before, before + pages, false,
                                          __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
    uint64_t peak = __atomic_load_n(&b->peak, __ATOMIC_RELAXED);
    while (peak < before + pages &&
           !__atomic_compare_exchange_n(&b->peak, &peak, before + pages, false,
                                         __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    return true;
}
void user_budget_uncharge(struct user_budget *b, uint64_t pages) {
    if (!b || !pages) return;
    uint64_t before = __atomic_load_n(&b->pages, __ATOMIC_ACQUIRE);
    do {
        kassert(before >= pages);
    } while (!__atomic_compare_exchange_n(&b->pages, &before, before - pages, false,
                                          __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
}
uint64_t user_budget_pages(const struct user_budget *b) {
    return b ? __atomic_load_n(&b->pages, __ATOMIC_ACQUIRE) : 0;
}
uint64_t user_budget_limit(const struct user_budget *b) { return b ? b->limit : 0; }
uint64_t user_budget_peak(const struct user_budget *b) {
    return b ? __atomic_load_n(&b->peak, __ATOMIC_ACQUIRE) : 0;
}

static bool take_counter(uint64_t *p, uint64_t *peak, uint64_t n, uint64_t max) {
    uint64_t old = __atomic_load_n(p, __ATOMIC_ACQUIRE);
    do { if (old > max || n > max - old) return false; }
    while (!__atomic_compare_exchange_n(p, &old, old + n, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
    uint64_t high = __atomic_load_n(peak, __ATOMIC_RELAXED);
    while (high < old + n && !__atomic_compare_exchange_n(peak, &high, old + n, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    return true;
}
static void give_counter(uint64_t *p, uint64_t n) {
    uint64_t old = __atomic_load_n(p, __ATOMIC_ACQUIRE);
    do { kassert(old >= n); }
    while (!__atomic_compare_exchange_n(p, &old, old - n, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
}
bool user_budget_virtual_take(struct user_budget *b, uint64_t pages) {
    bool ok = take_counter(&b->virtual_pages, &b->virtual_peak, pages, USER_VIRTUAL_MAX / PAGE_SIZE);
#if CONFIG_ELECTRON_PROFILE
    if (!ok) pr_warn("electron-virtual: denied current=%llu request=%llu limit=%llu pages\n",
                     (unsigned long long)user_budget_virtual_pages(b),
                     (unsigned long long)pages, (unsigned long long)(USER_VIRTUAL_MAX / PAGE_SIZE));
#endif
    return ok;
}
void user_budget_virtual_give(struct user_budget *b, uint64_t pages) { give_counter(&b->virtual_pages, pages); }
bool user_budget_record_take(struct user_budget *b, unsigned n) {
    bool ok=take_counter(&b->records, &b->record_peak, n, USER_RANGE_MAX);
#if CONFIG_ELECTRON_PROFILE
    if(!ok)pr_warn("electron-records: denied current=%llu request=%u limit=%u\n",
                  (unsigned long long)user_budget_records(b),n,USER_RANGE_MAX);
#endif
    return ok;
}
void user_budget_record_give(struct user_budget *b, unsigned n) { give_counter(&b->records, n); }
uint64_t user_budget_virtual_pages(const struct user_budget *b) { return __atomic_load_n(&b->virtual_pages, __ATOMIC_ACQUIRE); }
uint64_t user_budget_records(const struct user_budget *b) { return __atomic_load_n(&b->records, __ATOMIC_ACQUIRE); }

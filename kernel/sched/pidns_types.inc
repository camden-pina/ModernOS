/* P1 fixed metadata, compiled only for the independent approved experiment. */
#define P1_NAMESPACES 8
#define P1_DEPTH 4
#define P1_IDENTITIES 238
/* Compact signal provenance keeps all238 global task rows in24 pages, including sixteen independent filesystem states.
 * The 27-page charge covers the larger filesystem pool, external fields and stack. */
#define P1_ARENA_PAGES 24
#define S1_STATES 16
#define P1_CHARGED_PAGES 27
struct p1_identity {
    unsigned outer,references,number[P1_DEPTH];
    uint8_t ns,depth;uint16_t reserved;
};
struct p1_namespace {
    uint64_t id;
    struct c2_namespace *owner;
    unsigned references,next,init;
    uint8_t parent,depth,state,used;
};
struct p1_group {
    uint64_t group;
    unsigned pgid,sid;
    uint8_t pgid_id,sid_id;
#if CONFIG_FILESYSTEM_SANDBOX_TEST
    uint8_t filesystem_index,filesystem_flags;
#else
    uint16_t reserved;
#endif
};
/* Actual outer IDs are only0/1000; unmapped65534 is produced at delivery.
 * Pack the valid bit beside the15-bit stored UID. All records are protected
 * by c2_lock; no user ABI or lock-free/atomic layout depends on this record.
 * Sender is a retained identity index. code-128 represents SI_KERNEL128. */
struct __attribute__((packed)) p1_signal_record {
    uint8_t sender;
    int8_t code;
    uint16_t uid:15,valid:1;
    uint8_t status;
};
#if CONFIG_FILESYSTEM_SANDBOX_TEST
struct s1_filesystem {
    struct vnode root;
    uint64_t serial;
    unsigned references,creation_mask,borrower_outer_pid;
    uint8_t root_kind,used;uint16_t reserved;
    char cwd[OS_PATH_MAX];
};
_Static_assert(sizeof(struct s1_filesystem)==648,"S1 fixed layout");
#endif
struct p1_arena {
    uintptr_t physical;
    struct c2_domain *domain;
    unsigned installing,identities,namespaces,peak_identities,peak_namespaces;
    bool committed;
    struct p1_namespace ns[P1_NAMESPACES];
    struct p1_identity ids[P1_IDENTITIES];
    struct p1_group groups[C2_PROCESSES];
    unsigned signal_target[TASK_MAX];
    struct p1_signal_record signals[TASK_MAX][64];
#if CONFIG_FILESYSTEM_SANDBOX_TEST
    struct s1_filesystem filesystem[S1_STATES];
#endif
};
_Static_assert(sizeof(struct p1_signal_record)==5,"P1 signal provenance layout");
_Static_assert(P1_IDENTITIES<=UINT8_MAX,"P1 signal sender index width");
_Static_assert(sizeof(struct p1_arena)<=P1_ARENA_PAGES*PAGE_SIZE,"P1 arena bound");
_Static_assert(P1_ARENA_PAGES+1<=P1_CHARGED_PAGES,"P1 charged arena plus scratch margin");
_Static_assert(P1_CHARGED_PAGES*PAGE_SIZE==108*1024,"P1 hard metadata ceiling");

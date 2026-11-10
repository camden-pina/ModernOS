/* Boot verifier only: no syscall, credential or executable-admission escape.
 * These kernel-owned fixtures exercise page/lifetime primitives; actual
 * admission denials are also tested in userspace before real Electron. */
static unsigned r1_checks;
#define R1_OK(x) do { ++r1_checks; kassert(x); } while (0)
void ramfs_shared_write_selftest(void) {
    struct mount *m=kmallocz(sizeof(*m));
    struct vnode root,v,imported;
    struct address_space a={0},b={0},c={0},thread={0};
    struct user_budget *budget=user_budget_create(16);
    const uint64_t base=0x20000000,token=0x123456789ULL;
    R1_OK(m && budget && !ramfs_mount(m));
    strcpy(m->path,"/");R1_OK(!ram_root(m,&root));
    R1_OK(!ram_create(&root,"r1-unit",S_IFREG|0600,&v));
    struct ram_node *n=(void *)(uintptr_t)v.id;
    R1_OK(!n->write_origin && !n->imported);
    R1_OK(!ram_resize(&v,2*PAGE_SIZE));
    n->uid=n->gid=1000;node_info(m,n,&v);
    R1_OK(ramfs_shared_validate_write(&v,0,PAGE_SIZE,token)<0);
    n->write_origin=token; /* Unit-only fixture, never available to userspace. */
    R1_OK(!ramfs_shared_validate_write(&v,0,2*PAGE_SIZE,token));
    R1_OK(ramfs_shared_validate_write(&v,0,PAGE_SIZE,token+1)<0);
    R1_OK(ramfs_shared_validate_write(&v,1,PAGE_SIZE,token)<0);
    R1_OK(ramfs_shared_validate_write(&v,0,PAGE_SIZE-1,token)<0);
    R1_OK(ramfs_shared_validate_write(&v,2*PAGE_SIZE,PAGE_SIZE,token)<0);
    R1_OK(ramfs_shared_validate_write(&v,UINT64_MAX-PAGE_SIZE+1,PAGE_SIZE,token)<0);
    m->readonly=true;R1_OK(ramfs_shared_validate_write(&v,0,PAGE_SIZE,token)<0);m->readonly=false;
    n->uid=0;R1_OK(ramfs_shared_validate_write(&v,0,PAGE_SIZE,token)<0);n->uid=1000;
    n->mode=S_IFREG|0400;R1_OK(ramfs_shared_validate_write(&v,0,PAGE_SIZE,token)<0);n->mode=S_IFREG|0600;
    n->size--;R1_OK(CONFIG_RAMFS_PARTIAL_WRITE_TEST ?
        ramfs_shared_validate_write(&v,0,PAGE_SIZE,token)==0 :
        ramfs_shared_validate_write(&v,0,PAGE_SIZE,token)<0);n->size++;
    R1_OK(user_space_create(&a) && user_space_create(&b));
    R1_OK(user_space_budget_attach(&a,budget,0) && user_space_budget_attach(&b,budget,0));
    R1_OK(user_ranges_new_mapping(&a));
    r1_fail_after=1;
    R1_OK(!user_map_shared_file(&a,base,VM_RDWR,&v,0,token));
    R1_OK(!a.vm->pages && !user_budget_pages(budget) && !n->shared_mappings && !n->shared);
    r1_fail_after=0;
    for(unsigned i=0;i<2;i++)R1_OK(user_map_shared_file(&a,base+i*PAGE_SIZE,VM_RDWR,&v,i*PAGE_SIZE,token));
    R1_OK(user_ranges_new_mapping(&b));
    for(unsigned i=0;i<2;i++)R1_OK(user_map_shared_file(&b,base+i*PAGE_SIZE,VM_READ,&v,i*PAGE_SIZE,0));
    R1_OK(n->shared_mappings==4 && user_budget_pages(budget)==4);
    char x='A',y=0;
    R1_OK(user_copy_out(&a,base,&x,1));R1_OK(user_copy_in(&b,&y,base,1) && y=='A');
    R1_OK(ram_read(&v,0,&y,1)==1 && y=='A');
    x='B';R1_OK(ram_write(&v,PAGE_SIZE,&x,1)==1);
    R1_OK(user_copy_in(&a,&y,base+PAGE_SIZE,1) && y=='B');
    R1_OK(!user_copy_out(&b,base,&x,1));
    R1_OK(!user_protect_backing(&b,base,VM_RDWR));
    R1_OK(!user_protect_backing(&a,base,VM_READ|VM_EXEC));
    R1_OK(user_protect_backing(&a,base,0));
    R1_OK(!user_copy_in(&a,&y,base,1));
    R1_OK(user_space_clone(&c,&a)); /* Includes original maximum permission. */
    R1_OK(user_protect_backing(&c,base,VM_RDWR));
    x='C';R1_OK(user_copy_out(&c,base,&x,1));
    R1_OK(user_copy_in(&b,&y,base,1) && y=='C');
    R1_OK(user_protect_backing(&a,base,VM_READ));
    R1_OK(user_protect_backing(&a,base,VM_RDWR));
    user_space_share(&thread,&a);R1_OK(thread.vm==a.vm && user_budget_pages(budget)==6);
    user_space_destroy(&thread);R1_OK(user_budget_pages(budget)==6);
    struct ram_shared *p=n->shared;R1_OK(p);
    unsigned refs=p->references;p->references=UINT32_MAX;
    R1_OK(!ramfs_shared_retain(p->physical,true));p->references=refs;
    unsigned maps=n->shared_mappings;n->shared_mappings=UINT32_MAX;
    uint64_t unused;R1_OK(!ramfs_shared_map(&v,0,&unused));n->shared_mappings=maps;
    R1_OK(ram_resize(&v,PAGE_SIZE)==-E_BUSY);
    R1_OK(ram_resize(&v,3*PAGE_SIZE)==-E_BUSY);
    R1_OK(ram_truncate(&v)==-E_BUSY);
    ram_retain(&v);R1_OK(!ram_unlink(&root,"r1-unit",false));ram_release(&v);
    R1_OK(!n->links && !n->opens && n->shared_mappings==6);
    user_space_destroy(&a);user_space_destroy(&b);
    R1_OK(user_copy_in(&c,&y,base,1) && y=='C');
    user_space_destroy(&c); /* Last reference destroys the unlinked inode. */
    R1_OK(!user_budget_pages(budget) && !user_budget_records(budget) && !user_budget_virtual_pages(budget));
    user_budget_put(budget);
    R1_OK(!ram_create(&root,"r1-import",S_IFREG|0600,&imported));
    char *data=kmallocz(PAGE_SIZE);R1_OK(data);
    R1_OK(!ramfs_import(&imported,data,PAGE_SIZE));
    n=(void *)(uintptr_t)imported.id;R1_OK(n->imported && !n->write_origin);
    n->uid=n->gid=1000;n->write_origin=token;
    R1_OK(ramfs_shared_validate_write(&imported,0,PAGE_SIZE,token)<0);
    x='D';R1_OK(ram_write(&imported,0,&x,1)==1);
    R1_OK(!n->boot_data && n->imported);
    R1_OK(ramfs_shared_validate_write(&imported,0,PAGE_SIZE,token)<0);
    R1_OK(!ram_unlink(&root,"r1-import",false));kfree(data);
    /* Budget reservation/fork failure must leave original aliases intact. */
    R1_OK(!ram_create(&root,"r1-limit",S_IFREG|0600,&v));n=(void *)(uintptr_t)v.id;
    n->uid=n->gid=1000;n->write_origin=token;R1_OK(!ram_resize(&v,2*PAGE_SIZE));
    budget=user_budget_create(2);R1_OK(budget && user_space_create(&a));
    R1_OK(user_space_budget_attach(&a,budget,0) && user_ranges_new_mapping(&a));
    for(unsigned i=0;i<2;i++)R1_OK(user_map_shared_file(&a,base+i*PAGE_SIZE,VM_RDWR,&v,i*PAGE_SIZE,token));
    R1_OK(!user_space_clone(&c,&a));R1_OK(!c.root && user_budget_pages(budget)==2 && n->shared_mappings==2);
    R1_OK(!user_map_shared_file(&a,base+2*PAGE_SIZE,VM_RDWR,&v,0,token));
    R1_OK(user_budget_pages(budget)==2 && n->shared_mappings==2);
    user_space_destroy(&a);R1_OK(!n->shared_mappings && !n->shared);
    R1_OK(!ram_resize(&v,PAGE_SIZE) && !ram_resize(&v,0));
    R1_OK(!ram_unlink(&root,"r1-limit",false));
    R1_OK(!user_budget_pages(budget) && !user_budget_virtual_pages(budget) && !user_budget_records(budget));
    user_budget_put(budget);kfree(m->data);kfree(m);
    printk("R1_SHARED_WRITE_UNIT_PASS checks=%u true_aliases maxpermissions fork sharedVM unlink import_origin refcounts rollback caps cleanup inode_bytes=%zu\n",r1_checks,sizeof(struct ram_node));
}
#undef R1_OK

#if CONFIG_RAMFS_PARTIAL_WRITE_TEST
/* Independent two-CPU verifier only. Different address spaces contend on
   the actual RAMFS shared-page registry; no fake userspace admission. */
struct r1_stress {
    struct mount *mount;struct vnode root,file;struct address_space a,b;
    struct user_budget *budget;unsigned ready,done_a,done_b,cpu_a,cpu_b,tail_ready;
};
static void r1_stress_map(void *arg) {
    struct r1_stress *s=arg;s->cpu_a=this_cpu()->id;
    while(!__atomic_load_n(&s->ready,__ATOMIC_ACQUIRE))sched_yield();
    for(unsigned i=0;i<200;i++) {
        mutex_lock(&vfs_mutex);
        kassert(user_ranges_new_mapping(&s->a));
        kassert(user_map_shared_file(&s->a,0x20000000,VM_RDWR,&s->file,0,0x987654321ULL));
        mutex_unlock(&vfs_mutex);
        char x='A',y=0;kassert(user_copy_out(&s->a,0x20000000,&x,1));
        x='P';kassert(user_copy_out(&s->a,0x20000fff,&x,1));__atomic_store_n(&s->tail_ready,1,__ATOMIC_RELEASE);
        mutex_lock(&vfs_mutex);kassert(ram_read(&s->file,0,&y,1)==1 && y=='A');mutex_unlock(&vfs_mutex);
        kassert(user_ranges_unmap(&s->a,0x20000000,PAGE_SIZE));sched_yield();
    }
    __atomic_store_n(&s->done_a,1,__ATOMIC_RELEASE);
}
static void r1_stress_fork(void *arg) {
    struct r1_stress *s=arg;s->cpu_b=this_cpu()->id;
    __atomic_store_n(&s->ready,1,__ATOMIC_RELEASE);
    while(!__atomic_load_n(&s->tail_ready,__ATOMIC_ACQUIRE))sched_yield();
    for(unsigned i=0;i<200;i++) {
        struct address_space child={0};kassert(user_space_clone(&child,&s->b));
        char x='B',y=0;mutex_lock(&vfs_mutex);kassert(ram_write(&s->file,1,&x,1)==1);mutex_unlock(&vfs_mutex);
        kassert(user_copy_in(&child,&y,0x20000001,1) && y=='B');
        kassert(user_copy_in(&child,&y,0x20000fff,1) && y=='P');
        kassert(!user_protect_backing(&child,0x20000000,VM_RDWR));
        user_space_destroy(&child);sched_yield();
    }
    __atomic_store_n(&s->done_b,1,__ATOMIC_RELEASE);
}
static void r2_growth_map(void *arg) {
    struct r1_stress *s=arg;s->cpu_a=this_cpu()->id;
    while(!__atomic_load_n(&s->ready,__ATOMIC_ACQUIRE))sched_yield();
    for(unsigned i=0;i<200;i++) {
        mutex_lock(&vfs_mutex);bool partial=((struct ram_node *)(uintptr_t)s->file.id)->size==4;
        kassert(user_ranges_new_mapping(&s->a));kassert(user_map_shared_file(&s->a,0x20000000,VM_RDWR,&s->file,0,0x987654321ULL));mutex_unlock(&vfs_mutex);
        char x='A';kassert(user_copy_out(&s->a,0x20000000,&x,1));
        if(partial){x='P';kassert(user_copy_out(&s->a,0x20000fff,&x,1));}
        kassert(user_ranges_unmap(&s->a,0x20000000,PAGE_SIZE));sched_yield();
    }
    __atomic_store_n(&s->done_a,1,__ATOMIC_RELEASE);
}
static void r2_growth_resize(void *arg) {
    struct r1_stress *s=arg;s->cpu_b=this_cpu()->id;char *bytes=kmalloc(PAGE_SIZE);kassert(bytes);
    __atomic_store_n(&s->ready,1,__ATOMIC_RELEASE);
    for(unsigned i=0;i<200;i++) {
        mutex_lock(&vfs_mutex);uint64_t old=((struct ram_node *)(uintptr_t)s->file.id)->size;
        int e=ram_resize(&s->file,2*PAGE_SIZE);kassert(!e || e==-E_BUSY);
        if(!e && old==4) {
            kassert(ram_read(&s->file,4,bytes,PAGE_SIZE-4)==PAGE_SIZE-4);
            for(unsigned j=0;j<PAGE_SIZE-4;j++)kassert(!bytes[j]);
        }
        e=ram_resize(&s->file,4);kassert(!e || e==-E_BUSY);mutex_unlock(&vfs_mutex);sched_yield();
    }
    kfree(bytes);__atomic_store_n(&s->done_b,1,__ATOMIC_RELEASE);
}
static void r1_stress_coordinate(void *arg) {
    (void)arg;struct r1_stress *s=kmallocz(sizeof(*s));kassert(s);
    s->mount=kmallocz(sizeof(*s->mount));s->budget=user_budget_create(128);kassert(s->mount && s->budget);
    mutex_lock(&vfs_mutex);kassert(!ramfs_mount(s->mount));strcpy(s->mount->path,"/");kassert(!ram_root(s->mount,&s->root));
    kassert(!ram_create(&s->root,"r1-smp",S_IFREG|0600,&s->file));
    struct ram_node *n=(void *)(uintptr_t)s->file.id;n->uid=n->gid=1000;n->write_origin=0x987654321ULL;
    kassert(!ram_resize(&s->file,4));ram_retain(&s->file);mutex_unlock(&vfs_mutex);
    kassert(user_space_create(&s->a) && user_space_create(&s->b));
    kassert(user_space_budget_attach(&s->a,s->budget,0) && user_space_budget_attach(&s->b,s->budget,0));
    mutex_lock(&vfs_mutex);kassert(user_ranges_new_mapping(&s->b));
    kassert(user_map_shared_file(&s->b,0x20000000,VM_READ,&s->file,0,0));
    mutex_unlock(&vfs_mutex);
    kassert(task_kernel("r1-map",r1_stress_map,s,0)>0 && task_kernel("r1-fork",r1_stress_fork,s,1)>0);
    unsigned waited=0;
    while(!__atomic_load_n(&s->done_a,__ATOMIC_ACQUIRE) || !__atomic_load_n(&s->done_b,__ATOMIC_ACQUIRE)) {
        kassert(++waited<20000);sched_sleep(1000);
    }
    kassert(s->cpu_a==0 && s->cpu_b==1 && !s->a.vm->pages && s->b.vm->pages==1);
    kassert(user_budget_pages(s->budget)==1 && n->shared_mappings==1);
    user_space_destroy(&s->a);user_space_destroy(&s->b);
    kassert(user_space_create(&s->a) && user_space_budget_attach(&s->a,s->budget,0));
    s->done_a=s->done_b=s->ready=0;
    kassert(task_kernel("r2-grow-map",r2_growth_map,s,0)>0 && task_kernel("r2-grow-size",r2_growth_resize,s,1)>0);
    waited=0;
    while(!__atomic_load_n(&s->done_a,__ATOMIC_ACQUIRE) || !__atomic_load_n(&s->done_b,__ATOMIC_ACQUIRE)) {
        kassert(++waited<20000);sched_sleep(1000);
    }
    kassert(s->cpu_a==0 && s->cpu_b==1 && !s->a.vm->pages && !n->shared_mappings && !n->shared);
    mutex_lock(&vfs_mutex);kassert(!ram_resize(&s->file,2*PAGE_SIZE));
    char value=1;kassert(ram_read(&s->file,PAGE_SIZE-1,&value,1)==1 && !value);mutex_unlock(&vfs_mutex);
    user_space_destroy(&s->a);
    kassert(!user_budget_pages(s->budget) && !user_budget_records(s->budget) && !user_budget_virtual_pages(s->budget));
    mutex_lock(&vfs_mutex);kassert(!n->shared_mappings && !n->shared);kassert(!ram_unlink(&s->root,"r1-smp",false));ram_release(&s->file);mutex_unlock(&vfs_mutex);
    user_budget_put(s->budget);kfree(s->mount->data);kfree(s->mount);kfree(s);
    printk("R2_PARTIAL_PAGE_SMP_PASS cpus=0,1 map_unmap_cycles=200 fork_release_cycles=200 map_resize_cycles=200 coherent_padding readonly_inheritance exact_refunds=1\n");
}
void ramfs_shared_write_stress_start(void) {
    kassert(sched_cpu_count()>=2);kassert(task_kernel("r1-smp",r1_stress_coordinate,NULL,0)>0);
}

#endif

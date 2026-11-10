/* Verifier-only primitive fixtures; no userspace admission control. */
static unsigned r2_checks;
#define R2_OK(x) do { ++r2_checks; kassert(x); } while (0)
void ramfs_partial_selftest(void) {
    const uint64_t sizes[]={1,4,4095,4097},base=0x20000000,token=0xabcdef123ULL;
    struct mount *m=kmallocz(sizeof(*m));struct vnode root,v;
    char *bytes=kmalloc(PAGE_SIZE);R2_OK(m && bytes && !ramfs_mount(m));
    strcpy(m->path,"/");R2_OK(!ram_root(m,&root));
    for(unsigned test=0;test<4;test++) {
        uint64_t length=sizes[test],rounded=align(length,PAGE_SIZE);
        uint64_t last=(length-1)&~(PAGE_SIZE-1),tail=length&(PAGE_SIZE-1),padding=PAGE_SIZE-tail;
        struct address_space a={0},b={0},c={0};
        struct user_budget *budget=user_budget_create(64);R2_OK(budget);
        R2_OK(!ram_create(&root,"r2-unit",S_IFREG|0600,&v));
        struct ram_node *n=(void *)(uintptr_t)v.id;n->uid=n->gid=1000;n->write_origin=token;
        R2_OK(!ram_resize(&v,length));ram_retain(&v);
        char x='A',y=0;R2_OK(ram_write(&v,0,&x,1)==1);
        R2_OK(!ramfs_shared_validate_write(&v,0,rounded,token));
        R2_OK(!ramfs_shared_validate_write(&v,last,PAGE_SIZE,token));
        R2_OK(ramfs_shared_validate_write(&v,rounded,PAGE_SIZE,token)<0);
        R2_OK(ramfs_shared_validate_write(&v,0,rounded+PAGE_SIZE,token)<0);
        R2_OK(ramfs_shared_validate_write(&v,0,rounded,token+1)<0);
        R2_OK(ramfs_shared_validate_write(&v,1,PAGE_SIZE,token)<0);
        R2_OK(ramfs_shared_validate_write(&v,UINT64_MAX-PAGE_SIZE+1,PAGE_SIZE,token)<0);
        memset((uint8_t *)phys_ptr(n->pages[last/PAGE_SIZE])+tail,0x5a,padding);
        R2_OK(user_space_create(&a) && user_space_create(&b));
        R2_OK(user_space_budget_attach(&a,budget,0) && user_space_budget_attach(&b,budget,0));
        R2_OK(user_ranges_new_mapping(&a));r1_fail_after=1;
        R2_OK(!user_map_shared_file(&a,base,VM_RDWR,&v,0,token));
        R2_OK(!a.vm->pages && !user_budget_pages(budget) && !n->shared_mappings && !n->shared);
        r1_fail_after=0;
        for(uint64_t off=0;off<rounded;off+=PAGE_SIZE)
            R2_OK(user_map_shared_file(&a,base+off,VM_RDWR,&v,off,token));
        R2_OK(user_copy_in(&a,bytes,base+length,padding));
        for(unsigned i=0;i<padding;i++)R2_OK(bytes[i]==0);
        x='V';R2_OK(user_copy_out(&a,base+length-1,&x,1));
        R2_OK(ram_read(&v,length-1,&y,1)==1 && y=='V');
        memset(bytes,0xcc,padding);R2_OK(user_copy_out(&a,base+length,bytes,padding));
        /* A new alias must not erase padding written through a live alias. */
        R2_OK(user_ranges_new_mapping(&b));
        for(uint64_t off=0;off<rounded;off+=PAGE_SIZE)
            R2_OK(user_map_shared_file(&b,base+off,VM_READ,&v,off,0));
        R2_OK(user_copy_in(&b,bytes,base+length,padding));
        for(unsigned i=0;i<padding;i++)R2_OK((unsigned char)bytes[i]==0xcc);
        R2_OK(n->size==length && ram_read(&v,length,&y,1)==0);
        R2_OK(!user_protect_backing(&b,base+last,VM_RDWR));
        R2_OK(!user_protect_backing(&a,base+last,VM_READ|VM_EXEC));
        R2_OK(user_protect_backing(&a,base+last,0));
        R2_OK(user_space_clone(&c,&a));
        R2_OK(user_protect_backing(&c,base+last,VM_RDWR));
        x='W';R2_OK(user_copy_out(&c,base+length-1,&x,1));
        R2_OK(user_copy_in(&b,&y,base+length-1,1) && y=='W');
        R2_OK(ram_resize(&v,length+1)==-E_BUSY && ram_truncate(&v)==-E_BUSY);
        R2_OK(!ram_unlink(&root,"r2-unit",false));
        user_space_destroy(&a);user_space_destroy(&c);
        R2_OK(user_copy_in(&b,&y,base+length,1) && (unsigned char)y==0xcc);
        user_space_destroy(&b);R2_OK(!n->shared_mappings && !n->shared);
        R2_OK(!user_budget_pages(budget) && !user_budget_records(budget) && !user_budget_virtual_pages(budget));
        /* Same unlinked inode retained by its open description: a fresh page
           registry generation must not expose the old padding bytes. */
        R2_OK(user_space_create(&a) && user_space_budget_attach(&a,budget,0) && user_ranges_new_mapping(&a));
        for(uint64_t off=0;off<rounded;off+=PAGE_SIZE)
            R2_OK(user_map_shared_file(&a,base+off,VM_RDWR,&v,off,token));
        R2_OK(user_copy_in(&a,bytes,base+length,padding));
        for(unsigned i=0;i<padding;i++)R2_OK(bytes[i]==0);
        R2_OK(user_copy_in(&a,&y,base+length-1,1) && y=='W');
        user_space_destroy(&a);
        memset((uint8_t *)phys_ptr(n->pages[last/PAGE_SIZE])+tail,0xdd,padding);
        R2_OK(!ram_resize(&v,length+3));
        R2_OK(ram_read(&v,length,bytes,3)==3 && !bytes[0] && !bytes[1] && !bytes[2]);
        R2_OK(ram_read(&v,length-1,&y,1)==1 && y=='W');
        uint64_t shrink=length>1?length-1:0;
        R2_OK(!ram_resize(&v,shrink) && !ram_resize(&v,length+5));
        R2_OK(ram_read(&v,shrink,bytes,(length+5)-shrink)==(long)((length+5)-shrink));
        for(unsigned i=0;i<(length+5)-shrink;i++)R2_OK(bytes[i]==0);
        uint64_t old=n->size;
        memset((uint8_t *)phys_ptr(n->pages[old/PAGE_SIZE])+(old&(PAGE_SIZE-1)),0xee,PAGE_SIZE-(old&(PAGE_SIZE-1)));
        x='Z';R2_OK(ram_write(&v,old+5,&x,1)==1 && n->size==old+6);
        R2_OK(ram_read(&v,old,bytes,6)==6);
        for(unsigned i=0;i<5;i++)R2_OK(bytes[i]==0);
        R2_OK(bytes[5]=='Z' && !n->shared_mappings && !n->shared);
        R2_OK(!user_budget_pages(budget) && !user_budget_records(budget) && !user_budget_virtual_pages(budget));
        ram_release(&v);user_budget_put(budget);
    }
    kfree(bytes);kfree(m->data);kfree(m);
    printk("R2_PARTIAL_PAGE_UNIT_PASS checks=%u sizes=1,4,4095,4097 zero_padding coherent_live_aliases no_file_growth readonly fork unlink remap regrow gap_zeroing rollback refunds\n",r2_checks);
}
#undef R2_OK

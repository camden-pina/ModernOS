#!/usr/bin/env python3
"""Exercise the actual kernel ELF loader with synthetic ELF files and mock VM.

Only privileged VM/file boundaries are replaced. This proves validation,
auxiliary data, mapping flags and rollback; real relocation/page protection
behavior is covered by test_dynamic_linker.py in the booted guest.
"""
from pathlib import Path
import os
import shutil
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

SHIM = r'''
#ifndef ELF_TEST_SHIM
#define ELF_TEST_SHIM
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uapi/abi.h>
#include <mm/user.h>
#define register_t test_register_t
typedef struct { uint64_t rip, cs, ss, rflags, user_rsp; } register_t;
#define PAGE_SIZE 4096
#define VM_READ 1
#define VM_WRITE 2
#define VM_EXEC 4
#define VM_RDWR 3
#define USER_CS 0x23
#define USER_DS 0x1b
#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))
#define align(a,b) (((a)+(b)-1)&~((b)-1))
struct file { struct { uint64_t size; unsigned mode; } node; uint64_t offset; FILE *host; unsigned references; };
static int vfs_mutex;
static void mutex_lock(int *p) { (void)p; }
static void mutex_unlock(int *p) { (void)p; }
static void *kmalloc(size_t n);
static void *kmallocz(size_t n);
static void kfree(void *p);
static int vfs_open(const char *, int, int, struct file **);
static long vfs_read(struct file *, void *, size_t);
static void vfs_close(struct file *);
static long random_bytes(void *, size_t);
#endif
'''

HARNESS = r'''
#include <sched.h>
/* Test-only VM API shim; accounting has independent sanitizer coverage. */
struct user_budget {unsigned refs; uint64_t limit;};
struct user_budget *user_budget_create(uint64_t n){struct user_budget *b=kmallocz(sizeof(*b));if(b){b->limit=n;b->refs=1;}return b;}
void user_budget_put(struct user_budget *b){if(b&&!--b->refs)kfree(b);}
void user_space_limit(struct address_space *s,uint64_t n){if(n&&(!s->vm->page_limit||n<s->vm->page_limit))s->vm->page_limit=n;}
bool user_space_budget_attach(struct address_space *s,struct user_budget *b,uint64_t n){assert(!s->vm->budget);b->refs++;s->vm->budget=b;user_space_limit(s,n);user_space_limit(s,b->limit);return true;}
struct test_page { uint64_t address; unsigned flags; unsigned char bytes[4096]; struct test_page *next; };
static struct test_page *pages;
static unsigned allocations, files, mappings, reads;
static long map_limit = -1, read_limit = -1;
static const char *interpreter_file;
static void *kmalloc(size_t n) { void *p=malloc(n); if (p) allocations++; return p; }
static void *kmallocz(size_t n) { void *p=calloc(1,n); if (p) allocations++; return p; }
static void kfree(void *p) { if (p) { assert(allocations); allocations--; free(p); } }
static int vfs_open(const char *path, int flags, int mode, struct file **out) {
    (void)flags; (void)mode;
    if (!strcmp(path,"/lib/ld-test.so")) path=interpreter_file;
    FILE *f=fopen(path,"rb"); if (!f) return -E_NOENT;
    struct file *p=calloc(1,sizeof *p); assert(p);
    assert(!fseek(f,0,SEEK_END)); p->node.size=ftell(f); p->node.mode=S_IFREG|0755;
    if (getenv("MOCK_NOEXEC")) p->node.mode=S_IFREG|0644;
    p->host=f; *out=p; files++; return 0;
}
static long vfs_read(struct file *f, void *data, size_t n) {
    if (read_limit >= 0 && reads++ >= (unsigned)read_limit) return -E_IO;
    if (fseek(f->host,f->offset,SEEK_SET)) return -E_IO;
    size_t done=fread(data,1,n,f->host); f->offset+=done; return done;
}
static void vfs_close(struct file *f) { assert(files); files--; fclose(f->host); free(f); }
static long random_bytes(void *p,size_t n) { memset(p,0xa5,n); return n; }
bool user_space_create(struct address_space *s) { assert(!pages); s->root=1; s->vm=calloc(1,sizeof *s->vm); return !!s->vm; }
void user_space_destroy(struct address_space *s) {
    while(pages) { struct test_page *p=pages; pages=p->next; free(p); assert(mappings); mappings--; }
    user_budget_put(s->vm->budget); free(s->vm); s->vm=0; s->root=0;
}
static struct test_page *page(uint64_t va) { for(struct test_page *p=pages;p;p=p->next) if(p->address==(va&~4095ULL)) return p; return 0; }
bool user_map(struct address_space *s,uint64_t va,unsigned flags) {
    (void)s; assert(!(va&4095)); assert((flags&6)!=6);
    if(page(va) || (map_limit>=0 && mappings>=(unsigned)map_limit)) return false;
    struct test_page *p=calloc(1,sizeof *p); assert(p); p->address=va; p->flags=flags; p->next=pages; pages=p; mappings++; return true;
}
bool user_load_bytes(const struct address_space *s,uint64_t va,const void *data,size_t n) {
    (void)s; const unsigned char *src=data;
    while(n) { struct test_page *p=page(va); if(!p) return false; size_t part=min(n,4096-(va&4095)); memcpy(p->bytes+(va&4095),src,part); va+=part; src+=part; n-=part; }
    return true;
}
static uint64_t word(uint64_t address) { struct test_page *p=page(address); assert(p && (address&4095)<=4088); uint64_t v; memcpy(&v,p->bytes+(address&4095),8); return v; }
static unsigned char byte(uint64_t address) { struct test_page *p=page(address); assert(p); return p->bytes[address&4095]; }

bool user_map_file(struct address_space *s,uint64_t va,unsigned flags) { return user_map(s,va,flags); }
LOADER_SOURCE

int main(int argc,char **argv) {
    assert(argc==5); interpreter_file=argv[2];
    if(getenv("MOCK_MAP_LIMIT")) map_limit=atol(getenv("MOCK_MAP_LIMIT"));
    if(getenv("MOCK_READ_LIMIT")) read_limit=atol(getenv("MOCK_READ_LIMIT"));
    struct address_space s={0}; register_t frame;
    char *args[]={"fixture","tail",0}, *env[]={"DYNAMIC_TEST=present",0};
    int result=load_elf(argv[1],args,env,&s,&frame,NULL,0,NULL);
    if(result!=-atoi(argv[3])) { fprintf(stderr,"loader returned %d, expected -%s\n",result,argv[3]); return 1; }
    if(!result) {
        assert(!(frame.user_rsp&15) && frame.cs==USER_CS && frame.ss==USER_DS && frame.rflags==0x202);
        assert(word(frame.user_rsp)==2);
        uint64_t at=frame.user_rsp+8;
        assert(byte(word(at))=='f'); at+=8;
        assert(byte(word(at))=='t'); at+=8;
        assert(!word(at)); at+=8;
        assert(byte(word(at))=='D'); at+=8;
        assert(!word(at)); at+=8;
        uint64_t aux[40]={0}, seen=0;
        while(word(at)) { uint64_t key=word(at); assert(key<40); aux[key]=word(at+8); seen|=1ULL<<key; at+=16; }
        assert(aux[6]==4096 && aux[3] && aux[4]==sizeof(struct program_header) && aux[5]);
        assert((seen&0x7800)==0x7800 && !aux[11] && !aux[12] && !aux[13] && !aux[14]);
        assert((seen&(1ULL<<23)) && !aux[23]);
        assert(aux[31] && byte(aux[31])=='/');
        assert(aux[25] && byte(aux[25])==0xa5 && byte(aux[25]+15)==0xa5);
        if(!strcmp(argv[4],"pie")) { assert(aux[7]==ELF_INTERP_BASE && aux[9]==ELF_PIE_BASE+4096 && frame.rip==ELF_INTERP_BASE+4096); }
        else if(!strcmp(argv[4],"exec")) { assert(aux[7]==ELF_INTERP_BASE && aux[9]==0x401000 && frame.rip==ELF_INTERP_BASE+4096); }
        else if(!strcmp(argv[4],"direct")) { assert(!aux[7] && frame.rip==ELF_PIE_BASE+4096 && aux[9]==frame.rip); }
        else { assert(!aux[7] && frame.rip==0x401000 && aux[9]==frame.rip); }
        assert(page(aux[9])->flags==(VM_READ|VM_EXEC));
        assert(page(frame.user_rsp)->flags==VM_RDWR);
        assert(page(aux[9]+4096)->flags==VM_RDWR && !byte(aux[9]+8192));
        assert(s.vm->brk_base==aux[9]+16384 && s.vm->brk_end==s.vm->brk_base);
        assert(s.vm->mmap_next>=0x100000000ULL);
        user_space_destroy(&s);
    } else assert(!s.root && !pages);
    assert(!allocations && !files && !mappings);
    return 0;
}
'''


def image(kind=3, base=0, interpreter=True):
    data = bytearray(0x3000)
    ph = [[1, 4, 0, base, 0, 0x1000, 0x1000, 0x1000],
          [1, 5, 0x1000, base + 0x1000, 0, 16, 16, 0x1000],
          [1, 6, 0x2000, base + 0x2000, 0, 0x100, 0x3000, 0x1000],
          [6, 4, 64, base + 64, 0, 0, 0, 8],
          [0x6474e551, 6, 0, 0, 0, 0, 0, 16],
          [7, 4, 0x2000, base + 0x2000, 0, 8, 16, 8],
          [0x6474e552, 4, 0x2000, base + 0x2000, 0, 0x80, 0x80, 1]]
    if kind == 3 or interpreter:
        ph.append([2, 6, 0x2020, base + 0x2020, 0, 32, 32, 8])
        struct.pack_into('<QQQQ', data, 0x2020, 21, 0, 0, 0)
    if interpreter:
        name = b'/lib/ld-test.so\0'
        data[0x800:0x800 + len(name)] = name
        ph.append([3, 4, 0x800, base + 0x800, 0, len(name), len(name), 1])
    ph[3][5] = ph[3][6] = len(ph) * 56
    ident = b'\x7fELF\x02\x01\x01' + bytes(9)
    struct.pack_into('<16sHHIQQQIHHHHHH', data, 0, ident, kind, 62, 1, base + 4096,
                     64, 0, 0, 64, 56, len(ph), 0, 0, 0)
    for i, p in enumerate(ph):
        struct.pack_into('<IIQQQQQQ', data, 64 + i * 56, *p)
    data[4096] = 0xc3
    data[8192] = 7
    return data


def alter(data, kind, field, value):
    data = bytearray(data)
    count = struct.unpack_from('<H', data, 56)[0]
    offsets = [0, 4, 8, 16, 24, 32, 40, 48]
    for i in range(count):
        off = 64 + i * 56
        if struct.unpack_from('<I', data, off)[0] == kind:
            struct.pack_into('<I' if field < 2 else '<Q', data, off + offsets[field], value)
            return data
    raise AssertionError(kind)


def main():
    compiler = shutil.which('gcc')
    if not compiler:
        raise SystemExit('Host loader checks require gcc')
    source = (ROOT / 'fs/binfmt_elf.c').read_text().split('/* Resolve bounded, executable #! chains')[0]
    with tempfile.TemporaryDirectory(prefix='coast-elf-loader-') as temp:
        work = Path(temp)
        for name in ('mm/pmm.h', 'descriptor_tables/gdt.h', 'tty.h', 'video/display.h',
                     'pty.h', 'sandbox.h', 'fs/unix_socket.h', 'fs/memfd.h', 'random.h'):
            path = work / 'include' / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include <sched.h>\n')
        for name in ('mm/user.h', 'uapi/abi.h'):
            path = work / 'include' / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text((ROOT / 'include' / name).read_text())
        (work / 'include/sched.h').write_text(SHIM)
        (work / 'harness.c').write_text(HARNESS.replace('LOADER_SOURCE', source))
        subprocess.run([compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        '-I' + str(work / 'include'), str(work / 'harness.c'), '-o', str(work / 'harness')],
                       check=True)
        interpreter_path = work / 'interpreter'
        interpreter_path.write_bytes(image(interpreter=False))
        cases = [('static', image(2, 0x400000, False), 0, 'static', {}),
                 ('pie', image(), 0, 'pie', {}),
                 ('exec', image(2, 0x400000), 0, 'exec', {}),
                 ('direct', image(interpreter=False), 0, 'direct', {}),
                 ('missing-interpreter', image(), 2, 'pie', {'MOCK_INTERPRETER': str(work / 'missing')}),
                 ('no-permission', image(), 13, 'pie', {'MOCK_NOEXEC': '1'}),
                 ('map-failure', image(), 12, 'pie', {'MOCK_MAP_LIMIT': '12'}),
                 ('read-failure', image(), 5, 'pie', {'MOCK_READ_LIMIT': '8'})]
        for name, kind, field, value in (
            ('wx', 1, 1, 7), ('bad-load-align', 1, 7, 3),
            ('past-eof', 1, 5, 0x4000), ('wrapping-address', 1, 3, 0xfffffffffffff000),
            ('exec-stack', 0x6474e551, 1, 7), ('tls-align', 7, 7, 3),
            ('tls-coverage', 7, 3, 0x60000000), ('dynamic-coverage', 2, 3, 0x60000000),
            ('dynamic-size', 2, 5, 17), ('relro-coverage', 0x6474e552, 3, 0x60000000),
            ('missing-phdr', 6, 0, 0), ('wrong-phdr', 6, 3, 128)):
            cases.append((name, alter(image(), kind, field, value), 8, 'pie', {}))
        bad = image(); bad[0x800] = ord('x')
        cases.append(('relative-interpreter', bad, 8, 'pie', {}))
        bad = image(); bad[bad.index(0, 0x800)] = ord('x')
        cases.append(('unterminated-interpreter', bad, 8, 'pie', {}))
        bad = image(); struct.pack_into('<QQQQ', bad, 0x2020, 21, 0, 21, 0)
        cases.append(('unterminated-dynamic', bad, 8, 'pie', {}))
        bad = image(); struct.pack_into('<Q', bad, 0x2020, 22)
        cases.append(('textrel', bad, 8, 'pie', {}))
        bad = image(); struct.pack_into('<Q', bad, 24, 0x2000)
        cases.append(('nonexec-entry', bad, 8, 'pie', {}))
        cases.append(('interp-overlap', image(2, 0x200000000), 8, 'exec', {}))
        nested = work / 'nested-interpreter'; nested.write_bytes(image())
        cases.append(('nested-interpreter', image(), 8, 'pie', {'MOCK_INTERPRETER': str(nested)}))
        env = os.environ.copy()
        env['ASAN_OPTIONS'] = 'detect_leaks=1:halt_on_error=1'
        for name, data, expected, mode, extra in cases:
            path = work / name
            path.write_bytes(data)
            case_env = dict(env, **extra)
            subprocess.run([str(work / 'harness'), str(path),
                            extra.get('MOCK_INTERPRETER', str(interpreter_path)), str(expected), mode],
                           check=True, env=case_env)
        print(f'PASS actual ELF loader: {len(cases)} valid/malformed inputs, auxv, W^X flags and rollback (ASan/UBSan)')


if __name__ == '__main__':
    main()

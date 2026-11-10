#!/usr/bin/env python3
"""Compile the actual patched musl ORIGIN helper with controlled syscall edges."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <elf.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
struct dso { char *rpath, *rpath_orig, *name; int kernel_mapped; };
static struct { int secure; size_t *auxv; } libc;
static int proc_error, canonical_failure, allocation_failure;
static unsigned reads, canonicalizations, allocations;
static const char *proc_value, *expected_execfn;
static ssize_t adapted_readlink(const char *name, char *out, size_t n) {
    assert(!strcmp(name,"/proc/self/exe") && n==512);
    reads++;
    if(proc_error) { errno=proc_error; return -1; }
    size_t size=strlen(proc_value); if(size>n) size=n; memcpy(out,proc_value,size); return size;
}
static char *adapted_realpath(const char *path,char *out) {
    assert(!out && !strcmp(path,expected_execfn)); canonicalizations++;
    if(canonical_failure) { errno=ENOMEM; return 0; }
    char *p=realpath(path,0); if(p) allocations++; return p;
}
static void *adapted_malloc(size_t n) {
    if(allocation_failure) { errno=ENOMEM; return 0; }
    void *p=malloc(n); if(p) allocations++; return p;
}
static void adapted_free(void *p) { if(p) { assert(allocations); allocations--; free(p); } }
#define readlink adapted_readlink
#define realpath adapted_realpath
#define malloc adapted_malloc
#define free adapted_free
ACTUAL_HELPERS
#undef readlink
#undef realpath
#undef malloc
#undef free

static void check(const char *execfn,const char *name,int error,int secure,int badcanon,
                  int badalloc,int expected_result,const char *expected,unsigned expected_realpath) {
    assert(!allocations); proc_error=error; canonical_failure=badcanon; allocation_failure=badalloc;
    reads=canonicalizations=0; expected_execfn=execfn;
    size_t aux[]={AT_EXECFN,(size_t)execfn,0,0};
    libc.auxv=aux; libc.secure=secure;
    struct dso p={.rpath_orig="$ORIGIN/lib",.name=(char *)name,.kernel_mapped=1};
    char buffer[512]; memset(buffer,0xcc,sizeof buffer);
    int result=fixup_rpath(&p,buffer,sizeof buffer);
    assert(result==expected_result && canonicalizations==expected_realpath);
    if(expected) { assert(p.rpath && !strcmp(p.rpath,expected)); adapted_free(p.rpath); }
    else assert(!p.rpath);
    assert(reads==(secure?0u:1u));
    assert(!allocations);
}

int main(int argc,char **argv) {
    assert(argc==5);
    const char *actual=argv[1], *alias=argv[2], *long_path=argv[3], *expected=argv[4];
    proc_value=actual;
    check(actual,"unrelated-argv0",ENOENT,0,0,0,0,expected,1);
    check(actual,"/caller/controlled/argv0",ENOTDIR,0,0,0,0,expected,1);
    check(alias,"different-argv0",ENOENT,0,0,0,0,expected,1);
    check(actual,"unrelated-argv0",EACCES,0,0,0,0,0,0);
    check(actual,"unrelated-argv0",ENOENT,1,0,0,0,0,0);
    check("relative-execfn","/absolute-but-fake-argv0",ENOENT,0,0,0,0,0,0);
    check("","/absolute-but-fake-argv0",ENOENT,0,0,0,0,0,0);
    check(long_path,"unrelated-argv0",ENOENT,0,0,0,0,0,1);
    check(actual,"unrelated-argv0",ENOENT,0,1,0,0,0,1);
    check(actual,"unrelated-argv0",ENOENT,0,0,1,-1,0,1);
    check("relative-execfn","unrelated-argv0",0,0,0,0,0,expected,0);
    puts("PASS actual musl ORIGIN helper: 11 cases, real 512-byte caller, auxv/argv0, canonical symlink, access/secure/length/allocation errors (ASan/UBSan)");
    return 0;
}
'''


def function(source, declaration):
    begin = source.index(declaration)
    return source[begin:source.index('\n}\n', begin) + 3]


def main():
    path = ROOT / 'build/dynamic-linker/musl-1.2.5/ldso/dynlink.c'
    if not path.is_file():
        raise SystemExit('Build the shared runtime first: make dynamic-linker')
    source = path.read_text()
    helpers = '\n\n'.join(function(source, name) for name in
                           ('static int search_vec(', 'static int fixup_rpath('))
    if 'realpath((char *)execfn, 0)' not in helpers:
        raise SystemExit('The built musl source does not contain the expected ORIGIN fallback')
    with tempfile.TemporaryDirectory(prefix='coast-rpath-') as temp:
        work = Path(temp)
        actual = work / 'actual/program'
        actual.parent.mkdir()
        actual.write_bytes(b'fixture')
        alias = work / 'elsewhere/link'
        alias.parent.mkdir()
        alias.symlink_to(actual)
        long_path = work
        for i in range(5):
            long_path /= str(i) + 'x' * 110
            long_path.mkdir()
        long_path /= 'program'
        long_path.write_bytes(b'fixture')
        assert len(str(long_path)) > 512
        (work / 'harness.c').write_text(HARNESS.replace('ACTUAL_HELPERS', helpers))
        subprocess.run([shutil.which('gcc') or 'gcc', '-std=gnu11', '-O1', '-g',
                        '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        str(work / 'harness.c'), '-o', str(work / 'harness')], check=True)
        env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:halt_on_error=1')
        subprocess.run([str(work / 'harness'), str(actual), str(alias), str(long_path),
                        str(actual.parent / 'lib')], check=True, env=env)


if __name__ == '__main__':
    main()

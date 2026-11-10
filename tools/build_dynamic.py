#!/usr/bin/env python3
"""Build the pinned musl loader and small ELF ABI fixtures in an isolated cache.

This leaves the established static musl/compiler/port caches intact. The only
musl changes adapt its virtual-address reservation to Coast's eager file
snapshots and enforce the supported x86-64/W^X profile before mapping a DSO.
"""
import argparse
import hashlib
import inspect
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/dynamic-linker'
SOURCE = BUILD / 'musl-1.2.5'
PREFIX = BUILD / 'musl'
RUNTIME = ROOT / 'userland/runtime'
FIXTURES = RUNTIME / 'usr/lib/coast/dynamic'
INPUT = ROOT / 'userland/dynamic'
ARCHIVE = ROOT / 'userland/vendor/musl-1.2.5.tar.gz'
DIGEST = 'a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4'
FLAGS = ['-O2', '-g', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
         '-fno-stack-protector', '-mno-avx', '-mno-avx2']
LINK = ['-Wl,-z,max-page-size=4096,-z,relro,-z,now,-z,noexecstack']


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError('Pinned musl patch no longer matches: ' + old[:80])
    return text.replace(old, new)


def patch_musl():
    path = SOURCE / 'ldso/dynlink.c'
    text = path.read_text()
    text = replace_once(text, 'static void *map_library(int fd, struct dso *dso)',
                        (INPUT / 'musl-loader-validation.h').read_text() + '\n\n'
                        'static void *map_library(int fd, struct dso *dso)')
    text = replace_once(text, '\tEhdr buf[(896+sizeof(Ehdr))/sizeof(Ehdr)];',
                        '\tstruct stat coast_stat;\n'
                        '\tif (fstat(fd, &coast_stat) || coast_stat.st_size < 0) return 0;\n'
                        '\tEhdr buf[(896+sizeof(Ehdr))/sizeof(Ehdr)];')
    text = replace_once(text,
        '\tif (l<sizeof *eh || (eh->e_type != ET_DYN && eh->e_type != ET_EXEC))',
        '\tif (l<sizeof *eh || memcmp(eh->e_ident, "\\177ELF", 4)\n'
        '\t    || eh->e_ident[EI_CLASS] != ELFCLASS64 || eh->e_ident[EI_DATA] != ELFDATA2LSB\n'
        '\t    || eh->e_ident[EI_VERSION] != EV_CURRENT || eh->e_version != EV_CURRENT\n'
        '\t    || eh->e_machine != EM_X86_64 || eh->e_ehsize != sizeof *eh\n'
        '\t    || eh->e_phentsize != sizeof(Phdr) || !eh->e_phnum || eh->e_phnum > 64\n'
        '\t    || eh->e_phoff > coast_stat.st_size\n'
        '\t    || eh->e_phnum * sizeof(Phdr) > coast_stat.st_size - eh->e_phoff\n'
        '\t    || (eh->e_type != ET_DYN && eh->e_type != ET_EXEC))')
    text = replace_once(text,
        '\t\tif (ph->p_type != PT_LOAD) continue;\n\t\tnsegs++;',
        '\t\tif (ph->p_type == PT_GNU_STACK && (ph->p_flags & PF_X)) goto noexec;\n'
        '\t\tif (ph->p_type != PT_LOAD) continue;\n'
        '\t\tif (!(ph->p_flags & PF_R) || (ph->p_flags & (PF_W|PF_X)) == (PF_W|PF_X)\n'
        '\t\t    || ph->p_filesz > ph->p_memsz || ph->p_offset > coast_stat.st_size\n'
        '\t\t    || ph->p_filesz > coast_stat.st_size - ph->p_offset\n'
        '\t\t    || ph->p_memsz > 128*1024*1024\n'
        '\t\t    || ph->p_vaddr > SIZE_MAX - ph->p_memsz - PAGE_SIZE\n'
        '\t\t    || ((ph->p_vaddr - ph->p_offset) & (PAGE_SIZE-1))\n'
        '\t\t    || (ph->p_memsz > ph->p_filesz && !(ph->p_flags & PF_W))) goto noexec;\n'
        '\t\tnsegs++;')
    text = replace_once(text, '\tif (!dyn) goto noexec;',
                        '\tif (!dyn || !coast_metadata_valid(fd, eh, ph0)) goto noexec;')
    text = replace_once(text, '\tmap_len = addr_max - addr_min + off_start;',
                        '\tmap_len = addr_max - addr_min;')
    text = replace_once(text, '\t\tl = readlink("/proc/self/exe", buf, buf_size);',
        '\t\tl = readlink("/proc/self/exe", buf, buf_size);\n'
        '\t\t/* Coast has no proc executable link. For the nonsecure\n'
        '\t\t * main, use its kernel-supplied absolute AT_EXECFN, resolving\n'
        '\t\t * symlinks without relying on caller-controlled argv[0]. */\n'
        '\t\tif (l == -1 && (errno == ENOENT || errno == ENOTDIR)) {\n'
        '\t\t\tsize_t execfn = 0;\n'
        '\t\t\tsearch_vec(libc.auxv, &execfn, AT_EXECFN);\n'
        '\t\t\tif (!execfn || *(char *)execfn != \'/\') return 0;\n'
        '\t\t\tchar *canonical = realpath((char *)execfn, 0);\n'
        '\t\t\tif (!canonical) return 0;\n'
        '\t\t\tl = strlen(canonical);\n'
        '\t\t\tif (l < buf_size) memcpy(buf, canonical, l+1);\n'
        '\t\t\tfree(canonical);\n'
        '\t\t\tif (l >= buf_size) return 0;\n'
        '\t\t}')
    start = text.index('\t/* The first time, we map too much, possibly even more than')
    end = text.index('\n\tif (map==MAP_FAILED)', start)
    text = text[:start] + (
        '\t/* Coast eagerly snapshots file mappings and rejects pages beyond\n'
        '\t * EOF. Reserve inaccessible anonymous pages, then map exactly the\n'
        '\t * file-backed portion of every LOAD and zero-backed BSS below. */\n'
        '\tif (!nsegs || map_len > 128*1024*1024) goto noexec;\n'
        '\tmap = mmap(eh->e_type == ET_DYN ? 0 : (void *)addr_min, map_len, PROT_NONE,\n'
        '\t\tMAP_PRIVATE|MAP_ANONYMOUS, -1, 0);') + text[end:]
    start = text.index('\t\t/* Reuse the existing mapping for the lowest-address LOAD */')
    end = text.index('\n\t\tif (ph->p_memsz > ph->p_filesz', start)
    text = text[:start] + (
        '\t\tsize_t file_max = ph->p_vaddr+ph->p_filesz+PAGE_SIZE-1 & -PAGE_SIZE;\n'
        '\t\tif (ph->p_filesz) {\n'
        '\t\t\tif (mmap_fixed(base+this_min, file_max-this_min, prot,\n'
        '\t\t\t    MAP_PRIVATE|MAP_FIXED, fd, off_start) == MAP_FAILED) goto error;\n'
        '\t\t} else if (this_max > this_min && mmap_fixed(base+this_min,\n'
        '\t\t    this_max-this_min, prot, MAP_PRIVATE|MAP_FIXED|MAP_ANONYMOUS,\n'
        '\t\t    -1, 0) == MAP_FAILED) goto error;') + text[end:]
    text = replace_once(text,
        '\t\tif (ph->p_memsz > ph->p_filesz && (ph->p_flags&PF_W)) {',
        '\t\tif (ph->p_filesz && ph->p_memsz > ph->p_filesz && (ph->p_flags&PF_W)) {')
    text = replace_once(text,
        '\t\t\tif (mprotect(map, map_len, PROT_READ|PROT_WRITE|PROT_EXEC)\n'
        '\t\t\t    && errno != ENOSYS)\n\t\t\t\tgoto error;\n\t\t\tbreak;',
        '\t\t\terrno = ENOEXEC; /* Text relocations violate the W^X profile. */\n'
        '\t\t\tgoto error;')
    path.write_text(text)


def run(name, args, cwd=None, env=None):
    log = BUILD / (name + '.log')
    print(name + ': ' + str(args[0]), flush=True)
    with log.open('wb') as output:
        result = subprocess.run([str(a) for a in args], cwd=cwd, env=env,
                                stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.STDOUT)
    if result.returncode:
        raise SystemExit(f'{name} failed ({result.returncode}); see {log}')


def build_musl(jobs):
    if hashlib.sha256(ARCHIVE.read_bytes()).hexdigest() != DIGEST:
        raise SystemExit('Pinned musl archive checksum mismatch')
    compiler = shutil.which('gcc')
    if not compiler:
        raise SystemExit('Shared musl requires the host x86-64 GCC')
    version = subprocess.check_output([compiler, '--version'], text=True).splitlines()[0]
    key = hashlib.sha256(inspect.getsource(patch_musl).encode() +
                         (INPUT / 'musl-loader-validation.h').read_bytes() + DIGEST.encode() +
                         str(ROOT).encode() + version.encode() +
                         b'shared-musl-O2-noavx-relro-now-noexecstack-v1').hexdigest()
    stamp = BUILD / 'musl.stamp'
    if not stamp.exists() or stamp.read_text() != key or not (PREFIX / 'lib/libc.so').is_file():
        if SOURCE.exists():
            shutil.rmtree(SOURCE)
        with tarfile.open(ARCHIVE) as archive:
            archive.extractall(BUILD, filter='data')
        patch_musl()
        env = os.environ.copy()
        for name in ('CC', 'CXX', 'CFLAGS', 'CPPFLAGS', 'LDFLAGS', 'LIBS', 'CONFIG_SITE',
                     'GCC_EXEC_PREFIX', 'COMPILER_PATH', 'LIBRARY_PATH'):
            env.pop(name, None)
        env['CC'] = compiler
        env['CFLAGS'] = '-O2 -g0 -fno-stack-protector -mno-avx -mno-avx2'
        env['LDFLAGS'] = '-Wl,-z,max-page-size=4096,-z,relro,-z,now,-z,noexecstack'
        run('musl-configure', ['./configure', f'--prefix={PREFIX}',
                              f'--syslibdir={PREFIX}/lib', '--enable-shared'], SOURCE, env)
        run('musl-build', ['make', f'-j{jobs}'], SOURCE, env)
        run('musl-install', ['make', '-j1', 'install'], SOURCE, env)
        stamp.write_text(key)
    specs = PREFIX / 'lib/musl-gcc.specs'
    text = specs.read_text().replace(str(PREFIX / 'lib/ld-musl-x86_64.so.1'),
                                    '/lib/ld-musl-x86_64.so.1')
    if text != specs.read_text():
        specs.write_text(text)


def package_runtime():
    (RUNTIME / 'usr/lib').mkdir(parents=True, exist_ok=True)
    (RUNTIME / 'lib').mkdir(parents=True, exist_ok=True)
    (RUNTIME / 'usr/bin').mkdir(parents=True, exist_ok=True)
    shutil.copy2(PREFIX / 'lib/libc.so', RUNTIME / 'usr/lib/libc.so')
    links = [('ld-musl-x86_64.so.1', 'libc.so')]
    if not (RUNTIME / 'lib').is_symlink():
        links.append(('libc.so', '../usr/lib/libc.so'))
    for name, target in links:
        link = RUNTIME / 'lib' / name
        if link.is_symlink() and os.readlink(link) == target:
            continue
        if link.exists() or link.is_symlink():
            raise SystemExit(f'Refusing to replace unexpected runtime file: {link}')
        link.symlink_to(target)
    for name in ('Scrt1.o', 'crti.o', 'crtn.o'):
        destination = RUNTIME / 'usr/lib' / name
        if not destination.exists():
            shutil.copy2(PREFIX / 'lib' / name, destination)
    shutil.copy2(INPUT / 'coast-dynamic.specs', RUNTIME / 'usr/lib/coast-dynamic.specs')
    for name in ('gcc-dynamic', 'ldd'):
        shutil.copy2(INPUT / name, RUNTIME / 'usr/bin' / name)
        (RUNTIME / 'usr/bin' / name).chmod(0o755)
        alias = ROOT / 'userland/bin' / name
        alias.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(INPUT / name, alias)
        alias.chmod(0o755)


def compile_fixture(output, *args):
    output.parent.mkdir(parents=True, exist_ok=True)
    cc = PREFIX / 'bin/musl-gcc'
    run('fixture-' + output.name, [cc, *FLAGS, *args, *LINK, '-o', output])


def mutate_elf(source, target, mode):
    data = bytearray(source.read_bytes())
    phoff = struct.unpack_from('<Q', data, 32)[0]
    phsize, count = struct.unpack_from('<HH', data, 54)
    changed = False
    if mode == 'span':
        load = next(phoff + i * phsize for i in range(count)
                    if struct.unpack_from('<I', data, phoff + i * phsize)[0] == 1)
        length = struct.unpack_from('<Q', data, load + 32)[0]
        original = bytes(data[:length])
        if len(data) < 8192 + length:
            data.extend(bytes(8192 + length - len(data)))
        data[8192:8192 + length] = original
        struct.pack_into('<Q', data, load + 8, 8192)
        slot = next(phoff + i * phsize for i in range(count)
                    if struct.unpack_from('<I', data, phoff + i * phsize)[0] == 0x6474e551)
        struct.pack_into('<IIQQQQQQ', data, slot, 1, 6, 0, (1 << 64) - 12288, 0, 0, 8191, 4096)
        changed = True
    for i in range(count):
        off = phoff + i * phsize
        kind, flags = struct.unpack_from('<II', data, off)
        if mode == 'load-overlap' and kind == 1 and flags & 1:
            struct.pack_into('<Q', data, off + 16, 0)
            changed = True
            break
        if mode == 'wx' and kind == 1 and flags & 1:
            struct.pack_into('<I', data, off + 4, flags | 2)
            changed = True
            break
        if mode == 'stack' and kind == 0x6474e551:
            struct.pack_into('<I', data, off + 4, flags | 1)
            changed = True
            break
        if mode == 'nested-interp' and kind == 0x6474e551:
            struct.pack_into('<I', data, off, 3)
            changed = True
            break
        if mode == 'dynamic' and kind == 2:
            struct.pack_into('<Q', data, off + 16, 0x70000000)
            changed = True
            break
        if mode == 'dynamic-null' and kind == 2:
            fileoff, filesz = struct.unpack_from('<Q', data, off + 8)[0], struct.unpack_from('<Q', data, off + 32)[0]
            for entry in range(fileoff, fileoff + filesz, 16):
                if not struct.unpack_from('<Q', data, entry)[0]:
                    struct.pack_into('<Q', data, entry, 21)
            changed = True
            break
    if not changed:
        raise RuntimeError('ELF mutation did not find its target: ' + mode)
    target.write_bytes(data)
    target.chmod(0o755)


def build_fixtures():
    key = hashlib.sha256(inspect.getsource(build_fixtures).encode() +
                         inspect.getsource(mutate_elf).encode() +
                         repr((FLAGS, LINK)).encode() +
                         (PREFIX / 'lib/libc.so').read_bytes() +
                         b''.join(p.read_bytes() for p in sorted(INPUT.iterdir()) if p.is_file())).hexdigest()
    stamp = BUILD / 'fixtures.stamp'
    if (stamp.exists() and stamp.read_text() == key and
            (FIXTURES / 'dynamic-check').is_file() and
            (ROOT / 'userland/bin/dynamic-driver-check').is_file()):
        print('dynamic fixtures: cached', flush=True)
        return
    FIXTURES.mkdir(parents=True, exist_ok=True)
    libraries = FIXTURES / 'lib'
    libraries.mkdir(exist_ok=True)
    compile_fixture(libraries / 'libdynamic-leaf.so', '-fPIC', '-shared', INPUT / 'leaf.c',
                    '-Wl,-soname,libdynamic-leaf.so')
    compile_fixture(libraries / 'libdynamic-middle.so', '-fPIC', '-shared', INPUT / 'middle.c',
                    '-L' + str(libraries), '-ldynamic-leaf', '-Wl,-rpath,$ORIGIN',
                    '-Wl,-soname,libdynamic-middle.so')
    compile_fixture(libraries / 'libdynamic-plugin.so', '-fPIC', '-shared', INPUT / 'plugin.c',
                    '-Wl,-soname,libdynamic-plugin.so')
    for suffix in ('a', 'b'):
        compile_fixture(libraries / ('libdynamic-offset-' + suffix + '.so'), '-fPIC', '-shared',
                        INPUT / 'plugin.c', '-Wl,-Ttext-segment=0x10000',
                        '-Wl,-soname,libdynamic-offset-' + suffix + '.so')
    compile_fixture(libraries / 'libdynamic-unresolved.so', '-fPIC', '-shared',
                    INPUT / 'unresolved.c', '-Wl,-soname,libdynamic-unresolved.so')
    common = [INPUT / 'check.c', '-L' + str(libraries), '-ldynamic-middle', '-ldynamic-leaf',
              '-Wl,-rpath,$ORIGIN/lib', '-Wl,-rpath-link,' + str(libraries),
              '-Wl,--export-dynamic', '-pthread', '-ldl']
    compile_fixture(FIXTURES / 'dynamic-check', '-fPIE', '-pie', *common)
    compile_fixture(FIXTURES / 'dynamic-exec-check', '-fno-pie', '-no-pie', *common)
    compile_fixture(FIXTURES / 'dynamic-transitive', '-fPIE', '-pie', INPUT / 'transitive.c',
                    '-L' + str(libraries), '-ldynamic-middle', '-Wl,-rpath,$ORIGIN/lib',
                    '-Wl,-rpath-link,' + str(libraries))
    alias = RUNTIME / 'usr/bin/dynamic-transitive-link'
    target = '../lib/coast/dynamic/dynamic-transitive'
    if alias.exists() or alias.is_symlink():
        if not alias.is_symlink() or os.readlink(alias) != target:
            raise SystemExit(f'Refusing to replace unexpected runtime alias: {alias}')
    else:
        alias.symlink_to(target)
    compile_fixture(ROOT / 'userland/bin/dynamic-driver-check', '-static', '-fno-pie', '-no-pie',
                    INPUT / 'driver.c')
    compile_fixture(FIXTURES / 'missing-library', '-fPIE', '-pie', INPUT / 'missing.c',
                    '-L' + str(libraries), '-ldynamic-leaf', '-Wl,-rpath,/missing-dynamic-libraries')
    compile_fixture(FIXTURES / 'missing-interpreter', '-fPIE', '-pie', INPUT / 'hello.c',
                    '-Wl,--dynamic-linker=/missing-dynamic-interpreter')
    mutate_elf(FIXTURES / 'dynamic-check', FIXTURES / 'bad-wx', 'wx')
    mutate_elf(FIXTURES / 'dynamic-check', FIXTURES / 'bad-stack', 'stack')
    mutate_elf(PREFIX / 'lib/libc.so', FIXTURES / 'nested-interpreter.so', 'nested-interp')
    compile_fixture(FIXTURES / 'bad-interpreter', '-fPIE', '-pie', INPUT / 'hello.c',
                    '-Wl,--dynamic-linker=/usr/lib/coast/dynamic/nested-interpreter.so')
    mutate_elf(libraries / 'libdynamic-plugin.so', libraries / 'libdynamic-wx.so', 'wx')
    mutate_elf(libraries / 'libdynamic-plugin.so', libraries / 'libdynamic-bad-metadata.so', 'dynamic')
    mutate_elf(libraries / 'libdynamic-plugin.so', libraries / 'libdynamic-no-null.so', 'dynamic-null')
    mutate_elf(libraries / 'libdynamic-plugin.so', libraries / 'libdynamic-bad-span.so', 'span')
    mutate_elf(libraries / 'libdynamic-plugin.so', libraries / 'libdynamic-overlap.so', 'load-overlap')
    (libraries / 'libdynamic-bad.so').write_bytes(b'This is not an ELF shared object.\n')
    # Keep source examples small and available to the genuine in-guest GCC.
    examples = RUNTIME / 'usr/src/coast/dynamic'
    examples.mkdir(parents=True, exist_ok=True)
    for path in INPUT.glob('*.c'):
        shutil.copy2(path, examples / path.name)
    shutil.copy2(INPUT / 'Makefile', examples / 'Makefile')
    manifest = {'musl_version': '1.2.5', 'musl_archive_sha256': DIGEST,
                'interpreter': '/lib/ld-musl-x86_64.so.1',
                'libc_sha256': hashlib.sha256((RUNTIME / 'usr/lib/libc.so').read_bytes()).hexdigest(),
                'fixtures': {p.relative_to(FIXTURES).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                             for p in sorted(FIXTURES.rglob('*')) if p.is_file()}}
    (BUILD / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    stamp.write_text(key)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--jobs', type=int, default=1)
    parser.add_argument('--runtime-only', action='store_true')
    args = parser.parse_args()
    if not 1 <= args.jobs <= 8:
        parser.error('--jobs must be between 1 and 8')
    BUILD.mkdir(parents=True, exist_ok=True)
    build_musl(args.jobs)
    package_runtime()
    if not args.runtime_only:
        build_fixtures()
    print('Built shared musl 1.2.5, native dynamic SDK and ELF ABI fixtures.', flush=True)


if __name__ == '__main__':
    main()

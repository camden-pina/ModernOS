#!/usr/bin/env python3
"""Reproducible offline ports of Vim, BusyBox, and GNU make to ModernOS.

Build prerequisites (static musl/ncurses) are provided by build_nano.py.
No Linux service or simulated filesystem is required at runtime. BusyBox's
shell intentionally excludes job control because process groups are absent.
"""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
VENDOR = ROOT / 'userland/vendor'
BUILD = ROOT / 'build/vendor'
MUSL = ROOT / 'build/native-musl'
LIBS = ROOT / 'build/native-libs'
PACKAGES = {
    'vim-9.2.0000.tar.gz': '875875fb5988af3db0726bef9b048a559a9563aa0ecca8240e82057e8e5941c3',
    'busybox-1.37.0.tar.bz2': '3311dff32e746499f4df0d5df04d7eb396382d7e108bb9250e7b519b837043a4',
    'make-4.4.1.tar.gz': 'dd16fb1d67bfab79a72f5e8390735c49e3e8e70b4945a15ab1f81ddb78658fb3',
}


def run(where, log, *command, env=None, input=None):
    with (BUILD / log).open('wb') as output:
        result = subprocess.run(command, cwd=where, env=env, input=input,
                                stdout=output, stderr=subprocess.STDOUT)
    if result.returncode:
        raise SystemExit(f'{command[0]} failed; see {BUILD / log}')


BUILD.mkdir(parents=True, exist_ok=True)

if not (MUSL / 'bin/musl-gcc').exists() or not (LIBS / 'lib/libncursesw.a').exists():
    raise SystemExit('Build bin/nano first to install the pinned static musl/ncurses toolchain.')
config = VENDOR / 'busybox-modernos.config'
key = hashlib.sha256(Path(__file__).read_bytes() + config.read_bytes() +
                     str(ROOT).encode() + ''.join(PACKAGES.values()).encode()).hexdigest()
stamp = BUILD / 'modernos-ports.stamp'
vim = BUILD / 'vim-9.2.0000'
busybox = BUILD / 'busybox-1.37.0'
make = BUILD / 'make-4.4.1'
binaries = (vim / 'src/vim', busybox / 'busybox', make / 'make')

for filename, digest in PACKAGES.items():
    if hashlib.sha256((VENDOR / filename).read_bytes()).hexdigest() != digest:
        raise SystemExit(f'Upstream source checksum mismatch: {VENDOR / filename}')

if not stamp.exists() or stamp.read_text() != key or not all(p.exists() for p in binaries):
    for filename in PACKAGES:
        target = BUILD / filename.split('.tar.')[0]
        if target.exists():
            shutil.rmtree(target)
        with tarfile.open(VENDOR / filename) as source:
            source.extractall(BUILD, filter='data')

    jobs = str(min(os.cpu_count() or 1, 8))
    env = os.environ.copy()
    for name in ('CFLAGS', 'CPPFLAGS', 'LDFLAGS', 'LIBS', 'CONFIG_SITE', 'CROSS_COMPILE'):
        env.pop(name, None)
    env.update(CC=str(MUSL / 'bin/musl-gcc'),
               CFLAGS='-O2 -std=gnu11 -fno-pie -fno-stack-protector -mno-avx -mno-avx2',
               CPPFLAGS=f'-I{LIBS}/include -I{LIBS}/include/ncursesw',
               LDFLAGS=f'-static -no-pie -L{LIBS}/lib')
    vim_env = env.copy()

    # The upstream timer_create branch uses SIGEV_THREAD/pthreads. Select its
    # unmodified SIGALRM/setitimer implementation, matching the target ABI.
    vim_env['vim_cv_timer_create_works'] = 'no'
    run(vim, 'vim-configure.log', './configure', '--prefix=/usr', '--with-features=normal',
        '--enable-gui=no', '--without-x', '--without-wayland', '--disable-xsmp',
        '--disable-channel', '--disable-terminal', '--disable-netbeans', '--disable-gpm',
        '--disable-selinux', '--disable-smack', '--disable-xattr', '--disable-acl',
        '--disable-nls', '--with-tlib=ncursesw', env=vim_env)
    run(vim, 'vim-build.log', 'make', '-j' + jobs, env=env)

    run(busybox, 'busybox-configure.log', 'make', 'allnoconfig')
    configuration = busybox / '.config'
    lines = configuration.read_text().splitlines()
    options = dict(line.split('=', 1) for line in config.read_text().splitlines()
                   if line.startswith('CONFIG_'))
    for i, line in enumerate(lines):
        name = line.split('=', 1)[0] if line.startswith('CONFIG_') else \
            line[2:-11] if line.startswith('# CONFIG_') and line.endswith(' is not set') else ''
        if name in options:
            lines[i] = name + '=' + options.pop(name)

    if options:
        raise SystemExit('Unknown BusyBox configuration options: ' + ', '.join(options))
    configuration.write_text('\n'.join(lines) + '\n')
    run(busybox, 'busybox-oldconfig.log', 'make', 'oldconfig', input=b'\n' * 1024)

    # Upstream patch ignores rename's result. Report replacement failure on
    # filesystems lacking that operation instead of claiming a patch succeeded.
    patch_source = busybox / 'editors/patch.c'
    patch_text = patch_source.read_text()
    assert patch_text.count('rename(TT.tempname, temp);') == 1
    patch_source.write_text(patch_text.replace('rename(TT.tempname, temp);',
                                               'if (rename(TT.tempname, temp))\n'
                                               '\t\t\t\tbb_perror_msg_and_die("cannot replace %s; edited data is in %s", temp, TT.tempname);'))
    run(busybox, 'busybox-build.log', 'make', '-j' + jobs, f'CC={env["CC"]}', 'HOSTCC=gcc',
        'CFLAGS=-O2 -std=gnu11 -fno-pie -fno-stack-protector -mno-avx -mno-avx2',
        'EXTRA_LDFLAGS=-static -no-pie')

    # ModernOS retains its interactive PID-1 shell. Recipes need the real POSIX
    # shell. This is the only upstream source change in the GNU make port.
    job = make / 'src/job.c'
    text = job.read_text()
    assert text.count('const char *default_shell = "/bin/sh";') == 2
    job.write_text(text.replace('const char *default_shell = "/bin/sh";',
                                'const char *default_shell = "/bin/ash";'))
    make_env = env.copy()

    # Named filesystem FIFOs are absent. Upstream pipe jobserver is supported.
    make_env['ac_cv_func_mkfifo'] = 'no'

    # Recipes receive private address spaces and admission-time CPU balancing.
    # The kernel retains vfork for musl spawn/GCC; make uses its fork fallback.
    make_env['ac_cv_func_vfork'] = 'no'
    run(make, 'make-configure.log', './configure', '--prefix=/usr', '--disable-nls',
        '--disable-posix-spawn', '--without-guile', env=make_env)
    run(make, 'make-build.log', 'make', '-j' + jobs, env=make_env)
    stamp.write_text(key)

bin_dir = ROOT / 'userland/bin'
bin_dir.mkdir(parents=True, exist_ok=True)
for source, name in zip(binaries, ('vim', 'busybox', 'make')):
    shutil.copy2(source, bin_dir / name)
runtime = ROOT / 'userland/runtime/usr/share/vim/vim92'
if runtime.exists():
    shutil.rmtree(runtime)
shutil.copytree(vim / 'runtime', runtime,
                ignore=shutil.ignore_patterns('.git*', 'Makefile', '*.info'))
licenses = VENDOR / 'licenses'
licenses.mkdir(exist_ok=True)

for source, name in ((vim / 'LICENSE', 'vim-LICENSE.txt'),
                     (vim / 'runtime/doc/uganda.txt', 'vim-uganda.txt'),
                     (busybox / 'LICENSE', 'busybox-LICENSE.txt'),
                     (make / 'COPYING', 'make-COPYING.txt')):
    shutil.copy2(source, licenses / name)

print('Built native Vim 9.2.0000, BusyBox 1.37.0 and GNU make 4.4.1 (static musl).')

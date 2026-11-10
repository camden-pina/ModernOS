#!/usr/bin/env python3
"""Build a fresh makepkg/fakeroot environment without an inherited runtime.

The shared musl loader and archive tools are separate from the static package
CLI. Only build outputs are writable during compilation; target ELFs never run
on Linux. Existing signing identities and repository services are not inputs.
"""
from build_state import project_lock
from build_state import atomic_write
import argparse
import fcntl
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

from build_desktop import verify_receipt
from fetch_sources import fetch, read_manifest
from linux_uapi import prepare
from source_archive import unpack
from source_tree import digest

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler-build', type=Path, required=True)
    parser.add_argument('--packages-build', type=Path, required=True)
    parser.add_argument('--program-build', type=Path, required=True)
    parser.add_argument('--output-root', type=Path, required=True)
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--reconfigure', action='store_true', help='accept builder-only fixes with unchanged pins and components')
    args = parser.parse_args()
    if not 1 <= args.jobs <= 24:
        parser.error('--jobs must be 1..24')
    receipts = {name: path.resolve(strict=True) for name, path in
                [('compiler', args.compiler_build), ('packages', args.packages_build), ('program', args.program_build)]}
    records = {name: verify_receipt(path) for name, path in receipts.items()}
    if records['compiler'].get('thread_model') != 'posix' or records['program'].get('compiler_build_sha256') != digest(receipts['compiler']):
        parser.error('Python and makepkg must use the same source-built POSIX musl compiler')
    dependency_receipt = receipts['packages'].with_name('DEPENDENCIES.json')
    dependencies = verify_receipt(dependency_receipt)
    for name, expected in dependencies['artifacts'].items():
        if digest(Path(name)) != expected:
            raise ValueError('Package dependency changed: ' + name)
    deps = Path(dependencies['prefix'])
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=True)
    lock = (out / '.build.lock').open('a')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    inputs = [Path(__file__), ROOT / 'tools/build_package_support.py', ROOT / 'tools/patch_fakeroot_unix.py',
              ROOT / 'tools/build_dynamic.py', ROOT / 'userland/dynamic/musl-loader-validation.h',
              ROOT / 'tools/build_extra_programs.py', ROOT / 'tools/build_grep.py',
              ROOT / 'tools/fetch_sources.py', ROOT / 'tools/source_archive.py', ROOT / 'tools/linux_uapi.py',
              ROOT / 'userland/vendor/sources.json', ROOT / 'userland/vendor/package-support-sources.json',
              ROOT / 'userland/vendor/arch-ports-sources.json', ROOT / 'userland/vendor/linux-ports-sources.json',
              ROOT / 'userland/vendor/extra-program-sources.json', ROOT / 'ports/gnu-runtime/grep-splice-fallback.patch',
              ROOT / 'userland/linux-ports/configure-strcasecmp-check.c',
              ROOT / 'userland/packages/config/makepkg.conf']
    original = {str(p.relative_to(ROOT)): digest(p) for p in inputs}
    identity = dict(source_sha256=original, components={name: dict(path=str(path), sha256=digest(path)) for name, path in receipts.items()})
    configuration = out / 'CONFIGURATION.json'
    if configuration.exists() and json.loads(configuration.read_text()) != identity:
        previous = json.loads(configuration.read_text())
        builders = {name for name in original if name.startswith('tools/build_')}
        def stable(record):
            return {**record, 'source_sha256': {k: v for k, v in record['source_sha256'].items() if k not in builders and k in previous['source_sha256']}}
        if not args.reconfigure or set(previous['source_sha256']) - set(original) or stable(previous) != stable(identity):
            raise ValueError('makepkg inputs changed; choose a fresh output directory or --reconfigure for builder-only fixes')
        history = out / 'configuration-history'
        history.mkdir(exist_ok=True)
        shutil.copyfile(configuration, history / (digest(configuration) + '.json'))
    atomic_write(configuration, json.dumps(identity, indent=2) + '\n')
    vendor, runtime, logs = out / 'vendor', out / 'runtime', out / 'logs'
    for path in (vendor, runtime, logs):
        path.mkdir(exist_ok=True)
    confinement = ['bwrap', '--unshare-all', '--die-with-parent', '--new-session',
                   '--ro-bind', '/', '/', '--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp',
                   '--bind', str(out), str(out)]
    cross = str(receipts['compiler'].parent / 'cross/bin/x86_64-linux-musl-')
    headers = prepare(out / 'linux-uapi')
    flags = '-O2 -g0 -std=gnu11 -fPIC -fno-stack-protector -mno-avx -mno-avx2 -idirafter ' + str(headers)
    environment = dict(PATH='/usr/bin:/bin', HOME='/tmp', TMPDIR='/tmp', LANG='C.UTF-8',
                       CC=cross + 'gcc', CXX=cross + 'g++', AR=cross + 'ar', RANLIB=cross + 'ranlib',
                       STRIP=cross + 'strip', CFLAGS=flags, CXXFLAGS=flags.replace(' -std=gnu11', ''),
                       LDFLAGS='-static -no-pie', PKG_CONFIG_LIBDIR='/nonexistent',
                       CONFIG_SITE='/dev/null', FORCE_UNSAFE_CONFIGURE='1')
    commands = []

    def run(name, command, cwd=out, env=None):
        command = list(map(str, command))
        commands.append(dict(name=name, argv=command, cwd=str(cwd)))
        print('makepkg: ' + name, flush=True)
        with (logs / (name + '.log')).open('w') as log:
            subprocess.run([*confinement, '--chdir', str(cwd), '--', *command], cwd=cwd,
                           env=env or environment, stdin=subprocess.DEVNULL,
                           stdout=log, stderr=subprocess.STDOUT, check=True)

    def child(name, tool, *arguments):
        command = [sys.executable, '-B', ROOT / 'tools' / tool, *map(str, arguments)]
        # Child builders establish their own read-only workspace confinement.
        print('makepkg: ' + name, flush=True)
        with (logs / (name + '.log')).open('w') as log:
            subprocess.run(list(map(str, command)), stdin=subprocess.DEVNULL,
                           stdout=log, stderr=subprocess.STDOUT, check=True)

    def copy(source, name):
        destination = runtime / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)

    def stage_tree(source):
        for path in sorted(source.rglob('*')):
            name = str(path.relative_to(source))
            if path.is_symlink():
                target = runtime / name
                target.parent.mkdir(parents=True, exist_ok=True)
                if not target.is_symlink():
                    target.symlink_to(os.readlink(path))
                elif os.readlink(target) != os.readlink(path):
                    raise ValueError('Conflicting alias: ' + name)
            elif path.is_file() and not name.endswith('.la'):
                copy(path, name)

    registry = read_manifest()
    # Reuse the existing audited musl loader patch, with all globals bound to
    # this component's output. Its runtime/fixture installer is never invoked.
    import build_dynamic as dynamic
    dynamic.BUILD, dynamic.SOURCE, dynamic.PREFIX = out / 'shared-musl', out / 'shared-musl/musl-1.2.5', out / 'shared-musl/prefix'
    dynamic.BUILD.mkdir(exist_ok=True)
    fetch(registry['musl-1.2.5.tar.gz'], vendor)
    dynamic.ARCHIVE, dynamic.run = vendor / 'musl-1.2.5.tar.gz', run
    dynamic.build_musl(args.jobs)
    specs = dynamic.PREFIX / 'lib/coast-dynamic.specs'
    specs.write_text('*self_spec:\n%<static %<no-pie %{!shared:%{!r:-pie}}\n\n')
    dynamic_receipt = out / 'SHARED-MUSL.json'
    atomic_write(dynamic_receipt, json.dumps(dict(prefix=str(dynamic.PREFIX), target_host_executed=False,
        source_sha256={name: value for name, value in original.items() if name in
                       ('tools/build_dynamic.py', 'userland/dynamic/musl-loader-validation.h')},
        source=registry['musl-1.2.5.tar.gz'], artifacts={str(p): digest(p) for p in sorted(dynamic.PREFIX.rglob('*'))
            if p.is_file() and not p.is_symlink()}), indent=2) + '\n')
    copy(dynamic.PREFIX / 'lib/libc.so', 'usr/lib/libc.so')
    loader = runtime / 'lib/ld-musl-x86_64.so.1'
    loader.parent.mkdir(exist_ok=True)
    if not loader.is_symlink():
        loader.symlink_to('../usr/lib/libc.so')
    copy(dynamic.SOURCE / 'COPYRIGHT', 'usr/share/licenses/shared-musl/COPYRIGHT')
    copy(dynamic.ARCHIVE, 'usr/src/coast/makepkg/' + dynamic.ARCHIVE.name)
    child('support', 'build_package_support.py', '--compiler-build', receipts['compiler'],
          '--dynamic-build', dynamic_receipt, '--dependencies-build', dependency_receipt,
          '--output-root', out / 'support', '--jobs', args.jobs,
          *[item for name in ('coreutils', 'findutils', 'file', 'fakeroot') for item in ('--package', name)])
    support = out / 'support/support-install'
    stage_tree(support)
    # Sources and notices accompany the installed GNU/fakeroot tools.
    support_pins = json.loads((ROOT / 'userland/vendor/package-support-sources.json').read_text())
    support_pins['fakeroot'] = json.loads((ROOT / 'userland/vendor/arch-ports-sources.json').read_text())['fakeroot']
    for name, pin in support_pins.items():
        copy(out / 'support/vendor' / pin['archive'], 'usr/src/coast/makepkg/' + pin['archive'])
        for notice in ('COPYING', 'COPYING.LIB', 'COPYRIGHT'):
            path = out / 'support' / pin['source'] / notice
            if path.is_file():
                copy(path, 'usr/share/licenses/makepkg/' + name + '/' + notice)

    pins = {name: registry[archive] for name, archive in [('gawk', 'gawk-5.4.0.tar.xz'), ('git', 'git-2.56.0.tar.xz')]}
    pins.update({p['name']: p for p in json.loads((ROOT / 'userland/vendor/linux-ports-sources.json').read_text())['packages']
                 if p['name'] in ('sed', 'diffutils')})
    for name, pin in pins.items():
        fetch(pin, vendor)
        source = out / pin['archive'].split('.tar.')[0]
        if not source.exists():
            unpack(vendor / pin['archive'], out)
        build = out / (name + '-build')
        build.mkdir(exist_ok=True)
        if name == 'git':
            options = ['prefix=/usr', 'uname_S=Coast', 'uname_M=x86_64', 'SHELL_PATH=/bin/sh',
                'PERL_PATH=/usr/bin/perl', 'PYTHON_PATH=/usr/bin/python3', 'CC=' + environment['CC'],
                'CFLAGS=' + flags, 'LDFLAGS=-static -no-pie', 'ZLIB_PATH=' + str(deps),
                'CURLDIR=' + str(deps), 'CURL_CONFIG=' + str(deps / 'bin/curl-config'),
                'CURL_LDFLAGS=' + subprocess.check_output([deps / 'bin/curl-config', '--static-libs'], text=True).strip(),
                'NO_PTHREADS=YesPlease', 'NO_MMAP=YesPlease', 'NO_GETTEXT=YesPlease', 'NO_PERL=YesPlease',
                'NO_PYTHON=YesPlease', 'NO_TCLTK=YesPlease', 'NO_RUST=YesPlease', 'NO_OPENSSL=YesPlease',
                'NO_EXPAT=YesPlease', 'NO_UNIX_SOCKETS=YesPlease', 'NO_REGEX=NeedsStartEnd',
                'HAVE_ALLOCA_H=YesPlease', 'HAVE_GETDELIM=YesPlease', 'HAVE_CLOCK_GETTIME=YesPlease',
                'HAVE_CLOCK_MONOTONIC=YesPlease', 'HAVE_DEV_TTY=YesPlease', 'CSPRNG_METHOD=getrandom',
                'INSTALL_SYMLINKS=YesPlease', 'NO_INSTALL_HARDLINKS=YesPlease']
            run('git-build', ['make', '-j' + str(args.jobs), *options, 'all'], source)
            run('git-install', ['make', *options, 'DESTDIR=' + str(runtime), 'install'], source)
        else:
            package_env = environment
            if name == 'diffutils':
                # This release's Autoconf probe has no working cross branch.
                # Pinned musl implements strcasecmp; install the same probe for
                # mandatory execution in Coast instead of running target code
                # on Linux during configure.
                package_env = {**environment, 'gl_cv_func_strcasecmp_works': 'yes'}
                probe = out / 'strcasecmp-check'
                run('strcasecmp-probe-build', [environment['CC'], *shlex.split(flags), '-static', '-no-pie',
                    ROOT / 'userland/linux-ports/configure-strcasecmp-check.c', '-o', probe])
                copy(probe, 'usr/share/coast/makepkg/strcasecmp-check')
            options = [source / 'configure', '--build=x86_64-pc-linux-gnu', '--host=x86_64-linux-musl',
                       '--prefix=/usr', '--disable-nls', '--disable-dependency-tracking']
            if name == 'gawk':
                options += ['--disable-extensions', '--disable-pma', '--disable-mpfr', '--without-readline',
                            '--without-libiconv-prefix', '--without-libintl-prefix']
            if not (build / 'Makefile').exists():
                run(name + '-configure', options, build, package_env)
            run(name + '-build', ['make', '-j' + str(args.jobs)], build, package_env)
            run(name + '-install', ['make', 'DESTDIR=' + str(runtime), 'install'], build, package_env)
        copy(vendor / pin['archive'], 'usr/src/coast/makepkg/' + pin['archive'])
        for notice in ('COPYING', 'COPYING.LIB', 'LICENSE'):
            if (source / notice).is_file():
                copy(source / notice, 'usr/share/licenses/makepkg/' + name + '/' + notice)

    # A dynamic executable is essential: static bsdtar cannot see LD_PRELOAD.
    pin = json.loads((ROOT / 'userland/vendor/arch-ports-sources.json').read_text())['libarchive']
    fetch(pin, vendor)
    source = out / pin['source']
    if not source.exists():
        unpack(vendor / pin['archive'], out)
    build = out / 'libarchive-build'
    build.mkdir(exist_ok=True)
    env = {**environment, 'CFLAGS': flags + ' -B' + str(dynamic.PREFIX / 'lib') + ' -specs=' + str(specs),
           'CPPFLAGS': '-I' + str(deps / 'include'),
           'LDFLAGS': '-L' + str(deps / 'lib') + ' -L' + str(dynamic.PREFIX / 'lib') + ' -Wl,-z,relro,-z,now,-z,noexecstack'}
    if not (build / 'Makefile').exists():
        run('libarchive-configure', [source / 'configure', '--build=x86_64-pc-linux-gnu', '--host=x86_64-linux-musl',
            '--prefix=/usr', '--disable-shared', '--enable-static', '--disable-dependency-tracking',
            '--without-bz2lib', '--without-lzma', '--without-lz4', '--without-lzo2', '--without-nettle',
            '--with-openssl', '--without-libb2', '--without-xml2', '--without-expat', '--disable-xattr',
            '--disable-acl', '--without-iconv', '--with-zlib', '--with-zstd'], build, env)
    run('libarchive-build', ['make', '-j' + str(args.jobs)], build, env)
    for name in ('bsdtar', 'bsdcpio', 'bsdcat', 'bsdunzip'):
        copy(build / name, 'usr/bin/' + name)
    copy(source / 'COPYING', 'usr/share/licenses/makepkg/libarchive/COPYING')
    copy(vendor / pin['archive'], 'usr/src/coast/makepkg/' + pin['archive'])
    child('grep', 'build_grep.py', '--compiler-prefix', cross, '--output-root', out / 'grep', '--jobs', args.jobs)
    grep = verify_receipt(out / 'grep/BUILD.json')
    for name, asset in grep['files'].items():
        copy(asset['path'], name)
    child('extras', 'build_extra_programs.py', '--compiler-build', receipts['compiler'],
          '--base-build', receipts['program'], '--output-root', out / 'extras', '--jobs', args.jobs,
          *[item for name in ('ninja', 'pkgconf', 'm4', 'patch', 'xz') for item in ('--package', name)])
    extras = verify_receipt(out / 'extras/BUILD.json')
    for name, asset in extras['files'].items():
        # Python is a separate pipeline component; avoid duplicate ownership.
        if name not in records['program']['files']:
            copy(asset['path'], name)
    aliases = {name: target for name, target in extras['aliases'].items() if name not in records['program']['aliases']}
    aliases.update({'usr/bin/bash': '../../bin/bash', 'usr/bin/awk': 'gawk', 'usr/bin/patch': 'linux-patch',
                    'usr/bin/xz': 'linux-xz', 'usr/bin/unxz': 'linux-xz', 'usr/bin/xzcat': 'linux-xz'})
    # BusyBox's configured ash has no kill applet. Use the real Bash builtin
    # for fakeroot's daemon cleanup, as the established repository does.
    kill = runtime / 'usr/bin/kill'
    kill.write_text('#!/usr/bin/bash\nkill "$@"\n')
    kill.chmod(0o755)
    copy(ROOT / 'userland/packages/config/makepkg.conf', 'etc/makepkg.conf')
    for path in sorted(runtime.rglob('*')):
        if path.is_symlink():
            target = os.readlink(path)
            if target.startswith('/'):
                target = os.path.relpath(target.lstrip('/'), str(path.relative_to(runtime).parent))
            aliases[str(path.relative_to(runtime))] = target
    files = {str(p.relative_to(runtime)): dict(path=str(p), sha256=digest(p), mode=p.stat().st_mode & 0o777)
             for p in sorted(runtime.rglob('*')) if p.is_file() and not p.is_symlink()}
    for name, asset in files.items():
        path = Path(asset['path'])
        if path.read_bytes()[:4] == b'\x7fELF':
            elf = subprocess.check_output(['readelf', '-h', '-lW', path], text=True)
            if 'Advanced Micro Devices X86-64' not in elf or ' RWE ' in elf:
                raise ValueError('Invalid makepkg environment ELF: ' + name)
    if any(digest(ROOT / name) != checksum for name, checksum in original.items()):
        raise ValueError('makepkg source inputs changed during compilation')
    record = dict(state='compiled; guest qualification pending', target_host_executed=False,
        files=files, aliases=aliases, commands=commands, confinement=confinement,
        source_sha256=original, compiler_build_sha256=digest(receipts['compiler']),
        packages_build_sha256=digest(receipts['packages']), program_build_sha256=digest(receipts['program']),
        components=identity['components'], shared_musl=dict(path=str(dynamic_receipt), sha256=digest(dynamic_receipt)))
    atomic_write(out / 'BUILD.json', json.dumps(record, indent=2) + '\n')
    print('Fresh makepkg environment compiled:', out, flush=True)


if __name__ == '__main__':
    with project_lock():
        main()

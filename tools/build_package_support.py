#!/usr/bin/env python3
"""Build the real GNU tools and fakeroot needed by upstream makepkg."""
from source_archive import unpack
from build_state import project_lock
from build_state import atomic_write
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request
from patch_fakeroot_unix import patch, patch_musl
from fetch_sources import fetch

ROOT = Path(__file__).resolve().parents[1]
REGISTRY = ROOT / 'userland/vendor/package-support-sources.json'


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-root', type=Path, default=ROOT / 'build/arch-ports-r1')
    parser.add_argument('--package', choices=['coreutils', 'findutils', 'file', 'bash', 'fakeroot'], action='append')
    parser.add_argument('--jobs', type=int, default=8)
    parser.add_argument('--compiler-build', type=Path)
    parser.add_argument('--dynamic-build', type=Path, help='fresh shared musl receipt')
    parser.add_argument('--dependencies-build', type=Path, help='fresh static package dependency receipt')
    args = parser.parse_args()
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=True)
    (out / 'vendor').mkdir(exist_ok=True)
    install = out / 'support-install'
    install.mkdir(exist_ok=True)
    sources = json.loads(REGISTRY.read_text())
    arch_sources = json.loads((ROOT / 'userland/vendor/arch-ports-sources.json').read_text())
    bash = next(p for p in json.loads((ROOT / 'userland/vendor/linux-ports-sources.json').read_text())['packages'] if p['name'] == 'bash')
    sources['bash'] = {**bash, 'source': 'bash-5.3-dynamic'}
    sources['fakeroot'] = arch_sources['fakeroot']
    compiler_receipt = args.compiler_build.resolve() if args.compiler_build else None
    if compiler_receipt:
        from build_desktop import verify_receipt
        compiler = verify_receipt(compiler_receipt)
        dynamic = verify_receipt(args.dynamic_build)
        dependencies = verify_receipt(args.dependencies_build)
        if compiler.get('thread_model') != 'posix':
            parser.error('Package support needs the POSIX musl compiler')
        for record in (dynamic, dependencies):
            for path, checksum in record['artifacts'].items():
                if sha(path) != checksum:
                    raise ValueError('Package support dependency changed: ' + path)
    elif args.dynamic_build or args.dependencies_build:
        parser.error('Fresh dependencies also require --compiler-build')
    cross = compiler_receipt.parent / 'cross/bin/x86_64-linux-musl-' if compiler_receipt else ROOT / 'build/compiler/cross/bin/x86_64-linux-musl-'
    web = Path(dynamic['prefix']) if compiler_receipt else ROOT / 'build/web/deps'
    prefix = out / 'prefix'
    if compiler_receipt:
        from linux_uapi import prepare
        headers = prepare(out / 'linux-uapi')
        for directory in ('lib', 'include'):
            (prefix / directory).mkdir(parents=True, exist_ok=True)
        for name in ('zlib.h', 'zconf.h'):
            shutil.copy2(Path(dependencies['prefix']) / 'include' / name, prefix / 'include' / name)
        shutil.copy2(Path(dependencies['prefix']) / 'lib/libz.a', prefix / 'lib/libz.a')
    else:
        headers = ROOT / 'build/qt6/port-r1/linux-uapi'
    flags = f'-O2 -g0 -std=gnu11 -fPIC -fno-stack-protector -mno-avx -mno-avx2 -B{web}/lib -specs={web}/lib/coast-dynamic.specs -idirafter {headers}'
    links = f'-L{prefix}/lib -L{web}/lib -Wl,-z,max-page-size=4096,-z,relro,-z,now,-z,noexecstack'
    env = dict(PATH='/usr/bin:/bin', HOME='/tmp', TMPDIR='/tmp', LANG='C.UTF-8',
               CC=str(cross) + 'gcc', AR=str(cross) + 'ar', RANLIB=str(cross) + 'ranlib',
               STRIP=str(cross) + 'strip', CC_FOR_BUILD='/usr/bin/cc', BUILD_CC='/usr/bin/cc',
               CFLAGS_FOR_BUILD='-O2 -std=gnu11', BUILD_CFLAGS='-O2 -std=gnu11', CFLAGS=flags,
               CPPFLAGS=f'-I{prefix}/include -idirafter {headers}', LDFLAGS=links,
               PKG_CONFIG_LIBDIR=str(prefix / 'lib/pkgconfig'), CONFIG_SITE='/dev/null')
    confinement = ['bwrap', '--unshare-all', '--die-with-parent', '--new-session', '--ro-bind', '/', '/',
                   '--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp', '--bind', str(out), str(out)]
    commands = []
    inputs = [Path(__file__), ROOT / 'tools/patch_fakeroot_unix.py', REGISTRY,
              ROOT / 'userland/vendor/arch-ports-sources.json', ROOT / 'userland/vendor/linux-ports-sources.json']
    original = {str(p.relative_to(ROOT)): sha(p) for p in inputs}

    def run(name, command, build, source, extra=None):
        command = list(map(str, command))
        commands.append(dict(name=name, command=command, cwd=str(build)))
        print(name, flush=True)
        with (out / (name + '.log')).open('wb') as log:
            subprocess.run([*confinement, '--ro-bind', str(source), str(source), '--chdir', str(build), '--', *command],
                           env={**env, **(extra or {})}, stdin=subprocess.DEVNULL,
                           stdout=log, stderr=subprocess.STDOUT, check=True)

    for name, package in sources.items():
        if args.package and name not in args.package:
            continue
        archive = out / 'vendor' / package['archive']
        fetch(package, archive.parent)
        assert sha(archive) == package['sha256']
        source = out / package['source']
        if name == 'bash' and not source.exists():
            temporary = out / 'bash-unpack'
            temporary.mkdir(exist_ok=True)
            unpack(archive, temporary, filter='data')
            (temporary / 'bash-5.3').rename(source)
            for record in bash['patches']:
                patch_file = ROOT / 'userland/vendor/linux-ports' / record['file']
                assert sha(patch_file) == record['sha256']
                with patch_file.open('rb') as stream:
                    subprocess.run(['bwrap', '--unshare-all', '--die-with-parent', '--ro-bind', '/', '/',
                                    '--bind', str(out), str(out), '--chdir', str(source), '--',
                                    '/usr/bin/patch', '-p0', '--forward'], stdin=stream, check=True, stdout=subprocess.DEVNULL)
        if not source.exists():
            unpack(archive, out, filter='data')
        if name=='bash':
            # Keep the upstream descriptor-duplication redirection fallback
            # while using anonymous pipes for process substitution. Native
            # applications do not yet have a general /dev/fd filesystem.
            path=source/'redir.c'
            text=path.read_text()
            if 'defined (COAST_BASH_FD_REDIRECT)' not in text:
                text=text.replace('#if !defined (HAVE_DEV_FD)',
                                  '#if !defined (HAVE_DEV_FD) || defined (COAST_BASH_FD_REDIRECT)')
            path.write_text(text)
        if name == 'fakeroot':
            patched = out / 'fakeroot-unix-2.1.4'
            if not patched.exists():
                shutil.copytree(source, patched)
                patch(patched)
            source = patched
            changed = patch_musl(source)
            if changed or not (source / 'configure').exists():
                print('fakeroot-autoreconf (host generator)', flush=True)
                with (out / 'fakeroot-autoreconf.log').open('wb') as log:
                    subprocess.run([*confinement, '--chdir', str(source), '--', '/usr/bin/autoreconf', '-fi'],
                                   env={'PATH': '/usr/bin:/bin', 'HOME': '/tmp', 'LANG': 'C.UTF-8'},
                                   stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, check=True)
        snapshot = {str(p.relative_to(source)): sha(p) for p in source.rglob('*') if p.is_file() and not p.is_symlink()}
        build = out / (name + '-support-build')
        build.mkdir(exist_ok=True)
        options = [source / 'configure', '--build=x86_64-pc-linux-gnu', '--host=x86_64-linux-musl',
                   '--prefix=/usr', '--disable-nls', '--disable-dependency-tracking']
        extra = {}
        if name == 'coreutils':
            options += ['--enable-single-binary=symlinks', '--without-gmp', '--without-libgmp', '--without-openssl',
                        '--without-selinux', '--disable-acl', '--disable-xattr', '--disable-libcap']
        if name == 'findutils':
            options += ['--without-selinux']
        if name == 'file':
            options += ['--disable-shared', '--enable-static', '--disable-libseccomp', '--disable-bzlib',
                        '--disable-xzlib', '--disable-zstdlib', '--disable-lzlib']
        if name == 'bash':
            options += ['--disable-readline', '--without-bash-malloc', '--without-installed-readline']
            extra['CPPFLAGS']=env['CPPFLAGS']+' -DCOAST_BASH_FD_REDIRECT=1'
        if name == 'fakeroot':
            options += ['--with-ipc=tcp', '--enable-shared', '--disable-static', '--libdir=/usr/lib/libfakeroot']
            extra['CPPFLAGS'] = env['CPPFLAGS'] + ' -include linux/capability.h'
        key = hashlib.sha256(json.dumps([list(map(str, options)), env, extra, snapshot]).encode()).hexdigest()
        stamp = build / '.configured'
        if not stamp.exists() or stamp.read_text() != key:
            run(name + '-support-configure', options, build, source, extra)
            run(name + '-support-clean', ['make', 'clean'], build, source, extra)
            stamp.write_text(key)
        arguments = ['make', '-j' + str(args.jobs), 'all']
        if name == 'file':
            arguments += ['FILE_COMPILE=/usr/bin/file']
        if name == 'fakeroot':
            arguments += ['faked_LDFLAGS=-pie']
        run(name + '-support-build', arguments, build, source, extra)
        install_args = ['make', 'install', 'DESTDIR=' + str(install)]
        if name == 'file':
            install_args += ['FILE_COMPILE=/usr/bin/file']
        run(name + '-support-install', install_args, build, source, extra)
        assert {str(p.relative_to(source)): sha(p) for p in source.rglob('*') if p.is_file() and not p.is_symlink()} == snapshot
        atomic_write(out / (name + '-SUPPORT-SOURCES.json'), json.dumps(snapshot, indent=2) + '\n')
    assert {str(p.relative_to(ROOT)): sha(p) for p in inputs} == original
    artifacts={str(p):sha(p) for p in install.rglob('*') if p.is_file() and not p.is_symlink()}
    atomic_write(out / 'SUPPORT-COMPILATION.json', json.dumps(dict(state='compiled; guest validation pending', sources=sources,
        source_sha256=original, commands=commands, confinement=confinement,
        target_host_executed=False, shared_musl_sha256=sha(web / 'lib/libc.so'),
        compiler_build_sha256=sha(compiler_receipt) if compiler_receipt else None,
        dynamic_build=dict(path=str(args.dynamic_build.resolve()), sha256=sha(args.dynamic_build)) if args.dynamic_build else None,
        dependency_receipt=dict(path=str(args.dependencies_build.resolve()), sha256=sha(args.dependencies_build)) if args.dependencies_build else None,
        artifacts=artifacts,dynamic_specs_sha256=sha(web / 'lib/coast-dynamic.specs')), indent=2) + '\n')


if __name__ == '__main__':
    with project_lock():
        main()

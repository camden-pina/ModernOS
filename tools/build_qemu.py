#!/usr/bin/env python3
"""Build QEMU and its static SDK for Coast, on Linux or natively in Coast."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import sys
import tarfile

from fetch_sources import fetch
from source_archive import unpack
from qemu_firmware import build as build_firmware

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ROOT / 'userland/vendor/qemu-sources.json'
SDK = '/usr/lib/coast/qemu-sdk'
FLAGS = '-O2 -g0 -fPIC -fno-stack-protector -mno-avx -mno-avx2 -Wa,-mrelax-relocations=no'


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-root', type=Path)
    parser.add_argument('--native', action='store_true', help='build and run build tools using the installed Coast compiler')
    parser.add_argument('--jobs', type=int, default=8)
    parser.add_argument('--dependencies-only', action='store_true')
    parser.add_argument('--install-runtime', action='store_true')
    parser.add_argument('--compiler-build', type=Path,
                        help='source-built POSIX musl GCC receipt for a fresh cross-build')
    args = parser.parse_args()
    if not 1 <= args.jobs <= 32:
        parser.error('--jobs must be 1..32')
    if args.native and platform.system() != 'Coast':
        parser.error('--native requires Coast; use the cross-build mode on Linux')
    if args.native and args.compiler_build:
        parser.error('--compiler-build is a Linux cross-build input')
    out = (args.output_root or ROOT / ('build/qemu-native' if args.native else 'build/qemu')).resolve()
    out.mkdir(parents=True, exist_ok=True)
    lock = (out / '.build.lock').open('a')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    packages = json.loads(SOURCES.read_text())
    vendor, prefix, runtime = out / 'vendor', out / 'prefix', out / 'runtime'
    logs = out / 'logs'
    for path in (vendor, prefix, runtime, logs, out / 'host/bin'):
        path.mkdir(parents=True, exist_ok=True)
    compiler_receipt = args.compiler_build.resolve() if args.compiler_build else None
    if compiler_receipt:
        from build_desktop import verify_receipt
        compiler = verify_receipt(compiler_receipt)
        if compiler.get('thread_model') != 'posix' or compiler.get('target_host_executed') is not False:
            parser.error('QEMU requires the source-built POSIX musl compiler')
    cross = ('/usr/bin/' if args.native else compiler_receipt.parent / 'cross/bin/x86_64-linux-musl-'
             if compiler_receipt else ROOT / 'build/compiler/cross/bin/x86_64-linux-musl-')
    if not Path(str(cross) + 'gcc').is_file():
        parser.error('Build the musl cross toolchain first: python3 tools/build_gcc.py')
    meson_module = out / 'meson-tools/host-tools/meson-1.10.2'
    if not args.native:
        from prepare_browser_host import prepare_archive, PACKAGES
        prepare_archive(next(p for p in PACKAGES if p['kind'] == 'wheel'), out / 'meson-tools')
    meson = out / 'host/bin/meson'
    meson.write_text('#!' + sys.executable + '\nimport sys\n' +
                     ('' if args.native else 'sys.path.insert(0, ' + repr(str(meson_module)) + ')\n') +
                     'from mesonbuild.mesonmain import main\n'
                     'raise SystemExit(main())\n')
    meson.chmod(0o755)
    pkgconfig = out / 'host/bin/target-pkg-config'
    pkgconfig.write_text('#!/bin/sh\nexec /usr/bin/pkg-config --static "$@"\n')
    pkgconfig.chmod(0o755)
    from linux_uapi import prepare as prepare_uapi
    headers = prepare_uapi(out / 'linux-uapi')
    flags = FLAGS + ' -std=gnu11 -idirafter ' + str(headers)
    env = dict(PATH=str(out / 'host/bin') + ':/usr/bin:/bin', HOME='/tmp', TMPDIR='/tmp',
               LANG='C.UTF-8', CC=str(cross) + 'gcc', CXX=str(cross) + 'g++',
               AR=str(cross) + 'ar', RANLIB=str(cross) + 'ranlib',
               STRIP=str(cross) + 'strip', CFLAGS=flags, CXXFLAGS=flags.replace(' -std=gnu11', ''),
               LDFLAGS='-static -no-pie -Wl,-z,noexecstack',
               PKG_CONFIG=str(pkgconfig), PKG_CONFIG_LIBDIR=str(prefix / 'lib/pkgconfig'),
               PKG_CONFIG_PATH='', CONFIG_SITE='/dev/null')
    if args.native:
        env['PYTHONPATH'] = str(ROOT / 'ports/qemu/python')
    confinement = [] if args.native else ['bwrap', '--unshare-all', '--die-with-parent', '--new-session',
                   '--ro-bind', '/', '/', '--proc', '/proc', '--dev', '/dev',
                   '--tmpfs', '/tmp', '--bind', str(out), str(out)]
    commands = []
    recipe = [Path(__file__), SOURCES, *sorted((ROOT / 'ports/qemu').rglob('*')),
              ROOT / 'tools/qemu_firmware.py', ROOT / 'tools/source_archive.py',
              ROOT / 'tools/linux_uapi.py', ROOT / 'tools/prepare_browser_host.py',
              ROOT / 'tools/fetch_sources.py']
    original = {str(p.relative_to(ROOT)): sha(p) for p in recipe if p.is_file()}
    identity = dict(source_sha256=original, compiler_build_sha256=sha(compiler_receipt) if compiler_receipt else None,
                    compiler_sha256=sha(env['CC']), native=args.native)
    configuration = out / 'CONFIGURATION.json'
    if configuration.exists() and json.loads(configuration.read_text()) != identity:
        raise ValueError('QEMU build inputs changed; choose a fresh output directory')
    configuration.write_text(json.dumps(identity, indent=2) + '\n')

    def run(name, command, cwd, extra=None):
        command = list(map(str, command))
        commands.append(dict(name=name, command=command, cwd=str(cwd)))
        print(name, flush=True)
        with (logs / (name + '.log')).open('w') as log:
            invocation = command if args.native else [*confinement, '--chdir', str(cwd), '--', *command]
            result = subprocess.run(invocation, cwd=cwd,
                                    env={**env, **(extra or {})}, stdin=subprocess.DEVNULL,
                                    stdout=log, stderr=subprocess.STDOUT)
        if result.returncode:
            raise RuntimeError(f'{name} failed ({result.returncode}); see {log.name}')

    def extract(name):
        record = packages[name]
        archive = vendor / record['archive']
        for directory in (ROOT / 'userland/vendor', ROOT / 'vendor', Path('/usr/src/coast/qemu/vendor'),
                          Path('/usr/src/coast/qemu-build/vendor')):
            cached = directory / record['archive']
            if not archive.exists() and cached.is_file() and sha(cached) == record['sha256']:
                shutil.copyfile(cached, archive)
        fetch(record, vendor)
        source = out / record['source']
        stamp = source / '.coast-source'
        if not stamp.is_file() or stamp.read_text() != record['sha256']:
            if source.exists():
                raise RuntimeError(f'Unrecognized source tree: {source}; use a fresh output root')
            # EDK2's host-only X11IncludeHack points into /usr/include.
            # It is unrelated to the shipped x86 BIOS and is never needed.
            def safe_member(member, destination):
                if name == 'qemu' and member.name.endswith('/EmulatorPkg/Unix/Host/X11IncludeHack'):
                    return None
                return tarfile.data_filter(member, destination)
            unpack(archive, out, filter=safe_member)
            stamp.write_text(record['sha256'])
        return source

    crossfile = out / 'musl.ini'
    crossfile.write_text('[binaries]\n' + '\n'.join(
        key + ' = ' + repr(value) for key, value in {
            'c': env['CC'], 'cpp': env['CXX'], 'ar': env['AR'],
            'strip': env['STRIP'], 'pkg-config': str(pkgconfig),
        }.items()) + '\n[host_machine]\nsystem = \'linux\'\n'
        "cpu_family = 'x86_64'\ncpu = 'x86_64'\nendian = 'little'\n"
        '[properties]\nneeds_exe_wrapper = true\n[built-in options]\n'
        f'c_args = {shlex.split(flags)!r}\ncpp_args = {shlex.split(flags)!r}\n'
        "c_link_args = ['-static', '-no-pie']\ncpp_link_args = ['-static', '-no-pie']\n")
    sdk_files = {}

    ninja_source = extract('ninja')
    if args.native:
        # Bootstrap a genuine Ninja executable using the native C++ compiler.
        # Its non-console children need sessions under Coast's job-control ABI.
        ninja_patch = ROOT / 'ports/qemu/ninja-session.patch'
        stamp = ninja_source / '.coast-session-patch'
        if not stamp.exists():
            run('ninja-patch', ['patch', '-p1', '-i', ninja_patch], ninja_source)
            stamp.write_text(sha(ninja_patch))
        if stamp.read_text() != sha(ninja_patch):
            raise ValueError('Ninja patch changed; use a fresh output directory')
        ninja_key = sha(ninja_patch) + sha(Path(env['CXX'])) + flags
        built = ninja_source / '.coast-built'
        if not built.is_file() or built.read_text() != ninja_key or not (ninja_source / 'ninja').is_file():
            run('ninja-bootstrap', [sys.executable, 'configure.py', '--bootstrap'], ninja_source,
                {'CXXFLAGS': flags + ' -D_GNU_SOURCE', 'LDFLAGS': '-static -no-pie'})
            built.write_text(ninja_key)
        shutil.copyfile(ninja_source / 'ninja', out / 'host/bin/ninja')
        (out / 'host/bin/ninja').chmod(0o755)
        run('ninja-native-check', [sys.executable, ROOT / 'ports/qemu/ninja_check.py',
                                 '--ninja', out / 'host/bin/ninja'], out)

    def stage(name, configure, source, targets=(), outputs=(), extra=None):
        build = out / (name + '-build')
        build.mkdir(exist_ok=True)
        package_identity = {key: value for key, value in packages[name].items() if key != 'signature'}
        key = hashlib.sha256(json.dumps([package_identity, list(map(str, configure)),
                flags, extra, sha(crossfile), [sha(p) for p in sorted(prefix.glob('lib/*.a'))],
                sha(ROOT / 'ports/qemu/COAST.patch') if name == 'qemu' else None],
                sort_keys=True).encode()).hexdigest()
        # Dependency archive identities must not make their own stage invalidate.
        # Only the configuration and upstream inputs determine dependency builds.
        if name != 'qemu':
            key = hashlib.sha256(json.dumps([packages[name], list(map(str, configure)),
                    flags, extra, sha(crossfile)], sort_keys=True).encode()).hexdigest()
        stamp = build / '.coast-built'
        inventory = build / 'SDK-FILES.json'
        known_paths = json.loads(inventory.read_text()) if inventory.is_file() else []
        before = {p.relative_to(prefix): sha(p) for p in prefix.rglob('*')
                  if p.is_file() and not p.is_symlink()}
        if not stamp.is_file() or stamp.read_text() != key or any(not p.is_file() for p in outputs):
            if (build / '.coast-configured').exists() and (build / '.coast-configured').read_text() != key:
                shutil.rmtree(build)
                build.mkdir()
            run(name + '-configure', configure, build, extra)
            (build / '.coast-configured').write_text(key)
            engine = ['ninja'] if (build / 'build.ninja').is_file() else ['make']
            run(name + '-build', [*engine, '-j' + str(args.jobs), *targets], build, extra)
            if name != 'qemu':
                run(name + '-install', [*engine, 'install'], build, extra)
            for output in outputs:
                if not output.is_file():
                    raise RuntimeError(f'Missing output: {output}')
            stamp.write_text(key)
        else:
            print(name + ': cached', flush=True)
        changed = [str(p.relative_to(prefix)) for p in prefix.rglob('*') if p.is_file()
                   and not p.is_symlink() and before.get(p.relative_to(prefix)) != sha(p)]
        if changed:
            inventory.write_text(json.dumps(sorted(set(known_paths) | set(changed)), indent=2) + '\n')
        if inventory.is_file():
            sdk_files[name] = json.loads(inventory.read_text())
        return build

    for name, options, library in [
        ('libffi', ['--disable-docs', '--disable-multi-os-directory', '--disable-exec-static-tramp'], 'libffi.a'),
        ('pcre2', ['--disable-jit', '--enable-unicode', '--disable-pcre2grep', '--disable-pcre2test'], 'libpcre2-8.a'),
    ]:
        source = extract(name)
        stage(name, [source / 'configure', '--host=x86_64-linux-musl',
              '--build=' + subprocess.check_output(['gcc', '-dumpmachine'], text=True).strip(),
              '--prefix=' + str(prefix), '--libdir=' + str(prefix / 'lib'),
              '--disable-shared', '--enable-static', '--disable-dependency-tracking', *options], source,
              outputs=[prefix / 'lib' / library])

    def meson_stage(name, options, library):
        source = extract(name)
        return stage(name, [meson, 'setup', out / (name + '-build'), source,
                     '--cross-file', crossfile, '--prefix', prefix, '--libdir', 'lib',
                     '--default-library=static', '--buildtype=release', '--wrap-mode=nofallback',
                     *options], source, outputs=[prefix / 'lib' / library])

    source = extract('zlib')
    stage('zlib', [source / 'configure', '--static', '--prefix=' + str(prefix),
                   '--libdir=' + str(prefix / 'lib')], source,
          outputs=[prefix / 'lib/libz.a'], extra={'CHOST': 'x86_64-linux-musl'})
    meson_stage('glib', ['-Dtests=false', '-Dinstalled_tests=false', '-Ddocumentation=false',
                '-Dintrospection=disabled', '-Dman-pages=disabled', '-Dselinux=disabled',
                '-Dlibmount=disabled', '-Dxattr=false', '-Dnls=disabled', '-Dsysprof=disabled',
                '-Dlibelf=disabled', '-Ddtrace=disabled', '-Dsystemtap=disabled',
                '-Dforce_posix_threads=true'], 'libglib-2.0.a')
    meson_stage('pixman', ['-Dtests=disabled', '-Ddemos=disabled', '-Dgtk=disabled',
                         '-Dlibpng=disabled'], 'libpixman-1.a')
    meson_stage('libslirp', [], 'libslirp.a')
    meson_stage('libucontext', ['-Dcpu=x86_64', '-Ddocs=false',
                '-Dc_args=' + repr([*shlex.split(flags), '-Wa,--noexecstack'])], 'libucontext_posix.a')
    (out / 'SDK.json').write_text(json.dumps(sdk_files, indent=2) + '\n')
    if args.dependencies_only:
        return

    source = extract('qemu')
    signature = packages['qemu']['signature']
    installed_signature = Path('/usr/src/coast/qemu/vendor') / signature['archive']
    if not (vendor / signature['archive']).exists() and installed_signature.is_file():
        shutil.copyfile(installed_signature, vendor / signature['archive'])
    fetch(signature, vendor)
    public_key = ROOT / 'ports/qemu/qemu-release-key.asc'
    run('qemu-release-key', ['gpg', '--no-options', '--batch', '--yes', '--dearmor',
                            '--output', out / 'release-key.gpg', public_key], out)
    run('qemu-release-signature', ['gpgv', '--keyring', out / 'release-key.gpg', '--status-fd', '1',
                                  vendor / signature['archive'], vendor / packages['qemu']['archive']], out)
    if 'VALIDSIG ' + signature['fingerprint'] + ' ' not in (logs / 'qemu-release-signature.log').read_text():
        raise RuntimeError('Unexpected QEMU release signing fingerprint')
    patch = ROOT / 'ports/qemu/COAST.patch'
    applied = source / '.coast-patch'
    if not applied.is_file():
        if patch.read_text().strip():
            run('qemu-patch', ['patch', '-p1', '-i', patch], source)
        applied.write_text(sha(patch))
    if applied.read_text() != sha(patch):
        raise RuntimeError('Port patch changed; use a new output root')
    # musl has no ucontext implementation and Coast has no sigaltstack syscall.
    # Select the real libucontext backend, including its signal-mask wrappers.
    context_link = '-Wl,--whole-archive,' + str(prefix / 'lib/libucontext_posix.a') + ',--no-whole-archive'
    options = [source / 'configure', '--prefix=/usr', '--libdir=/usr/lib',
               '--datadir=/usr/share', '--sysconfdir=/etc', '--localstatedir=/var',
               '--target-list=x86_64-softmmu', '--cross-prefix=' + str(cross),
               '--cc=' + env['CC'], '--cxx=' + env['CXX'],
               '--host-cc=/usr/bin/gcc' + (' -static -no-pie' if args.native else ''),
               '--python=' + sys.executable, '--static', '--disable-pie',
               '--without-default-features', '--enable-system', '--enable-tcg',
               '--enable-tcg-interpreter', '--with-coroutine=ucontext', '--enable-tools', '--enable-pixman',
               '--enable-slirp', '--enable-vnc', '--disable-werror',
               '--disable-docs', '--disable-download', '--disable-install-blobs',
               '--extra-cflags=' + flags,
               '--extra-ldflags=-static -no-pie -Wl,-z,noexecstack ' + context_link,
               '--with-pkgversion=Coast']
    build = stage('qemu', options, source,
                  targets=['qemu-system-x86_64', 'qemu-img', 'qemu-io'],
                  outputs=[out / 'qemu-build' / p for p in ('qemu-system-x86_64', 'qemu-img', 'qemu-io')])

    if runtime.exists():
        shutil.rmtree(runtime)
    runtime.mkdir()
    def copy(path, relative, mode=None):
        dest = runtime / relative
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, dest)
        dest.chmod(path.stat().st_mode & 0o777)
        if mode is not None:
            dest.chmod(mode)

    for name in ('qemu-system-x86_64', 'qemu-img', 'qemu-io'):
        binary = build / name
        elf = subprocess.check_output(['readelf', '-h', '-l', binary], text=True)
        if 'Advanced Micro Devices X86-64' not in elf or 'INTERP' in elf or ' RWE ' in elf:
            raise RuntimeError(f'Invalid static target ELF: {binary}')
        copy(binary, 'usr/bin/' + name, 0o755)
    # Build the BIOS used by nested Coast and its VGA/boot ROMs from the
    # bundled sources, rather than inheriting those release binary blobs.
    firmware = build_firmware(source, out / 'firmware', run, cross, sys.executable,
                              args.jobs, native=args.native)
    for name in ('bios.bin', 'bios-256k.bin', 'bios-microvm.bin', 'vgabios-stdvga.bin',
                 'vgabios-cirrus.bin', 'vgabios-virtio.bin', 'kvmvapic.bin', 'linuxboot.bin',
                 'linuxboot_dma.bin', 'multiboot.bin', 'pvh.bin'):
        copy(firmware[name], 'usr/share/qemu/' + name)
    for pattern in ('*.rom', 'vgabios*.bin'):
        for path in sorted((source / 'pc-bios').glob(pattern)):
            copy(firmware.get(path.name, path), 'usr/share/qemu/' + path.name)
    for name, path in firmware.items():
        copy(path, 'usr/share/qemu/' + name)
    for path in sorted((source / 'pc-bios/keymaps').iterdir()):
        if path.is_file():
            copy(path, 'usr/share/qemu/keymaps/' + path.name)
    copy(source / 'pc-bios/README', 'usr/share/licenses/qemu-port/firmware/README')
    for name in ('COPYING', 'COPYING.LESSER'):
        copy(source / 'roms/seabios' / name, 'usr/share/licenses/qemu-port/firmware/seabios-' + name)
    for name, paths in sdk_files.items():
        for relative in paths:
            if not relative.startswith(('include/', 'lib/')) or relative.endswith('.la'):
                continue
            copy(prefix / relative, SDK.lstrip('/') + '/' + relative)
            target = runtime / SDK.lstrip('/') / relative
            if relative.endswith('.pc'):
                target.write_text(target.read_text().replace(str(prefix), SDK))
    for name, record in packages.items():
        source = out / record['source']
        candidates = ['COPYING', 'COPYING.LIB', 'LICENSE', 'LICENCE.md', 'COPYING.LESSER',
                      'COPYING.LGPLv2.1', 'COPYING.GPLv2', 'README']
        for candidate in candidates:
            if (source / candidate).is_file():
                copy(source / candidate, 'usr/share/licenses/qemu-port/' + name + '/' + candidate)
        copy(vendor / record['archive'], 'usr/src/coast/qemu/vendor/' + record['archive'])
    for path in [Path(__file__), SOURCES, patch, public_key, ROOT / 'Documentation/qemu.md',
                 ROOT / 'tools/fetch_sources.py', ROOT / 'tools/source_archive.py',
                 ROOT / 'tools/linux_uapi.py', ROOT / 'tools/prepare_browser_host.py',
                 ROOT / 'tools/qemu_firmware.py', ROOT / 'ports/qemu/boot_coast.py',
                 ROOT / 'ports/qemu/ninja-session.patch',
                 ROOT / 'ports/qemu/ninja_check.py',
                 ROOT / 'ports/qemu/python/sitecustomize.py',
                 ROOT / 'ports/qemu/check.py', ROOT / 'ports/qemu/boot.S',
                 ROOT / 'ports/qemu/boot.ld', ROOT / 'ports/qemu/context-check.c',
                 ROOT / 'ports/qemu/sdk-check.c']:
        copy(path, 'usr/src/coast/qemu/' + str(path.relative_to(ROOT)))
    copy(vendor / signature['archive'], 'usr/src/coast/qemu/vendor/' + signature['archive'])
    copy(ROOT / 'ports/qemu/check.py', 'usr/share/coast/qemu/check.py')
    copy(ROOT / 'ports/qemu/boot_coast.py', 'usr/share/coast/qemu/boot_coast.py')
    copy(ROOT / 'ports/qemu/sdk-check.c', 'usr/share/coast/qemu/sdk-check.c')
    run('context-check-build', [env['CC'], *shlex.split(flags), '-Wall', '-Wextra', '-Werror',
        '-static', '-no-pie', ROOT / 'ports/qemu/context-check.c',
        '-Wl,--whole-archive', prefix / 'lib/libucontext_posix.a', '-Wl,--no-whole-archive',
        '-Wl,-z,noexecstack', '-o', out / 'context-check'], out)
    copy(out / 'context-check', 'usr/share/coast/qemu/context-check', 0o755)
    # Build a real bootable 16-bit serial guest; its execution is tested in Coast.
    run('boot-assemble', ['/usr/bin/as', '--32', ROOT / 'ports/qemu/boot.S', '-o', out / 'boot.o'], out)
    run('boot-link', ['/usr/bin/ld', '-m', 'elf_i386', '-T', ROOT / 'ports/qemu/boot.ld',
                     out / 'boot.o', '-o', out / 'boot.img'], out)
    if (out / 'boot.img').stat().st_size != 512 or (out / 'boot.img').read_bytes()[-2:] != b'\x55\xaa':
        raise RuntimeError('Invalid BIOS boot sector')
    copy(out / 'boot.img', 'usr/share/coast/qemu/boot.img')
    files = {str(p.relative_to(runtime)): dict(path=str(p), sha256=sha(p),
                 mode=0o755 if p.stat().st_mode & 0o111 else 0o644)
             for p in sorted(runtime.rglob('*')) if p.is_file()}
    if any(sha(ROOT / name) != value for name, value in original.items()):
        raise ValueError('QEMU build sources changed during compilation')
    record = dict(state='compiled; guest validation pending', target_host_executed=args.native,
                  build_mode='native-coast' if args.native else 'linux-cross',
                  packages=packages, files=files, sdk=sdk_files, commands=commands,
                  options=list(map(str, options)), cflags=flags, confinement=confinement,
                  source_sha256=original, compiler_build_sha256=identity['compiler_build_sha256'],
                  compiler_sha256=sha(env['CC']), emulator='x86_64-softmmu', backend='TCI')
    record['source_built_firmware'] = {name: sha(path) for name, path in firmware.items()}
    if args.native:
        record['native_tools'] = {'ninja': dict(sha256=sha(out / 'host/bin/ninja'),
            version=packages['ninja']['version'], patch_sha256=sha(ROOT / 'ports/qemu/ninja-session.patch'))}
    record['release_supplied_firmware'] = sorted(p.name for p in (runtime / 'usr/share/qemu').iterdir()
                                                if p.is_file() and p.name not in firmware)
    (out / 'BUILD.json').write_text(json.dumps(record, indent=2) + '\n')
    if args.install_runtime:
        for relative, asset in files.items():
            if relative.startswith('usr/src/'):
                continue
            dest = ROOT / 'userland/runtime' / relative
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(asset['path'], dest)
    print('QEMU and SDK compiled; validate inside Coast with tools/test_qemu.py', flush=True)


if __name__ == '__main__':
    main()

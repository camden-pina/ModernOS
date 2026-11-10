#!/usr/bin/env python3
"""Build the Chromium/Code OSS bridge and Wayland SDK from pinned C sources."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tarfile

from fetch_sources import fetch, read_manifest
from prepare_browser_host import PACKAGES, prepare_archive

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-root', type=Path, default=ROOT / 'build/wayland-bridge')
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--cc', type=Path, help='source-built static musl compiler (for example the native base musl-gcc)')
    parser.add_argument('--gnu-sdk', type=Path, help='source-built application SDK for the GIO trash helper')
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=True)
    cross = ROOT / 'build/compiler/cross/bin/x86_64-linux-musl-'
    compiler = args.cc.resolve(strict=True) if args.cc else Path(str(cross) + 'gcc')
    if not compiler.is_file():
        parser.error('Build the musl cross compiler first: make -C userland runtime/usr/bin/gcc')
    cache = ROOT / 'build/modern-browser'
    meson_package = next(p for p in PACKAGES if p['kind'] == 'wheel')
    prepare_archive(meson_package, cache)
    meson_module = cache / 'host-tools' / meson_package['directory']
    hostbin = out / 'host/bin'
    hostbin.mkdir(parents=True, exist_ok=True)
    meson = hostbin / 'meson'
    meson.write_text('#!' + sys.executable + '\nimport sys\nsys.path.insert(0, ' + repr(str(meson_module))
                    + ')\nfrom mesonbuild.mesonmain import main\nraise SystemExit(main())\n')
    meson.chmod(0o755)
    conf = ['bwrap', '--unshare-all', '--die-with-parent', '--new-session', '--ro-bind', '/', '/',
            '--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp', '--bind', str(out), str(out)]
    environment = dict(PATH=str(hostbin) + ':/usr/bin:/bin', HOME='/tmp', TMPDIR='/tmp', LANG='C.UTF-8')
    commands = []

    def run(name, command, cwd=out, extra=None):
        command = list(map(str, command))
        commands.append(dict(name=name, command=command))
        print(name, flush=True)
        with (out / (name + '.log')).open('w') as log:
            subprocess.run([*conf, '--chdir', str(cwd), '--', *command],
                env={**environment, **(extra or {})}, stdin=subprocess.DEVNULL,
                stdout=log, stderr=subprocess.STDOUT, check=True)

    source_records = {'wayland': read_manifest()['wayland_1.21.0.orig.tar.gz'],
                      'libffi': json.loads((ROOT / 'userland/vendor/qemu-sources.json').read_text())['libffi']}
    vendor = out / 'vendor'
    vendor.mkdir(exist_ok=True)
    for name, record in source_records.items():
        cached = ROOT / 'userland/vendor' / record['archive']
        if cached.is_file() and not (vendor / record['archive']).exists() and sha(cached) == record['sha256']:
            shutil.copy2(cached, vendor / record['archive'])
        fetch(record, vendor)
        source = out / ('wayland-1.21.0' if name == 'wayland' else 'libffi-3.5.2')
        if not source.exists():
            with tarfile.open(vendor / record['archive']) as archive:
                archive.extractall(out, filter='data')
    source, prefix, host = out / 'wayland-1.21.0', out / 'sdk', out / 'host'
    flags = '-std=gnu11 -O2 -g0 -fPIC -fno-stack-protector -mno-avx -mno-avx2'
    ar = 'ar' if args.cc else str(cross) + 'ar'
    ranlib = 'ranlib' if args.cc else str(cross) + 'ranlib'
    strip = 'strip' if args.cc else str(cross) + 'strip'
    target_env = dict(CC=str(compiler), AR=ar, RANLIB=ranlib, CFLAGS=flags, LDFLAGS='-static -no-pie')
    ffi_build = out / 'ffi-build'
    ffi_build.mkdir(exist_ok=True)
    if not (prefix / 'lib/libffi.a').is_file():
        run('ffi-configure', [out / 'libffi-3.5.2/configure', '--host=x86_64-linux-musl',
            '--build=' + subprocess.check_output(['gcc', '-dumpmachine'], text=True).strip(),
            '--prefix=' + str(prefix), '--libdir=' + str(prefix / 'lib'), '--disable-shared', '--enable-static',
            '--disable-docs', '--disable-multi-os-directory', '--disable-exec-static-tramp'], ffi_build, target_env)
        run('ffi-build', ['make', '-j' + str(args.jobs)], ffi_build, target_env)
        run('ffi-install', ['make', 'install'], ffi_build, target_env)
    common = ['--buildtype=release', '--default-library=static', '-Dtests=false',
              '-Ddocumentation=false', '-Ddtd_validation=false', '--wrap-mode=nofallback']
    if not (out / 'scanner-build/build.ninja').exists():
        run('scanner-configure', [meson, 'setup', out / 'scanner-build', source, '--prefix', host,
                                  '--libdir=lib', '-Dlibraries=false', *common])
    run('scanner-build', ['ninja', '-C', out / 'scanner-build', '-j' + str(args.jobs)])
    run('scanner-install', ['ninja', '-C', out / 'scanner-build', 'install'])
    native = out / 'native.ini'
    native.write_text("[binaries]\nc = 'gcc'\ncpp = 'g++'\npkg-config = ['env', 'PKG_CONFIG_LIBDIR="
                      + str(host / 'lib/pkgconfig') + ":/usr/lib/pkgconfig:/usr/share/pkgconfig:/usr/lib/x86_64-linux-gnu/pkgconfig', 'pkg-config']\n")
    crossfile = out / 'cross.ini'
    crossfile.write_text('[binaries]\n' + '\n'.join(k + ' = ' + repr(v) for k, v in {
        'c': str(compiler), 'ar': ar, 'strip': strip,
        'pkg-config': ['env', 'PKG_CONFIG_LIBDIR=' + str(prefix / 'lib/pkgconfig'), 'pkg-config', '--static'],
        }.items()) + "\n[host_machine]\nsystem = 'linux'\ncpu_family = 'x86_64'\ncpu = 'x86_64'\nendian = 'little'\n"
        + '[properties]\nneeds_exe_wrapper = true\n[built-in options]\nc_args = ' + repr(shlex.split(flags))
        + "\nc_link_args = ['-static', '-no-pie']\n")
    if not (out / 'wayland-build/build.ninja').exists():
        run('wayland-configure', [meson, 'setup', out / 'wayland-build', source, '--prefix', prefix,
            '--libdir=lib', '--cross-file', crossfile, '--native-file', native, '-Dscanner=false', *common])
    run('wayland-build', ['ninja', '-C', out / 'wayland-build', '-j' + str(args.jobs)])
    run('wayland-install', ['ninja', '-C', out / 'wayland-build', 'install'])
    generated = out / 'generated'
    generated.mkdir(exist_ok=True)
    keymap = (ROOT / 'ports/wayland/keymap-us.xkb').read_text()
    (generated / 'keymap-us.h').write_text('/* Fixed US XKB map; see ports/wayland/xkeyboard-config-COPYING. */\n'
        'static const char k1_keymap[] = ' + json.dumps(keymap) + ';\n'
        + ''.join(f'#define K1_XKB_{name} (1u << {index})\n' for name, index in
                  [('Shift', 0), ('Lock', 1), ('Control', 2), ('Mod1', 3), ('Mod2', 4), ('Mod4', 6)]))
    scanner = hostbin / 'wayland-scanner'
    for name, record in json.loads((ROOT / 'ports/wayland/SOURCES.json').read_text()).items():
        xml = ROOT / 'ports/wayland' / name
        if sha(xml) != record['SHA256']:
            raise ValueError('Protocol source changed: ' + name)
        for mode, suffix in [('server-header', '-server-protocol.h'), ('private-code', '-protocol.c')]:
            run(name + '-' + mode, [scanner, mode, xml, generated / (xml.stem + suffix)])
    bridge = ROOT / 'userland/electron-wayland'
    cc = [str(compiler), *shlex.split(flags), '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
          '-static', '-no-pie', '-Wl,-z,noexecstack', '-I' + str(bridge), '-I' + str(generated),
          '-I' + str(source / 'src'), '-I' + str(out / 'wayland-build/src'),
          '-I' + str(prefix / 'include'), '-idirafter', str(ROOT / 'ports/linux-uapi/include'),
          '-idirafter', str(ROOT / 'include')]
    libraries = subprocess.check_output(['env', 'PKG_CONFIG_LIBDIR=' + str(prefix / 'lib/pkgconfig'),
                                         'pkg-config', '--static', '--libs', 'wayland-server'], text=True)
    run('bridge', [*cc, '-DCODEOSS_WORKBENCH=1', '-include', bridge / 'readonly-shm.h',
        bridge / 'native-surface.c', bridge / 'readonly-shm.c', *sorted(generated.glob('*-protocol.c')),
        *shlex.split(libraries), '-pthread', '-o', out / 'chromium-wayland'])
    for name in ['g2-wayland-launch', 'seccomp-check', 'pixel-window-check', 'private-stream-check',
                 'file-copy-check', 'inotify-check', 'trash-put', 'private-pty-check', 'read-prefix-check',
                 'null-device-check', 'native-node-launch-check', 'private-loopback-check', 'namespace-check']:
        extra = []
        if name == 'trash-put':
            if args.gnu_sdk:
                sdk = args.gnu_sdk.resolve(strict=True)
                record = json.loads((sdk / 'APPLICATION-SDK.json').read_text())
                for relative, expected in record['source_sha256'].items():
                    if sha(ROOT / relative) != expected:
                        raise ValueError('Application SDK recipe changed: ' + relative)
                for relative, asset in record['files'].items():
                    if sha(sdk / 'sysroot' / relative) != asset['sha256']:
                        raise ValueError('Application SDK library changed: ' + relative)
                run(name, [sdk / 'bin/gcc', '-O2', '-g0', '-Wl,-z,relro,-z,now,-z,noexecstack',
                    bridge / (name + '.c'), '-l:libgio-2.0.so.0', '-l:libgobject-2.0.so.0',
                    '-l:libglib-2.0.so.0', '-o', out / name])
                continue
            # The real desktop trash helper uses GIO, shared with the QEMU SDK.
            sdk = ROOT / 'build/qemu/prefix/lib/pkgconfig'
            if not (sdk / 'gio-2.0.pc').exists():
                parser.error('Build the GIO dependency SDK first: make qemu')
            extra = shlex.split(subprocess.check_output(['env', 'PKG_CONFIG_LIBDIR=' + str(sdk),
                'pkg-config', '--static', '--libs', 'gio-2.0'], text=True))
        run(name, [*cc, '-pthread', bridge / (name + '.c'), *extra, '-o', out / name])
    run('chromium-network-check', [*cc, ROOT / 'userland/chromium-browser/network-check.c',
        '-o', out / 'chromium-network-check'])
    run('network-check', [*cc, ROOT / 'userland/network-check.c', '-o', out / 'network-check'])
    products = {p.name: dict(path=str(p), sha256=sha(p)) for p in out.iterdir()
                if p.is_file() and p.read_bytes()[:4] == b'\x7fELF'}
    for name, product in products.items():
        if name != 'trash-put' and 'INTERP' in subprocess.check_output(['readelf', '-l', product['path']], text=True):
            raise ValueError('The display bridge must use its source-built static SDK')
    (out / 'BUILD.json').write_text(json.dumps(dict(products=products, sources=source_records,
        source_sha256={str(p.relative_to(ROOT)): sha(p) for p in [*bridge.iterdir(), Path(__file__),
            ROOT / 'userland/chromium-browser/network-check.c', ROOT / 'userland/network-check.c'] if p.is_file()},
        application_sdk_sha256=sha(args.gnu_sdk / 'APPLICATION-SDK.json') if args.gnu_sdk else None,
        commands=commands, target_host_executed=False), indent=2) + '\n')
    print('Source-built Wayland bridge:', out / 'chromium-wayland')


if __name__ == '__main__':
    main()

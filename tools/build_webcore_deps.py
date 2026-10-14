#!/usr/bin/env python3
"""Build pinned, genuine WebCore dependencies in the separate modern prefix.

Offline only. Keep the working SDK and JSC/GLib/NetSurf caches intact.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/modern-browser'
PREFIX = BUILD / 'prefix'
CROSS = ROOT / 'build/compiler/cross/bin/x86_64-linux-musl-'
WEB_DEPS = ROOT / 'build/web/deps'
PACKAGES = {
    'harfbuzz-11.2.1.tar.xz': '093714c8548a285094685f0bdc999e202d666b59eeb3df2ff921ab68b8336a49',
    'fontconfig-2.16.0.tar.xz': '6a33dc555cc9ba8b10caf7695878ef134eeb36d0af366041f639b1da9b6ed220',
    'libwebp-1.6.0.tar.gz': 'e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564',
}
NETWORK_PACKAGES = json.loads(
    (ROOT / 'userland/modern-browser/dependency-sources.json').read_text())
PACKAGES.update({item['archive']: item['sha256'] for item in NETWORK_PACKAGES})
FLAGS = '-O2 -g0 -fno-pie -fno-stack-protector -march=x86-64 -mno-avx -mno-avx2'


def run(directory, name, args, env):
    log = BUILD / f'{name}.log'
    print(f'{name}: {shlex.join(map(str, args[:5]))}', flush=True)
    with log.open('w') as output:
        result = subprocess.run(list(map(str, args)), cwd=directory, env=env,
                                stdout=output, stderr=subprocess.STDOUT)
    if result.returncode:
        print('\n'.join(log.read_text(errors='replace').splitlines()[-25:]), file=sys.stderr)
        raise SystemExit(f'Failed ({result.returncode}); see {log}')


def verify(path, digest):
    if not path.exists() or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
        raise SystemExit(f'Missing or invalid pinned archive: {path}')


def environment():
    env = os.environ.copy()
    for key in ('CC', 'CXX', 'CPPFLAGS', 'CFLAGS', 'CXXFLAGS', 'LDFLAGS', 'LIBS',
                'CONFIG_SITE', 'GCC_EXEC_PREFIX', 'COMPILER_PATH', 'LIBRARY_PATH',
                'PKG_CONFIG_PATH', 'LD_LIBRARY_PATH', 'CMAKE_PREFIX_PATH'):
        env.pop(key, None)

    env.update(CC=str(CROSS) + 'gcc', CXX=str(BUILD / 'musl-posix-g++'),
               AR=str(CROSS) + 'ar', RANLIB=str(CROSS) + 'ranlib',
               CFLAGS=FLAGS, CXXFLAGS=FLAGS, LDFLAGS=f'-static -no-pie -L{PREFIX}/lib64',
               PKG_CONFIG_LIBDIR=f'{PREFIX}/lib/pkgconfig:{WEB_DEPS}/lib/pkgconfig',
               PYTHONPATH=str(BUILD / 'host-tools/meson-1.10.2'))
    env['PATH'] = (str(BUILD / 'host-tools') + ':' +
                   str(ROOT / 'build/browser/host-tools/bin') + ':' +
                   str(CROSS.parent) + ':' + env['PATH'])
    return env


def stage(name, configure, commands, env, inputs='', outputs=()):
    directory = BUILD / name
    directory.mkdir(parents=True, exist_ok=True)
    key = hashlib.sha256(json.dumps([list(map(str, configure)), inputs,
                                     {k: env.get(k) for k in ('CC', 'CXX', 'CPPFLAGS', 'CFLAGS', 'CXXFLAGS',
                                                              'LDFLAGS', 'PKG_CONFIG_LIBDIR')}], sort_keys=True).encode()).hexdigest()
    stamp = directory / '.modernos-stage'
    if stamp.exists() and stamp.read_text() == key and all(path.is_file() for path in outputs):
        print(f'{name}: cached', flush=True)
        return
    stamp.unlink(missing_ok=True)
    configured = directory / '.modernos-configured'
    ninja = 'setup' in configure or '-G' in configure
    configuration = directory / ('meson-private/coredata.dat' if 'setup' in configure
                                 else 'CMakeCache.txt' if '-G' in configure else 'Makefile')
    buildfile = directory / ('build.ninja' if ninja else 'Makefile')
    if (not configured.exists() or configured.read_text() != key
            or not configuration.is_file() or not buildfile.is_file()):
        # An existing Meson directory must be reconfigured, never wiped.
        command = list(configure)
        if (directory / 'meson-private/coredata.dat').exists():
            command.insert(command.index('setup') + 1, '--clearcache')
            command.insert(command.index('setup') + 1, '--reconfigure')
        run(directory, name + '-configure', command, env)
        configured.write_text(key)

    for i, command in enumerate(commands):
        run(directory, name + ('-build' if i == 0 else '-install'), command, env)
    missing = [str(path) for path in outputs if not path.is_file()]
    if missing:
        raise SystemExit(f'{name}: missing build outputs: ' + ', '.join(missing))
    stamp.write_text(key)


def meson_stage(source, options, env, jobs, targets=None, inputs='', outputs=()):
    command = [sys.executable, '-m', 'mesonbuild.mesonmain']
    name = source + '-target'

    cross = BUILD / 'webcore-deps-cross.ini'
    stage(name, [*command, 'setup', BUILD / name, BUILD / source,
          '--cross-file', cross, '--prefix', PREFIX, '--libdir', 'lib',
                 '--wrap-mode=nofallback', '--default-library=static', '--buildtype=release',
                 '-Db_staticpic=false', '-Dprefer_static=true', *options],
          [[BUILD / 'host-tools/ninja', f'-j{jobs}', *(targets or [])],
           [*command, 'install', '--no-rebuild']], env,
          [cross.read_text(), PACKAGES.get(source + '.tar.xz'), inputs], outputs)


def autotools_stage(source, options, env, jobs, outputs=()):
    configure = [BUILD / source / 'configure', '--host=x86_64-linux-musl',
                 f'--prefix={PREFIX}', f'--libdir={PREFIX}/lib',
                 '--disable-shared', '--enable-static', *options]
    digest = next(value for name, value in PACKAGES.items() if name.startswith(source + '.tar.'))
    stage(source + '-target', configure,
          [['make', f'-j{jobs}'], ['make', 'install']], env, digest, outputs)


def restore_originals(archive, paths):
    # Recover missing or interrupted backups from the pinned archive, never
    # from a working source file that may already contain our modifications.
    # Stream once so restoring many WebCore files does not repeatedly seek
    # through and decompress the large WebKit source archive.
    remaining = set(paths)
    with tarfile.open(archive, 'r|*') as upstream:
        for member in upstream:
            if member.name not in paths:
                continue
            original = paths[member.name]
            remaining.discard(member.name)
            with upstream.extractfile(member) as contents:
                data = contents.read()
            if original.is_file() and original.read_bytes() == data:
                continue
            original.parent.mkdir(parents=True, exist_ok=True)
            with tempfile.NamedTemporaryFile(dir=original.parent, prefix='.' + original.name,
                                             delete=False) as output:
                temporary = Path(output.name)
                try:
                    output.write(data)
                    output.close()
                    temporary.replace(original)
                finally:
                    temporary.unlink(missing_ok=True)
    if remaining:
        raise SystemExit('Pinned source archive is missing: ' + ', '.join(sorted(remaining)))


def patch_glib_networking():
    source = BUILD / 'glib-networking-2.80.1'
    originals = BUILD / 'webcore-original'
    paths = {
        'tls/openssl/meson.build': originals / 'glib-networking-openssl-meson.build',
        'tls/meson.build': originals / 'glib-networking-tls-meson.build',
    }
    archive_name = 'glib-networking-2.80.1.tar.xz'
    archive = ROOT / 'userland/vendor' / archive_name
    verify(archive, PACKAGES[archive_name])
    restore_originals(archive, {'glib-networking-2.80.1/' + name: path
                              for name, path in paths.items()})

    patched = paths['tls/openssl/meson.build'].read_text().replace(
        'module = shared_module(', 'module = static_library(')
    patched = patched.replace('  link_args: module_ldflags,\n', '').replace(
        '  link_depends: symbol_map,\n', '')
    patched = patched.replace('  name_suffix: module_suffix,\n', '')
    patched = patched.replace('  install: build_shared,', '  install: true,')
    patched = patched[:patched.index('\nif build_static\n')] + '\npkg.generate(module)\n'
    # TLS tests run in the owned guest test, rather than Meson's host runner.
    tls_patched = paths['tls/meson.build'].read_text().replace("subdir('tests')", '')
    for relative, text in (('tls/openssl/meson.build', patched), ('tls/meson.build', tls_patched)):
        path = source / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        if not path.is_file() or path.read_text() != text:
            path.write_text(text)
    return [patched, tls_patched]


def network_dependencies(env, jobs):
    target_env = env.copy()
    target_env['CPPFLAGS'] = f'-I{PREFIX}/include -I{WEB_DEPS}/include'
    target_env['LDFLAGS'] += f' -L{PREFIX}/lib -L{WEB_DEPS}/lib'
    autotools_stage('libgpg-error-1.61', ['--disable-nls', '--disable-doc',
                                          '--disable-tests', '--disable-languages'], target_env, jobs,
                    outputs=[PREFIX / 'lib/libgpg-error.a', PREFIX / 'lib/pkgconfig/gpg-error.pc'])
    autotools_stage('libgcrypt-1.12.4', ['--disable-asm', '--disable-avx-support',
                                         '--disable-avx2-support', '--disable-avx512-support', '--disable-doc',
                                         f'--with-libgpg-error-prefix={PREFIX}'], target_env, jobs,
                    outputs=[PREFIX / 'lib/libgcrypt.a', PREFIX / 'lib/pkgconfig/libgcrypt.pc'])
    autotools_stage('libtasn1-4.20.0', ['--disable-doc',
                    '--disable-valgrind-tests'], target_env, jobs,
                    outputs=[PREFIX / 'lib/libtasn1.a', PREFIX / 'lib/pkgconfig/libtasn1.pc'])
    autotools_stage('libxml2-2.14.6', ['--without-python', '--without-lzma',
                                       '--without-zlib', '--without-iconv'], target_env, jobs,
                    outputs=[PREFIX / 'lib/libxml2.a', PREFIX / 'lib/pkgconfig/libxml-2.0.pc'])

    # The official amalgamation supports direct compilation. No loader or
    # file-mapping assumptions are required; upstream's real pager uses I/O.
    source = BUILD / 'sqlite-autoconf-3540000'
    directory = BUILD / 'sqlite-3.54.0-target'
    directory.mkdir(parents=True, exist_ok=True)
    sqlite_key = json.dumps([PACKAGES['sqlite-autoconf-3540000.tar.gz'], FLAGS,
                            'threadsafe-columnmetadata-nommap-noextensions-1'])
    stamp = directory / '.modernos-stage'
    sqlite_outputs = [PREFIX / 'lib/libsqlite3.a', PREFIX / 'include/sqlite3.h',
                      PREFIX / 'include/sqlite3ext.h', PREFIX / 'lib/pkgconfig/sqlite3.pc']
    if (not stamp.exists() or stamp.read_text() != sqlite_key
            or not all(path.is_file() for path in sqlite_outputs)):
        stamp.unlink(missing_ok=True)
        for path in sqlite_outputs:
            path.parent.mkdir(parents=True, exist_ok=True)
        run(directory, 'sqlite-3.54.0-build', [env['CC'], *FLAGS.split(), '-pthread',
            '-DSQLITE_THREADSAFE=1', '-DSQLITE_ENABLE_COLUMN_METADATA',
                                               '-DSQLITE_OMIT_LOAD_EXTENSION', '-DSQLITE_MAX_MMAP_SIZE=0',
                                               '-c', source / 'sqlite3.c', '-o', directory / 'sqlite3.o'], env)
        run(directory, 'sqlite-3.54.0-archive', [env['AR'], 'rcs',
            directory / 'libsqlite3.a', directory / 'sqlite3.o'], env)
        shutil.copy2(directory / 'libsqlite3.a', PREFIX / 'lib/libsqlite3.a')
        for header in ('sqlite3.h', 'sqlite3ext.h'):
            shutil.copy2(source / header, PREFIX / 'include' / header)
        (PREFIX / 'lib/pkgconfig/sqlite3.pc').write_text(
            f'prefix={PREFIX}\nlibdir=${{prefix}}/lib\nincludedir=${{prefix}}/include\n'
            'Name: SQLite\nDescription: SQLite database engine\nVersion: 3.54.0\n'
            'Cflags: -I${includedir}\nLibs: -L${libdir} -lsqlite3\nLibs.private: -lm -lpthread\n')
        stamp.write_text(sqlite_key)
    else:
        print('sqlite-3.54.0-target: cached', flush=True)

    target_env['LIBS'] = f'{PREFIX}/lib/libicudata.a {PREFIX}/lib64/libstdc++.a -lm -lpthread'

    # Ship the release's genuine built-in suffix list; do not fetch mutable data.
    autotools_stage('libpsl-0.21.5', ['--enable-runtime=libicu', '--disable-nls',
                                      '--disable-gtk-doc', '--disable-man', '--with-psl-file=' +
                                      str(BUILD / 'libpsl-0.21.5/list/public_suffix_list.dat')], target_env, jobs,
                    outputs=[PREFIX / 'lib/libpsl.a', PREFIX / 'lib/pkgconfig/libpsl.pc'])

    # libpsl's release metadata omits its ICU static-link dependencies.
    metadata = PREFIX / 'lib/pkgconfig/libpsl.pc'
    text = metadata.read_text()
    if 'Requires.private: icu-uc' not in text:
        metadata.write_text(text + '\nRequires.private: icu-uc\n' +
                            f'Libs.private: -L{PREFIX}/lib64 -lstdc++ -lm -pthread\n')
    target_env.pop('LIBS')
    autotools_stage('nghttp2-1.65.0', ['--enable-lib-only', '--disable-python-bindings',
                                       '--disable-app', '--disable-examples', '--disable-hpack-tools'], target_env, jobs,
                    outputs=[PREFIX / 'lib/libnghttp2.a', PREFIX / 'lib/pkgconfig/libnghttp2.pc'])

    # Upstream always creates a shared module before extracting a static
    # archive, even for default_library=static. Build the real module sources
    # directly as an archive on this OS, which has no dynamic loader.
    patched = patch_glib_networking()
    meson_stage('glib-networking-2.80.1', ['-Dgnutls=disabled', '-Dopenssl=enabled',
                                           '-Dlibproxy=disabled', '-Dgnome_proxy=disabled', '-Denvironment_proxy=disabled',
                                           '-Dinstalled_tests=false'], env, jobs, inputs=patched,
                outputs=[PREFIX / 'lib/gio/modules/libgioopenssl.a',
                         BUILD / 'glib-networking-2.80.1-target/tls/base/libtlsbase.a'])

    # A cross build cannot run a TLS executable on the host. The backend is
    # linked/registered explicitly and verified against certificates in guests.
    meson_stage('libsoup-3.8.0', ['-Dgssapi=disabled', '-Dntlm=disabled',
                                  '-Dbrotli=disabled', '-Dzstd=disabled', '-Dtls_check=false',
                                  '-Dintrospection=disabled', '-Dvapi=disabled', '-Ddocs=disabled',
                                  '-Dtests=false', '-Dautobahn=disabled', '-Dsysprof=disabled',
                                  '-Dpkcs11_tests=disabled'], env, jobs,
                outputs=[PREFIX / 'lib/libsoup-3.0.a', PREFIX / 'lib/pkgconfig/libsoup-3.0.pc'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--jobs', type=int, default=min(os.cpu_count() or 4, 8))
    args = parser.parse_args()

    if not 1 <= args.jobs <= 32:
        parser.error('--jobs must be 1..32')
    BUILD.mkdir(parents=True, exist_ok=True)

    for name, digest in PACKAGES.items():
        archive = ROOT / 'userland/vendor' / name
        verify(archive, digest)
        with tarfile.open(archive) as source:
            directory = source.getmembers()[0].name.split('/')[0]
            if not (BUILD / directory).exists():
                source.extractall(BUILD, filter='data')

    env = environment()

    if not (BUILD / 'musl-posix-g++').exists() or not (PREFIX / 'lib/libgio-2.0.a').exists():
        raise SystemExit('Prepare the existing javascriptcore and webkit-deps targets first.')

    cross = BUILD / 'webcore-deps-cross.ini'
    cross.write_text('[binaries]\n' + '\n'.join(
        f'{key} = {value!r}' for key, value in {
            'c': env['CC'], 'cpp': env['CXX'], 'ar': env['AR'],
            'strip': str(CROSS) + 'strip', 'pkg-config': shutil.which('pkg-config'),
        }.items()) + '\n[host_machine]\n'
        "system = 'linux'\ncpu_family = 'x86_64'\ncpu = 'x86_64'\nendian = 'little'\n"
        '[properties]\nneeds_exe_wrapper = true\n[built-in options]\n'
        f'c_args = {FLAGS.split()!r}\ncpp_args = {FLAGS.split()!r}\n'
        "c_link_args = ['-static', '-no-pie']\ncpp_link_args = ['-static', '-no-pie']\n")

    meson_stage('harfbuzz-11.2.1', ['-Dglib=enabled', '-Dgobject=disabled',
                                    '-Dicu=enabled', '-Dfreetype=enabled', '-Dcairo=disabled', '-Dchafa=disabled',
                                    '-Dtests=disabled', '-Dutilities=disabled', '-Ddocs=disabled',
                                    '-Dintrospection=disabled'], env, args.jobs,
                outputs=[PREFIX / 'lib/libharfbuzz.a', PREFIX / 'lib/pkgconfig/harfbuzz.pc'])

    meson_stage('fontconfig-2.16.0', ['-Dxml-backend=expat', '-Dnls=disabled',
                                      '-Dtests=disabled', '-Dtools=disabled', '-Dcache-build=disabled', '-Ddoc=disabled',
                                      '-Ddefault-fonts-dirs=/usr/share/fonts/TTF', '-Dadditional-fonts-dirs=',
                                      f'-Dbaseconfig-dir={PREFIX}/etc/fonts', '-Dcache-dir=/tmp/fontconfig-cache'], env, args.jobs,
                outputs=[PREFIX / 'lib/libfontconfig.a', PREFIX / 'lib/pkgconfig/fontconfig.pc'])

    cmake = BUILD / 'host-tools/cmake-4.4.4-linux-x86_64/bin/cmake'
    source = 'libjpeg-turbo-3.2.0'
    name = source + '-target'
    stage(name, [cmake, '-S', BUILD / source, '-B', BUILD / name, '-G', 'Ninja',
                 f'-DCMAKE_MAKE_PROGRAM={BUILD}/host-tools/ninja',
                 f'-DCMAKE_TOOLCHAIN_FILE={BUILD}/musl-toolchain.cmake',
                 f'-DCMAKE_C_FLAGS={FLAGS}', '-DCMAKE_BUILD_TYPE=Release',
                 f'-DCMAKE_INSTALL_PREFIX={PREFIX}', '-DCMAKE_INSTALL_LIBDIR=lib',
                 '-DENABLE_SHARED=OFF', '-DENABLE_STATIC=ON', '-DWITH_SIMD=OFF',
                 '-DWITH_TURBOJPEG=OFF', '-DWITH_TOOLS=OFF', '-DWITH_TESTS=OFF'],
          [[cmake, '--build', BUILD / name, '--parallel', str(args.jobs)],
           [cmake, '--install', BUILD / name]], env, PACKAGES[source + '.tar.gz'],
          outputs=[PREFIX / 'lib/libjpeg.a', PREFIX / 'lib/pkgconfig/libjpeg.pc'])

    source = 'libwebp-1.6.0'
    name = source + '-target'
    options = ['-DBUILD_SHARED_LIBS=OFF', '-DWEBP_ENABLE_SIMD=OFF',
               '-DWEBP_BUILD_ANIM_UTILS=OFF', '-DWEBP_BUILD_CWEBP=OFF',
               '-DWEBP_BUILD_DWEBP=OFF', '-DWEBP_BUILD_GIF2WEBP=OFF',
               '-DWEBP_BUILD_IMG2WEBP=OFF', '-DWEBP_BUILD_VWEBP=OFF',
               '-DWEBP_BUILD_WEBPINFO=OFF', '-DWEBP_BUILD_WEBPMUX=OFF',
               '-DWEBP_BUILD_EXTRAS=OFF', '-DWEBP_BUILD_LIBWEBPMUX=ON',
               '-DCMAKE_INSTALL_LIBDIR=lib']
    stage(name, [cmake, '-S', BUILD / source, '-B', BUILD / name, '-G', 'Ninja',
                 f'-DCMAKE_MAKE_PROGRAM={BUILD}/host-tools/ninja',
                 f'-DCMAKE_TOOLCHAIN_FILE={BUILD}/musl-toolchain.cmake',
                 f'-DCMAKE_C_FLAGS={FLAGS}', '-DCMAKE_BUILD_TYPE=Release',
                 f'-DCMAKE_INSTALL_PREFIX={PREFIX}', *options],
          [[cmake, '--build', BUILD / name, '--parallel', str(args.jobs)],
           [cmake, '--install', BUILD / name]], env, PACKAGES[source + '.tar.gz'],
          outputs=[PREFIX / 'lib/libwebp.a', PREFIX / 'lib/pkgconfig/libwebp.pc'])

    network_dependencies(env, args.jobs)

    licenses = ROOT / 'userland/vendor/licenses'
    licenses.mkdir(parents=True, exist_ok=True)
    for name, source in (('harfbuzz', 'harfbuzz-11.2.1/COPYING'),
                         ('fontconfig', 'fontconfig-2.16.0/COPYING'),
                         ('libjpeg-turbo', 'libjpeg-turbo-3.2.0/LICENSE.md'),
                         ('webp', 'libwebp-1.6.0/COPYING'),
                         ('webp-PATENTS', 'libwebp-1.6.0/PATENTS')):
        shutil.copy2(BUILD / source, licenses / (name + '-LICENSE.txt'))

    for name, source in (('libxml2', 'libxml2-2.14.6/Copyright'),
                         ('libsoup', 'libsoup-3.8.0/COPYING'),
                         ('glib-networking', 'glib-networking-2.80.1/COPYING'),
                         ('glib-networking-OpenSSL-exception',
                          'glib-networking-2.80.1/LICENSE_EXCEPTION'),
                         ('libgpg-error', 'libgpg-error-1.61/COPYING.LIB'),
                         ('libgcrypt', 'libgcrypt-1.12.4/COPYING.LIB'),
                         ('libtasn1', 'libtasn1-4.20.0/COPYING.LESSERv2'),
                         ('libpsl', 'libpsl-0.21.5/COPYING'),
                         ('nghttp2', 'nghttp2-1.65.0/COPYING')):
        shutil.copy2(BUILD / source, licenses / (name + '-LICENSE.txt'))

    sqlite = (BUILD / 'sqlite-autoconf-3540000/sqlite3.c').read_text()
    start = sqlite.index('/*\n** 2001 September 15')
    end = sqlite.index(
        '*************************************************************************', start)
    (licenses / 'sqlite-LICENSE.txt').write_text(sqlite[start:end] + '*/\n')
    shutil.copy2(BUILD / 'libpsl-0.21.5/src/LICENSE.chromium',
                 licenses / 'libpsl-Chromium-LICENSE.txt')
    suffixes = (BUILD / 'libpsl-0.21.5/list/public_suffix_list.dat').read_text()
    (licenses / 'libpsl-suffix-list-LICENSE.txt').write_text(
        suffixes[:suffixes.index('// ===BEGIN ICANN DOMAINS===')])

    print('Built genuine static rendering, Soup/XML/crypto and OpenSSL Gio TLS dependencies.', flush=True)


if __name__ == '__main__':
    main()

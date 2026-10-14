#!/usr/bin/env python3
"""Offline static-musl GLib/Gio layer for the genuine WPE WebKit port.

This preserves the existing SDK, NetSurf and JSC caches. Additional WebCore
graphics/network dependencies and the renderer/frontend are still unfinished.
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
from prepare_browser_host import prepare as prepare_host_tools

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/modern-browser'
PREFIX = BUILD / 'prefix'
CROSS = ROOT / 'build/compiler/cross/bin/x86_64-linux-musl-'
PACKAGES = {
    'glib-2.86.5.tar.xz': 'ce85a947bb8b3c0204dbeff79aec39bcb46371c6fafb64ba5b8726c71e038d5f',
    'libffi-3.5.2.tar.gz': 'f3a3082a23b37c293a4fcd1053147b371f2ff91fa7ea1b2a52e335676bac82dc',
    'pcre2-10.46.tar.bz2': '15fbc5aba6beee0b17aecb04602ae39432393aba1ebd8e39b7cabf7db883299f',
}
FLAGS = '-O2 -g0 -fno-pie -fno-stack-protector -mno-avx -mno-avx2'


def run(directory, name, args, env):
    print(f'{name}: {shlex.join(map(str, args[:4]))}', flush=True)
    with (BUILD / f'{name}.log').open('w') as output:
        result = subprocess.run(list(map(str, args)), cwd=directory, env=env,
                                stdout=output, stderr=subprocess.STDOUT)
    if result.returncode:
        raise SystemExit(f'Failed ({result.returncode}); see {BUILD / name}.log')


def verify(path, digest):
    if not path.exists() or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
        raise SystemExit(f'Missing or invalid pinned archive: {path}')


def build_stage(name, configure, commands, env, inputs):
    directory = BUILD / name
    directory.mkdir(exist_ok=True)
    key = hashlib.sha256(json.dumps([list(map(str, configure)), inputs, PACKAGES,
                                     FLAGS, {k: env.get(k) for k in ('CC', 'CXX', 'CFLAGS', 'LDFLAGS', 'PKG_CONFIG_LIBDIR')}], sort_keys=True).encode()).hexdigest()
    stamp = directory / '.modernos-stage'
    if stamp.exists() and stamp.read_text() == key:
        print(f'{name}: cached', flush=True)
        return
    configured = directory / '.modernos-configured'
    if not configured.exists() or configured.read_text() != key:
        run(directory, name + '-configure', configure, env)
        configured.write_text(key)
    for i, command in enumerate(commands):
        run(directory, name + ('-build' if i == 0 else '-install'), command, env)
    stamp.write_text(key)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--jobs', type=int, default=min(os.cpu_count() or 4, 8))
    args = parser.parse_args()

    host_tools = prepare_host_tools(jobs=args.jobs)

    for archive, digest in PACKAGES.items():
        path = ROOT / 'userland/vendor' / archive
        verify(path, digest)
        with tarfile.open(path) as source:
            directory = source.getmembers()[0].name.split('/')[0]
            if not (BUILD / directory).exists():
                source.extractall(BUILD, filter='data')

    meson = host_tools['meson']
    ninja = host_tools['ninja']
    env = os.environ.copy()
    env.update(CC=str(CROSS) + 'gcc', CXX=str(BUILD / 'musl-posix-g++'),
               AR=str(CROSS) + 'ar', RANLIB=str(CROSS) + 'ranlib',
               CFLAGS=FLAGS, CXXFLAGS=FLAGS, LDFLAGS='-static -no-pie',
               PKG_CONFIG_LIBDIR=str(PREFIX / 'lib/pkgconfig') + ':' + str(ROOT / 'build/web/deps/lib/pkgconfig'),
               PYTHONPATH=str(meson), PATH=str(ninja.parent) + ':' + env['PATH'])
    for variable in ('PKG_CONFIG_PATH', 'LD_LIBRARY_PATH', 'CMAKE_PREFIX_PATH'):
        env.pop(variable, None)

    for name, options in (
        ('libffi-3.5.2', ['--disable-docs',
         '--disable-multi-os-directory', '--disable-exec-static-tramp']),
        ('pcre2-10.46', ['--disable-jit', '--enable-unicode',
         '--disable-pcre2grep', '--disable-pcre2test']),
    ):
        configure = [BUILD / name / 'configure', '--host=x86_64-linux-musl',
                     f'--prefix={PREFIX}', '--libdir=' + str(PREFIX / 'lib'),
                     '--disable-shared', '--enable-static', *options]
        build_stage(name + '-target', configure,
                    [['make', f'-j{args.jobs}'], ['make', 'install']], env, options)

    cross = BUILD / 'glib-musl-cross.ini'
    cross.write_text('[binaries]\n' + '\n'.join(
        f'{key} = {value!r}' for key, value in {
            'c': env['CC'], 'cpp': env['CXX'], 'ar': env['AR'],
            'strip': str(CROSS) + 'strip', 'pkg-config': shutil.which('pkg-config'),
        }.items()) + '\n[host_machine]\n'
        "system = 'linux'\ncpu_family = 'x86_64'\ncpu = 'x86_64'\nendian = 'little'\n"
        '[properties]\nneeds_exe_wrapper = true\n[built-in options]\n'
        f'c_args = {FLAGS.split()!r}\ncpp_args = {FLAGS.split()!r}\n'
        "c_link_args = ['-static', '-no-pie']\ncpp_link_args = ['-static', '-no-pie']\n")
    options = ['--default-library=static', '--buildtype=release', '-Db_staticpic=false',
               '-Dtests=false', '-Dinstalled_tests=false', '-Ddocumentation=false',
               '-Dintrospection=disabled', '-Dman-pages=disabled', '-Dselinux=disabled',
               '-Dlibmount=disabled', '-Dxattr=false', '-Dnls=disabled',
               '-Dsysprof=disabled', '-Dlibelf=disabled', '-Ddtrace=disabled',
               '-Dsystemtap=disabled', '-Dforce_posix_threads=true']
    command = [sys.executable, '-m', 'mesonbuild.mesonmain']
    build_stage('glib-target', [*command, 'setup', BUILD / 'glib-target', BUILD / 'glib-2.86.5',
                '--cross-file', cross, '--prefix', PREFIX, '--libdir', 'lib', '--wrap-mode=nofallback', *options],
                [[ninja, f'-j{args.jobs}'], [*command, 'install', '--no-rebuild']], env, cross.read_text())
    licenses = ROOT / 'userland/vendor/licenses'
    for source, target in (('glib-2.86.5/COPYING', 'glib-LICENSE.txt'),
                           ('libffi-3.5.2/LICENSE', 'libffi-LICENSE.txt'),
                           ('pcre2-10.46/LICENCE.md', 'pcre2-LICENSE.txt')):
        shutil.copy2(BUILD / source, licenses / target)

    cflags = subprocess.check_output(
        ['pkg-config', '--cflags', 'gio-unix-2.0', 'gthread-2.0'], env=env, text=True)
    libs = subprocess.check_output(
        ['pkg-config', '--static', '--libs', 'gio-unix-2.0', 'gthread-2.0'], env=env, text=True)
    run(ROOT, 'glib-check-build', [env['CC'], *FLAGS.split(), '-Wall', '-Wextra', '-Werror', '-pthread',
        '-static', '-no-pie', *shlex.split(cflags), ROOT / 'userland/modern-browser/glib-check.c',
                                   *shlex.split(libs), '-o', ROOT / 'userland/bin/glib-check'], env)

    print('Built genuine static GLib/GObject/Gio and guest check; full WebCore remains unfinished.', flush=True)


if __name__ == '__main__':
    main()

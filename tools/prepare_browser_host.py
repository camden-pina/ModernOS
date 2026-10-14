#!/usr/bin/env python3
"""Prepare verified browser build tools locally; no system installation needed."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile

from fetch_sources import ROOT, fetch, read_manifest

PACKAGES = (
    {
        'archive': 'modernos-cmake-4.4.4.tar.gz',
        'url': 'https://github.com/Kitware/CMake/releases/download/v4.4.4/cmake-4.4.4-linux-x86_64.tar.gz',
        'mirrors': ['https://cmake.org/files/v4.4/cmake-4.4.4-linux-x86_64.tar.gz'],
        'sha256': 'e5bb807f7728cb60cd8b27ebc97a2edb469b68655f21e844a600c3575b76f5bb',
        'directory': 'cmake-4.4.4-linux-x86_64',
        'ready': ('bin/cmake', 'share/cmake-4.4/Modules/CMakeDetermineSystem.cmake'),
        'kind': 'tar',
    },
    {
        'archive': 'modernos-ninja-1.13.2.zip',
        'url': 'https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-linux.zip',
        'sha256': '5749cbc4e668273514150a80e387a957f933c6ed3f5f11e03fb30955e2bbead6',
        'directory': None,
        'ready': ('ninja',),
        'kind': 'zip',
    },
    {
        'archive': 'meson-1.10.2-py3-none-any.whl',
        'url': 'https://files.pythonhosted.org/packages/0b/78/7d049e63e624d51d0065191dae101a1e36d5d3a2360633772d9ad8afb2d5/meson-1.10.2-py3-none-any.whl',
        'mirrors': ['https://mirrors.sjtug.sjtu.edu.cn/pypi-packages/0b/78/7d049e63e624d51d0065191dae101a1e36d5d3a2360633772d9ad8afb2d5/meson-1.10.2-py3-none-any.whl'],
        'sha256': '5f84ef186e6e788d9154db63620fc61b3ece69f643b94b43c8b9203c43d89b36',
        'directory': 'meson-1.10.2',
        'ready': ('mesonbuild/mesonmain.py',),
        'kind': 'wheel',
    },
)


def prepare_archive(entry, cache):
    host = cache / 'host-tools'
    host.mkdir(parents=True, exist_ok=True)
    target = host / entry['directory'] if entry['directory'] else host
    stamp = host / ('.' + entry['archive'] + '.prepared')
    if (stamp.is_file() and stamp.read_text() == entry['sha256']
            and all((target / name).is_file() for name in entry['ready'])):
        return

    fetch(entry, cache / 'downloads')
    archive = cache / 'downloads' / entry['archive']

    # Publish an extracted directory only when all required inputs exist.
    with tempfile.TemporaryDirectory(prefix='.browser-host-', dir=host) as temporary:
        staging = Path(temporary)
        if entry['kind'] == 'tar':
            with tarfile.open(archive) as contents:
                contents.extractall(staging, filter='data')
            unpacked = staging / entry['directory']
        else:
            unpacked = staging / 'unpacked'
            unpacked.mkdir()
            with zipfile.ZipFile(archive) as contents:
                for item in contents.infolist():
                    name = Path(item.filename)
                    mode = (item.external_attr >> 16) & 0o170000
                    if name.is_absolute() or '..' in name.parts or mode == 0o120000:
                        raise ValueError(f'Unsafe build-tool archive member: {item.filename}')
                contents.extractall(unpacked)

        if not all((unpacked / name).is_file() for name in entry['ready']):
            raise ValueError(f'Incomplete build-tool archive: {archive}')

        if entry['kind'] == 'zip':
            # The Ninja release contains a single executable, not a directory.
            source = unpacked / 'ninja'
            source.chmod(0o755)
            source.replace(host / 'ninja')
        else:
            previous = staging / '.previous'
            if target.exists():
                target.rename(previous)

            try:
                unpacked.rename(target)
            except BaseException:
                if previous.exists():
                    previous.rename(target)
                raise
    stamp.write_text(entry['sha256'])


def ruby_ready(executable):
    if not executable.is_file():
        return False
    try:
        result = subprocess.run([str(executable), '-e',
                                 'require "erb"; require "json"; puts RUBY_VERSION'],
                                capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return False
    return result.returncode == 0 and result.stdout.strip() == '3.4.7'


def run_logged(directory, cache, name, command, env):
    log = cache / ('browser-host-' + name + '.log')
    print(f'Browser build tools: {name}', flush=True)
    with log.open('w') as output:
        result = subprocess.run(list(map(str, command)), cwd=directory, env=env,
                                stdout=output, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f'Browser build-tool preparation failed; see {log}')


def prepare_ruby(root, cache, jobs):
    prefix = cache / 'host-tools/ruby'
    executable = prefix / 'bin/ruby'

    # Also accept the already verified Ruby cache created by earlier builders.
    if ruby_ready(executable):
        return executable
    entry = read_manifest(root / 'userland/vendor/sources.json')['ruby-3.4.7.tar.xz']
    fetch(entry, root / 'userland/vendor')
    source = cache / 'ruby-3.4.7'
    if not (source / 'configure').is_file():
        with tempfile.TemporaryDirectory(prefix='.ruby-extract-', dir=cache) as temporary:
            with tarfile.open(root / 'userland/vendor' / entry['archive']) as archive:
                archive.extractall(temporary, filter='data')
            extracted = Path(temporary) / 'ruby-3.4.7'
            if not (extracted / 'configure').is_file():
                raise ValueError('The pinned Ruby source archive is incomplete')
            if source.exists():
                shutil.rmtree(source)
            extracted.rename(source)

    build = cache / 'ruby-host'
    build.mkdir(exist_ok=True)
    env = os.environ.copy()
    for name in ('CC', 'CXX', 'CFLAGS', 'CXXFLAGS', 'CPPFLAGS', 'LDFLAGS', 'LIBS',
                 'CONFIG_SITE', 'GCC_EXEC_PREFIX', 'COMPILER_PATH', 'LIBRARY_PATH'):
        env.pop(name, None)
    env.update(CC='gcc', CXX='g++', CFLAGS='-O2', CXXFLAGS='-O2')

    for name in ('gcc', 'make'):
        if not shutil.which(name, path=env.get('PATH')):
            raise RuntimeError(f'Automatic browser preparation requires the host tool {name}')

    configure = [source / 'configure', f'--prefix={prefix}', '--disable-install-doc',
                 '--disable-shared', '--disable-yjit', '--disable-rjit', '--without-gmp',
                 '--with-out-ext=openssl']
    key = hashlib.sha256(json.dumps([list(map(str, configure)), entry['sha256'],
                                    env['CC'], env['CFLAGS']]).encode()).hexdigest()
    configured = build / '.browser-host-configured'
    if not configured.is_file() or configured.read_text() != key:
        run_logged(build, cache, 'ruby-configure', configure, env)
        configured.write_text(key)
    run_logged(build, cache, 'ruby-build', ['make', f'-j{jobs}'], env)
    run_logged(build, cache, 'ruby-install', ['make', 'install'], env)

    if not ruby_ready(executable):
        raise RuntimeError('Prepared Ruby is missing the required erb/json generators')
    return executable


def prepare(root=ROOT, cache=None, jobs=8):
    if sys.platform != 'linux' or platform.machine().lower() not in ('x86_64', 'amd64'):
        raise RuntimeError('The complete browser build currently requires an x86-64 Linux host')
    if not 1 <= jobs <= 32:
        raise ValueError('Browser build-tool jobs must be 1..32')

    root = Path(root).resolve()
    cache = Path(cache or root / 'build/modern-browser').resolve()
    cache.mkdir(parents=True, exist_ok=True)

    # Direct builders and parallel Make invocations share one host cache.
    with (cache / '.browser-host.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        for entry in PACKAGES:
            prepare_archive(entry, cache)
        ruby = prepare_ruby(root, cache, jobs)

    return {'cmake': cache / 'host-tools/cmake-4.4.4-linux-x86_64/bin/cmake',
            'ninja': cache / 'host-tools/ninja', 'ruby': ruby,
            'meson': cache / 'host-tools/meson-1.10.2'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache', type=Path, help='alternate host-tool cache for verification')
    parser.add_argument('--jobs', type=int, default=min(os.cpu_count() or 4, 8))
    args = parser.parse_args()
    prepare(cache=args.cache, jobs=args.jobs)
    print('Browser build tools ready: CMake, Ninja, Meson and Ruby.', flush=True)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error)) from error

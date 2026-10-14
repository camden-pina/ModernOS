#!/usr/bin/env python3
"""Build the POSIX libgcc used with the separate threaded C++ runtime.

The SDK's seed compiler deliberately uses gthr-single.h. Reuse its target
configuration and upstream make rules in a separate directory, with the POSIX
thread backend, without modifying the seed compiler or its installed archives.
"""
import hashlib
import json
import fcntl
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SEED = ROOT / 'build/compiler/seed'
SOURCE = ROOT / 'build/compiler/src/gcc-14.2.0/libgcc'
TARGET = 'x86_64-linux-musl'


def build_posix_libgcc(build, prefix, jobs, env):
    build.mkdir(parents=True, exist_ok=True)

    # JSC and WebCore can be separate parallel Make prerequisites. Both use
    # this cache, so the stamp must be checked after acquiring its lock.
    with (build / '.libgcc-posix-build.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        return _build_posix_libgcc(build, prefix, jobs, env)


def _build_posix_libgcc(build, prefix, jobs, env):
    original = SEED / TARGET / 'libgcc'
    gcc = SEED / 'gcc'
    archive = prefix / 'lib64/libgcc.a'
    inputs = [original / 'Makefile', original / 'libgcc.a',
              original / 'auto-target.h', gcc / 'libgcc.mvars',
              gcc / 'xgcc', gcc / 'cc1',
              SOURCE / 'gthr-posix.h', Path(__file__)]
    key = hashlib.sha256(json.dumps([
        [str(path), hashlib.sha256(path.read_bytes()).hexdigest()]
        for path in inputs
    ], sort_keys=True).encode()).hexdigest()
    directory = build / 'libgcc-posix' / key
    stamp = directory / '.modernos-stage'
    if not stamp.exists() or not (directory / 'libgcc.a').exists():
        # Existing objects contain no-op locks. Recompile every object
        # using the generated target configuration and real POSIX header.
        # Dereference header links and allow an interrupted copy to resume.
        shutil.copytree(original, directory, dirs_exist_ok=True,
                        ignore=shutil.ignore_patterns('*.o', '*.a', '*.dep',
                                                      'gthr-default.h'))
        shutil.copy2(SOURCE / 'gthr-posix.h', directory / 'gthr-default.h')
        command = ['make', f'-j{jobs}', f'gcc_objdir={gcc}',
                   'thread_header=gthr-posix.h', '-o', 'Makefile',
                   '-o', 'config.status', 'libgcc.a']
        print('libgcc-posix: building threaded unwind runtime', flush=True)
        build_env = env.copy()

        # Use this Makefile's recorded target flags, rather than inheriting
        # command-line overrides or a jobserver from the parent build.
        for variable in ('MAKEFLAGS', 'MFLAGS', 'MAKEOVERRIDES'):
            build_env.pop(variable, None)
        with (build / 'libgcc-posix-build.log').open('w') as output:
            result = subprocess.run(command, cwd=directory, env=build_env,
                                    stdout=output, stderr=subprocess.STDOUT)

        if result.returncode:
            raise SystemExit(f'POSIX libgcc failed; see {build / "libgcc-posix-build.log"}')
        symbols = subprocess.check_output(['nm', '-u', str(directory / 'libgcc.a')],
                                          text=True, stderr=subprocess.DEVNULL)
        for function in ('pthread_mutex_lock', 'pthread_mutex_unlock', 'pthread_once'):
            if f' U {function}' not in symbols:
                raise SystemExit(f'POSIX libgcc is missing {function}')
        stamp.write_text(key)
    else:
        print('libgcc-posix: cached', flush=True)

    archive.parent.mkdir(parents=True, exist_ok=True)
    source = directory / 'libgcc.a'

    # Different cache directories may install to the same prefix. Serialize
    # installation there too, and publish atomically so a linker never reads
    # a partially copied archive.
    with (archive.parent / '.libgcc-posix-install.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if not archive.exists() or archive.read_bytes() != source.read_bytes():
            temporary = archive.with_name(archive.name + f'.{os.getpid()}.pending')
            try:
                shutil.copy2(source, temporary)
                temporary.replace(archive)
            finally:
                temporary.unlink(missing_ok=True)
    return archive

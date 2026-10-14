#!/usr/bin/env python3
"""Package verified corresponding toolchain sources and the native rebuild script."""
import gzip
import hashlib
from pathlib import Path
import shutil
import subprocess

from build_gcc import ROOT, VENDOR, BUILD, PACKAGES
from build_gawk import PACKAGE as GAWK_PACKAGE, DIGEST as GAWK_DIGEST

SOURCES = dict(PACKAGES, **{
    'musl-1.2.5.tar.gz': 'a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4',
    'make-4.4.1.tar.gz': 'dd16fb1d67bfab79a72f5e8390735c49e3e8e70b4945a15ab1f81ddb78658fb3',
    GAWK_PACKAGE: GAWK_DIGEST,
})


def main():
    runtime = ROOT / 'userland/runtime'
    cache = BUILD / 'source-bundle'
    cache.mkdir(parents=True, exist_ok=True)
    destination = runtime / 'usr/src/modernos'
    destination.mkdir(parents=True, exist_ok=True)
    manifest, originals = [], []
    for name, expected in SOURCES.items():
        source = VENDOR / name
        with source.open('rb') as original:
            digest = hashlib.file_digest(original, 'sha256').hexdigest()
        if digest != expected:
            raise SystemExit(f'Upstream source checksum mismatch: {source}')
        originals.append(f'{expected}  {name}\n')

        if name.endswith('.gz'):
            # Recompress only the tar stream. Its upstream files/timestamps are
            # unchanged, and the original vendored archive remains available.
            output = cache / name.replace('.gz', '.xz')
            stamp = output.with_suffix('.stamp')
            if not output.exists() or not stamp.exists() or stamp.read_text() != expected:
                print('Recompressing ' + name, flush=True)
                temporary = output.with_suffix('.tmp')
                with gzip.open(source, 'rb') as data, temporary.open('wb') as compressed:
                    process = subprocess.Popen(['xz', '-T4', '-6', '-c'], stdin=subprocess.PIPE,
                                               stdout=compressed)
                    try:
                        shutil.copyfileobj(data, process.stdin, 1024 * 1024)
                    finally:
                        process.stdin.close()

                    if process.wait():
                        temporary.unlink(missing_ok=True)
                        raise SystemExit('xz source compression failed')

                temporary.replace(output)
                stamp.write_text(expected)
        else:
            output = source

        target = destination / output.name
        shutil.copy2(output, target)
        with target.open('rb') as file:
            digest = hashlib.file_digest(file, 'sha256').hexdigest()
        manifest.append(f'{digest}  {target.name}\n')

    helper = destination / 'prepare-gcc-options.sh'
    shutil.copy2(ROOT / 'userland/sdk/prepare-gcc-options.sh', helper)
    manifest.append(f'{hashlib.sha256(helper.read_bytes()).hexdigest()}  {helper.name}\n')
    (destination / 'SHA256SUMS').write_text(''.join(manifest))
    (destination / 'UPSTREAM-SHA256SUMS').write_text(''.join(originals))
    examples = destination / 'examples'
    examples.mkdir(exist_ok=True)
    for name in ('hello.c', 'hello.cpp', 'Makefile'):
        shutil.copy2(ROOT / 'userland/sdk' / name, examples / name)
    shutil.copy2(ROOT / 'userland/compiler-check.c', examples / 'abi-check.c')

    # The ABI example uses the same private protocol definition as the
    # installed compiler-check binary. Keep its relative include intact so a
    # recursively copied example remains independently buildable.
    (examples / 'include').mkdir(exist_ok=True)
    shutil.copy2(ROOT / 'userland/include/memory_usage.h',
                 examples / 'include/memory_usage.h')
    script = runtime / 'usr/bin/bootstrap-toolchain'
    shutil.copy2(ROOT / 'userland/sdk/bootstrap-toolchain.sh', script)
    script.chmod(0o755)

    for path, target in ((runtime / 'usr/lib64', 'lib'), (runtime / 'lib', 'usr/lib')):
        if not path.is_symlink():
            if path.exists():
                raise SystemExit(f'Refusing to replace an existing SDK directory: {path}')
            path.symlink_to(target, target_is_directory=True)

    print('Packaged native toolchain sources and bootstrap-toolchain.', flush=True)


if __name__ == '__main__':
    main()

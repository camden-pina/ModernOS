#!/usr/bin/env python3
"""Stage source-built GNU headers/libraries and isolated C/C++ driver wrappers.

The resulting sysroot is separate from Coast's MUSL SDK. Linux build tools can
use its wrappers to avoid introducing newer host GLIBC/GLIBCXX requirements
into application addons. The wrappers do not run the compiled target programs.
"""
from build_state import project_lock
from build_state import atomic_write
import argparse
import json
from pathlib import Path, PurePosixPath
import platform
import shlex
import shutil

from source_tree import digest

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--glibc-build', type=Path, required=True)
    parser.add_argument('--libgcc-build', type=Path, required=True)
    parser.add_argument('--cxx-build', type=Path, required=True)
    parser.add_argument('--output-root', type=Path, required=True)
    parser.add_argument('--cc', type=Path, default=Path('/usr/bin/gcc'))
    parser.add_argument('--cxx', type=Path, default=Path('/usr/bin/g++'))
    parser.add_argument('--native', action='store_true')
    args = parser.parse_args()
    if args.native and platform.system() != 'Coast':
        parser.error('--native requires Coast')
    receipts = {name: getattr(args, name + '_build').resolve(strict=True) for name in ['glibc', 'libgcc', 'cxx']}
    records = {name: json.loads(path.read_text()) for name, path in receipts.items()}
    if any(records[name]['glibc_build_sha256'] != digest(receipts['glibc']) for name in ['libgcc', 'cxx']):
        parser.error('GNU SDK libraries must use the same glibc build')
    if records['cxx'].get('libgcc_build_sha256') != digest(receipts['libgcc']):
        parser.error('GNU C++ must have been built with this source-built unwinder')
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=False)
    sysroot = out / 'sysroot'
    staged, aliases = {}, {}
    for name, record in records.items():
        for relative, expected in record['source_sha256'].items():
            if digest(ROOT / relative) != expected:
                raise ValueError('GNU SDK recipe changed: ' + relative)
        for relative, asset in record['files'].items():
            path = PurePosixPath(relative)
            if path.is_absolute() or '..' in path.parts:
                raise ValueError('Invalid GNU SDK path: ' + relative)
            source = receipts[name].parent / 'runtime' / relative
            if digest(source) != asset['sha256']:
                raise ValueError('GNU SDK artifact changed: ' + relative)
            if relative in staged and staged[relative]['sha256'] != asset['sha256']:
                raise ValueError('Conflicting GNU SDK artifact: ' + relative)
            target = sysroot / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
            target.chmod(asset['mode'])
            staged[relative] = dict(sha256=asset['sha256'], component=name, mode=asset['mode'])
        for relative, target in record.get('aliases', {}).items():
            path = PurePosixPath(relative)
            if (path.is_absolute() or '..' in path.parts or PurePosixPath(target).is_absolute() or
                    relative in staged or relative in aliases):
                raise ValueError('Conflicting GNU SDK alias: ' + relative)
            aliases[relative] = target
    # Resolve links inside the assembled SDK; never allow them to escape it.
    for relative, target in aliases.items():
        path = sysroot / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.symlink_to(target)
    for relative in aliases:
        resolved = (sysroot / relative).resolve(strict=True)
        if not resolved.is_relative_to(sysroot) or not resolved.is_file():
            raise ValueError('GNU SDK alias escapes its runtime: ' + relative)
    libraries = sysroot / 'usr/lib/x86_64-linux-gnu'
    support = sysroot / 'usr/lib/gcc/x86_64-coast-linux-gnu/14.2.0'
    headers = sysroot / 'usr/include/c++/14.2.0'
    common = ['--sysroot=' + str(sysroot), '-B' + str(support) + '/', '-B' + str(libraries) + '/',
        '-mno-avx', '-mno-avx2', '-idirafter', str(ROOT / 'ports/linux-uapi/include'),
        '-L' + str(libraries), '-L' + str(support), '-Wl,-rpath-link,' + str(libraries)]
    if args.native:
        specs = out / 'gnu-driver.specs'
        specs.write_text('*self_spec:\n%<static %<no-pie\n\n')
        common = ['-specs=' + str(specs), *common]
    cpp = ['-nostdinc++', '-isystem', str(headers), '-isystem', str(headers / 'x86_64-coast-linux-gnu'),
        '-isystem', str(headers / 'backward')]
    binary = out / 'bin'
    binary.mkdir()
    commands = {}
    for name, compiler, extra in [('gcc', args.cc, []), ('cc', args.cc, []), ('g++', args.cxx, cpp), ('c++', args.cxx, cpp)]:
        command = [str(compiler.resolve(strict=True)), *common, *extra]
        path = binary / name
        path.write_text('#!/bin/sh\nexec ' + shlex.join(command) + ' "$@"\n')
        path.chmod(0o755)
        commands[name] = command
    atomic_write(out / 'SDK.json', json.dumps(dict(
        state='source-built GNU SDK staged; consumer qualification required',
        component_build_sha256={name: digest(path) for name, path in receipts.items()},
        files=staged, aliases=aliases, compiler_commands=commands,
        build_mode='native-coast' if args.native else 'linux-cross',
        source_sha256={str(Path(__file__).relative_to(ROOT)): digest(Path(__file__))},
        target_executed=False), indent=2) + '\n')
    print('GNU SDK staged:', out, flush=True)


if __name__ == '__main__':
    with project_lock():
        main()

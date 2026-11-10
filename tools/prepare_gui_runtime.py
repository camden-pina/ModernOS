#!/usr/bin/env python3
"""Stage pinned public GUI libraries with Coast's source-built GNU runtime.

Debian GUI libraries are binary bootstrap inputs, never host executables.
Their package scripts are not run. glibc, libgcc and libstdc++ come exclusively
from the supplied source-build receipts. This is not a GTK/NSS source builder.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import io
import json
from pathlib import Path, PurePosixPath
import posixpath
import re
import shutil
import subprocess
import tarfile

from fetch_sources import fetch
from source_tree import digest
from debian_package import package_tar

ROOT = Path(__file__).resolve().parents[1]
PINS = ROOT / 'ports/chromium/runtime-bootstrap.json'


def canonical(relative):
    while relative.startswith('./'):
        relative = relative[2:]
    path = PurePosixPath(relative)
    if path.is_absolute() or '..' in path.parts:
        raise ValueError('Unsafe GUI package path: ' + relative)
    if str(path).startswith('lib/'):
        path = PurePosixPath('usr') / path
    return str(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--glibc-build', type=Path, required=True)
    parser.add_argument('--libgcc-build', type=Path, required=True)
    parser.add_argument('--cxx-build', type=Path, required=True)
    parser.add_argument('--output-root', type=Path, default=ROOT / 'build/gui-runtime-build')
    parser.add_argument('--vendor-root', type=Path, default=ROOT / 'build/gui-runtime/vendor')
    parser.add_argument('--jobs', type=int, default=4, help='parallel checksum-pinned downloads')
    parser.add_argument('--source-build', type=Path,
                        help='complete build_gui_sources receipt; stage source-built GUI libraries')
    args = parser.parse_args()
    if not 1 <= args.jobs <= 8:
        parser.error('--jobs must be 1..8')
    receipts = {name: getattr(args, name + '_build').resolve(strict=True) for name in ('glibc', 'libgcc', 'cxx')}
    records = {name: json.loads(path.read_text()) for name, path in receipts.items()}
    if (any(records[name].get('glibc_build_sha256') != digest(receipts['glibc']) for name in ('libgcc', 'cxx')) or
            records['cxx'].get('libgcc_build_sha256') != digest(receipts['libgcc'])):
        parser.error('Select the matching source-built GNU ABI components')
    for record in records.values():
        for name, expected in record['source_sha256'].items():
            if digest(ROOT / name) != expected:
                raise ValueError('GNU runtime source changed: ' + name)
    pins = json.loads(PINS.read_text())
    source_build = None
    if args.source_build:
        source_build = json.loads(args.source_build.read_text())
        if not source_build.get('complete') or source_build.get('gui_libraries_compiled_from_source') is not True:
            parser.error('The GUI source build must be complete')
        if set(source_build['packages']) != set(pins['packages']):
            parser.error('Source-built packages must cover the entire pinned GUI closure')
        for name, expected in source_build['source_sha256'].items():
            if digest(ROOT / name) != expected:
                raise ValueError('GUI source build recipe changed: ' + name)
        for name, asset in source_build['packages'].items():
            if asset.get('compiled_from_source') is not True or digest(Path(asset['path'])) != asset['sha256']:
                raise ValueError('Source-built GUI package changed: ' + name)
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=False)
    runtime = out / 'runtime'
    runtime.mkdir()
    vendor = args.vendor_root.resolve()
    vendor.mkdir(parents=True, exist_ok=True)
    if not source_build:
        with ThreadPoolExecutor(max_workers=args.jobs) as executor:
            list(executor.map(lambda record: fetch(record, vendor), pins['packages'].values()))
    files, aliases = {}, {}

    def put(relative, data, mode, component):
        relative = canonical(relative)
        target = runtime / relative
        if relative in aliases:
            raise ValueError('File conflicts with a runtime alias: ' + relative)
        if relative in files:
            if target.read_bytes() != data:
                raise ValueError('Conflicting GNU runtime file: ' + relative)
            return
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        target.chmod(mode)
        files[relative] = dict(path=str(target), sha256=digest(target), mode=mode, component=component)

    def link(relative, target):
        relative = canonical(relative)
        if relative in files or relative in aliases and aliases[relative] != target:
            raise ValueError('Conflicting runtime alias: ' + relative)
        aliases[relative] = target

    for package, pin in pins['packages'].items():
        archive = Path(source_build['packages'][package]['path']) if source_build else vendor / pin['archive']
        with package_tar(archive, 'data') as bundle:
            for member in bundle:
                name = canonical(member.name)
                library = name.startswith(('usr/lib/x86_64-linux-gnu/', 'usr/lib/')) and bool(re.search(r'\.so(?:\.|$)', PurePosixPath(name).name))
                notice = name.startswith('usr/share/doc/') and PurePosixPath(name).name == 'copyright'
                if not library and not notice:
                    continue
                if member.issym():
                    raw = member.linkname
                    original = member.name.removeprefix('./')
                    resolved = raw.lstrip('/') if raw.startswith('/') else posixpath.normpath(posixpath.join(posixpath.dirname(original), raw))
                    target = canonical(resolved)
                    link(name, posixpath.relpath(target, posixpath.dirname(name)))
                elif member.isfile() or member.islnk():
                    with bundle.extractfile(member) as source:
                        data = source.read()
                    if library and data[:4] != b'\x7fELF':
                        raise ValueError('Expected an ELF shared library: ' + name)
                    put(name, data, 0o755 if member.mode & 0o111 else 0o644,
                        ('source:debian:' if source_build else 'debian:') + package)
    for component, record in records.items():
        for relative, asset in record['files'].items():
            library = relative.startswith('usr/lib/x86_64-linux-gnu/') and '.so' in PurePosixPath(relative).name
            notice = relative.startswith('usr/share/licenses/')
            if not library and not notice:
                continue
            source = Path(asset['path'])
            if digest(source) != asset['sha256']:
                raise ValueError('Source-built GNU runtime changed: ' + relative)
            data = source.read_bytes()
            if library and data[:4] != b'\x7fELF':
                continue  # GNU linker scripts belong in the development SDK.
            put(relative, data, asset['mode'], 'source:' + component)
        for relative, target in record.get('aliases', {}).items():
            if relative.startswith('usr/lib/x86_64-linux-gnu/') and '.so' in PurePosixPath(relative).name:
                link(relative, target)
    loader = runtime / 'usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2'
    put('lib64/ld-linux-x86-64.so.2', loader.read_bytes(), 0o755, 'source:glibc')
    for relative, target in aliases.items():
        path = runtime / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.symlink_to(target)
    for relative in aliases:
        target = (runtime / relative).resolve(strict=True)
        if not target.is_relative_to(runtime) or not target.is_file():
            raise ValueError('Runtime alias escapes its staged libraries: ' + relative)
    available = {PurePosixPath(name).name for name in [*files, *aliases] if '.so' in PurePosixPath(name).name}
    closure = {}
    for name in files:
        if '.so' not in PurePosixPath(name).name or name.startswith('usr/share/'):
            continue
        result = subprocess.check_output(['readelf', '-h', '-d', runtime / name], text=True)
        if 'Advanced Micro Devices X86-64' not in result:
            raise ValueError('Unexpected shared-library architecture: ' + name)
        needed = re.findall(r'\(NEEDED\).*?\[([^\]]+)\]', result)
        if missing := set(needed) - available:
            raise ValueError('Missing GUI library dependencies for ' + name + ': ' + ', '.join(sorted(missing)))
        closure[name] = needed
    (out / 'BUILD.json').write_text(json.dumps(dict(
        state='GNU GUI runtime staged; Coast image qualification pending', files=files, aliases=aliases,
        needed=closure, component_build_sha256={name: digest(path) for name, path in receipts.items()},
        bootstrap_manifest_sha256=None if source_build else digest(PINS),
        bootstrap_archives={} if source_build else {p['archive']: p['sha256'] for p in pins['packages'].values()},
        gui_source_build_sha256=digest(args.source_build) if source_build else None,
        gui_source_packages=source_build['packages'] if source_build else {},
        source_sha256={str(p.relative_to(ROOT)): digest(p) for p in [Path(__file__), PINS,
            ROOT / 'tools/debian_package.py', ROOT / 'tools/fetch_sources.py']},
        gui_libraries_compiled_from_source=bool(source_build), gnu_abi_compiled_from_source=True,
        target_execution_on_linux=False, maintainer_scripts_executed=False), indent=2) + '\n')
    print('Pinned public GUI runtime staged:', out, flush=True)


if __name__ == '__main__':
    main()

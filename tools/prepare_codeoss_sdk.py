#!/usr/bin/env python3
"""Prepare an isolated GNU SDK for Code OSS's source-built native modules."""
from build_state import project_lock
from build_state import atomic_write
import argparse
import io
import json
from pathlib import Path, PurePosixPath
import posixpath
import subprocess
import sys
import tarfile

from fetch_sources import fetch
from prepare_gui_runtime import canonical
from source_tree import digest

ROOT = Path(__file__).resolve().parents[1]
HEADERS = ROOT / 'ports/codeoss-app/native-headers.json'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('glibc', 'libgcc', 'cxx'):
        parser.add_argument('--' + name + '-build', type=Path, required=True)
    parser.add_argument('--gui-runtime', type=Path, required=True)
    parser.add_argument('--output-root', type=Path, required=True)
    args = parser.parse_args()
    out = args.output_root.resolve()
    command = [sys.executable, '-B', ROOT / 'tools/prepare_gnu_sdk.py', '--output-root', out]
    for name in ('glibc', 'libgcc', 'cxx'):
        command += ['--' + name + '-build', getattr(args, name + '_build').resolve(strict=True)]
    subprocess.run(list(map(str, command)), check=True)
    sysroot = out / 'sysroot'
    gui = json.loads(args.gui_runtime.read_text())
    for name, expected in gui['source_sha256'].items():
        if digest(ROOT / name) != expected:
            raise ValueError('GUI runtime recipe changed: ' + name)
    if gui['component_build_sha256'] != json.loads((out / 'SDK.json').read_text())['component_build_sha256']:
        raise ValueError('GUI runtime and SDK use different GNU ABI components')
    files, aliases = {}, {}

    def put(name, data, component):
        name = canonical(name)
        target = sysroot / name
        if target.exists() or target.is_symlink():
            if target.is_symlink() or target.read_bytes() != data:
                raise ValueError('Application SDK file conflict: ' + name)
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            target.chmod(0o644)
        files[name] = dict(sha256=digest(target), component=component)

    def link(name, target):
        name = canonical(name)
        path = sysroot / name
        if path.exists() or path.is_symlink():
            if not path.is_symlink() or path.readlink().as_posix() != target:
                raise ValueError('Application SDK alias conflict: ' + name)
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.symlink_to(target)
        aliases[name] = target

    for name, asset in gui['files'].items():
        source = Path(asset['path'])
        if digest(source) != asset['sha256']:
            raise ValueError('GUI runtime library changed: ' + name)
        put(name, source.read_bytes(), 'gui-runtime')
    for name, target in gui['aliases'].items():
        link(name, target)
    vendor = ROOT / 'build/codeoss-native-headers/vendor'
    packages = json.loads(HEADERS.read_text())['packages']
    for package, pin in packages.items():
        fetch(pin, vendor)
        archive = vendor / pin['archive']
        members = subprocess.check_output(['ar', 't', archive], text=True).splitlines()
        data_name, = [n for n in members if n.startswith('data.tar.')]
        compressed = subprocess.check_output(['ar', 'p', archive, data_name])
        if data_name.endswith('.zst'):
            compressed = subprocess.check_output(['zstd', '-dc'], input=compressed)
        with tarfile.open(fileobj=io.BytesIO(compressed), mode='r:*') as bundle:
            for member in bundle:
                name = canonical(member.name)
                source_header = name.startswith('usr/include/')
                pkgconfig = name.startswith(('usr/lib/x86_64-linux-gnu/pkgconfig/', 'usr/share/pkgconfig/')) and name.endswith('.pc')
                library_alias = name.startswith('usr/lib/x86_64-linux-gnu/') and name.endswith('.so') and member.issym()
                notice = name == 'usr/share/doc/' + package + '/copyright'
                if not (source_header or pkgconfig or library_alias or notice):
                    continue
                if member.issym():
                    raw = member.linkname
                    original = member.name.removeprefix('./')
                    resolved = raw.lstrip('/') if raw.startswith('/') else posixpath.normpath(posixpath.join(posixpath.dirname(original), raw))
                    target = canonical(resolved)
                    if library_alias and not (sysroot / target).is_file():
                        continue  # Expose only libraries supplied by this runtime.
                    link(name, posixpath.relpath(target, posixpath.dirname(name)))
                elif member.isfile() or member.islnk():
                    with bundle.extractfile(member) as stream:
                        data = stream.read()
                    if data.startswith((b'\x7fELF', b'!<arch>\n')):
                        raise ValueError('Development header package unexpectedly supplied compiled code')
                    put(name, data, 'headers:' + package)
    for name in aliases:
        target = (sysroot / name).resolve(strict=True)
        if not target.is_relative_to(sysroot) or not (target.is_file() or target.is_dir()):
            raise ValueError('Application SDK alias leaves its staged inputs: ' + name)
    inputs = [Path(__file__), HEADERS, ROOT / 'tools/prepare_gnu_sdk.py', ROOT / 'tools/prepare_gui_runtime.py']
    atomic_write(out / 'APPLICATION-SDK.json', json.dumps(dict(
        state='GNU application SDK staged; native-module consumer qualification pending',
        gnu_sdk_sha256=digest(out / 'SDK.json'), gui_runtime_sha256=digest(args.gui_runtime), files=files, aliases=aliases,
        header_archives={p['archive']: p['sha256'] for p in packages.values()},
        source_sha256={str(p.relative_to(ROOT)): digest(p) for p in inputs},
        host_headers_used=False, maintainer_scripts_executed=False), indent=2) + '\n')
    print('Code OSS GNU application SDK ready:', out, flush=True)


if __name__ == '__main__':
    with project_lock():
        main()

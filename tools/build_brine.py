#!/usr/bin/env python3
"""Build the Brine client with explicit, optional repository trust."""
from build_state import project_lock
from build_state import atomic_write
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess

from build_desktop import verify_receipt
from source_tree import digest

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler-build', type=Path, required=True)
    parser.add_argument('--program-build', type=Path, required=True)
    parser.add_argument('--output-root', type=Path, required=True)
    parser.add_argument('--repository-config', type=Path,
                        help='JSON with url, signing_fingerprint and public_key; no private keys')
    args = parser.parse_args()
    compiler_path, program_path = args.compiler_build.resolve(strict=True), args.program_build.resolve(strict=True)
    compiler, program = verify_receipt(compiler_path), verify_receipt(program_path)
    if compiler.get('thread_model') != 'posix' or program.get('compiler_build_sha256') != digest(compiler_path):
        parser.error('Use Python and the matching source-built POSIX musl compiler')
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=False)
    runtime = out / 'runtime'
    inputs = [Path(__file__), ROOT / 'LICENSE', *[p for p in (ROOT / 'userland/packages').rglob('*') if p.is_file()]]
    before = {str(p.relative_to(ROOT)): digest(p) for p in inputs}
    cc = compiler_path.parent / 'cross/bin/x86_64-linux-musl-gcc'
    confinement = ['bwrap', '--unshare-all', '--die-with-parent', '--new-session', '--ro-bind', '/', '/',
                   '--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp', '--bind', str(out), str(out)]
    binary = out / 'brine'
    command = list(map(str, [cc, '-O2', '-static', '-no-pie', '-fno-stack-protector',
        '-Wl,-z,relro,-z,now,-z,noexecstack', ROOT / 'userland/packages/launcher.c', '-o', binary]))
    subprocess.run([*confinement, '--', *command], stdin=subprocess.DEVNULL, check=True,
                   env=dict(PATH='/usr/bin:/bin', HOME='/tmp', LANG='C.UTF-8'))

    def copy(source, name, mode=None):
        path = runtime / name
        path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, path)
        if mode is not None:
            path.chmod(mode)

    copy(binary, 'usr/bin/brine', 0o755)
    for name in ('client.py', 'search.py', 'community.py', 'configure.py'):
        copy(ROOT / 'userland/packages' / name, 'usr/lib/brine/' + name)
    for path in (ROOT / 'userland/packages/recipes').rglob('*'):
        if path.is_file():
            copy(path, 'usr/share/brine/recipes/' + str(path.relative_to(ROOT / 'userland/packages/recipes')))
    for path in (ROOT / 'userland/packages/web').iterdir():
        if path.is_file():
            copy(path, 'usr/share/brine/web/' + path.name)
    for name in ('pacman.conf', 'makepkg.conf'):
        copy(ROOT / 'userland/packages/config' / name, 'etc/' + name)
    copy(ROOT / 'LICENSE', 'usr/share/licenses/brine/LICENSE')
    config = runtime / 'etc/brine/config.json'
    config.parent.mkdir(parents=True, exist_ok=True)
    atomic_write(config, json.dumps(dict(version=1, url=None, signing_fingerprint=None, community_url=None), indent=2) + '\n')
    config.chmod(0o644)  # Repository URL and public fingerprint must be readable by desktop users.
    keyring = runtime / 'etc/pacman.d/gnupg'
    keyring.mkdir(parents=True)
    configured = None
    if args.repository_config:
        selection = json.loads(args.repository_config.read_text())
        if set(selection) != {'url', 'signing_fingerprint', 'public_key'}:
            parser.error('Repository config must contain only url, signing_fingerprint, public_key')
        key = Path(selection['public_key'])
        if not key.is_absolute():
            key = args.repository_config.resolve().parent / key
        spec = importlib.util.spec_from_file_location('brine_client', ROOT / 'userland/packages/client.py')
        client = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(client)
        spec = importlib.util.spec_from_file_location('brine_configure', ROOT / 'userland/packages/configure.py')
        configure = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(configure)
        url = client.server_origin(selection['url'])
        configure.initialize(url, key, selection['signing_fingerprint'], root=runtime, owner=os.getuid(), server_texts=client.server_config_texts)
        configured = dict(url=url, signing_fingerprint=selection['signing_fingerprint'], public_key_sha256=digest(key),
                          configuration_sha256=digest(args.repository_config))
    if any(digest(ROOT / name) != expected for name, expected in before.items()):
        raise ValueError('Brine sources changed during compilation')
    files = {str(p.relative_to(runtime)): dict(path=str(p), sha256=digest(p), mode=p.stat().st_mode & 0o777)
             for p in sorted(runtime.rglob('*')) if p.is_file()}
    record = dict(state='compiled; guest qualification pending', target_host_executed=False,
        compiler_build_sha256=digest(compiler_path), program_build_sha256=digest(program_path),
        source_sha256=before, files=files, aliases={}, directories=['etc/brine', 'etc/pacman.d/gnupg'],
        repository=configured, inherited_signing_identity=False, confinement=confinement, commands=[command])
    atomic_write(out / 'BUILD.json', json.dumps(record, indent=2) + '\n')
    print('Brine staged:', runtime, flush=True)


if __name__ == '__main__':
    with project_lock():
        main()

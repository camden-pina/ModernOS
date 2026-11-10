#!/usr/bin/env python3
"""Record a manual GN build for exact-binary Coast kernel admission.

Use this after running GN/Ninja yourself, including in a Coast guest. It
checks the pinned source overlays and arguments, strips copies of the two
products, and records their hashes. It never executes either runtime and
does not claim that a manual native build or its runtime has been qualified.
"""
from build_state import project_lock
from build_state import atomic_write
import argparse
import json
from pathlib import Path
import platform
import subprocess

from build_electron_source import LOCK, ROOT, sha, verify
from electron_admission import prepare


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--checkout', type=Path, required=True)
    parser.add_argument('--electron-output', type=Path, required=True)
    parser.add_argument('--node-output', type=Path, required=True)
    parser.add_argument('--output-root', type=Path, required=True)
    parser.add_argument('--native', action='store_true')
    parser.add_argument('--strip', default='strip')
    args = parser.parse_args()
    if args.native and platform.system() != 'Coast':
        parser.error('--native requires Coast')
    checkout = args.checkout.resolve(strict=True)
    verify(checkout, json.loads(LOCK.read_text()), overlays=True)
    graph = {name: getattr(args, name + '_output').resolve(strict=True) for name in ['electron', 'node']}
    for name, directory in graph.items():
        if sha(directory / 'args.gn') != sha(ROOT / 'ports/electron/args.gn'):
            parser.error('Use the exact checked-in Coast GN arguments: ' + name)
        if not (directory / 'build.ninja').is_file() or not (directory / '.ninja_log').is_file():
            parser.error('A completed GN/Ninja output graph is required: ' + name)
        with (directory / name).open('rb') as product:
            magic = product.read(4)
        if magic != b'\x7fELF':
            parser.error('Missing linked ELF product: ' + name)
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=False)
    products = {}
    for name, directory in graph.items():
        binary = out / (name + '-stripped')
        subprocess.run([args.strip, '--strip-all', '-o', str(binary), str(directory / name)], check=True)
        products[name] = dict(path=str(binary), sha256=sha(binary),
            unstripped=str(directory / name), unstripped_sha256=sha(directory / name))
    names = ['snapshot_blob.bin', 'resources.pak', 'chrome_100_percent.pak',
             'chrome_200_percent.pak', 'icudtl.dat', 'libffmpeg.so']
    sidecars = {name: dict(path=str(graph['electron'] / name), sha256=sha(graph['electron'] / name))
                for name in names if (graph['electron'] / name).is_file()}
    required = {'resources.pak', 'chrome_100_percent.pak', 'chrome_200_percent.pak', 'icudtl.dat', 'libffmpeg.so'}
    if required - sidecars.keys():
        parser.error('Incomplete Electron sidecars: ' + ', '.join(sorted(required - sidecars.keys())))
    record = dict(state='source compiled; guest qualification pending', products=products,
        binary=products['electron']['path'], binary_sha256=products['electron']['sha256'],
        source_lock_sha256=sha(LOCK), args_sha256=sha(ROOT / 'ports/electron/args.gn'),
        guest_execution_on_host=False, target_host_executed=False,
        features=dict(jitless_compiled=True, drumbrake_explicit_bounds_checks=True), sidecars=sidecars,
        build_mode='manual-native-coast' if args.native else 'manual-linux', runtime_executed_by_recorder=False,
        source_policy_sha256={path: sha(ROOT / path) for path in json.loads(LOCK.read_text())['overlays'].values()},
        graphs={name: dict(args_sha256=sha(directory / 'args.gn'),
            ninja_sha256=sha(directory / 'build.ninja'), log_sha256=sha(directory / '.ninja_log'))
            for name, directory in graph.items()},
        recorder_sha256=sha(Path(__file__)))
    receipt = out / 'BUILD.json'
    atomic_write(receipt, json.dumps(record, indent=2) + '\n')
    # Apply the same ELF bounds, W^X, source-policy and canonical path checks
    # used by the kernel builder before accepting a manual build receipt.
    prepare(receipt, out / 'electron-admission.h')
    license_path = checkout / 'src/third_party/electron_node/LICENSE'
    atomic_write(out / 'NODE.json', json.dumps(dict(binary=products['node']['path'],
        binary_sha256=products['node']['sha256'], parent_electron_sha256=products['electron']['sha256'],
        guest_execution_on_host=False, sidecars={name: sidecars[name] for name in ['snapshot_blob.bin', 'icudtl.dat'] if name in sidecars},
        license=dict(path=str(license_path), sha256=sha(license_path)),
        state='source compiled; guest qualification pending'), indent=2) + '\n')
    print('Manual Electron build recorded; qualify inside Coast:', receipt, flush=True)


if __name__ == '__main__':
    with project_lock():
        main()

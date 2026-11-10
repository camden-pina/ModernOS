#!/usr/bin/env python3
"""Record complete resources from an existing verified source build, preserving it."""
import argparse
import json
from pathlib import Path

from build_electron_source import LOCK, ROOT, runtime_sidecars, verify
from electron_admission import prepare
from source_tree import digest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime-build', type=Path, required=True)
    parser.add_argument('--checkout', type=Path, required=True)
    parser.add_argument('--output-root', type=Path, required=True)
    args = parser.parse_args()
    receipt = args.runtime_build.resolve(strict=True)
    checkout = args.checkout.resolve(strict=True)
    lock = json.loads(LOCK.read_text())
    verify(checkout, lock)
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=False)
    prepare(receipt, out / 'original-admission.h')
    record = json.loads(receipt.read_text())
    binary = Path(record['products']['electron']['unstripped'])
    if digest(binary) != record['products']['electron']['unstripped_sha256']:
        raise ValueError('The linked source-built Electron product changed')
    record['sidecars'] = runtime_sidecars(binary.parent)
    record['source_policy_sha256'] = {p: digest(ROOT / p) for p in lock['overlays'].values()}
    record['parent_source_build_sha256'] = digest(receipt)
    record['source_checkout'] = str(checkout)
    (out / 'BUILD.json').write_text(json.dumps(record, indent=2) + '\n')
    node = json.loads(receipt.with_name('NODE.json').read_text())
    (out / 'NODE.json').write_text(json.dumps(node, indent=2) + '\n')
    prepare(out / 'BUILD.json', out / 'electron-admission.h')
    print('Complete source-built Electron runtime:', out)


if __name__ == '__main__':
    main()

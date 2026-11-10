#!/usr/bin/env python3
"""Generate exact-binary kernel admission from a verified source-build receipt."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]
IMAGE_LIMIT = 256 * 1024 * 1024


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def prepare(receipt, output, source_lock=ROOT / 'ports/electron/sources.lock.json'):
    receipt, output = Path(receipt).resolve(strict=True), Path(output).resolve()
    record = json.loads(receipt.read_text())
    lock_hash = sha(source_lock)
    if (record.get('source_lock_sha256') != lock_hash or
            not record.get('state', '').startswith('source compiled;') or
            record.get('guest_execution_on_host') is not False or
            record.get('features') != dict(jitless_compiled=True, drumbrake_explicit_bounds_checks=True)):
        raise ValueError('Select a completed source build with the current pinned Coast runtime policies')
    pins = json.loads(source_lock.read_text())
    policies = {path: sha(ROOT / path) for path in pins.get('overlays', {}).values()}
    recorded_policies = record.get('source_policy_sha256')
    if recorded_policies is None:
        # Automatic builds retain their input identity alongside BUILD.json.
        # Manual build receipts carry the same policy hashes directly.
        identity_path = next((receipt.parent / name for name in ['CONFIGURE.json', 'SOURCE.json']
                              if (receipt.parent / name).is_file()), None)
        if identity_path is None:
            raise ValueError('The runtime receipt lacks its source policy identity')
        identity = json.loads(identity_path.read_text())
        if identity.get('source_lock_sha256', identity.get('lock_sha256')) != lock_hash:
            raise ValueError('The runtime source identity uses a different graph')
        recorded_policies = identity.get('overlays')
    if recorded_policies != policies:
        raise ValueError('The runtime source policy changed; rebuild the selected runtime')
    args_hash = sha(ROOT / 'ports/electron/args.gn')
    if (record.get('args_sha256') != args_hash or
            any(graph.get('args_sha256') != args_hash for graph in record.get('graphs', {}).values())):
        raise ValueError('The runtime GN arguments changed; rebuild the selected runtime')
    products = {}
    for name in ('electron', 'node'):
        product = record['products'][name]
        binary = Path(product['path']).resolve(strict=True)
        size = binary.stat().st_size
        if not 64 <= size <= IMAGE_LIMIT or sha(binary) != product['sha256']:
            raise ValueError('Runtime bytes differ from the source-build receipt: ' + name)
        with binary.open('rb') as stream:
            header = stream.read(64)
            if (header[:6] != b'\x7fELF\x02\x01' or
                    struct.unpack_from('<H', header, 16)[0] not in (2, 3) or
                    struct.unpack_from('<H', header, 18)[0] != 62):
                raise ValueError('Runtime must be an x86-64 little-endian ELF: ' + name)
            phoff, = struct.unpack_from('<Q', header, 32)
            phsize, count = struct.unpack_from('<HH', header, 54)
            if phsize != 56 or not 1 <= count <= 128 or phoff > size or count * phsize > size - phoff:
                raise ValueError('Invalid runtime program headers: ' + name)
            stream.seek(phoff)
            headers = [struct.unpack('<IIQQQQQQ', stream.read(phsize)) for _ in range(count)]
        loads = [p for p in headers if p[0] == 1]
        if (not loads or any(p[1] & 3 == 3 or p[5] > p[6] or p[2] > size or p[5] > size - p[2]
                or p[3] + p[6] > 0x0000800000000000 for p in loads) or
                max(p[3] + p[6] for p in loads) - min(p[3] for p in loads) > IMAGE_LIMIT):
            raise ValueError('Runtime segments exceed the bounded non-WX profile: ' + name)
        products[name] = dict(path=str(binary), sha256=product['sha256'], bytes=size)
    if (record.get('binary_sha256') != products['electron']['sha256'] or
            Path(record['binary']).resolve() != Path(products['electron']['path'])):
        raise ValueError('The selected Electron product differs from its receipt')
    text = '/* Generated from verified source-built ELF bytes; canonical paths remain enforced. */\n'
    for name in ('electron', 'node'):
        octets = bytes.fromhex(products[name]['sha256'])
        text += '#define COAST_' + name.upper() + '_SHA256 {' + ','.join(f'0x{n:02x}' for n in octets) + '}\n'
    output.parent.mkdir(parents=True, exist_ok=True)
    if not output.exists() or output.read_text() != text:
        output.write_text(text)
    evidence = dict(source_build_sha256=sha(receipt), source_lock_sha256=lock_hash,
        source_policy_sha256=policies,
        header_sha256=sha(output), products=products, canonical_paths={
            name: '/opt/electron-43.7.3/' + name for name in products},
        target_executed=False)
    output.with_suffix('.json').write_text(json.dumps(evidence, indent=2) + '\n')
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime-build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    print(prepare(args.runtime_build, args.output))


if __name__ == '__main__':
    main()

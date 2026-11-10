#!/usr/bin/env python3
"""Build Code OSS's proxy resolver against the matching source-built GNU SDK.

Pinned Rust host tools stay local to this build. Cargo uses the upstream
lockfile. The upstream wasm2c PAC backend retains explicit memory bounds
checks and has no runtime JIT. Its vendored PAC wasm is recorded separately.
"""
from build_state import project_lock
from build_state import atomic_write
import argparse
import fcntl
import json
from pathlib import Path
import re
import shutil
import subprocess

from fetch_sources import fetch
from source_archive import unpack
from source_tree import digest

ROOT = Path(__file__).resolve().parents[1]
PINS = ROOT / 'ports/codeoss-app/rust-addons.json'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--application-build', type=Path)
    parser.add_argument('--sdk', type=Path)
    parser.add_argument('--output-root', type=Path, required=True)
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--prepare-only', action='store_true', help='download host tools and build the pinned host wasm2c')
    parser.add_argument('--reconfigure', action='store_true', help='accept builder-only changes with the same pinned sources and host tools')
    args = parser.parse_args()
    if not 1 <= args.jobs <= 24:
        parser.error('--jobs must be 1..24')
    if not args.prepare_only and (not args.application_build or not args.sdk):
        parser.error('Compilation requires --application-build and --sdk')
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=True)
    lock = (out / '.build.lock').open('a')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    pins = json.loads(PINS.read_text())
    inputs = [Path(__file__), PINS, ROOT / 'tools/fetch_sources.py', ROOT / 'tools/source_archive.py']
    identity = {str(p.relative_to(ROOT)): digest(p) for p in inputs}
    stamp = out / 'SOURCE.json'
    if stamp.exists() and json.loads(stamp.read_text()) != identity:
        previous = json.loads(stamp.read_text())
        builder = str(Path(__file__).relative_to(ROOT))
        if not args.reconfigure or {k: v for k, v in previous.items() if k != builder} != {k: v for k, v in identity.items() if k != builder}:
            raise ValueError('Rust addon build recipes changed; select a fresh output root or --reconfigure for builder-only changes')
        history = out / 'configuration-history'
        history.mkdir(exist_ok=True)
        snapshot = history / (digest(stamp) + '.json')
        if not snapshot.exists():
            shutil.copyfile(stamp, snapshot)
    atomic_write(stamp, json.dumps(identity, indent=2) + '\n')
    vendor = out / 'vendor'
    vendor.mkdir(exist_ok=True)
    for pin in [pins['source'], pins['wabt'], *pins['rust_host_tools'].values()]:
        fetch(pin, vendor)
        if not (out / pin['source']).exists():
            unpack(vendor / pin['archive'], out)
    rust = out / 'host-tools/rust'
    rust.mkdir(parents=True, exist_ok=True)
    for name, pin in pins['rust_host_tools'].items():
        component = 'rust-std-x86_64-unknown-linux-gnu' if name == 'rust-std' else name
        shutil.copytree(out / pin['source'] / component, rust, dirs_exist_ok=True, symlinks=True)
    home = out / 'home'
    home.mkdir(exist_ok=True)
    environment = dict(PATH=str(rust / 'bin') + ':/usr/bin:/bin', HOME=str(home), TMPDIR='/tmp', LANG='C.UTF-8',
        CARGO_HOME=str(out / 'cargo-home'), RUSTC=str(rust / 'bin/rustc'),
        GIT_TERMINAL_PROMPT='0', CARGO_NET_RETRY='3', PYTHONDONTWRITEBYTECODE='1')
    confinement = ['bwrap', '--unshare-user', '--unshare-pid', '--unshare-ipc', '--unshare-uts',
        '--die-with-parent', '--new-session', '--ro-bind', '/', '/', '--proc', '/proc', '--dev', '/dev',
        '--tmpfs', '/tmp', '--bind', str(out), str(out)]
    commands = []

    def run(name, command, cwd=out, extra=None):
        command = list(map(str, command))
        commands.append(dict(name=name, argv=command, cwd=str(cwd)))
        print('Code OSS Rust: ' + name, flush=True)
        with (out / (name + '.log')).open('w') as log:
            subprocess.run([*confinement, '--chdir', str(cwd), '--', *command],
                env={**environment, **(extra or {})}, stdin=subprocess.DEVNULL,
                stdout=log, stderr=subprocess.STDOUT, check=True)

    wabt = out / 'wabt-build'
    if not (wabt / 'build.ninja').exists():
        run('wasm2c-configure', ['cmake', '-S', out / pins['wabt']['source'], '-B', wabt, '-G', 'Ninja',
            '-DCMAKE_BUILD_TYPE=Release', '-DBUILD_TESTS=OFF', '-DBUILD_LIBWASM=OFF'])
    run('wasm2c-build', ['cmake', '--build', wabt, '--target', 'wasm2c', '-j', args.jobs])
    wasm2c = wabt / 'wasm2c'
    version = subprocess.check_output([wasm2c, '--version'], text=True).strip()
    if version != pins['wabt']['version']:
        raise ValueError('The host wasm2c version differs from the pinned PAC runtime')
    if args.prepare_only:
        print('Pinned Rust host tools and source-built wasm2c prepared', flush=True)
        return
    application = args.application_build.resolve(strict=True)
    module = application / 'workspace/vscode/node_modules/@vscodium/os-proxy-resolver/package.json'
    if json.loads(module.read_text())['version'] != pins['module_version']:
        raise ValueError('The proxy resolver differs from the application lockfile')
    sdk = args.sdk.resolve(strict=True)
    sdk_record = json.loads((sdk / 'APPLICATION-SDK.json').read_text())
    if sdk_record['gnu_sdk_sha256'] != digest(sdk / 'SDK.json'):
        raise ValueError('The GNU application SDK changed')
    gnu_sdk = json.loads((sdk / 'SDK.json').read_text())
    for record in [sdk_record, gnu_sdk]:
        for name, expected in record['source_sha256'].items():
            if digest(ROOT / name) != expected:
                raise ValueError('GNU SDK recipe changed: ' + name)
        for name, asset in record['files'].items():
            if digest(sdk / 'sysroot' / name) != asset['sha256']:
                raise ValueError('GNU SDK input changed: ' + name)
    source = out / pins['source']['source']
    lockfile = source / 'npm/native/Cargo.lock'
    lock_hash = digest(lockfile)
    extra = dict(CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER=str(sdk / 'bin/gcc'),
        CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_RUSTFLAGS='-C target-cpu=x86-64 -C target-feature=-avx,-avx2 -C link-arg=-Wl,-z,relro,-z,now,-z,noexecstack',
        CC_x86_64_unknown_linux_gnu=str(sdk / 'bin/gcc'), AR_x86_64_unknown_linux_gnu='ar',
        CFLAGS_x86_64_unknown_linux_gnu='-O2 -g0 -fPIC -mno-avx -mno-avx2',
        OS_PROXY_RESOLVER_WASM2C=str(wasm2c))
    run('cargo-build', [rust / 'bin/cargo', 'build', '--locked', '--release',
        '--manifest-path', source / 'npm/native/Cargo.toml', '--target', 'x86_64-unknown-linux-gnu',
        '--target-dir', out / 'target', '--jobs', args.jobs], source, extra)
    product = out / 'target/x86_64-unknown-linux-gnu/release/libos_proxy_resolver_node.so'
    elf = subprocess.check_output(['readelf', '-h', '-lW', '-d', '--version-info', product], text=True)
    libraries = sdk / 'sysroot/usr/lib/x86_64-linux-gnu'
    available = set()
    for name in ('libc.so.6', 'libm.so.6', 'libgcc_s.so.1', 'libstdc++.so.6'):
        available.update(re.findall(r'\b(?:GLIBC|GLIBCXX|CXXABI|GCC)_[A-Za-z0-9_.]+',
            subprocess.check_output(['readelf', '--version-info', libraries / name], text=True)))
    required = set(re.findall(r'\b(?:GLIBC|GLIBCXX|CXXABI|GCC)_[A-Za-z0-9_.]+', elf))
    needed = set(re.findall(r'\(NEEDED\).*?\[([^\]]+)\]', elf))
    if ('Advanced Micro Devices X86-64' not in elf or ' RWE ' in elf or required - available or
            needed - {'libgcc_s.so.1', 'libm.so.6', 'libc.so.6', 'ld-linux-x86-64.so.2'}):
        raise ValueError('The proxy resolver requires code/ABI outside its GNU SDK: ' + repr(sorted(required - available)))
    if digest(lockfile) != lock_hash or any(digest(ROOT / name) != sha for name, sha in identity.items()):
        raise ValueError('Rust source inputs changed while compiling')
    relative = 'node_modules.asar.unpacked/@vscodium/os-proxy-resolver-linux-x64-gnu/os_proxy_resolver.node'
    licenses = {}
    for name, path in [('proxy-resolver/LICENSE.txt', source / 'LICENSE.txt'),
            ('proxy-resolver/ThirdPartyNotices.txt', source / 'npm/ThirdPartyNotices.txt'),
            ('proxy-wasm-rt/LICENSE', source / 'pac-wasm-guest/wasm-rt/LICENSE'),
            *[('proxy-rust/' + name, out / pins['rust_host_tools']['rustc']['source'] / name)
              for name in ('LICENSE-APACHE', 'LICENSE-MIT')]]:
        licenses['usr/share/licenses/codeoss-' + name] = dict(path=str(path), sha256=digest(path), mode=0o644)
    record = dict(state='Rust proxy resolver source compiled; Coast qualification pending',
        files={relative: dict(path=str(product), sha256=digest(product), needed=sorted(needed), abi_versions=sorted(required))},
        application_source_receipt_sha256=digest(application / 'SOURCE.json'),
        sdk_receipt_sha256=digest(sdk / 'APPLICATION-SDK.json'), gui_runtime_sha256=sdk_record['gui_runtime_sha256'],
        gnu_component_build_sha256=gnu_sdk['component_build_sha256'],
        source_sha256=identity, cargo_lock_sha256=lock_hash, sources=pins, commands=commands,
        rust_host_tools={name: pin['sha256'] for name, pin in pins['rust_host_tools'].items()},
        host_wasm2c_sha256=digest(wasm2c), jit_enabled=False, explicit_pac_memory_bounds_checks=True,
        embedded_upstream_pac_wasm_sha256=digest(source / 'pac-wasm-guest/pac_guest.wasm'),
        embedded_pac_wasm_rebuilt_from_source=False, licenses=licenses, target_execution_on_linux=False)
    atomic_write(out / 'BUILD.json', json.dumps(record, indent=2) + '\n')
    print('Code OSS proxy resolver rebuilt for the source GNU ABI', flush=True)


if __name__ == '__main__':
    with project_lock():
        main()

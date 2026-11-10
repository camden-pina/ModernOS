#!/usr/bin/env python3
"""Rebuild pinned Code OSS C/C++ modules using the isolated GNU application SDK.

Builds separate copies of module sources; the prepared application is read-only.
Static ELF/ABI checks precede image assembly. Loading the modules inside Coast
is a separate qualification step. Rust and authentication binaries are outside
this C/C++ recipe's coverage.
"""
from build_state import project_lock
from build_state import atomic_write
import argparse
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess

from fetch_sources import fetch
from prepare_electron_host import prepare_node
from source_archive import unpack
from source_tree import digest

ROOT = Path(__file__).resolve().parents[1]
MODULES = ROOT / 'ports/codeoss-app/native-modules.json'
HEADERS = ROOT / 'ports/codeoss-app/electron-headers.json'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--application-build', type=Path, required=True, help='build_codeoss_source output directory')
    parser.add_argument('--sdk', type=Path, required=True, help='prepare_codeoss_sdk output directory')
    parser.add_argument('--electron-checkout', type=Path)
    parser.add_argument('--native', action='store_true')
    parser.add_argument('--host-bin', type=Path, help='working native Node/npm tools on Coast')
    parser.add_argument('--node-gyp', type=Path, help='node-gyp JavaScript entry point on Coast')
    parser.add_argument('--output-root', type=Path, required=True)
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    if not 1 <= args.jobs <= 24 or args.native and platform.system() != 'Coast':
        parser.error('Use --jobs 1..24; --native requires Coast')
    if args.native and (not args.host_bin or not args.node_gyp) or not args.native and not args.electron_checkout:
        parser.error('Use --electron-checkout on Linux, or --host-bin and --node-gyp with --native')
    source_build = args.application_build.resolve(strict=True)
    source = source_build / 'workspace/vscode'
    for relative, expected in json.loads((source_build / 'INSTALLED.json').read_text())['lockfiles'].items():
        if digest(source / relative) != expected:
            raise ValueError('Application dependency lock changed: ' + relative)
    sdk = args.sdk.resolve(strict=True)
    application_sdk = json.loads((sdk / 'APPLICATION-SDK.json').read_text())
    gnu_sdk = json.loads((sdk / 'SDK.json').read_text())
    if application_sdk['gnu_sdk_sha256'] != digest(sdk / 'SDK.json'):
        raise ValueError('GNU application SDK changed')
    for record in [gnu_sdk, application_sdk]:
        for relative, expected in record['source_sha256'].items():
            if digest(ROOT / relative) != expected:
                raise ValueError('SDK recipe changed: ' + relative)
        for relative, asset in record['files'].items():
            if digest(sdk / 'sysroot' / relative) != asset['sha256']:
                raise ValueError('SDK input changed: ' + relative)
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=False)
    host_record = None
    if args.native:
        host_bin, node_gyp = args.host_bin.resolve(strict=True), args.node_gyp.resolve(strict=True)
    else:
        checkout = args.electron_checkout.resolve(strict=True)
        host_bin, host_record = prepare_node(out / 'host-tools', checkout / 'src')
        host_pin = json.loads((ROOT / 'ports/electron/node-host.json').read_text())
        node_gyp = checkout / 'src' / host_pin['npm_source'] / 'node_modules/node-gyp/bin/node-gyp.js'
    header_pin = json.loads(HEADERS.read_text())
    vendor = ROOT / 'build/codeoss-native-headers/vendor'
    fetch(header_pin, vendor)
    headers = out / 'headers'
    headers.mkdir()
    unpack(vendor / header_pin['archive'], headers)
    matches = list(headers.rglob('include/node/node_api.h'))
    if len(matches) != 1:
        raise ValueError('Unrecognized Electron header archive layout')
    node_dir = matches[0].parents[2]
    home = out / 'home'
    home.mkdir()
    environment = dict(PATH=str(host_bin) + ':/usr/bin:/bin', HOME=str(home), TMPDIR='/tmp', LANG='C.UTF-8',
        NODE_PATH=str(source / 'node_modules'), PYTHONDONTWRITEBYTECODE='1',
        CC=str(sdk / 'bin/gcc'), CXX=str(sdk / 'bin/g++'), LINK=str(sdk / 'bin/g++'),
        AR='/usr/bin/ar', GYP_DEFINES='kerberos_use_rtld=true',
        PKG_CONFIG_SYSROOT_DIR=str(sdk / 'sysroot'),
        PKG_CONFIG_LIBDIR=str(sdk / 'sysroot/usr/lib/x86_64-linux-gnu/pkgconfig') + ':' + str(sdk / 'sysroot/usr/share/pkgconfig'),
        npm_config_devdir=str(out / 'node-gyp'), npm_config_build_from_source='true')
    conf = [] if args.native else ['bwrap', '--unshare-all', '--die-with-parent', '--new-session',
        '--ro-bind', '/', '/', '--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp', '--bind', str(out), str(out)]
    libraries = sdk / 'sysroot/usr/lib/x86_64-linux-gnu'
    available_versions = set()
    for name in ['libc.so.6', 'libm.so.6', 'libgcc_s.so.1', 'libstdc++.so.6']:
        text = subprocess.check_output(['readelf', '--version-info', libraries / name], text=True)
        available_versions.update(re.findall(r'\b(?:GLIBC|GLIBCXX|CXXABI|GCC)_[A-Za-z0-9_.]+', text))
    available_libraries = {p.name for p in libraries.rglob('*') if p.is_file()}
    products, modules = {}, {}
    for name, version in json.loads(MODULES.read_text())['modules'].items():
        original = source / 'node_modules' / name
        if json.loads((original / 'package.json').read_text())['version'] != version:
            raise ValueError('Module version differs from the pinned source recipe: ' + name)
        destination = out / 'modules' / name

        def ignored(directory, names):
            return [n for n in names if n == '.git' or Path(directory) == original and n in ('build', 'prebuilds')]

        shutil.copytree(original, destination, ignore=ignored, symlinks=False)
        source_hashes = {str(p.relative_to(destination)): digest(p) for p in destination.rglob('*') if p.is_file()}
        command = [str(host_bin / 'node'), str(node_gyp), 'rebuild', '--release', '--arch=x64',
            '--nodedir=' + str(node_dir), '--jobs=' + str(args.jobs)]
        log = out / (name.replace('/', '-') + '.log')
        print('Building target native module ' + name, flush=True)
        with log.open('w') as stream:
            subprocess.run([*conf, '--chdir', str(destination), '--', *command] if conf else command,
                cwd=destination, env=environment, stdin=subprocess.DEVNULL, stdout=stream, stderr=subprocess.STDOUT, check=True)
        built = [p for p in (destination / 'build/Release').glob('*.node') if p.is_file()]
        if not built:
            raise ValueError('Module produced no target addon: ' + name)
        for binary in built:
            elf = subprocess.check_output(['readelf', '-h', '-lW', '-d', '--version-info', binary], text=True)
            if 'Advanced Micro Devices X86-64' not in elf or ' RWE ' in elf:
                raise ValueError('Unexpected native module ELF layout: ' + name)
            required = set(re.findall(r'\b(?:GLIBC|GLIBCXX|CXXABI|GCC)_[A-Za-z0-9_.]+', elf))
            needed = set(re.findall(r'\(NEEDED\).*?\[([^\]]+)\]', elf))
            if required - available_versions or needed - available_libraries:
                raise ValueError('Module requires libraries or ABI versions outside the GNU SDK: ' + name +
                    ': ' + repr(sorted((required - available_versions) | (needed - available_libraries))))
            relative = 'node_modules.asar.unpacked/' + name + '/build/Release/' + binary.name
            products[relative] = dict(path=str(binary), sha256=digest(binary), needed=sorted(needed), abi_versions=sorted(required))
        modules[name] = dict(version=version, source_sha256=source_hashes, command=command, log_sha256=digest(log))
        print('Built and checked GNU ABI for ' + name, flush=True)
    inputs = [Path(__file__), MODULES, HEADERS, ROOT / 'tools/prepare_electron_host.py', ROOT / 'ports/electron/node-host.json']
    atomic_write(out / 'BUILD.json', json.dumps(dict(
        state='C/C++ native modules source compiled; Coast loading qualification pending', files=products, modules=modules,
        application_source_receipt_sha256=digest(source_build / 'SOURCE.json'),
        sdk_receipt_sha256=digest(sdk / 'APPLICATION-SDK.json'), header_archive_sha256=header_pin['sha256'],
        gui_runtime_sha256=application_sdk['gui_runtime_sha256'],
        host_tools=host_record, native_modules_loaded_on_linux=False,
        source_sha256={str(p.relative_to(ROOT)): digest(p) for p in inputs},
        build_mode='native-coast' if args.native else 'linux-cross',
        remaining_inputs='Rust proxy resolver and third-party authentication/extension binaries'), indent=2) + '\n')
    print('Code OSS C/C++ addons compiled; qualify their loading inside Coast', flush=True)


if __name__ == '__main__':
    with project_lock():
        main()

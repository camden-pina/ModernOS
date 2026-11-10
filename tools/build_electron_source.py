#!/usr/bin/env python3
"""Fetch the pinned Electron source graph and build Coast's runtime from source.

The source lock and patches replace private historical checkout dependencies.
This is a large Linux-host build. No downloaded or built guest runtime is run.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from prepare_electron_host import (prepare as prepare_host, prepare_node, protobuf_isolation,
    generated_source_overlay, generated_source_record, out_of_tree_generator)

ROOT = Path(__file__).resolve().parents[1]
LOCK = ROOT / 'ports/electron/sources.lock.json'


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def run(command, cwd, env=None):
    subprocess.run(list(map(str, command)), cwd=cwd, env=env, check=True, stdin=subprocess.DEVNULL)


def runtime_sidecars(directory):
    # Electron's default app loader and English locale must accompany a fresh
    # image. Overlay images used to inherit these files from an older ISO.
    names = ['snapshot_blob.bin', 'resources.pak', 'chrome_100_percent.pak',
             'chrome_200_percent.pak', 'icudtl.dat', 'libffmpeg.so',
             'resources/default_app.asar', 'locales/en-US.pak']
    products = {name: dict(path=str(directory / name), sha256=sha(directory / name))
                for name in names if (directory / name).is_file()}
    required = set(names) - {'snapshot_blob.bin'}
    if missing := required - products.keys():
        raise ValueError('Incomplete source-built Electron resources: ' + ', '.join(sorted(missing)))
    return products


def verify(checkout, lock, overlays=False):
    for record in lock['repositories']:
        repo = checkout / record['path']
        tree = subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD^{tree}'], text=True).strip()
        if tree != record['patched_tree']:
            # In a fresh build the complete patches are uncommitted. Verify
            # their source bytes against the pinned base instead of trusting HEAD.
            base = subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip()
            if base != record['commit']:
                raise ValueError('Unexpected source revision: ' + record['path'])
            if record.get('patch'):
                run(['git', 'apply', '--reverse', '--check', ROOT / record['patch']], repo)
        if record.get('patch') and sha(ROOT / record['patch']) != record['patch_sha256']:
            raise ValueError('Port patch changed: ' + record['patch'])
    if overlays:
        for target, source in lock['overlays'].items():
            if sha(checkout / target) != sha(ROOT / source):
                raise ValueError('Coast policy differs: ' + target)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-root', type=Path, default=ROOT / 'build/electron-source')
    parser.add_argument('--checkout', type=Path, help='use existing verified source read-only; build outputs stay under --output-root')
    parser.add_argument('--configure-check', action='store_true', help='generate fresh build graphs over a read-only verified checkout')
    parser.add_argument('--sync', action='store_true', help='create a fresh pinned source checkout and run upstream build-tool hooks')
    parser.add_argument('--build', action='store_true', help='compile Electron and standalone Node after source preparation')
    parser.add_argument('--reconfigure', action='store_true',
        help='record changed inputs and regenerate this builder\'s existing read-only-source output graph')
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    if args.reconfigure and (not args.checkout or not (args.configure_check or args.build)):
        parser.error('--reconfigure requires --checkout and a configure/build action')
    lock = json.loads(LOCK.read_text())
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    if args.checkout:
        if args.sync:
            parser.error('--checkout cannot be combined with --sync')
        verify(args.checkout.resolve(), lock)
        print('ELECTRON SOURCE LOCK PASS:', len(lock['repositories']), 'repositories')
        if args.configure_check or args.build:
            out = args.output_root.resolve()
            out.mkdir(parents=True, exist_ok=True)
            checkout = args.checkout.resolve()
            if out.is_relative_to(checkout) or checkout.is_relative_to(out):
                parser.error('Read-only source and build output must be separate directories')
            source = checkout / 'src'
            conf = ['bwrap', '--unshare-all', '--die-with-parent', '--new-session', '--ro-bind', '/', '/',
                    '--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp', '--bind', str(out), str(out)]
            conf += generated_source_overlay(out / 'host-tools', source)
            for target, overlay in lock['overlays'].items():
                conf += ['--ro-bind', str(ROOT / overlay), str(checkout / target)]
            environment = dict(PATH='/usr/bin:/bin', HOME='/tmp', TMPDIR='/tmp', LANG='C.UTF-8',
                               PYTHONDONTWRITEBYTECODE='1', PYTHONNOUSERSITE='1',
                               PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION='python', DEPOT_TOOLS_UPDATE='0')
            identity = dict(source_lock_sha256=sha(LOCK),
                args_sha256=sha(ROOT / 'ports/electron/args.gn'),
                host_tools_sha256=sha(ROOT / 'ports/electron/host-tools.json'),
                overlays={p: sha(ROOT / p) for p in lock['overlays'].values()})
            receipt = out / 'CONFIGURE.json'
            if receipt.exists():
                previous = json.loads(receipt.read_text())
                if any(previous.get(k) != v for k, v in identity.items()):
                    if not args.reconfigure:
                        parser.error('Build inputs changed; select a fresh output directory or use --reconfigure')
                    history = out / 'configuration-history'
                    history.mkdir(exist_ok=True)
                    snapshot = history / (sha(receipt) + '.json')
                    if not snapshot.exists():
                        shutil.copyfile(receipt, snapshot)
            elif any(out.iterdir()):
                parser.error('Unrecognized output contents; select a fresh output directory')
            isolation, hidden_modules = protobuf_isolation(out)
            conf += isolation
            generator_mount, generator_record = out_of_tree_generator(out / 'host-tools', source)
            conf += generator_mount
            products = {}
            for name, root in [('electron', '//electron:electron_app'), ('node', '//third_party/electron_node:node')]:
                build = out / name
                build.mkdir(exist_ok=True)
                shutil.copyfile(ROOT / 'ports/electron/args.gn', build / 'args.gn')
                run([*conf, '--chdir', build, '--', source / 'buildtools/linux64/gn', '--root=' + str(source),
                     '--root-pattern=' + root, '--root-target=' + root, '--script-executable=' + sys.executable,
                     'gen', '.'], out, environment)
            receipt.write_text(json.dumps(dict(**identity, compiled=False, source_read_only=True,
                target_host_executed=False, protobuf_backend='vendored-python',
                hidden_system_protobuf_modules=hidden_modules), indent=2) + '\n')
            if args.build:
                environment['NODE_PATH'] = str(prepare_host(out / 'host-tools'))
                host_bin, host_node = prepare_node(out / 'host-tools', source)
                environment.update(PATH=str(host_bin) + ':/usr/bin:/bin',
                    npm_config_cache=str(out / 'host-tools/npm-cache'), npm_config_update_notifier='false')
                for name in ('electron', 'node'):
                    build = out / name
                    environment['COAST_GN_ROOT_TARGET'] = ('//electron:electron_app' if name == 'electron'
                        else '//third_party/electron_node:node')
                    run([*conf, '--chdir', build, '--', source / 'third_party/ninja/ninja',
                         '-C', build, '-j' + str(args.jobs), name], out, environment)
                    binary = out / (name + '-stripped')
                    run([*conf, '--', source / 'third_party/llvm-build/Release+Asserts/bin/llvm-strip',
                         '--strip-all', '-o', binary, build / name], out, environment)
                    products[name] = dict(path=str(binary), sha256=sha(binary),
                        unstripped=str(build / name), unstripped_sha256=sha(build / name))
                sidecars = runtime_sidecars(out / 'electron')
                record = dict(products=products, args_sha256=identity['args_sha256'],
                    graphs={name: dict(args_sha256=sha(Path(product['unstripped']).parent / 'args.gn')) for name, product in products.items()},
                    binary=products['electron']['path'],
                    binary_sha256=products['electron']['sha256'], source_lock_sha256=sha(LOCK),
                    source_read_only=True, target_host_executed=False, guest_execution_on_host=False,
                    protobuf_backend='vendored-python', hidden_system_protobuf_modules=hidden_modules,
                    host_node=host_node,
                    generated_source_files=generated_source_record(out / 'host-tools'),
                    original_fs_generator=generator_record,
                    configuration_history={p.name: sha(p) for p in sorted((out / 'configuration-history').glob('*.json'))},
                    build_scripts_sha256={str(p.relative_to(ROOT)): sha(p) for p in
                        [Path(__file__), ROOT / 'tools/prepare_electron_host.py', ROOT / 'ports/electron/node-host.json']},
                    sidecars=sidecars, features=dict(jitless_compiled=True, drumbrake_explicit_bounds_checks=True),
                    state='source compiled; guest qualification pending')
                (out / 'BUILD.json').write_text(json.dumps(record, indent=2) + '\n')
                license = source / 'third_party/electron_node/LICENSE'
                (out / 'NODE.json').write_text(json.dumps(dict(binary=products['node']['path'],
                    binary_sha256=products['node']['sha256'], parent_electron_sha256=products['electron']['sha256'],
                    guest_execution_on_host=False, sidecars={p: sidecars[p] for p in ('snapshot_blob.bin', 'icudtl.dat') if p in sidecars},
                    license=dict(path=str(license), sha256=sha(license)),
                    state='source compiled; guest qualification pending'), indent=2) + '\n')
        return
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=True)
    checkout, depot = out / 'checkout', out / 'depot_tools'
    environment = {**os.environ, 'DEPOT_TOOLS_UPDATE': '0', 'DEPOT_TOOLS_METRICS': '0',
                   'GIT_TERMINAL_PROMPT': '0', 'PYTHONDONTWRITEBYTECODE': '1'}
    environment['PATH'] = str(depot) + ':' + environment['PATH']
    if args.sync:
        if checkout.exists() or depot.exists():
            parser.error('A fresh output root is required for --sync; existing source trees are preserved')
        run(['git', 'clone', '--no-checkout', lock['depot_tools']['url'], depot], out)
        run(['git', 'checkout', '--detach', lock['depot_tools']['commit']], depot)
        checkout.mkdir()
        electron = next(r for r in lock['repositories'] if r['path'] == 'src/electron')
        config = dict(name='src/electron', url=electron['url'], deps_file='DEPS', managed=False,
                      custom_deps={}, custom_vars={'apply_patches': False})
        (checkout / '.gclient').write_text('solutions = ' + repr([config]) + '\n'
                'target_os = ["linux"]\ntarget_os_only = True\n'
                'target_cpu = ["x64"]\ntarget_cpu_only = True\n')
        run([depot / 'gclient', 'sync', '--nohooks', '--with_branch_heads', '--with_tags',
             '--revision', 'src/electron@' + electron['commit']], checkout, environment)
        for record in lock['repositories']:
            repo = checkout / record['path']
            actual = subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip()
            if actual != record['commit']:
                raise ValueError('gclient dependency differs from the source lock: ' + record['path'])
            if record.get('patch'):
                patch = ROOT / record['patch']
                if sha(patch) != record['patch_sha256']:
                    raise ValueError('Port patch checksum mismatch: ' + str(patch))
                run(['git', 'apply', '--check', patch], repo)
                run(['git', 'apply', patch], repo)
        # The lock includes upstream Electron's patches as well as the Coast
        # NodePlatform adapter. apply_patches=False avoids applying them twice.
        host_bin, _host_node = prepare_node(out / 'host-tools', checkout / 'src')
        environment['PATH'] = str(host_bin) + ':' + environment['PATH']
        environment['npm_config_cache'] = str(out / 'host-tools/npm-cache')
        environment['npm_config_update_notifier'] = 'false'
        run([depot / 'gclient', 'runhooks'], checkout, environment)
        verify(checkout, lock)
        for target, source in lock['overlays'].items():
            shutil.copyfile(ROOT / source, checkout / target)
        (out / 'SOURCE.json').write_text(json.dumps(dict(lock_sha256=sha(LOCK),
            overlays={p: sha(ROOT / p) for p in lock['overlays'].values()}), indent=2) + '\n')
    if not (out / 'SOURCE.json').is_file():
        parser.error('Run --sync first, or use --checkout for read-only source verification')
    receipt = json.loads((out / 'SOURCE.json').read_text())
    if receipt['lock_sha256'] != sha(LOCK):
        parser.error('Source lock changed; prepare a new output root')
    for path, digest in receipt['overlays'].items():
        if sha(ROOT / path) != digest:
            parser.error('Source policy changed; prepare a new output root: ' + path)
    for target, source in lock['overlays'].items():
        if sha(checkout / target) != sha(ROOT / source):
            parser.error('Prepared source changed: ' + target)
    if args.build:
        build_args_sha256 = sha(ROOT / 'ports/electron/args.gn')
        environment['NODE_PATH'] = str(prepare_host(out / 'host-tools'))
        environment.update(PYTHONNOUSERSITE='1', PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION='python')
        source = checkout / 'src'
        host_bin, host_node = prepare_node(out / 'host-tools', source)
        environment.update(PATH=str(host_bin) + ':' + environment['PATH'],
            npm_config_cache=str(out / 'host-tools/npm-cache'), npm_config_update_notifier='false')
        isolation, hidden_modules = protobuf_isolation(out)
        generator_mount, generator_record = out_of_tree_generator(out / 'host-tools', source)
        conf = ['bwrap', '--unshare-all', '--die-with-parent', '--new-session', '--ro-bind', '/', '/',
            '--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp', '--bind', str(out), str(out),
            *generated_source_overlay(out / 'host-tools', source), *isolation,
            *generator_mount,
            '--chdir', str(source), '--']
        gn, ninja = source / 'buildtools/linux64/gn', source / 'third_party/ninja/ninja'
        products = {}
        for name, target, root in [('electron', 'electron', '//electron:electron_app'),
                                    ('node', 'node', '//third_party/electron_node:node')]:
            environment['COAST_GN_ROOT_TARGET'] = root
            build = source / ('out/Coast-' + name)
            build.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / 'ports/electron/args.gn', build / 'args.gn')
            run([*conf, gn, 'gen', build, '--root-pattern=' + root, '--root-target=' + root], source, environment)
            run([*conf, ninja, '-C', build, '-j' + str(args.jobs), target], source, environment)
            binary = out / name
            run([source / 'third_party/llvm-build/Release+Asserts/bin/llvm-strip', '--strip-all',
                 '-o', binary, build / target], source, environment)
            products[name] = dict(path=str(binary), sha256=sha(binary),
                                   unstripped=str(build / target), unstripped_sha256=sha(build / target))
        sidecars = runtime_sidecars(source / 'out/Coast-electron')
        record = dict(products=products, args_sha256=build_args_sha256,
            graphs={name: dict(args_sha256=sha(Path(product['unstripped']).parent / 'args.gn')) for name, product in products.items()},
            binary=products['electron']['path'],
            binary_sha256=products['electron']['sha256'], source_lock_sha256=sha(LOCK),
            target_host_executed=False, guest_execution_on_host=False, sidecars=sidecars,
            protobuf_backend='vendored-python', hidden_system_protobuf_modules=hidden_modules,
            host_node=host_node,
            generated_source_files=generated_source_record(out / 'host-tools'),
            original_fs_generator=generator_record,
            build_scripts_sha256={str(p.relative_to(ROOT)): sha(p) for p in
                [Path(__file__), ROOT / 'tools/prepare_electron_host.py', ROOT / 'ports/electron/node-host.json']},
            features=dict(jitless_compiled=True, drumbrake_explicit_bounds_checks=True),
            state='source compiled; guest qualification pending')
        (out / 'BUILD.json').write_text(json.dumps(record, indent=2) + '\n')
        license = source / 'third_party/electron_node/LICENSE'
        node = dict(binary=products['node']['path'], binary_sha256=products['node']['sha256'],
            parent_electron_sha256=products['electron']['sha256'], guest_execution_on_host=False,
            sidecars={p: sidecars[p] for p in ('snapshot_blob.bin', 'icudtl.dat') if p in sidecars},
            license=dict(path=str(license), sha256=sha(license)),
            state='source compiled; guest qualification pending')
        (out / 'NODE.json').write_text(json.dumps(node, indent=2) + '\n')
    print('Source checkout:', checkout)


if __name__ == '__main__':
    main()

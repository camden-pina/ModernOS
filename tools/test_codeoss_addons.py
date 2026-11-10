#!/usr/bin/env python3
"""Load the exact rebuilt Code OSS modules and run SQLite inside an owned Coast VM."""
import argparse
import base64
import json
from pathlib import Path
import threading
import traceback

from serve_repository import RepositoryServer
from source_tree import digest
import test_runtime as runtime


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, required=True)
    parser.add_argument('--repository', type=Path,
                        help='optional fixture server; without it, transfer the fixtures over the guest console')
    parser.add_argument('--addons-build', type=Path, required=True)
    parser.add_argument('--output-root', type=Path, required=True)
    args = parser.parse_args()
    iso = args.iso.resolve(strict=True)
    image = json.loads(iso.with_name('MANIFEST.json').read_text())
    if image['iso_sha256'] != digest(iso) or image['addons_build_sha256'] != digest(args.addons_build):
        parser.error('Select the image containing these exact source-built modules')
    addons = json.loads(args.addons_build.read_text())
    for name, expected in addons['source_sha256'].items():
        if digest(runtime.ROOT / name) != expected:
            raise ValueError('Module recipe changed: ' + name)
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=False)
    runtime.WORK = out
    manifest = out / 'modules.json'
    manifest.write_text(json.dumps(addons['files'], indent=2) + '\n')
    checker = runtime.ROOT / 'ports/codeoss-app/addons_check.cjs'
    fixtures = [('check.cjs', checker), ('modules.json', manifest)]
    rust_receipt = image.get('components', {}).get('rust_build')
    proxy_checker = runtime.ROOT / 'ports/codeoss-app/proxy_check.cjs'
    if rust_receipt:
        if digest(Path(rust_receipt['path'])) != rust_receipt['sha256']:
            raise ValueError('Rust resolver build receipt changed')
        rust = json.loads(Path(rust_receipt['path']).read_text())
        rust_manifest = out / 'rust-modules.json'
        rust_manifest.write_text(json.dumps(rust['files'], indent=2) + '\n')
        fixtures += [('proxy-check.cjs', proxy_checker), ('rust-modules.json', rust_manifest)]
    report = dict(state='failed', checks=[], iso_sha256=digest(iso),
        fixture_transport='repository HTTP' if args.repository else 'guest console',
        repository_manifest_sha256=digest(args.repository) if args.repository else None,
        addons_build_sha256=digest(args.addons_build), source_sha256={
            str(p.relative_to(runtime.ROOT)): digest(p) for p in [Path(__file__), checker,
                *([proxy_checker] if rust_receipt else [])]},
        rust_build_sha256=rust_receipt['sha256'] if rust_receipt else None,
        target_execution_on_linux=False)
    vm = server = thread = None

    def check(name, command, expected, timeout=240, user=True):
        print('START ' + name, flush=True)
        output = vm.shell_command(command, expect=expected, as_user=user, timeout=timeout)
        (out / (f'{len(report["checks"]):02d}-output.txt')).write_text(output)
        report['checks'].append(name)
        print('PASS ' + name, flush=True)

    try:
        if args.repository:
            server = RepositoryServer(('127.0.0.1', 0), args.repository)
            for name, source in fixtures:
                stat = source.stat()
                server.repository['assets']['/distfiles/addons-' + name] = dict(path=source,
                    sha256=digest(source), identity=(stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns, stat.st_ctime_ns))
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            origin = 'http://10.0.2.2:' + str(server.server_port) + '/distfiles/addons-'
        vm = runtime.VM(4, storage=False, network=bool(args.repository), memory='12G', accel='kvm', iso=iso, label='source-codeoss-addons')
        vm.command('true')
        if args.repository:
            check('select the live fixture repository and initialize guest networking',
                'brine server ' + origin.split('/distfiles/')[0], 'Package downloads and source builds now use')
        check('source-built modules run inside the Coast kernel', 'uname -s', 'Coast')
        for name, source in fixtures:
            target = '/tmp/addons-' + name
            if args.repository:
                check('fetch and verify ' + name, 'curl -fsS ' + origin + name + ' -o ' + target +
                    ' && sha256sum ' + target, digest(source))
            else:
                # Keep each real shell command below the native console's
                # 511-byte limit. No guest memory is written by the monitor.
                encoded = base64.b64encode(source.read_bytes()).decode()
                vm.shell_command(': > ' + target + '.b64', as_user=True)
                for offset in range(0, len(encoded), 200):
                    vm.shell_command("printf '%s' '" + encoded[offset:offset + 200] + "' >> " + target + '.b64', as_user=True)
                check('transfer and verify ' + source.name + ' through the guest console',
                    '/bin/busybox base64 -d ' + target + '.b64 > ' + target +
                    ' && /bin/busybox sha256sum ' + target, digest(source))
        check('Node verifies eleven source-built artifacts, loads ten Linux modules and writes SQLite through its worker API',
            '/bin/native-node-launch-check --jitless /tmp/addons-check.cjs /tmp/addons-modules.json',
            'CODEOSS_ADDONS_ALL_PASS', timeout=600, user=False)
        if rust_receipt:
            check('the rebuilt Rust resolver loads, reads configuration and honors proxy/no_proxy without a network connection',
                '/bin/native-node-launch-check --jitless /tmp/addons-proxy-check.cjs /tmp/addons-rust-modules.json',
                'CODEOSS_PROXY_ALL_PASS', timeout=600, user=False)
        vm.command('check', expect='USERLAND CHECK PASS', timeout=120)
        report['checks'].append('Coast remains healthy after native Node modules and worker threads')
        report['state'] = 'passed'
    except BaseException as error:
        report['error'] = repr(error)
        (out / 'exception.txt').write_text(traceback.format_exc())
        raise
    finally:
        if vm:
            vm.close()
        if server:
            server.shutdown()
            server.server_close()
        if thread:
            thread.join(5)
        report['owned_vm_stopped'] = bool(vm and vm.process.poll() is not None)
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()

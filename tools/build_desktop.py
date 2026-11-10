#!/usr/bin/env python3
"""Build a fresh Coast Chromium/Code OSS ISO from the public pinned sources.

Host setup is a prerequisite. Each completed component is verified and reused
on restart. An explicit component map can reuse source-build receipts; existing
ISOs and runtime staging directories are never accepted as desktop inputs.
"""
import argparse
import fcntl
import json
from pathlib import Path
import subprocess
import sys
import time

from source_tree import audit, digest

ROOT = Path(__file__).resolve().parents[1]


def verify_receipt(path):
    path = Path(path).resolve(strict=True)
    record = json.loads(path.read_text())
    for name, expected in record.get('source_sha256', {}).items():
        if digest(ROOT / name) != expected:
            raise ValueError('Source recipe changed; rebuild this component: ' + name)

    def verify_assets(value):
        if isinstance(value, dict):
            if 'path' in value and 'sha256' in value:
                if digest(Path(value['path'])) != value['sha256']:
                    raise ValueError('Build output changed: ' + value['path'])
            for child in value.values():
                verify_assets(child)
        elif isinstance(value, list):
            for child in value:
                verify_assets(child)

    # Electron records source-relative GN/Ninja paths as host-tool provenance.
    # They are not deployed artifacts; runtime admission checks the products,
    # source policies and build graph independently below.
    verify_assets({name: value for name, value in record.items() if name not in ('host_node', 'host_tools')})
    for name in ('iso', 'initramfs'):
        if name in record and name + '_sha256' in record:
            if digest(Path(record[name])) != record[name + '_sha256']:
                raise ValueError('Build output changed: ' + record[name])
    if 'kernel_sha256' in record and path.name == 'KERNEL.json':
        if not record.get('source_sha256'):
            raise ValueError('Rebuild the kernel with its complete source identity')
        if digest(path.parent / 'vmlinux') != record['kernel_sha256']:
            raise ValueError('Source-built kernel changed')
    if record.get('inherited_iso') is False and 'programs' in record:
        for name, expected in record['programs'].items():
            if digest(path.parent / 'programs' / name) != expected:
                raise ValueError('Native base program changed: ' + name)
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-root', type=Path, default=ROOT / 'build/desktop-source')
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--electron-checkout', type=Path,
                        help='optional verified Electron checkout used read-only instead of a fresh source sync')
    parser.add_argument('--runtime-build', type=Path,
                        help='optional complete source-built Electron/Node receipt; requires --electron-checkout')
    parser.add_argument('--components', type=Path,
                        help='optional JSON map of component names to verified source-build receipt paths')
    parser.add_argument('--diagnostics', action='store_true', help='include inert, opt-in Code OSS QA diagnostics')
    parser.add_argument('--qualify', action='store_true', help='also check boot, terminal, addons, browser and editor in owned QEMU guests')
    parser.add_argument('--with-integrations', action='store_true',
                        help='also build Coast Packages/makepkg, Python, Qt and QEMU with its source viewer')
    parser.add_argument('--repository-config', type=Path,
                        help='optional explicit public repository trust JSON for the wider image')
    parser.add_argument('--with-microsoft-auth', action='store_true',
                        help='include upstream proprietary Microsoft sign-in binaries (omitted by default)')
    parser.add_argument('--plan', action='store_true', help='print the build commands without running them')
    args = parser.parse_args()
    args.diagnostics = args.diagnostics or args.qualify
    if not 1 <= args.jobs <= 24:
        parser.error('--jobs must be 1..24')
    if args.runtime_build and not args.electron_checkout:
        parser.error('--runtime-build also requires its verified --electron-checkout')
    if args.repository_config and not args.with_integrations:
        parser.error('--repository-config requires --with-integrations')
    out = args.output_root.resolve()
    overrides = json.loads(args.components.read_text()) if args.components else {}
    names = {'glibc', 'libgcc', 'cxx', 'gui-sources', 'gui-runtime', 'application', 'sdk',
             'addons', 'rust', 'kernel', 'native', 'compiler', 'packages', 'cli', 'bridge', 'assets'}
    if args.with_integrations:
        names.update(('program', 'makepkg', 'frontend', 'qt', 'qemu', 'viewer'))
    if not isinstance(overrides, dict) or set(overrides) - names:
        parser.error('--components must map known component names to receipt paths')
    receipts = {name: Path(overrides.get(name, out / name / (
        'KERNEL.json' if name == 'kernel' else 'PACKAGED.json' if name == 'application'
        else 'APPLICATION-SDK.json' if name == 'sdk' else 'BUILD.json'))).resolve() for name in names}
    runtime = args.runtime_build.resolve() if args.runtime_build else out / 'electron/BUILD.json'
    checkout = args.electron_checkout.resolve() if args.electron_checkout else out / 'electron/checkout'
    jobs = str(args.jobs)

    def command(tool, *arguments):
        return list(map(str, [sys.executable, '-B', ROOT / 'tools' / tool, *arguments]))

    gnu = ['--glibc-build', receipts['glibc'], '--libgcc-build', receipts['libgcc'], '--cxx-build', receipts['cxx']]
    stages = [
        ('glibc', command('build_glibc.py', '--output-root', receipts['glibc'].parent, '--jobs', jobs), True),
        ('libgcc', command('build_gnu_libgcc.py', '--glibc-build', receipts['glibc'],
            '--output-root', receipts['libgcc'].parent, '--jobs', jobs), True),
        ('cxx', command('build_gnu_cxx.py', '--glibc-build', receipts['glibc'], '--libgcc-build', receipts['libgcc'],
            '--output-root', receipts['cxx'].parent, '--jobs', jobs), True),
        ('gui-sources', command('build_gui_sources.py', '--output-root', receipts['gui-sources'].parent, '--jobs', jobs), True),
        ('gui-runtime', command('prepare_gui_runtime.py', *gnu, '--source-build', receipts['gui-sources'],
            '--output-root', receipts['gui-runtime'].parent), False),
        ('application', command('build_codeoss_source.py', '--electron-checkout', checkout,
            '--output-root', receipts['application'].parent, '--prepare', '--build', '--package-app', '--jobs', jobs), True),
        ('sdk', command('prepare_codeoss_sdk.py', *gnu, '--gui-runtime', receipts['gui-runtime'],
            '--output-root', receipts['sdk'].parent), False),
        ('addons', command('build_codeoss_addons.py', '--application-build', receipts['application'].parent,
            '--sdk', receipts['sdk'].parent, '--electron-checkout', checkout,
            '--output-root', receipts['addons'].parent, '--jobs', jobs), False),
        ('rust', command('build_codeoss_rust.py', '--application-build', receipts['application'].parent,
            '--sdk', receipts['sdk'].parent, '--output-root', receipts['rust'].parent, '--jobs', jobs), True),
        ('kernel', command('build_desktop_kernel.py', '--runtime-build', runtime,
            '--output-root', receipts['kernel'].parent, '--jobs', jobs), True),
        ('native', command('build_native_iso.py', '--kernel', receipts['kernel'].parent / 'vmlinux',
            '--output-root', receipts['native'].parent, '--jobs', jobs), True),
        ('compiler', command('build_gcc.py', '--musl-root', receipts['native'].parent / 'musl',
            '--output-root', receipts['compiler'].parent, '--runtime-root', receipts['compiler'].parent / 'runtime',
            '--posix-threads', '--jobs', jobs), True),
        ('packages', command('build_desktop_packages.py', '--native-build', receipts['native'].parent,
            '--output-root', receipts['packages'].parent, '--jobs', jobs), True),
        ('cli', command('build_desktop_cli.py', '--native-build', receipts['native'].parent,
            '--output-root', receipts['cli'].parent, '--jobs', jobs), True),
        ('bridge', command('build_wayland_bridge.py', '--cc', receipts['native'].parent / 'musl/bin/musl-gcc',
            '--gnu-sdk', receipts['sdk'].parent, '--output-root', receipts['bridge'].parent, '--jobs', jobs), True),
        ('assets', command('prepare_desktop_assets.py', '--output-root', receipts['assets'].parent), True),
    ]
    if args.with_integrations:
        stages += [
            ('program', command('build_program_ports.py', '--compiler-build', receipts['compiler'],
                '--dependencies-build', receipts['packages'].with_name('DEPENDENCIES.json'),
                '--output-root', receipts['program'].parent, '--jobs', jobs), True),
            ('makepkg', command('build_desktop_makepkg.py', '--compiler-build', receipts['compiler'],
                '--packages-build', receipts['packages'], '--program-build', receipts['program'],
                '--output-root', receipts['makepkg'].parent, '--jobs', jobs), True),
            ('frontend', command('build_coast_packages.py', '--compiler-build', receipts['compiler'],
                '--program-build', receipts['program'], '--output-root', receipts['frontend'].parent,
                *(['--repository-config', args.repository_config.resolve()] if args.repository_config else [])), False),
            ('qt', command('build_qt6.py', '--compiler-build', receipts['compiler'],
                '--output-root', receipts['qt'].parent, '--jobs', jobs), True),
            ('qemu', command('build_qemu.py', '--compiler-build', receipts['compiler'],
                '--output-root', receipts['qemu'].parent, '--jobs', jobs), True),
            ('viewer', command('prepare_qemu_viewer.py', '--qemu-build', receipts['qemu'],
                '--program-build', receipts['program'], '--output-root', receipts['viewer'].parent), False),
        ]
    assembly = command('assemble_desktop.py', '--runtime-build', runtime, '--output-root', out / 'image',
        '--native-build', receipts['native'].parent, '--kernel-build', receipts['kernel'].parent,
        '--gui-runtime', receipts['gui-runtime'], '--application-build', receipts['application'].parent,
        '--addons-build', receipts['addons'], '--rust-build', receipts['rust'], '--bridge-build', receipts['bridge'],
        '--assets-build', receipts['assets'], '--cli-build', receipts['cli'], '--compiler-build', receipts['compiler'],
        '--packages-build', receipts['packages'],
        *([item for name in ('program', 'makepkg', 'frontend', 'qt', 'qemu', 'viewer')
            for item in ('--' + name + '-build', receipts[name])] if args.with_integrations else []),
        *(['--diagnostics'] if args.diagnostics else []),
        *(['--with-microsoft-auth'] if args.with_microsoft_auth else []))
    electron = command('build_electron_source.py', '--output-root', out / 'electron', '--jobs', jobs,
        *(['--checkout', checkout] if args.electron_checkout else []), '--build')
    if args.plan:
        print(json.dumps(dict(runtime_receipt=str(runtime), inherited_iso=False,
            source_sync=None if args.electron_checkout or args.runtime_build else
                command('build_electron_source.py', '--output-root', out / 'electron', '--sync', '--jobs', jobs),
            runtime_build=None if args.runtime_build else electron,
            stages=[dict(name=name, receipt=str(receipts[name]), reused=name in overrides, argv=argv)
                    for name, argv, _ in stages], assembly=assembly,
            qualification=command('qualify_desktop.py', '--iso', out / 'image/Coast-desktop.iso',
                '--output-root', out / 'qualification') if args.qualify else None), indent=2))
        return
    out.mkdir(parents=True, exist_ok=True)
    lock = (out / '.build.lock').open('a')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    inventory = audit()
    # Prose/handoff edits do not invalidate compiled components. All other
    # publishable inputs are fixed throughout this workflow.
    source_identity = {name: entry['sha256'] for name, entry in inventory.items() if not name.endswith('.md')}
    identity = dict(source_sha256=source_identity, overrides={name: dict(path=str(receipts[name]),
        sha256=digest(receipts[name])) for name in overrides},
        external_runtime=dict(path=str(runtime), sha256=digest(runtime)) if args.runtime_build else None,
        electron_checkout=str(checkout), diagnostics=args.diagnostics, microsoft_auth=args.with_microsoft_auth,
        wider_integrations=args.with_integrations,
        repository_config_sha256=digest(args.repository_config) if args.repository_config else None)
    stamp = out / 'PIPELINE.json'
    if stamp.exists() and json.loads(stamp.read_text()) != identity:
        raise ValueError('Desktop build inputs changed; choose a fresh --output-root to preserve completed outputs')
    stamp.write_text(json.dumps(identity, indent=2) + '\n')
    logs = out / 'logs'
    logs.mkdir(exist_ok=True)

    def run(name, argv):
        print('Desktop source build: ' + name + ' (log: ' + str(logs / (name + '.log')) + ')', flush=True)
        with (logs / (name + '.log')).open('a') as log:
            subprocess.run(argv, cwd=ROOT, stdin=subprocess.DEVNULL,
                stdout=log, stderr=subprocess.STDOUT, check=True)

    if not args.runtime_build and not runtime.exists():
        if not args.electron_checkout and not (out / 'electron/SOURCE.json').exists():
            run('electron-sync', command('build_electron_source.py', '--output-root', out / 'electron', '--sync', '--jobs', jobs))
        run('electron', electron)
    verify_receipt(runtime)
    # Also verifies repository pins, policies, exact ELF hashes and the paired
    # Node product. Generated admission files remain in this workflow's output.
    from electron_admission import prepare
    prepare(runtime, out / 'electron-admission.h')
    for name, argv, resumable in stages:
        path = receipts[name]
        if path.exists():
            verified = verify_receipt(path)
            if name == 'frontend' and args.repository_config:
                if (verified.get('repository') or {}).get('configuration_sha256') != digest(args.repository_config):
                    raise ValueError('The selected Coast Packages receipt uses a different public repository configuration')
            if name == 'application':
                packaged = json.loads(path.read_text())
                compilation = path.with_name('BUILD.json')
                verify_receipt(compilation)
                if packaged['compilation_receipt_sha256'] != digest(compilation):
                    raise ValueError('Application compilation and packaging receipts differ')
            print('Verified desktop component: ' + name, flush=True)
            continue
        if name in overrides:
            raise ValueError('Missing explicitly selected source-build receipt: ' + str(path))
        if not resumable and path.parent.exists():
            failed = out / 'failed'
            failed.mkdir(exist_ok=True)
            path.parent.rename(failed / (name + '-' + str(time.time_ns())))
        run(name, argv)
        verify_receipt(path)
    manifest = out / 'image/MANIFEST.json'
    if not manifest.exists():
        if manifest.parent.exists():
            failed = out / 'failed'
            failed.mkdir(exist_ok=True)
            manifest.parent.rename(failed / ('image-' + str(time.time_ns())))
        run('assembly', assembly)
    result = verify_receipt(manifest)
    if result.get('inherited_iso') is not False or not result.get('gui_libraries_compiled_from_source'):
        raise ValueError('The full desktop must use fresh assembly and source-built GUI libraries')
    if any(digest(ROOT / name) != expected for name, expected in source_identity.items()):
        raise ValueError('Desktop sources changed while building')
    record = dict(state='source-built desktop assembled; guest qualification separate',
        iso=result['iso'], iso_sha256=result['iso_sha256'], inherited_iso=False,
        manifest_sha256=digest(manifest), pipeline_sha256=digest(stamp),
        components={name: dict(path=str(path), sha256=digest(path)) for name, path in receipts.items()},
        runtime_build_sha256=digest(runtime), prebuilt_application_inputs=result['prebuilt_application_inputs'],
        wider_integrations=args.with_integrations,
        optional_microsoft_auth_binaries=args.with_microsoft_auth)
    (out / 'BUILD.json').write_text(json.dumps(record, indent=2) + '\n')
    print('Fresh source-built desktop ISO:', result['iso'], flush=True)
    if args.qualify:
        proof = out / 'qualification/QUALIFICATION.json'
        if proof.exists() and verify_receipt(proof).get('state') == 'passed':
            if json.loads(proof.read_text())['iso_sha256'] != result['iso_sha256']:
                raise ValueError('The qualification receipt describes another image')
            print('Verified fresh desktop qualification:', proof, flush=True)
        else:
            if proof.parent.exists():
                failed = out / 'failed'
                failed.mkdir(exist_ok=True)
                proof.parent.rename(failed / ('qualification-' + str(time.time_ns())))
            run('qualification', command('qualify_desktop.py', '--iso', result['iso'], '--output-root', proof.parent))
        if verify_receipt(proof).get('state') != 'passed':
            raise ValueError('The fresh desktop qualification did not pass')
        if any(digest(ROOT / name) != expected for name, expected in source_identity.items()):
            raise ValueError('Desktop sources changed during qualification')
        record.update(state='source-built desktop assembled and qualified in Coast',
            qualification=dict(path=str(proof), sha256=digest(proof)))
        (out / 'BUILD.json').write_text(json.dumps(record, indent=2) + '\n')


if __name__ == '__main__':
    main()

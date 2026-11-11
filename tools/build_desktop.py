#!/usr/bin/env python3
"""Build a fresh Coast Chromium/Code OSS ISO from the public pinned sources.

Host setup is a prerequisite. Each completed component is verified and reused
on restart. An explicit component map can reuse source-build receipts; existing
ISOs and runtime staging directories are never accepted as desktop inputs.
"""
from build_state import project_lock
from build_state import atomic_write
import argparse
import fcntl
import json
import os
from pathlib import Path
import subprocess
import sys
import time

from source_tree import audit, digest
from build_progress import SourceSyncStatus, run_logged
from build_components import APPS, APP_REQUIRES, APP_COMPONENTS, DEPENDENCIES, RESTARTABLE, app_selection, components_for, graph_for, receipt_path
from build_session import BuildSession
from build_state import Checkpoints, read_json, write_json

ROOT = Path(__file__).resolve().parents[1]


def verify_receipt(path):
    path = Path(path).resolve(strict=True)
    record = json.loads(path.read_text())
    for field in ('source_sha256', 'source_policy_sha256', 'build_scripts_sha256'):
        for name, expected in record.get(field, {}).items():
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
    parser.add_argument('--output-root', type=Path, default=ROOT / 'build/coast')
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--electron-checkout', type=Path,
                        help='optional verified Electron checkout used read-only instead of a fresh source sync')
    parser.add_argument('--runtime-build', type=Path,
                        help='optional complete source-built Electron/Node receipt; requires --electron-checkout')
    parser.add_argument('--components', type=Path,
                        help='optional JSON map of component names to verified source-build receipt paths')
    parser.add_argument('--diagnostics', action='store_true', help='include inert, opt-in Code OSS QA diagnostics')
    parser.add_argument('--qualify', action='store_true', help='also check boot, terminal, addons, browser and editor in owned QEMU guests')
    parser.add_argument('--repository-config', type=Path,
                        help='optional explicit public repository trust JSON')
    parser.add_argument('--with-microsoft-auth', action='store_true',
                        help='include upstream proprietary Microsoft sign-in binaries (omitted by default)')
    parser.add_argument('--plan', action='store_true', help='print the build commands without running them')
    parser.add_argument('--apps', default='all', help='space/comma-separated apps, all (default), or none')
    parser.add_argument('--without', default='', help='omit apps and their runtime dependents; explain exclusions in the plan')
    parser.add_argument('--add', default='', help='add apps to the last ISO selection, building missing dependencies')
    parser.add_argument('--build-only', action='store_true', help='build components without assembling the ISO')
    parser.add_argument('--target', default='', help='apps or internal component names to build with their dependencies')
    args = parser.parse_args()
    if args.build_only and set(args.target.replace(',', ' ').split()) & {'image', 'qualification'}:
        parser.error('Use make iso (and QUALIFY=1) for assembly and guest verification')
    args.diagnostics = args.diagnostics or args.qualify
    if not 1 <= args.jobs <= 24:
        parser.error('--jobs must be 1..24')
    if args.runtime_build and not args.electron_checkout:
        parser.error('--runtime-build also requires its verified --electron-checkout')
    if Path(os.path.abspath(args.output_root)) != args.output_root.resolve():
        parser.error('Choose a real output directory, without symlinked parents')
    out = args.output_root.resolve()
    last_selection = read_json(out / 'image/MANIFEST.json') or read_json(out / 'SELECTION.json', {})
    previous_apps = last_selection.get('apps') if args.add else None
    apps, omitted = app_selection(args.apps, args.without, args.add, previous_apps)
    if args.with_microsoft_auth and 'codeoss' in apps:
        parser.error('WITH_MICROSOFT_AUTH=1 needs WebKitGTK, libsoup, libsecret, libcurl and libuuid '
                     'in the Coast runtime; these dependencies are not built yet. '
                     'Use WITH_MICROSOFT_AUTH=0 for the non-broker sign-in path.')
    needed = components_for(apps, args.target if args.build_only else None)
    if args.build_only and not args.target:
        needed.discard('image')
    graph = graph_for(apps)
    if 'electron' in needed:
        graph['kernel'].add('electron')
    else:
        graph['kernel'].discard('electron')
    overrides = json.loads(args.components.read_text()) if args.components else {}
    names = {'glibc', 'libgcc', 'cxx', 'gui-sources', 'gui-runtime', 'application', 'sdk',
             'addons', 'rust', 'kernel', 'native', 'compiler', 'packages', 'cli', 'bridge', 'assets'}
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
        ('program', command('build_program_ports.py', '--compiler-build', receipts['compiler'],
            '--dependencies-build', receipts['packages'].with_name('DEPENDENCIES.json'),
            '--output-root', receipts['program'].parent, '--jobs', jobs), True),
        ('makepkg', command('build_desktop_makepkg.py', '--compiler-build', receipts['compiler'],
            '--packages-build', receipts['packages'], '--program-build', receipts['program'],
            '--output-root', receipts['makepkg'].parent, '--jobs', jobs), True),
        ('frontend', command('build_brine.py', '--compiler-build', receipts['compiler'],
            '--program-build', receipts['program'], '--output-root', receipts['frontend'].parent,
            *(['--repository-config', args.repository_config.resolve()] if args.repository_config else [])), False),
        ('qt', command('build_qt6.py', '--compiler-build', receipts['compiler'],
            '--output-root', receipts['qt'].parent, '--jobs', jobs), True),
        ('qemu', command('build_qemu.py', '--compiler-build', receipts['compiler'],
            '--output-root', receipts['qemu'].parent, '--jobs', jobs), True),
        ('viewer', command('prepare_qemu_viewer.py', '--qemu-build', receipts['qemu'],
            '--program-build', receipts['program'], '--output-root', receipts['viewer'].parent), False),
    ]
    stages = [stage for stage in stages if stage[0] in needed]
    if 'electron' not in needed:
        for index, (name, argv, resumable) in enumerate(stages):
            if name == 'kernel':
                position = argv.index('--runtime-build')
                stages[index] = (name, argv[:position] + argv[position + 2:], resumable)
    assembly = command('assemble_desktop.py', '--runtime-build', runtime, '--output-root', out / 'image',
        '--native-build', receipts['native'].parent, '--kernel-build', receipts['kernel'].parent,
        '--gui-runtime', receipts['gui-runtime'], '--application-build', receipts['application'].parent,
        '--addons-build', receipts['addons'], '--rust-build', receipts['rust'], '--bridge-build', receipts['bridge'],
        '--assets-build', receipts['assets'], '--cli-build', receipts['cli'], '--compiler-build', receipts['compiler'],
        '--packages-build', receipts['packages'],
        *[item for name in ('program', 'makepkg', 'frontend', 'qt', 'qemu', 'viewer')
            for item in ('--' + name + '-build', receipts[name])],
        *(['--diagnostics'] if args.diagnostics else []),
        *(['--with-microsoft-auth'] if args.with_microsoft_auth else []))
    assembly_options = {'--runtime-build': 'electron', '--native-build': 'native', '--kernel-build': 'kernel',
        '--gui-runtime': 'gui-runtime', '--application-build': 'application', '--addons-build': 'addons',
        '--rust-build': 'rust', '--bridge-build': 'bridge', '--assets-build': 'assets', '--cli-build': 'cli',
        '--compiler-build': 'compiler', '--packages-build': 'packages', '--program-build': 'program',
        '--makepkg-build': 'makepkg', '--frontend-build': 'frontend', '--qt-build': 'qt',
        '--qemu-build': 'qemu', '--viewer-build': 'viewer'}
    for option, name in assembly_options.items():
        if name not in needed and option in assembly:
            position = assembly.index(option)
            del assembly[position:position + 2]
    assembly += ['--apps', ' '.join(apps) or 'none']
    electron = command('build_electron_source.py', '--output-root', out / 'electron', '--jobs', jobs,
        *(['--checkout', checkout] if args.electron_checkout else []), '--build')
    if args.plan:
        print(json.dumps(dict(runtime_receipt=str(runtime) if 'electron' in needed else None, inherited_iso=False,
            apps=apps, omitted_apps=omitted, app_dependencies={app: sorted(APP_REQUIRES.get(app, ())) for app in apps},
            build_components=sorted(needed), shared_runtime_consumers=sorted(set(apps) & {'chromium', 'codeoss'}),
            source_sync=None if 'sources' not in needed or args.electron_checkout or args.runtime_build else
                command('build_electron_source.py', '--output-root', out / 'electron', '--sync', '--jobs', jobs),
            runtime_build=None if 'electron' not in needed or args.runtime_build else electron,
            stages=[dict(name=name, receipt=str(receipts[name]), reused=name in overrides, argv=argv)
                    for name, argv, _ in stages], assembly=None if args.build_only else assembly,
            qualification=command('qualify_desktop.py', '--iso', out / 'image/Coast.iso',
                '--output-root', out / 'qualification') if args.qualify and not args.build_only else None), indent=2))
        return
    inventory = audit()
    source_identity = {name: entry['sha256'] for name, entry in inventory.items() if not name.endswith('.md')}
    from clean_project import assert_idle, validate_output
    validate_output(ROOT, out)
    assert_idle(ROOT, paths=[out])
    out.mkdir(parents=True, exist_ok=True)
    session_graph = graph_for(last_selection.get('apps', apps)) if args.build_only else graph
    if 'electron' in needed:
        session_graph['kernel'].add('electron')
    session = BuildSession(ROOT, out, source_identity, session_graph)
    if last_selection.get('apps') is not None:
        write_json(out / 'SELECTION.json', dict(apps=last_selection['apps']))
    stamp = out / 'PIPELINE.json'
    identity = dict(source_sha256=source_identity, apps=apps,
        overrides={name: dict(path=str(receipts[name]), sha256=digest(receipts[name])) for name in overrides},
        electron_checkout=str(checkout), diagnostics=args.diagnostics, microsoft_auth=args.with_microsoft_auth)
    previous = read_json(stamp)
    if previous and previous != identity:
        write_json(out / 'history' / ('pipeline-' + str(time.time_ns()) + '.json'), previous)
    write_json(stamp, identity)
    for app, reason in sorted(omitted.items()):
        print('Omitted app: ' + app + ' (' + reason + ')', flush=True)
    if 'chromium' not in apps and 'codeoss' in apps:
        print('Shared Electron/Chromium engine is required by Code OSS; only the browser app is omitted.', flush=True)
    total = len(stages) + int('sources' in needed) + int('electron' in needed) + int(not args.build_only) + int(args.qualify and not args.build_only)
    step = 0
    completed = {}

    def execute(name, argv, path, *, restart=False, external=False, detail=None):
        nonlocal step
        step += 1
        dependencies = [completed[dep] for dep in graph.get(name, ()) if dep in completed]
        def verify(path):
            record = verify_receipt(path)
            aliases = {'gui_runtime': 'gui-runtime', 'sdk_receipt': 'sdk', 'application_sdk': 'sdk', 'runtime': 'electron'}
            for dep, dep_path in completed.items():
                prefix = dep.replace('-', '_')
                keys = [prefix + '_build_sha256'] + [key + '_sha256' for key, value in aliases.items() if value == dep]
                for key in keys:
                    if key in record and record[key] != digest(dep_path):
                        raise ValueError('Component uses a different dependency: ' + name + ' -> ' + dep)
            if name == 'sources':
                from build_electron_source import verify as verify_sources, LOCK
                if record.get('lock_sha256') != digest(LOCK):
                    raise ValueError('Electron source lock changed')
                verify_sources(checkout, json.loads(LOCK.read_text()), overlays=True)
            if name == 'qualification' and record.get('state') != 'passed':
                raise ValueError('Qualification is incomplete; retry unfinished phases')
            if name == 'image' and record.get('apps', list(APPS)) != apps:
                raise ValueError('Image has a different application selection')
            if name == 'kernel':
                expected = digest(runtime) if 'electron' in needed else None
                if record.get('runtime_build_sha256') != expected:
                    raise ValueError('Kernel admits a different runtime')
            if name == 'application':
                compilation = path.with_name('BUILD.json')
                verify_receipt(compilation)
                if record['compilation_receipt_sha256'] != digest(compilation):
                    raise ValueError('Application compilation and packaging receipts differ')
            if name == 'frontend' and args.repository_config:
                if (record.get('repository') or {}).get('configuration_sha256') != digest(args.repository_config):
                    raise ValueError('Package repository configuration changed')
            return record
        record = session.run(name, argv, path, verify, dependencies=dependencies,
            restart=restart, step=step, total=total, detail=detail, external=external,
            extra={'apps': apps, 'diagnostics': args.diagnostics} if name == 'image' else None)
        completed[name] = Path(path)
        return record

    if 'sources' in needed:
        if args.electron_checkout or args.runtime_build:
            from build_electron_source import verify as verify_sources, LOCK
            verify_sources(checkout, json.loads(LOCK.read_text()))
            completed['sources'] = ROOT / 'ports/electron/sources.lock.json'
            step += 1
        else:
            source_lock = json.loads((ROOT / 'ports/electron/sources.lock.json').read_text())
            execute('sources', command('build_electron_source.py', '--output-root', out / 'electron', '--sync', '--jobs', jobs),
                out / 'electron/SOURCE.json', detail=SourceSyncStatus(checkout, source_lock['repositories']))
    if 'electron' in needed:
        execute('electron', electron, runtime, external=bool(args.runtime_build))
        from electron_admission import prepare
        prepare(runtime, out / 'electron-admission.h')
    for name, argv, resumable in stages:
        execute(name, argv, receipts[name], restart=not resumable, external=name in overrides)
    if args.build_only:
        print('Requested components are built. Add an app to the ISO with: make iso ADD=app-name', flush=True)
        return
    result = execute('image', assembly, out / 'image/MANIFEST.json', restart=True)
    write_json(out / 'SELECTION.json', dict(apps=apps))
    if result.get('inherited_iso') is not False:
        raise ValueError('The ISO must use fresh source assembly')
    if any(digest(ROOT / name) != expected for name, expected in source_identity.items()):
        raise ValueError('Sources changed while building; rerun make iso to revalidate affected components')
    record = dict(state='source-built desktop assembled; guest qualification separate',
        iso=result['iso'], iso_sha256=result['iso_sha256'], apps=apps, inherited_iso=False,
        manifest_sha256=digest(out / 'image/MANIFEST.json'), pipeline_sha256=digest(stamp),
        components={name: dict(path=str(path), sha256=digest(path)) for name, path in completed.items()},
        runtime_build_sha256=digest(runtime) if 'electron' in needed else None,
        prebuilt_application_inputs=result.get('prebuilt_application_inputs', []),
        optional_microsoft_auth_binaries=args.with_microsoft_auth)
    write_json(out / 'BUILD.json', record)
    print('Source-built Coast ISO:', result['iso'], flush=True)
    if args.qualify:
        proof = out / 'qualification/QUALIFICATION.json'
        execute('qualification', command('qualify_desktop.py', '--iso', result['iso'], '--output-root', proof.parent), proof)
        if verify_receipt(proof).get('state') != 'passed':
            raise ValueError('Guest qualification did not pass')
        if any(digest(ROOT / name) != expected for name, expected in source_identity.items()):
            raise ValueError('Sources changed during qualification')
        record.update(state='source-built desktop assembled and qualified in Coast',
            qualification=dict(path=str(proof), sha256=digest(proof)))
        write_json(out / 'BUILD.json', record)


if __name__ == '__main__':
    from contextlib import nullcontext
    try:
        with (nullcontext() if '--plan' in sys.argv else project_lock()):
            main()
    except KeyboardInterrupt:
        print('\nBuild interrupted. Component logs are saved under the build directory.', file=sys.stderr)
        sys.exit(130)
    except subprocess.CalledProcessError as error:
        sys.exit(error.returncode if error.returncode > 0 else 1)
    except (OSError, ValueError) as error:
        sys.exit(str(error))

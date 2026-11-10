#!/usr/bin/env python3
"""Qualify one exact fresh source-built desktop image in owned disposable VMs."""
import argparse
import fcntl
import json
from pathlib import Path
import subprocess
import sys
import traceback

from build_desktop import verify_receipt
from source_tree import digest

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, required=True)
    parser.add_argument('--output-root', type=Path, required=True)
    parser.add_argument('--timeout-scale', type=float, default=2)
    args = parser.parse_args()
    if not 1 <= args.timeout_scale <= 4:
        parser.error('--timeout-scale must be 1..4')
    iso = args.iso.resolve(strict=True)
    manifest = iso.with_name('MANIFEST.json')
    image = verify_receipt(manifest)
    if image['iso_sha256'] != digest(iso) or image.get('inherited_iso') is not False:
        parser.error('Select the exact fresh-assembly image')
    if not image.get('gui_libraries_compiled_from_source'):
        parser.error('Qualification requires the source-built GUI closure')
    if not image.get('diagnostics_qa_only'):
        parser.error('Build with --diagnostics (or build_desktop.py --qualify) for opt-in editor observations')
    addons = Path(image['components']['addons_build']['path'])
    if digest(addons) != image['addons_build_sha256']:
        raise ValueError('The native-module receipt changed')
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=False)
    phases = [('boot', 'test_desktop_boot.py', []),
              ('terminal', 'test_desktop_terminal.py', []),
              ('development', 'test_desktop_development.py', []),
              ('packages', 'test_desktop_packages.py', []),
              ('addons', 'test_codeoss_addons.py', ['--addons-build', addons]),
              ('browser', 'test_chromium_browser.py', ['--network-backend', 'passt', '--timeout-scale', args.timeout_scale]),
              ('codeoss', 'test_codeoss_desktop.py', ['--usability-check', '--timeout-scale', args.timeout_scale])]
    if image.get('wider_integrations'):
        phases += [('integrations', 'test_desktop_integrations.py', []),
                   ('qt', 'test_qt6_desktop.py', []),
                   ('qt-clipboard', 'test_qt6_clipboard.py', []),
                   ('qemu-viewer', 'test_qemu_viewer.py', ['--preinstalled'])]
    scripts = [Path(__file__), ROOT / 'tools/test_runtime.py', ROOT / 'tools/test_desktop.py',
               ROOT / 'ports/codeoss-app/addons_check.cjs', ROOT / 'ports/codeoss-app/proxy_check.cjs',
               *[ROOT / 'tools' / script for _, script, _ in phases]]
    if image.get('wider_integrations'):
        scripts += [ROOT / 'tools/desktop_repository_fixture.py', ROOT / 'tools/serve_repository.py',
                    ROOT / 'tools/guest_ramfs.py', ROOT / 'tools/clipboard-window-check.c']
    sources = {str(path.relative_to(ROOT)): digest(path) for path in scripts}
    report = dict(state='failed', iso=str(iso), iso_sha256=digest(iso),
        manifest=dict(path=str(manifest), sha256=digest(manifest)), phases={}, source_sha256=sources,
        inherited_iso=False, gui_libraries_compiled_from_source=True, selected_images_changed=False)
    with Path(str(iso) + '.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_SH | fcntl.LOCK_NB)
        try:
            for name, script, extra in phases:
                print('Qualifying fresh desktop: ' + name, flush=True)
                command = list(map(str, [sys.executable, '-B', ROOT / 'tools' / script,
                    '--iso', iso, '--output-root', out / name, *extra]))
                with (out / (name + '.log')).open('w') as log:
                    subprocess.run(command, stdin=subprocess.DEVNULL, stdout=log,
                        stderr=subprocess.STDOUT, check=True)
                path = out / name / 'report.json'
                result = json.loads(path.read_text())
                stopped = (all(case['owned_vm_stopped'] for case in result['cases'])
                           if name == 'boot' else result.get('owned_vm_stopped'))
                if (not result['state'].startswith('passed') or not stopped or
                        result.get('iso_sha256') != report['iso_sha256']):
                    raise ValueError('Qualification did not pass for this exact image: ' + name)
                report['phases'][name] = dict(path=str(path), sha256=digest(path), owned_vm_stopped=True)
                print('Passed fresh desktop: ' + name, flush=True)
            if digest(iso) != report['iso_sha256'] or digest(manifest) != report['manifest']['sha256']:
                raise ValueError('The image or its component manifest changed')
            if any(digest(ROOT / name) != expected for name, expected in sources.items()):
                raise ValueError('A qualification harness changed during this run')
            report['image_unchanged'] = True
            report['state'] = 'passed'
        except BaseException as error:
            report['error'] = repr(error)
            (out / 'exception.txt').write_text(traceback.format_exc())
            raise
        finally:
            (out / 'QUALIFICATION.json').write_text(json.dumps(report, indent=2) + '\n')
    print('Fresh desktop qualification passed:', iso, flush=True)


if __name__ == '__main__':
    main()

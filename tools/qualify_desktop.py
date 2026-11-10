#!/usr/bin/env python3
"""Qualify one exact fresh source-built desktop image in owned disposable VMs."""
from build_state import project_lock
from build_state import atomic_write, read_json, write_json
import argparse
import fcntl
import json
import shutil
from pathlib import Path
import subprocess
import sys
import traceback

from build_desktop import verify_receipt
from source_tree import digest

ROOT = Path(__file__).resolve().parents[1]


def phase_plan(image, timeout_scale=2):
    from build_components import APPS, app_names
    apps = app_names(image.get('apps', APPS))
    features = apps | ({'engine'} if apps & {'chromium', 'codeoss'} else set())
    addons = image.get('components', {}).get('addons_build', {}).get('path', '')
    candidates = [
        ('boot', 'test_desktop_boot.py', [], set()),
        ('terminal', 'test_desktop_terminal.py', [], {'bash', 'engine'}),
        ('development', 'test_desktop_development.py', [], {'gcc', 'bash'}),
        ('packages', 'test_desktop_packages.py', [], {'brine', 'makepkg'}),
        ('addons', 'test_codeoss_addons.py', ['--addons-build', addons], {'codeoss'}),
        ('browser', 'test_chromium_browser.py', ['--network-backend', 'passt', '--timeout-scale', timeout_scale], {'chromium'}),
        ('codeoss', 'test_codeoss_desktop.py', ['--usability-check', '--timeout-scale', timeout_scale], {'codeoss'}),
        ('integrations', 'test_desktop_integrations.py', [], {'brine', 'makepkg', 'qt', 'qemu-viewer'}),
        ('qt', 'test_qt6_desktop.py', [], {'qt'}),
        ('qt-clipboard', 'test_qt6_clipboard.py', [], {'qt', 'chromium'}),
        ('qemu-viewer', 'test_qemu_viewer.py', ['--preinstalled'], {'qemu-viewer', 'brine', 'makepkg', 'qt'}),
    ]
    phases, skipped = [], {}
    for name, script, extra, required in candidates:
        if required <= features:
            phases.append((name, script, extra))
        else:
            skipped[name] = 'requires ' + ', '.join(sorted(required - features))
    return phases, skipped


def passed_phase(path, iso_sha256, name):
    result = read_json(path, {})
    stopped = (bool(result.get('cases')) and all(case.get('owned_vm_stopped') for case in result['cases'])
               if name == 'boot' else result.get('owned_vm_stopped'))
    return bool(str(result.get('state', '')).startswith('passed') and stopped and result.get('iso_sha256') == iso_sha256)


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
    engine = bool(set(image.get('apps', ('chromium', 'codeoss'))) & {'chromium', 'codeoss'})
    if engine and not image.get('gui_libraries_compiled_from_source'):
        parser.error('Qualification requires the source-built GUI closure')
    if 'codeoss' in image.get('apps', ('codeoss',)) and not image.get('diagnostics_qa_only'):
        parser.error('Build with --diagnostics (or build_desktop.py --qualify) for opt-in editor observations')
    if image.get('addons_build_sha256'):
        addons = Path(image['components']['addons_build']['path'])
        if digest(addons) != image['addons_build_sha256']:
            raise ValueError('The native-module receipt changed')
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=True)
    phases, skipped = phase_plan(image, args.timeout_scale)
    scripts = [Path(__file__), ROOT / 'tools/test_runtime.py', ROOT / 'tools/test_desktop.py',
               ROOT / 'ports/codeoss-app/addons_check.cjs', ROOT / 'ports/codeoss-app/proxy_check.cjs',
               *[ROOT / 'tools' / script for _, script, _ in phases]]
    if image.get('wider_integrations'):
        scripts += [ROOT / 'tools/desktop_repository_fixture.py', ROOT / 'tools/serve_repository.py',
                    ROOT / 'tools/guest_ramfs.py', ROOT / 'tools/clipboard-window-check.c']
    sources = {str(path.relative_to(ROOT)): digest(path) for path in scripts}
    report = dict(state='failed', iso=str(iso), iso_sha256=digest(iso),
        manifest=dict(path=str(manifest), sha256=digest(manifest)), phases={}, source_sha256=sources,
        inherited_iso=False, gui_libraries_compiled_from_source=image.get('gui_libraries_compiled_from_source', False), selected_images_changed=False,
        apps=image.get('apps'), skipped_phases=skipped)
    state_path = out / 'QUALIFICATION-STATE.json'
    binding = dict(iso_sha256=report['iso_sha256'], manifest=report['manifest'], source_sha256=sources, timeout_scale=args.timeout_scale)
    state = read_json(state_path, {})
    if state.get('binding') != binding:
        state = dict(binding=binding, phases={})
    write_json(state_path, state)
    with Path(str(iso) + '.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_SH | fcntl.LOCK_NB)
        try:
            for name, script, extra in phases:
                path = out / name / 'report.json'
                previous = state['phases'].get(name, {})
                if previous.get('sha256') and path.is_file() and digest(path) == previous['sha256'] and passed_phase(path, report['iso_sha256'], name):
                    report['phases'][name] = previous
                    print('Verified guest checkpoint: ' + name, flush=True)
                    continue
                if (out / name).exists():
                    from clean_project import select_paths
                    for disposable in select_paths([out / name], ROOT):
                        if disposable.is_dir() and not disposable.is_symlink():
                            shutil.rmtree(disposable)
                        else:
                            disposable.unlink(missing_ok=True)
                    if (out / name).exists():
                        raise ValueError('Qualification directory contains protected files; select a new output directory: ' + str(out / name))
                command = list(map(str, [sys.executable, '-B', ROOT / 'tools' / script,
                    '--iso', iso, '--output-root', out / name, *extra]))
                from build_progress import run_logged
                run_logged('qualify-' + name, command, cwd=ROOT, log_path=out / (name + '.log'))
                if not passed_phase(path, report['iso_sha256'], name):
                    raise ValueError('Qualification did not pass for this exact image: ' + name)
                report['phases'][name] = dict(path=str(path), sha256=digest(path), owned_vm_stopped=True)
                state['phases'][name] = report['phases'][name]
                write_json(state_path, state)
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
            atomic_write(out / 'QUALIFICATION.json', json.dumps(report, indent=2) + '\n')
    print('Fresh desktop qualification passed:', iso, flush=True)


if __name__ == '__main__':
    with project_lock():
        main()

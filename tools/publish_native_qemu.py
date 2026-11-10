#!/usr/bin/env python3
"""Promote qualified Coast-built QEMU, libraries and Ninja in a new signed snapshot."""
import argparse
import json
from pathlib import Path
import tarfile

from fetch_sources import fetch
from repository_extension import extend
from source_tree import digest

ROOT = Path(__file__).resolve().parents[1]
LIBRARIES = dict(libffi='libffi', pcre2='pcre2', glib='glib2', zlib='zlib',
                 pixman='pixman', libslirp='libslirp', libucontext='libucontext')
SDK = 'usr/lib/coast/qemu-sdk/'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repository', type=Path, required=True,
                        help='candidate containing qemu-build, coast-vm and their sources')
    parser.add_argument('--qualification', type=Path, required=True,
                        help='completed tools/test_qemu_native.py output directory')
    parser.add_argument('--output-root', type=Path, required=True)
    parser.add_argument('--staging-root', type=Path, required=True)
    parser.add_argument('--signing-home', type=Path, default=ROOT / 'build/coast-signing-keys')
    parser.add_argument('--repo-add', type=Path, default=ROOT / 'build/coast-security-ports-r3/install/usr/bin/repo-add')
    args = parser.parse_args()
    proof = args.qualification.resolve(strict=True)
    qualified = json.loads((proof / 'report.json').read_text())
    if (qualified['state'] != 'passed' or qualified['build_host'] != 'Coast'
            or qualified['guest_execution_on_linux'] is not False or not qualified['owned_vm_stopped']):
        parser.error('A successful, stopped, native Coast qualification is required')
    if digest(args.repository) != qualified['repository_manifest_sha256']:
        parser.error('Select the exact repository used by the native qualification')
    for name, expected in qualified['exports'].items():
        if digest(proof / name) != expected:
            raise ValueError('Qualified artifact changed: ' + name)
    build = json.loads((proof / 'qemu-build.json').read_text())
    nested = json.loads((proof / 'nested-report.json').read_text())
    if build['build_mode'] != 'native-coast' or build['target_host_executed'] is not True or nested['state'] != 'passed':
        parser.error('Native emulator compilation and nested desktop checks must both pass')
    for relative, expected in build['source_sha256'].items():
        if digest(ROOT / relative) != expected:
            raise ValueError('Build source changed after qualification: ' + relative)
    if digest(proof / 'ninja-native') != build['native_tools']['ninja']['sha256']:
        raise ValueError('Native Ninja artifact differs from its compilation receipt')
    work = args.staging_root.resolve()
    work.mkdir(parents=True, exist_ok=False)
    stage = work / 'native-runtime'
    stage.mkdir(exist_ok=False)
    with tarfile.open(proof / 'qemu-native-runtime.tar.gz') as bundle:
        bundle.extractall(stage, filter='data')
    for relative, asset in build['files'].items():
        if relative.startswith('usr/src/'):
            continue
        if digest(stage / relative) != asset['sha256']:
            raise ValueError('Native runtime differs from compilation receipt: ' + relative)
    baseline = json.loads(args.repository.read_text())
    replacements = {}
    packages = []

    def package(name, version, description, license, files, depends=(), provides=()):
        replacements[name] = baseline['packages'][name]['version']
        packages.append(dict(name=name, version=version, description=description,
            license=license, depends=list(depends), provides=list(provides), files=files))

    package('qemu', '10.2.4-2', 'QEMU x86-64 TCI emulator and disk tools compiled inside Coast',
        'GPL-2.0-only', {p: stage / p for p in build['files'] if p.startswith('usr/bin/')},
        depends=['musl', 'qemu-common=10.2.4-2'], provides=['qemu-system-x86_64', 'qemu-img', 'qemu-io'])
    common = {p: stage / p for p in build['files'] if p.startswith('usr/share/')}
    summary = work / 'native-qualification.json'
    summary.write_text(json.dumps(dict(build_host='Coast', backend='TCI',
        checks=qualified['checks'], nested_checks=nested['checks'],
        source_sha256=build['source_sha256'], source_built_firmware=build['source_built_firmware'],
        release_supplied_firmware=build['release_supplied_firmware'],
        runtime_sha256=qualified['exports']['qemu-native-runtime.tar.gz'],
        guest_execution_on_linux=False), indent=2) + '\n')
    common['usr/share/doc/qemu/native-qualification.json'] = summary
    common['usr/share/doc/qemu/coast.md'] = ROOT / 'Documentation/qemu.md'
    package('qemu-common', '10.2.4-2', 'Coast-built x86 firmware, licenses and emulator integration tools',
        'LicenseRef-Coast-Mixed', common)
    for key, name in LIBRARIES.items():
        metadata = build['packages'][key]
        files = {SDK + p: stage / (SDK + p) for p in build['sdk'][key]
                 if p.startswith(('lib/', 'include/')) and not p.endswith('.la')}
        license_root = stage / 'usr/share/licenses/qemu-port' / key
        files.update({'usr/share/licenses/' + name + '/' + p.name: p
                      for p in license_root.iterdir() if p.is_file()})
        depends = ['musl'] + (['libffi', 'pcre2', 'zlib'] if key == 'glib' else ['glib2'] if key == 'libslirp' else [])
        package(name, metadata['version'] + '-2', name + ' static SDK compiled inside Coast',
            metadata['license'], files, depends=depends)
    sdk_doc = work / 'sdk.txt'
    sdk_doc.write_text('Native static SDK: /usr/lib/coast/qemu-sdk\n'
        'export PKG_CONFIG_PATH=/usr/lib/coast/qemu-sdk/lib/pkgconfig\n'
        'python3 /usr/share/coast/qemu/check.py --sdk\n'
        'Install qemu-build for the complete offline native source rebuild.\n')
    package('qemu-devel', '10.2.4-2', 'Complete native-built QEMU dependency SDK', 'MIT',
        {'usr/share/doc/qemu/sdk.txt': sdk_doc},
        depends=[*LIBRARIES.values(), 'meson', 'ninja', 'pkgconf', 'gcc', 'make', 'python'])
    ninja = build['packages']['ninja']
    vendor = work / 'vendor'
    fetch(ninja, vendor)
    package('ninja', '1.13.2-4', 'Native Ninja with headless sessions and verified job cancellation',
        ninja['license'], {'usr/bin/ninja': proof / 'ninja-native',
            'usr/share/licenses/extra-programs/ninja/COPYING': proof / 'ninja-COPYING',
            'usr/src/coast/extra-programs/' + ninja['archive']: vendor / ninja['archive']}, depends=['musl'])
    recipe_dir = work / 'ninja-recipe'
    recipe_dir.mkdir()
    (recipe_dir / 'PKGBUILD').write_bytes((ROOT / 'userland/packages/recipes/ninja/PKGBUILD').read_bytes())
    extra = [ROOT / 'ports/qemu/ninja-session.patch', ROOT / 'ports/qemu/ninja_check.py']
    (recipe_dir / '.SRCINFO').write_text('pkgbase = ninja\n\tpkgdesc = Native Ninja build system\n'
        '\tpkgver = 1.13.2\n\tpkgrel = 4\n\tarch = x86_64\n\tlicense = Apache-2.0\n'
        '\tdepends = musl\n\tdepends = gcc-libs\n\tmakedepends = gcc\n\tmakedepends = python\n\tmakedepends = patch\n' +
        ''.join('\tsource = ' + p.name + '\n' for p in [vendor / ninja['archive'], *extra]) +
        ''.join('\tsha256sums = ' + digest(p) + '\n' for p in [vendor / ninja['archive'], *extra]) + '\npkgname = ninja\n')
    recipe = dict(name='ninja', version='1.13.2-4', description='Native Ninja with private job sessions',
        license='Apache-2.0', directory=recipe_dir, depends=['musl', 'gcc-libs'], makedepends=['gcc', 'python', 'patch'])
    old_recipe = next(r['Version'] for r in baseline['recipes'] if r['Name'] == 'ninja')
    manifest = extend(args.repository, args.output_root, args.signing_home, args.repo_add, packages,
        recipes=[recipe], replace_packages=replacements, replace_recipes={'ninja': old_recipe},
        distfiles=extra)
    receipt = dict(repository=str(manifest), qualification=str(proof),
        qualification_sha256=digest(proof / 'report.json'), replaced=replacements,
        compiled_files_in_source=False, signing_secret_included=False)
    (args.output_root / 'NATIVE-PUBLICATION.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps(receipt, indent=2))


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Add the native QEMU build kit and source recipe to a signed Coast snapshot."""
import argparse
import gzip
import io
import json
from pathlib import Path, PurePosixPath
import tarfile
import zipfile

from fetch_sources import fetch
from repository_extension import extend
from source_tree import digest, export, EPOCH

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repository', type=Path, default=ROOT / 'build/brine-repository-server/repository.json')
    parser.add_argument('--output-root', type=Path, required=True)
    parser.add_argument('--source-output', type=Path, default=ROOT / 'build/qemu-native-source/coast-qemu-10.2.4.tar.gz')
    parser.add_argument('--refresh-os-source', action='store_true', help='also version and refresh the complete OS source kit and build closure')
    parser.add_argument('--signing-home', type=Path, default=ROOT / 'build/coast-signing-keys')
    parser.add_argument('--repo-add', type=Path, default=ROOT / 'build/coast-security-ports-r3/install/usr/bin/repo-add')
    args = parser.parse_args()
    novnc = json.loads((ROOT / 'ports/qemu/novnc-source.json').read_text())
    novnc_vendor = ROOT / 'build/qemu-native-source/vendor'
    fetch(novnc, novnc_vendor)
    novnc_archive = novnc_vendor / novnc['archive']
    novnc_tree = ROOT / 'build/qemu-native-source' / novnc['source']
    if not novnc_tree.exists():
        with tarfile.open(novnc_archive) as bundle:
            bundle.extractall(novnc_tree.parent, filter='data')
    viewer_files = {'usr/share/novnc/' + str(p.relative_to(novnc_tree)): p
        for directory in ('core', 'vendor') for p in (novnc_tree / directory).rglob('*') if p.is_file()}
    viewer_files.update({'usr/share/novnc/' + name: novnc_tree / name for name in ('AUTHORS', 'LICENSE.txt')})
    viewer_files.update({'usr/share/licenses/novnc/' + p.name: p for p in (novnc_tree / 'docs').glob('LICENSE.*')})
    viewer_files['usr/src/coast/novnc/' + novnc_archive.name] = novnc_archive
    viewer_files['usr/src/coast/novnc/SOURCES.json'] = ROOT / 'ports/qemu/novnc-source.json'
    viewer = dict(name='novnc', version=novnc['version'] + '-1',
        description='Source JavaScript VNC client for viewing QEMU in Chromium',
        license=novnc['license'], depends=[], files=viewer_files)
    wrapper = args.source_output.resolve().parent / 'coast-vm'
    wrapper.parent.mkdir(parents=True, exist_ok=True)
    wrapper.write_text('#!/usr/bin/python3\nfrom runpy import run_path\nrun_path("/usr/share/coast/qemu/coast_vm.py", run_name="__main__")\n')
    wrapper.chmod(0o755)
    launcher = dict(name='coast-vm', version='1.0-1',
        description='Local QEMU desktop launcher and Chromium viewer', license='MIT',
        depends=['qemu', 'novnc', 'python'], files={
            'usr/bin/coast-vm': wrapper,
            'usr/share/coast/qemu/coast_vm.py': ROOT / 'ports/qemu/coast_vm.py',
            'usr/share/novnc/coast.html': ROOT / 'ports/qemu/coast.html',
            'usr/share/licenses/coast-vm/LICENSE': ROOT / 'LICENSE'})
    python_tools = json.loads((ROOT / 'ports/qemu/python-tools.json').read_text())
    python_packages, python_distfiles = [], []
    staging = args.source_output.resolve().parent / 'python-packages'
    for name, record in python_tools.items():
        for kind in ('source', 'wheel'):
            fetch(record[kind], ROOT / 'build/qemu-native-source/vendor')
        wheel = ROOT / 'build/qemu-native-source/vendor' / record['wheel']['archive']
        original = ROOT / 'build/qemu-native-source/vendor' / record['source']['archive']
        python_distfiles += [original, wheel]
        entries = {}
        with zipfile.ZipFile(wheel) as bundle:
            for entry in bundle.infolist():
                relative = PurePosixPath(entry.filename)
                if relative.is_absolute() or '..' in relative.parts:
                    raise ValueError('Unsafe Python wheel member')
                if entry.is_dir() or relative.suffix in ('.exe', '.pyc', '.pyo'):
                    continue
                data = bundle.read(entry)
                if data.startswith((b'\x7fELF', b'MZ')):
                    raise ValueError('Unexpected compiled Python tool payload: ' + entry.filename)
                path = staging / name / entry.filename
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
                entries['usr/lib/python3.14/site-packages/' + entry.filename] = path
        entries['usr/src/coast/python-build-tools/' + original.name] = original
        if name == 'pip':
            for command in ('pip', 'pip3'):
                path = staging / 'bin' / command
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('#!/usr/bin/python3\nfrom pip._internal.cli.main import main\nraise SystemExit(main())\n')
                path.chmod(0o755)
                entries['usr/bin/' + command] = path
        python_packages.append(dict(name='python-' + name, version=record['version'] + '-1',
            description='Native Python ' + name + ' build tooling with matching source archive',
            license=record['license'], depends=['python'], files=entries))
    sources = json.loads((ROOT / 'userland/vendor/qemu-sources.json').read_text())
    fetch(sources['ninja'], ROOT / 'build/qemu-native-source/vendor')
    ninja = ROOT / 'build/qemu-native-source/vendor' / sources['ninja']['archive']
    files = ['LICENSE', 'Documentation/qemu.md', 'userland/vendor/qemu-sources.json',
             'tools/build_qemu.py', 'tools/fetch_sources.py', 'tools/source_archive.py',
             'tools/linux_uapi.py', 'tools/qemu_firmware.py', 'tools/prepare_browser_host.py']
    files += [str(p.relative_to(ROOT)) for p in (ROOT / 'ports/qemu').rglob('*')
              if p.is_file() and '__pycache__' not in p.parts and p.suffix != '.pyc']
    inputs = {p: ROOT / p for p in files}
    inputs['vendor/' + ninja.name] = ninja
    inventory = {p: digest(source) for p, source in sorted(inputs.items())}
    archive = args.source_output.resolve()
    archive.parent.mkdir(parents=True, exist_ok=True)
    with archive.open('wb') as raw, gzip.GzipFile(fileobj=raw, mode='wb', mtime=EPOCH, filename='') as compressed, \
            tarfile.open(fileobj=compressed, mode='w|', format=tarfile.PAX_FORMAT) as bundle:
        for relative, source in sorted(inputs.items()):
            data = source.read_bytes()
            item = tarfile.TarInfo('coast-qemu/' + relative)
            item.mtime, item.uid, item.gid, item.mode, item.size = EPOCH, 0, 0, 0o644, len(data)
            bundle.addfile(item, io.BytesIO(data))
        data = (json.dumps(dict(version=1, files=inventory), indent=2) + '\n').encode()
        item = tarfile.TarInfo('coast-qemu/SOURCE-MANIFEST.json')
        item.mtime, item.uid, item.gid, item.mode, item.size = EPOCH, 0, 0, 0o644, len(data)
        bundle.addfile(item, io.BytesIO(data))
    recipes = archive.parent / 'recipe'
    recipes.mkdir(exist_ok=True)
    dependencies = ['qemu-source', 'base-devel', 'gcc', 'binutils', 'make', 'python',
                    'meson', 'ninja', 'pkgconf', 'gnupg', 'patch', 'xz', 'coast-linux-uapi',
                    'python-pip', 'python-setuptools']
    pkgbuild = '''# Maintainer: Coast local ports
pkgname=('qemu' 'qemu-common')
pkgbase=qemu
pkgver=10.2.4
pkgrel=2
pkgdesc='QEMU x86-64 TCI emulator, dependencies and BIOS built natively in Coast'
arch=('x86_64')
url='https://www.qemu.org/'
license=('GPL-2.0-only')
makedepends=('qemu-build')
source=("${BRINE_REPOSITORY_URL:-http://10.0.2.2:8765}/distfiles/coast-qemu-10.2.4.tar.gz")
sha256sums=('@SHA@')
noextract=('coast-qemu-10.2.4.tar.gz')

prepare() {
    cd "$srcdir"
    tar -xzf coast-qemu-10.2.4.tar.gz
}

build() {
    cd "$srcdir/coast-qemu"
    python3 -B tools/build_qemu.py --native --jobs "${COAST_BUILD_JOBS:-4}"
}

check() {
    cd "$srcdir/coast-qemu"
    python3 -B ports/qemu/check.py --runtime build/qemu-native/runtime --sdk
}

package_qemu() {
    depends=('musl' 'qemu-common=10.2.4-2')
    provides=('qemu-system-x86_64' 'qemu-img' 'qemu-io')
    cd "$srcdir/coast-qemu/build/qemu-native/runtime"
    install -dm755 "$pkgdir/usr/bin"
    cp -R usr/bin/. "$pkgdir/usr/bin/"
}

package_qemu-common() {
    pkgdesc='Source-built x86 QEMU BIOS/VGA/boot ROMs, firmware and validation tools'
    license=('LicenseRef-Coast-Mixed')
    depends=()
    cd "$srcdir/coast-qemu/build/qemu-native/runtime"
    install -dm755 "$pkgdir/usr/share"
    cp -R usr/share/. "$pkgdir/usr/share/"
}
'''.replace('@SHA@', digest(archive))
    (recipes / 'PKGBUILD').write_text(pkgbuild)
    (recipes / '.SRCINFO').write_text('pkgbase = qemu\n\tpkgdesc = Native QEMU and firmware build\n'
        '\tpkgver = 10.2.4\n\tpkgrel = 2\n\tarch = x86_64\n\tlicense = GPL-2.0-only\n'
        '\tmakedepends = qemu-build\n\tsource = coast-qemu-10.2.4.tar.gz\n'
        '\tsha256sums = ' + digest(archive) + '\n\n'
        'pkgname = qemu\n\tdepends = musl\n\tdepends = qemu-common=10.2.4-2\n\n'
        'pkgname = qemu-common\n\tlicense = LicenseRef-Coast-Mixed\n')
    package = dict(name='qemu-build', version='10.2.4-1',
        description='Offline native QEMU build kit and complete build dependency closure',
        license='LicenseRef-Coast-Mixed', depends=dependencies,
        files={'usr/src/coast/qemu-build/' + archive.name: archive,
               'usr/share/doc/qemu/native-build.md': ROOT / 'Documentation/qemu.md'})
    recipe = dict(name='qemu', version='10.2.4-2', description='Build QEMU, its seven libraries and BIOS inside Coast',
        license='GPL-2.0-only', directory=recipes, depends=['musl'], makedepends=['qemu-build'])
    packages = [*python_packages, viewer, launcher, package]
    distfiles = [archive, novnc_archive, *python_distfiles]
    replacements = {}
    if args.refresh_os_source:
        baseline = json.loads(args.repository.read_text())
        os_source = archive.parent / 'coast-source-2026.11.06-2.tar.xz'
        export(os_source, kernel_sources=True)
        packages += [dict(name='coast-source', version='2026.11.06-2',
            description='Coast OS sources, native emulator ports and offline kernel inputs',
            license='LicenseRef-Coast-Mixed', depends=[],
            files={'usr/src/coast/os/coast-source-2026.11.06.tar.xz': os_source,
                   'usr/src/coast/os/coast-source.tar.xz': os_source}),
            dict(name='coast-build', version='2026.11.06-2',
            description='Native Coast OS and nested QEMU build dependencies', license='MIT',
            files={}, depends=['coast-source', 'base-devel', 'gcc', 'binutils', 'make', 'python', 'git',
                'nasm', 'ninja', 'meson', 'pkgconf', 'm4', 'patch', 'gperf', 'xz', 'wayland-devel',
                'coast-distfiles', 'coast-linux-uapi', 'xorriso', 'mtools', 'qemu-build', 'coast-vm'])]
        replacements = {name: baseline['packages'][name]['version'] for name in ('coast-source', 'coast-build')}
        distfiles.append(os_source)
    manifest = extend(args.repository, args.output_root, args.signing_home, args.repo_add,
                      packages, recipes=[recipe], replace_packages=replacements, distfiles=distfiles)
    print(json.dumps(dict(repository=str(manifest), source_archive=str(archive),
        source_sha256=digest(archive), compiled_files_in_build_kit=False,
        signing_secret_included=False), indent=2))


if __name__ == '__main__':
    main()

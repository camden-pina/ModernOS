#!/usr/bin/env python3
"""Assemble a fresh Chromium/Code OSS desktop from explicit source-build outputs.

No ISO is read as an input. Source-built native programs, the admitted runtime,
GUI libraries, application and addon receipts supply the complete initramfs.
"""
import argparse
import base64
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import posixpath
import re
import subprocess
import sys
import tarfile
import tempfile

from build_cursor_theme import build as build_cursors
from debian_package import safe_relative
from electron_admission import prepare as prepare_admission
from source_tree import digest

ROOT = Path(__file__).resolve().parents[1]
EPOCH = 1793923200


def receipt(path):
    path = Path(path).resolve(strict=True)
    record = json.loads(path.read_text())
    for name, expected in record.get('source_sha256', {}).items():
        if digest(ROOT / name) != expected:
            raise ValueError('Build recipe changed; rebuild this component: ' + name)
    return record


class Payload:
    def __init__(self):
        self.entries = {}

    def add(self, name, data=b'', mode=0o644, kind=tarfile.REGTYPE, link='', replace=False):
        name = safe_relative(name)
        if kind == tarfile.SYMTYPE:
            target = posixpath.normpath(posixpath.join(posixpath.dirname(name), link))
            if not link or link.startswith('/') or target == '..' or target.startswith('../'):
                raise ValueError('Unsafe initramfs link: ' + name)
        elif kind not in (tarfile.REGTYPE, tarfile.DIRTYPE):
            raise ValueError('Unsupported initramfs member: ' + name)
        entry = dict(data=data, mode=mode, kind=kind, link=link)
        previous = self.entries.get(name)
        if previous and previous != entry and not replace:
            raise ValueError('Conflicting initramfs input: ' + name)
        self.entries[name] = entry

    def file(self, name, asset, mode=None, replace=False):
        path = Path(asset['path'])
        data = path.read_bytes()
        if hashlib.sha256(data).hexdigest() != asset['sha256']:
            raise ValueError('Build output changed: ' + name)
        self.add(name, data, mode if mode is not None else asset.get('mode', 0o644), replace=replace)

    def write(self, path):
        for name in list(self.entries):
            for parent in PurePosixPath(name).parents:
                if str(parent) == '.':
                    continue
                entry = self.entries.get(str(parent))
                if entry and entry['kind'] != tarfile.DIRTYPE:
                    raise ValueError('An initramfs file is used as a directory: ' + str(parent))
                if not entry:
                    self.add(str(parent), mode=0o755, kind=tarfile.DIRTYPE)
        for name, entry in self.entries.items():
            if entry['kind'] != tarfile.SYMTYPE:
                continue
            target = name
            visited = set()
            while self.entries.get(target, {}).get('kind') == tarfile.SYMTYPE:
                if target in visited:
                    raise ValueError('Cyclic initramfs link: ' + name)
                visited.add(target)
                target = posixpath.normpath(posixpath.join(posixpath.dirname(target), self.entries[target]['link']))
            if target not in self.entries:
                raise ValueError('Missing initramfs link target: ' + name)
        with tarfile.open(path, 'w', format=tarfile.USTAR_FORMAT) as archive:
            for name, entry in sorted(self.entries.items(), key=lambda p: (p[0].count('/'), p[0])):
                info = tarfile.TarInfo(name)
                info.type, info.mode, info.linkname = entry['kind'], entry['mode'], entry['link']
                info.mtime = EPOCH
                info.size = len(entry['data'])
                archive.addfile(info, io.BytesIO(entry['data']) if info.isfile() else None)

    def hashes(self):
        return {n: hashlib.sha256(e['data']).hexdigest() for n, e in self.entries.items() if e['kind'] == tarfile.REGTYPE}


def application_payload(payload, application, addons, diagnostics=False, microsoft_auth=False):
    products = application['files']
    module_prefix = 'usr/share/codeoss-workbench/'
    prebuilt, excluded = [], []
    for name, asset in sorted(products.items()):
        safe_relative(name)
        if not microsoft_auth and name in (
                'extensions/microsoft-authentication/dist/libmsalruntime.so',
                'extensions/microsoft-authentication/dist/msal-node-runtime.node'):
            excluded.append(name)
            continue
        # Intermediate object copies and Windows-only binaries do not belong
        # in an x86-64 Coast runtime.
        if '/obj.target/' in name or name.endswith(('.dll', '.exe')) or 'win32-app-container-tokens.win32-' in name:
            excluded.append(name)
            continue
        data = Path(asset['path']).read_bytes()
        if hashlib.sha256(data).hexdigest() != asset['sha256']:
            raise ValueError('Packaged application changed: ' + name)
        if name in addons['files']:
            replacement = addons['files'][name]
            data = Path(replacement['path']).read_bytes()
            if hashlib.sha256(data).hexdigest() != replacement['sha256']:
                raise ValueError('Source-built native module changed: ' + name)
        elif data.startswith(b'\x7fELF'):
            prebuilt.append(name)
        if name == 'product.json':
            continue
        if name in ('out/main.js', 'out/bootstrap-fork.js'):
            data = b"import '/usr/share/codeoss-workbench/out/coast-extension-reads.mjs';\n" + data
        if name == 'out/main.js':
            data = (ROOT / 'userland/electron-wayland/workbench-entry.mjs').read_bytes() + b'\n' + data
            if diagnostics:
                data = b"if(process.env.COAST_CODEOSS_QA==='1')await import('/usr/share/codeoss-workbench/out/coast-diagnostics.mjs');\n" + data
        if name == 'out/vs/workbench/workbench.desktop.main.css':
            data += b'\n' + (ROOT / 'userland/electron-wayland/workbench-native.css').read_bytes()
        if name == 'out/mainImpl.js':
            marker = b'backgroundThrottling:!1}),c}function Ag'
            if data.count(marker) != 1:
                raise ValueError('Review the pinned workbench window-options factory')
            data = data.replace(marker, b'backgroundThrottling:!1}),c.frame===!1&&(c.hasShadow=!1,c.roundedCorners=!1),c}function Ag')
        payload.add(module_prefix + name, data, 0o755 if data.startswith(b'\x7fELF') else 0o644)
    for source, target in [('bounded-extension-reads.mjs', 'coast-extension-reads.mjs'),
                           *([('workbench-diagnostics.mjs', 'coast-diagnostics.mjs')] if diagnostics else [])]:
        payload.add(module_prefix + 'out/' + target, (ROOT / 'userland/electron-wayland' / source).read_bytes())
    product = json.loads(Path(products['product.json']['path']).read_text())
    for name in product['checksums']:
        data = payload.entries[module_prefix + 'out/' + name]['data']
        product['checksums'][name] = base64.b64encode(hashlib.sha256(data).digest()).decode().rstrip('=')
    payload.add(module_prefix + 'product.json', (json.dumps(product, indent=2) + '\n').encode())
    return prebuilt, excluded


def validate_elf_payload(payload, gui, output):
    """Check actual image bytes, including prebuilt NPM helpers, against its ABI."""
    version_pattern = r'\b(?:GLIBC|GLIBCXX|CXXABI|GCC)_[A-Za-z0-9_.]+'
    libraries = {PurePosixPath(name).name for name in payload.entries if '.so' in PurePosixPath(name).name}
    cache, versions, checks = {}, set(), {}
    with tempfile.TemporaryDirectory(prefix='elf-check-', dir=output) as temporary:
        scratch = Path(temporary) / 'input.elf'

        def inspect(data):
            checksum = hashlib.sha256(data).hexdigest()
            if checksum not in cache:
                scratch.write_bytes(data)
                cache[checksum] = subprocess.check_output(
                    ['readelf', '-h', '-lW', '-d', '--version-info', scratch], text=True)
            return cache[checksum]

        for name, asset in gui['files'].items():
            if asset.get('component') in ('source:glibc', 'source:libgcc', 'source:cxx'):
                data = payload.entries[name]['data']
                if data.startswith(b'\x7fELF'):
                    versions.update(re.findall(version_pattern, inspect(data)))
        if not versions:
            raise ValueError('The image lacks its source-built GNU ABI version definitions')
        for name, entry in payload.entries.items():
            if entry['kind'] != tarfile.REGTYPE or not entry['data'].startswith(b'\x7fELF'):
                continue
            elf = inspect(entry['data'])
            needed = set(re.findall(r'\(NEEDED\).*?\[([^\]]+)\]', elf))
            required = set(re.findall(version_pattern, elf))
            if 'Advanced Micro Devices X86-64' not in elf or ' RWE ' in elf:
                raise ValueError('Unexpected image ELF architecture or writable executable segment: ' + name)
            if missing := (needed - libraries) | (required - versions):
                raise ValueError('Image ELF requires unavailable libraries/ABI: ' + name + ': ' + ', '.join(sorted(missing)))
            checks[name] = dict(needed=sorted(needed), abi_versions=sorted(required))
    return checks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('native-build', 'kernel-build', 'runtime-build', 'gui-runtime', 'application-build', 'addons-build', 'rust-build', 'bridge-build', 'assets-build', 'cli-build', 'compiler-build', 'packages-build'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--output-root', type=Path, required=True)
    for name in ('program-build', 'makepkg-build', 'frontend-build', 'qt-build', 'qemu-build', 'viewer-build'):
        parser.add_argument('--' + name, type=Path, help='explicit wider source integration receipt (supply all six)')
    parser.add_argument('--diagnostics', action='store_true')
    parser.add_argument('--with-microsoft-auth', action='store_true',
                        help='include the optional upstream Microsoft sign-in binaries; these cannot be built from public sources')
    parser.add_argument('--allow-bootstrap-gui', action='store_true', help='development qualification only; record binary GUI inputs explicitly')
    args = parser.parse_args()
    wider = ('program_build', 'makepkg_build', 'frontend_build', 'qt_build', 'qemu_build', 'viewer_build')
    if any(getattr(args, name) for name in wider) and not all(getattr(args, name) for name in wider):
        parser.error('Supply all six wider integration receipts together')
    paths = {name: getattr(args, name).resolve(strict=True) for name in
             ('runtime_build', 'gui_runtime', 'addons_build', 'rust_build', 'bridge_build', 'assets_build', 'cli_build', 'compiler_build', 'packages_build')}
    paths.update(native_build=args.native_build.resolve(strict=True) / 'BUILD.json',
        kernel_build=args.kernel_build.resolve(strict=True) / 'KERNEL.json',
        application_build=args.application_build.resolve(strict=True) / 'PACKAGED.json')
    if args.program_build:
        paths.update({name: getattr(args, name).resolve(strict=True) for name in wider})
    records = {name: receipt(path) for name, path in paths.items()}
    before = {name: digest(path) for name, path in paths.items()}
    native, kernel, runtime, gui, application, addons, rust, bridge, assets, cli, compiler = (records[name] for name in
        ('native_build', 'kernel_build', 'runtime_build', 'gui_runtime', 'application_build', 'addons_build', 'rust_build', 'bridge_build', 'assets_build', 'cli_build', 'compiler_build'))
    packages = records['packages_build']
    if args.program_build:
        for name in ('program_build', 'makepkg_build', 'frontend_build', 'qt_build', 'qemu_build'):
            if records[name].get('compiler_build_sha256') != before['compiler_build']:
                parser.error('Wider integration uses a different native compiler: ' + name)
        for name in ('makepkg_build', 'frontend_build', 'viewer_build'):
            if records[name].get('program_build_sha256') != before['program_build']:
                parser.error('Wider integration uses a different Python runtime: ' + name)
        if records['makepkg_build'].get('packages_build_sha256') != before['packages_build']:
            parser.error('makepkg and pacman must use the same source package tools')
        if records['viewer_build'].get('qemu_build_sha256') != before['qemu_build']:
            parser.error('The viewer must use this source QEMU build')
        if records['qemu_build'].get('backend') != 'TCI':
            parser.error('Coast QEMU requires its non-JIT TCI backend')
    if not gui.get('gui_libraries_compiled_from_source') and not args.allow_bootstrap_gui:
        parser.error('Source-built GUI libraries are required; complete tools/build_gui_sources.py first')
    if native.get('inherited_iso') is not False:
        parser.error('The native base must have been built from source without an inherited ISO')
    if not kernel.get('source_sha256'):
        parser.error('Rebuild the desktop kernel with its complete source identity')
    if cli.get('native_build_sha256') != before['native_build']:
        parser.error('The terminal must use this source-built native base')
    if packages.get('native_build_sha256') != before['native_build']:
        parser.error('The package tools must use this source-built native base')
    if (compiler.get('thread_model') != 'posix' or compiler.get('musl_libc_sha256') !=
            digest(args.native_build.resolve() / 'musl/lib/libc.a')):
        parser.error('Build the native C/C++ compiler with this musl base and POSIX threads')
    if kernel.get('runtime_build_sha256') != before['runtime_build']:
        parser.error('Rebuild the desktop kernel for this exact Electron/Node source build')
    if native.get('kernel_sha256') != kernel['kernel_sha256']:
        parser.error('The native base must use this desktop kernel')
    if addons.get('gui_runtime_sha256') != before['gui_runtime']:
        parser.error('Rebuild the native modules with this GUI runtime SDK')
    if bridge.get('application_sdk_sha256') != addons['sdk_receipt_sha256']:
        parser.error('The bridge and native modules must use the same application SDK')
    # This module links only the GNU ABI, not GTK/NSS/X11. Match those exact
    # source-built components even when its SDK also had development GUI inputs.
    if rust.get('gnu_component_build_sha256') != gui['component_build_sha256']:
        parser.error('Rebuild the Rust resolver with the matching source-built GNU ABI')
    compilation = args.application_build.resolve() / 'BUILD.json'
    if application['compilation_receipt_sha256'] != digest(compilation):
        raise ValueError('The application compilation receipt changed')
    receipt(compilation)
    if addons['application_source_receipt_sha256'] != digest(args.application_build / 'SOURCE.json'):
        raise ValueError('Native modules were built for different application sources')
    if rust['application_source_receipt_sha256'] != digest(args.application_build / 'SOURCE.json'):
        raise ValueError('The Rust resolver was built for different application sources')
    kernel_path = args.kernel_build.resolve() / 'vmlinux'
    if digest(kernel_path) != kernel['kernel_sha256']:
        raise ValueError('Desktop kernel changed')
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=False)
    prepare_admission(paths['runtime_build'], out / 'electron-admission.h')
    if digest(out / 'electron-admission.h') != json.loads((args.kernel_build / 'electron-admission.json').read_text())['header_sha256']:
        raise ValueError('The desktop kernel admits a different runtime pair')
    payload = Payload()
    initramfs = Path(native['initramfs'])
    if digest(initramfs) != native['initramfs_sha256']:
        raise ValueError('Native source-built initramfs changed')
    with tarfile.open(initramfs) as archive:
        for entry in archive:
            payload.add(entry.name, archive.extractfile(entry).read() if entry.isfile() else b'',
                        entry.mode, entry.type, entry.linkname)
    for name, asset in cli['files'].items():
        payload.file(name, asset)
    for name, target in cli['aliases'].items():
        payload.add(name, mode=0o777, kind=tarfile.SYMTYPE, link=target)
    payload.add('usr/bin/linux-bash', mode=0o777, kind=tarfile.SYMTYPE, link='../../bin/bash')
    for name, asset in compiler['files'].items():
        payload.file(name, asset)
    for name, asset in packages['files'].items():
        payload.file(name, asset)
    for name, target in packages['aliases'].items():
        payload.add(name, mode=0o777, kind=tarfile.SYMTYPE, link=target)
    for name in packages['directories']:
        payload.add(name, mode=0o755, kind=tarfile.DIRTYPE)
    payload.add('var/lib/pacman/local/ALPM_DB_VERSION', b'9\n')
    # libc-based tools need the same UID names as Coast's native accounts.
    # Password authentication remains the kernel account system's responsibility.
    count = int(re.search(r'#define OS_ACCOUNT_COUNT (\d+)u',
        (ROOT / 'include/uapi/account.h').read_text())[1])
    passwd = ['root:!:0:0:root:/root:/bin/sh',
              'alpm:!:990:990:Package downloader:/:/bin/false',
              'user:!:1000:1000:Desktop user:/home/user:/bin/sh']
    groups = ['root:!:0:', 'alpm:!:990:', 'user:!:1000:']
    for index in range(1, count):
        passwd.append(f'profile-{index}:!:{1000+index}:{1000+index}:Profile {index}:/home/profile-{index}:/bin/sh')
        groups.append(f'profile-{index}:!:{1000+index}:')
    payload.add('etc/passwd', ('\n'.join(passwd) + '\n').encode())
    payload.add('etc/group', ('\n'.join(groups) + '\n').encode())
    for name, asset in gui['files'].items():
        payload.file(name, asset)
    for name, target in gui['aliases'].items():
        payload.add(name, mode=0o777, kind=tarfile.SYMTYPE, link=target)
    for name, asset in assets['files'].items():
        payload.file(name, asset)
    for name, asset in runtime['products'].items():
        payload.file('opt/electron-43.7.3/' + name, asset, 0o755)
    for name, asset in runtime['sidecars'].items():
        payload.file('opt/electron-43.7.3/' + safe_relative(name), asset)
    required = {'resources/default_app.asar', 'locales/en-US.pak'}
    if required - runtime['sidecars'].keys():
        raise ValueError('The runtime receipt lacks its default application loader or locale; package_electron_runtime.py records complete old source builds')
    for name, product in bridge['products'].items():
        payload.file('bin/' + name, product, 0o755)
    payload.file('bin/codeoss-wayland', bridge['products']['chromium-wayland'], 0o755)
    launcher = args.native_build.resolve() / 'programs/code-launch'
    launcher_asset = dict(path=str(launcher), sha256=native['programs']['code-launch'])
    for name in ('bin/code', 'usr/bin/code'):
        payload.file(name, launcher_asset, 0o4755)
    if set(addons['files']) & set(rust['files']):
        raise ValueError('Conflicting native-module build outputs')
    prebuilt, excluded = application_payload(payload, application,
        dict(files={**addons['files'], **rust['files']}), args.diagnostics, args.with_microsoft_auth)
    for name, asset in rust['licenses'].items():
        payload.file(name, asset)
    for path in sorted((ROOT / 'userland/chromium-browser').iterdir()):
        if path.is_file() and path.suffix not in ('.c',):
            payload.add('usr/share/coast-browser/' + path.name, path.read_bytes())
    for name in ('hosts', 'nsswitch.conf'):
        payload.add('etc/' + name, (ROOT / 'userland/chromium-browser/network' / name).read_bytes())
    payload.add('etc/ssl/certs/ca-certificates.crt', (ROOT / 'userland/vendor/mozilla-ca-bundle.pem').read_bytes())
    payload.add('etc/fonts/fonts.conf', (ROOT / 'userland/electron-wayland/fonts.conf').read_bytes())
    payload.add('usr/share/coast/desktop-defaults.ini', b'scale=3\nterminal_shell=1\n')
    for name, data in build_cursors(out / 'cursors').items():
        payload.add(name, data)
    node = json.loads(paths['runtime_build'].with_name('NODE.json').read_text())
    license_path = Path(node['license']['path'])
    if digest(license_path) != node['license']['sha256']:
        raise ValueError('Electron Node license changed')
    payload.add('usr/share/licenses/coast-electron-node/LICENSE', license_path.read_bytes())
    for name, path in [('Electron', ROOT / 'userland/electron-runtime/LICENSE'),
                       ('Chromium', ROOT / 'userland/electron-runtime/Chromium-LICENSE'),
                       ('V8', ROOT / 'userland/electron-runtime/V8-LICENSE')]:
        payload.add('usr/share/licenses/coast-electron-source/' + name + '-LICENSE', path.read_bytes())
    package_database = None
    if args.program_build:
        replacements = {'etc/pacman.conf', 'etc/makepkg.conf', 'usr/bin/bsdtar',
                        'usr/bin/bsdcpio', 'usr/bin/bsdcat', 'usr/bin/bsdunzip'}
        for component in wider:
            record = records[component]
            if record.get('target_host_executed') is not False:
                raise ValueError('A wider integration ran target programs on Linux: ' + component)
            for name, asset in record['files'].items():
                mode = asset.get('mode', 0o755 if name.startswith('usr/bin/') else 0o644)
                payload.file(name, asset, mode=mode, replace=name in replacements)
            for name, target in record.get('aliases', {}).items():
                payload.add(name, mode=0o777, kind=tarfile.SYMTYPE, link=target, replace=name in replacements)
            for name in record.get('directories', []):
                payload.add(name, mode=0o755, kind=tarfile.DIRTYPE)
        from desktop_package_database import create
        package_database = create(payload, records)
    elf_checks = validate_elf_payload(payload, gui, out)
    archive = out / 'initramfs.tar'
    payload.write(archive)
    image = out / 'Coast.iso'
    subprocess.run(list(map(str, [sys.executable, '-B', ROOT / 'tools/build_boot_iso.py',
        '--bootloader', 'limine', '--config', ROOT / 'arch/x86/boot/boot.conf',
        '--kernel', kernel_path, '--initramfs', archive, '--source-root', ROOT,
        '--root', out / 'iso-root', '--output', image,
        '--limine-dir', args.native_build.resolve() / 'limine/output/bin'])), check=True)
    if any(digest(paths[name]) != sha for name, sha in before.items()) or digest(kernel_path) != kernel['kernel_sha256']:
        raise ValueError('A desktop assembly input changed')
    source_inputs = [Path(__file__), ROOT / 'userland/electron-wayland/workbench-entry.mjs',
        ROOT / 'include/uapi/account.h',
        ROOT / 'userland/electron-wayland/workbench-native.css',
        ROOT / 'userland/electron-wayland/bounded-extension-reads.mjs',
        ROOT / 'userland/electron-wayland/fonts.conf', ROOT / 'tools/build_cursor_theme.py',
        ROOT / 'userland/vendor/mozilla-ca-bundle.pem',
        *[p for p in (ROOT / 'userland/chromium-browser').rglob('*') if p.is_file()]]
    if args.diagnostics:
        source_inputs.append(ROOT / 'userland/electron-wayland/workbench-diagnostics.mjs')
    if args.program_build:
        source_inputs += [ROOT / 'tools/desktop_package_database.py', ROOT / 'tools/build_extra_programs.py',
                          ROOT / 'userland/vendor/extra-program-sources.json', ROOT / 'userland/vendor/linux-ports-sources.json']
    record = dict(state='fresh desktop assembled; Coast qualification pending',
        iso=str(image), iso_sha256=digest(image), inherited_iso=False, kernel_sha256=kernel['kernel_sha256'],
        components={name: dict(path=str(paths[name]), sha256=sha) for name, sha in before.items()},
        runtime_build_sha256=before['runtime_build'], addons_build_sha256=before['addons_build'],
        gui_runtime_sha256=before['gui_runtime'], gui_libraries_compiled_from_source=bool(gui.get('gui_libraries_compiled_from_source')),
        inherited_application_javascript=False, prebuilt_application_inputs=prebuilt, excluded_application_files=excluded,
        optional_microsoft_auth_binaries=args.with_microsoft_auth,
        wider_integrations=bool(args.program_build), package_database=package_database,
        files=payload.hashes(), elf_checks=elf_checks, diagnostics_qa_only=args.diagnostics, target_execution_on_linux=False,
        source_sha256={str(p.relative_to(ROOT)): digest(p) for p in source_inputs})
    (out / 'MANIFEST.json').write_text(json.dumps(record, indent=2) + '\n')
    print('Fresh desktop image:', image, flush=True)


if __name__ == '__main__':
    main()

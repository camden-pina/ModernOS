#!/usr/bin/env python3
"""Build the Coast Chromium browser and overlay a preserved Electron desktop ISO.

The existing pinned Coast Electron runtime supplies Chromium. Only native
kernel/desktop/bridge sources are compiled here; target programs never run on
the host. A fresh output directory keeps prior images and evidence intact.
"""
import argparse
import fcntl
import hashlib
import io
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
from retired_browser import clean_modules

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--base-iso', type=Path, default=ROOT / 'build/Coast-desktop-base.iso')
    p.add_argument('--output-root', type=Path, required=True)
    p.add_argument('--bridge-build', type=Path, default=ROOT / 'build/wayland-bridge/BUILD.json')
    p.add_argument('--verify-sandbox', action='store_true', help='enable kernel filesystem sandbox selftests in this QA image')
    args = p.parse_args()
    base, out = args.base_iso.resolve(), args.output_root.resolve()
    if not base.is_file() or out.exists():
        p.error('An existing Electron desktop ISO and unused output directory are required')
    out.mkdir(parents=True)
    source_paths = [ROOT / 'Makefile', ROOT / 'tools/build_chromium_browser.py', ROOT / 'tools/build_native_gui.py', ROOT / 'ports/chromium/UPSTREAM.json']
    for directory in ['arch', 'drivers', 'fs', 'include', 'init', 'kernel', 'lib', 'mm', 'net', 'scripts',
                      'userland/desktop', 'userland/include', 'userland/electron-wayland', 'userland/chromium-browser', 'ports/chromium']:
        source_paths.extend(file for file in (ROOT / directory).rglob('*') if file.is_file())
    source_paths.extend((ROOT / 'userland').glob('*.c'))
    source_paths.extend((ROOT / 'userland').glob('*.S'))
    source_paths.extend((ROOT / 'userland').glob('*.ld'))
    source_hashes = {str(file.relative_to(ROOT)): sha(file) for file in sorted(set(source_paths))}
    with Path(str(base) + '.lock').open('a') as image_lock:
        fcntl.flock(image_lock, fcntl.LOCK_SH | fcntl.LOCK_NB)
        before = sha(base)
        dependency = json.loads((ROOT / 'ports/chromium/UPSTREAM.json').read_text())['additional_runtime_dependencies'][0]
        cache = ROOT / 'build/chromium-browser-vendor'; cache.mkdir(parents=True, exist_ok=True)
        package = cache / dependency['url'].rsplit('/', 1)[1]
        if not package.is_file():
            temporary = out / 'sqlite.deb.part'
            subprocess.run(['curl', '-fL', '--retry', '2', '--connect-timeout', '15', '--max-time', '120',
                            '-o', str(temporary), dependency['url']], check=True)
            if sha(temporary) != dependency['sha256']: raise RuntimeError('SQLite package checksum mismatch')
            temporary.replace(package)
        if sha(package) != dependency['sha256']: raise RuntimeError('Cached SQLite package checksum mismatch')
        dictionary_asset = json.loads((ROOT / 'ports/chromium/UPSTREAM.json').read_text())['bundled_assets'][0]
        dictionary = cache / dictionary_asset['url'].rsplit('/', 1)[1]
        if not dictionary.is_file():
            temporary = out / 'dictionary.bdic.part'
            subprocess.run(['curl', '-fL', '--retry', '2', '--connect-timeout', '15', '--max-time', '120',
                            '-o', str(temporary), dictionary_asset['url']], check=True)
            if sha(temporary) != dictionary_asset['sha256']: raise RuntimeError('Dictionary checksum mismatch')
            temporary.replace(dictionary)
        if sha(dictionary) != dictionary_asset['sha256'] or dictionary.stat().st_size != dictionary_asset['bytes']:
            raise RuntimeError('Cached dictionary mismatch')
        members = subprocess.check_output(['ar', 't', str(package)], text=True).splitlines()
        data_member = next(name for name in members if name.startswith('data.tar.'))
        data = subprocess.check_output(['ar', 'p', str(package), data_member])
        with tarfile.open(fileobj=io.BytesIO(data), mode='r:*') as archive:
            for name in ['usr/lib/x86_64-linux-gnu/libsqlite3.so.0.8.6', 'usr/share/doc/libsqlite3-0/copyright']:
                member = archive.getmember('./' + name)
                if not member.isfile() or member.size > 16 * 1024 * 1024: raise RuntimeError('Invalid SQLite package member')
                destination = out / 'sqlite' / name; destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(archive.extractfile(member).read())
        sqlite = out / 'sqlite/usr/lib/x86_64-linux-gnu/libsqlite3.so.0.8.6'
        versions = subprocess.check_output(['readelf', '--version-info', str(sqlite)], text=True)
        if max(tuple(map(int, value.split('.'))) for value in re.findall(r'GLIBC_([0-9.]+)', versions)) > (2, 36):
            raise RuntimeError('SQLite requires a newer glibc than the port')
        flags = json.loads((ROOT / 'ports/chromium/kernel-flags.json').read_text())
        if args.verify_sandbox:
            for name in ['USERNS_VERIFY', 'PID_NAMESPACE_VERIFY', 'FILESYSTEM_SANDBOX_VERIFY']:
                flags[name] = True
        subprocess.run(['make', '-C', str(ROOT), 'O=' + str(out / 'kernel'), '-j8',
                        *[f'{name}={int(value)}' for name, value in flags.items()], 'vmlinux'], check=True)
        subprocess.run([sys.executable, '-B', str(ROOT / 'tools/build_native_gui.py'),
                        '--output-root', str(out / 'desktop')], check=True)
        if not args.bridge_build.is_file():
            subprocess.run([sys.executable, '-B', str(ROOT / 'tools/build_wayland_bridge.py'),
                            '--output-root', str(args.bridge_build.resolve().parent)], check=True)
        bridge_record = json.loads(args.bridge_build.read_text())
        if bridge_record['target_host_executed'] is not False:
            raise ValueError('A source-built Coast display bridge is required')
        for name, digest in bridge_record['source_sha256'].items():
            if sha(ROOT / name) != digest:
                raise ValueError('Rebuild the display bridge after source changes: ' + name)
        for name in ['chromium-wayland', 'g2-wayland-launch', 'seccomp-check']:
            asset = bridge_record['products'][name]
            if sha(asset['path']) != asset['sha256']:
                raise ValueError('Display bridge build output changed: ' + name)
            shutil.copyfile(asset['path'], out / name)
        subprocess.run([str(ROOT / 'build/native-musl/bin/musl-gcc'), '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-static', '-no-pie', '-fno-pie', '-mno-avx', '-mno-avx2', '-idirafter', str(ROOT / 'include'),
                        str(ROOT / 'userland/chromium-browser/network-check.c'), '-o', str(out / 'network-check')], check=True)
        subprocess.run([str(ROOT / 'build/native-musl/bin/musl-gcc'), '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-static', '-no-pie', '-fno-pie', '-mno-avx', '-mno-avx2', '-idirafter', str(ROOT / 'include'),
                        str(ROOT / 'userland/network-check.c'), '-o', str(out / 'native-network-check')], check=True)
        files = {
            'bin/gui': out / 'desktop/gui',
            'bin/chromium-wayland': out / 'chromium-wayland',
            'bin/codeoss-wayland': out / 'chromium-wayland',
            'bin/g2-wayland-launch': out / 'g2-wayland-launch',
            'bin/chromium-network-check': out / 'network-check',
            'bin/network-check': out / 'native-network-check',
            'bin/seccomp-check': out / 'seccomp-check',
            'usr/lib/x86_64-linux-gnu/libsqlite3.so.0': sqlite,
            'usr/share/licenses/coast-browser-sqlite/copyright': out / 'sqlite/usr/share/doc/libsqlite3-0/copyright',
            'etc/hosts': ROOT / 'userland/chromium-browser/network/hosts',
            'etc/nsswitch.conf': ROOT / 'userland/chromium-browser/network/nsswitch.conf',
            'usr/share/coast-browser/dictionaries/en-US-10-1.bdic': dictionary,
        }
        files.update({'usr/share/licenses/coast-browser-dictionary/' + Path(name).name: ROOT / name for name in dictionary_asset['licenses']})
        files.update({'usr/share/coast-browser/' + file.name: file for file in sorted((ROOT / 'userland/chromium-browser').iterdir()) if file.is_file()})
        shutil.copyfile(args.bridge_build, out / 'CODEOSS-BRIDGE-BUILD.json')
        archive = out / 'coast-browser.tar'
        with tarfile.open(archive, 'w', format=tarfile.USTAR_FORMAT) as tar:
            directories = {str(parent) for name in files for parent in Path(name).parents if str(parent) != '.'}
            for name in sorted(directories, key=lambda value: (value.count('/'), value)):
                info = tarfile.TarInfo(name); info.type = tarfile.DIRTYPE
                info.mode = 0o755; info.uid = info.gid = 0; info.mtime = 1793923200
                tar.addfile(info)
            for name, file in files.items():
                data = file.read_bytes()
                info = tarfile.TarInfo(name); info.size = len(data)
                info.mode = 0o755 if name.startswith('bin/') else 0o644
                info.uid = info.gid = 0; info.mtime = 1793923200
                tar.addfile(info, io.BytesIO(data))
        listing = subprocess.check_output(['xorriso', '-no_rc', '-indev', str(base), '-find', '/boot/limine/limine.conf', '-exec', 'report_lba', '--'], stderr=subprocess.DEVNULL, text=True)
        found = re.search(r"File data lba:\s*0\s*,\s*(\d+)\s*,\s*\d+\s*,\s*(\d+)\s*,", listing)
        if not found: raise RuntimeError('The selected ISO has no Limine configuration')
        with base.open('rb') as stream:
            stream.seek(int(found[1]) * 2048); config = stream.read(int(found[2])).decode()
        if '/boot/coast-browser.tar' in config: raise RuntimeError('Use the preserved desktop image as the base')
        if config.count('\n/Reboot') != 1: raise RuntimeError('Unexpected boot configuration')
        cleaned_modules, removed = clean_modules(base, config, out / 'clean-modules')
        config = config.replace('\n/Reboot', '    module_path: boot():/boot/coast-browser.tar\n    module_string: initramfs\n\n/Reboot')
        (out / 'limine.conf').write_text(config)
        image = out / 'Coast-browser.iso'
        subprocess.run(['cp', '--reflink=auto', str(base), str(image)], check=True)
        with (out / 'image.log').open('w') as log:
            clean_arguments = [arg for name, path in cleaned_modules.items() for arg in ['-map', str(path), name]]
            subprocess.run(['xorriso', '-no_rc', '-dev', str(image),
                            *clean_arguments,
                            '-map', str(out / 'kernel/vmlinux'), '/boot/kernel.elf',
                            '-map', str(archive), '/boot/coast-browser.tar',
                            '-map', str(out / 'limine.conf'), '/boot/limine/limine.conf',
                            '-boot_image', 'any', 'keep', '-commit', '-end'], stdout=log, stderr=subprocess.STDOUT, check=True)
        if sha(base) != before: raise RuntimeError('Base ISO changed during packaging')
        if any(sha(ROOT / name) != digest for name, digest in source_hashes.items()):
            raise RuntimeError('Source changed during compilation/packaging')
        record = dict(state='built; guest verification pending', iso=str(image), iso_sha256=sha(image),
                      base_iso=str(base), base_iso_sha256=before, kernel_sha256=sha(out / 'kernel/vmlinux'),
                      kernel_flags=flags, files={name: dict(source=str(file), sha256=sha(file)) for name, file in files.items()},
                      source_sha256=source_hashes,
                      retired_browser_files=removed,
                      runtime_dependencies=[dependency], bundled_assets=[dictionary_asset],
                      target_host_executed=False, runtime='Coast-pinned Electron 43.7.3 / Chromium',
                      launch='Super+Space, Chromium Browser, Enter',
                      limits=['software rendering', 'jitless', 'four tabs', 'owner profile only', 'RAM files reset on reboot'])
        base_manifest = base.parent / 'MANIFEST.json'
        record['diagnostics_qa_only'] = bool(base_manifest.is_file() and json.loads(base_manifest.read_text()).get('diagnostics_qa_only'))
        (out / 'BUILD.json').write_text(json.dumps(record, indent=2) + '\n')
        (out / 'MANIFEST.json').write_text(json.dumps(record, indent=2) + '\n')
        print('Browser ISO ready:', image, flush=True)


if __name__ == '__main__':
    main()

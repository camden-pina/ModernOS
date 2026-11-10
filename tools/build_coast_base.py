#!/usr/bin/env python3
"""Migrate a preserved live image to Coast and rebuild its native kernel/tools.

Only host compilers run, in a network-isolated namespace. Original images and
toolchains remain read-only. Archive paths, links and text are migrated together;
executables are rebuilt from source rather than edited as raw bytes.
"""
from build_state import project_lock
from build_state import atomic_write
import argparse
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
OLD = 'Modern' + 'OS'
PATTERN = re.compile(re.escape(OLD), re.I)


def rename(text):
    return PATTERN.sub(lambda m: 'coast' if m[0].islower() else 'COAST' if m[0].isupper() else 'Coast', text)


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--base-iso', type=Path, required=True)
    p.add_argument('--output-root', type=Path, required=True)
    p.add_argument('--artifacts', type=Path, required=True)
    p.add_argument('--busybox', type=Path, required=True)
    p.add_argument('--browser-build',type=Path)
    p.add_argument('--qt-build',type=Path)
    p.add_argument('--gcc-build',type=Path)
    a = p.parse_args()
    base, out, artifacts = a.base_iso.resolve(), a.output_root.resolve(), a.artifacts.resolve()
    out.mkdir(parents=True, exist_ok=False)
    initial = sha(base)
    conf = ['bwrap', '--unshare-all', '--die-with-parent', '--new-session', '--ro-bind', '/', '/',
            '--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp', '--bind', str(out), str(out)]
    env = dict(PATH='/usr/bin:/bin', HOME='/tmp', TMPDIR='/tmp', LANG='C.UTF-8')
    commands = []

    def run(command, cwd=ROOT):
        command = list(map(str, command))
        commands.append(command)
        subprocess.run([*conf, '--chdir', str(cwd), '--', *command], env=env,
                       stdin=subprocess.DEVNULL, check=True)

    sources = {str(path.relative_to(ROOT)): sha(path)
               for directory in ['arch', 'drivers', 'fs', 'include', 'init', 'kernel', 'lib', 'mm', 'net', 'scripts']
               for path in (ROOT / directory).rglob('*') if path.is_file()}
    sources['Makefile'] = sha(ROOT / 'Makefile')
    flags = json.loads((artifacts / 'BUILD-on.json').read_text())['flags']
    kernel = out / 'kernel'
    run(['make', '-C', ROOT, f'O={kernel}', '-j8',
         *[f'{name}={int(value)}' for name, value in flags.items()], 'vmlinux'])
    # Desktop, account helpers and native interactive shell share the new ABI.
    subprocess.run(['/usr/bin/python3', '-B', str(ROOT / 'tools/build_native_gui.py'),
                    '--output-root', str(out / 'desktop'), '--artifacts', str(artifacts)],
                   stdin=subprocess.DEVNULL, check=True)
    user = ROOT / 'userland'
    compile_flags = ['-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-ffreestanding',
                     '-fno-builtin', '-fno-pie', '-fno-stack-protector', '-m64', '-mno-avx', '-mno-avx2',
                     '-I' + str(user / 'include'), '-I' + str(ROOT / 'include'),
                     '-I' + str(ROOT / 'arch/x86/include/asm')]
    native = out / 'native'
    native.mkdir()
    for name in ['applet', 'tools', 'sdk-tool', 'hello', 'check', 'spin', 'fault', 'exec-test',
                 'edit', 'env', 'stty', 'tty', 'reset', 'build-sh', 'browser-sandbox-check']:
        obj = native / (name + '.o')
        run(['/usr/bin/cc', *compile_flags, '-c', user / (name + '.c'), '-o', obj])
        run(['/usr/bin/ld', '-nostdlib', '-static', '-z', 'max-page-size=4096', '-z', 'noexecstack',
             '-T', user / 'user.ld', out / 'desktop/crt0.S.o', out / 'desktop/libc.c.o', obj,
             '-o', native / name])
    listing = subprocess.check_output(['xorriso', '-no_rc', '-indev', str(base), '-find', '/boot',
                                       '-type', 'f', '-exec', 'report_lba', '--']).decode()
    extents = {name: (int(lba) * 2048, int(size)) for lba, size, name in re.findall(
        r"File data lba:\s*0\s*,\s*(\d+)\s*,\s*\d+\s*,\s*(\d+)\s*,\s*'([^']+)'", listing)}

    def read_asset(name):
        offset, size = extents[name]
        with base.open('rb') as stream:
            stream.seek(offset)
            return stream.read(size)

    config = read_asset('/boot/limine/limine.conf').decode()
    label='Coast' if '\n/Coast\n' in config else OLD
    entry = config.split('\n/' + label + '\n', 1)[1].split('\n/', 1)[0]
    modules = re.findall(r'module_path:\s*boot\(\):([^\n]+)', entry)
    overrides = {}
    for path in (out / 'desktop').iterdir():
        if path.is_file() and path.name in {'gui','coast-sh','passwd','login','id','whoami','sudo','clip','profile-session','code-launch'}:
            overrides['bin/' + ('code' if path.name == 'code-launch' else path.name)] = path
    overrides['bin/busybox'] = a.busybox.resolve()
    for receipt,name in [(a.browser_build,'bin/web-renderer'),(a.qt_build,'usr/bin/qt6-demo')]:
        if receipt:
            record=json.loads(receipt.read_text())
            assert record['target_host_executed'] is False and sha(record['binary'])==record['binary_sha256']
            overrides[name]=Path(record['binary'])
    if a.gcc_build:
        record=json.loads(a.gcc_build.read_text())
        assert record['target_host_executed'] is False
        for name,asset in record['files'].items():
            assert sha(asset['path'])==asset['sha256']
            target='usr/libexec/gcc/x86_64-linux-musl/14.2.0/'+name if name in {'cc1','cc1plus'} else 'usr/bin/'+name
            overrides[target]=Path(asset['path'])
            if name in {'gcc','g++'}:overrides['usr/bin/'+('cc' if name=='gcc' else 'c++')]=Path(asset['path'])
    for name in ['hello','check','spin','fault','exec-test','edit','env','stty','tty','reset','build-sh','browser-sandbox-check']:
        overrides['bin/' + name] = native / name
    text_tools = 'head tail wc grep find hexdump'.split()
    sdk_tools = 'gcc g++ cc c++ cpp as ld ar ranlib nm objcopy objdump readelf size strings strip bootstrap-toolchain'.split()
    applets = re.search(r'^APPLETS := (.*)$', (user / 'Makefile').read_text(), re.M)[1].split()
    for names, binary in [(text_tools,'tools'),(sdk_tools,'sdk-tool'),(applets,'applet')]:
        for name in names:
            overrides['bin/' + name] = native / binary
    for name in ['grep','egrep','fgrep','head','tail','wc','find','hexdump','stat']:
        overrides['usr/bin/' + name] = native / 'applet'
    image = out / 'image'
    (image / 'boot/limine').mkdir(parents=True)
    changes = []
    for number, module in enumerate(modules):
        target = '/boot/coast-base-' + str(number) + '.tar'
        offset, size = extents[module]
        with base.open('rb') as stream, tarfile.open(image / target.lstrip('/'), 'w', format=tarfile.USTAR_FORMAT) as dest:
            stream.seek(offset)
            with tarfile.open(fileobj=stream, mode='r|') as archive:
                for member in archive:
                    original = member.name
                    member.pax_headers = {}
                    member.mtime = int(member.mtime)
                    member.name, member.linkname = rename(member.name), rename(member.linkname)
                    if PurePosixPath(member.name).is_absolute() or '..' in PurePosixPath(member.name).parts:
                        raise ValueError('Unsafe archive member')
                    if member.isfile():
                        name = member.name.removeprefix('./')
                        if name in overrides:
                            data = overrides[name].read_bytes()
                        else:
                            data = archive.extractfile(member).read()
                            if b'\0' not in data and PATTERN.search(data.decode('utf-8', errors='replace')):
                                data = rename(data.decode('utf-8')).encode()
                                changes.append(dict(module=module, name=name, action='text'))
                        member.size = len(data)
                        dest.addfile(member, io.BytesIO(data))
                    else:
                        dest.addfile(member)
                    if original != member.name:
                        changes.append(dict(module=module, name=member.name, action='path'))
        config = config.replace(module, target)
    extra = '/boot/coast-native.tar'
    with tarfile.open(image / extra.lstrip('/'), 'w', format=tarfile.USTAR_FORMAT) as archive:
        for name,path in overrides.items():
            data=path.read_bytes();member=tarfile.TarInfo(name)
            member.size=len(data);member.mode=0o4755 if name in {'bin/sudo','bin/code'} else 0o755
            archive.addfile(member,io.BytesIO(data))
    config=config.replace('\n/Reboot', '    module_path: boot():'+extra+'\n    module_string: initramfs\n\n/Reboot')
    config = rename(config)
    (image / 'boot/limine/limine.conf').write_text(config)
    for name in ['limine-bios-cd.bin', 'limine-bios.sys', 'limine-uefi-cd.bin']:
        (image / 'boot/limine' / name).write_bytes(read_asset('/boot/limine/' + name))
    (image / 'boot/boot-background.jpg').write_bytes(read_asset('/boot/boot-background.jpg'))
    shutil.copy2(kernel / 'vmlinux', image / 'boot/kernel.elf')
    for source, target in [('arch/x86/boot/boot.conf', 'boot/coast.conf')]:
        shutil.copy2(ROOT / source, image / target)
    # Preserve the native UEFI bootloader from the original ISO.
    subprocess.run(['xorriso', '-no_rc', '-osirrox', 'on', '-indev', str(base),
                    '-extract', '/EFI', str(image / 'EFI')], check=True)
    iso = out / 'Coast-base.iso'
    subprocess.run(['xorriso', '-as', 'mkisofs', '-R', '-J', '-V', 'COAST_ISO', '-b', 'boot/limine/limine-bios-cd.bin',
                    '-no-emul-boot', '-boot-load-size', '4', '-boot-info-table', '--efi-boot', 'boot/limine/limine-uefi-cd.bin',
                    '-efi-boot-part', '--efi-boot-image', '--protective-msdos-label', '-o', str(iso), str(image)], check=True)
    assert sha(base) == initial, 'Original image changed'
    assert all(sha(ROOT / name) == digest for name, digest in sources.items()), 'Kernel source changed during build'
    record = dict(iso=str(iso), iso_sha256=sha(iso), kernel_sha256=sha(kernel/'vmlinux'), source_sha256=sources,
                  base=dict(path=str(base), sha256=initial), changes=changes, commands=commands,
                  confinement=conf, target_host_executed=False,
                  native_binaries={name:dict(path=str(path),sha256=sha(path)) for name,path in overrides.items()})
    atomic_write(out / 'BUILD.json', json.dumps(record, indent=2) + '\n')
    print('Coast base ready:', iso, flush=True)


if __name__ == '__main__':
    with project_lock():
        main()

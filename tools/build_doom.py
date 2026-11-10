#!/usr/bin/env python3
"""Build pinned Doomgeneric with the native Coast display/input/PCM backend.

Only this port's cache is rebuilt, with one compiler process at a time. Sources
are restored from official per-file HTTPS URLs and checked against both the Git
blob IDs and SHA-256 manifest before a deterministic source archive is created.
"""
from source_archive import unpack
from build_state import project_lock
import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / 'userland/doom'
VENDOR = ROOT / 'userland/vendor'
BUILD = ROOT / 'build/doom'
MANIFEST = PORT / 'sources.json'
MUSL = ROOT / 'build/native-musl/bin/musl-gcc'
FLAGS = ['-O2', '-std=gnu11', '-fno-pie', '-fno-stack-protector', '-mno-avx',
         '-mno-avx2', '-fno-strict-aliasing', '-fwrapv', '-ffunction-sections',
         '-fdata-sections', '-DNORMALUNIX', '-DLINUX', '-D_DEFAULT_SOURCE',
         '-DDOOMGENERIC_RESX=320', '-DDOOMGENERIC_RESY=200']


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def check_file(data, record):
    if (len(data) != record['size'] or hashlib.sha256(data).hexdigest() != record['sha256']
            or hashlib.sha1(f'blob {len(data)}\0'.encode() + data).hexdigest() != record['sha']):
        raise ValueError('Doomgeneric source mismatch: ' + record['path'])


def write_archive(path, source, manifest):
    with tarfile.open(path, 'w:xz', format=tarfile.USTAR_FORMAT) as archive:
        for record in manifest['files']:
            data = (source / record['path']).read_bytes()
            check_file(data, record)
            entry = tarfile.TarInfo(manifest['directory'] + '/' + record['path'])
            entry.mode = int(record['mode'], 8) & 0o777
            entry.size = len(data)
            archive.addfile(entry, io.BytesIO(data))


def source_archive(check_only=False):
    manifest = json.loads(MANIFEST.read_text())
    archive = VENDOR / manifest['archive']
    if archive.exists():
        if digest(archive) != manifest['sha256']:
            raise ValueError(f'Doomgeneric archive checksum mismatch; preserve/remove {archive}')
        return archive, manifest
    if check_only:
        raise FileNotFoundError(f'Missing Doomgeneric sources: {archive}; run make doom-sources')
    # Every byte is verified before the locally reconstructed archive is installed.
    from fetch_sources import download
    VENDOR.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='doom-source-', dir=VENDOR) as temporary:
        source = Path(temporary) / 'tree'
        for record in manifest['files']:
            relative = Path(record['path'])
            if relative.is_absolute() or '..' in relative.parts:
                raise ValueError('Invalid Doomgeneric source path')
            target = source / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            url = (f'https://raw.githubusercontent.com/ozkl/doomgeneric/'
                   f'{manifest["commit"]}/{record["path"]}')
            download(url, target)
            check_file(target.read_bytes(), record)
        packed = Path(temporary) / archive.name
        write_archive(packed, source, manifest)
        if digest(packed) != manifest['sha256']:
            raise ValueError('Reconstructed Doomgeneric archive checksum mismatch')
        packed.replace(archive)
    return archive, manifest


def replace_once(path, original, replacement):
    contents = path.read_text()
    if contents.count(original) != 1:
        raise ValueError(f'Expected exactly one upstream patch location in {path.name}')
    path.write_text(contents.replace(original, replacement))


def copy_changed(source, target, executable=False):
    target.parent.mkdir(parents=True, exist_ok=True)
    if not target.exists() or digest(source) != digest(target):
        shutil.copyfile(source, target)
    target.chmod(0o755 if executable else 0o644)


def engine(archive, manifest):
    if not MUSL.exists():
        raise FileNotFoundError('Build userland/bin/nano first for the pinned static musl toolchain')
    BUILD.mkdir(parents=True, exist_ok=True)
    key = hashlib.sha256(Path(__file__).read_bytes() + MANIFEST.read_bytes() +
                         str(ROOT).encode() + (ROOT / 'include/uapi/window.h').read_bytes() + b''.join(p.read_bytes() for p in sorted(PORT.glob('*.[ch]')))).hexdigest()
    binary, stamp = BUILD / 'doom', BUILD / 'build.stamp'
    if not binary.exists() or not stamp.exists() or stamp.read_text() != key:
        source_root = BUILD / 'source'
        if source_root.exists():
            shutil.rmtree(source_root)
        source_root.mkdir()
        unpack(archive, source_root, filter='data')
        official = source_root / manifest['directory']
        for record in manifest['files']:
            check_file((official / record['path']).read_bytes(), record)
        source = official / 'doomgeneric'
        # Explicit adaptations remain in this owned, checked builder.
        replace_once(source / 'd_main.c', 'M_SetConfigDir(NULL);',
                     'M_SetConfigDir("/home/user/.doom");')
        replace_once(source / 'i_system.c',
                     '#if ORIGCODE\n    SDL_Quit();\n\n    exit(0);\n#endif\n}',
                     '#if ORIGCODE\n    SDL_Quit();\n#endif\n    exit(0);\n}')
        replace_once(source / 'i_input.c',
                     '            }\n            break;\n        }\n    }',
                     '            }\n        }\n    }')
        replace_once(source / 'i_input.c', 'void I_GetEvent(void)\n{',
                     'extern void Doom_InputBeginTic(void);\nvoid I_GetEvent(void)\n{\n    Doom_InputBeginTic();')
        replace_once(source / 'g_game.c', 'if (gamekeydown[key_up])',
                     'if (gamekeydown[key_up] || gamekeydown[KEY_UPARROW])')
        replace_once(source / 'g_game.c', 'if (gamekeydown[key_down])',
                     'if (gamekeydown[key_down] || gamekeydown[KEY_DOWNARROW])')
        replace_once(source / 'g_game.c', 'if (gamekeydown[key_use]\n',
                     "if (gamekeydown[key_use] || gamekeydown[' ']\n")
        # Fixed native output has a 320x200 backing buffer; main rejects options
        # that could change its layout. User response files are rejected too.
        makefile = (source / 'Makefile').read_text()
        names = re.search(r'^SRC_DOOM = (.+)$', makefile, re.M)[1].split()
        inputs = [source / (name[:-2] + '.c') for name in names
                  if name not in ('i_sound.o', 'doomgeneric_xlib.o')]
        inputs += [PORT / 'doomgeneric_coast.c', PORT / 'i_coast_sound.c', PORT / 'coast_midi.c']
        objects = BUILD / 'obj'
        objects.mkdir(exist_ok=True)
        products = []
        with (BUILD / 'build.log').open('w') as log:
            for item in inputs:
                output = objects / (item.stem + '.o')
                warnings = ['-Wall', '-Wextra', '-Werror'] if item.parent == PORT else ['-w']
                command = [str(MUSL), *FLAGS, *warnings, '-I' + str(source), '-I' + str(PORT),
                           '-c', str(item), '-o', str(output)]
                log.write(' '.join(command) + '\n')
                log.flush()
                subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
                products.append(output)
            subprocess.run([str(MUSL), '-static', '-no-pie', '-Wl,--gc-sections',
                            *map(str, products), '-lm', '-o', str(binary)],
                           stdout=log, stderr=subprocess.STDOUT, check=True)
        stamp.write_text(key)
    copy_changed(binary, ROOT / 'userland/bin/doom', True)
    license_target = VENDOR / 'licenses/doomgeneric-LICENSE.txt'
    with tarfile.open(archive) as source:
        license_data = source.extractfile(manifest['directory'] + '/LICENSE').read()
    license_target.parent.mkdir(parents=True, exist_ok=True)
    license_target.write_bytes(license_data)
    return binary


def package(archive, manifest):
    subprocess.run([sys.executable, str(ROOT / 'tools/build_freedoom.py')], check=True)
    data_root = ROOT / 'build/doom-freedoom'
    data = data_root / 'freedoom2.wad'
    if not data.exists():
        raise FileNotFoundError(f'Freedoom builder did not create {data}')
    destination = ROOT / 'userland/runtime/usr/share/games/doom'
    copy_changed(data, destination / data.name)
    # The data builder supplies original BSD notices/credits and exact provenance.
    notices = data_root / 'notices'
    if not notices.is_dir():
        raise FileNotFoundError(f'Freedoom notices missing: {notices}')
    for source in sorted(notices.iterdir()):
        if source.is_file():
            copy_changed(source, destination / source.name)
    for source in sorted(data_root.glob('*provenance*.json')):
        copy_changed(source, destination / source.name)
    corresponding = ROOT / 'userland/runtime/usr/src/coast/doom'
    copy_changed(archive, corresponding / archive.name)
    for source in sorted(VENDOR.glob('freedoom-*-source.tar.xz')) + sorted(VENDOR.glob('deutex-*-source.tar.xz')):
        copy_changed(source, corresponding / source.name)
    for source in sorted(VENDOR.glob('freedoom-*-source.json')) + sorted(VENDOR.glob('deutex-*-source.json')):
        copy_changed(source, corresponding / source.name)
    for source in sorted(PORT.rglob('*')):
        if source.is_file():
            copy_changed(source, corresponding / 'port' / source.relative_to(PORT))
    for name in ('build_doom.py', 'build_freedoom.py'):
        copy_changed(ROOT / 'tools' / name, corresponding / name)
    copy_changed(ROOT / 'tools/fetch_sources.py', corresponding / 'fetch_sources.py')
    copy_changed(ROOT / 'include/uapi/audio.h', corresponding / 'include/uapi/audio.h')
    copy_changed(ROOT / 'include/uapi/window.h', corresponding / 'include/uapi/window.h')
    copy_changed(PORT / 'SOURCE-README.txt', corresponding / 'README.txt')
    (destination / 'BUILD.txt').write_text(
        'Doomgeneric ' + manifest['commit'] + '\n'
        'Engine: GPL-2.0-or-later; data: Freedoom 0.13.0 (BSD-3-Clause).\n'
        'The IWAD was built from the pinned official Freedoom source.\n'
        'Corresponding engine/data sources and port: /usr/src/coast/doom\n'
        'Controls/help: doom --help. Sound effects and original procedural MUS/MIDI synthesis supported.\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--fetch-only', action='store_true')
    mode.add_argument('--check-sources-only', action='store_true')
    mode.add_argument('--engine-only', action='store_true')
    args = parser.parse_args()
    archive, manifest = source_archive(args.check_sources_only)
    if args.fetch_only or args.check_sources_only:
        helper = [sys.executable, str(ROOT / 'tools/build_freedoom.py'),
                  '--check-sources-only' if args.check_sources_only else '--fetch-only']
        subprocess.run(helper, check=True)
        print('Verified pinned Doomgeneric and Freedoom source archives.')
        return
    engine(archive, manifest)
    if not args.engine_only:
        package(archive, manifest)
    print('Built native Doomgeneric (static musl; native display, HID/mouse, stereo sound effects and procedural music).')


if __name__ == '__main__':
    with project_lock():
        try:
            main()
        except (OSError, ValueError, subprocess.CalledProcessError) as error:
            raise SystemExit(f"{error}; see {BUILD / 'build.log'} for compiler diagnostics") from error

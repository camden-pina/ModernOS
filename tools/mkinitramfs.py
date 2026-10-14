#!/usr/bin/env python3
"""Create a deterministic ustar initramfs containing the built static programs."""
import argparse
import os
import pathlib
import tarfile

parser = argparse.ArgumentParser()
parser.add_argument("output", type=pathlib.Path)
parser.add_argument("programs", type=pathlib.Path)
parser.add_argument("--font-license", type=pathlib.Path)
parser.add_argument("--native-licenses", type=pathlib.Path)
parser.add_argument("--runtime", type=pathlib.Path)
parser.add_argument("--wallpapers", type=pathlib.Path)
parser.add_argument("--audio-firmware", type=pathlib.Path,
                    help="Locally supplied AW88399 speaker firmware")
args = parser.parse_args()

args.output.parent.mkdir(parents=True, exist_ok=True)

with tarfile.open(args.output, "w", format=tarfile.USTAR_FORMAT) as archive:
    directory = tarfile.TarInfo("bin")
    directory.type = tarfile.DIRTYPE
    directory.mode = 0o755
    archive.addfile(directory)

    for path in sorted(args.programs.iterdir()):
        entry = tarfile.TarInfo("bin/" + path.name)
        entry.size = path.stat().st_size
        entry.mode = 0o755
        with path.open("rb") as source:
            archive.addfile(entry, source)

    if args.audio_firmware:
        size = args.audio_firmware.stat().st_size
        if not 80 <= size <= 512 * 1024:
            raise ValueError('AW88399 firmware size is outside the supported bounds')

        for name in ('usr', 'usr/lib', 'usr/lib/firmware'):
            entry = tarfile.TarInfo(name);
            entry.type = tarfile.DIRTYPE;
            entry.mode = 0o755;
            archive.addfile(entry)

        entry = tarfile.TarInfo('usr/lib/firmware/aw88399_acf.bin');
        entry.size = size;
        entry.mode = 0o444
        with args.audio_firmware.open('rb') as source:
            archive.addfile(entry, source)

    if args.font_license:
        for name in ("usr", "usr/share", "usr/share/licenses", "usr/share/licenses/modernos-desktop"):
            entry = tarfile.TarInfo(name)
            entry.type = tarfile.DIRTYPE
            entry.mode = 0o755
            archive.addfile(entry)

        entry = tarfile.TarInfo("usr/share/licenses/modernos-desktop/FONT-LICENSE.txt")
        entry.size = args.font_license.stat().st_size
        entry.mode = 0o444
        with args.font_license.open("rb") as source:
            archive.addfile(entry, source)

    if args.wallpapers:
        for name in ("usr", "usr/share", "usr/share/wallpapers"):
            entry = tarfile.TarInfo(name)
            entry.type = tarfile.DIRTYPE
            entry.mode = 0o755
            archive.addfile(entry)
        for path in sorted(args.wallpapers.iterdir()):
            if not path.is_file():
                continue
            entry = tarfile.TarInfo("usr/share/wallpapers/" + path.name)
            entry.size = path.stat().st_size
            entry.mode = 0o444
            with path.open("rb") as source:
                archive.addfile(entry, source)

    if args.native_licenses:
        entry = tarfile.TarInfo('usr/share/licenses/native-programs')
        entry.type = tarfile.DIRTYPE
        entry.mode = 0o755
        archive.addfile(entry)

        for path in sorted(args.native_licenses.glob('*.txt')):
            entry = tarfile.TarInfo('usr/share/licenses/native-programs/' + path.name)
            entry.size = path.stat().st_size
            entry.mode = 0o444
            with path.open('rb') as source:
                archive.addfile(entry, source)

    if args.runtime:
        for path in sorted(args.runtime.rglob('*')):
            name = path.relative_to(args.runtime).as_posix()
            entry = tarfile.TarInfo(name)
            if path.is_symlink():
                entry.type = tarfile.SYMTYPE
                entry.linkname = os.readlink(path)
                entry.mode = 0o777
                archive.addfile(entry)
            elif path.is_dir():
                entry.type = tarfile.DIRTYPE
                entry.mode = 0o755
                archive.addfile(entry)
            elif path.is_file() and not path.is_symlink():
                entry.size = path.stat().st_size
                entry.mode = 0o755 if path.stat().st_mode & 0o111 else 0o644
                with path.open('rb') as source:
                    archive.addfile(entry, source)

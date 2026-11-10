#!/usr/bin/env python3
"""Real packaging regression: unique deterministic entries and atomic failure."""
import io
import pathlib
import subprocess
import sys
import tarfile
import tempfile
from test_power import validate_tar

ROOT = pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='coast-initramfs-') as temporary:
    root = pathlib.Path(temporary)
    programs = root / 'programs'; programs.mkdir()
    (programs / 'check').write_bytes(b'fixture executable')
    (programs / 'sudo').write_bytes(b'fixture sudo executable')
    runtime = root / 'runtime'; (runtime / 'usr/lib').mkdir(parents=True)
    (runtime / 'usr/share').mkdir()
    (runtime / 'lib').symlink_to('usr/lib')
    (runtime / 'usr/lib/data').write_bytes(b'shared data')
    license = root / 'license'; license.write_text('fixture license\n')
    wallpapers = root / 'wallpapers'; wallpapers.mkdir()
    (wallpapers / 'sample.ppm').write_bytes(b'fixture pixels')
    firmware = root / 'firmware'; firmware.write_bytes(bytes(range(80)))
    output = root / 'initramfs.tar'
    command = [sys.executable, str(ROOT / 'tools/mkinitramfs.py'), str(output), str(programs),
               '--runtime', str(runtime), '--font-license', str(license),
               '--wallpapers', str(wallpapers), '--audio-firmware', str(firmware)]
    subprocess.run(command, check=True)
    original = output.read_bytes()
    assert output.stat().st_mode & 0o777 == 0o644
    with tarfile.open(output) as archive:
        names = archive.getnames()
        assert len(names) == len(set(names))
        assert archive.getmember('lib').issym()
        sudo=archive.getmember('bin/sudo')
        assert sudo.mode==0o4755 and sudo.uid==0 and sudo.gid==0
        assert archive.extractfile('usr/lib/data').read() == b'shared data'
    assert validate_tar(output) == len(names)
    subprocess.run(command, check=True)
    assert original == output.read_bytes(), 'packaging is not deterministic'
    (runtime / 'bin').mkdir(); (runtime / 'bin/check').write_bytes(b'conflicting runtime executable')
    failed = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    assert failed.returncode and b'Conflicting initramfs member: bin/check' in failed.stdout
    assert output.read_bytes() == original, 'failed packaging overwrote successful archive'
    assert not list(root.glob('.initramfs.tar-*')), 'staging file leaked'
    for case in ('identical-directories', 'different-mode', 'file-directory', 'duplicate-file', 'duplicate-link'):
        path = root / (case + '.tar')
        with tarfile.open(path, 'w') as archive:
            check = tarfile.TarInfo('bin/check'); archive.addfile(check, io.BytesIO())
            for index in range(2):
                entry = tarfile.TarInfo('usr'); entry.type = tarfile.DIRTYPE; entry.mode = 0o755
                if case == 'different-mode' and index: entry.mode = 0o700
                if case == 'file-directory' and index == 0: entry.type = tarfile.REGTYPE
                if case == 'duplicate-file': entry.type = tarfile.REGTYPE
                if case == 'duplicate-link': entry.type = tarfile.SYMTYPE; entry.linkname = 'elsewhere'
                archive.addfile(entry, io.BytesIO() if entry.isfile() else None)
        try: validate_tar(path)
        except ValueError:
            assert case != 'identical-directories'
        else: assert case == 'identical-directories', case
print('INITRAMFS PASS: unique deterministic packaging, exact directory-only compatibility, collision rejection and atomic failure')

"""Safely unpack pinned tar sources without optional Python compression modules.

Coast's Python has zlib but no _bz2/_lzma extensions. Use its native compressors
as streams while retaining Python's data filter for archive paths and links.
The caller must verify the source archive checksum before unpacking it.
"""
from pathlib import Path
from contextlib import contextmanager
import fcntl
import shutil
import zipfile
import os
import subprocess
import tarfile
import tempfile


def _publish(source, destination):
    if source.is_dir() and not source.is_symlink() and destination.is_dir() and not destination.is_symlink():
        for child in source.iterdir():
            _publish(child, destination / child.name)
    else:
        os.replace(source, destination)


@contextmanager
def extraction_lock(directory):
    directory.mkdir(parents=True, exist_ok=True)
    with (directory / '.coast-unpack.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        # Only this helper creates these temporary trees. A held lock proves
        # any earlier tree was abandoned, including after a hard process kill.
        for orphan in directory.glob('.coast-unpack-*'):
            if orphan.is_symlink() or orphan.is_file():
                orphan.unlink()
            else:
                shutil.rmtree(orphan)
        yield


def unpack_zip(archive, destination):
    archive, destination = Path(archive).resolve(), Path(destination).absolute()
    with extraction_lock(destination.parent):
        with tempfile.TemporaryDirectory(prefix='.coast-unpack-', dir=destination.parent) as temporary:
            staging = Path(temporary)
            with zipfile.ZipFile(archive) as source:
                for member in source.infolist():
                    name = Path(member.filename)
                    mode = (member.external_attr >> 16) & 0o170000
                    if name.is_absolute() or '..' in name.parts or mode == 0o120000:
                        raise ValueError('Unsafe zip source member: ' + member.filename)
                source.extractall(staging)
            _publish(staging, destination)


def unpack(archive, destination, filter='data'):
    destination = Path(destination).resolve()
    with extraction_lock(destination):
        _unpack(archive, destination, filter)


def _unpack(archive, destination, filter='data'):
    archive, destination = Path(archive).resolve(), Path(destination).resolve()
    destination.mkdir(parents=True, exist_ok=True)
    suffixes = {'.gz': 'gzip', '.tgz': 'gzip', '.bz2': 'bzip2', '.xz': 'xz'}
    tool = suffixes.get(archive.suffix)
    # Publish complete source directories, never a half-extracted tree that
    # an exists() check would mistake for a usable checkout after interruption.
    with tempfile.TemporaryDirectory(prefix='.coast-unpack-', dir=destination) as temporary:
        staging = Path(temporary)
        if tool is None:
            with tarfile.open(archive, 'r:') as source:
                source.extractall(staging, filter=filter)
        else:
            with subprocess.Popen([tool, '-dc', '--', str(archive)], stdout=subprocess.PIPE) as process:
                try:
                    with tarfile.open(fileobj=process.stdout, mode='r|') as source:
                        source.extractall(staging, filter=filter)
                    # Tar stops at its end marker, which may precede the end
                    # of the compressed stream. Drain it before closing the
                    # pipe so the decompressor can exit without SIGPIPE.
                    while process.stdout.read(1024 * 1024):
                        pass
                except BaseException:
                    process.terminate()
                    raise
                finally:
                    process.stdout.close()
                status = process.wait()
                if status:
                    raise subprocess.CalledProcessError(status, [tool, '-dc', str(archive)])
        for child in staging.iterdir():
            _publish(child, destination / child.name)

"""Safely unpack pinned tar sources without optional Python compression modules.

Coast's Python has zlib but no _bz2/_lzma extensions. Use its native compressors
as streams while retaining Python's data filter for archive paths and links.
The caller must verify the source archive checksum before unpacking it.
"""
from pathlib import Path
import subprocess
import tarfile


def unpack(archive, destination, filter='data'):
    archive, destination = Path(archive).resolve(), Path(destination).resolve()
    suffixes = {'.gz': 'gzip', '.bz2': 'bzip2', '.xz': 'xz'}
    tool = suffixes.get(archive.suffix)
    if tool is None:
        with tarfile.open(archive, 'r:') as source:
            source.extractall(destination, filter=filter)
        return
    with subprocess.Popen([tool, '-dc', '--', str(archive)], stdout=subprocess.PIPE) as process:
        try:
            with tarfile.open(fileobj=process.stdout, mode='r|') as source:
                source.extractall(destination, filter=filter)
        except BaseException:
            process.terminate()
            raise
        finally:
            process.stdout.close()
        status = process.wait()
        if status:
            raise subprocess.CalledProcessError(status, [tool, '-dc', str(archive)])

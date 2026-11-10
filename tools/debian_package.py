"""Read Debian control records and archives without installing their payloads."""
import io
from pathlib import PurePosixPath
import subprocess
import tarfile


def control_records(text):
    records = []
    for paragraph in text.strip().split('\n\n'):
        record, key = {}, None
        for line in paragraph.splitlines():
            if line.startswith((' ', '\t')) and key:
                record[key] += '\n' + line[1:]
            elif ':' in line:
                key, value = line.split(':', 1)
                record[key] = value.lstrip()
        if record:
            records.append(record)
    return records


def package_tar(path, kind):
    members = subprocess.check_output(['ar', 't', str(path)], text=True).splitlines()
    member, = [name for name in members if name.startswith(kind + '.tar.') or name == kind + '.tar']
    data = subprocess.check_output(['ar', 'p', str(path), member])
    if member.endswith('.zst'):
        data = subprocess.check_output(['zstd', '-dc'], input=data)
    return tarfile.open(fileobj=io.BytesIO(data), mode='r:*')


def package_control(path):
    with package_tar(path, 'control') as archive:
        member, = [m for m in archive if m.name.removeprefix('./') == 'control' and m.isfile()]
        return control_records(archive.extractfile(member).read().decode())[0]


def safe_relative(name):
    while name.startswith('./'):
        name = name[2:]
    path = PurePosixPath(name)
    if path.is_absolute() or '..' in path.parts or not path.parts:
        raise ValueError('Unsafe package path: ' + name)
    return path.as_posix()

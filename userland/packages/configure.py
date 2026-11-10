"""Initialize explicit repository trust for a fresh Coast installation."""
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import tempfile


def public_key(path, fingerprint, gpg='/usr/bin/gpg'):
    if not re.fullmatch(r'[A-F0-9]{40}', fingerprint):
        raise ValueError('use the full 40-character repository signing fingerprint')
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    with os.fdopen(descriptor, 'rb') as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode) or not 0 < info.st_size <= 1024 * 1024:
            raise ValueError('repository public key must be a regular file of at most 1 MiB')
        data = stream.read(1024 * 1024 + 1)
    with tempfile.TemporaryDirectory(prefix='coast-key-check-') as temporary:
        home = Path(temporary)
        key = home / 'public.asc'
        key.write_bytes(data)
        result = subprocess.run([gpg, '--no-options', '--batch', '--no-autostart', '--no-auto-key-retrieve',
            '--homedir', str(home), '--with-colons', '--show-keys', str(key)],
            capture_output=True, text=True, check=True)
    lines = result.stdout.splitlines()
    primary = [lines[index + 1].split(':')[9] for index, line in enumerate(lines[:-1])
               if line.startswith('pub:') and lines[index + 1].startswith('fpr:')]
    if primary != [fingerprint] or any(line.startswith(('sec:', 'ssb:')) for line in lines):
        raise ValueError('public key does not match the requested repository signing fingerprint')
    return data


def initialize(url, key, fingerprint, *, root=Path('/'), owner=0, server_texts=None):
    """Initialize only an unconfigured installation; key rotation is separate."""
    if root == Path('/') and (os.getuid() != 0 or os.geteuid() != 0 or owner != 0):
        raise ValueError('repository configuration requires authenticated sudo')
    config = root / 'etc/coast/packages.json'
    for path in (root / 'etc', config.parent, root / 'etc/pacman.d'):
        info = path.lstat()
        if not stat.S_ISDIR(info.st_mode) or info.st_uid != owner or info.st_mode & 0o022:
            raise ValueError('unsafe repository configuration directory')
    paths = [config, root / 'etc/pacman.conf', root / 'etc/makepkg.conf']
    originals = []
    for path in paths:
        descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
        with os.fdopen(descriptor, 'r') as stream:
            info = os.fstat(stream.fileno())
            if not stat.S_ISREG(info.st_mode) or info.st_uid != owner or info.st_mode & 0o022 or info.st_size > 1024 * 1024:
                raise ValueError('unsafe repository configuration file')
            originals.append(stream.read())
    current = json.loads(originals[0])
    if current.get('version') != 1 or current.get('signing_fingerprint') or current.get('url'):
        raise ValueError('repository already configured; use coast server to retain its signing key')
    data = public_key(key, fingerprint)
    current.update(url=url, signing_fingerprint=fingerprint)
    texts = server_texts(url, json.dumps(current), *originals[1:])
    keyring = root / 'etc/pacman.d/gnupg'
    info = keyring.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != owner or info.st_mode & 0o022:
        raise ValueError('unsafe repository keyring directory')
    if any((keyring / name).exists() for name in ('pubring.gpg', 'pubring.kbx', 'trustdb.gpg')):
        raise ValueError('existing repository keyring must be preserved')
    lock = config.parent / '.server-update.lock'
    descriptor = os.open(lock, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    os.close(descriptor)
    staged, installed, replaced = [], [], []

    def stage(path, content):
        descriptor, name = tempfile.mkstemp(prefix='.coast-configure-', dir=path.parent)
        with os.fdopen(descriptor, 'wb') as stream:
            stream.write(content)
        os.chmod(name, 0o644)
        return Path(name)

    try:
        with tempfile.TemporaryDirectory(prefix='.coast-trust-', dir=keyring.parent) as temporary:
            home = Path(temporary)
            public = home / 'public.asc'
            public.write_bytes(data)
            command = ['/usr/bin/gpg', '--no-options', '--batch', '--no-autostart', '--homedir', str(home)]
            subprocess.run([*command, '--import', str(public)], check=True, capture_output=True)
            subprocess.run([*command, '--import-ownertrust'], input=(fingerprint + ':6:\n').encode(), check=True, capture_output=True)
            subprocess.run([*command, '--check-trustdb'], check=True, capture_output=True)
            exported = subprocess.run([*command, '--export', fingerprint], check=True, capture_output=True).stdout
            for path, content in [(keyring / 'pubring.gpg', exported), (keyring / 'trustdb.gpg', (home / 'trustdb.gpg').read_bytes())]:
                pending = stage(path, content)
                staged.append(pending)
                os.replace(pending, path)
                installed.append(path)
        for path, text in zip(paths, texts):
            pending = stage(path, text.encode())
            staged.append(pending)
            os.replace(pending, path)
            replaced.append(path)
    except BaseException:
        for path in reversed(replaced):
            pending = stage(path, originals[paths.index(path)].encode())
            os.replace(pending, path)
        for path in installed:
            path.unlink(missing_ok=True)
        raise
    finally:
        for path in staged:
            path.unlink(missing_ok=True)
        lock.unlink()

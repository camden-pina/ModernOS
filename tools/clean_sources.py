"""Distinguish pinned upstream fixtures from user credentials in source caches."""
from functools import lru_cache
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import tarfile
import zipfile

from source_tree import files, digest

SOURCE_NAMES = {'src', 'source', 'sources', 'upstream', 'workspace', 'checkout', 'depot_tools'}


def source_directory(path):
    return path.name in SOURCE_NAMES or bool(re.match(r'^[A-Za-z][A-Za-z0-9_.+-]*-\d', path.name))


def source_root(path, root):
    # Keep the outer source tree coherent if it contains actual private edits.
    candidates = []
    for parent in (path, *path.parents):
        if parent in (root, root / 'build', Path('/')) or (parent / 'OWNER.json').is_file():
            break
        if parent.is_dir() and not parent.is_symlink() and source_directory(parent):
            candidates.append(parent)
    return candidates[-1] if candidates else None


@lru_cache(maxsize=8)
def pins(root):
    archives, commits = {}, set()
    def visit(value):
        if isinstance(value, dict):
            if isinstance(value.get('archive'), str) and re.fullmatch(r'[a-f0-9]{64}', str(value.get('sha256', ''))):
                archives.setdefault(value['archive'], set()).add(value['sha256'])
            if re.fullmatch(r'[a-f0-9]{40}', str(value.get('commit', ''))):
                commits.add(value['commit'])
            for child in value.values():
                visit(child)
        elif isinstance(value, list):
            for child in value:
                visit(child)
    for name in files(root):
        if name.endswith('.json') and name.startswith(('ports/', 'userland/vendor/')):
            try:
                visit(json.loads((root / name).read_text()))
            except (ValueError, UnicodeError):
                pass
    return archives, commits


@lru_cache(maxsize=16)
def archive_inventory(path, expected, size, modified):
    if digest(path) not in expected:
        return {}
    result = {}
    def add(name, kind, value):
        parts = PurePosixPath(name).parts
        if not parts or '..' in parts or name.startswith('/'):
            return
        result[str(PurePosixPath(*parts))] = kind, value
        if len(parts) > 1:
            result[str(PurePosixPath(*parts[1:]))] = kind, value
    try:
        if path.suffix in ('.zip', '.whl'):
            with zipfile.ZipFile(path) as source:
                for entry in source.infolist():
                    if not entry.is_dir():
                        data = source.read(entry)
                        link = (entry.external_attr >> 16) & 0o170000 == 0o120000
                        add(entry.filename, 'link' if link else 'file', data.decode() if link else hashlib.sha256(data).hexdigest())
        elif path.name.endswith(('.tar.gz', '.tar.xz', '.tar.bz2', '.tar', '.tgz')):
            with tarfile.open(path) as source:
                for entry in source:
                    if entry.isfile():
                        with source.extractfile(entry) as stream:
                            add(entry.name, 'file', hashlib.file_digest(stream, 'sha256').hexdigest())
                    elif entry.issym():
                        add(entry.name, 'link', entry.linkname)
    except (OSError, ValueError, tarfile.TarError, zipfile.BadZipFile):
        return {}
    return result


def public_fixture(path, root):
    """Only unchanged, pinned source bytes can bypass credential-name protection."""
    tree = source_root(path, root)
    if tree is None:
        return False
    if path.is_dir() and not path.is_symlink():
        children = list(path.rglob('*'))
        if any(not (child.is_dir() or child.is_file() or child.is_symlink()) for child in children):
            return False
        members = [child for child in children if child.is_file() or child.is_symlink()]
    elif path.is_file() or path.is_symlink():
        members = [path]
    else:
        return False
    if not members:
        return True  # Empty generated directories contain no private data.
    archives, commits = pins(root)
    for parent in (path, *path.parents):
        if parent == root:
            break
        if (parent / '.git').is_dir():
            head = subprocess.run(['git', '-C', str(parent), 'rev-parse', 'HEAD'], capture_output=True, text=True)
            if head.returncode == 0 and head.stdout.strip() in commits:
                relative = str(path.relative_to(parent))
                tracked = subprocess.run(['git', '-C', str(parent), 'ls-files', '--', relative], capture_output=True, text=True)
                status = subprocess.run(['git', '-C', str(parent), 'status', '--porcelain', '--ignored', '--untracked-files=all', '--', relative], capture_output=True, text=True)
                if tracked.returncode == status.returncode == 0 and tracked.stdout and not status.stdout:
                    return True
            break
    candidates = []
    for parent in (tree, *tree.parents):
        if parent == root.parent:
            break
        for vendor in (parent / 'vendor', parent / 'downloads'):
            if vendor.is_dir() and not vendor.is_symlink():
                candidates.extend(p for p in vendor.iterdir() if p.name in archives and p.is_file() and not p.is_symlink())
        if parent == root / 'build':
            break
    # Core archives also use the common vendor directory. Limit this fallback
    # to the source's own name so unrelated caches need not be decompressed.
    common = root / 'userland/vendor'
    for name in archives:
        if name.startswith(tree.name) and (common / name).is_file():
            candidates.append(common / name)
    for archive in set(candidates):
        stat = archive.stat()
        inventory = archive_inventory(archive, tuple(sorted(archives[archive.name])), stat.st_size, stat.st_mtime_ns)
        if inventory and all(inventory.get(str(member.relative_to(tree))) ==
                (('link', os.readlink(member)) if member.is_symlink() else ('file', digest(member))) for member in members):
            return True
    return False

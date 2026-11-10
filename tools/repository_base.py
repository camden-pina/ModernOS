"""Read the qualified boot payload; never run its programs on the host.

Base tool binaries in the working tree may predate the selected desktop.
Package the actual ISO bytes, applying its boot overlays in their boot order.
Only selected regular files are materialized; archive links remain metadata.
"""
import hashlib
from pathlib import Path, PurePosixPath
import re
import tarfile


COMMANDS = {
    'gcc': {'gcc', 'g++', 'cc', 'c++', 'cpp', 'gcov', 'gcc-dynamic', 'g++-dynamic', 'bootstrap-toolchain'},
    'binutils': {'addr2line', 'ar', 'as', 'c++filt', 'ld', 'nm', 'objcopy', 'objdump', 'ranlib', 'readelf', 'size', 'strings', 'strip'},
    'make': {'make'}, 'gawk': {'gawk', 'awk'},
    'git': {'git', 'git-cvsserver', 'git-receive-pack', 'git-shell', 'git-upload-archive', 'git-upload-pack', 'scalar'},
    'nano': {'nano'}, 'vim': {'vim', 'vi', 'view', 'vimdiff', 'ex'},
}


def owner(name):
    path = PurePosixPath(name)
    if str(path.parent) in ('bin', 'usr/bin'):
        return next((package for package, commands in COMMANDS.items() if path.name in commands), None)
    if name.startswith(('usr/lib/gcc/', 'usr/libexec/gcc/', 'usr/include/c++/',
                        'usr/include/c++-dynamic/', 'usr/lib/coast/c++/')) or name in {
                            'usr/lib/coast-cpp.specs', 'usr/lib/coast-dynamic.specs',
                            'usr/lib/libstdc++.a', 'usr/lib/libstdc++exp.a', 'usr/lib/libstdc++fs.a', 'usr/lib/libsupc++.a'}:
        return 'gcc'
    if name.startswith(('usr/libexec/git-core/', 'usr/share/git-core/')):
        return 'git'
    if name.startswith('usr/share/vim/'):
        return 'vim'
    if name == 'etc/nanorc':
        return 'nano'
    if name.startswith('usr/share/licenses/native-programs/'):
        if path.name.startswith(('gmp-', 'mpfr-', 'mpc-')):
            return 'gcc'
        return next((package for package in COMMANDS if path.name.startswith(package + '-')), None)
    return None


def extract(iso, destination, host):
    iso = Path(iso).resolve()
    with iso.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    listing = host(['xorriso', '-no_rc', '-indev', iso, '-find', '/boot', '-type', 'f', '-exec', 'report_lba', '--']).decode()
    extents = {name: (int(lba) * 2048, int(size)) for lba, size, name in re.findall(
        r"File data lba:\s*0\s*,\s*(\d+)\s*,\s*\d+\s*,\s*(\d+)\s*,\s*'([^']+)'", listing)}
    with iso.open('rb') as stream:
        offset, size = extents['/boot/limine/limine.conf']
        stream.seek(offset)
        config = stream.read(size).decode()
    # Only the selected first Coast entry is booted, not alternate entries.
    entry = config.split('\n/Coast\n', 1)[1].split('\n/', 1)[0]
    modules = re.findall(r'module_path:\s*boot\(\):([^\n]+)', entry)
    assert modules and all(module in extents and module.endswith('.tar') for module in modules)
    files, aliases, origins = {}, {}, {}
    destination.mkdir(parents=True, exist_ok=False)
    for module in modules:
        with iso.open('rb') as stream:
            stream.seek(extents[module][0])
            with tarfile.open(fileobj=stream, mode='r|') as archive:
                for member in archive:
                    name = member.name.removeprefix('./').rstrip('/')
                    if PurePosixPath(name).is_absolute() or '..' in PurePosixPath(name).parts:
                        raise ValueError('unsafe boot archive path: ' + name)
                    package = owner(name)
                    if not package or member.isdir():
                        continue
                    origins[name] = module
                    if member.isfile():
                        path = destination / name
                        path.parent.mkdir(parents=True, exist_ok=True)
                        with archive.extractfile(member) as source, path.open('wb') as output:
                            while block := source.read(1024 * 1024):
                                output.write(block)
                        files[name] = dict(path=str(path), mode=0o755 if member.mode & 0o111 else 0o644, owner=package)
                        aliases.pop(name, None)
                    elif member.issym():
                        aliases[name] = dict(target=member.linkname, owner=package)
                        files.pop(name, None)
                    else:
                        raise ValueError('unsupported selected boot archive entry: ' + name)
    for package in COMMANDS:
        assert any(asset['owner'] == package for asset in files.values()), 'missing base package: ' + package
    assert files['usr/bin/gcc'] and files['usr/bin/g++']
    with iso.open('rb') as stream:
        assert hashlib.file_digest(stream, 'sha256').hexdigest() == digest, 'base ISO changed while packaging'
    return files, aliases, dict(iso=str(iso), sha256=digest, boot_modules=modules, origins=origins)

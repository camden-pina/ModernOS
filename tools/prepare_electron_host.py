"""Restore checksum-pinned optional Node modules used by Linux build scripts."""
from source_archive import unpack
from build_state import atomic_write
import base64
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import site
import sysconfig
import tarfile

from fetch_sources import fetch

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / 'ports/electron/host-tools.json'


def generated_source_overlay(output, source):
    """Keep upstream's two repo-root TypeScript outputs outside the checkout.

    The upstream docs parser and TypeScript consumers share fixed source-root
    paths. A private mount supplies those generated files at their expected paths
    while preserving the original checkout, including its Git metadata.
    """
    output, source = Path(output).resolve(), Path(source).resolve()
    upper = output / 'generated-electron'
    upper.mkdir(parents=True, exist_ok=True)
    electron = source / 'electron'
    generated = {'electron-api.json', 'electron.d.ts'}
    mounts = ['--tmpfs', str(electron)]
    for child in sorted(electron.iterdir()):
        if child.name in generated:
            continue
        if child.is_symlink():
            mounts += ['--symlink', str(child.readlink()), str(child)]
        else:
            mounts += ['--ro-bind', str(child), str(child)]
    for name in sorted(generated):
        path = upper / name
        if not path.exists():
            path.touch()
        mounts += ['--bind', str(path), str(electron / name)]
    return mounts


def generated_source_record(output):
    upper = Path(output) / 'generated-electron'
    files = {str(path.relative_to(upper)): hashlib.sha256(path.read_bytes()).hexdigest()
             for path in upper.rglob('*') if path.is_file()}
    if set(files) - {'electron-api.json', 'electron.d.ts'}:
        raise ValueError('Unexpected upstream source writes: ' + ', '.join(sorted(files)))
    return files


def out_of_tree_generator(output, source):
    """Resolve Node's original-fs inputs from its source, not the output cwd."""
    output, source = Path(output).resolve(), Path(source).resolve()
    original = source / 'electron/script/node/generate_original_fs.py'
    contents = original.read_text()
    before = 'NODE_ROOT_DIR = "../../third_party/electron_node"'
    after = ('NODE_ROOT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), '
             '"../../../third_party/electron_node"))')
    if contents.count(before) != 1:
        raise ValueError('Unexpected upstream original-fs generator; review its output path port')
    output.mkdir(parents=True, exist_ok=True)
    patched = output / 'generate_original_fs.py'
    patched.write_text(contents.replace(before, after))
    return ['--ro-bind', str(patched), str(original)], dict(
        upstream_sha256=hashlib.sha256(contents.encode()).hexdigest(),
        patched_sha256=hashlib.sha256(patched.read_bytes()).hexdigest())


def prepare_node(output, source):
    """Expose pinned host Node and vendored npm without a global installation."""
    output, source = Path(output).resolve(), Path(source).resolve()
    pin = json.loads((ROOT / 'ports/electron/node-host.json').read_text())
    node, npm = source / pin['node_binary'], source / pin['npm_source']
    if hashlib.sha256(node.read_bytes()).hexdigest() != pin['node_binary_sha256']:
        raise ValueError('Host Node differs from Chromium\'s pinned build tool')
    if hashlib.sha256((npm / 'package.json').read_bytes()).hexdigest() != pin['npm_package_sha256']:
        raise ValueError('Vendored npm differs from the pinned Node source')
    metadata = json.loads((npm / 'package.json').read_text())
    if metadata['version'] != pin['npm_version']:
        raise ValueError('Unexpected vendored npm version')
    binary = output / 'bin'
    binary.mkdir(parents=True, exist_ok=True)
    for name, script in [('node', None), ('npm', npm / 'bin/npm-cli.js'), ('npx', npm / 'bin/npx-cli.js')]:
        path = binary / name
        command = [str(node)] + ([str(script)] if script else [])
        path.write_text('#!/bin/sh\nexec ' + shlex.join(command) + ' "$@"\n')
        path.chmod(0o755)
    for name, asset in pin.get('executables', {}).items():
        executable = source / asset['path']
        if hashlib.sha256(executable.read_bytes()).hexdigest() != asset['sha256']:
            raise ValueError('Unexpected pinned host build tool: ' + name)
        path = binary / name
        command = [str(executable)] + (['--root=' + str(source)] if name == 'gn' else [])
        extra = (' "--root-pattern=$COAST_GN_ROOT_TARGET" "--root-target=$COAST_GN_ROOT_TARGET"'
                 if name == 'gn' else '')
        path.write_text('#!/bin/sh\nexec ' + shlex.join(command) + extra + ' "$@"\n')
        path.chmod(0o755)
    record = dict(node_sha256=pin['node_binary_sha256'], npm_version=pin['npm_version'],
        npm_package_sha256=pin['npm_package_sha256'], executables=pin.get('executables', {}),
        source='Chromium host tools and pinned Node npm sources')
    atomic_write(output / 'NODE.json', json.dumps(record, indent=2) + '\n')
    return binary, record


def protobuf_isolation(output):
    """Hide system native protobuf modules from the pinned Python sources.

    The older vendored API probes native extensions before consulting its
    backend environment variable. A newer system upb module can therefore
    fail during the probe itself. Empty read-only mounts prevent that mixed
    API import without changing either upstream source or host installation.
    """
    output = Path(output).resolve()
    empty = output / 'host-tools/empty-native-protobuf'
    empty.mkdir(parents=True, exist_ok=True)
    roots = {Path(p) for p in site.getsitepackages()}
    roots.update(Path(sysconfig.get_path(k)) for k in ('purelib', 'platlib'))
    paths = sorted({root / relative for root in roots
        for relative in ('google/_upb', 'google/protobuf/pyext') if (root / relative).is_dir()})
    return [argument for path in paths for argument in ('--ro-bind', str(empty), str(path))], [str(p) for p in paths]


def prepare(output):
    output = Path(output).resolve()
    downloads = output / 'downloads'
    modules = output / 'node_modules'
    downloads.mkdir(parents=True, exist_ok=True)
    for record in json.loads(MANIFEST.read_text()):
        archive = downloads / record['archive']
        cached = ROOT / 'build/electron-host-downloads' / record['archive']
        if not archive.exists() and cached.is_file():
            if hashlib.sha256(cached.read_bytes()).hexdigest() != record['sha256']:
                raise ValueError('Corrupt optional host-module cache')
            shutil.copyfile(cached, archive)
        fetch(record, downloads)
        if 'sha512-' + base64.b64encode(hashlib.sha512(archive.read_bytes()).digest()).decode() != record['integrity']:
            raise ValueError('NPM lock integrity differs: ' + record['name'])
        destination = modules / record['name']
        stamp = destination / '.coast-source'
        unpacked = output / ('unpack-' + record['archive'])
        # An interrupted extraction can leave this owned scratch directory.
        # Recreate it from the checksum-verified archive on retry.
        if unpacked.is_symlink():
            raise ValueError('Host-module scratch directory cannot be a symlink')
        if unpacked.exists():
            shutil.rmtree(unpacked)
        if stamp.is_file() and stamp.read_text() == record['sha256']:
            continue
        if destination.exists():
            raise ValueError('Unrecognized host module; select a fresh output directory')
        unpacked.mkdir()
        unpack(archive, unpacked, filter='data')
        package = unpacked / 'package'
        metadata = json.loads((package / 'package.json').read_text())
        if metadata['name'] != record['name'] or metadata['version'] != record['version']:
            raise ValueError('Unexpected NPM host module identity')
        destination.parent.mkdir(parents=True, exist_ok=True)
        # The completed marker moves with the package, so a process killed
        # immediately after publication still leaves a reusable module.
        atomic_write(package / '.coast-source', record['sha256'])
        package.replace(destination)
        shutil.rmtree(unpacked)
    return modules

#!/usr/bin/env python3
"""Plan/apply dependency-aware cleanup of generated Coast build products."""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import time

from build_components import APPS, APP_COMPONENTS, DEPENDENCIES, RECEIPTS, TOOLCHAINS, app_names, dependents, graph_for, receipt_path, words
from build_state import Checkpoints, _process, project_lock, read_json, write_json
from source_tree import files
from clean_sources import public_fixture, source_directory, source_root

ROOT = Path(__file__).resolve().parents[1]
PRIVATE = {'private-keys-v1.d', 'openpgp-revocs.d', 'coast-signing-keys', '.gnupg',
           'credentials', 'state', 'home', 'root', '.ssh', '.aws', '.docker', 'local-notes', 'cleanup-records', 'maintenance-checks'}
PRIVATE_FILES = {'.env', '.netrc', '.npmrc', '.pypirc', 'secring.gpg', 'id_rsa', 'id_ed25519', 'id_ecdsa'}
CACHE_NAMES = {'__pycache__', '.pytest_cache', 'npm-cache', 'thinlto-cache', 'cargo-home'}
SOURCE_NAMES = {'src', 'source', 'sources', 'upstream', 'workspace', 'checkout'}
DOWNLOAD_NAMES = {'vendor', 'downloads', 'distfiles'}
ARCHIVES = ('.tar.gz', '.tar.xz', '.tar.bz2', '.tar.zst', '.tgz', '.zip', '.whl', '.deb', '.dsc')


def beneath(path, parent):
    return path == parent or path.is_relative_to(parent)


def validate_output(root, output):
    root, output = Path(root).resolve(), Path(os.path.abspath(output))
    if output.is_symlink() or output.resolve() != output or output in (root, root / 'build', root.parent, Path('/')):
        raise ValueError('Choose a dedicated, real build directory for cleanup: ' + str(output))
    if not beneath(output, root / 'build') and output.exists():
        owner = read_json(output / 'OWNER.json', {})
        if owner.get('source_root') != str(root):
            raise ValueError('External build directory is not registered to this project: ' + str(output))
    return output


def protected_paths(root):
    root = Path(root).resolve()
    protected = {root / '.git', root / 'build/.lifecycle.lock', root / 'build/.lifecycle-owner.json',
                 root / 'build/build-roots.json', root / 'build/kernel-roots.json', root / 'build/config-files.json', root / 'build/.fetch-locks'}
    protected.update(root / name for name in files(root))
    for name in read_json(root / 'build/config-files.json', []):
        protected.update((Path(name), Path(name + '.old')))
    protected.update(root.glob('*.config*'))
    protected.update((root / '.config', root / '.config.old'))
    build = root / 'build'
    if build.exists():
        for link in build.iterdir():
            if link.is_symlink():
                protected.add(link)
                target = link.resolve()
                protected.add(target.parent if target.is_file() else target)
    return protected


def _preserved(path, keep, component=None, root=None):
    if path.name in PRIVATE or path.name in PRIVATE_FILES or path.name.startswith('.env.') or path.name in {'.config', '.config.old'} or path.name.endswith(('.config', '.config.old')) or path.suffix in {'.key', '.p12', '.pfx'}:
        return not (root is not None and public_fixture(path, root))
    if 'downloads' in keep and (path.name in DOWNLOAD_NAMES or path.name.endswith(ARCHIVES) or (path.name.endswith('.part') and any(ext + '.' in path.name for ext in ARCHIVES))):
        return True
    if 'sources' in keep and (path.is_dir() and not path.is_symlink() and source_directory(path)):
        return True
    if 'toolchains' in keep and (component in TOOLCHAINS or path.name in TOOLCHAINS or path.name in {'musl', 'host-tools', 'cross', 'sysroot', 'sdk', 'toolchains', 'checkout', 'depot_tools', 'SOURCE.json', 'SYNC.json'}):
        return True
    if 'toolchains' in keep and path.name.endswith('-sdk'):
        return True
    if 'toolchains' in keep and re.match(r'^(?:musl|gcc|gnu-sdk|codeoss-sdk|glibc|gnu-libgcc|gnu-cxx|llvm|nasm|limine|image-tools)(?:[-.]|$)', path.name):
        return True
    if 'toolchains' in keep and path.is_dir() and not path.is_symlink():
        if (path / 'SDK.json').is_file() or (path / 'APPLICATION-SDK.json').is_file() or (path / 'cross/bin').is_dir():
            return True
        receipt = read_json(path / 'BUILD.json', {})
        if isinstance(receipt, dict) and {'electron', 'node'} <= receipt.get('products', {}).keys():
            return True
    if 'images' in keep and path.suffix == '.iso':
        return True
    if 'logs' in keep and (path.name == 'logs' or path.suffix == '.log'):
        return True
    if 'tests' in keep and path.name in {'qualification', 'test-fixtures'}:
        return True
    return False


def select_paths(paths, root, *, keep=(), component=None, protected=None):
    """Prune whole disposable subtrees; descend only around protected entries."""
    root = Path(root).resolve()
    protected = protected_paths(root) if protected is None else protected
    selected = []
    private_trees = set()
    companions = set()
    preparation = {'SOURCE.json', 'SYNC.json', 'PATCHED.json', 'INSTALLED.json',
                   'PREPARED.json', 'APPLICATION-PACKAGING.json'}

    def retain_preparation(tree):
        for parent in tree.parents:
            if parent in (root, root / 'build', Path('/')):
                break
            companions.update(parent / name for name in preparation)
            if (parent / 'OWNER.json').is_file():
                break

    def walk(path):
        if not path.exists() and not path.is_symlink():
            return
        if any(parent in protected or parent in private_trees for parent in (path, *path.parents)):
            return
        if _preserved(path, keep, component, root):
            if _preserved(path, (), None, root):
                tree = source_root(path, Path(root))
                if tree is not None:
                    private_trees.add(tree)
                    retain_preparation(tree)
            elif 'sources' in keep and source_directory(path):
                retain_preparation(path)
            return
        if path.is_symlink():
            selected.append(path)
            return
        if path.is_dir():
            children = list(path.iterdir())
            before = len(selected)
            for child in children:
                walk(child)
            if any(parent in private_trees for parent in (path, *path.parents)):
                selected[before:] = []
                return
            chosen = selected[before:]
            if len(chosen) == len(children) and set(chosen) == set(children):
                selected[before:] = [path]
            elif not children:
                selected.append(path)
        else:
            selected.append(path)

    for path in paths:
        walk(Path(path))
    return sorted(set(selected) - companions)


def component_paths(output, component, keep):
    output = Path(output)
    if component == 'sources':
        return [output / 'electron' / name for name in ('checkout', 'depot_tools', 'SOURCE.json', 'SYNC.json')]
    if component == 'electron':
        directory = output / 'electron'
        # Git source and host dependencies are separate from its object trees.
        paths = [directory / 'checkout/src/out/Coast-electron', directory / 'checkout/src/out/Coast-node']
        if directory.exists():
            paths += [p for p in directory.iterdir() if p.name not in
                      {'checkout', 'depot_tools', 'SOURCE.json', 'SYNC.json', '.source.lock'}]
        return paths
    return [output / component]


def invalidation_paths(output, names):
    paths = [Path(output) / 'BUILD.json', Path(output) / 'PIPELINE.json'] if 'image' in names else []
    for name in names:
        paths.append(receipt_path(output, name))
        if name == 'electron':
            paths.append(Path(output) / 'electron/NODE.json')
    return paths


def kernel_paths(root, output):
    root, output = Path(root).resolve(), Path(output).resolve()
    if output.exists() and output != root and not beneath(output, root / 'build'):
        if read_json(output / '.coast-kernel.json', {}).get('source_root') != str(root):
            raise ValueError('External kernel output is not registered to this project: ' + str(output))
    command = ['make', '--no-print-directory', '-n', '-f', str(Path(root) / 'scripts/Makefile.clean'),
               'srctree=' + str(root), 'objtree=' + str(output), 'obj=.']
    result = subprocess.run(command, cwd=root, capture_output=True, text=True, check=True)
    paths = []
    for line in result.stdout.splitlines():
        if line.startswith('rm -f -- '):
            paths.extend(Path(name).absolute() for name in shlex.split(line)[3:])
    paths += [Path(output) / name for name in ('vmlinux', 'vmlinux.cmd', 'initramfs.tar', 'iso_root', 'arch/x86/boot/efi')]
    return paths


def git_recovery_paths(root, output):
    lock = read_json(Path(root) / 'ports/electron/sources.lock.json', {})
    electron = Path(output) / 'electron'
    repositories = [electron / 'depot_tools'] + [electron / 'checkout' / record['path'] for record in lock.get('repositories', [])]
    result = []
    for repo in repositories:
        gitdir = repo / '.git'
        if not gitdir.is_dir() or gitdir.is_symlink():
            continue
        result += [gitdir / name for name in ('index.lock', 'HEAD.lock', 'shallow.lock', 'config.lock')]
        result += list((gitdir / 'objects/pack').glob('tmp_pack_*'))
    return result


def plan(root=ROOT, output=None, components='', all_generated=False, keep=None, kernel_output=None):
    root = Path(root).resolve()
    output = validate_output(root, output or root / 'build/coast')
    if keep == 'none':
        keep = ''
    if components == 'all':
        components = ''
    if all_generated and components:
        raise ValueError('Choose ALL=1 or COMPONENT=...; do not combine a full reset with component selection')
    keep = words(keep) if keep is not None else set() if all_generated else {'sources', 'downloads'}
    allowed_keep = {'sources', 'downloads', 'toolchains', 'images', 'logs', 'tests'}
    if keep - allowed_keep:
        raise ValueError('Unknown KEEP categories: ' + ', '.join(sorted(keep - allowed_keep)))
    requested = words(components) if components else set(DEPENDENCIES) - {'sources'}
    pseudo = {'cache', 'downloads', 'sources', 'logs', 'failed', 'tests', 'containers', 'userland', 'kernel-tree'}
    unknown = requested - DEPENDENCIES.keys() - APP_COMPONENTS.keys() - pseudo
    if unknown:
        raise ValueError('Unknown cleanup components: ' + ', '.join(sorted(unknown)))
    targets = set()
    for name in requested:
        targets |= APP_COMPONENTS.get(name, {name})
    nodes = targets & DEPENDENCIES.keys()
    if not components and not ({'sources', 'toolchains'} & keep):
        nodes.add('sources')
    if 'toolchains' in keep:
        nodes -= TOOLCHAINS
    selection = read_json(output / 'image/MANIFEST.json') or read_json(output / 'SELECTION.json') or read_json(output / 'PIPELINE.json', {})
    active_apps = app_names(selection.get('apps', APPS))
    invalidated = dependents(nodes, graph_for(active_apps))
    protected = protected_paths(root)
    paths = []
    for name in sorted(nodes) if not all_generated else ():
        node_keep = keep - {'sources'} if name == 'sources' and components else keep
        paths += select_paths(component_paths(output, name, keep), root, keep=node_keep,
                              component=name, protected=protected)
    # A preserved source checkout must still lose its compiler output trees.
    if 'electron' in nodes and 'toolchains' not in keep:
        paths += select_paths([output / 'electron/checkout/src/out/Coast-electron',
                               output / 'electron/checkout/src/out/Coast-node'], root,
                              keep=keep - {'sources'}, protected=protected)
    for path in invalidation_paths(output, invalidated):
        if path.exists() and not any(parent in protected for parent in (path, *path.parents)):
            # Retained toolchain receipts remain usable. Their source hashes
            # and dependency identities are revalidated before the next reuse.
            if 'toolchains' in keep and any(beneath(path, output / name) for name in TOOLCHAINS):
                continue
            paths.append(path)
    if not components or targets & {'kernel-tree', 'kernel'}:
        paths += select_paths(kernel_paths(root, Path(kernel_output or root / 'build')), root,
                              keep=keep, protected=protected)
    if not components or 'userland' in targets:
        paths += select_paths([root / 'userland' / name for name in ('obj', 'bin', 'runtime')], root,
                              keep=keep, protected=protected)
    output_roots = [str(output)]
    if all_generated:
        for name in read_json(root / 'build/kernel-roots.json', []):
            kernel = Path(name)
            if kernel.exists():
                paths += select_paths(kernel_paths(root, kernel), root, keep=keep, protected=protected)
        for name in read_json(root / 'build/build-roots.json', []):
            other = Path(name)
            if other == output or not other.exists():
                continue
            validate_output(root, other)
            output_roots.append(str(other))
            if beneath(other, root / 'build'):
                continue
            paths += select_paths([p for p in other.iterdir() if p.name != 'OWNER.json'], root, keep=keep, protected=protected)
        build = root / 'build'
        if output.exists() and not beneath(output, build):
            paths += select_paths([p for p in output.iterdir() if p.name != 'OWNER.json'], root, keep=keep, protected=protected)
        paths += select_paths([build], root, keep=keep, protected=protected)
        if 'downloads' not in keep:
            paths += select_paths([root / 'userland/vendor'], root, keep=keep, protected=protected)
    if 'downloads' in targets:
        paths += select_paths([root / 'userland/vendor'], root, protected=protected)
    if targets & {'cache', 'downloads', 'logs', 'failed', 'tests'}:
        names = CACHE_NAMES if 'cache' in targets else set()
        if 'downloads' in targets:
            names |= DOWNLOAD_NAMES
        if 'logs' in targets:
            names |= {'logs'}
        if 'failed' in targets:
            names |= {'failed'}
        if 'tests' in targets:
            names |= {'qualification', 'test-fixtures'}
        scan_roots = [root / 'build'] + ([output] if not beneath(output, root / 'build') else [])
        for current, directories, filenames in (entry for base in scan_roots for entry in os.walk(base, followlinks=False)):
            base = Path(current)
            if any(parent in protected for parent in (base, *base.parents)) or base.name in PRIVATE:
                directories[:] = []
                continue
            if 'cache' in targets:
                for name in filenames:
                    if re.match(r'^\.(?:[A-Z][A-Z0-9_-]*|build-roots|kernel-roots|config-files)\.json\..+\.tmp$', name):
                        paths += select_paths([base / name], root, protected=protected)
            for name in list(directories):
                child = base / name
                if child.is_symlink():
                    directories.remove(name)
                elif name in names or ('cache' in targets and name.startswith('.coast-unpack-')):
                    paths += select_paths([child], root, protected=protected)
                    directories.remove(name)
                elif name in {'checkout', 'node_modules', '.git'}:
                    directories.remove(name)
    if 'cache' in targets or all_generated:
        paths += select_paths(git_recovery_paths(root, output), root, protected=protected)
        for base in (root / 'tools', root / 'ports', root / 'userland'):
            for current, directories, _ in os.walk(base, followlinks=False):
                for name in list(directories):
                    child = Path(current) / name
                    if name in CACHE_NAMES:
                        paths += select_paths([child], root, protected=protected)
                        directories.remove(name)
                    elif name in {'vendor', 'runtime', 'obj', 'bin', 'node_modules', '.git'} or child.is_symlink():
                        directories.remove(name)
    containers = 'containers' in targets or 'gui-sources' in nodes or (all_generated and 'toolchains' not in keep)
    if containers:
        for directory in output_roots:
            paths += select_paths([Path(directory) / 'gui-sources/ENVIRONMENT.json'], root, protected=protected)
    unique = sorted(set(paths), key=lambda p: (len(p.parts), str(p)))
    selected = []
    for path in unique:
        if not any(beneath(path, parent) for parent in selected):
            selected.append(path)
    return dict(format_version=1, source_root=str(root), output_root=str(output),
        requested=sorted(targets), invalidated=sorted(invalidated), keep=sorted(keep),
        affected_apps=sorted(app for app in active_apps if APP_COMPONENTS[app] & invalidated),
        all_generated=all_generated, output_roots=output_roots, paths=[str(p) for p in selected],
        containers=containers,
        protected='Tracked sources, configuration, selected images/services and private state')


def assert_idle(root, paths=None):
    """Refuse cleanup around legacy/manual project jobs outside the lifecycle lock."""
    root = Path(root).resolve()
    ancestors, pid = set(), os.getpid()
    while pid > 1:
        ancestors.add(pid)
        state = _process(pid)
        if not state:
            break
        pid = state[0]
    for directory in Path('/proc').glob('[0-9]*'):
        if int(directory.name) in ancestors:
            continue
        try:
            arguments = (directory / 'cmdline').read_bytes().decode(errors='replace').split('\0')[:-1]
            if not arguments:
                continue
            executable = Path(arguments[0]).name
            cwd = (directory / 'cwd').resolve(strict=True)
            builders = any(str(root / 'tools') + '/build_' in arg or str(root / 'tools') + '/prepare_' in arg for arg in arguments[:3])
            related = beneath(cwd, root) or any(str(root / 'build') in arg for arg in arguments)
            if executable.startswith('qemu-system') and not related:
                related = any(str(root / 'build') in os.readlink(fd) for fd in (directory / 'fd').iterdir())
            if executable.startswith('qemu-system') and related and paths is not None:
                references = []
                for descriptor in (directory / 'fd').iterdir():
                    try:
                        target = os.readlink(descriptor)
                        if target.startswith('/'):
                            references.append(Path(target.removesuffix(' (deleted)')))
                    except OSError:
                        continue
                for argument in arguments[1:]:
                    for field in argument.split(','):
                        value = field.split('=', 1)[-1]
                        candidate = Path(value) if value.startswith('/') else cwd / value
                        if candidate.exists():
                            references.append(candidate.resolve())
                if not any(beneath(reference, Path(path)) for reference in references for path in paths):
                    continue
            git_writer = executable == 'git' and bool(set(arguments[1:]) & {'clone', 'fetch', 'pull', 'checkout', 'switch', 'reset', 'rebase', 'merge', 'gc', 'repack', 'index-pack', 'pack-objects', 'update-index', 'add', 'commit', 'apply', 'submodule'})
            worker = git_writer or executable.startswith(('qemu-system', 'clang', 'gcc', 'cc1', 'g++')) or executable in {'make', 'gmake', 'ninja', 'gclient', 'rustc', 'cargo', 'cmake', 'meson'}
            if related and (builders or worker):
                raise ValueError('Project process still running (PID ' + directory.name + ', ' + executable + '). Stop it before cleanup.')
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            continue


def remove_containers(outputs, runner=None, available=None):
    runner, available = runner or subprocess.run, available or shutil.which
    if not available('podman'):
        return
    for output in outputs:
        label = 'label=io.coast.source-build=' + str(Path(output) / 'gui-sources')
        result = runner(['podman', 'ps', '-a', '--filter', label, '--format', '{{.ID}}'],
                        capture_output=True, text=True)
        if result.returncode:
            raise ValueError('Cannot inspect managed GUI containers; check rootless podman before cleaning that component')
        for container in result.stdout.splitlines():
            if not re.fullmatch(r'[a-f0-9]{12,64}', container):
                raise ValueError('Unexpected managed container identifier')
            runner(['podman', 'rm', '-f', container], check=True, stdout=subprocess.DEVNULL)


def apply(record, root=ROOT):
    root = Path(root).resolve()
    with project_lock(root):
        assert_idle(root, record['paths'])
        before = shutil.disk_usage(root).free
        destination = root / 'build/cleanup-records' / (str(time.time_ns()) + '.json')
        record.update(applied=False, free_bytes_before=before)
        write_json(destination, record)
        output = Path(record['output_root'])
        selection = read_json(output / 'image/MANIFEST.json')
        if selection and 'apps' in selection and not record['all_generated']:
            selected = app_names(selection['apps'])
            write_json(output / 'SELECTION.json', dict(apps=[app for app in APPS if app in selected]))
        if record['containers']:
            remove_containers(record.get('output_roots', [record['output_root']]))
        protected = protected_paths(root)
        paths = select_paths(record['paths'], root, protected=protected)
        for path in paths:
            if path.is_dir() and not path.is_symlink():
                shutil.rmtree(path)
            else:
                path.unlink(missing_ok=True)
        for name in record.get('output_roots', [record['output_root']]):
            output = Path(name)
            if (output / 'STATE.json').exists():
                Checkpoints(output).invalidate(record['invalidated'])
        record.update(applied=True, free_bytes_before=before, free_bytes_after=shutil.disk_usage(root).free)
        record['journal'] = str(destination)
        write_json(destination, record)
        return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-root', type=Path, default=ROOT / 'build/coast')
    parser.add_argument('--kernel-output', type=Path, default=ROOT / 'build')
    parser.add_argument('--component', default='')
    parser.add_argument('--keep', default=None)
    parser.add_argument('--all', action='store_true', dest='all_generated')
    parser.add_argument('--preview', action='store_true')
    parser.add_argument('--json', action='store_true', help='print the applied record as JSON instead of a summary')
    args = parser.parse_args()
    with project_lock():
        from build_progress import activity, readable_bytes
        with activity('Planning cleanup', stream=sys.stderr):
            record = plan(ROOT, args.output_root, args.component, args.all_generated, args.keep, args.kernel_output)
        if not args.preview:
            with activity('Removing generated products', stream=sys.stderr):
                apply(record)
        if args.preview or args.json:
            print(json.dumps(record, indent=2))
        else:
            recovered = max(0, record['free_bytes_after'] - record['free_bytes_before'])
            print('Cleanup complete: ' + str(len(record['paths'])) + ' paths; ' + readable_bytes(recovered) + ' free space gained.')
            print('Retained: ' + (', '.join(record['keep']) or 'protected source/configuration/private state'))
            print('Invalidated: ' + ', '.join(record['invalidated']))
            print('Record: ' + record['journal'])


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error)) from error

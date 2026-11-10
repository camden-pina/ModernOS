"""Checkpointed component execution, dependency invalidation and safe retries."""
import hashlib
import json
from pathlib import Path
import shutil

from build_components import APPS, dependents, graph_for, receipt_path
from build_progress import run_logged
from build_state import Checkpoints, cache_downloads, read_json, write_json
from clean_project import invalidation_paths, protected_paths, remove_containers, select_paths
from source_tree import digest


def normalized_command(argv):
    result = []
    skip = False
    for item in map(str, argv):
        if skip:
            skip = False
        elif item == '--jobs':
            skip = True
        else:
            result.append(item)
    return result


class BuildSession:
    def __init__(self, root, output, snapshot, graph=None):
        self.root, self.output = Path(root), Path(output)
        self.snapshot = snapshot
        self.graph = graph_for(APPS) if graph is None else graph
        self.state = Checkpoints(output)
        self.output.mkdir(parents=True, exist_ok=True)
        write_json(self.output / 'OWNER.json', dict(source_root=str(self.root.resolve())))
        registry = self.root / 'build/build-roots.json'
        roots = read_json(registry, [])
        if str(self.output.resolve()) not in roots:
            write_json(registry, sorted(set(roots + [str(self.output.resolve())])))
        self.logs = self.output / 'logs'
        self.logs.mkdir(exist_ok=True)

    def invalidate(self, name):
        names = dependents({name}, self.graph)
        protected = protected_paths(self.root)
        for path in invalidation_paths(self.output, names):
            if path == self.output / 'PIPELINE.json':
                continue  # This describes the current attempt, not completed output.
            if path.exists() and not any(parent in protected for parent in (path, *path.parents)):
                path.unlink()
        self.state.invalidate(names)

    def reset(self, name, directory):
        if name == 'sources':
            return  # The source synchronizer owns its own resumable phases.
        directory = Path(directory)
        if not directory.exists():
            return
        if not directory.is_relative_to(self.output) or directory == self.output:
            raise ValueError('Refusing to reset an external component: ' + str(directory))
        protected = protected_paths(self.root)
        if any(path == directory or path.is_relative_to(directory) or directory.is_relative_to(path) for path in protected):
            raise ValueError('Component output contains protected sources/selected targets; choose a different output directory: ' + str(directory))
        if name == 'gui-sources':
            remove_containers([self.output])
        cache_downloads(directory, self.root)
        keep = {'sources', 'downloads'} if name == 'electron' else {'toolchains'} if name == 'native' else set()
        if name == 'electron':
            output_protected = protected.copy()
            protected |= {directory / n for n in ('checkout', 'depot_tools', 'SOURCE.json', 'SYNC.json', '.source.lock')}
            for folder in ('Coast-electron', 'Coast-node'):
                path = directory / 'checkout/src/out' / folder
                for disposable in select_paths([path], self.root, protected=output_protected):
                    if disposable.is_dir() and not disposable.is_symlink():
                        shutil.rmtree(disposable)
                    else:
                        disposable.unlink(missing_ok=True)
        for path in select_paths([directory], self.root, keep=keep, protected=protected):
            if path.is_dir() and not path.is_symlink():
                shutil.rmtree(path)
            else:
                path.unlink(missing_ok=True)

    def run(self, name, argv, receipt, verify, *, dependencies=(), restart=False,
            step=None, total=None, detail=None, external=False, extra=None):
        receipt = Path(receipt)
        command = normalized_command(argv)
        builder = next((Path(part) for part in command if part.endswith('.py') and Path(part).is_file()), None)
        identity = dict(argv=command, builder_sha256=digest(builder) if builder else None,
            dependencies={str(path): digest(Path(path)) for path in dependencies},
            files={part: digest(Path(part)) for part in command if Path(part).is_file()}, extra=extra)
        fingerprint = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()
        previous = self.state.get(name)
        changed = previous.get('fingerprint') not in (None, fingerprint)
        verification_failed = False
        if receipt.exists() and not changed:
            try:
                record = verify(receipt)
            except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError):
                if external:
                    raise
                verification_failed = True
            else:
                self.state.set(name, 'complete', fingerprint=fingerprint, receipt=str(receipt))
                print('Verified build component: ' + name, flush=True)
                return record
        if external:
            raise ValueError('Missing or changed externally selected component: ' + str(receipt))
        was_interrupted = previous.get('status') in {'running', 'interrupted', 'failed'}
        inputs_changed = was_interrupted and previous.get('source_snapshot') != self.snapshot
        self.invalidate(name)
        if restart or changed or inputs_changed or (verification_failed and name != 'qualification') or previous.get('status') == 'invalidated':
            self.reset(name, receipt.parent)
        elif name == 'application' and was_interrupted and (not read_json(receipt.parent / 'SOURCE.json') or not read_json(receipt.parent / 'PATCHED.json')):
            # Source setup can stop between workspace renames and its checkpoint.
            self.reset(name, receipt.parent)
        # Only build checkpoints are repaired. package.json, lockfiles and
        # user settings are never interpreted as disposable recovery metadata.
        for marker in ('SOURCE.json', 'CONFIGURE.json', 'CONFIGURATION.json', 'PATCHED.json',
                       'INSTALLED.json', 'PREPARED.json', 'ENVIRONMENT.json'):
            path = receipt.parent / marker
            if path.exists() and not isinstance(read_json(path), dict):
                if name != 'sources':
                    self.reset(name, receipt.parent)
                break
        self.state.set(name, 'running', fingerprint=fingerprint, source_snapshot=self.snapshot, receipt=str(receipt))
        try:
            run_logged(name, argv, cwd=self.root, log_path=self.logs / (('electron-sync' if name == 'sources' else name) + '.log'),
                       step=step, total=total, detail=detail)
            record = verify(receipt)
        except BaseException as error:
            self.state.set(name, 'interrupted' if isinstance(error, KeyboardInterrupt) else 'failed',
                fingerprint=fingerprint, source_snapshot=self.snapshot, receipt=str(receipt), error=str(error)[-1000:])
            raise
        self.state.set(name, 'complete', fingerprint=fingerprint, receipt=str(receipt))
        return record

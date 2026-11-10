"""Durable build checkpoints and the project-wide build/cleanup lock."""
from contextlib import contextmanager
import errno
import fcntl
import json
import os
from pathlib import Path
import secrets
import re
import subprocess
import sys
import tempfile
import time
import shutil

ROOT = Path(__file__).resolve().parents[1]


def atomic_write(path, text, encoding='utf-8'):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    pending = None
    try:
        with tempfile.NamedTemporaryFile(prefix='.' + path.name + '.', suffix='.tmp',
                dir=path.parent, delete=False) as stream:
            pending = Path(stream.name)
            stream.write(text.encode(encoding) if isinstance(text, str) else text)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(pending, path)
        directory = os.open(path.parent, os.O_RDONLY)
        try:
            try:
                os.fsync(directory)
            except OSError as error:
                if error.errno not in (errno.EINVAL, errno.ENOTSUP):
                    raise
        finally:
            os.close(directory)
    finally:
        if pending is not None:
            pending.unlink(missing_ok=True)


def write_json(path, value):
    atomic_write(path, json.dumps(value, indent=2, sort_keys=True) + '\n')


def atomic_copy(source, destination):
    destination = Path(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(prefix='.' + destination.name + '.', dir=destination.parent, delete=False) as stream:
        temporary = Path(stream.name)
        try:
            with Path(source).open('rb') as original:
                shutil.copyfileobj(original, stream)
            stream.flush()
            os.fsync(stream.fileno())
            os.replace(temporary, destination)
        finally:
            temporary.unlink(missing_ok=True)


def read_json(path, default=None):
    try:
        return json.loads(Path(path).read_text())
    except (FileNotFoundError, json.JSONDecodeError, UnicodeError):
        return default


def cache_downloads(directory, root=ROOT):
    """Move verified-on-reuse archives out of a stage before restarting it."""
    import hashlib
    directory, root = Path(directory), Path(root)
    extensions = ('.tar.gz', '.tar.xz', '.tar.bz2', '.tar.zst', '.tgz', '.zip', '.deb', '.dsc', '.whl')
    areas = [directory, directory / 'vendor', directory / 'downloads']
    for area in areas:
        if not area.is_dir():
            continue
        for source in area.iterdir():
            if source.is_file() and not source.is_symlink() and source.name.endswith(extensions):
                with source.open('rb') as stream:
                    checksum = hashlib.file_digest(stream, 'sha256').hexdigest()
                destination = root / 'build/downloads' / checksum / source.name
                destination.parent.mkdir(parents=True, exist_ok=True)
                if destination.exists():
                    with destination.open('rb') as stream:
                        if hashlib.file_digest(stream, 'sha256').hexdigest() != checksum:
                            destination.unlink()
                            shutil.move(source, destination)
                            continue
                    source.unlink()
                else:
                    shutil.move(source, destination)


def _process(pid):
    try:
        fields = (Path('/proc') / str(pid) / 'stat').read_text().rsplit(')', 1)[1].split()
        return int(fields[1]), fields[19]
    except (OSError, ValueError, IndexError):
        return None


def owns_lock(root=ROOT):
    root = Path(root).resolve()
    token = os.environ.get('COAST_PROJECT_LOCK_TOKEN')
    if not token:
        return False
    owner = read_json(root / 'build/.lifecycle-owner.json', {})
    if owner.get('token') != token or owner.get('root') != str(root):
        return False
    process = _process(owner.get('pid', -1))
    if not process:
        # Native Coast builds do not expose every Linux /proc field. The
        # unguessable inherited token still distinguishes a nested build.
        try:
            os.kill(owner.get('pid', -1), 0)
            return owner.get('start') == ''
        except OSError:
            return False
    if process[1] != owner.get('start'):
        return False
    pid = os.getpid()
    while pid > 1:
        if pid == owner['pid']:
            return True
        state = _process(pid)
        if state is None:
            break
        pid = state[0]
    return False


@contextmanager
def project_lock(root=ROOT):
    root = Path(root).resolve()
    if owns_lock(root):
        yield
        return
    build = root / 'build'
    build.mkdir(parents=True, exist_ok=True)
    with (build / '.lifecycle.lock').open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise ValueError('Another build, VM or cleanup owns this project. Stop it before continuing.') from None
        previous = os.environ.get('COAST_PROJECT_LOCK_TOKEN')
        token = secrets.token_hex(24)
        owner_path = build / '.lifecycle-owner.json'
        process = _process(os.getpid())
        write_json(owner_path, dict(root=str(root), pid=os.getpid(), start=process[1] if process else '', token=token))
        os.environ['COAST_PROJECT_LOCK_TOKEN'] = token
        try:
            yield
        finally:
            if read_json(owner_path, {}).get('token') == token:
                owner_path.unlink(missing_ok=True)
            if previous is None:
                os.environ.pop('COAST_PROJECT_LOCK_TOKEN', None)
            else:
                os.environ['COAST_PROJECT_LOCK_TOKEN'] = previous


class Checkpoints:
    def __init__(self, output):
        self.path = Path(output) / 'STATE.json'
        self.record = read_json(self.path, dict(format_version=1, components={}))
        if not isinstance(self.record, dict) or not isinstance(self.record.get('components'), dict):
            self.record = dict(format_version=1, components={})

    def set(self, component, status, **values):
        self.record['components'][component] = dict(status=status, updated=time.time(), **values)
        write_json(self.path, self.record)

    def get(self, component):
        return self.record['components'].get(component, {})

    def invalidate(self, names):
        for name in names:
            previous = self.record['components'].get(name, {})
            self.record['components'][name] = dict(previous, status='invalidated', updated=time.time())
        write_json(self.path, self.record)


def register_kernel(output, root=ROOT):
    root, output = Path(root).resolve(), Path(output).resolve()
    if output != root:
        output.mkdir(parents=True, exist_ok=True)
        marker = output / '.coast-kernel.json'
        previous = read_json(marker, {})
        if previous and previous.get('source_root') != str(root):
            raise ValueError('Kernel output belongs to a different source tree: ' + str(output))
        write_json(marker, dict(source_root=str(root)))
    registry = root / 'build/kernel-roots.json'
    write_json(registry, sorted(set(read_json(registry, []) + [str(output)])))


def main():
    command, *arguments = sys.argv[1:]
    if command == 'held':
        print('yes' if owns_lock() else 'no')
    elif command == 'register-kernel':
        with project_lock(ROOT):
            register_kernel(arguments[0])
    elif command == 'guard':
        if arguments[:1] == ['--']:
            arguments = arguments[1:]
        if not arguments:
            raise ValueError('guard requires a command')
        with project_lock(ROOT):
            try:
                descriptors = []
                match = re.search(r'--jobserver-(?:auth|fds)=(\d+),(\d+)', os.environ.get('MAKEFLAGS', ''))
                if match:
                    for descriptor in map(int, match.groups()):
                        try:
                            os.fstat(descriptor)
                            descriptors.append(descriptor)
                        except OSError:
                            pass
                process = subprocess.Popen(arguments, pass_fds=tuple(descriptors))
                try:
                    status = process.wait()
                except KeyboardInterrupt:
                    # Keep cleanup excluded until nested Make has stopped its workers.
                    import signal
                    previous_handler = signal.signal(signal.SIGINT, signal.SIG_IGN)
                    try:
                        # The terminal already signals Make and its recipe.
                        # Sending another SIGINT can interrupt their cleanup.
                        try:
                            process.wait(timeout=30)
                        except subprocess.TimeoutExpired:
                            process.terminate()
                            process.wait()
                    finally:
                        signal.signal(signal.SIGINT, previous_handler)
                    status = 130
                raise SystemExit(status if status >= 0 else 128 - status)
            except KeyboardInterrupt:
                raise SystemExit(130) from None
    else:
        raise ValueError('Unknown build-state command: ' + command)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError) as error:
        raise SystemExit(str(error)) from error

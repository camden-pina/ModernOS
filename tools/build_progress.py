"""Live build progress while retaining complete, unmodified component logs."""
import codecs
import os
from pathlib import Path
import re
import selectors
import shutil
import signal
import subprocess
import sys
import time


ANSI = re.compile(r'\x1b\[[0-?]*[ -/]*[@-~]')
COUNTER = re.compile(r'^\[(\d+)/(\d+)\]')
PERCENT = re.compile(r'^\[\s*(\d+)%\]|(?:Receiving objects|Resolving deltas|Counting objects|Compressing objects):\s*(\d+)%')


def readable_bytes(size):
    for unit in ('B', 'KiB', 'MiB', 'GiB', 'TiB'):
        if size < 1024 or unit == 'TiB':
            return f'{size:.1f} {unit}'
        size /= 1024


class SourceSyncStatus:
    """Observe downloads without opening, modifying or locking Git objects."""

    def __init__(self, checkout, repositories):
        self.checkout = Path(checkout)
        self.repositories = repositories
        self.pins = {self.checkout / record['path']: record['commit'] for record in repositories}
        self.previous = None
        self.message = ''

    def __call__(self, force=False):
        now = time.monotonic()
        if not force and self.previous and now - self.previous[0] < 2:
            return self.message
        size = complete = 0
        roots = list(self.pins)
        roots.extend(self.checkout.glob('_gclient_*'))
        for root in roots:
            try:
                if root in self.pins and (root / '.git/HEAD').read_text().strip() == self.pins[root]:
                    complete += 1
            except OSError:
                pass
            try:
                for pack in (root / '.git/objects/pack').glob('*'):
                    if pack.suffix == '.pack' or pack.name.startswith('tmp_pack_'):
                        try:
                            size += pack.stat().st_size
                        except OSError:
                            pass
            except OSError:
                pass
        rate = ''
        if self.previous and size > self.previous[1]:
            rate = ', ' + readable_bytes((size - self.previous[1]) / (now - self.previous[0])) + '/s'
        self.previous = now, size
        self.message = f'sources {complete}/{len(self.repositories)}, Git {readable_bytes(size)}{rate}'
        return self.message


class Progress:
    def __init__(self, name, step=None, total=None, stream=None, interval=None):
        self.name = name
        self.label = f'Stage {step}/{total}: {name}' if step and total else name
        self.stream = stream if stream is not None else sys.stdout
        self.tty = self.stream.isatty()
        self.interval = interval if interval is not None else (0.5 if self.tty else 30)
        self.start = time.monotonic()
        self.last = float('-inf')
        self.latest = ''
        self.fraction = None

    def feed(self, line):
        line = ANSI.sub('', line).strip()
        line = ''.join(c if c.isprintable() else ' ' for c in line)
        if not line:
            return
        self.latest = line
        counter = COUNTER.search(line)
        percent = PERCENT.search(line)
        if counter and int(counter[2]) and int(counter[1]) <= int(counter[2]):
            self.fraction = int(counter[1]) / int(counter[2])
        elif percent:
            self.fraction = min(1, int(percent[1] or percent[2]) / 100)
        elif self.fraction == 1:
            # A finished Git/CMake subcommand is followed by more preparation.
            # Its 100% must not persist throughout the next, unmeasured task.
            self.fraction = None

    def render(self, detail='', final=None, force=False):
        now = time.monotonic()
        if not force and now - self.last < self.interval:
            return
        self.last = now
        elapsed = int(now - self.start)
        clock = f'{elapsed // 3600:02}:{elapsed // 60 % 60:02}:{elapsed % 60:02}'
        width = 16
        if final == 'done':
            bar, measure = '=' * width, 'done'
        elif final:
            bar, measure = '!' * width, final
        elif self.fraction is not None:
            filled = int(self.fraction * width)
            bar, measure = '=' * filled + '.' * (width - filled), f'{self.fraction:.0%}'
        else:
            position = int((now - self.start) * 2) % (width - 2)
            bar, measure = '.' * position + '===' + '.' * (width - position - 3), 'working'
        # The stage heading is printed once above a terminal's live line.
        # Keep its limited width available for download sizes and tool output.
        prefix = '' if self.tty else self.label + ' '
        line = f'{prefix}[{bar}] {measure} {clock}'
        if detail:
            line += ' | ' + detail
        if self.latest:
            line += ' | ' + self.latest
        if self.tty:
            line = line[:max(1, shutil.get_terminal_size((100, 24)).columns - 1)]
            self.stream.write('\r\x1b[K' + line + ('\n' if final else ''))
        else:
            self.stream.write(line + '\n')
        self.stream.flush()


def activity(name, stream=None):
    """Heartbeat for local operations whose tools do not report a total."""
    from contextlib import contextmanager
    import threading

    @contextmanager
    def showing():
        progress = Progress(name, stream=stream)
        stopped = threading.Event()
        def update():
            while not stopped.wait(0.5):
                progress.render()
        progress.render(force=True)
        worker = threading.Thread(target=update, daemon=True)
        worker.start()
        final = 'done'
        try:
            yield progress
        except BaseException:
            final = 'interrupted'
            raise
        finally:
            stopped.set()
            worker.join()
            progress.render(final=final, force=True)
    return showing()


def stop_group(process, grace=2):
    """Finish owned-child teardown even if Ctrl+C is pressed again."""
    previous = signal.signal(signal.SIGINT, signal.SIG_IGN)
    try:
        _stop_group(process, grace)
    finally:
        signal.signal(signal.SIGINT, previous)


def _stop_group(process, grace=2):
    """Forward Ctrl+C to this stage and reap any surviving stage children."""
    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGKILL):
        try:
            os.killpg(process.pid, sig)
        except ProcessLookupError:
            break
        until = time.monotonic() + grace
        while time.monotonic() < until:
            process.poll()
            try:
                os.killpg(process.pid, 0)
            except ProcessLookupError:
                break
            time.sleep(0.05)
        else:
            continue
        break
    process.wait()


def run_logged(name, argv, *, cwd, log_path, step=None, total=None, detail=None,
               stream=None, interval=None):
    """Show real tool counters, or activity/elapsed time when totals are unknown."""
    progress = Progress(name, step, total, stream, interval)
    log_path = Path(log_path)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    print(f'{progress.label} (log: {log_path})', file=progress.stream, flush=True)
    decoder = codecs.getincrementaldecoder('utf-8')('replace')
    pending = ''
    environment = {**os.environ, 'PYTHONUNBUFFERED': '1'}
    with log_path.open('ab') as log, selectors.DefaultSelector() as events:
        process = subprocess.Popen(list(map(str, argv)), cwd=cwd, stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=environment, start_new_session=True)
        try:
            events.register(process.stdout, selectors.EVENT_READ)
            while events.get_map() or process.poll() is None:
                for key, _ in events.select(timeout=0.2):
                    data = os.read(key.fd, 65536)
                    if not data:
                        events.unregister(key.fileobj)
                        if pending:
                            progress.feed(pending)
                        continue
                    log.write(data)
                    log.flush()
                    pending += decoder.decode(data)
                    lines = re.split(r'[\r\n]', pending)
                    pending = lines.pop()[-4096:]
                    for line in lines:
                        progress.feed(line)
                progress.render(detail() if detail else '')
            code = process.wait()
        except BaseException:
            stop_group(process)
            remaining = process.stdout.read()
            if remaining:
                log.write(remaining)
            progress.render(final='interrupted', force=True)
            raise
        finally:
            process.stdout.close()
        final_detail = detail(force=True) if isinstance(detail, SourceSyncStatus) else detail() if detail else ''
        progress.render(final_detail, final='failed' if code else 'done', force=True)
        if code:
            print(f'Build failed; full output: {log_path}', file=progress.stream, flush=True)
            raise subprocess.CalledProcessError(code, argv)

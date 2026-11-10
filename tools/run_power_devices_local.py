#!/usr/bin/env python3
"""Reserve and supervise one local, unchanged-ISO power-device case.

Printing is the default. --execute requires --exclusive-ready and an explicit
MemAvailable floor of at least 4 GiB. One direct child gets a fresh session and
an inherited, peer-credential-checked admission socket. No cloud pilot, file-RAM
wrapper, host disk, security setting or network configuration is changed.
"""
import argparse
import contextlib
import ctypes
import json
import os
from pathlib import Path
import select
import signal
import socket
import stat
import struct
import subprocess
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
MIN_AVAILABLE_KIB = 4 * 1024 * 1024
LOCAL_FD = 'COAST_POWER_DEVICE_LOCAL_ADMISSION_FD'
LOCAL_TOKEN_PREFIX = 'power-devices-local:'
TERM_GRACE = 2.0
KILL_GRACE = 1.0


def process_identity(pid):
    try:
        fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
        return dict(pid=pid, state=fields[0], parent=int(fields[1]),
                    group=int(fields[2]), session=int(fields[3]), start=int(fields[19]))
    except (FileNotFoundError, ProcessLookupError):
        return None


def available_kib():
    for line in Path('/proc/meminfo').read_text().splitlines():
        if line.startswith('MemAvailable:'):
            return int(line.split()[1])
    raise RuntimeError('Cannot establish MemAvailable; local execution refused')


def check_memory(floor):
    if not isinstance(floor, int) or floor < MIN_AVAILABLE_KIB:
        raise RuntimeError('Local admission requires a memory floor of at least 4194304 KiB')
    available = available_kib()
    if available < floor:
        raise RuntimeError(f'MemAvailable={available} KiB is below required {floor} KiB')
    return available


def no_recovery_blocker(root):
    path = root / 'build/release-file-ram-recovery-required.json'
    if path.exists() or path.is_symlink():
        raise RuntimeError('Unresolved owned-process cleanup blocks local execution')


def write_json(path, value):
    with Path(path).open('w') as output:
        json.dump(value, output, indent=2, sort_keys=True)
        output.write('\n'); output.flush(); os.fsync(output.fileno())


def read_slot(root):
    path = root / 'build/browser-guests-active'
    try:
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
        with os.fdopen(fd, 'rb') as source:
            info = os.fstat(source.fileno())
            if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid():
                raise RuntimeError('Local guest marker is not an owned regular file')
            data = source.read(256)
    except OSError as error:
        raise RuntimeError('Local guest marker is missing or cannot be verified') from error
    return dict(device=info.st_dev, inode=info.st_ino, token=data.decode().strip())


class Slot:
    """O_EXCL acquisition; release only the exact inode and token we created."""
    def __init__(self, root):
        self.root = root
        self.path = root / 'build/browser-guests-active'
        self.token = LOCAL_TOKEN_PREFIX + uuid.uuid4().hex
        self.identity = None

    def acquire(self):
        no_recovery_blocker(self.root)
        fd = os.open(self.path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        info = os.fstat(fd)
        self.identity = dict(device=info.st_dev, inode=info.st_ino, token=self.token)
        try:
            with os.fdopen(fd, 'w') as output:
                output.write(self.token + '\n'); output.flush(); os.fsync(output.fileno())
            no_recovery_blocker(self.root)
        except BaseException:
            self.release()
            raise
        return self.identity

    def verify(self):
        if self.identity is None or read_slot(self.root) != self.identity:
            raise RuntimeError('Local guest marker ownership changed; refusing to remove it')

    def release(self):
        self.verify()
        self.path.unlink()
        self.identity = None


def verify_lease(lease, root=ROOT):
    """Recheck direct parent, fresh session, reservation and RAM before a boot."""
    if lease.get('protocol') != 1 or lease.get('root') != str(root.resolve()):
        raise RuntimeError('Local admission protocol/root mismatch')
    current = process_identity(os.getpid())
    supervisor = process_identity(os.getppid())
    expected_child = lease.get('child', {})
    expected_parent = lease.get('supervisor', {})
    if (not current or not supervisor or current['group'] != current['pid'] or
            current['session'] != current['pid'] or
            any(current.get(key) != expected_child.get(key) for key in ('pid', 'start', 'group', 'session', 'parent')) or
            any(supervisor.get(key) != expected_parent.get(key) for key in ('pid', 'start')) or
            current['parent'] != supervisor['pid'] or supervisor['state'] == 'Z'):
        raise RuntimeError('Local execution requires its direct supervisor and fresh owned session')
    expected_slot = lease.get('slot', {})
    if (not expected_slot.get('token', '').startswith(LOCAL_TOKEN_PREFIX) or
            read_slot(root) != expected_slot):
        raise RuntimeError('Actual local reservation marker/token mismatch')
    no_recovery_blocker(root)
    return check_memory(lease.get('min_available_kib'))


def admit_child(environment=None, root=ROOT):
    """Consume a private inherited socket; environment strings alone cannot admit."""
    environment = os.environ if environment is None else environment
    number = environment.get(LOCAL_FD, '')
    if not number.isdecimal() or int(number) < 3:
        raise RuntimeError('Local execution requires an inherited supervisor admission socket')
    try:
        connection = socket.socket(fileno=int(number))
    except OSError as error:
        raise RuntimeError('Invalid local supervisor admission socket') from error
    with connection:
        if connection.family != socket.AF_UNIX or connection.type != socket.SOCK_STREAM:
            raise RuntimeError('Local admission requires a private Unix stream socket')
        peer, uid, _ = struct.unpack('3i', connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
        if peer != os.getppid() or uid != os.getuid():
            raise RuntimeError('Local admission socket is not owned by the direct parent')
        connection.setblocking(False)
        payload = bytearray(); deadline = time.monotonic() + 10
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([connection], [], [], remaining)[0]:
                raise RuntimeError('Local supervisor admission timed out')
            block = connection.recv(8193 - len(payload))
            if not block:
                break
            payload.extend(block)
            if len(payload) > 8192:
                raise RuntimeError('Oversized local supervisor admission refused')
    try:
        lease = json.loads(payload)
    except (ValueError, UnicodeDecodeError) as error:
        raise RuntimeError('Invalid local supervisor admission payload') from error
    if not isinstance(lease, dict) or lease.get('supervisor', {}).get('pid') != peer:
        raise RuntimeError('Local admission peer identity mismatch')
    available = verify_lease(lease, root)
    return dict(lease, mode='local', available_kib_at_admission=available)


@contextlib.contextmanager
def subreaper():
    if not hasattr(os, 'pidfd_open') or not hasattr(signal, 'pidfd_send_signal'):
        raise RuntimeError('Local execution requires Linux pidfds')
    libc = ctypes.CDLL(None, use_errno=True); previous = ctypes.c_int()
    if libc.prctl(37, ctypes.byref(previous), 0, 0, 0) or libc.prctl(36, 1, 0, 0, 0):
        raise RuntimeError('Cannot establish local child reaping')
    try:
        yield
    finally:
        libc.prctl(36, previous.value, 0, 0, 0)


def processes():
    result = []
    for path in Path('/proc').iterdir():
        if path.name.isdecimal():
            current = process_identity(int(path.name))
            if current:
                result.append(current)
    return result


class OwnedProcesses:
    """Track this fresh session and descendants adopted by this subreaper only."""
    def __init__(self):
        self.supervisor = os.getpid()
        self.before = {(p['pid'], p['start']) for p in processes() if p['parent'] == self.supervisor}
        self.known = {}
        self.leader = None

    def register(self, identity):
        self.leader = identity
        self.known[identity['pid']] = identity['start']

    def observe(self):
        snapshot = processes()
        changed = True
        while changed:
            changed = False
            for current in snapshot:
                key = (current['pid'], current['start'])
                if current['pid'] == self.supervisor or key in self.before:
                    continue
                owned = (self.known.get(current['pid']) == current['start'] or
                         (current['group'] == self.leader['pid'] and current['session'] == self.leader['pid'] and current['start'] >= self.leader['start']) or
                         (current['parent'] in self.known and any(p['pid'] == current['parent'] and self.known[p['pid']] == p['start'] for p in snapshot)) or
                         current['parent'] == self.supervisor)
                if owned and self.known.get(current['pid']) != current['start']:
                    self.known[current['pid']] = current['start']; changed = True
        return [p for p in snapshot if self.known.get(p['pid']) == p['start']]

    def reap(self, child):
        child.poll()
        # Never wait for another pre-existing direct child of the supervisor.
        for pid, start in list(self.known.items()):
            current = process_identity(pid)
            if current and current['start'] == start and current['parent'] == self.supervisor:
                try:
                    os.waitpid(pid, os.WNOHANG)
                except ChildProcessError:
                    pass

    def cleanup(self, child):
        for number, grace in ((signal.SIGTERM, TERM_GRACE), (signal.SIGKILL, KILL_GRACE)):
            deadline = time.monotonic() + grace
            while True:
                members = self.observe()
                for current in members:
                    if current['state'] == 'Z':
                        continue
                    try:
                        fd = os.pidfd_open(current['pid'])
                        try:
                            after = process_identity(current['pid'])
                            if after and (after['start'], after['parent'], after['group'], after['session']) == (current['start'], current['parent'], current['group'], current['session']):
                                signal.pidfd_send_signal(fd, number)
                        finally:
                            os.close(fd)
                    except ProcessLookupError:
                        pass
                self.reap(child)
                remaining = self.observe()
                if not remaining:
                    return []
                if time.monotonic() >= deadline:
                    break
                time.sleep(.02)
        return self.observe()


def record_blocker(root, folder, members, error=None):
    value = dict(result='cleanup-blocked', run=str(folder), supervisor=process_identity(os.getpid()),
                 members=members, error=error, manual_recovery_required=True,
                 instruction='Hold compiler/guest admission until owned identities are verified dead; never infer ownership or auto-delete this blocker')
    write_json(folder / 'cleanup-blocked.json', value)
    path = root / 'build/release-file-ram-recovery-required.json'
    try:
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    except FileExistsError:
        return
    with os.fdopen(fd, 'w') as output:
        json.dump(value, output, indent=2); output.write('\n'); output.flush(); os.fsync(output.fileno())


class Interrupted(KeyboardInterrupt):
    pass


def supervise(command, *, root, folder, floor, timeout):
    """Internal supervisor; the CLI supplies only the fixed power-device harness."""
    if timeout <= 0:
        raise ValueError('Supervisor timeout must be positive')
    no_recovery_blocker(root)
    available = check_memory(floor)
    if not (root / 'build').is_dir():
        raise RuntimeError('The checkout build directory must already exist')
    folder.mkdir(parents=True, exist_ok=False)
    slot = Slot(root); child = None; tracker = None; old = {}; cancelled = []
    report = dict(result='failed', command=command, min_available_kib=floor,
                  available_kib_before_launch=available, timeout_seconds=timeout,
                  started_ns=time.time_ns(), physical_hardware_validated=False)
    with subreaper():
        tracker = OwnedProcesses()
        try:
            def interrupted(number, frame):
                # Defer the exception until Popen has returned and the child
                # identity is recorded; never lose a child in an assignment race.
                cancelled.append(number)
            def check_interrupt():
                if cancelled:
                    raise Interrupted('Interrupted with signal ' + str(cancelled[0]))
            for sig in (signal.SIGINT, signal.SIGTERM):
                old[sig] = signal.signal(sig, interrupted)
            slot.acquire()
            report['slot_acquired'] = True
            report['slot'] = slot.identity
            check_interrupt()
            parent, admission = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                environment = dict(os.environ)
                for name in list(environment):
                    if name.startswith('COAST_QEMU_FILE_RAM') or name == LOCAL_FD:
                        environment.pop(name)
                environment[LOCAL_FD] = str(admission.fileno())
                slot.verify(); no_recovery_blocker(root)
                report['available_kib_at_launch'] = check_memory(floor)
                with (folder / 'harness.log').open('wb') as output:
                    child = subprocess.Popen(command, cwd=root, stdin=subprocess.DEVNULL,
                                             stdout=output, stderr=subprocess.STDOUT,
                                             env=environment, pass_fds=(admission.fileno(),), start_new_session=True)
                    admission.close()
                    current = process_identity(child.pid)
                    if (not current or current['parent'] != os.getpid() or
                            current['group'] != child.pid or current['session'] != child.pid):
                        raise RuntimeError('Cannot verify the fresh direct child session')
                    tracker.register(current)
                    fd = os.pidfd_open(child.pid)
                    try:
                        check_interrupt()
                        lease = dict(protocol=1, root=str(root.resolve()), child=current,
                                     supervisor=process_identity(os.getpid()), slot=slot.identity,
                                     min_available_kib=floor)
                        report['lease'] = lease
                        parent.sendall(json.dumps(lease).encode()); parent.shutdown(socket.SHUT_WR); parent.close()
                        deadline = time.monotonic() + timeout
                        while child.poll() is None:
                            check_interrupt()
                            tracker.observe()
                            remaining = deadline - time.monotonic()
                            if remaining <= 0:
                                report['timed_out'] = True
                                raise RuntimeError('Local power-device supervisor timed out')
                            select.select([fd], [], [], min(.1, remaining))
                        report['child_returncode'] = child.returncode
                        check_interrupt()
                        if child.returncode:
                            raise RuntimeError(f'Power-device harness exited {child.returncode}')
                        report['result'] = 'passed'
                    finally:
                        os.close(fd)
            finally:
                admission.close(); parent.close()
        except BaseException as error:
            report['result'] = 'failed'; report['error'] = str(error)
        finally:
            # Ignore further interrupts until verified cleanup and marker handling finish.
            for sig in old:
                signal.signal(sig, signal.SIG_IGN)
            remaining = []
            try:
                if child is not None:
                    if tracker.leader is None:
                        current = process_identity(child.pid)
                        if not current or current['parent'] != os.getpid():
                            raise RuntimeError('Child identity unconfirmed during cleanup')
                        tracker.register(current)
                    remaining = tracker.cleanup(child)
                if remaining:
                    raise RuntimeError('Owned descendants remain; reservation retained for manual recovery')
                acquired = slot.identity is not None
                if acquired:
                    no_recovery_blocker(root)
                    slot.release()
                report['cleanup_verified'] = True; report['marker_released'] = acquired
            except BaseException as error:
                report['result'] = 'failed'; report['cleanup_error'] = str(error)
                report['cleanup_verified'] = False; report['marker_released'] = False
                # A failed acquisition never owns the pre-existing marker.
                if slot.identity is not None:
                    if tracker.leader is not None:
                        remaining = tracker.observe()
                    record_blocker(root, folder, remaining, str(error))
            finally:
                for sig, handler in old.items():
                    signal.signal(sig, handler)
            report['ended_ns'] = time.time_ns()
            if folder.is_dir():
                write_json(folder / 'report.json', report)
    return report


def arguments(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ('iso', 'kernel', 'shell', 'gui'):
        parser.add_argument('--' + key, type=Path, required=True)
    parser.add_argument('--kind', choices=('poweroff', 'reboot'), required=True)
    parser.add_argument('--memory', default='2G')
    parser.add_argument('--timeout', type=int, default=60, help='Guest terminal timeout')
    parser.add_argument('--supervisor-timeout', type=int, default=900)
    parser.add_argument('--execute', action='store_true')
    parser.add_argument('--exclusive-ready', action='store_true', help='Attest all competing real builder/guest work has exited')
    parser.add_argument('--min-available-kib', type=int)
    args = parser.parse_args(argv)
    if args.memory != '2G' or min(args.timeout, args.supervisor_timeout) <= 0:
        parser.error('Exactly 2G RAM and positive timeouts are required')
    if args.min_available_kib is not None and args.min_available_kib < MIN_AVAILABLE_KIB:
        parser.error('--min-available-kib must be at least 4194304 (4 GiB)')
    if args.execute and not (args.exclusive_ready and args.min_available_kib):
        parser.error('--execute requires --exclusive-ready and --min-available-kib')
    return args


def main(argv=None):
    args = arguments(argv)
    command = [sys.executable, '-B', '-u', str(ROOT / 'tools/test_power_devices.py'),
               '--execute', '--admission', 'local']
    for key in ('iso', 'kernel', 'shell', 'gui', 'kind', 'memory', 'timeout'):
        value = getattr(args, key)
        if isinstance(value, Path):
            value = value.expanduser().absolute()
        command += ['--' + key, str(value)]
    if not args.execute:
        print(json.dumps(dict(result='not-run', command=command,
                              marker=str(ROOT / 'build/browser-guests-active'),
                              min_available_kib=args.min_available_kib,
                              supervisor_timeout=args.supervisor_timeout), indent=2))
        return 0
    folder = ROOT / 'build/power-device-supervision' / (args.kind + '-' + str(time.time_ns()) + '-' + uuid.uuid4().hex[:8])
    print('Local power-device supervisor report=' + str(folder / 'report.json') +
          ' log=' + str(folder / 'harness.log'), flush=True)
    report = supervise(command, root=ROOT, folder=folder,
                       floor=args.min_available_kib, timeout=args.supervisor_timeout)
    print('LOCAL POWER DEVICES ' + report['result'].upper() + ' report=' + str(folder / 'report.json'), flush=True)
    return 0 if report['result'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())

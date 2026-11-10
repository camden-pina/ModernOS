#!/usr/bin/env python3
"""Exercise live logs, meaningful counters, failures and interrupted child trees."""
import io
import os
from pathlib import Path
import shlex
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

from build_progress import Progress, SourceSyncStatus, run_logged

ROOT = Path(__file__).resolve().parents[1]


class Terminal(io.StringIO):
    def isatty(self):
        return True


class ProgressTests(unittest.TestCase):
    def test_known_tool_counts_and_indeterminate_activity(self):
        output = Terminal()
        progress = Progress('compiler', 3, 25, output, interval=0)
        progress.render(force=True)
        self.assertIn('working', output.getvalue())
        self.assertNotIn('0%', output.getvalue())
        progress.feed('[50/200] CXX object')
        progress.render(force=True)
        self.assertIn('25%', output.getvalue())
        progress.feed('Receiving objects: 60% (60/100)')
        progress.render(force=True)
        self.assertIn('60%', output.getvalue())
        progress.feed('Receiving objects: 100% (100/100)')
        progress.feed('Preparing the next dependency')
        self.assertIsNone(progress.fraction)
        progress.feed('[ 75%] Linking program')
        progress.render(final='done', force=True)
        self.assertEqual(progress.label, 'Stage 3/25: compiler')
        self.assertTrue(output.getvalue().endswith('\n'))

    def test_redirected_output_gets_heartbeat_without_terminal_escapes(self):
        output = io.StringIO()
        progress = Progress('download', stream=output, interval=0)
        progress.feed('\x1b[31mreceived\x1b[0m\x00 data')
        progress.render('Git 1.0 GiB', force=True)
        self.assertNotIn('\x1b', output.getvalue())
        self.assertNotIn('\x00', output.getvalue())
        self.assertIn('received  data', output.getvalue())
        self.assertIn('Git 1.0 GiB', output.getvalue())

    def test_terminal_keeps_download_size_visible_at_80_columns(self):
        output = Terminal()
        progress = Progress('electron-sync', 1, 26, output)
        with patch('build_progress.shutil.get_terminal_size', return_value=os.terminal_size((80, 24))):
            progress.render('sources 12/170, Git 1.4 GiB, 25.0 MiB/s', force=True)
        line = output.getvalue().removeprefix('\r\x1b[K')
        self.assertLessEqual(len(line), 79)
        self.assertIn('Git 1.4 GiB', line)

    def test_raw_log_preserves_stdout_stderr_and_carriage_return_progress(self):
        with tempfile.TemporaryDirectory() as temporary:
            log = Path(temporary) / 'stage.log'
            output = io.StringIO()
            source = ('import os; os.write(1,b"[1/2] compile\\r[2/2] link\\n"); '
                      'os.write(2,b"diagnostic\\n")')
            log.write_bytes(b'earlier invocation\n')
            run_logged('fixture', [sys.executable, '-c', source], cwd=temporary,
                       log_path=log, stream=output, interval=0)
            self.assertEqual(log.read_bytes(), b'earlier invocation\n[1/2] compile\r[2/2] link\ndiagnostic\n')
            self.assertIn('done', output.getvalue())
            self.assertIn(str(log), output.getvalue())

    def test_failure_keeps_error_log_and_exit_status(self):
        with tempfile.TemporaryDirectory() as temporary:
            log = Path(temporary) / 'stage.log'
            output = io.StringIO()
            with self.assertRaises(subprocess.CalledProcessError) as error:
                run_logged('fixture', [sys.executable, '-c', 'print("actual failure");raise SystemExit(7)'],
                           cwd=temporary, log_path=log, stream=output)
            self.assertEqual(error.exception.returncode, 7)
            self.assertIn(b'actual failure', log.read_bytes())
            self.assertIn('failed', output.getvalue())

    def test_source_status_counts_locked_repositories_and_growing_packs(self):
        with tempfile.TemporaryDirectory() as temporary:
            checkout = Path(temporary)
            git = checkout / 'src/.git'
            packs = git / 'objects/pack'
            packs.mkdir(parents=True)
            pin = 'a' * 40
            (git / 'HEAD').write_text(pin + '\n')
            pack = packs / 'tmp_pack_fixture'
            pack.write_bytes(b'X' * 1024)
            watcher = SourceSyncStatus(checkout, [dict(path='src', commit=pin)])
            with patch('build_progress.time.monotonic', side_effect=[10, 13]):
                self.assertIn('sources 1/1, Git 1.0 KiB', watcher())
                pack.write_bytes(b'X' * 4096)
                message = watcher()
            self.assertIn('Git 4.0 KiB', message)
            self.assertIn('1.0 KiB/s', message)
            self.assertEqual((git / 'HEAD').read_text(), pin + '\n')

    def test_source_completion_is_refreshed_even_inside_throttle_interval(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            git = root / 'src/.git'
            git.mkdir(parents=True)
            pin = 'a' * 40
            watcher = SourceSyncStatus(root, [dict(path='src', commit=pin)])
            with patch('build_progress.time.monotonic', side_effect=[10, 10.1, 10.2]):
                self.assertIn('sources 0/1', watcher())
                (git / 'HEAD').write_text(pin + '\n')
                self.assertIn('sources 0/1', watcher())
                self.assertIn('sources 1/1', watcher(force=True))

    def test_ctrl_c_stops_stage_and_its_child(self):
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            ready = work / 'ready'
            child = ('import pathlib,signal,time; signal.signal(signal.SIGINT,signal.SIG_IGN); '
                     'signal.signal(signal.SIGTERM,signal.SIG_IGN); '
                     'pathlib.Path("ready").write_text(str(__import__("os").getpid())); time.sleep(60)')
            stage = ('import subprocess,sys,time; '
                     f'subprocess.Popen([sys.executable,"-c",{child!r}]); time.sleep(60)')
            driver = ('import sys; from build_progress import run_logged; '
                      f'run_logged("fixture",[sys.executable,"-c",{stage!r}],'
                      f'cwd={str(work)!r},log_path={str(work / "stage.log")!r})')
            (work / 'driver.py').write_text(driver)
            (work / 'Makefile').write_text('all:\n\t' + shlex.join([sys.executable, '-B', str(work / 'driver.py')]) + '\n')
            env = {**os.environ, 'PYTHONPATH': str(ROOT / 'tools'), 'PYTHONDONTWRITEBYTECODE': '1'}
            for name in ('MAKEFLAGS', 'MFLAGS', 'MAKELEVEL'):
                env.pop(name, None)
            guarded = ('import pathlib,sys,build_state; '
                       f'build_state.ROOT=pathlib.Path({str(work)!r}); '
                       'sys.argv=["build_state","guard","--","make","--no-print-directory"]; build_state.main()')
            process = subprocess.Popen([sys.executable, '-B', '-c', guarded], cwd=work,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, start_new_session=True)
            child_pid = stage_group = None
            try:
                until = time.monotonic() + 5
                while not ready.exists() and time.monotonic() < until:
                    time.sleep(0.02)
                self.assertTrue(ready.exists(), 'fixture child did not start')
                child_pid = int(ready.read_text())
                stage_group = os.getpgid(child_pid)
                os.killpg(process.pid, signal.SIGINT)
                output, _ = process.communicate(timeout=12)
                self.assertNotEqual(process.returncode, 0)
                self.assertIn(b'interrupted', output)
                proc = Path('/proc') / str(child_pid) / 'stat'
                self.assertTrue(not proc.exists() or proc.read_text().split()[2] == 'Z',
                                'interrupted stage left a running child')
            finally:
                if stage_group:
                    try:
                        os.killpg(stage_group, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                if process.poll() is None:
                    os.killpg(process.pid, signal.SIGKILL)
                process.communicate(timeout=5)


if __name__ == '__main__':
    unittest.main()

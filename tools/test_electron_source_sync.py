#!/usr/bin/env python3
"""Check shallow pinned downloads with real local Git histories and port patches."""
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import build_electron_source as builder


# Emulate only gclient's transport and hook boundary; all Git operations,
# patch application, locked revision checks and overlay checks run for real.
GCLIENT = '''#!/usr/bin/env python3
import json, os, pathlib, runpy, subprocess, sys
root = pathlib.Path.cwd()
if sys.argv[1] == 'runhooks':
    if os.environ.get('FIXTURE_FAIL_HOOKS') and not (root / 'hooks-failed').exists():
        (root / 'hooks-failed').touch()
        raise SystemExit(13)
    (root / 'hooks-ran').touch()
    raise SystemExit(0)
config = runpy.run_path(str(root / '.gclient'))
(root / 'sync-arguments.json').write_text(json.dumps(sys.argv[1:]))
assert config['cache_dir'] is None
solution = config['solutions'][0]
repo = root / solution['name']
repo.parent.mkdir(parents=True, exist_ok=True)
subprocess.run(['git', 'init', str(repo)], check=True, stdout=subprocess.DEVNULL)
def git(*args):
    return subprocess.run(['git', '-C', str(repo), *args], check=True, stdout=subprocess.DEVNULL)
if subprocess.run(['git', '-C', str(repo), 'remote', 'get-url', 'origin'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
    git('remote', 'add', 'origin', solution['url'])
revision = sys.argv[sys.argv.index('--revision') + 1].split('@', 1)[1]
if os.environ.get('FIXTURE_WRONG_REVISION'):
    revision = 'HEAD'
options = ['--depth=1'] if '--no-history' in sys.argv else []
git('fetch', '--no-tags', *options, 'origin', revision)
git('checkout', '--detach', 'FETCH_HEAD')
if os.environ.get('FIXTURE_FAIL_SYNC') and not (root / 'sync-failed').exists():
    (root / 'sync-failed').touch()
    raise SystemExit(12)
'''


class SourceSyncTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / 'upstream-source'
        self.depot = self.root / 'upstream-depot'
        self.output = self.root / 'output'
        self.project = self.root / 'project'
        for repository in (self.source, self.depot):
            subprocess.run(['git', 'init', '-q', str(repository)], check=True)
            self.git(repository, 'config', 'user.name', 'Coast test')
            self.git(repository, 'config', 'user.email', 'test@example.invalid')
        self.payload = self.source / 'payload.txt'
        self.payload.write_text('old source\n')
        self.commit(self.source)
        self.old = self.git(self.source, 'rev-parse', 'HEAD')
        self.git(self.source, 'tag', 'old-release')
        self.payload.write_text('locked source\n')
        (self.source / 'other.txt').write_text('locked companion\n')
        self.commit(self.source)
        self.pin = self.git(self.source, 'rev-parse', 'HEAD')
        self.payload.write_text('Coast patched source\n')
        (self.source / 'other.txt').write_text('Coast patched companion\n')
        patch_data = self.git(self.source, 'diff') + '\n'
        self.git(self.source, 'add', '.')
        patched_tree = self.git(self.source, 'write-tree')
        self.git(self.source, 'reset', '--hard', 'HEAD')
        self.payload.write_text('future source\n')
        self.commit(self.source)
        (self.depot / 'gclient').write_text(GCLIENT)
        (self.depot / 'gclient').chmod(0o755)
        self.commit(self.depot)
        depot_pin = self.git(self.depot, 'rev-parse', 'HEAD')
        (self.depot / 'future-file').touch()
        self.commit(self.depot)
        self.git(self.depot, 'tag', 'future-release')
        (self.project / 'ports/electron').mkdir(parents=True)
        (self.project / 'port.patch').write_text(patch_data)
        (self.project / 'policy.h').write_text('pinned Coast policy\n')
        self.lock_path = self.project / 'ports/electron/sources.lock.json'
        self.lock = dict(depot_tools=dict(url=self.depot.as_uri(), commit=depot_pin),
            repositories=[dict(path='src/electron', url=self.source.as_uri(), commit=self.pin,
                patched_tree=patched_tree, patch='port.patch',
                patch_sha256=hashlib.sha256(patch_data.encode()).hexdigest())],
            overlays={'src/electron/policy.h': 'policy.h'})
        self.lock_path.write_text(json.dumps(self.lock))

    @staticmethod
    def git(repository, *args):
        return subprocess.check_output(['git', '-C', str(repository), *args],
                                       text=True, stderr=subprocess.PIPE).strip()

    def commit(self, repository):
        self.git(repository, 'add', '.')
        self.git(repository, 'commit', '-qm', 'fixture')

    def sync(self, **environment):
        argv = ['build_electron_source.py', '--sync', '--output-root', str(self.output)]
        def quiet_run(command, cwd, env=None):
            subprocess.run(list(map(str, command)), cwd=cwd, env=env, check=True,
                           stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        with patch.object(builder, 'ROOT', self.project), patch.object(builder, 'LOCK', self.lock_path), \
                patch.object(builder, 'run', side_effect=quiet_run), \
                patch.object(builder, 'prepare_node', return_value=(Path('/usr/bin'), {})), \
                patch.object(sys, 'argv', argv), patch.dict(os.environ, environment), \
                contextlib.redirect_stdout(io.StringIO()):
            builder.main()

    def overlap_policy_and_patch(self):
        self.lock['overlays']['src/electron/payload.txt'] = 'policy.h'
        self.lock_path.write_text(json.dumps(self.lock))

    def verify(self, overlays=False):
        with patch.object(builder, 'ROOT', self.project):
            builder.verify(self.output / 'checkout', self.lock, overlays=overlays)

    def test_completed_sync_with_policy_replacing_patched_file_is_reused_read_only(self):
        self.overlap_policy_and_patch()
        self.sync()
        source = self.output / 'checkout/src/electron'
        self.assertEqual((source / 'payload.txt').read_bytes(), (self.project / 'policy.h').read_bytes())
        self.assertEqual((source / 'other.txt').read_text(), 'Coast patched companion\n')
        before = {path: (path.read_bytes(), path.stat().st_mtime_ns) for path in (
            source / 'payload.txt', source / 'other.txt', source / '.git/index',
            self.output / 'checkout/sync-arguments.json', self.output / 'checkout/hooks-ran',
            self.output / 'SOURCE.json', self.output / 'SYNC.json')}
        with patch.object(builder, 'fetch_pinned_repository', side_effect=AssertionError('unexpected fetch')):
            self.sync()
        self.verify()  # Read-only runtime/packaging consumers use this mode.
        self.verify(overlays=True)  # The managed pipeline requires every policy.
        self.assertEqual(before, {path: (path.read_bytes(), path.stat().st_mtime_ns) for path in before})

    def test_interrupted_policy_copy_resumes_without_fetching_or_reapplying_patches(self):
        self.overlap_policy_and_patch()
        atomic_write = builder.atomic_write

        def interrupt_after_policy(path, contents):
            atomic_write(path, contents)
            if Path(path).name == 'payload.txt':
                raise KeyboardInterrupt()

        with patch.object(builder, 'atomic_write', side_effect=interrupt_after_policy):
            with self.assertRaises(KeyboardInterrupt):
                self.sync()
        self.assertEqual(json.loads((self.output / 'SYNC.json').read_text())['phase'], 'overlays')
        self.assertFalse((self.output / 'SOURCE.json').exists())
        arguments = self.output / 'checkout/sync-arguments.json'
        previous = arguments.stat().st_mtime_ns
        self.sync()
        self.assertEqual(arguments.stat().st_mtime_ns, previous)
        self.verify(overlays=True)

    def test_policy_match_does_not_skip_other_patch_files(self):
        self.overlap_policy_and_patch()
        self.sync()
        source = self.output / 'checkout/src/electron'
        (source / 'other.txt').write_text('locked companion\n')
        for overlays in (False, True):
            with self.subTest(overlays=overlays):
                with self.assertRaises(subprocess.CalledProcessError):
                    self.verify(overlays=overlays)

    def test_changed_policy_is_rejected_even_outside_patch_context(self):
        self.overlap_policy_and_patch()
        self.sync()
        source = self.output / 'checkout/src/electron'
        (source / 'payload.txt').write_text('altered Coast policy\n')
        with self.assertRaisesRegex(ValueError, 'Coast policy differs'):
            self.verify(overlays=True)

    def test_changed_port_patch_is_rejected_after_policy_replacement(self):
        self.overlap_policy_and_patch()
        self.sync()
        (self.project / 'port.patch').write_text('corrupt patch\n')
        with self.assertRaisesRegex(ValueError, 'Port patch changed'):
            self.verify(overlays=True)

    def test_policy_can_replace_all_files_in_a_patch(self):
        self.overlap_policy_and_patch()
        self.lock['overlays']['src/electron/other.txt'] = 'policy.h'
        self.lock_path.write_text(json.dumps(self.lock))
        self.sync()
        self.verify()
        self.verify(overlays=True)

    def test_completed_sync_with_invalidated_receipt_reuses_downloads_and_patches(self):
        self.overlap_policy_and_patch()
        self.sync()
        (self.output / 'SOURCE.json').unlink()  # BuildSession invalidates changed builders.
        paths = [self.output / 'checkout/sync-arguments.json', self.output / 'checkout/hooks-ran',
                 self.output / 'depot_tools/.git/FETCH_HEAD']
        before = {path: (path.read_bytes(), path.stat().st_mtime_ns) for path in paths}
        self.sync()
        self.verify(overlays=True)
        self.assertTrue((self.output / 'SOURCE.json').exists())
        self.assertEqual(before, {path: (path.read_bytes(), path.stat().st_mtime_ns) for path in paths})

    def test_interrupted_fetch_is_adopted_and_reused(self):
        with self.assertRaises(subprocess.CalledProcessError):
            self.sync(FIXTURE_FAIL_SYNC='1')
        self.assertEqual(json.loads((self.output / 'SYNC.json').read_text())['phase'], 'fetch')
        source = self.output / 'checkout/src/electron'
        self.assertEqual(self.git(source, 'rev-parse', 'HEAD'), self.pin)
        # Reproduce the pre-checkpoint layout of a canceled earlier build.
        (self.output / 'SYNC.json').unlink()
        self.sync()
        self.assertEqual((source / 'payload.txt').read_text(), 'Coast patched source\n')
        self.assertTrue((self.output / 'SOURCE.json').is_file())

    def test_interrupted_hooks_resume_without_fetching_or_reapplying_patches(self):
        with self.assertRaises(subprocess.CalledProcessError):
            self.sync(FIXTURE_FAIL_HOOKS='1')
        self.assertEqual(json.loads((self.output / 'SYNC.json').read_text())['phase'], 'hooks')
        arguments = self.output / 'checkout/sync-arguments.json'
        previous = arguments.stat().st_mtime_ns
        self.sync()
        self.assertEqual(arguments.stat().st_mtime_ns, previous)
        self.assertEqual(json.loads((self.output / 'SYNC.json').read_text())['phase'], 'complete')
        self.sync()  # A completed sync is verified without changing source bytes.

    def test_invalid_sync_checkpoint_is_reconstructed_from_managed_sources(self):
        with self.assertRaises(subprocess.CalledProcessError):
            self.sync(FIXTURE_FAIL_HOOKS='1')
        (self.output / 'SYNC.json').write_text('[]')
        self.sync()
        self.assertTrue((self.output / 'SOURCE.json').exists())
        self.assertEqual(json.loads((self.output / 'SYNC.json').read_text())['phase'], 'complete')

    def test_shallow_sync_preserves_locked_sources_patches_policies_and_hooks(self):
        self.sync(GIT_CACHE_PATH=str(self.root / 'unwanted-global-mirror'))
        checkout = self.output / 'checkout'
        source = checkout / 'src/electron'
        depot = self.output / 'depot_tools'
        self.assertEqual(self.git(source, 'rev-parse', 'HEAD'), self.pin)
        self.assertEqual((source / 'payload.txt').read_text(), 'Coast patched source\n')
        self.assertEqual((source / 'policy.h').read_bytes(), (self.project / 'policy.h').read_bytes())
        self.assertEqual(self.git(source, 'rev-parse', '--is-shallow-repository'), 'true')
        self.assertEqual(self.git(depot, 'rev-parse', '--is-shallow-repository'), 'true')
        self.assertEqual(self.git(depot, 'rev-parse', 'HEAD'), self.lock['depot_tools']['commit'])
        self.assertEqual(self.git(depot, 'tag'), '')
        self.assertEqual(self.git(source, 'tag'), '')
        with self.assertRaises(subprocess.CalledProcessError):
            self.git(source, 'cat-file', '-e', self.old)
        arguments = json.loads((checkout / 'sync-arguments.json').read_text())
        self.assertIn('--no-history', arguments)
        self.assertNotIn('--with_branch_heads', arguments)
        self.assertNotIn('--with_tags', arguments)
        self.assertTrue((checkout / 'hooks-ran').is_file())
        self.assertTrue((self.output / 'SOURCE.json').is_file())
        self.assertFalse((self.root / 'unwanted-global-mirror').exists())

    def test_unlocked_revision_is_rejected_before_hooks_or_receipt(self):
        with self.assertRaisesRegex(ValueError, 'dependency differs'):
            self.sync(FIXTURE_WRONG_REVISION='1')
        self.assertFalse((self.output / 'checkout/hooks-ran').exists())
        self.assertFalse((self.output / 'SOURCE.json').exists())

    def test_changed_port_patch_is_rejected_before_hooks_or_receipt(self):
        (self.project / 'port.patch').write_text('corrupt patch\n')
        with self.assertRaisesRegex(ValueError, 'patch checksum mismatch'):
            self.sync()
        self.assertFalse((self.output / 'checkout/hooks-ran').exists())
        self.assertFalse((self.output / 'SOURCE.json').exists())


if __name__ == '__main__':
    unittest.main()

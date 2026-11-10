#!/usr/bin/env python3
"""Exercise recovery and cleanup against disposable source/build trees."""
import contextlib
import hashlib
import io
import itertools
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

from build_components import APPS, APP_REQUIRES, app_selection, components_for, graph_for
from build_session import BuildSession
from build_state import atomic_write, project_lock, read_json, write_json
import clean_project as cleaner
from qualify_desktop import phase_plan, passed_phase
from source_archive import unpack, unpack_zip

ROOT = Path(__file__).resolve().parents[1]


class Project(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        subprocess.run(['git', 'init', '-q', self.root], check=True)
        (self.root / '.gitignore').write_text('build/\n.config\nuserland/vendor/*\nuserland/bin/\nuserland/runtime/\n')
        self.output = self.root / 'build/coast'
        self.output.mkdir(parents=True)

    def file(self, name, text='fixture'):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        return path

    def plan(self, **kw):
        with patch.object(cleaner, 'kernel_paths', return_value=[self.root / 'build/vmlinux']):
            return cleaner.plan(self.root, self.output, **kw)

    def removed(self, record, path):
        return any(path == Path(parent) or path.is_relative_to(parent) for parent in record['paths'])

    def apply(self, record):
        return cleaner.apply(record, self.root)


class SelectionTests(unittest.TestCase):
    def test_every_app_combination_has_runtime_and_build_dependencies(self):
        for mask in range(1 << len(APPS)):
            selected, _ = app_selection(' '.join(app for bit, app in enumerate(APPS) if mask & (1 << bit)) or 'none')
            for app in selected:
                self.assertLessEqual(APP_REQUIRES.get(app, set()), set(selected))
            graph, nodes = graph_for(selected), components_for(selected)
            self.assertTrue(all(graph[name] <= nodes for name in nodes))

    def test_omissions_propagate_and_shared_engine_is_explicit(self):
        selected, reasons = app_selection(without='chromium')
        self.assertIn('codeoss', selected)
        self.assertNotIn('qemu-viewer', selected)
        self.assertIn('chromium', reasons['qemu-viewer'])
        self.assertIn('electron', components_for(selected))
        selected, _ = app_selection(without='chromium codeoss')
        self.assertNotIn('electron', components_for(selected))
        selected, reasons = app_selection(without='gcc')
        self.assertTrue({'packages', 'makepkg'} <= reasons.keys())
        self.assertTrue({'packages', 'makepkg', 'gcc'}.isdisjoint(selected))

    def test_building_one_app_does_not_build_unrelated_runtime(self):
        needed = components_for(APPS, 'qt')
        self.assertIn('compiler', needed)
        self.assertTrue({'image', 'electron', 'application', 'qemu'}.isdisjoint(needed))
        selected, _ = app_selection('none', add='chromium', previous=['qt'])
        self.assertEqual(selected, ['chromium', 'qt'])
        selected, _ = app_selection('all', without='chromium', add='chromium', previous=['qt'])
        self.assertEqual(selected, ['chromium', 'qt'])
        with self.assertRaisesRegex(ValueError, 'Unknown apps'):
            app_selection('chromun')

    def test_qualification_only_claims_applicable_suites(self):
        full, skipped = phase_plan(dict(apps=APPS))
        self.assertEqual(len(full), 11)
        self.assertFalse(skipped)
        phases, skipped = phase_plan(dict(apps=['qt']))
        self.assertEqual([phase[0] for phase in phases], ['boot', 'qt'])
        self.assertIn('browser', skipped)


class CleanupTests(Project):
    def test_component_cleanup_invalidates_reverse_dependencies_only(self):
        write_json(self.output / 'image/MANIFEST.json', dict(apps=['qt']))
        compiler = self.file('build/coast/compiler/BUILD.json', '{}')
        qt = self.file('build/coast/qt/BUILD.json', '{}')
        image = self.file('build/coast/image/Coast.iso')
        self.file('build/coast/qualification/QUALIFICATION.json', '{}')
        independent = self.file('build/coast/qemu/BUILD.json', '{}')
        record = self.plan(components='qt')
        self.assertEqual(set(record['invalidated']), {'qt', 'image', 'qualification'})
        self.assertEqual(record['affected_apps'], ['qt'])
        self.apply(record)
        self.assertTrue(compiler.exists() and independent.exists() and image.exists())
        self.assertFalse(qt.exists() or (self.output / 'image/MANIFEST.json').exists())
        self.assertEqual(read_json(self.output / 'SELECTION.json')['apps'], ['qt'])

    def test_unselected_engine_cleanup_keeps_current_iso_valid(self):
        manifest = self.file('build/coast/image/MANIFEST.json', json.dumps(dict(apps=['qt'])))
        self.file('build/coast/electron/BUILD.json', '{}')
        record = self.plan(components='electron')
        self.assertNotIn('image', record['invalidated'])
        self.apply(record)
        self.assertTrue(manifest.exists())

    def test_default_cleanup_retains_downloads_sources_configuration_and_secrets(self):
        preserve = [self.file('build/coast/qt/src/qt.cpp'), self.file('build/coast/qt/qt-6.tar.xz'),
                    self.file('.config'), self.file('build/coast/compiler/signing/private.key'),
                    self.file('build/coast/native/state/users.json'), self.file('source.c')]
        product = self.file('build/coast/qt/build/a.o')
        kernel = self.file('build/vmlinux')
        record = self.plan()
        self.apply(record)
        self.assertTrue(all(path.exists() for path in preserve))
        self.assertFalse(product.exists() or kernel.exists())

    def test_source_clean_drops_sync_and_runtime_but_keeps_other_downloads(self):
        self.file('build/coast/electron/checkout/src/.git/HEAD')
        self.file('build/coast/electron/SYNC.json', '{}')
        self.file('build/coast/electron/BUILD.json', '{}')
        self.file('build/coast/native/BUILD.json', '{}')
        archive = self.file('build/coast/qt/qt.tar.xz')
        record = self.plan(components='sources')
        self.assertTrue({'sources', 'electron', 'kernel', 'native', 'image'} <= set(record['invalidated']))
        self.apply(record)
        self.assertFalse((self.output / 'electron/checkout').exists())
        self.assertTrue(archive.exists())

    def test_full_reset_preserves_selected_images_services_and_private_state(self):
        selected = self.file('build/release/image.iso')
        service = self.file('build/service/state/users.db')
        private = self.file('build/development/credentials/token')
        tracked = self.file('build/force-added.txt')
        subprocess.run(['git', '-C', self.root, 'add', '-f', tracked], check=True)
        (self.root / 'build/selected.iso').symlink_to('release/image.iso')
        (self.root / 'build/current-service').symlink_to('service', target_is_directory=True)
        source = self.file('build/coast/electron/checkout/src/source.c')
        archive = self.file('userland/vendor/unused.tar.gz')
        (self.root / 'userland/vendor/registry.json').write_text('{}')
        subprocess.run(['git', '-C', self.root, 'add', '-f', self.root / 'userland/vendor/registry.json'], check=True)
        record = self.plan(all_generated=True)
        self.apply(record)
        self.assertTrue(all(path.exists() for path in [selected, service, private, tracked]))
        self.assertFalse(source.exists() or archive.exists())
        self.assertTrue((self.root / 'userland/vendor/registry.json').exists())
        self.assertTrue((self.root / 'build/.lifecycle.lock').exists())

    def test_keep_toolchains_preserves_complete_receipts_and_electron_host_dependencies(self):
        preserve = [self.file('build/coast/compiler/BUILD.json', '{}'), self.file('build/coast/compiler/bin/gcc'),
                    self.file('build/coast/electron/checkout/src/buildtools/clang'),
                    self.file('build/coast/electron/SYNC.json', '{}'), self.file('build/gnu-sdk-old/sysroot/libc.a')]
        disposable = self.file('build/coast/image/partial.tmp')
        record = self.plan(all_generated=True, keep='toolchains')
        self.apply(record)
        self.assertTrue(all(path.exists() for path in preserve))
        self.assertFalse(disposable.exists())

    def test_explicit_cache_and_download_cleanup_preserves_manifest_files(self):
        cache = self.file('tools/__pycache__/cache.pyc')
        archive = self.file('userland/vendor/download.tar.xz')
        source = self.file('userland/vendor/sources.json')
        subprocess.run(['git', '-C', self.root, 'add', '-f', source], check=True)
        # Python caches are ignored by the actual repository too.
        with (self.root / '.gitignore').open('a') as stream:
            stream.write('__pycache__/\n')
        record = self.plan(components='cache downloads', keep='none')
        self.apply(record)
        self.assertFalse(cache.exists() or archive.exists())
        self.assertTrue(source.exists())

    def test_external_roots_and_symlinks_require_registration(self):
        with tempfile.TemporaryDirectory() as temporary:
            outside = Path(temporary)
            with self.assertRaisesRegex(ValueError, 'not registered'):
                cleaner.plan(self.root, outside, components='qt')
            write_json(outside / 'OWNER.json', dict(source_root=str(self.root)))
            write_json(self.root / 'build/build-roots.json', [str(outside)])
            cache = outside / 'npm-cache/partial'
            cache.parent.mkdir()
            cache.touch()
            cache_plan = cleaner.plan(self.root, outside, components='cache')
            self.apply(cache_plan)
            self.assertFalse(cache.exists())
            product = outside / 'generated.o'
            product.touch()
            record = self.plan(all_generated=True)
            self.apply(record)
            self.assertFalse(product.exists())
            self.assertTrue((outside / 'OWNER.json').exists())
        link = self.root / 'build/symlink'
        link.symlink_to(self.output, target_is_directory=True)
        for unsafe in (self.root, self.root / 'build', link):
            with self.assertRaises(ValueError):
                cleaner.validate_output(self.root, unsafe)

    def test_apply_rechecks_new_private_files_after_preview(self):
        self.file('build/coast/qt/generated.o')
        record = self.plan(components='qt')
        key = self.file('build/coast/qt/new.key')
        self.apply(record)
        self.assertTrue(key.exists())

    def test_cleanup_journal_survives_interrupted_removal(self):
        self.file('build/coast/qt/a.o')
        record = self.plan(components='qt')
        with patch.object(cleaner.shutil, 'rmtree', side_effect=KeyboardInterrupt):
            with self.assertRaises(KeyboardInterrupt):
                self.apply(record)
        journals = list((self.root / 'build/cleanup-records').glob('*.json'))
        self.assertEqual(len(journals), 1)
        self.assertFalse(read_json(journals[0])['applied'])

    def test_canceled_git_download_cleanup_keeps_complete_objects_and_source_checkpoint(self):
        write_json(self.root / 'ports/electron/sources.lock.json', dict(repositories=[dict(path='src/engine')]))
        pending = self.file('build/coast/electron/checkout/src/engine/.git/objects/pack/tmp_pack_fixture')
        complete = self.file('build/coast/electron/checkout/src/engine/.git/objects/pack/pack-complete.pack')
        index_lock = self.file('build/coast/electron/checkout/src/engine/.git/index.lock')
        sync = self.file('build/coast/electron/SYNC.json', '{}')
        self.apply(self.plan(components='cache'))
        self.assertFalse(pending.exists() or index_lock.exists())
        self.assertTrue(complete.exists() and sync.exists())

    def test_environment_files_and_efi_sources_survive_generated_product_cleanup(self):
        secret = self.file('build/coast/qt/.env.local')
        efi_source = self.file('arch/x86/boot/efi/main.c')
        efi_product = self.file('arch/x86/boot/efi/coast.efi')
        with (self.root / '.gitignore').open('a') as stream:
            stream.write('*.efi\n')
        paths = cleaner.select_paths([efi_source.parent], self.root)
        self.assertEqual(paths, [efi_product])
        self.apply(self.plan(components='qt'))
        self.assertTrue(secret.exists())

    def test_full_reset_preserves_registered_named_wizard_settings_and_backups(self):
        import build_config
        config = self.root / 'build/settings/personal'
        with patch.object(build_config, 'ROOT', self.root):
            values = build_config.defaults()
            build_config.save(config, values)
            values['SMP'] = 8
            build_config.save(config, values)
        self.apply(self.plan(all_generated=True))
        self.assertTrue(config.exists() and Path(str(config) + '.old').exists())

    def test_cache_cleanup_removes_orphan_receipt_and_extraction_scratch_only(self):
        pending = self.file('build/coast/.STATE.json.orphan.tmp')
        extraction = self.file('build/coast/.coast-unpack-orphan/partial.c')
        config = self.file('build/coast/personal.tmp')
        self.apply(self.plan(components='cache'))
        self.assertFalse(pending.exists() or extraction.exists())
        self.assertTrue(config.exists())

    def test_gui_cleanup_resets_container_environment_and_keep_retains_toolchain_container(self):
        environment = self.file('build/coast/gui-sources/ENVIRONMENT.json', '{}')
        record = self.plan(components='containers')
        self.assertTrue(record['containers'])
        self.assertTrue(self.removed(record, environment))
        kept = self.plan(all_generated=True, keep='toolchains')
        self.assertFalse(kept['containers'])
        self.assertFalse(self.removed(kept, environment))
        commands = []
        def runner(command, **kwargs):
            commands.append(command)
            return subprocess.CompletedProcess(command, 0, 'a' * 64 + '\n' if command[1] == 'ps' else '')
        cleaner.remove_containers([self.output], runner=runner, available=lambda _: 'podman')
        self.assertIn('label=io.coast.source-build=' + str(self.output / 'gui-sources'), commands[0])
        self.assertEqual(commands[1], ['podman', 'rm', '-f', 'a' * 64])

    def test_cleanup_unlinks_stopped_vm_fifos_and_sockets(self):
        import socket
        directory = self.output / 'image'
        directory.mkdir()
        fifo = directory / 'qmp.in'
        os.mkfifo(fifo)
        endpoint = socket.socket(socket.AF_UNIX)
        endpoint.bind(str(directory / 'qmp.socket'))
        endpoint.close()
        # A private neighbor prevents simply removing the entire parent.
        state = self.file('build/coast/image/state/settings')
        self.apply(self.plan(components='image'))
        self.assertFalse(fifo.exists() or (directory / 'qmp.socket').exists())
        self.assertTrue(state.exists())

    def test_retained_workspace_keeps_source_preparation_receipts(self):
        source = self.file('build/coast/application/workspace/source.c')
        markers = [self.file('build/coast/application/' + name, '{}') for name in
                   ('SOURCE.json', 'PATCHED.json', 'INSTALLED.json', 'PREPARED.json', 'APPLICATION-PACKAGING.json')]
        product = self.file('build/coast/application/PACKAGED.json', '{}')
        self.apply(self.plan(components='application'))
        self.assertTrue(source.exists() and all(path.exists() for path in markers))
        self.assertFalse(product.exists())

    def test_manual_vm_is_detected_without_being_killed(self):
        # A real process with QEMU-like argv tests /proc discovery and reaping.
        process = subprocess.Popen(['qemu-system-fixture', '-c', 'import time; time.sleep(30)', str(self.root / 'build/image.iso')], executable=sys.executable)
        try:
            with self.assertRaisesRegex(ValueError, 'Project process still running'):
                cleaner.assert_idle(self.root)
            self.assertIsNone(process.poll())
        finally:
            process.terminate()
            process.wait()


class SourceCachePrivacyTests(Project):
    def source_archive(self):
        vendor = self.output / 'qt/vendor'
        vendor.mkdir(parents=True)
        source = self.output / 'qt/library-1.0'
        source.mkdir()
        (source / 'library.c').write_text('public source')
        (source / 'test.key').write_text('public upstream fixture')
        archive = vendor / 'library-1.0.tar.gz'
        with tarfile.open(archive, 'w:gz') as bundle:
            for name in ('library.c', 'test.key'):
                bundle.add(source / name, arcname='library-1.0/' + name)
        write_json(self.root / 'ports/library/SOURCES.json', dict(archive=archive.name, sha256=hashlib.sha256(archive.read_bytes()).hexdigest()))
        return source, archive

    def test_full_reset_removes_unchanged_pinned_upstream_key_fixtures(self):
        source, archive = self.source_archive()
        record = self.plan(all_generated=True)
        self.apply(record)
        self.assertFalse(source.exists() or archive.exists())

    def test_private_edit_retains_complete_source_tree_instead_of_leaving_partial_sources(self):
        source, _ = self.source_archive()
        (source / 'test.key').write_text('user private bytes')
        self.apply(self.plan(all_generated=True))
        self.assertEqual((source / 'test.key').read_text(), 'user private bytes')
        self.assertEqual((source / 'library.c').read_text(), 'public source')

    def test_private_fifo_in_source_cache_is_never_opened_or_deleted(self):
        source, _ = self.source_archive()
        (source / 'test.key').unlink()
        os.mkfifo(source / 'test.key')
        self.apply(self.plan(all_generated=True))
        self.assertTrue((source / 'test.key').exists() and (source / 'library.c').exists())

    def test_untrusted_archive_never_justifies_deleting_a_private_file(self):
        source, archive = self.source_archive()
        archive.write_bytes(b'not the pinned archive')
        self.apply(self.plan(all_generated=True))
        self.assertTrue((source / 'test.key').exists() and (source / 'library.c').exists())

    def test_pinned_git_environment_fixture_is_disposable_but_user_edits_are_retained(self):
        checkout = self.output / 'electron/checkout'
        repo = checkout / 'src/engine'
        subprocess.run(['git', 'init', '-q', repo], check=True)
        for key, value in (('user.name', 'fixture'), ('user.email', 'fixture@example.invalid')):
            subprocess.run(['git', '-C', repo, 'config', key, value], check=True)
        (repo / '.npmrc').write_text('public upstream configuration')
        (repo / 'source.c').write_text('public source')
        subprocess.run(['git', '-C', repo, 'add', '.'], check=True)
        subprocess.run(['git', '-C', repo, 'commit', '-qm', 'public source fixture'], check=True)
        commit = subprocess.check_output(['git', '-C', repo, 'rev-parse', 'HEAD'], text=True).strip()
        write_json(self.root / 'ports/electron/sources.lock.json', dict(repositories=[dict(path='src/engine', commit=commit)]))
        self.assertTrue(self.removed(self.plan(all_generated=True), repo))
        (repo / '.npmrc').write_text('private user configuration')
        self.apply(self.plan(all_generated=True))
        self.assertTrue((repo / '.git').exists() and (repo / 'source.c').exists())
        self.assertEqual((repo / '.npmrc').read_text(), 'private user configuration')


class GuiContainerRecoveryTests(Project):
    def exercise(self, mounted=True, owned=True, exists=True):
        from build_gui_sources import prepare_container
        output = self.output / 'gui-sources'
        output.mkdir()
        environment = output / 'ENVIRONMENT.json'
        environment.write_text('{}')
        commands = []
        def runner(command, **kwargs):
            commands.append(command)
            code, stdout = 0, ''
            if command[1:3] == ['container', 'exists']:
                code = 0 if exists else 1
            elif command[1] == 'inspect':
                stdout = json.dumps([dict(Config=dict(Labels={'io.coast.source-build': str(output) if owned else 'another-project'}), ImageName='pinned-image')])
            elif command[1] == 'exec':
                stdout = (output / '.coast-container-binding').read_text() if mounted else 'obsolete-inode'
            return subprocess.CompletedProcess(command, code, stdout)
        try:
            prepare_container(output, dict(container_image='pinned-image'), runner=runner)
        except BaseException:
            self.assertFalse(any(command[1] == 'rm' for command in commands))
            raise
        return environment, commands

    def test_current_container_retains_its_installed_host_tools(self):
        environment, commands = self.exercise()
        self.assertTrue(environment.exists())
        self.assertFalse(any(command[1] in {'create', 'rm'} for command in commands))

    def test_stale_mount_recreates_owned_container_and_resets_environment(self):
        environment, commands = self.exercise(mounted=False)
        self.assertFalse(environment.exists())
        self.assertEqual(sum(command[1] == 'rm' for command in commands), 1)
        self.assertEqual(sum(command[1] == 'create' for command in commands), 1)

    def test_new_container_does_not_trust_an_old_environment_receipt(self):
        environment, commands = self.exercise(exists=False)
        self.assertFalse(environment.exists())
        self.assertTrue(any(command[1] == 'create' for command in commands))

    def test_foreign_container_is_never_removed(self):
        with self.assertRaisesRegex(ValueError, 'does not belong'):
            self.exercise(owned=False)


class DurabilityTests(Project):
    def test_atomic_metadata_keeps_previous_receipt_on_replace_failure(self):
        path = self.file('build/coast/BUILD.json', '{"previous":true}')
        with patch('build_state.os.replace', side_effect=OSError('fixture interruption')):
            with self.assertRaises(OSError):
                atomic_write(path, '{"new":true}')
        self.assertEqual(read_json(path), dict(previous=True))
        self.assertEqual(list(path.parent.glob('.BUILD.json.*')), [])

    def test_lock_is_reentrant_for_owned_children_and_excludes_rivals(self):
        code = 'from build_state import project_lock; import sys;\nwith project_lock(sys.argv[1]): print("owned")'
        env = dict(os.environ, PYTHONPATH=str(ROOT / 'tools'))
        with project_lock(self.root):
            with project_lock(self.root):
                pass
            env.update(COAST_PROJECT_LOCK_TOKEN=os.environ['COAST_PROJECT_LOCK_TOKEN'])
            child = subprocess.run([sys.executable, '-B', '-c', code, self.root], env=env, capture_output=True, text=True)
            self.assertEqual(child.returncode, 0, child.stderr)
            env.pop('COAST_PROJECT_LOCK_TOKEN')
            rival = subprocess.run([sys.executable, '-B', '-c', code, self.root], env=env, capture_output=True, text=True)
            self.assertNotEqual(rival.returncode, 0)
            self.assertIn('Another build', rival.stderr)
        self.assertFalse((self.root / 'build/.lifecycle-owner.json').exists())

    def test_interrupted_incremental_stage_resumes_and_verified_completion_is_reused(self):
        builder = self.file('builder.py', '''import json, pathlib, sys
out = pathlib.Path(sys.argv[1]); out.mkdir(exist_ok=True)
progress = out / 'progress'
if not progress.exists():
    progress.write_text('compiled first object')
    raise SystemExit(9)
(out / 'BUILD.json').write_text(json.dumps(dict(state='complete', resumed=progress.read_text())))
''')
        output = self.output / 'qt'
        session = BuildSession(self.root, self.output, {'source': 'pin'})
        verify = lambda p: json.loads(p.read_text())
        command = [sys.executable, '-B', str(builder), str(output)]
        with contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(subprocess.CalledProcessError):
                session.run('qt', command, output / 'BUILD.json', verify)
            self.assertEqual(session.state.get('qt')['status'], 'failed')
            result = session.run('qt', command, output / 'BUILD.json', verify)
            self.assertEqual(result['resumed'], 'compiled first object')
            with patch('build_session.run_logged') as run:
                session.run('qt', command, output / 'BUILD.json', verify)
                run.assert_not_called()

    def test_retry_preserves_preparation_receipts_for_a_private_source_workspace(self):
        source = self.file('build/coast/application/workspace/.npmrc', 'user private configuration')
        markers = [self.file('build/coast/application/' + name, '{}') for name in
                   ('SOURCE.json', 'PATCHED.json', 'INSTALLED.json', 'PREPARED.json')]
        self.file('build/coast/application/PACKAGED.json', '{}')
        session = BuildSession(self.root, self.output, {'source': 'pin'})
        session.reset('application', self.output / 'application')
        self.assertTrue(source.exists() and all(path.exists() for path in markers))
        self.assertFalse((self.output / 'application/PACKAGED.json').exists())

    def test_failed_qualification_retains_passed_phase_checkpoint_on_retry(self):
        builder = self.file('qualifier.py', """import pathlib,json,sys
out=pathlib.Path(sys.argv[1]); out.mkdir(exist_ok=True)
phase=out/'passed-phase'
if not phase.exists():
    phase.write_text('passed first phase')
    (out/'QUALIFICATION.json').write_text('{"state":"failed"}')
    raise SystemExit(9)
(out/'QUALIFICATION.json').write_text(json.dumps(dict(state='passed', resumed=phase.read_text())))
""")
        session = BuildSession(self.root, self.output, {'source': 'pin'})
        receipt = self.output / 'qualification/QUALIFICATION.json'
        def verify(path):
            result = read_json(path)
            if result['state'] != 'passed':
                raise ValueError('Qualification is unfinished')
            return result
        command = [sys.executable, '-B', str(builder), str(receipt.parent)]
        with contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(subprocess.CalledProcessError):
                session.run('qualification', command, receipt, verify)
            result = session.run('qualification', command, receipt, verify)
        self.assertEqual(result['resumed'], 'passed first phase')

    def test_damaged_completed_artifact_restarts_only_its_component(self):
        builder = self.file('builder.py', "import pathlib,sys; out=pathlib.Path(sys.argv[1]); out.mkdir(exist_ok=True); (out/'BUILD.json').write_text('{\"state\":\"valid\"}')\n")
        session = BuildSession(self.root, self.output, {'source': 'pin'})
        receipt = self.file('build/coast/qt/BUILD.json', '{"state":"damaged"}')
        independent = self.file('build/coast/qemu/BUILD.json', '{}')
        scratch = self.file('build/coast/qt/half-installed')
        def verify(path):
            result = read_json(path)
            if result.get('state') != 'valid':
                raise ValueError('Damaged artifact')
            return result
        with contextlib.redirect_stdout(io.StringIO()):
            session.run('qt', [sys.executable, '-B', str(builder), str(receipt.parent)], receipt, verify)
        self.assertTrue(independent.exists())
        self.assertFalse(scratch.exists())

    def test_zip_failure_and_orphan_extraction_are_recoverable(self):
        import zipfile
        source = self.root / 'source.zip'
        with zipfile.ZipFile(source, 'w') as archive:
            archive.writestr('module/source.c', 'complete source')
        destination = self.root / 'source'
        with patch.object(zipfile.ZipFile, 'extractall', side_effect=KeyboardInterrupt):
            with self.assertRaises(KeyboardInterrupt):
                unpack_zip(source, destination)
        self.assertFalse(destination.exists())
        orphan = self.root / '.coast-unpack-abandoned'
        orphan.mkdir()
        (orphan / 'unfinished').touch()
        unpack_zip(source, destination)
        self.assertFalse(orphan.exists())
        self.assertEqual((destination / 'module/source.c').read_text(), 'complete source')

    def test_archive_extraction_never_publishes_half_a_source_tree(self):
        archive = self.root / 'source.tar'
        with tarfile.open(archive, 'w') as tar:
            for name in ('source/first.c', 'source/last.c'):
                member = tarfile.TarInfo(name)
                member.size = 3
                tar.addfile(member, io.BytesIO(b'abc'))
        destination = self.root / 'unpacked'
        def interrupt(tar, path, **kw):
            tar.extract(tar.next(), path, filter='data')
            raise KeyboardInterrupt
        with patch.object(tarfile.TarFile, 'extractall', interrupt):
            with self.assertRaises(KeyboardInterrupt):
                unpack(archive, destination)
        self.assertFalse((destination / 'source').exists())
        self.assertEqual([path.name for path in destination.iterdir()], ['.coast-unpack.lock'])
        unpack(archive, destination)
        self.assertEqual((destination / 'source/last.c').read_bytes(), b'abc')
        # Debian overlays may intentionally merge into an already complete source tree.
        (destination / 'source/retained.c').write_text('keep')
        unpack(archive, destination)
        self.assertEqual((destination / 'source/retained.c').read_text(), 'keep')


if __name__ == '__main__':
    unittest.main()

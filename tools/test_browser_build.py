#!/usr/bin/env python3
"""Check browser tool bootstrap, dependency patches and cold-build ordering."""
import hashlib
import io
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import build_javascriptcore
import build_webcore
import build_webcore_deps
import build_webkit_deps
from prepare_browser_host import ROOT, prepare_archive


class HostArchiveTest(unittest.TestCase):
    def setUp(self):
        work = tempfile.TemporaryDirectory()
        self.addCleanup(work.cleanup)
        self.cache = Path(work.name)
        (self.cache / 'downloads').mkdir()

    def fixture(self, kind, members=None):
        members = members or {'bin/tool': b'verified tool fixture'}
        entry = {'archive': 'fixture.tar.gz' if kind == 'tar' else 'fixture.zip',
                 'url': 'https://example.invalid/tool', 'kind': kind,
                 'directory': 'tool-1' if kind != 'zip' else None,
                 'ready': ('bin/tool',) if kind != 'zip' else ('ninja',)}
        archive = self.cache / 'downloads' / entry['archive']
        if kind == 'tar':
            with tarfile.open(archive, 'w:gz') as contents:
                for name, data in members.items():
                    info = tarfile.TarInfo('tool-1/' + name)
                    info.size = len(data)
                    contents.addfile(info, io.BytesIO(data))
        else:
            with zipfile.ZipFile(archive, 'w') as contents:
                for name, data in members.items():
                    contents.writestr(name, data)

        entry['sha256'] = hashlib.sha256(archive.read_bytes()).hexdigest()
        return entry, archive

    def test_verified_tar_is_reused_offline(self):
        entry, archive = self.fixture('tar')
        prepare_archive(entry, self.cache)
        archive.unlink()
        with patch('prepare_browser_host.fetch') as fetch:
            prepare_archive(entry, self.cache)
            fetch.assert_not_called()

        self.assertEqual((self.cache / 'host-tools/tool-1/bin/tool').read_bytes(),
                         b'verified tool fixture')

    def test_host_tools_use_the_shared_custom_mirror(self):
        entry, archive = self.fixture('tar')
        payload = archive.read_bytes()
        archive.unlink()

        def download(command, **kwargs):
            self.assertEqual(command[-1], 'https://cache.invalid/tools/' + entry['archive'])
            Path(command[command.index('--output') + 1]).write_bytes(payload)

        with patch.dict(os.environ, {'SOURCE_MIRRORS': 'https://cache.invalid/tools'}), \
                patch('fetch_sources.subprocess.run', side_effect=download):
            prepare_archive(entry, self.cache)
        self.assertEqual((self.cache / 'host-tools/tool-1/bin/tool').read_bytes(),
                         b'verified tool fixture')

    def test_corrupt_archive_does_not_replace_previous_installation(self):
        entry, archive = self.fixture('tar')
        target = self.cache / 'host-tools/tool-1/bin/tool'
        target.parent.mkdir(parents=True)
        target.write_bytes(b'previous working installation')
        archive.write_bytes(b'corrupt download')
        with self.assertRaisesRegex(ValueError, 'SHA-256 mismatch'):
            prepare_archive(entry, self.cache)

        self.assertEqual(target.read_bytes(), b'previous working installation')

    def test_missing_installed_file_is_repaired(self):
        entry, _ = self.fixture('tar')
        prepare_archive(entry, self.cache)
        target = self.cache / 'host-tools/tool-1/bin/tool'
        target.unlink()
        prepare_archive(entry, self.cache)
        self.assertTrue(target.is_file())

    def test_ninja_is_executable(self):
        entry, _ = self.fixture('zip', {'ninja': b'fixture executable'})
        prepare_archive(entry, self.cache)
        self.assertTrue(os.access(self.cache / 'host-tools/ninja', os.X_OK))

    def test_wheel_reinstallation_removes_previous_directory(self):
        entry, _ = self.fixture('wheel')
        prepare_archive(entry, self.cache)
        target = self.cache / 'host-tools/tool-1'
        (target / 'obsolete-file').touch()
        (target / 'bin/tool').unlink()
        prepare_archive(entry, self.cache)

        self.assertFalse((target / 'obsolete-file').exists())
        self.assertFalse((target / '.previous').exists())
        self.assertEqual(list(target.iterdir()), [target / 'bin'])

    def test_archive_escape_is_rejected(self):
        entry, _ = self.fixture('wheel', {'../escape': b'unsafe'})
        with self.assertRaisesRegex(ValueError, 'Unsafe build-tool archive member'):
            prepare_archive(entry, self.cache)
        self.assertFalse((self.cache / 'host-tools/escape').exists())
        self.assertFalse((self.cache / 'host-tools/tool-1').exists())


class NetworkDependencyBuildTest(unittest.TestCase):
    def setUp(self):
        work = tempfile.TemporaryDirectory()
        self.addCleanup(work.cleanup)
        self.root = Path(work.name) / 'checkout'
        self.build = self.root / 'build/modern-browser'
        self.originals = self.build / 'webcore-original'
        self.module_path = self.build / 'glib-networking-2.80.1/tls/openssl/meson.build'
        self.tls_path = self.module_path.parents[1] / 'meson.build'
        self.module_text = """module = shared_module(
  'gioopenssl',
  link_args: module_ldflags,
  link_depends: symbol_map,
  name_suffix: module_suffix,
  install: build_shared,
)

if build_static
  pkg.generate(module)
endif
"""
        self.tls_text = "subdir('base')\nsubdir('openssl')\nsubdir('tests')\n"
        self.archive = self.root / 'userland/vendor/glib-networking-2.80.1.tar.xz'
        self.archive.parent.mkdir(parents=True)
        with tarfile.open(self.archive, 'w:xz') as contents:
            for relative, text in (('tls/openssl/meson.build', self.module_text),
                                   ('tls/meson.build', self.tls_text)):
                data = text.encode()
                info = tarfile.TarInfo('glib-networking-2.80.1/' + relative)
                info.size = len(data)
                contents.addfile(info, io.BytesIO(data))
        packages = dict(build_webcore_deps.PACKAGES)
        packages[self.archive.name] = hashlib.sha256(self.archive.read_bytes()).hexdigest()
        settings = patch.multiple(build_webcore_deps, ROOT=self.root, BUILD=self.build,
                                  PREFIX=self.build / 'prefix', PACKAGES=packages)
        settings.start()
        self.addCleanup(settings.stop)

    def assert_patched(self):
        self.assertEqual(
            (self.originals / 'glib-networking-openssl-meson.build').read_text(),
            self.module_text)
        self.assertEqual(
            (self.originals / 'glib-networking-tls-meson.build').read_text(), self.tls_text)
        self.assertIn('module = static_library(', self.module_path.read_text())
        self.assertNotIn('shared_module(', self.module_path.read_text())
        self.assertEqual(self.tls_path.read_text(), "subdir('base')\nsubdir('openssl')\n\n")

    def test_first_build_creates_patch_backups_and_rerun_preserves_them(self):
        self.assertFalse(self.build.exists())
        first = build_webcore_deps.patch_glib_networking()
        self.assert_patched()
        self.assertEqual(build_webcore_deps.patch_glib_networking(), first)
        self.assert_patched()

    def test_deleted_backups_are_restored_from_upstream(self):
        first = build_webcore_deps.patch_glib_networking()
        shutil.rmtree(self.originals)
        self.assertEqual(build_webcore_deps.patch_glib_networking(), first)
        self.assert_patched()

    def test_partial_backups_and_missing_working_source_are_repaired(self):
        first = build_webcore_deps.patch_glib_networking()
        (self.originals / 'glib-networking-openssl-meson.build').write_text('partial write')
        (self.originals / 'glib-networking-tls-meson.build').unlink()
        self.module_path.unlink()
        self.assertEqual(build_webcore_deps.patch_glib_networking(), first)
        self.assert_patched()

    def test_interrupted_backup_publication_can_be_retried(self):
        with patch('build_webcore_deps.Path.replace', side_effect=OSError('interrupted')):
            with self.assertRaisesRegex(OSError, 'interrupted'):
                build_webcore_deps.patch_glib_networking()
        self.assertEqual(list(self.originals.iterdir()), [])
        build_webcore_deps.patch_glib_networking()
        self.assert_patched()

    def test_bad_archive_is_rejected_before_sources_are_modified(self):
        self.archive.write_bytes(b'invalid archive')
        with self.assertRaisesRegex(SystemExit, 'Missing or invalid pinned archive'):
            build_webcore_deps.patch_glib_networking()
        self.assertFalse(self.build.exists())

    def test_sqlite_installation_is_repaired_despite_its_completion_stamp(self):
        source = self.build / 'sqlite-autoconf-3540000'
        source.mkdir(parents=True)
        for name in ('sqlite3.h', 'sqlite3ext.h'):
            (source / name).write_text('upstream header\n')
        prefix = self.build / 'prefix'
        metadata = prefix / 'lib/pkgconfig/libpsl.pc'
        metadata.parent.mkdir(parents=True)
        metadata.write_text('Name: libpsl\n')

        def compile(directory, name, command, env):
            if name == 'sqlite-3.54.0-archive':
                Path(command[2]).write_bytes(b'compiled sqlite archive')

        with patch.object(build_webcore_deps, 'autotools_stage'), \
                patch.object(build_webcore_deps, 'meson_stage'), \
                patch.object(build_webcore_deps, 'run', side_effect=compile) as run:
            build_webcore_deps.network_dependencies(
                {'CC': 'cc', 'AR': 'ar', 'LDFLAGS': '-static'}, jobs=1)
            (prefix / 'lib/libsqlite3.a').unlink()
            shutil.rmtree(prefix / 'include')
            (prefix / 'lib/pkgconfig/sqlite3.pc').unlink()
            build_webcore_deps.network_dependencies(
                {'CC': 'cc', 'AR': 'ar', 'LDFLAGS': '-static'}, jobs=1)
            self.assertEqual(run.call_count, 4)
            self.assertEqual((prefix / 'lib/libsqlite3.a').read_bytes(), b'compiled sqlite archive')
            self.assertEqual((prefix / 'include/sqlite3.h').read_text(), 'upstream header\n')
            self.assertTrue((prefix / 'lib/pkgconfig/sqlite3.pc').is_file())


class WebCoreSourcePatchTest(unittest.TestCase):
    def test_deleted_or_partial_backups_never_become_patch_inputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = root / 'webkit.tar.xz'
            relative = 'Source/WebCore/fixture.cpp'
            upstream = 'original upstream source\n'
            with tarfile.open(archive, 'w:xz') as contents:
                info = tarfile.TarInfo('wpewebkit-2.54.0/' + relative)
                info.size = len(upstream.encode())
                contents.addfile(info, io.BytesIO(upstream.encode()))
            build = root / 'build/modern-browser'
            source = build / 'webcore-source/wpewebkit-2.54.0'
            original = build / 'webcore-original/upstream' / relative
            patched = source / relative
            patches = [(relative, lambda text: text.replace('original', 'adapted'))]
            with patch.multiple(build_webcore, BUILD=build, SOURCE=source):
                build_webcore.apply_patches(patches, archive)
                first_write = patched.stat().st_mtime_ns
                build_webcore.apply_patches(patches, archive)
                self.assertEqual(patched.stat().st_mtime_ns, first_write)
                for damage in ('deleted', 'partial'):
                    with self.subTest(damage=damage):
                        if damage == 'deleted':
                            shutil.rmtree(build / 'webcore-original')
                        else:
                            original.write_text('partial backup')
                            patched.unlink()
                        build_webcore.apply_patches(patches, archive)
                        self.assertEqual(original.read_text(), upstream)
                        self.assertEqual(patched.read_text(), 'adapted upstream source\n')


class DependencyStageTest(unittest.TestCase):
    modules = (build_javascriptcore, build_webkit_deps, build_webcore_deps)

    def fixture(self, root):
        configure = root / 'configure'
        configure.write_text('#!' + sys.executable + '\n' + FAKE_CONFIGURE)
        configure.chmod(0o755)
        make = root / 'host-bin/make'
        make.parent.mkdir()
        make.write_text('#!' + sys.executable + '\n' + FAKE_MAKE)
        make.chmod(0o755)
        artifact = root / 'prefix/lib/fixture.a'
        env = dict(os.environ, PATH=str(make.parent) + os.pathsep + os.environ['PATH'],
                   MODERNOS_TEST_EVENTS=str(root / 'events'),
                   MODERNOS_TEST_ARTIFACT=str(artifact))
        return configure, artifact, env

    def invoke(self, module, configure, artifact, env, inputs=''):
        if module is build_javascriptcore:
            module.stage('fixture', [configure], [], env, jobs=1, outputs=[artifact])
        else:
            stage = module.stage if module is build_webcore_deps else module.build_stage
            stage('fixture', [configure], [['make'], ['make', 'install']], env,
                  inputs, outputs=[artifact])

    def test_cold_stages_repair_missing_installations_and_configuration(self):
        for module in self.modules:
            with self.subTest(module=module.__name__), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                configure, artifact, env = self.fixture(root)
                build = root / 'missing/build'
                with patch.object(module, 'BUILD', build):
                    self.invoke(module, configure, artifact, env)
                    self.invoke(module, configure, artifact, env)
                    self.assertEqual((root / 'events').read_text().splitlines(),
                                     ['configure', 'build', 'install'])
                    shutil.rmtree(artifact.parents[1])
                    self.invoke(module, configure, artifact, env)
                    self.assertTrue(artifact.is_file())
                    self.assertEqual((root / 'events').read_text().splitlines()[-2:],
                                     ['build', 'install'])
                    artifact.unlink()
                    (build / 'fixture/Makefile').unlink()
                    self.invoke(module, configure, artifact, env)
                    self.assertEqual((root / 'events').read_text().splitlines()[-3:],
                                     ['configure', 'build', 'install'])

    def test_failed_repairs_cannot_reuse_a_previous_completion_stamp(self):
        for module in self.modules:
            with self.subTest(module=module.__name__), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                configure, artifact, env = self.fixture(root)
                build = root / 'build'
                with patch.object(module, 'BUILD', build):
                    self.invoke(module, configure, artifact, env)
                    artifact.unlink()
                    env['MODERNOS_TEST_FAIL_INSTALL'] = '1'
                    with self.assertRaises(SystemExit):
                        self.invoke(module, configure, artifact, env)
                    self.assertFalse((build / 'fixture/.modernos-stage').exists())
                    del env['MODERNOS_TEST_FAIL_INSTALL']
                    self.invoke(module, configure, artifact, env)
                    self.assertTrue(artifact.is_file())

    def test_missing_outputs_cannot_be_marked_complete(self):
        for module in self.modules:
            with self.subTest(module=module.__name__), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                configure, artifact, env = self.fixture(root)
                env['MODERNOS_TEST_SKIP_OUTPUT'] = '1'
                build = root / 'build'
                with patch.object(module, 'BUILD', build):
                    with self.assertRaisesRegex(SystemExit, 'missing build outputs'):
                        self.invoke(module, configure, artifact, env)
                    self.assertFalse((build / 'fixture/.modernos-stage').exists())

    def test_meson_reconfigures_existing_builds_when_inputs_change(self):
        for module in (build_webkit_deps, build_webcore_deps):
            with self.subTest(module=module.__name__), tempfile.TemporaryDirectory() as temporary:
                build = Path(temporary) / 'build'
                coredata = build / 'fixture/meson-private/coredata.dat'
                buildfile = build / 'fixture/build.ninja'
                artifact = build / 'prefix/fixture.a'
                calls = []

                def run(directory, name, command, env):
                    calls.append(command)
                    if 'setup' in command:
                        if not coredata.exists() or '--reconfigure' in command:
                            coredata.parent.mkdir(parents=True, exist_ok=True)
                            coredata.write_text(env['TEST_CONFIGURATION'])
                            buildfile.touch()
                    elif 'install' in command:
                        artifact.parent.mkdir(parents=True, exist_ok=True)
                        artifact.touch()

                stage = module.stage if module is build_webcore_deps else module.build_stage
                with patch.object(module, 'BUILD', build), patch.object(module, 'run', run):
                    for version in ('first', 'changed'):
                        stage('fixture', ['meson', 'setup', build / 'fixture'],
                              [['ninja'], ['meson', 'install']],
                              {'TEST_CONFIGURATION': version}, version, outputs=[artifact])
                        self.assertEqual(coredata.read_text(), version)
                    buildfile.unlink()
                    artifact.unlink()
                    stage('fixture', ['meson', 'setup', build / 'fixture'],
                          [['ninja'], ['meson', 'install']],
                          {'TEST_CONFIGURATION': version}, version, outputs=[artifact])
                    self.assertTrue(buildfile.is_file())
                self.assertIn('--reconfigure', calls[3])
                self.assertIn('--clearcache', calls[3])
                self.assertIn('--reconfigure', calls[6])


class ParallelBrowserBuildTest(unittest.TestCase):
    def test_empty_outputs_build_in_dependency_order(self):
        # Run the real Makefile with cheap stand-in compilers. Each stage checks
        # that its actual prerequisite artifacts exist before publishing outputs.
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            userland = root / 'userland'
            userland.mkdir()
            shutil.copy2(ROOT / 'userland/Makefile', userland / 'Makefile')
            for directory in ('include', 'modern-browser'):
                shutil.copytree(ROOT / 'userland' / directory, userland / directory)
            (root / 'include/uapi').mkdir(parents=True)
            shutil.copy2(ROOT / 'include/uapi/audio.h', root / 'include/uapi/audio.h')
            (userland / 'vendor').mkdir()
            (userland / 'vendor/mozilla-ca-bundle.pem').touch()
            (root / 'tools').mkdir()
            scripts = ('fetch_sources', 'prepare_browser_host', 'build_nano', 'build_gcc',
                       'build_web_deps', 'build_javascriptcore', 'build_threaded_libgcc',
                       'build_webkit_deps', 'build_webcore_deps', 'build_media_deps',
                       'build_webcore_checks', 'build_webcore')
            for name in scripts:
                (root / 'tools' / (name + '.py')).touch()
            launcher = root / 'host-bin/python3'
            launcher.parent.mkdir()
            launcher.write_text('#!' + sys.executable + '\n' + FAKE_BUILDERS)
            launcher.chmod(0o755)
            env = dict(os.environ, PATH=str(launcher.parent) + os.pathsep + os.environ['PATH'])
            env.pop('MAKEFLAGS', None)
            result = subprocess.run(['make', '-j8', '-C', str(userland), 'bin/web-renderer'],
                                    env=env, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertTrue((userland / 'bin/web-renderer').is_file())
            events = (userland / 'events').read_text().splitlines()
            for name in ('build_nano', 'build_gcc', 'build_web_deps', 'build_javascriptcore',
                         'build_webkit_deps', 'build_webcore_deps', 'build_media_deps',
                         'build_webcore_checks', 'build_webcore'):
                self.assertEqual(events.count(name), 1, events)


FAKE_CONFIGURE = '''
import os
from pathlib import Path
with Path(os.environ['MODERNOS_TEST_EVENTS']).open('a') as output:
    output.write('configure\\n')
Path('Makefile').write_text('configured fixture')
'''


FAKE_MAKE = '''
import os
from pathlib import Path
import sys
stage = 'install' if 'install' in sys.argv else 'build'
with Path(os.environ['MODERNOS_TEST_EVENTS']).open('a') as output:
    output.write(stage + '\\n')
if stage == 'install':
    if os.environ.get('MODERNOS_TEST_FAIL_INSTALL'):
        raise SystemExit(1)
    if not os.environ.get('MODERNOS_TEST_SKIP_OUTPUT'):
        artifact = Path(os.environ['MODERNOS_TEST_ARTIFACT'])
        artifact.parent.mkdir(parents=True, exist_ok=True)
        artifact.write_bytes(b'installed fixture')
'''


FAKE_BUILDERS = '''
import os
from pathlib import Path
import sys
import time
stage = Path(sys.argv[1]).stem
requirements = {
    "build_nano": ("sources-ready",),
    "build_gcc": ("bin/nano",),
    "build_web_deps": ("bin/nano",),
    "build_javascriptcore": ("runtime/usr/bin/gcc",),
    "build_webkit_deps": ("bin/jsc", "bin/curl"),
    "build_webcore_deps": ("webkit-ready",),
    "build_media_deps": ("webcore-deps-ready",),
    "build_webcore_checks": ("webcore-deps-ready",),
    "build_webcore": ("bin/web-broker", "media-ready"),
}
outputs = {
    "build_nano": ("bin/nano",),
    "build_gcc": ("runtime/usr/bin/gcc",),
    "build_web_deps": ("bin/curl", "bin/openssl"),
    "build_javascriptcore": ("bin/jsc", "bin/thread-cxx-check",
        "runtime/usr/share/modern-browser/engine-check.js",
        "runtime/usr/share/modern-browser/module-check.mjs",
        "runtime/usr/share/modern-browser/feature-module.mjs",
        "runtime/usr/share/modern-browser/agent-check.js"),
    "build_webkit_deps": ("webkit-ready",),
    "build_webcore_deps": ("webcore-deps-ready",),
    "build_media_deps": ("media-ready",),
    "build_webcore_checks": ("bin/web-broker",),
    "build_webcore": ("bin/web-renderer",),
}
if stage == "fetch_sources":
    outputs[stage] = ("vendor/ffmpeg-9.0.2.tar.xz", "vendor/nasm-2.16.03.tar.xz", "sources-ready")
for name in requirements.get(stage, ()):
    if not Path(name).is_file():
        raise SystemExit(stage + " started before " + name)
time.sleep(0.04)
for name in outputs[stage]:
    path = Path(name)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.touch()
with open("events", "a") as events:
    events.write(stage + "\\n")
'''


if __name__ == '__main__':
    unittest.main()

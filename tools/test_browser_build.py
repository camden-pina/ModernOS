#!/usr/bin/env python3
"""Check browser tool bootstrap and Make's parallel cold-build ordering."""
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

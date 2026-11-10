#!/usr/bin/env python3
"""Add the pinned real VSCodium workbench to a preserved Electron test ISO.

Builds guest bridges under the existing confined compiler. Reuses the input
kernel unchanged; use build_electron_iso.py first when changing kernel sources.
An unused output directory is required. No editor binaries run on the host.
"""
from build_state import project_lock
from build_state import atomic_write
import argparse
import base64
import fcntl
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import posixpath
import re
import shutil
import subprocess
import sys
import tarfile
from codeoss_rpc_trace import instrument_rpc, instrument_wire
from build_cursor_theme import build as build_cursor_theme

ROOT = Path(__file__).resolve().parents[1]
BUNDLE_HASH = '61223b74f9e40834117311ec4177cb386d09b84c1aae4f9e674c96580569cef3'
ELECTRON_HASH = 'c8f5819cb4d5160cee5d951c4f123c075f65c63c89405a4a674f5609191748d6'


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--artifacts', type=Path, default=Path.home() / 'Documents/Codex/2026-10-24/task-2/codeoss-partial-page-r2')
    p.add_argument('--base-iso', type=Path, required=True)
    p.add_argument('--output-root', type=Path, required=True)
    p.add_argument('--diagnostics', action='store_true', help='log startup dialogs while retaining their original behavior')
    p.add_argument('--wire-trace', action='store_true', help='observe native port callbacks separately from RPC')
    p.add_argument('--rpc-trace', action='store_true', help='bounded observation through the original workbench RPC logger')
    p.add_argument('--startup-cache', type=Path, help='guest-generated CACHE.json bound to this runtime and installed application sources')
    p.add_argument('--font-dir', type=Path, default=Path('/usr/share/fonts/TTF'))
    p.add_argument('--font-license', type=Path, default=Path('/usr/share/licenses/ttf-dejavu/LICENSE'))
    p.add_argument('--cjk-font',type=Path,default=Path('/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc'))
    p.add_argument('--cjk-license',type=Path,default=Path('/usr/share/licenses/noto-fonts-cjk/LICENSE'))
    p.add_argument('--emoji-font',type=Path,default=Path('/usr/share/fonts/noto/NotoColorEmoji.ttf'))
    p.add_argument('--emoji-license',type=Path,default=Path('/usr/share/licenses/noto-fonts-emoji/LICENSE'))
    p.add_argument('--xkbfile-deb', type=Path)
    p.add_argument('--runtime-build', type=Path, help='completed private Electron build manifest')
    p.add_argument('--native-node-build',type=Path,help='verified matching standalone Node build manifest')
    p.add_argument('--qt-build',type=Path,help='compiled static Qt 6 Coast port manifest')
    p.add_argument('--broker-build',type=Path,help='verified native browser service rebuild')
    p.add_argument('--browser-build',type=Path,help='verified native browser platform rebuild')
    p.add_argument('--program-ports-build',type=Path,help='verified static Python/jq port manifest')
    p.add_argument('--oniguruma-build',type=Path,help='verified build-time Oniguruma JS translation manifest')
    a = p.parse_args()
    programs=json.loads(a.program_ports_build.read_text()) if a.program_ports_build else None
    if programs:
        if programs.get('target_host_executed') is not False:p.error('Program ports must be cross compiled')
        packaged_base=programs.get('sources',{}).get('base')
        if packaged_base and sha(a.base_iso)!=packaged_base['sha256']:p.error('Native packages were assembled from a different base image')
        for name,asset in programs['files'].items():
            if PurePosixPath(name).is_absolute() or '..' in PurePosixPath(name).parts or sha(Path(asset['path']))!=asset['sha256']:p.error('Program port changed or has invalid path: '+name)
            if asset.get('mode',0o644) not in (0o600,0o644,0o755):p.error('Invalid program file mode: '+name)
        for name,mode in programs.get('directories',{}).items():
            if PurePosixPath(name).is_absolute() or '..' in PurePosixPath(name).parts or mode not in (0o700,0o755):p.error('Invalid program directory: '+name)
        for name,digest in programs['source_sha256'].items():
            if sha(ROOT/name)!=digest:p.error('Program port recipe/source changed: '+name)
        for name,target in programs['aliases'].items():
            resolved=posixpath.normpath(target.lstrip('/') if target.startswith('/') else str(PurePosixPath(name).parent/target))
            if not name.startswith('usr/') or '..' in PurePosixPath(name).parts or not resolved.startswith('usr/'):p.error('Invalid program alias: '+name)
            visited={name}
            while resolved in programs['aliases']:
                if resolved in visited:p.error('Cyclic program alias: '+name)
                visited.add(resolved)
                link=programs['aliases'][resolved]
                resolved=posixpath.normpath(link.lstrip('/') if link.startswith('/') else str(PurePosixPath(resolved).parent/link))
                if not resolved.startswith('usr/'):p.error('Program alias escapes /usr: '+name)
            if resolved not in programs['files']:p.error('Program alias has no packaged file: '+name)
    broker_record=json.loads(a.broker_build.read_text()) if a.broker_build else None
    if broker_record:
        if broker_record['target_host_executed'] is not False or sha(Path(broker_record['binary']))!=broker_record['binary_sha256']:p.error('Browser service rebuild changed')
        for name,digest in broker_record['source_sha256'].items():
            if sha(ROOT/name)!=digest:p.error('Browser service source changed: '+name)
    browser_record=json.loads(a.browser_build.read_text()) if a.browser_build else None
    if browser_record:
        if browser_record['target_host_executed'] is not False or sha(Path(browser_record['binary']))!=browser_record['binary_sha256']:p.error('Browser rebuild changed')
        for name,digest in browser_record['source_sha256'].items():
            if sha(ROOT/name)!=digest:p.error('Browser platform source changed: '+name)
    qt_record=json.loads(a.qt_build.read_text()) if a.qt_build else None
    if qt_record and (qt_record.get('target_host_executed') is not False or sha(Path(qt_record['binary']))!=qt_record['binary_sha256']):p.error('Qt port differs from its completed build record')
    if qt_record:
        for name,digest in qt_record['source_sha256'].items():
            if sha(ROOT/name)!=digest:p.error('Qt port source changed after compilation: '+name)
        archive=ROOT/('build/qt6/vendor/qtbase-everywhere-src-'+qt_record['version']+'.tar.xz')
        if sha(archive)!=qt_record['source']['sha256']:p.error('Qt source archive differs from its build record')
    artifacts, base, out = a.artifacts.resolve(), a.base_iso.resolve(), a.output_root.resolve()
    startup_cache = json.loads(a.startup_cache.read_text()) if a.startup_cache else None
    if startup_cache and (startup_cache['state'] != 'generated in isolated guest' or sha(Path(startup_cache['archive'])) != startup_cache['archive_sha256']):
        p.error('Startup cache differs from its generation record')
    runtime_record = json.loads(a.runtime_build.read_text()) if a.runtime_build else None
    runtime_binary = Path(runtime_record['binary']) if runtime_record else None
    runtime_hash = runtime_record['binary_sha256'] if runtime_record else ELECTRON_HASH
    runtime_sidecars=runtime_record.get('sidecars',{}) if runtime_record else {}
    allowed_sidecars={'snapshot_blob.bin','resources.pak','chrome_100_percent.pak','chrome_200_percent.pak','icudtl.dat','libffmpeg.so'}
    for name,asset in runtime_sidecars.items():
        if name not in allowed_sidecars or sha(Path(asset['path']))!=asset['sha256']:
            p.error('Runtime sidecar differs from its completed build record: '+name)
    native_node=json.loads(a.native_node_build.read_text()) if a.native_node_build else None
    if native_node:
        if native_node['parent_electron_sha256']!=runtime_hash or sha(Path(native_node['binary']))!=native_node['binary_sha256']:
            p.error('Standalone Node does not match its completed build and selected Electron runtime')
        if sha(Path(native_node['license']['path']))!=native_node['license']['sha256']:
            p.error('Standalone Node license differs from its build record')
        for name,asset in native_node['sidecars'].items():
            if name not in ['snapshot_blob.bin','icudtl.dat'] or runtime_sidecars.get(name,{}).get('sha256')!=asset['sha256']:
                p.error('Standalone Node requires matching V8/ICU sidecars: '+name)
    oniguruma_record=json.loads(a.oniguruma_build.read_text()) if a.oniguruma_build else None
    if oniguruma_record:
        if sha(Path(oniguruma_record['asar']))!=oniguruma_record['asar_sha256'] or sha(Path(oniguruma_record['license']))!=oniguruma_record['license_sha256']:
            p.error('Translated Oniguruma assets differ from their build record')
    if runtime_binary and sha(runtime_binary) != runtime_hash:
        p.error('Selected Electron runtime differs from its completed build record')
    if startup_cache and startup_cache['runtime'] != runtime_hash:
        p.error('Startup cache targets a different Electron runtime')
    bundle = artifacts.parent / 'codeoss-app-a0/vendor/VSCodium-linux-x64-1.140.06880.tar.gz'
    xkb_deb = a.xkbfile_deb or artifacts / 'workbench-2026-11-01/vendor/libxkbfile1_1.1.0-1_amd64.deb'
    xkb_hash = '7c58d9986f918b71568ad83dbb6f4ab22c185f243461d41acee920cc5e13d347'
    if not base.is_file() or out.exists():
        p.error('A preserved input ISO and an unused output directory are required')
    if sha(bundle) != BUNDLE_HASH:
        p.error('Cached workbench bundle does not match its pinned provenance')
    if sha(xkb_deb) != xkb_hash:
        p.error('Pinned libxkbfile package does not match its cached Debian index')
    # The fixture directory is shared by both builders and runtime packaging.
    with (artifacts / 'test/bridge-build.lock').open('a') as build_lock, Path(str(base) + '.lock').open('a') as image_lock:
        fcntl.flock(build_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        fcntl.flock(image_lock, fcntl.LOCK_SH | fcntl.LOCK_NB)
        base_hash = sha(base)
        out.mkdir(parents=True)
        source = ROOT / 'userland/electron-wayland'
        for options in [[], ['--workbench']]:
            subprocess.run([sys.executable, '-B', str(artifacts / 'test/build_bridge_local.py'),
                            '--source', str(source), '--kernel-source', str(ROOT), *options], check=True)
        for name in ['BRIDGE-LOCAL-BUILD.json', 'CODEOSS-BRIDGE-BUILD.json']:
            shutil.copyfile(artifacts / 'test' / name, out / name)
        subprocess.run([sys.executable, '-B', str(ROOT / 'tools/build_native_gui.py'),
                        '--output-root', str(out / 'gui-build'), '--artifacts', str(artifacts)], check=True)
        files = {
            'bin/gui': out / 'gui-build/gui',
            'bin/coast-sh': out / 'gui-build/coast-sh',
            'bin/passwd': out / 'gui-build/passwd',
            'bin/login': out / 'gui-build/login',
            'bin/id': out / 'gui-build/id',
            'bin/whoami': out / 'gui-build/whoami',
            'bin/sudo': out / 'gui-build/sudo',
            'bin/clip': out / 'gui-build/clip',
            'bin/profile-session': out / 'gui-build/profile-session',
            'bin/vector-io-check': out / 'gui-build/vector-io-check',
            'bin/web-renderer': Path(browser_record['binary']) if browser_record else ROOT / 'userland/bin/web-renderer',
            'bin/web-broker': Path(broker_record['binary']) if broker_record else ROOT / 'userland/bin/web-broker',
            'bin/clipboard-check': ROOT / 'userland/bin/clipboard-check',
            'usr/include/coast/clipboard.h': ROOT / 'include/uapi/clipboard.h',
            'bin/electron-wayland-demo': artifacts / 'test/fixtures/electron-wayland-demo-local',
            'bin/codeoss-wayland': artifacts / 'test/fixtures/codeoss-wayland-local',
            'bin/code': out / 'gui-build/code-launch',
            'usr/bin/code': out / 'gui-build/code-launch',
            'bin/g2-wayland-launch': artifacts / 'test/fixtures/g2-wayland-launch-local',
            'bin/private-stream-check': artifacts / 'test/fixtures/private-stream-check',
            'bin/file-copy-check': artifacts / 'test/fixtures/file-copy-check',
            'bin/inotify-check': artifacts / 'test/fixtures/inotify-check',
            'bin/trash-put': artifacts / 'test/fixtures/trash-put',
            'bin/private-pty-check': artifacts / 'test/fixtures/private-pty-check',
            'bin/font-fallback-check': artifacts / 'test/fixtures/font-fallback-check',
            'bin/read-prefix-check': artifacts / 'test/fixtures/read-prefix-check',
            'bin/null-device-check': artifacts / 'test/fixtures/null-device-check',
            'bin/seccomp-check': artifacts / 'test/fixtures/seccomp-check',
            'bin/private-loopback-check': artifacts / 'test/fixtures/private-loopback-check',
            'bin/native-node-launch-check': artifacts / 'test/fixtures/native-node-launch-check',
            'usr/share/codeoss-workbench/out/coast-extension-reads.mjs': source / 'bounded-extension-reads.mjs',
            **({'usr/share/codeoss-workbench/out/coast-diagnostics.mjs': source / 'workbench-diagnostics.mjs'} if a.diagnostics else {}),
            'bin/pixel-window-check': artifacts / 'test/fixtures/pixel-window-check',
            'bin/namespace-c4-check': artifacts / 'test/fixtures/namespace-check-local',
            'etc/fonts/fonts.conf': source / 'fonts.conf',
            'usr/share/licenses/coast-dejavu/LICENSE': a.font_license,
            'usr/share/fonts/coast/NotoSansCJK-Regular.ttc':a.cjk_font,
            'usr/share/licenses/coast-noto-cjk/LICENSE':a.cjk_license,
            'usr/share/fonts/coast/NotoColorEmoji.ttf':a.emoji_font,
            'usr/share/licenses/coast-noto-emoji/LICENSE':a.emoji_license,
        }
        if native_node:
            files['opt/electron-43.7.3/node']=Path(native_node['binary'])
            files['usr/share/licenses/coast-native-node/LICENSE']=Path(native_node['license']['path'])
        if programs:
            for name,asset in programs['files'].items():
                if name in files:raise ValueError('Program port collides with desktop payload: '+name)
                files[name]=Path(asset['path'])
        if qt_record:
            files['usr/bin/qt6-demo']=Path(qt_record['binary'])
            files['usr/src/qt6/qtbase-'+qt_record['version']+'.tar.xz']=ROOT/('build/qt6/vendor/qtbase-everywhere-src-'+qt_record['version']+'.tar.xz')
            for path in (ROOT/'userland/qt6').iterdir():
                if path.is_file():files['usr/src/coast/qt6/'+path.name]=path
            qt_source=Path(qt_record.get('source_directory',a.qt_build.resolve().parent/('qtbase-everywhere-src-'+qt_record['version'])))
            for path in (qt_source/'LICENSES').iterdir():
                if path.is_file():files['usr/share/licenses/qt6/'+path.name]=path

        for name in ['DejaVuSans.ttf', 'DejaVuSans-Bold.ttf', 'DejaVuSansMono.ttf', 'DejaVuSansMono-Bold.ttf']:
            files['usr/share/fonts/coast/' + name] = a.font_dir / name
        if runtime_binary:
            files['opt/electron-43.7.3/electron'] = runtime_binary
        for name,asset in runtime_sidecars.items():
            files['opt/electron-43.7.3/'+name]=Path(asset['path'])
        if oniguruma_record:
            files['usr/share/licenses/coast-oniguruma-js/Binaryen-LICENSE']=Path(oniguruma_record['license'])
        payload, native, transformations = {}, {}, {}
        cursor_assets = build_cursor_theme(out / 'cursor-theme')
        overlay = out / 'codeoss-runtime.tar'
        seen = set()
        with tarfile.open(overlay, 'w', format=tarfile.USTAR_FORMAT) as dest:
            def add(name, data, mode=0o644, kind=tarfile.REGTYPE, link='', uid=0):
                for parent in reversed(PurePosixPath(name).parents):
                    key = str(parent)
                    if key != '.' and key not in seen:
                        entry = tarfile.TarInfo(key)
                        entry.type, entry.mode = tarfile.DIRTYPE, programs.get('directories',{}).get(key,0o755) if programs else 0o755
                        if key.startswith(('home/port-test/.cache/codeoss-compile', 'home/port-test/.cache/fontconfig')):
                            entry.mode = 0o700
                            entry.uid = entry.gid = 1000
                        dest.addfile(entry)
                        seen.add(key)
                if name in seen:
                    raise ValueError('Duplicate payload path: ' + name)
                entry = tarfile.TarInfo(name)
                entry.type, entry.mode, entry.linkname = kind, mode, link
                if programs and (name in programs['files'] or name in programs['aliases']):entry.mtime=programs.get('mtime',0)
                entry.uid = entry.gid = uid
                entry.size = len(data)
                dest.addfile(entry, io.BytesIO(data) if data else None)
                seen.add(name)
                if kind == tarfile.REGTYPE:
                    payload[name] = hashlib.sha256(data).hexdigest()
            if programs:
                for name,mode in sorted(programs.get('directories',{}).items(),key=lambda pair:(pair[0].count('/'),pair[0])):
                    if name not in seen:add(name,b'',mode,tarfile.DIRTYPE)
            for name, path in files.items():
                mode=0o4755 if name in ['bin/sudo','bin/code','usr/bin/code'] else 0o755 if name.startswith(('bin/','usr/bin/')) or name in ['opt/electron-43.7.3/electron','opt/electron-43.7.3/node'] else 0o644
                if programs and name in programs['files']:mode=programs['files'][name].get('mode',mode)
                add(name, path.read_bytes(),mode)
            if programs:
                for name,target in programs['aliases'].items():add(name,b'',0o777,tarfile.SYMTYPE,target)
            for name, data in cursor_assets.items():
                add(name, data)
            expected_runtime={name:asset['sha256'] for name,asset in runtime_sidecars.items()}
            if runtime_binary:expected_runtime['electron']=runtime_hash
            if native_node:expected_runtime['node']=native_node['binary_sha256']
            for name,digest in expected_runtime.items():
                if payload['opt/electron-43.7.3/'+name]!=digest:
                    raise ValueError('Runtime asset changed during packaging: '+name)
            add('usr/share/coast/desktop-defaults.ini', b'scale=3\nterminal_shell=1\n')
            archive_member = next(name for name in subprocess.check_output(['ar', 't', str(xkb_deb)], text=True).splitlines() if name.startswith('data.tar.'))
            dependency_archive = subprocess.check_output(['ar', 'p', str(xkb_deb), archive_member])
            with tarfile.open(fileobj=io.BytesIO(dependency_archive)) as dependency:
                for src_name, target in [('./usr/lib/x86_64-linux-gnu/libxkbfile.so.1.0.2', 'usr/lib/x86_64-linux-gnu/libxkbfile.so.1'),
                                         ('./usr/share/doc/libxkbfile1/copyright', 'usr/share/licenses/codeoss-libxkbfile1/copyright')]:
                    add(target, dependency.extractfile(src_name).read())
            product_data=None
            with tarfile.open(bundle) as src:
                for entry in src:
                    name = entry.name.removeprefix('./')
                    if not name.startswith('resources/app/') or entry.isdir():
                        continue
                    relative = name[len('resources/app/'):]
                    if '..' in PurePosixPath(relative).parts or relative.startswith('/'):
                        raise ValueError('Unsafe archive path: ' + name)
                    target = 'usr/share/codeoss-workbench/' + relative
                    if entry.isfile():
                        data = src.extractfile(entry).read()
                        if relative=='product.json':
                            product_data=data
                            continue
                        if a.rpc_trace and relative in ('out/vs/workbench/api/node/extensionHostProcess.js', 'out/vs/workbench/workbench.desktop.main.js'):
                            transformations[target] = dict(original_sha256=hashlib.sha256(data).hexdigest(), rpc_trace=True)
                            data = instrument_rpc(data)
                            if a.wire_trace:
                                transformations[target]['wire_trace'] = True
                                data = instrument_wire(data)
                        if relative in ['out/main.js', 'out/bootstrap-fork.js']:
                            transformations[target] = dict(original_sha256=hashlib.sha256(data).hexdigest(),
                                metadata_queue_sha256=sha(source / 'bounded-extension-reads.mjs'))
                            data = b"import '/usr/share/codeoss-workbench/out/coast-extension-reads.mjs';\n" + data
                        if relative=='node_modules.asar' and oniguruma_record:
                            if hashlib.sha256(data).hexdigest()!=oniguruma_record['original_asar_sha256']:
                                raise ValueError('Translated Oniguruma targets a different ASAR')
                            transformations[target]=dict(original_sha256=hashlib.sha256(data).hexdigest(),oniguruma_build=str(a.oniguruma_build.resolve()),oniguruma_build_sha256=sha(a.oniguruma_build))
                            data=Path(oniguruma_record['asar']).read_bytes()
                        if relative == 'out/vs/workbench/workbench.desktop.main.css':
                            transformations[target] = dict(original_sha256=hashlib.sha256(data).hexdigest(),
                                                           native_styles_sha256=sha(source / 'workbench-native.css'))
                            data += b'\n' + (source / 'workbench-native.css').read_bytes()
                        if relative == 'out/mainImpl.js':
                            # The desktop already supplies the window shadow
                            # and rounded outer frame. Electron's second full-
                            # window mask adds offscreen software render passes.
                            # Set supported constructor options for frameless
                            # windows, keeping the existing custom HTML menus.
                            marker = b'backgroundThrottling:!1}),c}function Ag'
                            replacement = b'backgroundThrottling:!1}),c.frame===!1&&(c.hasShadow=!1,c.roundedCorners=!1),c}function Ag'
                            if data.count(marker) != 1:
                                raise ValueError('Pinned workbench window-options factory changed')
                            transformations[target] = dict(original_sha256=hashlib.sha256(data).hexdigest(),
                                                           native_desktop_frame=True)
                            data = data.replace(marker, replacement)
                        if relative == 'out/main.js':
                            transformations[target].update(entry_prelude_sha256=sha(source / 'workbench-entry.mjs'))
                            data = (source / 'workbench-entry.mjs').read_bytes() + b'\n' + data
                            if a.diagnostics:
                                transformations[target]['diagnostic_prelude_sha256'] = sha(source / 'workbench-diagnostics.mjs')
                                data = b"if(process.env.COAST_CODEOSS_QA==='1')await import('/usr/share/codeoss-workbench/out/coast-diagnostics.mjs');\n" + data
                        add(target, data, 0o755 if entry.mode & 0o111 else 0o644)
                        if name.endswith('.node'):
                            native[target] = payload[target]
                    elif entry.issym():
                        resolved = posixpath.normpath(posixpath.join(posixpath.dirname(relative), entry.linkname))
                        if entry.linkname.startswith('/') or resolved == '..' or resolved.startswith('../'):
                            raise ValueError('External payload symlink: ' + name)
                        add(target, b'', kind=tarfile.SYMTYPE, link=entry.linkname)
                    else:
                        raise ValueError('Unsupported payload entry: ' + name)
            # Keep the upstream integrity check enabled and describe the
            # actual installed port payload, including our native stylesheet.
            # Otherwise its delayed check calls a correctly ported app corrupt.
            if product_data is None:
                raise ValueError('Pinned workbench has no product manifest')
            product=json.loads(product_data)
            checksum_updates={}
            for name,original in product['checksums'].items():
                target='usr/share/codeoss-workbench/out/'+name
                if target not in payload:
                    raise ValueError('Missing integrity-checked workbench asset: '+name)
                actual=base64.b64encode(bytes.fromhex(payload[target])).decode().rstrip('=')
                if actual!=original:
                    checksum_updates[name]=dict(original=original,installed=actual)
                    product['checksums'][name]=actual
            target='usr/share/codeoss-workbench/product.json'
            transformations[target]=dict(original_sha256=hashlib.sha256(product_data).hexdigest(),
                                          installed_checksum_updates=checksum_updates)
            add(target,(json.dumps(product,indent=2)+'\n').encode())
            if startup_cache:
                installed = {name: digest for name, digest in payload.items()
                             if name.startswith('usr/share/codeoss-workbench/')}
                if installed != startup_cache['sources']:
                    raise ValueError('Startup cache application sources changed; regenerate it in the guest')
                with tarfile.open(startup_cache['archive']) as cache:
                    total = 0
                    for entry in cache:
                        relative = entry.name.removeprefix('./')
                        if entry.isdir() or not entry.size: continue
                        parts = PurePosixPath(relative).parts
                        if not entry.isfile() or not parts or '..' in parts or relative.startswith('/'):
                            raise ValueError('Invalid startup cache entry: ' + entry.name)
                        total += entry.size
                        if total > 128 * 1024 * 1024:
                            raise ValueError('Startup cache exceeds its packaging bound')
                        if not re.fullmatch(r'[0-9a-f]{8}', parts[-1]):
                            raise ValueError('Startup cache contains an unfinished file: ' + relative)
                        add('home/port-test/.cache/codeoss-compile/' + relative,
                            cache.extractfile(entry).read(), 0o600, uid=1000)
            if startup_cache and startup_cache.get('font_cache'):
                font_cache = startup_cache['font_cache']
                installed = {name: digest for name, digest in payload.items()
                             if name.startswith('usr/share/fonts/coast/') or name == 'etc/fonts/fonts.conf'}
                if installed != font_cache['sources'] or sha(Path(font_cache['archive'])) != font_cache['archive_sha256']:
                    raise ValueError('Font cache differs from its font/configuration inputs')
                with tarfile.open(font_cache['archive']) as cache:
                    total = 0
                    for entry in cache:
                        relative = entry.name.removeprefix('./')
                        if entry.isdir() or not entry.size: continue
                        if not entry.isfile() or not re.fullmatch(r'[0-9a-f]+-le64[.]cache-[0-9]+|CACHEDIR[.]TAG', relative):
                            raise ValueError('Invalid font cache entry: ' + entry.name)
                        total += entry.size
                        if total > 16 * 1024 * 1024: raise ValueError('Font cache exceeds its packaging bound')
                        add('home/port-test/.cache/fontconfig/' + relative, cache.extractfile(entry).read(), 0o600, uid=1000)
            # Keep the sources that produced the native integration alongside it.
            for path in source.iterdir():
                if path.is_file():
                    add('usr/src/coast/electron-wayland/' + path.name, path.read_bytes())
        config = out / 'limine.conf'
        subprocess.run(['xorriso', '-osirrox', 'on', '-indev', str(base), '-extract',
                        '/boot/limine/limine.conf', str(config)], check=True, capture_output=True)
        text = config.read_text()
        text, resolutions = re.subn(r'(?m)^(\s*(?:interface_)?resolution:)\s*\S+', r'\1 2560x1600x32', text)
        if not resolutions:
            text = text.replace('    protocol: limine\n', '    protocol: limine\n    resolution: 2560x1600x32\n', 1)
        if 'resolution: 2560x1600x32' not in text:
            raise ValueError('Cannot set the native2560x1600 boot display mode')
        needle = '    module_path: boot():/boot/codeoss-runtime.tar\n    module_string: initramfs\n'
        if 'codeoss-runtime.tar' in text:
            raise ValueError('Input ISO already contains a workbench overlay')
        point = text.index('\n/Reboot')
        config.write_text(text[:point] + needle + text[point:])
        iso = out / 'Coast-codeoss.iso'
        subprocess.run(['cp', '--reflink=auto', str(base), str(iso)], check=True)
        subprocess.run(['xorriso', '-no_rc', '-dev', str(iso), '-map', str(overlay),
                        '/boot/codeoss-runtime.tar', '-map', str(config), '/boot/limine/limine.conf',
                        '-boot_image', 'any', 'keep', '-commit', '-end'], check=True)
        layout = subprocess.run(['xorriso', '-no_rc', '-indev', str(iso), '-find',
                                 '/boot/kernel.elf', '-exec', 'report_lba', '--'],
                                check=True, capture_output=True, text=True)
        match = re.search(r"File data lba:\s*0\s*,\s*(\d+)\s*,\s*\d+\s*,\s*(\d+)\s*,\s*'/boot/kernel.elf'", layout.stdout)
        if not match:
            raise ValueError('Cannot locate the preserved kernel')
        lba, size = map(int, match.groups())
        with iso.open('rb') as stream:
            stream.seek(lba * 2048)
            kernel_hash = hashlib.sha256(stream.read(size)).hexdigest()
        if sha(base) != base_hash:
            raise ValueError('Input image changed during packaging')
        record = dict(startup_cache=dict(path=str(a.startup_cache.resolve()), sha256=sha(a.startup_cache)) if a.startup_cache else None,
                      program_ports_build=dict(path=str(a.program_ports_build.resolve()),sha256=sha(a.program_ports_build)) if a.program_ports_build else None,
                      qt_build=dict(path=str(a.qt_build.resolve()),sha256=sha(a.qt_build),binary_sha256=qt_record['binary_sha256']) if a.qt_build else None,
                      native_node_build=str(a.native_node_build.resolve()) if a.native_node_build else None,
                      native_node_sha256=native_node['binary_sha256'] if native_node else None,
                      state='built; guest workbench validation pending', base_iso=str(base),
                      base_iso_sha256=base_hash, iso=str(iso), iso_sha256=sha(iso),
                      kernel_sha256=kernel_hash, bundle=str(bundle), bundle_sha256=BUNDLE_HASH,
                      canonical_electron_sha256=runtime_hash, overlay_sha256=sha(overlay),
                      runtime_build=dict(path=str(a.runtime_build.resolve()),sha256=sha(a.runtime_build)) if a.runtime_build else None,
                      oniguruma_build=dict(path=str(a.oniguruma_build.resolve()),sha256=sha(a.oniguruma_build)) if a.oniguruma_build else None,
                      config_sha256=sha(config), files=payload, native_modules=native,
                      diagnostic_transformations=transformations,
                      libxkbfile=dict(package=str(xkb_deb.resolve()),sha256=xkb_hash,version='1:1.1.0-1',
                                     source_index=str(artifacts.parent/'codeoss-app-a3/vendor/main-Packages.xz')),
                      local_assets={name:dict(source=str(path.resolve()),sha256=sha(path)) for name,path in files.items() if not name.startswith('bin/')},
                      kernel_rebuilt=False, host_editor_execution=False,
                      rpc_trace=a.rpc_trace, wire_trace=a.wire_trace, rpc_relay=False, diagnostics_qa_only=a.diagnostics,
                      framebuffer=dict(width=2560,height=1600,desktop_scale=200),native_modifier_trace=True,
                      confinement='existing read-only, network-unshared bridge compiler',
                      limits=['one2400x1300 physical-pixel workbench;1200x650 logical at2x', 'jitless', 'software pixels',
                              'folder access follows guest inode permissions; RAM files are not persistent'])
        atomic_write(out / 'MANIFEST.json', json.dumps(record, indent=2) + '\n')
        print('ISO ready:', iso, flush=True)


if __name__ == '__main__':
    with project_lock():
        main()

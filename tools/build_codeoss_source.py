#!/usr/bin/env python3
"""Prepare pinned VSCodium sources and compile the workbench application.

This builds the application separately from Coast's Electron runtime. Upstream
NPM lockfiles select build dependencies; native addons are requested from source.
The generated application still needs image assembly and Coast qualification.
"""
import argparse
import fcntl
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess

from fetch_sources import fetch
from prepare_electron_host import prepare_node
from source_archive import unpack
from source_tree import digest

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ROOT / 'ports/codeoss-app/SOURCES.json'
PACKAGING_PATCH = ROOT / 'ports/codeoss-app/application-only.patch'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-root', type=Path, default=ROOT / 'build/codeoss-source-build')
    parser.add_argument('--electron-checkout', type=Path,
                        help='verified Electron source checkout containing its pinned Linux host tools')
    parser.add_argument('--native', action='store_true')
    parser.add_argument('--host-bin', type=Path, help='native Node/npm build tools on Coast')
    parser.add_argument('--prepare', action='store_true', help='apply VSCodium branding and patches, then install locked dependencies')
    parser.add_argument('--build', action='store_true', help='compile and minimize the workbench and extensions')
    parser.add_argument('--package-app', action='store_true', help='package the application without downloading or running Electron')
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--reconfigure', action='store_true',
                        help='record a changed builder while retaining the same pinned prepared sources')
    args = parser.parse_args()
    if not 1 <= args.jobs <= 24 or args.native and platform.system() != 'Coast':
        parser.error('Use --jobs 1..24; --native requires Coast')
    if not args.prepare and not args.build and not args.package_app:
        parser.error('Select --prepare, --build and/or --package-app')
    if args.native and not args.host_bin or not args.native and not args.electron_checkout:
        parser.error('Select --host-bin on Coast or --electron-checkout on Linux')
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=True)
    lock = (out / '.build.lock').open('a')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    # esbuild probes ancestor directories for package.json using a
    # case-insensitive directory lookup. A PACKAGE.json receipt in this
    # ancestor makes a second compilation fail on case-sensitive Linux.
    # Keep the receipt, under a name that cannot shadow the NPM manifest.
    legacy_package = out / 'PACKAGE.json'
    packaged = out / 'PACKAGED.json'
    if legacy_package.exists():
        if packaged.exists() and digest(packaged) != digest(legacy_package):
            raise ValueError('Conflicting legacy application packaging receipts')
        legacy_package.replace(packaged)
    pins = json.loads(SOURCES.read_text())
    vendor, workspace = out / 'vendor', out / 'workspace'
    vendor.mkdir(exist_ok=True)
    identity = dict(sources_sha256=digest(SOURCES), builder_sha256=digest(Path(__file__)),
                    packaging_patch_sha256=digest(PACKAGING_PATCH))
    source_receipt = out / 'SOURCE.json'
    if source_receipt.exists():
        previous = json.loads(source_receipt.read_text())
        if previous != identity:
            if previous['sources_sha256'] != identity['sources_sha256'] or not args.reconfigure:
                parser.error('Code OSS build inputs changed; select a fresh output root or use --reconfigure for builder-only changes')
            history = out / 'configuration-history'
            history.mkdir(exist_ok=True)
            snapshot = history / (digest(source_receipt) + '.json')
            if not snapshot.exists():
                shutil.copyfile(source_receipt, snapshot)
            source_receipt.write_text(json.dumps(identity, indent=2) + '\n')
    else:
        if workspace.exists():
            parser.error('Unrecognized application source workspace; select a fresh output root')
        for name in ['vscodium', 'vscode']:
            record = pins[name]
            cached = ROOT / 'build/codeoss-source/vendor' / record['archive']
            if cached.is_file():
                if digest(cached) != record['sha256']:
                    raise ValueError('Code OSS source cache checksum mismatch')
                shutil.copyfile(cached, vendor / cached.name)
            fetch(record, vendor)
            unpack(vendor / record['archive'], out)
        shutil.move(out / pins['vscodium']['source'], workspace)
        shutil.move(out / pins['vscode']['source'], workspace / 'vscode')
        # Upstream postinstall records local repository settings. This generated
        # checkout needs no commits, user identity, remotes or credentials.
        subprocess.run(['git', 'init', '--quiet', '--initial-branch=source-build', str(workspace / 'vscode')], check=True)
        source_receipt.write_text(json.dumps(identity, indent=2) + '\n')
    source = workspace / 'vscode'
    host_record = None
    if args.native:
        host_bin = args.host_bin.resolve(strict=True)
    else:
        from build_electron_source import verify
        checkout = args.electron_checkout.resolve(strict=True)
        verify(checkout, json.loads((ROOT / 'ports/electron/sources.lock.json').read_text()))
        host_bin, host_record = prepare_node(out / 'host-tools', checkout / 'src')
    home = out / 'home'
    home.mkdir(exist_ok=True)
    environment = dict(PATH=str(host_bin) + ':/usr/bin:/bin', HOME=str(home), TMPDIR='/tmp', LANG='C.UTF-8',
        PYTHONDONTWRITEBYTECODE='1', GIT_TERMINAL_PROMPT='0',
        RELEASE_VERSION=pins['release'], MS_COMMIT=pins['vscode']['commit'],
        BUILD_SOURCEVERSION=pins['build_sourceversion']['sha1'],
        OS_NAME='linux', VSCODE_QUALITY='stable', VSCODE_ARCH='x64',
        CI_BUILD='yes', SHOULD_BUILD='yes', SHOULD_BUILD_REH='no', SHOULD_BUILD_REH_WEB='no',
        DISABLE_UPDATE='yes', APP_NAME='VSCodium', APP_NAME_LC='vscodium',
        ASSETS_REPOSITORY='VSCodium/vscodium', BINARY_NAME='codium', GH_REPO_PATH='VSCodium/vscodium',
        ORG_NAME='VSCodium', TUNNEL_APP_NAME='codium-tunnel', GLOBAL_DIRNAME='VSCodium',
        MAX_OLD_SPACE_SIZE='4096', NODE_OPTIONS='--max-old-space-size=4096',
        ELECTRON_SKIP_BINARY_DOWNLOAD='1', PLAYWRIGHT_SKIP_BROWSER_DOWNLOAD='1',
        VSCODE_SKIP_NODE_VERSION_CHECK='1', npm_config_build_from_source='true',
        npm_config_cache=str(out / 'npm-cache'), npm_config_update_notifier='false',
        npm_config_fund='false', npm_config_audit='false', npm_config_jobs=str(args.jobs),
        # Every postinstall workspace uses its checked-in lockfile.
        npm_command='ci', npm_config_devdir=str(out / 'node-gyp'))
    # Network is shared for pinned source/NPM downloads. All host paths remain
    # read-only except this build directory; no user home or credentials enter
    # the sanitized build environment.
    confinement = [] if args.native else ['bwrap', '--unshare-user', '--unshare-pid', '--unshare-ipc',
        '--unshare-uts', '--die-with-parent', '--new-session', '--ro-bind', '/', '/',
        '--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp', '--bind', str(out), str(out)]
    commands = []

    def run(name, command, directory=source):
        command = list(map(str, command))
        commands.append(dict(name=name, argv=command, cwd=str(directory)))
        print('Code OSS ' + name, flush=True)
        with (out / (name + '.log')).open('w') as log:
            subprocess.run([*confinement, '--chdir', str(directory), '--', *command] if confinement else command,
                cwd=directory, env=environment, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, check=True)

    if args.prepare:
        # Preserve upstream's exact preparation commands while checkpointing
        # source patching separately from the resumable NPM install phase.
        script = (workspace / 'prepare_vscode.sh').read_text()
        first = '# {{{ install dependencies\n'
        second = '# }}}\n\n# package.json\n'
        if script.count(first) != 1 or script.split(first)[1].count(second) != 1:
            raise ValueError('Upstream preparation stages changed; review the pinned recipe')
        prefix, rest = script.split(first)
        install, suffix = rest.split(second)
        (out / 'prepare-prefix.sh').write_text(prefix)
        (out / 'prepare-install.sh').write_text('set -e\n' + install)
        (out / 'prepare-suffix.sh').write_text('set -e\n# package.json\n' + suffix)
        patched = out / 'PATCHED.json'
        functions = out / 'prepare-functions.sh'
        if not patched.exists():
            run('patch', ['bash', '-c', '. "$1"; declare -f > "$2"', 'coast-codeoss',
                out / 'prepare-prefix.sh', functions], workspace)
            patched.write_text(json.dumps(dict(source_identity=identity,
                product_sha256=digest(source / 'product.json'),
                npm_lock_sha256=digest(source / 'package-lock.json')), indent=2) + '\n')
        if not (out / 'INSTALLED.json').exists():
            run('npm', ['bash', out / 'prepare-install.sh'])
            locks = {str(p.relative_to(source)): digest(p) for p in source.rglob('package-lock.json')
                     if 'node_modules' not in p.relative_to(source).parts}
            (out / 'INSTALLED.json').write_text(json.dumps(dict(lockfiles=locks, host_tools=host_record,
                native_addons_requested_from_source=True), indent=2) + '\n')
        if not (out / 'PREPARED.json').exists():
            run('branding', ['bash', '-c', '. "$1"; . "$2"', 'coast-codeoss', functions, out / 'prepare-suffix.sh'])
            (out / 'PREPARED.json').write_text(json.dumps(dict(source_identity=identity,
                package_sha256=digest(source / 'package.json'), product_sha256=digest(source / 'product.json')),
                indent=2) + '\n')
    if args.build:
        if not (out / 'PREPARED.json').is_file():
            parser.error('Complete --prepare first')
        environment['VSCODE_PUBLISH_COUNTER'] = '1'
        run('compile', ['npm', 'run', 'gulp', 'vscode-min-prepack'])
        # Upstream's named esbuild subtasks are composed into registered Gulp
        # tasks, but are not themselves registered CLI tasks. Invoke the same
        # desktop bundler entry point used by runEsbuildBundle, without also
        # packaging/downloading a different Electron runtime.
        run('compile-workbench', ['node', '--experimental-strip-types', 'build/next/index.ts',
            'bundle', '--out', 'out-vscode-min', '--target', 'desktop', '--minify', '--nls',
            '--source-map-base-url', 'https://github.com/VSCodium/sourcemaps/releases/download/stable-' +
            pins['vscode']['commit'] + '/core'])
        products = source / 'out-vscode-min'
        if not (products / 'main.js').is_file():
            raise ValueError('Upstream build did not produce the application entry point')
        inputs = [Path(__file__), SOURCES, ROOT / 'tools/prepare_electron_host.py',
            ROOT / 'ports/electron/node-host.json', ROOT / 'tools/fetch_sources.py', ROOT / 'tools/source_archive.py', PACKAGING_PATCH]
        (out / 'BUILD.json').write_text(json.dumps(dict(version=pins['release'], sources=pins,
            application_directory=str(products), commands=commands,
            files={str(p.relative_to(products)): dict(path=str(p), sha256=digest(p))
                   for p in sorted(products.rglob('*')) if p.is_file()},
            host_tools=host_record, native_addons_requested_from_source=True,
            configuration_history={p.name: digest(p) for p in sorted((out / 'configuration-history').glob('*.json'))},
            source_sha256={str(p.relative_to(ROOT)): digest(p) for p in inputs},
            build_mode='native-coast' if args.native else 'linux-host',
            application_executed_on_linux=False,
            state='application compiled; runtime packaging and Coast qualification pending'), indent=2) + '\n')
    if args.package_app:
        compilation = out / 'BUILD.json'
        if not compilation.is_file() or not (source / 'out-vscode-min/main.js').is_file():
            parser.error('Complete --build before packaging')
        stamp = out / 'APPLICATION-PACKAGING.json'
        gulpfile = source / 'build/gulpfile.vscode.ts'
        if not stamp.exists():
            run('packaging-patch', ['patch', '-p1', '-i', PACKAGING_PATCH])
            stamp.write_text(json.dumps(dict(patch_sha256=digest(PACKAGING_PATCH),
                gulpfile_sha256=digest(gulpfile)), indent=2) + '\n')
        expected = json.loads(stamp.read_text())
        if expected != dict(patch_sha256=digest(PACKAGING_PATCH), gulpfile_sha256=digest(gulpfile)):
            raise ValueError('Application packaging patch changed; select a fresh source workspace')
        environment['COAST_APPLICATION_ONLY'] = '1'
        environment['VSCODE_PUBLISH_COUNTER'] = '1'
        run('package', ['npm', 'run', 'gulp', 'coast-application-package'])
        application = workspace / 'Coast-application-linux-x64/resources/app'
        if not all((application / relative).is_file() for relative in ['package.json', 'product.json', 'out/main.js']):
            raise ValueError('Incomplete packaged Code OSS application')
        packaged.write_text(json.dumps(dict(version=pins['release'],
            application_directory=str(application), compilation_receipt_sha256=digest(compilation),
            files={str(p.relative_to(application)): dict(path=str(p), sha256=digest(p))
                   for p in sorted(application.rglob('*')) if p.is_file()},
            source_sha256={str(p.relative_to(ROOT)): digest(p) for p in [Path(__file__), SOURCES, PACKAGING_PATCH]},
            target_electron_downloaded=False, target_electron_executed=False,
            native_modules_coast_qualified=False,
            state='application packaged; target native modules and Coast qualification pending'), indent=2) + '\n')
    print('Code OSS selected source build stages completed', flush=True)


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Rebuild the complete pinned Debian GUI library closure in a rootless container.

Only build tools/development dependencies come from binary packages. Every
package exported for the Coast runtime is built from its checksum-pinned source
and Debian patches. No package is installed on the host. Completed source
packages are verified and reused on restart; failed builds retain their logs.
"""
import argparse
import fcntl
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import subprocess

from debian_package import package_control
from fetch_sources import fetch
from source_tree import digest

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ROOT / 'ports/chromium/gui-sources.json'


def atomic_json(path, value):
    temporary = path.with_suffix('.json.part')
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')
    temporary.replace(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-root', type=Path, default=ROOT / 'build/gui-sources')
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--only', nargs='+', help='build selected source packages; does not produce a complete runtime receipt')
    parser.add_argument('--reconfigure', action='store_true',
                        help='accept builder-only changes after verifying all completed source packages')
    args = parser.parse_args()
    if not 1 <= args.jobs <= 24:
        parser.error('--jobs must be 1..24')
    if not shutil.which('podman'):
        parser.error('The GUI source build requires rootless Podman on the Linux build host')
    if subprocess.check_output(['podman', 'info', '--format', '{{.Host.Security.Rootless}}'], text=True).strip() != 'true':
        parser.error('Use rootless Podman for the GUI source build')
    pins = json.loads(SOURCES.read_text())
    names = args.only or list(pins['sources'])
    if set(names) - pins['sources'].keys():
        parser.error('Unknown source package: ' + ', '.join(sorted(set(names) - pins['sources'].keys())))
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=True)
    lock = (out / '.build.lock').open('a')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    inputs = [Path(__file__), SOURCES, ROOT / 'tools/debian_package.py', ROOT / 'tools/fetch_sources.py']
    identity = {str(p.relative_to(ROOT)): digest(p) for p in inputs}
    identity_path = out / 'SOURCE.json'
    if identity_path.exists() and json.loads(identity_path.read_text()) != identity:
        previous = json.loads(identity_path.read_text())
        if not args.reconfigure or previous.get(str(SOURCES.relative_to(ROOT))) != identity[str(SOURCES.relative_to(ROOT))]:
            raise ValueError('GUI source recipes changed; select a fresh output root, or --reconfigure for builder-only changes')
        history = out / 'configuration-history'
        history.mkdir(exist_ok=True)
        snapshot = history / (digest(identity_path) + '.json')
        if not snapshot.exists():
            shutil.copyfile(identity_path, snapshot)
    atomic_json(identity_path, identity)
    for directory in ['vendor', 'objects', 'packages', 'receipts', 'logs']:
        (out / directory).mkdir(exist_ok=True)
    container = 'coast-gui-' + hashlib.sha256(str(out).encode()).hexdigest()[:16]
    label = 'io.coast.source-build'

    def host(command, **kwargs):
        return subprocess.run(list(map(str, command)), check=True, stdin=subprocess.DEVNULL, **kwargs)

    def shell(script, log):
        with (out / 'logs' / (log + '.log')).open('w') as stream:
            result = subprocess.run(['podman', 'exec', '-i', container, 'sh', '-es'],
                input=script, text=True, stdout=stream, stderr=subprocess.STDOUT)
        if result.returncode:
            raise RuntimeError('GUI source stage failed; see ' + str(out / 'logs' / (log + '.log')))

    exists = subprocess.run(['podman', 'container', 'exists', container], check=False).returncode == 0
    if exists:
        info = json.loads(subprocess.check_output(['podman', 'inspect', container], text=True))[0]
        if info['Config']['Labels'].get(label) != str(out) or info['ImageName'] != pins['container_image']:
            raise ValueError('The existing container does not belong to this source build')
    else:
        host(['podman', 'create', '--name', container, '--label', label + '=' + str(out),
            '--platform', 'linux/amd64', '--volume', str(out) + ':/work',
            pins['container_image'], 'sleep', 'infinity'], stdout=subprocess.DEVNULL)
    host(['podman', 'start', container], stdout=subprocess.DEVNULL)
    try:
        if not (out / 'ENVIRONMENT.json').exists():
            snapshot = pins['snapshot']
            repositories = '\n'.join(f'{kind} [check-valid-until=no] http://snapshot.debian.org/archive/{archive}/{snapshot}/ {suite} main'
                for archive, suite in [('debian', 'bookworm'), ('debian-security', 'bookworm-security')]
                for kind in ('deb', 'deb-src')) + '\n'
            shell('rm -f /etc/apt/sources.list.d/debian.sources\n'
                + 'printf %s ' + shlex.quote(repositories) + ' > /etc/apt/sources.list\n'
                + 'apt-get -o Acquire::Retries=3 update\n'
                + 'DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends build-essential ca-certificates dpkg-dev fakeroot python3\n', 'environment')
            atomic_json(out / 'ENVIRONMENT.json', dict(image=pins['container_image'], snapshot=snapshot,
                source_identity=identity, container=container, host_installations=False))
        for name in names:
            pin = pins['sources'][name]
            receipt = out / 'receipts' / (name + '.json')
            if receipt.is_file():
                built = json.loads(receipt.read_text())
                if built['source'] != pin or any(digest(Path(p['path'])) != p['sha256'] for p in built['packages'].values()):
                    raise ValueError('A completed GUI source package changed: ' + name)
                if (out / 'objects' / name).exists():
                    shell('rm -rf -- ' + shlex.quote('/work/objects/' + name) + '\n', name + '-cleanup')
                print('Verified source-built GUI package: ' + name, flush=True)
                continue
            print('Building GUI sources: ' + name + ' ' + pin['version'], flush=True)
            for archive in pin['archives']:
                fetch(archive, out / 'vendor')
            descriptor, = [p for p in pin['archives'] if p['archive'].endswith('.dsc')]
            work = out / 'objects' / name
            work.mkdir(exist_ok=True)
            package_dir = out / 'packages' / name
            package_dir.mkdir(exist_ok=True)
            source = '/work/objects/' + name + '/source'
            script = 'export DEBIAN_FRONTEND=noninteractive\n'
            script += 'apt-get -o Acquire::Retries=3 build-dep -y --no-install-recommends ' + shlex.quote(name + '=' + pin['version']) + '\n'
            script += 'if [ ! -d ' + shlex.quote(source) + ' ]; then dpkg-source --no-check -x ' + shlex.quote('/work/vendor/' + descriptor['archive']) + ' ' + shlex.quote(source) + '; fi\n'
            script += 'cd ' + shlex.quote(source) + '\n'
            # Some pinned Debian rules still install/chmod example files when
            # nodoc suppresses dh_installexamples (notably expat). Build their
            # normal documentation; only runtime libraries/notices are staged.
            script += 'export DEB_BUILD_OPTIONS=' + shlex.quote('nocheck noautodbgsym parallel=' + str(args.jobs)) + '\n'
            if name == 'nss':
                # Debian's nocheck still builds the large upstream gtest
                # executables. None belong to libnss3; use NSS's own switch
                # to omit only those test targets, keeping all runtime tools.
                script += 'export NSS_DISABLE_GTESTS=1\n'
            # Let Debian recipes choose safe parallelism. Forcing MAKEFLAGS
            # breaks NSS's recursive get_objs expansion; its legacy recipe
            # remains serial with this opt-in setting.
            script += 'dpkg-buildpackage -B -us -uc --jobs=' + str(args.jobs) + '\n'
            shell(script, name)
            packages = {}
            for path in sorted(work.glob('*.deb')):
                info = package_control(path)
                package = info['Package']
                if package not in pin['packages']:
                    continue
                if info['Architecture'] not in ('amd64', 'all'):
                    raise ValueError('Unexpected built package architecture: ' + package)
                if info.get('Source', package).split()[0] != name:
                    raise ValueError('Built package source differs: ' + package)
                target = package_dir / path.name
                shutil.copyfile(path, target)
                packages[package] = dict(path=str(target), archive=target.name, sha256=digest(target),
                    source=name, source_version=pin['version'], source_descriptor_sha256=descriptor['sha256'],
                    compiled_from_source=True)
            if set(packages) != set(pin['packages']):
                raise ValueError('Source build did not produce required packages: ' + repr(set(pin['packages']) - packages.keys()))
            atomic_json(receipt, dict(source=pin, packages=packages, log_sha256=digest(out / 'logs' / (name + '.log'))))
            # Only this builder's successful object tree is disposable; archives,
            # source pins, packages and logs remain available for verification.
            # Debian staging can contain package-owned, mode-000 directories.
            # Remove them inside our rootless container's namespace, which owns
            # those IDs, rather than changing permissions on the host.
            shell('rm -rf -- ' + shlex.quote('/work/objects/' + name) + '\n', name + '-cleanup')
        complete, packages = True, {}
        for name, pin in pins['sources'].items():
            receipt = out / 'receipts' / (name + '.json')
            if not receipt.is_file():
                complete = False
                continue
            built = json.loads(receipt.read_text())
            if built['source'] != pin or any(digest(Path(p['path'])) != p['sha256'] for p in built['packages'].values()):
                raise ValueError('A GUI source output changed: ' + name)
            packages.update(built['packages'])
        if any(digest(ROOT / name) != expected for name, expected in identity.items()):
            raise ValueError('GUI source recipes changed during this build')
        record = dict(state='source compiled; Coast qualification pending' if complete else 'partial source build',
            complete=complete, packages=packages, source_sha256=identity,
            gui_libraries_compiled_from_source=complete, container_image=pins['container_image'],
            build_dependencies='binary host tools inside rootless container; never copied into the Coast runtime',
            host_installations=False, target_guest_executed=False)
        atomic_json(out / ('BUILD.json' if complete else 'PARTIAL.json'), record)
        print('GUI source build ' + ('complete' if complete else 'checkpoint saved'), flush=True)
    finally:
        host(['podman', 'stop', '--time', '0', container], stdout=subprocess.DEVNULL)


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Install or check Linux host prerequisites for building Coast."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import sys
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
CHROMIUM_REVISION = 'f916f1b7136907af19d96b95b891533d091d932a'
INSTALLER_SHA256 = '22ea2aebdd09d5554029b55a63757d0d30a71272ebb3c5ee444aaeb822230920'
INSTALLER_URL = ('https://raw.githubusercontent.com/chromium/chromium/'
                 + CHROMIUM_REVISION + '/build/install-build-deps.py')
BASE_PACKAGES = ('build-essential clang lld git python3 curl ca-certificates patch '
    'bzip2 xz-utils unzip zip file gnupg nasm mtools xorriso '
    'qemu-system-x86 qemu-system-gui ovmf').split()
DESKTOP_PACKAGES = ('autoconf automake libtool bison flex gperf gettext texinfo '
    'cmake ninja-build pkg-config bubblewrap podman uidmap slirp4netns fuse-overlayfs '
    'ncurses-bin libncurses-dev libssl-dev libffi-dev libreadline-dev zlib1g-dev '
    'libsqlite3-dev libbz2-dev liblzma-dev libgdbm-dev uuid-dev libkrb5-dev '
    'libx11-dev libxkbfile-dev libsecret-1-dev lsb-release').split()
BASE_TOOLS = 'make gcc g++ ld git python3 curl tar patch mformat mcopy xorriso'.split()
DESKTOP_TOOLS = 'autoconf automake libtoolize bison flex gperf gettext makeinfo cmake ninja pkg-config bwrap podman tic infocmp'.split()


def checked_installer(path):
    if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != INSTALLER_SHA256:
        raise ValueError('Chromium prerequisite installer checksum mismatch')
    return path


def check(native=False):
    missing = [name for name in BASE_TOOLS + ([] if native else DESKTOP_TOOLS)
               if not shutil.which(name)]
    if missing:
        raise ValueError('Missing host tools: ' + ', '.join(missing))
    if not native:
        result = subprocess.run(['podman', 'info', '--format', '{{.Host.Security.Rootless}}'],
            check=True, capture_output=True, text=True)
        if result.stdout.strip() != 'true':
            raise ValueError('Run the build as a normal user with rootless Podman')
        subprocess.run(['bwrap', '--unshare-all', '--ro-bind', '/', '/', '--proc', '/proc',
                        '--dev', '/dev', '--', '/bin/true'], check=True)
    print('Coast host prerequisites: ready')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_mutually_exclusive_group()
    actions.add_argument('--install', action='store_true', help='explicitly install host packages using sudo')
    actions.add_argument('--print', dest='print_commands', action='store_true', help='show package commands without installing')
    actions.add_argument('--check', action='store_true', help='check tools and rootless namespaces without installing')
    parser.add_argument('--native', action='store_true', help='only prepare the smaller native image')
    args = parser.parse_args()
    if platform.system() != 'Linux' or platform.machine() not in ('x86_64', 'amd64'):
        parser.error('Use an x86-64 Linux host')
    if sys.version_info < (3, 12):
        parser.error('Python 3.12 or newer is required')
    if not args.install and not args.print_commands:
        check(args.native)
        return
    if platform.freedesktop_os_release().get('ID') not in ('ubuntu', 'debian'):
        parser.error('Automatic package installation supports Ubuntu/Debian; see Documentation/host-setup.md')
    packages = BASE_PACKAGES + ([] if args.native else DESKTOP_PACKAGES)
    commands = [['sudo', 'apt-get', 'update'], ['sudo', 'apt-get', 'install', '-y', *packages]]
    if args.print_commands:
        for command in commands:
            print(shlex.join(command))
        if not args.native:
            print('Chromium prerequisites: ' + INSTALLER_URL)
            print('Required SHA-256: ' + INSTALLER_SHA256)
        return
    if os.geteuid() == 0:
        parser.error('Run this as your normal user; sudo is used only for package installation')
    if not args.native:
        lock = json.loads((ROOT / 'ports/electron/sources.lock.json').read_text())
        chromium = next(record for record in lock['repositories'] if record['path'] == 'src')
        if chromium['commit'] != CHROMIUM_REVISION:
            parser.error('Update the pinned prerequisite installer for the Electron source lock')
    for command in commands:
        subprocess.run(command, check=True)
    if not args.native:
        output = ROOT / 'build/host-setup'
        output.mkdir(parents=True, exist_ok=True)
        installer = output / 'install-build-deps.py'
        if not installer.exists():
            pending = installer.with_suffix('.part')
            try:
                with urllib.request.urlopen(INSTALLER_URL, timeout=60) as response:
                    data = response.read(1024 * 1024 + 1)
                if len(data) > 1024 * 1024:
                    raise ValueError('Chromium prerequisite installer exceeds the size limit')
                pending.write_bytes(data)
                checked_installer(pending)
                pending.replace(installer)
            finally:
                pending.unlink(missing_ok=True)
        subprocess.run(['sudo', 'python3', str(checked_installer(installer)),
                        '--no-prompt', '--no-arm', '--no-chromeos-fonts'], check=True)
    check(args.native)


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error) + '\nSee Documentation/host-setup.md for host configuration.')

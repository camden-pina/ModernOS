#!/usr/bin/env python3
"""Source-build static xorriso and mtools for assembling boot images in Coast."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile

from fetch_sources import fetch, read_manifest

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-root', type=Path, default=ROOT / 'build/image-tools')
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=True)
    cross = ROOT / 'build/compiler/cross/bin/x86_64-linux-musl-'
    if not Path(str(cross) + 'gcc').is_file():
        parser.error('Build the musl toolchain first: make -C userland runtime/usr/bin/gcc')
    conf = ['bwrap', '--unshare-all', '--die-with-parent', '--new-session', '--ro-bind', '/', '/',
            '--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp', '--bind', str(out), str(out)]
    env = dict(PATH='/usr/bin:/bin', HOME='/tmp', TMPDIR='/tmp', LANG='C.UTF-8',
        CC=str(cross) + 'gcc', AR=str(cross) + 'ar', RANLIB=str(cross) + 'ranlib',
        CFLAGS='-O2 -g0 -fno-pie -fno-stack-protector -mno-avx -mno-avx2',
        CPPFLAGS='-idirafter ' + str(ROOT / 'ports/linux-uapi/include'), LDFLAGS='-static -no-pie')
    commands, packages = [], {}
    manifest = read_manifest()
    definitions = [
        ('xorriso', '1.5.8.pl02', 'xorriso-1.5.8',
         ['--disable-shared', '--disable-libacl', '--disable-xattr', '--disable-zlib',
          '--disable-libbz2', '--disable-libjte', '--disable-readline', '--disable-libedit'],
         ['xorriso/xorriso'], 'COPYING', 'GPL-3.0-or-later'),
        ('mtools', '4.0.49', 'mtools-4.0.49', ['--without-x', '--disable-floppyd'],
         ['mtools', 'mformat', 'mcopy', 'mdir', 'mmd', 'mtype', 'mdel', 'mlabel', 'mshowfat'],
         'COPYING', 'GPL-3.0-or-later'),
    ]
    for name, version, directory, options, binaries, license, license_id in definitions:
        record = manifest[name + '-' + version + '.tar.gz']
        fetch(record)
        source, build, prefix = out / directory, out / (name + '-build'), out / 'runtime'
        if not source.exists():
            with tarfile.open(ROOT / 'userland/vendor' / record['archive']) as archive:
                archive.extractall(out, filter='data')
        if name == 'xorriso':
            patch = ROOT / 'ports/image-tools/xorriso-private-buffer.patch'
            apply = ['patch', '-p1', '-i', str(patch)]
            if subprocess.run([*apply, '--dry-run', '--forward'], cwd=source,
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
                subprocess.run(apply, cwd=source, check=True)
            elif subprocess.run([*apply, '--dry-run', '--reverse'], cwd=source,
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
                raise ValueError('xorriso buffer patch does not match the pinned source')
        build.mkdir(exist_ok=True)
        for step, command in [('configure', [source / 'configure', '--prefix=/usr',
                    '--build=' + subprocess.check_output(['gcc', '-dumpmachine'], text=True).strip(),
                    '--host=x86_64-linux-musl', *options]),
                ('build', ['make', '-j' + str(args.jobs)])]:
            command = list(map(str, command))
            commands.append(dict(package=name, step=step, command=command))
            print(name + ': ' + step, flush=True)
            with (out / (name + '-' + step + '.log')).open('w') as log:
                subprocess.run([*conf, '--chdir', str(build), '--', *command], env=env,
                    stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, check=True)
        files = {}
        for binary in binaries:
            path = build / binary
            if not path.exists() and name == 'mtools':
                path = build / 'mtools'
            if 'INTERP' in subprocess.check_output(['readelf', '-l', str(path)], text=True):
                raise ValueError('Image tools must be static: ' + binary)
            destination = prefix / 'usr/bin' / Path(binary).name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, destination)
            destination.chmod(0o755)
            files[str(destination.relative_to(prefix))] = dict(path=str(destination), sha256=sha(destination))
        destination = prefix / 'usr/share/licenses' / name / 'COPYING'
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source / license, destination)
        files[str(destination.relative_to(prefix))] = dict(path=str(destination), sha256=sha(destination))
        packages[name] = dict(version=version, release=2 if name == 'xorriso' else 1,
                             source=record, files=files, license=license_id)
    (out / 'BUILD.json').write_text(json.dumps(dict(packages=packages, commands=commands,
        source_sha256={str(p.relative_to(ROOT)): sha(p) for p in
            [Path(__file__), ROOT / 'ports/image-tools/xorriso-private-buffer.patch']},
        target_host_executed=False), indent=2) + '\n')
    print('Native image tools built:', out / 'runtime')


if __name__ == '__main__':
    main()

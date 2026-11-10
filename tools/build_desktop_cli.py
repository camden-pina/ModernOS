#!/usr/bin/env python3
"""Build the desktop's GNU Bash/Readline terminal from pinned sources.

The native base already supplies source-built BusyBox command tools. This
component adds the parent-controlled PTY port used by Coast's Terminal, with
its own static ncurses library. It never imports an existing ISO or runtime.
"""
from build_state import project_lock
from build_state import atomic_write
import argparse
import fcntl
import json
from pathlib import Path
import shutil
import subprocess

from fetch_sources import fetch, read_manifest
from source_archive import unpack
from source_tree import digest

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / 'ports/bash/COAST-PARENT-TTY.patch'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native-build', type=Path, required=True)
    parser.add_argument('--output-root', type=Path, required=True)
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    if not 1 <= args.jobs <= 24:
        parser.error('--jobs must be 1..24')
    base = args.native_build.resolve(strict=True)
    native = json.loads((base / 'BUILD.json').read_text())
    if native.get('inherited_iso') is not False:
        parser.error('Use a source-built native base')
    compiler = base / 'musl/bin/musl-gcc'
    if not compiler.is_file():
        parser.error('The Linux native base must include its compiled musl SDK')
    for tool in ('bwrap', 'tic', 'infocmp', 'make', 'patch'):
        if not shutil.which(tool):
            parser.error('Missing host build tool: ' + tool)
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=True)
    lock = (out / '.build.lock').open('a')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    bash = next(p for p in json.loads((ROOT / 'userland/vendor/linux-ports-sources.json').read_text())['packages']
                if p['name'] == 'bash')
    curses = read_manifest()['ncurses-6.5.tar.gz']
    inputs = [Path(__file__), PATCH, ROOT / 'userland/vendor/linux-ports-sources.json',
              ROOT / 'userland/vendor/sources.json', ROOT / 'tools/source_archive.py']
    identity = dict(source_sha256={str(p.relative_to(ROOT)): digest(p) for p in inputs},
                    native_build_sha256=digest(base / 'BUILD.json'),
                    compiler_sha256=digest(compiler), libc_sha256=digest(base / 'musl/lib/libc.a'))
    stamp = out / 'SOURCE.json'
    if stamp.exists() and json.loads(stamp.read_text()) != identity:
        raise ValueError('Terminal build inputs changed; select a fresh output root')
    atomic_write(stamp, json.dumps(identity, indent=2) + '\n')
    vendor = out / 'vendor'
    vendor.mkdir(exist_ok=True)
    sources = {'bash': bash, 'ncurses': curses}
    for name, pin in sources.items():
        fetch(pin, vendor)
        source = out / ('bash-5.3' if name == 'bash' else 'ncurses-6.5')
        if not source.exists():
            unpack(vendor / pin['archive'], out)
    source = out / 'bash-5.3'
    for patch in bash['patches']:
        asset = ROOT / 'userland/vendor/linux-ports' / patch['file']
        if digest(asset) != patch['sha256']:
            raise ValueError('Bash official patch changed: ' + patch['file'])
        stamp = out / ('.' + patch['file'])
        if not stamp.exists():
            subprocess.run(['patch', '--batch', '-p0', '-i', str(asset)], cwd=source, check=True)
            stamp.write_text(patch['sha256'])
    stamp = out / '.parent-tty'
    if not stamp.exists():
        if digest(source / 'jobs.c') != 'ea8914be42b734addddb3a72e1bd8e39a607e3422eeb7eb98ce9ad4ee7886a12':
            raise ValueError('Unexpected GNU Bash jobs.c after official patches')
        subprocess.run(['patch', '--batch', '-p1', '-i', str(PATCH)], cwd=source, check=True)
        stamp.write_text(digest(PATCH))
    if stamp.read_text() != digest(PATCH):
        raise ValueError('Bash PTY port changed')
    # Keep anonymous pipes for process substitution, but let Bash duplicate
    # their /dev/fd/N redirections itself. Coast does not expose a general
    # /dev/fd filesystem. This is also required by upstream makepkg's reads.
    redirection = source / 'redir.c'
    text = redirection.read_text()
    if 'defined (COAST_BASH_FD_REDIRECT)' not in text:
        if text.count('#if !defined (HAVE_DEV_FD)') != 3:
            raise ValueError('Pinned Bash descriptor redirection source changed')
        redirection.write_text(text.replace('#if !defined (HAVE_DEV_FD)',
            '#if !defined (HAVE_DEV_FD) || defined (COAST_BASH_FD_REDIRECT)'))
    prefix = out / 'sdk'
    environment = dict(PATH='/usr/bin:/bin', HOME='/tmp', TMPDIR='/tmp', LANG='C.UTF-8',
        CC=str(compiler), AR='ar', RANLIB='ranlib', CC_FOR_BUILD='gcc -std=gnu11',
        CFLAGS='-O2 -g0 -fno-pie -fno-stack-protector -mno-avx -mno-avx2',
        LDFLAGS='-static -no-pie -Wl,-z,max-page-size=4096,-z,noexecstack',
        PKG_CONFIG_LIBDIR='/nonexistent', CONFIG_SITE='/dev/null')
    confinement = ['bwrap', '--unshare-all', '--die-with-parent', '--new-session',
        '--ro-bind', '/', '/', '--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp',
        '--bind', str(out), str(out)]
    commands = []

    def run(name, command, cwd, extra=None):
        command = list(map(str, command))
        commands.append(dict(name=name, argv=command, cwd=str(cwd)))
        print('Desktop terminal: ' + name, flush=True)
        with (out / (name + '.log')).open('w') as log:
            subprocess.run([*confinement, '--chdir', str(cwd), '--', *command],
                env={**environment, **(extra or {})}, stdin=subprocess.DEVNULL,
                stdout=log, stderr=subprocess.STDOUT, check=True)

    build = out / 'ncurses-build'
    build.mkdir(exist_ok=True)
    if not (build / 'Makefile').exists():
        run('ncurses-configure', [out / 'ncurses-6.5/configure', '--prefix=' + str(prefix),
            '--build=' + subprocess.check_output(['gcc', '-dumpmachine'], text=True).strip(),
            '--host=x86_64-linux-musl', '--without-shared', '--without-debug', '--without-ada',
            '--without-cxx', '--without-cxx-binding', '--without-tests', '--without-progs',
            '--disable-db-install', '--disable-database', '--with-fallbacks=vt100,xterm',
            '--with-tic-path=' + shutil.which('tic'), '--with-infocmp-path=' + shutil.which('infocmp'),
            '--disable-home-terminfo', '--enable-widec'], build)
    run('ncurses-build', ['make', '-j' + str(args.jobs)], build)
    run('ncurses-install', ['make', 'install'], build)
    build = out / 'bash-build'
    build.mkdir(exist_ok=True)
    extra = dict(CFLAGS=environment['CFLAGS'] + ' -DCOAST_PARENT_TTY=1 -DCOAST_BASH_FD_REDIRECT=1',
        CPPFLAGS='-idirafter ' + str(prefix / 'include') + ' -isystem ' + str(ROOT / 'ports/linux-uapi/include'),
        LDFLAGS=environment['LDFLAGS'] + ' -L' + str(prefix / 'lib'), LIBS='-lncursesw')
    if not (build / 'Makefile').exists():
        run('bash-configure', [source / 'configure', '--prefix=/usr',
            '--build=' + subprocess.check_output(['gcc', '-dumpmachine'], text=True).strip(),
            '--host=x86_64-linux-musl', '--disable-nls', '--without-bash-malloc',
            '--enable-readline', '--with-curses', '--enable-static-link'], build, extra)
    run('bash-build', ['make', '-j' + str(args.jobs), 'bash'], build, extra)
    binary = build / 'bash'
    elf = subprocess.check_output(['readelf', '-h', '-lW', binary], text=True)
    if 'Advanced Micro Devices X86-64' not in elf or 'INTERP' in elf or ' RWE ' in elf:
        raise ValueError('Bash must be a static x86-64 executable without writable executable segments')
    files = {}
    for name, asset, mode in [('bin/bash', binary, 0o755),
            ('usr/share/licenses/bash/COPYING', source / 'COPYING', 0o644),
            ('usr/share/licenses/ncurses/COPYING', out / 'ncurses-6.5/COPYING', 0o644)]:
        files[name] = dict(path=str(asset), sha256=digest(asset), mode=mode)
    if identity != dict(source_sha256={str(p.relative_to(ROOT)): digest(p) for p in inputs},
            native_build_sha256=digest(base / 'BUILD.json'), compiler_sha256=digest(compiler),
            libc_sha256=digest(base / 'musl/lib/libc.a')):
        raise ValueError('Terminal build inputs changed during compilation')
    record = dict(**identity, sources=sources, files=files, aliases={'usr/bin/bash': '../../bin/bash'},
        commands=commands, target_execution_on_linux=False,
        state='Bash/Readline source compiled; Coast PTY qualification pending')
    atomic_write(out / 'BUILD.json', json.dumps(record, indent=2) + '\n')
    print('Desktop Bash terminal built:', binary, flush=True)


if __name__ == '__main__':
    with project_lock():
        main()

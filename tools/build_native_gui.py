#!/usr/bin/env python3
"""Compile the native desktop into a fresh output with read-only source mounts."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output-root', type=Path, required=True)
    p.add_argument('--artifacts', type=Path, help=argparse.SUPPRESS)
    a = p.parse_args()
    out = a.output_root.resolve()
    out.mkdir(parents=True, exist_ok=False)
    userland = ROOT / 'userland'
    sources = [userland / name for name in ['gui.c', 'libc.c', 'shell_completion.c', 'shell_completion_native.c']]
    sources += sorted((userland / 'desktop').glob('*.c'))
    inputs = set(sources + [userland / 'vector-io-check.c', userland / 'clip.c', userland / 'sudo.c', userland / 'code-launch.c', userland / 'passwd.c', userland / 'login.c', userland / 'sh.c', userland / 'shell_line_editor.c', userland / 'shell_terminal_editor.c', userland / 'shell_commands.inc', userland / 'id.c', userland / 'whoami.c', userland / 'profile-session.c', userland / 'crt0.S', userland / 'user.ld', userland / 'Makefile'])
    for directory in [userland / 'desktop', userland / 'include', ROOT / 'include', ROOT / 'arch/x86/include']:
        inputs.update(directory.rglob('*.h'))
    def hashes():
        return {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest() for path in sorted(inputs)}
    before = hashes()
    conf = ['bwrap', '--unshare-all', '--die-with-parent', '--new-session',
            '--ro-bind', '/', '/', '--proc', '/proc', '--dev', '/dev',
            '--tmpfs', '/tmp', '--bind', str(out), str(out),
            '--chdir', str(userland)]
    env = dict(HOME='/tmp', PATH='/usr/bin:/bin', LANG='C.UTF-8', TMPDIR='/tmp')
    flags = ['-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-ffreestanding',
             '-fno-builtin', '-fno-pie', '-fno-stack-protector', '-m64', '-mno-avx', '-mno-avx2',
             '-I' + str(userland / 'include'), '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'arch/x86/include/asm')]
    commands, objects = [], []
    for source in [userland / 'crt0.S', *sources]:
        obj = out / (str(source.relative_to(userland)).replace('/', '_') + '.o')
        command = ['/usr/bin/cc', *(['-m64'] if source.suffix == '.S' else flags), '-c', str(source), '-o', str(obj)]
        subprocess.run(conf + ['--', *command], check=True, env=env, stdin=subprocess.DEVNULL)
        commands.append(command)
        objects.append(str(obj))
    binary = out / 'gui'
    command = ['/usr/bin/ld', '-nostdlib', '-static', '-z', 'max-page-size=4096', '-z', 'noexecstack',
               '-T', str(userland / 'user.ld'), *objects, '-o', str(binary)]
    subprocess.run(conf + ['--', *command], check=True, env=env, stdin=subprocess.DEVNULL)
    commands.append(command)
    for helper in ['vector-io-check', 'clip', 'sudo', 'code-launch', 'profile-session', 'id', 'whoami', 'passwd', 'login']:
        check_obj = out / (helper + '.o')
        command = ['/usr/bin/cc', *flags, '-c', str(userland / (helper + '.c')), '-o', str(check_obj)]
        subprocess.run(conf + ['--', *command], check=True, env=env, stdin=subprocess.DEVNULL)
        commands.append(command)
        command = ['/usr/bin/ld', '-nostdlib', '-static', '-z', 'max-page-size=4096', '-z', 'noexecstack',
                   '-T', str(userland / 'user.ld'), str(out / 'crt0.S.o'), str(out / 'libc.c.o'),
                   str(check_obj), '-o', str(out / helper)]
        subprocess.run(conf + ['--', *command], check=True, env=env, stdin=subprocess.DEVNULL)
        commands.append(command)
    shell_objects=[]
    for name in ['sh', 'shell_line_editor', 'shell_terminal_editor']:
        obj=out/(name+'.o')
        command=['/usr/bin/cc',*flags,'-c',str(userland/(name+'.c')),'-o',str(obj)]
        subprocess.run(conf+['--',*command],check=True,env=env,stdin=subprocess.DEVNULL)
        commands.append(command);shell_objects.append(str(obj))
    command=['/usr/bin/ld','-nostdlib','-static','-z','max-page-size=4096','-z','noexecstack','-T',str(userland/'user.ld'),str(out/'crt0.S.o'),str(out/'libc.c.o'),*shell_objects,str(out/'shell_completion.c.o'),str(out/'shell_completion_native.c.o'),'-o',str(out/'coast-sh')]
    subprocess.run(conf+['--',*command],check=True,env=env,stdin=subprocess.DEVNULL)
    commands.append(command)
    if hashes() != before:
        raise RuntimeError('Desktop source changed during compilation')
    record = dict(source_sha256=before, commands=commands, confinement=conf,
                  binary=str(binary), binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(), host_executed=False)
    (out / 'BUILD.json').write_text(json.dumps(record, indent=2) + '\n')
    print('Native desktop ready:', binary, flush=True)


if __name__ == '__main__':
    main()

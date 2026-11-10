#!/usr/bin/env python3
"""Actual upstream program workloads. Run inside Coast as an ordinary user."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


def run(arguments, data=None, code=0, timeout=40):
    result = subprocess.run(arguments, input=data, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, timeout=timeout)
    assert result.returncode == code, (arguments, result.returncode, result.stdout, result.stderr)
    return result.stdout


def passed(name):
    print('EXTRA_PROGRAMS_PASS ' + name, flush=True)


def main():
    assert os.getuid() == 1000 and os.geteuid() == 1000
    destination = Path(sys.argv[1] if len(sys.argv) > 1 else str(Path.home() / 'Documents'))
    work = Path(tempfile.mkdtemp(prefix='extra-programs-', dir=destination))
    os.chdir(work)
    print('EXTRA_PROGRAMS_WORK ' + str(work), flush=True)
    versions = {'ninja': ('--version', b'1.13.2'), 'pkgconf': ('--version', b'2.5.1'),
                'm4': ('--version', b'1.4.21'), 'linux-patch': ('--version', b'2.8.1'),
                'gperf': ('--version', b'3.3'), 'bc': ('--version', b'1.08.2'),
                'dc': ('--version', b'1.5.2'), 'zstd': ('--version', b'1.5.7'),
                'lz4': ('--version', b'1.11.0'), 'linux-xz': ('--version', b'5.8.4'),
                'rsync': ('--version', b'3.5.2')}
    for command, (option, expected) in versions.items():
        assert expected in run([command, option]), command
    passed('eleven upstream program versions under UID 1000')

    assert run(['bc', '-l'], b'scale=20; 1/7\n2^80\nsqrt(2)\n') == b'.14285714285714285714\n1208925819614629174706176\n1.41421356237309504880\n'
    assert run(['dc'], b'2 80 ^ p\n10 k 1 7 / p\n') == b'1208925819614629174706176\n.1428571428\n'
    passed('bc and dc arbitrary precision, fractions and math library')

    Path('included.m4').write_text('define(`greet\', `Hello, $1!\')dnl\n')
    macro = b"include(`included.m4')dnl\ngreet(`Coast')\neval(6*7)\n"
    assert run(['m4'], macro) == b'Hello, Coast!\n42\n'
    passed('GNU m4 includes, macros and arithmetic')

    payload = (b'Coast real compression round trip.\n' * 19000) + bytes(range(256)) * 1024
    Path('payload.bin').write_bytes(payload)
    for command, suffix, compress in [('zstd', '.zst', ['-q', '-T2', '-f']),
                                      ('lz4', '.lz4', ['-q', '-T2', '-f']),
                                      ('linux-xz', '.xz', ['-T2', '-k', '-f'])]:
        arguments = [command, *compress, 'payload.bin']
        if command != 'linux-xz': arguments += ['payload.bin' + suffix]
        run(arguments)
        archive = Path('payload.bin' + suffix)
        assert archive.stat().st_size < len(payload) // 2
        assert run([command, '-d', '-c', str(archive)]) == payload
        run([command, '-t', str(archive)])
        streamed = run([command, '-c'], payload)
        assert run([command, '-d', '-c'], streamed) == payload
        damaged = bytearray(archive.read_bytes()); damaged[len(damaged) // 2] ^= 0x80
        Path('broken' + suffix).write_bytes(damaged)
        result = subprocess.run([command, '-t', 'broken' + suffix], capture_output=True, timeout=30)
        assert result.returncode != 0, command
        passed(command + ' threaded compression, file/pipe round trips and corruption rejection')
    assert run(['zstdcat', 'payload.bin.zst']) == payload
    assert run(['lz4cat', 'payload.bin.lz4']) == payload
    assert run(['linux-xzcat', 'payload.bin.xz']) == payload
    passed('compression aliases preserve upstream command behavior')

    Path('original.txt').write_text('alpha\nbeta\ngamma\n')
    patch = b'--- original.txt\n+++ original.txt\n@@ -1,3 +1,3 @@\n alpha\n-beta\n+Coast\n gamma\n'
    run(['linux-patch', '--dry-run', '-p0'], patch)
    assert Path('original.txt').read_text() == 'alpha\nbeta\ngamma\n'
    run(['linux-patch', '-p0'], patch)
    assert Path('original.txt').read_text() == 'alpha\nCoast\ngamma\n'
    run(['linux-patch', '-R', '-p0'], patch)
    assert Path('original.txt').read_text() == 'alpha\nbeta\ngamma\n'
    passed('GNU patch dry run, actual unified patch and reverse')

    source, target = Path('sync-source'), Path('sync-target')
    (source / 'nested').mkdir(parents=True); target.mkdir()
    (source / 'nested/data.bin').write_bytes(payload)
    (source / 'notes.txt').write_text('first revision\n')
    (source / 'excluded.tmp').write_text('excluded\n')
    (source / 'notes-link').symlink_to('notes.txt')
    (source / 'empty').mkdir()
    sync = ['rsync', '-rlpc', '--exclude=*.tmp', str(source) + '/', str(target) + '/']
    run(sync)
    assert (target / 'nested/data.bin').read_bytes() == payload
    assert (target / 'notes-link').is_symlink() and os.readlink(target / 'notes-link') == 'notes.txt'
    assert (target / 'empty').is_dir() and not (target / 'excluded.tmp').exists()
    (source / 'notes.txt').write_text('second revision\n')
    (target / 'obsolete.txt').write_text('remove me\n')
    run([*sync[:2], '--delete', *sync[2:]])
    assert (target / 'notes.txt').read_text() == 'second revision\n'
    assert not (target / 'obsolete.txt').exists()
    (source / 'notes.txt').write_text('dry run revision\n')
    run([*sync[:2], '--dry-run', *sync[2:]])
    assert (target / 'notes.txt').read_text() == 'second revision\n'
    passed('rsync local recursive/checksum updates, symlinks, excludes, delete and dry run')

    # Resolve a real .pc file and use its flags in a compiled Ninja dependency graph.
    prefix = work / 'sdk'
    (prefix / 'include').mkdir(parents=True); (prefix / 'lib/pkgconfig').mkdir(parents=True)
    (prefix / 'include/answer.h').write_text('#define ANSWER 42\n')
    (prefix / 'lib/pkgconfig/coast-answer.pc').write_text(
        'prefix=' + str(prefix) + '\nName: Coast Answer\nDescription: Native program fixture\nVersion: 1.2.3\nCflags: -I${prefix}/include\nLibs:\n')
    os.environ['PKG_CONFIG_PATH'] = str(prefix / 'lib/pkgconfig')
    assert run(['pkg-config', '--modversion', 'coast-answer']) == b'1.2.3\n'
    flags = run(['pkgconf', '--cflags', 'coast-answer']).decode().strip()
    assert flags == '-I' + str(prefix / 'include')
    run(['pkgconf', '--exists', 'coast-answer >= 1.2'])
    run(['pkgconf', '--exists', 'coast-answer >= 2.0'], code=1)
    passed('pkgconf and pkg-config resolve include paths, versions and constraints')

    Path('main.c').write_text('#include <stdio.h>\n#include "answer.h"\nint main(void) { printf("NINJA_ANSWER=%d\\n", ANSWER); return 0; }\n')
    Path('build.ninja').write_text('rule compile\n  command = gcc -O0 ' + flags + ' -MMD -MF $out.d -c $in -o $out\n  depfile = $out.d\n  deps = gcc\nrule link\n  command = gcc $in -o $out\nbuild main.o: compile main.c\nbuild answer: link main.o\ndefault answer\n')
    first = run(['ninja', '-j2']); assert b'2/2' in first, first
    assert run(['./answer']) == b'NINJA_ANSWER=42\n'
    assert b'no work to do' in run(['ninja'])
    time.sleep(1.05)
    (prefix / 'include/answer.h').write_text('#define ANSWER 84\n')
    assert b'2/2' in run(['ninja', '-j2'])
    assert run(['./answer']) == b'NINJA_ANSWER=84\n'
    run(['ninja', '-t', 'clean']); assert not Path('answer').exists()
    run(['ninja', '-j2']); assert run(['./answer']) == b'NINJA_ANSWER=84\n'
    passed('Ninja invokes native gcc, links and runs ELF, tracks headers, skips unchanged work and cleans')

    generated = run(['gperf', '-L', 'ANSI-C', '-t', '-N', 'lookup_keyword'],
                    b'%{\n#include <string.h>\n%}\nstruct keyword { const char *name; int value; };\n%%\nred, 1\ngreen, 2\nblue, 3\n%%\n')
    Path('keywords.c').write_bytes(generated + b'\nint main(void) { const struct keyword *k = lookup_keyword("green", 5); return !(k && k->value == 2 && !lookup_keyword("unknown", 7)); }\n')
    run(['gcc', '-O0', 'keywords.c', '-o', 'keywords'])
    run(['./keywords'])
    passed('gperf generates a real perfect hash table compiled and executed by native gcc')

    for command, arguments in [('zstd', ['-q', '-f', 'payload.bin', '/usr/bin/extra-denied.zst']),
                               ('rsync', ['-r', str(source) + '/', '/usr/extra-denied/'])]:
        result = subprocess.run([command, *arguments], capture_output=True, timeout=30)
        assert result.returncode != 0, command
    assert not Path('/usr/bin/extra-denied.zst').exists() and not Path('/usr/extra-denied').exists()
    passed('new programs retain ordinary-account system-write denial')
    Path('receipt.json').write_text(json.dumps(dict(uid=os.getuid(), payload_sha256=hashlib.sha256(payload).hexdigest(), state='passed')) + '\n')
    print('EXTRA_PROGRAMS_ALL_PASS', flush=True)


if __name__ == '__main__':
    main()

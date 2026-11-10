#!/usr/bin/env python3
"""Compile, link and run real C/C++ programs using LLVM only inside Coast."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--sysroot', type=Path, default=Path('/'))
    args = parser.parse_args()
    if platform.system() != 'Coast':
        parser.error('Run these target programs inside Coast')
    runtime, sysroot = args.runtime.resolve(strict=True), args.sysroot.resolve(strict=True)
    work = Path(tempfile.mkdtemp(prefix='llvm-check-', dir='/tmp'))
    binary = runtime / 'usr/bin'
    clang, cxx, lld = [binary / name for name in ['clang', 'clang++', 'ld.lld']]
    common = ['--no-default-config', '--target=x86_64-linux-musl', '--sysroot=' + str(sysroot),
        '--gcc-install-dir=' + str(sysroot / 'usr/lib/gcc/x86_64-linux-musl/14.2.0'),
        '-L' + str(sysroot / 'usr/lib'), '-static', '-fno-pie', '-no-pie',
        '--unwindlib=none', '-mno-avx', '-mno-avx2']

    def run(command, success=True):
        result = subprocess.run(list(map(str, command)), cwd=work, stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=180)
        with (work / 'check.log').open('a') as log:
            log.write('$ ' + repr(list(map(str, command))) + '\n' + result.stdout + '\n')
        if (result.returncode == 0) != success:
            raise AssertionError(result.stdout[-5000:])
        return result.stdout

    for program in [clang, lld, binary / 'llvm-ar']:
        if '23.0.0' not in run([program, '--version']):
            raise AssertionError('Unexpected LLVM source version: ' + str(program))
    source = work / 'hello.c'
    source.write_text('#include <stdint.h>\n#include <stdio.h>\n'
        'int main(void){uint64_t n=0;for(unsigned i=0;i<1000;i++)n+=i;'
        'printf("LLVM_C_PASS %llu\\n",(unsigned long long)n);return n!=499500;}\n')
    for mode in ['-fintegrated-cc1', '-fno-integrated-cc1']:
        run([clang, *common, '-std=c11', '-O2', mode, source, '-o', work / 'hello'])
        assert 'LLVM_C_PASS 499500' in run([work / 'hello'])
    root = Path(__file__).resolve().parents[2]
    threads = root / 'ports/build-tools/thread_check.cc'
    run([cxx, *common, '-std=c++17', '-O2', '-pthread', threads, '-o', work / 'threads'])
    assert 'CXX_THREADS_PASS' in run([work / 'threads'])
    run([cxx, *common, '-std=c++17', '-O2', '-pthread', '-fuse-ld=lld', '--ld-path=' + str(lld),
        threads, '-o', work / 'threads-lld'])
    assert 'CXX_THREADS_PASS' in run([work / 'threads-lld'])
    run([clang, *common, '-O2', '-flto=thin', '-fuse-ld=lld', '--ld-path=' + str(lld), source,
        '-o', work / 'thin-lto'])
    assert 'LLVM_C_PASS 499500' in run([work / 'thin-lto'])
    library = work / 'library.c'
    library.write_text('int source_answer(void){return 42;}\n')
    consumer = work / 'consumer.c'
    consumer.write_text('int source_answer(void);int main(void){return source_answer()!=42;}\n')
    run([clang, *common, '-c', library, '-o', work / 'library.o'])
    run([binary / 'llvm-ar', 'rcs', work / 'library.a', work / 'library.o'])
    run([binary / 'llvm-ranlib', work / 'library.a'])
    assert 'source_answer' in run([binary / 'llvm-nm', work / 'library.a'])
    run([clang, *common, consumer, work / 'library.a', '-o', work / 'consumer'])
    run([work / 'consumer'])
    assert 'EM_X86_64' in run([binary / 'llvm-readobj', '--file-headers', work / 'consumer'])
    stripped = work / 'consumer-stripped'
    run([binary / 'llvm-strip', '--strip-all', work / 'consumer', '-o', stripped])
    run([stripped])

    def parallel(index):
        output = work / ('parallel-' + str(index))
        run([clang, *common, '-fno-integrated-cc1', '-O1', source, '-o', output])
        assert 'LLVM_C_PASS 499500' in run([output])

    with ThreadPoolExecutor(max_workers=4) as executor:
        list(executor.map(parallel, range(8)))
    broken = work / 'broken.c'
    broken.write_text('int main(void){return intentional_error;}\n')
    assert 'intentional_error' in run([clang, *common, broken, '-o', work / 'broken'], success=False)
    assert not (work / 'broken').exists()
    checks = ['pinned LLVM source version', 'integrated and separate C compiler processes',
        'C++ threads, futures, TLS, destructors and exceptions', 'LLD links and runs the C++ thread consumer',
        'ThinLTO compilation, linking and execution', 'archive, index and symbol tools produce a working library',
        'ELF inspection and stripping preserve executable behavior',
        'four Python workers compile and run eight programs', 'compiler errors propagate without an executable']
    (work / 'report.json').write_text(json.dumps(dict(state='passed', checks=checks,
        clang_sha256=hashlib.sha256(clang.read_bytes()).hexdigest(),
        target_execution_on_linux=False), indent=2) + '\n')
    print('LLVM_ALL_PASS ' + str(len(checks)) + ' report=' + str(work / 'report.json'), flush=True)


if __name__ == '__main__':
    main()

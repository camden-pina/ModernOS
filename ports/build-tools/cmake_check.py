#!/usr/bin/env python3
"""Compile, run, test, install and package a real native CMake C/C++ project."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import tarfile
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, default=Path('/'))
    parser.add_argument('--ninja', type=Path)
    args = parser.parse_args()
    if platform.system() != 'Coast':
        parser.error('Run this integration check inside Coast')
    runtime = args.runtime.resolve(strict=True)
    tools = runtime / 'usr/bin'
    work = Path(tempfile.mkdtemp(prefix='cmake-check-', dir='/tmp'))
    source = work / 'source'
    source.mkdir()
    (source / 'answer.c').write_text('int answer(void) { return 42; }\n')
    (source / 'main.cc').write_text('#include <atomic>\n#include <cstdio>\n#include <thread>\n'
        'extern "C" int answer(void);\nint main() { std::atomic<int> n{0}; '
        'std::thread a([&]{ n += answer(); }), b([&]{ n += answer(); }); '
        'a.join(); b.join(); std::printf("CMAKE_NATIVE_PROGRAM %d\\n", n.load()); return n != 84; }\n')
    (source / 'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.20)\n'
        'project(CoastConsumer VERSION 1.0 LANGUAGES C CXX)\n'
        'include(CheckCXXSourceRuns)\n'
        'check_cxx_source_runs("int main(){return 0;}" COAST_COMPILER_RUNS)\n'
        'if(NOT COAST_COMPILER_RUNS)\nmessage(FATAL_ERROR "Native compiler output did not run")\nendif()\n'
        'add_library(answer STATIC answer.c)\nadd_executable(consumer main.cc)\n'
        'target_link_libraries(consumer PRIVATE answer pthread)\nenable_testing()\n'
        'add_test(NAME first COMMAND consumer)\nadd_test(NAME second COMMAND consumer)\n'
        'install(TARGETS consumer RUNTIME DESTINATION bin)\n'
        'set(CPACK_PACKAGE_NAME CoastConsumer)\nset(CPACK_GENERATOR TGZ)\ninclude(CPack)\n')
    checks = []

    def run(command, cwd=work):
        result = subprocess.run(list(map(str, command)), cwd=cwd, stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=180)
        with (work / 'check.log').open('a') as log:
            log.write('$ ' + repr(command) + '\n' + result.stdout + '\n')
        if result.returncode:
            raise AssertionError(result.stdout[-5000:])
        return result.stdout

    for generator in ('Unix Makefiles', 'Ninja'):
        build = work / generator.replace(' ', '-')
        configure = [tools / 'cmake', '-S', source, '-B', build, '-G', generator,
            '-DCMAKE_C_FLAGS=-O2 -mno-avx -mno-avx2 -fno-pie',
            '-DCMAKE_CXX_FLAGS=-O2 -mno-avx -mno-avx2 -fno-pie',
            '-DCMAKE_EXE_LINKER_FLAGS=-static -no-pie']
        if generator == 'Ninja' and args.ninja:
            configure += ['-DCMAKE_MAKE_PROGRAM=' + str(args.ninja.resolve(strict=True))]
        run(configure)
        run([tools / 'cmake', '--build', build, '--parallel', '2'])
        assert 'CMAKE_NATIVE_PROGRAM 84' in run([build / 'consumer'])
        assert '100% tests passed' in run([tools / 'ctest', '--test-dir', build, '--parallel', '2', '--output-on-failure'])
        installed = build / 'installed'
        run([tools / 'cmake', '--install', build, '--prefix', installed])
        assert (installed / 'bin/consumer').read_bytes() == (build / 'consumer').read_bytes()
        run([tools / 'cpack', '--config', build / 'CPackConfig.cmake', '-G', 'TGZ', '-B', build / 'packages'])
        archive = next((build / 'packages').glob('*.tar.gz'))
        with tarfile.open(archive) as bundle:
            consumer = next(m for m in bundle if m.isfile() and m.name.endswith('/bin/consumer'))
            assert bundle.extractfile(consumer).read() == (build / 'consumer').read_bytes()
        checks.append(generator + ': configure, compiler try-run, parallel C/C++, threads, CTest, install and CPack bytes')
        print('CMAKE_PASS ' + checks[-1], flush=True)
    (work / 'report.json').write_text(json.dumps(dict(state='passed', checks=checks,
        cmake_sha256=hashlib.sha256((tools / 'cmake').read_bytes()).hexdigest()), indent=2) + '\n')
    print('CMAKE_ALL_PASS ' + str(len(checks)) + ' report=' + str(work / 'report.json'), flush=True)


if __name__ == '__main__':
    main()

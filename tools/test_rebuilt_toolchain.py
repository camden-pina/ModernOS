#!/usr/bin/env python3
"""Validate/export the real native compiler from a completed RAM checkpoint.

Compilation, linking, execution and archive creation all happen in ModernOS.
The host reads finished RAMFS artifacts with QEMU's read-only memory monitor.
"""
import argparse
import hashlib
import json
from pathlib import Path
import posixpath
import re
import shlex
import uuid

from guest_ramfs import GuestRamFS, boot_kernel
from test_runtime import VM, WORK


CPP17 = '''#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
int main() {
    std::vector<int> values{2, 3, 5};
    if (values.size() != 3 || values.at(2) != 5) return 1;
    try { throw std::runtime_error("NATIVE_CPP17_OK"); }
    catch (const std::exception& e) {
        std::cout << std::string(e.what()) << std::endl;
        return 0;
    }
    return 1;
}
'''

CPP20 = '''#include <iostream>
#include <span>
#include <vector>
static_assert(__cplusplus == 202002L);
int main() {
    std::vector<int> values{1, 4, 7};
    std::span<const int> view(values);
    int total = 0;
    for (int value : view) total += value;
    if (total != 12 || view.subspan(1).size() != 2) return 1;
    std::cout << "NATIVE_CPP20_OK" << std::endl;
}
'''


def write_source(shell, path, contents):
    # Literal source bytes/newlines pass through the real guest shell, within
    # the console line limit. No host-generated binaries enter the guest.
    for offset in range(0, len(contents), 220):
        part = contents[offset:offset + 220].replace('\\', '\\\\').replace('\n', '\\n').replace('\t', '\\t')
        shell('printf %b ' + shlex.quote(part) + (' > ' if not offset else ' >> ') + shlex.quote(path))


def exercise(checkpoint, destination, prefix='/tmp/toolchain/install', abi_source_root=None):
    checkpoint = Path(checkpoint).resolve()
    destination = Path(destination).resolve()
    if not prefix.startswith('/') or any(p in ('.', '..') for p in prefix.split('/')):
        raise ValueError('Use an absolute, normalized compiler prefix')
    abi_sources = {}
    if abi_source_root is not None:
        source_root = Path(abi_source_root).resolve()
        for source, target in (('compiler-check.c', 'abi-check.c'),
                               ('include/memory_usage.h', 'include/memory_usage.h')):
            data = (source_root / source).read_bytes()
            abi_sources[target] = dict(contents=data.decode('utf-8'),
                                       sha256=hashlib.sha256(data).hexdigest())

    manifest = json.loads(Path(str(checkpoint) + '.json').read_text())
    configuration = manifest['configuration']
    vm = VM(configuration['cpus'], bios=configuration['bios'],
            storage=configuration['storage'], iso=configuration['iso'],
            buffered=configuration['buffered'], memory=configuration['memory'],
            network=configuration['network'], cpu=configuration.get('cpu'),
            accel=configuration.get('accel', 'tcg'),
            network_model=configuration.get('network_model', 'e1000e'),
            label='rebuilt-' + uuid.uuid4().hex[:8], resume=checkpoint)
    artifacts = destination.with_name(destination.name + '.proofs')
    artifacts.mkdir(parents=True, exist_ok=True)
    directory = '/tmp/rebuilt-validation'

    def shell(script, expected=None, timeout=180):
        output = vm.shell_command(script, expected, timeout=timeout)
        if re.search(r'pid \d+ (?:exited [1-9]|terminated by signal)|FAIL:|PANIC:', output):
            raise AssertionError(output)
        print(output, flush=True)
        return output

    try:
        shell('test "$(cat /tmp/rebuild.status)" = 0 && '
              'test "$(cat /tmp/toolchain/.modernos-stage)" = complete && echo REBUILD_COMPLETE',
              'REBUILD_COMPLETE')
        compiler = shell(shlex.quote(prefix + '/bin/gcc') + ' --version', 'ModernOS native rebuild')
        runtimes = {}
        for library in ('libgcc.a', 'libstdc++.a'):
            output = shell(shlex.quote(prefix + '/bin/g++') + ' -print-file-name=' + library)
            paths = re.findall(r'^(/[^\n]+)$', output, re.M)
            if len(paths) != 1 or not posixpath.normpath(paths[0]).startswith(prefix.rstrip('/') + '/'):
                raise AssertionError(
                    'Compiler resolved a runtime outside its rebuilt installation: ' + output)
            runtimes[library] = paths[0]
            shell('test -f ' + shlex.quote(paths[0]))

        shell('mkdir -p ' + directory + '; cp -R /usr/src/modernos/examples/. ' + directory + '/')

        # A resumed checkpoint retains its packaged examples. Explicitly
        # stage only source text when validating a corrected ABI example;
        # the rebuilt compiler and all other checkpoint files stay intact.
        if abi_sources:
            shell('mkdir -p ' + directory + '/include')
            for target, source in abi_sources.items():
                path = directory + '/' + target
                write_source(shell, path, source['contents'])
                output = shell('sha256sum ' + shlex.quote(path))
                if not re.search(r'^' + source['sha256'] + '  ' + re.escape(path) + r'$', output, re.M):
                    raise AssertionError(
                        'Staged ABI source differs from the host source: ' + output)

            print('Staged ABI sources: ' + json.dumps(
                {target: source['sha256'] for target, source in abi_sources.items()},
                sort_keys=True), flush=True)

        # Explicit driver paths ensure the installed seed cannot supply a proof.
        shell('cd ' + directory + '; make -j' + str(configuration['cpus']) +
              ' CC=' + shlex.quote(prefix + '/bin/gcc') +
              ' CXX=' + shlex.quote(prefix + '/bin/g++') + ' check', 'NATIVE_CPP_OK', timeout=300)
        shell(shlex.quote(prefix + '/bin/gcc') + ' -O2 ' + directory + '/abi-check.c -o ' +
              directory + '/abi-check && ' + directory + '/abi-check', 'COMPILER ABI CHECK PASS')

        for standard, source in ((17, CPP17), (20, CPP20)):
            write_source(shell, directory + '/cpp' + str(standard) + '.cpp', source)
            binary = directory + '/cpp' + str(standard)
            shell(shlex.quote(prefix + '/bin/g++') + ' -std=c++' + str(standard) + ' -O2 ' +
                  binary + '.cpp -o ' + binary + ' && ' + binary,
                  'NATIVE_CPP' + str(standard) + '_OK')

        for name in ('hello-c', 'hello-cxx', 'abi-check', 'cpp17', 'cpp20'):
            info = shell(shlex.quote(prefix + '/bin/readelf') + ' -hl ' + directory + '/' + name)
            if 'EXEC (Executable file)' not in info or 'INTERP' in info or ' RWE ' in info:
                raise AssertionError('Rebuilt compiler emitted an unsupported ELF: ' + info)

        print('PASS rebuilt GCC/G++: C, C++17 exceptions/containers, C++20 span, compiler ABI, static ELF', flush=True)
        archive = '/tmp/native-toolchain-export.tar.gz'
        shell('tar -czf ' + archive + ' -C ' + shlex.quote(prefix) + ' .', timeout=1800)
        output = shell('sha256sum ' + archive)
        match = re.search(r'\n([0-9a-f]{64})  ' + re.escape(archive) + r'\n', output)
        if not match:
            raise AssertionError('No guest archive digest: ' + output)
        destination.parent.mkdir(parents=True, exist_ok=True)
        vm.qmp('stop')

        try:
            ramfs = GuestRamFS(vm, boot_kernel(configuration['iso']), artifacts / 'memory')
            digest = ramfs.export(archive, destination)
            if digest != match.group(1):
                raise AssertionError('Native archive differs from its guest SHA-256')
            for name in ('hello-c', 'hello-cxx', 'abi-check', 'cpp17', 'cpp20'):
                ramfs.export(directory + '/' + name, artifacts / name)
            ramfs.export('/tmp/rebuild.log', artifacts / 'rebuild.log')
        finally:
            vm.qmp('cont')

        record = dict(archive=str(destination), sha256=digest, bytes=destination.stat().st_size,
                      prefix=prefix, checkpoint=str(checkpoint), configuration=configuration,
                      compiler_version=compiler.splitlines(),
                      runtimes=runtimes,
                      staged_abi_sources={target: source['sha256']
                                          for target, source in abi_sources.items()},
                      proofs=['C', 'C++17 exceptions/containers', 'C++20 span', 'compiler ABI', 'static ELF'])
        destination.with_name(destination.name + '.json').write_text(json.dumps(record, indent=2) + '\n')
        destination.with_name(destination.name + '.sha256').write_text(digest + '  ' + destination.name + '\n')

        print('PASS native SDK export: ' + str(destination) + '\nSHA256 ' + digest, flush=True)
    finally:
        vm.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('checkpoint', help='Completed native-rebuild RAM checkpoint')
    parser.add_argument('--output', default=str(WORK.parent / 'native-toolchain.tar.gz'))
    parser.add_argument('--prefix', default='/tmp/toolchain/install')
    parser.add_argument('--abi-source-root',
                        help='Explicitly stage compiler-check.c and include/memory_usage.h '
                             'from this source directory into the resumed validation directory')
    args = parser.parse_args()

    exercise(args.checkpoint, args.output, args.prefix, args.abi_source_root)

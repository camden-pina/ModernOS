#!/usr/bin/env python3
"""Compile and execute real C/C++ consumers in an owned fresh desktop VM."""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import traceback

from source_tree import digest
import test_runtime as runtime

C_SOURCE = r'''#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
static void *worker(void *value) { return (void *)((uintptr_t)value + 1); }
int main(void) {
    pthread_t thread; void *result = 0;
    if (pthread_create(&thread, 0, worker, (void *)41) ||
        pthread_join(thread, &result) || (uintptr_t)result != 42) return 1;
    FILE *file = fopen("/home/user/source-c-output.txt", "w");
    if (!file || fputs("source-c-file-bytes\n", file) < 0 || fclose(file)) return 2;
    puts("SOURCE_NATIVE_C_PASS threads static_sdk file_io");
    return 0;
}
'''

CXX_SOURCE = r'''#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>
thread_local int marker = 7;
int main() {
    std::atomic<int> answer{0};
    std::thread thread([&] { marker = 11; answer.store(marker); });
    thread.join();
    std::vector<int> values{13, 29};
    if (marker != 7 || answer.load() != 11 || values[0] + values[1] != 42) return 1;
    try { throw std::runtime_error("source-exception"); }
    catch (const std::runtime_error&) {
        std::puts("SOURCE_NATIVE_CXX_PASS posix_threads tls containers exceptions");
        return 0;
    }
    return 2;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, required=True)
    parser.add_argument('--output-root', type=Path, required=True)
    args = parser.parse_args()
    iso = args.iso.resolve(strict=True)
    image = json.loads(iso.with_name('MANIFEST.json').read_text())
    component = image['components']['compiler_build']
    if digest(iso) != image['iso_sha256'] or digest(Path(component['path'])) != component['sha256']:
        parser.error('Select the image containing this exact compiler component')
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=False)
    runtime.WORK = out
    report = dict(state='failed', checks=[], iso_sha256=digest(iso), network=False,
        extra_disks=False, compiler_build_sha256=component['sha256'],
        source_sha256={str(Path(__file__).relative_to(runtime.ROOT)): digest(Path(__file__))})
    vm = None

    def check(name, command, expected, timeout=180):
        output = vm.shell_command(command, expect=expected, as_user=True, timeout=timeout)
        (out / (f'{len(report["checks"]):02d}-output.txt')).write_text(output)
        report['checks'].append(name)
        print('PASS ' + name, flush=True)

    try:
        vm = runtime.VM(4, storage=False, network=False, memory='16G', accel='kvm',
            iso=iso, label='fresh-source-development')
        vm.command('true')
        for name, source in [('source-native.c', C_SOURCE), ('source-native.cpp', CXX_SOURCE)]:
            (out / name).write_text(source)
            encoded = base64.b64encode(source.encode()).decode()
            target = '/tmp/' + name
            vm.shell_command(': > ' + target + '.b64', as_user=True)
            for offset in range(0, len(encoded), 200):
                vm.shell_command("printf '%s' '" + encoded[offset:offset + 200] + "' >> " + target + '.b64', as_user=True)
            check('transfer and verify ' + name,
                '/bin/busybox base64 -d ' + target + '.b64 > ' + target +
                ' && /bin/busybox sha256sum ' + target, hashlib.sha256(source.encode()).hexdigest())
        check('native GCC compiles and runs a threaded C/file consumer',
            '/usr/bin/gcc -O2 -pthread /tmp/source-native.c -o /tmp/source-native-c && /tmp/source-native-c',
            'SOURCE_NATIVE_C_PASS', timeout=600)
        check('native G++ compiles and runs POSIX threads, TLS, containers and exceptions',
            '/usr/bin/g++ -O2 -pthread /tmp/source-native.cpp -o /tmp/source-native-cxx && /tmp/source-native-cxx',
            'SOURCE_NATIVE_CXX_PASS', timeout=600)
        check('the native-compiled program writes the expected user file',
            'cat /home/user/source-c-output.txt', 'source-c-file-bytes')
        vm.command('check', expect='USERLAND CHECK PASS', timeout=120)
        report['checks'].append('kernel and userland remain healthy after compiler children')
        if digest(iso) != report['iso_sha256']:
            raise ValueError('The development-tested image changed')
        report.update(state='passed', image_unchanged=True)
    except BaseException as error:
        report['error'] = repr(error)
        (out / 'exception.txt').write_text(traceback.format_exc())
        raise
    finally:
        if vm:
            vm.close()
        report['owned_vm_stopped'] = bool(vm and vm.process.poll() is not None)
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()

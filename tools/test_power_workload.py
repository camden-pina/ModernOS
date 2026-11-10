#!/usr/bin/env python3
"""Run the actual fixture verifier with POSIX file I/O and mocked guest syscalls.

This is host evidence only. It does not establish FAT persistence or real guest
power behavior. --generate-only validates Python/source generation without
running a compiler, linker, guest, or native test binary.
"""
import argparse
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / 'tools/fixtures/power-workload.c'
SIZE = 128 * 1024
MARKER = 'POWER WORKLOAD VERIFIED bytes=131072 eof=1'
CAPABILITY = 'POWER WORKLOAD CAPABILITY verify=1 bytes=131072 eof=1'

# Compile the real fixture separately so its ABI definitions never collide
# with host libc headers. All guest file and syscall entry points are adapters.
HEADER = r'''
#ifndef POWER_WORKLOAD_TEST_LIBC_H
#define POWER_WORKLOAD_TEST_LIBC_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <uapi/abi.h>
int fixture_open(const char *, int, unsigned);
long fixture_read(int, void *, size_t);
int fixture_close(int);
long fixture_lseek(int, long, int);
int fixture_write_all(int, const void *, size_t);
long fixture_syscall6(long, long, long, long, long, long, long);
#define open fixture_open
#define read fixture_read
#define close fixture_close
#define lseek fixture_lseek
#define write_all fixture_write_all
#define call2(n,a,b) fixture_syscall6(n,(long)(a),(long)(b),0,0,0,0)
#define call3(n,a,b,c) fixture_syscall6(n,(long)(a),(long)(b),(long)(c),0,0,0)
#endif
'''

SOURCE = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int power_workload_main(int, char **);
static unsigned opens, successful_opens, closes, guest_syscalls, unsafe_io;
static unsigned reads, read_errors;
static int opened_fd = -1, fail_open, fail_close;
static long error_at = -1;
static size_t read_cap, consumed;

int fixture_open(const char *path, int flags, unsigned mode) {
    opens++;
    /* Coast O_RDONLY is zero. Refuse rather than execute mutating flags. */
    if (flags != 0 || mode != 0) { unsafe_io++; return -EPERM; }
    if (fail_open) return -EACCES;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -errno;
    assert(opened_fd == -1);
    successful_opens++;
    opened_fd = fd;
    return fd;
}
long fixture_read(int fd, void *data, size_t size) {
    assert(fd == opened_fd && fd >= 0);
    reads++;
    if (error_at >= 0 && consumed >= (size_t)error_at) {
        read_errors++;
        return -EIO;
    }
    if (read_cap && size > read_cap) size = read_cap;
    if (error_at >= 0 && consumed < (size_t)error_at &&
            size > (size_t)error_at - consumed)
        size = (size_t)error_at - consumed;
    ssize_t n = read(fd, data, size);
    if (n < 0) return -errno;
    consumed += (size_t)n;
    return n;
}
int fixture_close(int fd) {
    assert(fd == opened_fd && fd >= 0);
    closes++;
    opened_fd = -1;
    int result = close(fd);
    if (result < 0) return -errno;
    /* The descriptor is really closed even when injecting a close failure. */
    return fail_close ? -EIO : 0;
}
long fixture_lseek(int fd, long offset, int whence) {
    (void)fd; (void)offset; (void)whence;
    unsafe_io++;
    return -EPERM;
}
int fixture_write_all(int fd, const void *data, size_t size) {
    (void)fd; (void)data; (void)size;
    unsafe_io++;
    return -EPERM;
}
long fixture_syscall6(long n, long a, long b, long c, long d, long e, long f) {
    (void)n; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
    guest_syscalls++;
    return -EPERM;
}
int main(int argc, char **argv) {
    assert(argc == 9);
    read_cap = (size_t)strtoul(argv[2], NULL, 10);
    error_at = strtol(argv[3], NULL, 10);
    fail_open = atoi(argv[4]);
    fail_close = atoi(argv[5]);
    int expected_success = atoi(argv[6]);
    int expected_read_error = atoi(argv[7]);
    int capability = !strcmp(argv[8], "capability");
    char *args[] = {"power-workload", argv[1], "verify", NULL};
    char *capability_args[] = {"power-workload", "--verify-capability", NULL};
    int result = capability ? power_workload_main(2, capability_args) :
                              power_workload_main(3, args);
    assert((result == 0) == expected_success);
    assert(opens == (capability ? 0U : 1U) && closes == successful_opens);
    assert(opened_fd == -1 && guest_syscalls == 0 && unsafe_io == 0);
    assert((read_errors != 0) == expected_read_error);
    if (capability) assert(reads == 0 && consumed == 0);
    else if (expected_success) {
        assert(consumed == 131072 && reads > 1);
        if (read_cap) assert(reads >= (131072 + read_cap - 1) / read_cap + 1);
    }
    printf("HOST ADAPTER PASS opens=%u closes=%u guest_syscalls=%u unsafe_io=%u\n",
           opens, closes, guest_syscalls, unsafe_io);
    return 0;
}
'''


def generate(folder):
    """Prepare the exact C adapter and compiler argv; do not execute them."""
    header = folder / 'libc.h'
    source = folder / 'adapter.c'
    header.write_text(HEADER)
    source.write_text(SOURCE)
    compiler = shlex.split(os.environ.get('HOST_CC', 'cc'))
    common = compiler + ['-std=gnu11', '-O1', '-g', '-Wall', '-Wextra',
                         '-Werror', '-fsanitize=address,undefined',
                         '-fno-omit-frame-pointer']
    fixture_object = folder / 'power-workload.o'
    binary = folder / 'test-power-workload'
    commands = [common + ['-I' + str(folder), '-idirafter', str(ROOT / 'include'),
                          '-Dmain=power_workload_main', '-c', str(FIXTURE),
                          '-o', str(fixture_object)],
                common + [str(source), str(fixture_object), '-o', str(binary)]]
    assert FIXTURE.is_file()
    assert header.read_text() == HEADER and source.read_text() == SOURCE
    assert commands[0][commands[0].index('-c') + 1] == str(FIXTURE)
    return commands, binary


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def exercise(binary, folder):
    # Build expected bytes independently of the fixture's block writer.
    correct = bytes((offset * 31 + 17) & 255 for offset in range(SIZE))
    tests = []

    def case(name, data, success=False, cap=0, error_at=-1, open_error=False,
             close_error=False):
        path = folder / (name + '.bin')
        if data is not None:
            path.write_bytes(data)
        tests.append((name, path, success, cap, error_at, open_error, close_error))

    case('correct', correct, True)
    for cap in (1, 7, 257, 4095):
        case('short-reads-' + str(cap), correct, True, cap=cap)
    for length in (0, 1, 4095, 4096, SIZE - 1):
        case('truncated-' + str(length), correct[:length])
    case('extra-byte', correct + b'\x00')
    case('extra-block', correct + correct[:4096])
    # Include the last byte and bytes past the original one-block readback.
    for offset in (0, 4095, 4096, 65536, SIZE - 1):
        corrupt = bytearray(correct)
        corrupt[offset] ^= 1
        case('corrupt-' + str(offset), corrupt)
    for offset in (0, 13, 4096, SIZE - 1, SIZE):
        case('read-error-' + str(offset), correct, error_at=offset)
    case('short-read-then-error', correct, cap=7, error_at=12345)
    case('missing', None)
    case('open-error', correct, open_error=True)
    case('close-error', correct, close_error=True)

    env = dict(os.environ)
    env['ASAN_OPTIONS'] = 'detect_leaks=0:halt_on_error=1'
    capability = subprocess.run(
        [str(binary), str(folder / 'never-opened'), '0', '-1', '0', '0', '1',
         '0', 'capability'], check=True, capture_output=True, text=True,
        timeout=15, env=env)
    assert capability.stdout.count(CAPABILITY) == 1, capability.stdout
    assert MARKER not in capability.stdout, capability.stdout
    assert 'POWER WORKLOAD FAIL' not in capability.stdout, capability.stdout
    assert 'HOST ADAPTER PASS opens=0 closes=0 guest_syscalls=0 unsafe_io=0' in (
        capability.stdout), capability.stdout
    for name, path, success, cap, error_at, open_error, close_error in tests:
        before = digest(path) if path.exists() else None
        command = [str(binary), str(path), str(cap), str(error_at),
                   str(int(open_error)), str(int(close_error)),
                   str(int(success)), str(int(error_at >= 0)), 'verify']
        result = subprocess.run(command, check=True, capture_output=True,
                                text=True, timeout=15, env=env)
        assert result.stdout.count(MARKER) == int(success), (name, result.stdout)
        assert ('POWER WORKLOAD FAIL' in result.stdout) == (not success), (
            name, result.stdout)
        assert 'HOST ADAPTER PASS' in result.stdout, (name, result.stdout)
        assert (digest(path) if path.exists() else None) == before, name
    print(f'POWER WORKLOAD HOST PASS: {len(tests) + 1} actual-C verifier cases; '
          'native read-only file I/O, injected failures, no guest syscalls')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--generate-only', action='store_true',
                        help='validate generated source/argv without compiling')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='coast-power-workload-') as tmp:
        folder = Path(tmp)
        commands, binary = generate(folder)
        if args.generate_only:
            print('POWER WORKLOAD GENERATION PASS: source and argv only; '
                  'C compilation and actual-C regression tests NOT RUN')
            return
        for command in commands:
            subprocess.run(command, check=True, timeout=60)
        exercise(binary, folder)


if __name__ == '__main__':
    main()

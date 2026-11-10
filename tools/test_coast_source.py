#!/usr/bin/env python3
"""Install the signed OS source kit and rebuild the desktop kernel inside Coast."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import threading
import traceback

from serve_repository import RepositoryServer
from source_tree import digest
import test_runtime as runtime


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, default=runtime.ROOT / 'build/Coast-browser.iso')
    parser.add_argument('--repository', type=Path, required=True)
    parser.add_argument('--output-root', type=Path, required=True)
    parser.add_argument('--boot-rebuilt', action='store_true', help='export the native-built kernel and boot it in a second owned VM (requires host gdb/xorriso)')
    parser.add_argument('--build-image', action='store_true', help='also build Limine and a complete native desktop ISO inside Coast')
    parser.add_argument('--image-tools-only', action='store_true', help='quick native FAT/ISO write/read regression without a full source build')
    args = parser.parse_args()
    args.iso = args.iso.resolve()
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=False)
    runtime.WORK = out
    report = dict(state='failed', checks=[], target_host_executed=False,
                  iso_sha256=digest(args.iso), harness_sha256=digest(Path(__file__)), exports={})
    with args.repository.open('rb') as stream:
        report['repository_manifest_sha256'] = hashlib.file_digest(stream, 'sha256').hexdigest()
    vm = server = thread = None

    def check(name, command, expected=None, user=True, timeout=120):
        output = vm.shell_command(command, expect=expected, as_user=user, timeout=timeout)
        (out / (f'{len(report["checks"]):02d}-output.txt')).write_text(output)
        report['checks'].append(name)
        print('PASS ' + name, flush=True)
        return output

    try:
        server = RepositoryServer(('127.0.0.1', 0), args.repository)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        vm = runtime.VM(4, storage=False, network=True, memory='16G', accel='kvm',
                        iso=args.iso, label='coast-source-build')
        vm.command('true')
        check('select local source repository', 'coast server http://10.0.2.2:' + str(server.server_port),
              'Package downloads and source builds now use')
        check('verify signed package metadata', 'pacman -Syy --noconfirm', 'Synchronizing', user=False)
        check('refresh the matching signed file database', 'pacman -Fyy --noconfirm', 'Synchronizing', user=False)
        if args.image_tools_only:
            check('install native image tools', 'coast install xorriso --noconfirm && coast install mtools --noconfirm',
                  'installing xorriso', timeout=120)
            check('write an actual ISO from native RAMFS inputs',
                  'mkdir -p /tmp/iso-input && echo COAST_ISO_BYTES_PASS > /tmp/iso-input/answer.txt && '
                  'xorriso -report_about DEBUG -as mkisofs -R -J -o /tmp/probe.iso /tmp/iso-input',
                  'Writing to', timeout=90)
            check('read exact file bytes back from the native-written ISO',
                  'xorriso -osirrox on -indev /tmp/probe.iso -extract /answer.txt /tmp/recovered.txt && '
                  'cmp /tmp/iso-input/answer.txt /tmp/recovered.txt && cat /tmp/recovered.txt',
                  'COAST_ISO_BYTES_PASS')
            report['state'] = 'passed'
            return
        check('install source kit and native build dependencies', 'coast install coast-build --noconfirm',
              'installing coast-build', timeout=600)
        check('verify source and assembler package ownership', 'pacman -Qkk coast-source nasm', '0 altered files')
        check('native assembler and build system versions', 'nasm --version && meson --version', '1.10.2')
        check('native ISO creation tool runs', 'xorriso -version', '1.5.8')
        check('native FAT tool writes and reads exact file bytes',
              'python3 -c "open(\'/tmp/coast-fat.img\',\'wb\').truncate(1474560)" && '
              'mformat -i /tmp/coast-fat.img -f 1440 :: && echo COAST_FAT_TOOLS_PASS > /tmp/answer.txt && '
              'mcopy -i /tmp/coast-fat.img /tmp/answer.txt ::ANSWER.TXT && mtype -i /tmp/coast-fat.img ::ANSWER.TXT',
              'COAST_FAT_TOOLS_PASS')
        check('extract complete source kit as the desktop user',
              'mkdir -p /home/user/Builds/coast && cd /home/user/Builds/coast && '
              'tar -xJf /usr/src/coast/os/coast-source.tar.xz --strip-components=1 && '
              'test -f ports/electron/sources.lock.json && test -f userland/desktop/desktop.c && echo SOURCE_KIT_UNPACKED',
              'SOURCE_KIT_UNPACKED', timeout=240)
        check('write a real assembler input',
              "cd /home/user/Builds/coast && printf 'section .text\\nglobal coast_asm_answer\\ncoast_asm_answer: mov eax,42\\nret\\n' > answer.asm && "
              'nasm -f elf64 answer.asm -o answer.o')
        check('write the GCC consumer',
              'cd /home/user/Builds/coast && '
              "printf '#include <stdio.h>\\nextern int coast_asm_answer(void); int main(void){int n=coast_asm_answer();printf(\"NATIVE_ASSEMBLER_PASS %%d\\\\n\",n);return n!=42;}\\n' > answer.c && "
              'test -s answer.c')
        check('link and run the assembled GCC consumer', 'cd /home/user/Builds/coast && '
              'gcc -static -no-pie -Wl,-z,noexecstack answer.c answer.o -o answer && ./answer',
              'NATIVE_ASSEMBLER_PASS 42')
        check('compile and link the full Chromium desktop kernel using native GCC',
              'cd /home/user/Builds/coast && python3 -B tools/build_desktop_kernel.py --jobs 4 > /tmp/coast-kernel-build.log 2>&1; '
              'result=$?; tail -35 /tmp/coast-kernel-build.log; test "$result" = 0 && echo COAST_KERNEL_SOURCE_BUILD_PASS',
              'COAST_KERNEL_SOURCE_BUILD_PASS', timeout=1200)
        check('native build produces a genuine x86-64 ELF kernel',
              'readelf -h /home/user/Builds/coast/build/desktop-kernel/vmlinux', 'Advanced Micro Devices X86-64')
        check('compile the native desktop with Coast GCC',
              'cd /home/user/Builds/coast && make -C userland bin/gui -j4 > /tmp/gui-build.log 2>&1; '
              'result=$?; tail -8 /tmp/gui-build.log; test "$result" = 0 && echo COAST_DESKTOP_SOURCE_BUILD_PASS',
              'COAST_DESKTOP_SOURCE_BUILD_PASS', timeout=600)
        check('install the source-built desktop at its admitted broker path',
              'cp /home/user/Builds/coast/userland/bin/gui /bin/gui && '
              'chown 0:0 /bin/gui && chmod 755 /bin/gui', user=False)
        check('run the source-built native desktop info path',
              '/bin/gui --info', 'display ABI=')
        check('compile against the installed Wayland SDK',
              'cd /home/user/Builds/coast && export PKG_CONFIG_PATH=/usr/lib/coast/wayland-sdk/lib/pkgconfig; '
              'gcc -static -no-pie $(pkg-config --cflags wayland-server) ports/wayland/sdk-check.c '
              '$(pkg-config --static --libs wayland-server) -o /tmp/wayland-check && /tmp/wayland-check',
              'COAST_WAYLAND_SDK_PASS 1.21.0')
        check('refresh the source recipe catalog', 'coast refresh', 'Updated signed source catalog')
        check('fetch native NASM source recipe', 'coast clone nasm', 'Recipe checkout')
        check('build and check NASM from source with makepkg',
              'cd /home/user/Builds/nasm && makepkg --noconfirm', 'Finished making', timeout=600)
        if args.build_image:
            check('compile Limine and assemble the complete native desktop image inside Coast',
                  'cd /home/user/Builds/coast && python3 -B tools/build_native_iso.py '
                  '--kernel build/desktop-kernel/vmlinux --jobs 4 > /tmp/coast-image.log 2>&1; '
                  'result=$?; tail -70 /tmp/coast-image.log; test "$result" = 0 && echo COAST_NATIVE_IMAGE_PASS',
                  'COAST_NATIVE_IMAGE_PASS', timeout=900)
        vm.command('check', expect='USERLAND CHECK PASS', timeout=90)
        report['checks'].append('Coast remains healthy after native kernel and assembler builds')
        if args.boot_rebuilt:
            from guest_ramfs import GuestRamFS, boot_kernel
            vm.qmp('stop')
            try:
                fs = GuestRamFS(vm, boot_kernel(args.iso), out / 'native-export')
                kernel = out / 'native-vmlinux'
                report['native_kernel_sha256'] = fs.export('/home/user/Builds/coast/build/desktop-kernel/vmlinux', kernel)
                report['exports'][kernel.name] = report['native_kernel_sha256']
                if args.build_image:
                    image = out / 'native-from-sources.iso'
                    report['native_image_sha256'] = fs.export('/home/user/Builds/coast/build/native-image/Coast-native.iso', image)
                    report['exports'][image.name] = report['native_image_sha256']
            finally:
                vm.qmp('cont')
            vm.close()
            if not args.build_image:
                image = out / 'native-kernel.iso'
                subprocess.run(['cp', '--reflink=auto', str(args.iso), str(image)], check=True)
                with (out / 'native-image.log').open('w') as log:
                    subprocess.run(['xorriso', '-no_rc', '-dev', str(image), '-map', str(kernel), '/boot/kernel.elf',
                        '-boot_image', 'any', 'keep', '-commit', '-end'], stdout=log, stderr=subprocess.STDOUT, check=True)
            vm = runtime.VM(4, storage=False, network=False, memory='16G', accel='kvm', iso=image, label='native-rebuilt')
            vm.command('check', expect='USERLAND CHECK PASS', timeout=90)
            report['checks'].append('boot the native-built kernel and pass actual guest ABI health checks')
            if args.build_image:
                vm.command('gui --info', expect='display ABI=')
                report['checks'].append('native image boots its source-built desktop broker')
        report['state'] = 'passed'
    except BaseException as error:
        report['error'] = repr(error)
        (out / 'exception.txt').write_text(traceback.format_exc())
        if vm and vm.process.poll() is None:
            try:
                from guest_ramfs import GuestRamFS, boot_kernel
                vm.qmp('stop')
                try:
                    fs = GuestRamFS(vm, boot_kernel(args.iso), out / 'failure-export')
                    for name in ['coast-image', 'coast-kernel-build', 'gui-build']:
                        try:
                            fs.export('/tmp/' + name + '.log', out / (name + '.log'))
                        except FileNotFoundError:
                            pass
                finally:
                    vm.qmp('cont')
            except BaseException as diagnostic:
                report['diagnostic_error'] = repr(diagnostic)
        raise
    finally:
        if vm:
            vm.close()
        if server:
            server.shutdown()
            server.server_close()
        if thread:
            thread.join(5)
        report['owned_vm_stopped'] = bool(vm and vm.process.poll() is not None)
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()

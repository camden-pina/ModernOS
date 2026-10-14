#!/usr/bin/env python3
"""Exercise the bundled source rebuild inside ModernOS (long-running TCG test)."""
import argparse
import json
from pathlib import Path
import re
import shutil
import time
import uuid

from test_runtime import VM, WORK, assert_success


def exercise(cpus, memory, iso=None, timeout=43200, label='rebuild',
             checkpoint=None, resume=None, restart=False, checkpoint_every=900,
             accel=None, cpu=None):
    WORK.mkdir(parents=True, exist_ok=True)
    checkpoint = Path(checkpoint or WORK / ('native-' + label + '.state')).resolve()
    if resume:
        configuration = json.loads(Path(str(resume) + '.json').read_text())['configuration']
        iso = iso or configuration['iso']
        accel = accel or configuration.get('accel', 'tcg')
        cpu = cpu or configuration.get('cpu')
    else:
        # Long builds must retain the exact ISO even if another test rebuilds
        # build/os.iso. The checkpoint manifest checks its content on restore.
        source = Path(iso or WORK.parent / 'os.iso').resolve()
        iso = WORK / ('native-' + label + '-' + uuid.uuid4().hex[:8] + '.iso')
        shutil.copy2(source, iso)

    vm = VM(cpus, storage=False, network=False, memory=memory, iso=iso, label=label,
            resume=resume, accel=accel or 'tcg', cpu=cpu)
    saved = WORK / ('native-rebuild-progress.log' if label == 'rebuild'
                    else 'native-' + label + '-progress.log')
    started = time.monotonic()
    last_checkpoint, last_stage = started, None
    try:
        # A successful configure is not enough when asynchronous signal return
        # can corrupt its shell. Prove registers/processes before a long build.
        if not resume:
            assert_success(vm.command('build-check', 'BUILD CHECK PASS', timeout=120))
            assert_success(vm.command('compiler-check', 'COMPILER ABI CHECK PASS', timeout=120))
        elif restart:
            assert_success(vm.shell_command(
                'test -f /tmp/rebuild.status || { echo REBUILD_STILL_RUNNING; exit 1; }'))

        if not resume or restart:
            script = ('rm -f /tmp/rebuild.status; '
                      '(bootstrap-toolchain -j' + str(cpus) + ' > /tmp/rebuild.log 2>&1; '
                      'printf "%s\\n" "$?" > /tmp/rebuild.status) & echo NATIVE_REBUILD_STARTED')
            vm.shell_command(script, 'NATIVE_REBUILD_STARTED')

        with saved.open('a' if resume else 'w') as progress:
            while True:
                probe = ('if test -f /tmp/toolchain/.modernos-stage; then printf REBUILD_STAGE=; '
                         'cat /tmp/toolchain/.modernos-stage; fi; '
                         'if test -f /tmp/rebuild.status; then printf REBUILD_STATUS=; '
                         'cat /tmp/rebuild.status; else tail -n 2 /tmp/rebuild.log; fi')
                output = vm.shell_command(probe, timeout=120)
                progress.write(output);
                progress.flush()

                # Autoconf deliberately executes faulting capability probes
                # (intl checks integer division by zero). Keep diagnostics in
                # the serial log; judge the actual build status and C/C++ proofs.
                status = re.search(r'\nREBUILD_STATUS=(\d+)\n', output)
                print(f'NATIVE_REBUILD elapsed={int(time.monotonic() - started)}s\n' + output,
                      flush=True)

                assert 'Out of memory (-12)' not in output, 'Native build cannot spawn its progress probe: ' + output
                assert time.monotonic() - started < timeout, 'Native source rebuild exceeded its test deadline'

                if status:
                    tail = vm.shell_command('tail -n 80 /tmp/rebuild.log', timeout=120)
                    progress.write(tail);
                    progress.flush()
                    print(tail, flush=True)
                    print('SAVING final guest state: ' + str(checkpoint), flush=True)
                    vm.checkpoint(checkpoint)
                    last_checkpoint = time.monotonic()

                    assert status.group(1) == '0', tail
                    assert 'REBUILT_GCC_RUN_OK' in tail and 'REBUILT_GXX_RUN_OK' in tail, tail
                    vm.shell_command('/tmp/toolchain/install/bin/gcc --version',
                                     'ModernOS native rebuild')

                    print(
                        'PASS native source rebuild: binutils, GCC/G++, libgcc/libstdc++, compiled C/C++ proofs', flush=True)
                    return

                stage = re.search(r'\nREBUILD_STAGE=(\w+)\n', output)
                stage = stage.group(1) if stage else None
                if ((stage and stage != last_stage) or
                        time.monotonic() - last_checkpoint >= checkpoint_every):
                    print(f'SAVING guest checkpoint: {checkpoint} stage={stage}', flush=True)
                    vm.checkpoint(checkpoint)
                    last_checkpoint, last_stage = time.monotonic(), stage
                    print('CHECKPOINT complete', flush=True)

                # The outer Codex call yields while this test continues; it is
                # also interruptible and retains the complete guest serial log.
                time.sleep(20)
    except BaseException:
        # Keep real guest RAM, files, devices and running processes available
        # for diagnosis/resume instead of losing hours of native compilation.
        try:
            if time.monotonic() - last_checkpoint > 5:
                print('SAVING interrupted/failed rebuild: ' + str(checkpoint), flush=True)
                vm.checkpoint(checkpoint)
            print(f'Recover with --cpus {cpus} --memory {memory} --resume {checkpoint}', flush=True)
        except Exception as error:
            print('Checkpoint failed: ' + str(error), flush=True)
        raise
    finally:
        vm.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cpus', type=int, default=4)
    parser.add_argument('--memory', default='8G')
    parser.add_argument('--iso')
    parser.add_argument('--timeout', type=int, default=43200)
    parser.add_argument('--label', default='rebuild',
                        help='Separate VM/log name for isolated rebuild runs')
    parser.add_argument(
        '--checkpoint', help='File migration checkpoint (default: per-label state in runtime-tests)')
    parser.add_argument('--resume', help='Restore a previous RAM-only guest checkpoint')
    parser.add_argument('--restart', action='store_true',
                        help='After restore, rerun bootstrap-toolchain to resume failed make stages')
    parser.add_argument('--checkpoint-every', type=int, default=900,
                        help='Periodic checkpoint interval in seconds')
    parser.add_argument('--accel', choices=('tcg', 'kvm'),
                        help='QEMU accelerator (default: TCG, or the resumed checkpoint setting)')
    parser.add_argument(
        '--cpu', help='QEMU CPU model (default: accelerator default, or checkpoint setting)')
    args = parser.parse_args()

    if args.restart and not args.resume:
        parser.error('--restart requires --resume')
    if args.checkpoint_every < 1:
        parser.error('--checkpoint-every must be positive')
    exercise(args.cpus, args.memory, args.iso, args.timeout, args.label,
             args.checkpoint, args.resume, args.restart, args.checkpoint_every,
             args.accel, args.cpu)

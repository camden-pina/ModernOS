#!/usr/bin/env python3
"""Recover a real RAM-only ModernOS guest and its live processes from a file."""
from test_runtime import VM, WORK, assert_success


def main():
    WORK.mkdir(parents=True, exist_ok=True)
    checkpoint = WORK / 'checkpoint-test.state'
    vm = VM(4, storage=False, network=False, label='checkpoint-test')
    try:
        assert_success(vm.command("build-sh -c 'printf RAM_CHECKPOINT_OK > /tmp/checkpoint-proof'"))
        vm.command("build-sh -c '(sleep 10; printf CHILD_CHECKPOINT_OK > /tmp/checkpoint-child) & echo CHILD_STARTED'",
                   'CHILD_STARTED')
        vm.checkpoint(checkpoint)
        vm.command('cat /tmp/checkpoint-proof', 'RAM_CHECKPOINT_OK')
    finally:
        vm.close()

    vm = VM(4, storage=False, network=False, label='checkpoint-restored', resume=checkpoint)
    try:
        vm.command('cat /tmp/checkpoint-proof', 'RAM_CHECKPOINT_OK')
        vm.command("build-sh -c 'sleep 12; cat /tmp/checkpoint-child'",
                   'CHILD_CHECKPOINT_OK', timeout=30)
        assert_success(vm.command('build-check --suspend', 'BUILD SUSPEND CHECK PASS', timeout=120))
        print('PASS checkpoint restore: RAM filesystem, live child, timers, CPUs and signal waits', flush=True)
    finally:
        vm.close()


if __name__ == '__main__':
    main()

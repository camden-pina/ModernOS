#!/usr/bin/env python3
"""Exercise real Git object/ref/index operations inside the booted OS."""
import argparse
import re
import shlex
from test_runtime import VM, WORK


def exercise(cpus):
    WORK.mkdir(parents=True, exist_ok=True)
    vm = VM(cpus, storage=False)

    def shell(script, expected=None, status=0):
        line = 'build-sh -c ' + shlex.quote(script)
        assert len(line.encode()) < 511
        output = vm.command(line, expected, timeout=120)
        exits = re.findall(r'pid \d+ exited (\d+)', output)
        assert not exits if not status else exits and int(exits[-1]) == status, output
        assert 'terminated by signal' not in output, output
        return output

    try:
        vm.command('compiler-check', 'COMPILER ABI CHECK PASS', timeout=120)
        vm.command('git --version', 'git version 2.56.0')
        shell('git init -b main /tmp/git-test', 'Initialized empty Git repository')
        shell('cd /tmp/git-test; git config user.name "ModernOS Test"; '
              'git config user.email test@example.invalid; '
              'git config commit.gpgsign false; git config core.pager cat')
        shell('cd /tmp/git-test; printf "first revision\\n" > document.txt; '
              'git add document.txt && git commit -m first', 'first')
        shell('cd /tmp/git-test; git status --porcelain > /tmp/git-status; '
              'test ! -s /tmp/git-status')
        shell('cd /tmp/git-test; printf "second revision\\n" >> document.txt; '
              'git --no-pager diff -- document.txt', '+second revision')
        shell('cd /tmp/git-test; git add document.txt && git commit -m second', 'second')
        shell('cd /tmp/git-test; git branch feature; git checkout feature; '
              'printf "feature\\n" > feature.txt; git add feature.txt; git commit -m feature', 'feature')
        shell('cd /tmp/git-test; git checkout main; test ! -e feature.txt; '
              'git merge --ff-only feature; test -f feature.txt', 'Fast-forward')
        shell('cd /tmp/git-test; git --no-pager log --oneline', 'first')
        shell('git clone --no-hardlinks /tmp/git-test /tmp/git-clone', 'Cloning into')
        shell('cd /tmp/git-clone; git fsck --full; test "$(git rev-parse HEAD)" = '
              '"$(git -C /tmp/git-test rev-parse HEAD)"')
        shell('git -C /tmp/git-test bundle create /tmp/native.bundle --all')
        shell('git clone /tmp/native.bundle /tmp/git-bundle', 'Cloning into')
        shell('cd /tmp/git-bundle; git fsck --full; git --no-pager log --oneline', 'first')
        shell('git -C /tmp/git-test unsupported-subcommand', 'not a git command', status=1)

        print('PASS native Git: init/config/add/commit/status/diff/branch/checkout/merge/log/clone/bundle/fsck', flush=True)
    finally:
        vm.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cpus', type=int, default=4)
    args = parser.parse_args()
    exercise(args.cpus)

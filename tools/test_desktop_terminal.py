#!/usr/bin/env python3
"""Check the fresh source-built Bash terminal in an owned offline desktop VM."""
import argparse
import json
import os
from pathlib import Path
import re
import time
import traceback

from source_tree import digest
import test_desktop as desktop
import test_runtime as runtime


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, required=True)
    parser.add_argument('--output-root', type=Path, required=True)
    args = parser.parse_args()
    iso = args.iso.resolve(strict=True)
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=False)
    (out / 'HARNESS.py').write_bytes(Path(__file__).read_bytes())
    runtime.WORK = desktop.WORK = out
    report = dict(state='failed', checks=[], iso_sha256=digest(iso),
        harness_sha256=digest(Path(__file__)), network=False, extra_disks=False)
    vm = display = None

    def raw():
        while vm.selector.select(.005):
            data = os.read(vm.process.stdout.fileno(), 65536)
            if not data:
                break
            vm.pending.extend(data)
            vm.log.write(data)
            vm.log.flush()
        return Path(vm.log.name).read_text(errors='replace')

    def terminal(start):
        output, metadata = [], []
        for record in re.split(r'(?m)(?<=\n)(?=DESKTOP )', raw()[start:]):
            if record.endswith('\n'):
                record = record[:-1]
            match = re.match(r'DESKTOP TERMINAL_OUTPUT window=\d+ workspace=\d+ ([\s\S]*)', record)
            if match:
                output.append(match[1])
            else:
                metadata.append(record + '\n')
        return re.sub(r'\x1b\[[0-?]*[ -/]*[@-~]', '', ''.join(output) + '\n' + ''.join(metadata)).replace('\r', '')

    def wait(pattern, start, timeout=30):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            match = re.search(pattern, terminal(start))
            if match:
                return match
            time.sleep(.02)
        raise AssertionError('Missing ' + pattern + '\n' + terminal(start)[-2500:])

    def command(script, token):
        start = len(raw())
        vm.send(script + '\n')
        wait(r'\n' + re.escape(token) + r'\n', start)
        return terminal(start)

    def passed(name):
        report['checks'].append(name)
        print('PASS ' + name, flush=True)

    try:
        vm = runtime.VM(4, storage=False, network=False, memory='12G', accel='kvm', iso=iso,
                        label='fresh-source-terminal')
        vm.send('gui --trace --stats\n')
        vm.until(rb'GUI_READY:', 90)
        display = desktop.DesktopTest(vm)
        start = len(raw())
        display.key('t', 'meta_l')
        wait(r'TERMINAL_SHELL[^\n]*linux-bash', start)
        wait(r'TERMINAL_MODE[^\n]*raw', start)
        command("test -n \"$BASH_VERSION\" && printf '\\n%s\\n' SOURCE_BASH_READY", 'SOURCE_BASH_READY')
        passed('the fresh image selects its source-built GNU Bash terminal')
        command("printf 'SOURCE_PIPE_OK\\n' | cat", 'SOURCE_PIPE_OK')
        command("value=$(printf source-substitution); test \"$value\" = source-substitution && printf '\\n%s\\n' SOURCE_SUBSTITUTION_OK", 'SOURCE_SUBSTITUTION_OK')
        command("printf '%s\\n' source-terminal-bytes > /home/user/source-terminal.txt; printf '\\n%s\\n' SOURCE_FILE_READY", 'SOURCE_FILE_READY')
        passed('pipes, command substitution and user-owned file writes')
        command("read -r value < <(printf 'source-process-substitution\\n'); test \"$value\" = source-process-substitution && printf '\\n%s\\n' SOURCE_PROCESS_SUBSTITUTION_OK", 'SOURCE_PROCESS_SUBSTITUTION_OK')
        passed('Bash reads process substitution through native descriptor duplication')
        command("printf source-trash-bytes > /home/user/source-trash.txt; /bin/trash-put -- /home/user/source-trash.txt && test ! -e /home/user/source-trash.txt && printf '\\n%s\\n' SOURCE_TRASH_MOVED", 'SOURCE_TRASH_MOVED')
        command("test \"$(cat /home/user/.local/share/Trash/files/source-trash.txt)\" = source-trash-bytes && test -f /home/user/.local/share/Trash/info/source-trash.txt.trashinfo && printf '\\n%s\\n' SOURCE_TRASH_VERIFIED", 'SOURCE_TRASH_VERIFIED')
        passed('the matching GNU/GIO trash helper preserves file bytes and restore metadata')
        start = len(raw())
        vm.send('sleep 30\n')
        wait(r'TERMINAL_MODE[^\n]*canonical', start)
        display.key('z', 'ctrl')
        wait(r'\n[^\n]*Stopped[^\n]*\n', start)
        output = command("jobs -l; bg %+; jobs -l; printf '\\n%s\\n' SOURCE_JOB_BACKGROUND", 'SOURCE_JOB_BACKGROUND')
        assert 'Stopped' in output and 'Running' in output, output
        start = len(raw())
        vm.send('fg %+\n')
        wait(r'TERMINAL_MODE[^\n]*canonical', start)
        display.key('c', 'ctrl')
        command("printf '\\n%s\\n' SOURCE_JOB_INTERRUPTED", 'SOURCE_JOB_INTERRUPTED')
        passed('USB Ctrl-Z, jobs, bg, fg and Ctrl-C restore the real foreground shell')
        command("export SOURCE_OUTER_PID=$$; printf '\\n%s\\n' SOURCE_OUTER", 'SOURCE_OUTER')
        start = len(raw())
        vm.send('/usr/bin/linux-bash --noprofile --norc -i\n')
        wait(r'linux-bash-5[.]3[#$] ', start)
        command("test \"$$\" != \"$SOURCE_OUTER_PID\" && printf '\\n%s\\n' SOURCE_INNER", 'SOURCE_INNER')
        start = len(raw())
        vm.send('exit\n')
        wait(r'linux-bash-5[.]3[#$] ', start)
        command("test \"$$\" = \"$SOURCE_OUTER_PID\" && printf '\\n%s\\n' SOURCE_OUTER_RESTORED", 'SOURCE_OUTER_RESTORED')
        passed('nested interactive Bash returns ownership to its parent')
        display.screenshot('terminal')
        display.exit()
        assert 'source-terminal-bytes' in vm.command('cat /home/user/source-terminal.txt')
        vm.command('check', expect='USERLAND CHECK PASS', timeout=120)
        passed('desktop shutdown preserves exact file bytes and returns to a healthy console')
        assert digest(iso) == report['iso_sha256'], 'Image changed during qualification'
        report['image_unchanged'] = True
        report['state'] = 'passed'
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

#!/usr/bin/env python3
"""Exercise the full Code OSS desktop entry in an owned, diskless QEMU guest.

Requires an image built with build_codeoss_iso.py --diagnostics. QA observation
is enabled only in this test guest; input still uses the real USB keyboard.
"""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import time
import traceback

import test_desktop as desktop
import test_runtime as runtime


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, required=True)
    parser.add_argument('--output-root', type=Path, required=True)
    parser.add_argument('--memory', default='16G')
    parser.add_argument('--timeout-scale', type=float, default=1,
                        help='scale UI observation deadlines on a busy build host (1..4)')
    parser.add_argument('--usability-check', action='store_true', help='exercise personal file ownership, New File/Save As, normal folder access and redraw correctness')
    parser.add_argument('--accel', choices=['kvm', 'tcg'], default='kvm')
    args = parser.parse_args()
    if not 1 <= args.timeout_scale <= 4:
        parser.error('--timeout-scale must be between 1 and 4')
    iso, out = args.iso.resolve(), args.output_root.resolve()
    manifest = json.loads((iso.parent / 'MANIFEST.json').read_text())
    if not manifest.get('diagnostics_qa_only'):
        parser.error('The selected image must include opt-in QA diagnostics')
    out.mkdir(parents=True, exist_ok=False)
    runtime.WORK = desktop.WORK = out
    vm = display = None
    report = dict(state='failed', iso=str(iso), network=False, extra_disks=False,
                  checks=[], harness_sha256=sha(Path(__file__)), timeout_scale=args.timeout_scale)
    (out / 'HARNESS.py').write_bytes(Path(__file__).read_bytes())

    def raw():
        while vm.selector.select(.005):
            data = os.read(vm.process.stdout.fileno(), 65536)
            if not data:
                break
            vm.pending.extend(data)
            vm.log.write(data)
            vm.log.flush()
        return Path(vm.log.name).read_text(errors='replace')

    def observed(start=0):
        # PTY output is drained in bounded chunks. Rejoin those bytes before
        # matching receipts that can cross a native terminal trace boundary.
        source=raw()[start:]
        kernel_pattern=r'\x1b\[0;32m\[\s*\d+[.]\d+\][^\n]*\n'
        kernel=re.findall(kernel_pattern,source)
        source=re.sub(kernel_pattern,'',source)
        output=[];meta=[]
        for record in re.split(r'(?m)(?<=\n)(?=DESKTOP )',source):
            if record.endswith('\n'):record=record[:-1]
            match=re.match(r'DESKTOP TERMINAL_OUTPUT window=\d+ workspace=\d+ ([\s\S]*)',record)
            if match:output.append(match[1])
            else:meta.append(record+'\n')
        return re.sub(r'\x1b\[[0-?]*[ -/]*[@-~]','',(''.join(output)+'\n'+''.join(meta)+'\n'+''.join(kernel)).replace('\r',''))

    def wait(pattern, start=0, timeout=60):
        deadline = time.monotonic() + timeout * args.timeout_scale
        while True:
            evidence = observed(start)
            assert 'PANIC:' not in evidence, evidence[-4000:]
            assert 'C1_RUNTIME_OUTPUT_TRUNCATED' not in evidence
            match = re.search(pattern, evidence)
            if match:
                return match
            if time.monotonic() >= deadline:
                raise AssertionError('Missing ' + pattern + '\n' + evidence[-4000:])
            time.sleep(.02)

    def passed(description):
        report['checks'].append(description)
        print('PASS ' + description, flush=True)

    def type_text(text):
        keys = {' ': 'spc', '/': 'slash', '.': 'dot', '-': 'minus'}
        for char in text:
            raw()  # Keep the observed guest's serial pipe drained between keys.
            began=time.monotonic()
            if char.isupper():
                display.timed_key(char.lower(), 'shift')
            else:
                display.timed_key(keys.get(char, char))
            with (out/'HOST-KEY-TIMING.jsonl').open('a') as timing:
                timing.write(json.dumps(dict(char=char,elapsed_ms=(time.monotonic()-began)*1000))+'\n')
            raw()

    def launch(action):
        start = len(raw())
        display.key('spc', 'meta_l')
        vm.send('Code OSS')
        # Wait for the launcher's own input loop, then select its first match.
        time.sleep(.2)
        display.key('ret')
        match = wait(r'DESKTOP ' + action + r' window=(\d+)[^\n]* Code OSS\n', start)
        return int(match[1]), start

    def open_fixture():
        start = len(raw())
        display.key('p', 'ctrl')
        type_text('/home/port-test/project/hello.txt')
        wait(r'CODEOSS_DOM[^\n]*"quickInputVisible":true', start)
        # Wait for the actual quick-open provider to resolve this file.
        wait(r'CODEOSS_DOM[^\n]*"label":"hello.txt"', start)
        display.key('ret')
        return start

    def observe(marker, predicate, start=0, timeout=45):
        deadline=time.monotonic()+timeout
        while time.monotonic()<deadline:
            for line in reversed(observed(start).splitlines()):
                found=re.search(marker+r' (\{[^\n]*\})',line)
                if found:
                    try: value=json.loads(found[1])
                    except ValueError: continue
                    if predicate(value): return value
            time.sleep(.02)
        raise AssertionError('Observation predicate missing: '+marker+'\n'+raw()[-2000:])

    def personal_save(path, text):
        start=len(raw());display.key('n','ctrl')
        observe(r'CODEOSS_DOM \d+',lambda m:m.get('editors',0)>0 and 'Untitled' in m['body'] and not m['quickInputVisible'],start)
        type_text(text)
        observe(r'CODEOSS_DOM \d+',lambda m:text in m.get('editorLines',[]),start)
        start=len(raw())
        display.key('s','ctrl','shift')
        # The input receives focus before the asynchronous initial directory
        # lookup sets its value. Wait for that lookup's rows before selecting
        # the path, otherwise its completion can replace the selection mid-type.
        observe(r'CODEOSS_DOM \d+',lambda m:m['quickInputVisible'] and m.get('quickPickRows')
                and m['activeElement'].get('label')=='Folder path - Save As',start)
        display.key('a','ctrl');type_text(path)
        parent=path.rsplit('/',1)[0]
        def resolved_picker(m):
            if path not in m['inputs'] or not m['quickInputVisible']:return False
            rows=[row['text'].strip() for row in m.get('quickPickRows',[])]
            # Input text can update before the asynchronous provider switches
            # directories. Enter must not accept a stale row from / or the
            # previous folder while testing Save As in another directory.
            if parent=='/home/user/Projects':return 'native-created' in rows
            if parent=='/home/user/Documents':return rows==['..']
            if parent=='/tmp':return 'codeoss-root-owned' in rows
            return False
        observe(r'CODEOSS_DOM \d+',resolved_picker,start)
        display.key('ret')
        files=observe('CODEOSS_QA_PERSONAL_FILES',lambda files:files.get(path,{}).get('text')==text,start)
        assert files[path]['uid']==1000 and files[path]['gid']==1000,files[path]

    def full_redraw_matches(name):
        from PIL import Image, ImageChops
        time.sleep(1)
        display.screenshot(name+'-partial')
        start=len(raw());display.key('f12','ctrl','alt')
        wait('DESKTOP GLASS_RECOMPOSE',start)
        receipt=wait(r'DESKTOP RECOMPOSE_CHECK error=0 pixels=\d+ mismatches=(\d+) first=\d+,\d+ same_retained_frame=1',start)
        assert receipt[1]=='0','Same-frame incremental recomposition mismatch: '+receipt[0]
        time.sleep(.3);display.screenshot(name+'-full')
        with Image.open(out/(name+'-partial.png')) as before, Image.open(out/(name+'-full.png')) as after:
            # Exclude the top clock, which is allowed to tick independently.
            area=(0,60,display.width,display.height)
            difference=ImageChops.difference(before.crop(area),after.crop(area))
            difference.save(out/(name+'-difference.png'))
            # The guest compares both compositions before another app tick.
            # These separate screenshots may include legitimate client
            # animation frames (for example a tab's progress/shadow strip).
            report.setdefault('redraw_checks',[]).append(dict(name=name,receipt=receipt[0],
                later_screenshot_difference=difference.getbbox()))

    def close_editor(exit_desktop=False, cli=False):
        start = len(raw())
        if exit_desktop:
            display.key('spc', 'meta_l')
            vm.send('Return to console')
            time.sleep(.2)
            display.key('ret')
        else:
            display.key('f4', 'alt')
        wait(r'C1_RUNTIME_WAIT status=0 exit=0 signal=0 timeout=0', start)
        wait(r'C1_RUNTIME_DESCENDANTS_REAPED helpers=\d+ complete=1', start)
        wait(r'P6_SURFACE_EXIT[^\n]*child_status=0[^\n]*fatal=0[^\n]*close_fallback=0', start)
        if not cli:wait(r'DESKTOP GAME_EXIT pid=\d+ status=0 reaped=1', start)

    with Path(str(iso) + '.lock').open('a') as image_lock:
        fcntl.flock(image_lock, fcntl.LOCK_SH | fcntl.LOCK_NB)
        report['iso_sha256'] = sha(iso)
        assert report['iso_sha256'] == manifest['iso_sha256']
        try:
            vm = runtime.VM(2, storage=False, network=False, memory=args.memory,
                            accel=args.accel, iso=iso, label='codeoss-desktop')
            report['qemu_pid'] = vm.process.pid
            if args.usability_check:
                vm.shell_command("printf 'root preserved' > /tmp/codeoss-root-owned")
                vm.shell_command('/bin/busybox chmod 644 /tmp/codeoss-root-owned')
                vm.command('mkdir -p /tmp/codeoss-root-private')
                vm.shell_command('/bin/busybox chmod 700 /tmp/codeoss-root-private')
            vm.command('mkdir -p /home/user/.config')
            vm.shell_command("printf 'scale=3\\nterminal_shell=1\\n' > /home/user/.config/desktop.ini")
            vm.send("build-sh -c 'COAST_CODEOSS_QA=1 "+('COAST_USER_FILES_CHECK=1 ' if args.usability_check else '')+"exec /bin/gui --trace --stats'\n")
            vm.until(rb'GUI_READY:', 60)
            display = desktop.DesktopTest(vm)
            display.screenshot('00-desktop')
            assert (display.width, display.height) == (1920, 1080)

            if args.usability_check:
                display.key('n','ctrl','shift');vm.send('native-created\n')
                wait(r'DESKTOP CREATE_FOLDER[^\n]*/home/user/Projects/native-created')
                display.key('n','ctrl');vm.send('from-native.txt\n')
                wait(r'DESKTOP CREATE_FILE[^\n]*/home/user/Projects/from-native.txt')
                passed('native Files creates a real folder and file in Projects before the editor starts')

            cancelled, cancel_start = launch('OPEN')
            assert not re.search(r'DESKTOP GAME_VIEW window=' + str(cancelled) + r'\b', raw()[cancel_start:])
            close_editor()
            wait(r'DESKTOP NATIVE_START_CANCELLED client=\d+ id=\d+', cancel_start)
            assert len(re.findall(r'DESKTOP OPEN[^\n]* Code OSS\n', raw()[cancel_start:])) == 1
            passed('closing the startup window cancels its later surface without reopening the editor or leaking helpers')

            ident, start = launch('OPEN')
            again, _ = launch('ACTIVATE')
            assert again == ident
            wait(r'ELECTRON_DISPLAY_FD_VERIFIED fd=3 unnamed=1', start)
            wait(r'G2_LAUNCH_CONFIG actual_nnp=1 actual_filter=2', start)
            wait(r'C1_RUNTIME_CHILD uid=1000 euid=1000 gid=1000 egid=1000', start)
            wait(r'CODEOSS_EFFECTIVE_SANDBOX[^\n]*"noSandbox":false[^\n]*"disableGpuSandbox":false', start)
            wait(r'CODEOSS_DOM[^\n]*"ready":"complete"[^\n]*"fonts":"loaded"', start, 90)
            wait(r'ELECTRON_CLIENT_PIXELS_PRESENTED', start)
            wait(r'DESKTOP GAME_VIEW window=' + str(ident) + r'[^\n]*native=1', start)
            assert len(re.findall(r'DESKTOP GAME_PROCESS[^\n]*/bin/codeoss-wayland --codeoss', raw()[start:])) == 1
            assert not re.search(r'DESKTOP OPEN[^\n]*(?:Terminal|Core Preview)', raw())
            passed('launcher starts the full editor without a terminal; repeated activation reuses its window')
            passed('real editor pixels and loaded fonts retain UID 1000, NNP, seccomp and Chromium sandbox')
            display.screenshot('01-codeoss-from-desktop')

            if args.usability_check:
                permissions=observe('CODEOSS_USER_FILES_PASS',lambda m:m['nativeFilesEditable'] and m['systemWriteDenied'],start)
                report['permissions']=permissions
                passed('UID 1000 creates, renames and removes personal and temporary files; native files are editable and system permissions are enforced')
                controls=observe(r'CODEOSS_DOM \d+',lambda m:'windowControls' in m,start)
                assert controls['windowControls']==[],controls['windowControls']
                passed('the full editor has no visible redundant inner minimize, maximize or close controls')
                for path,text in [('/home/user/Projects/editor-created.txt','Projects file created in Code OSS'),('/home/user/Documents/editor-created.txt','Documents file created in Code OSS'),('/tmp/codeoss-created.txt','Temporary file created in Code OSS')]:
                    personal_save(path,text)
                passed('real New File and Save As create exact user-owned files in Projects, Documents and tmp')
                full_redraw_matches('03-editor-redraw')
                passed('incremental editor pixels match a forced full desktop recomposition exactly')

            start = open_fixture()
            wait(r'CODEOSS_DOM[^\n]*Coast original', start)
            display.key('a', 'ctrl')
            text = 'Coast desktop launch saved'
            type_text(text)
            start = len(raw())
            display.key('s', 'ctrl')
            wait(r'CODEOSS_QA_FILE ' + re.escape(json.dumps(text)), start)
            passed('USB keyboard opens a project file and saves exact edited bytes')

            start = len(raw())
            display.key('f1')
            wait(r'CODEOSS_DOM[^\n]*"quickInputVisible":true', start)
            display.key('esc')
            passed('F1 reaches the editor command palette')

            start = len(raw())
            display.key('down', 'meta_l')
            wait(r'DESKTOP MINIMIZE window=' + str(ident), start)
            # APP_WORKBENCH is appended as role 9, preserving saved pin masks.
            items = re.findall(r'DESKTOP DOCK_ITEM role=9 x=(\d+) y=(\d+) width=(\d+) height=(\d+)[^\n]*running=1', raw())
            assert items, 'The editor must have its own running Dock entry'
            x, y, width, height = map(int, items[-1])
            start = len(raw())
            display.click(x + width // 2, y + height // 2)
            wait(r'DESKTOP FOCUS window=' + str(ident) + r'[^\n]*Code OSS', start)
            wait(r'CODEOSS_DOM[^\n]*"focused":true', start)
            passed('Code OSS has its own running Dock icon, which restores its minimized window')
            start = len(raw())
            display.key('down', 'meta_l')
            wait(r'DESKTOP MINIMIZE window=' + str(ident), start)
            again, start = launch('ACTIVATE')
            assert again == ident
            wait(r'CODEOSS_DOM[^\n]*"focused":true', start)
            passed('desktop entry restores the minimized editor with focus and the same window identity')
            display.screenshot('02-restored-editor')

            close_editor()
            passed('desktop-owned launcher and all Electron descendants exit cleanly without forced cleanup')
            ident2, start = launch('OPEN')
            assert ident2 != ident
            wait(r'CODEOSS_DOM[^\n]*"ready":"complete"[^\n]*"fonts":"loaded"', start, 90)
            # Verify session restoration itself. Reopening via Quick Open
            # would conceal a failed restore and can race its startup provider.
            observe(r'CODEOSS_DOM \d+',lambda m:not m['quickInputVisible'] and
                    m.get('lineTexts')==[text] and 'hello.txt' in m['body'],start,90)
            passed('editor relaunch reclaims its profile and reopens the saved file')
            close_editor(exit_desktop=True)
            vm.until(rb'gui: returned to terminal\.[\s\S]*os:[^\n]*\$ $', 30)
            passed('returning to console closes the active editor and drains its launcher through desktop shutdown')
            report['saved_file'] = vm.command('cat /home/port-test/project/hello.txt')
            assert text in report['saved_file']
            if args.usability_check:
                vm.send('gui --trace --stats\n');vm.until(rb'GUI_READY:',60)
                display=desktop.DesktopTest(vm);display.screenshot('04-folder-access')
                display.key('t','meta_l');wait(r'TERMINAL_MODE[^\n]*raw')
                for folder in ['/tmp','/usr']:
                    start=len(raw());vm.send('COAST_CODEOSS_QA=1 code '+folder+'\n')
                    wait(r'CODEOSS_PROJECT path='+re.escape(folder)+r' uid=1000',start)
                    wait(r'CODEOSS_DOM[^\n]*"ready":"complete"[^\n]*"fonts":"loaded"',start,90)
                    wait('ELECTRON_CLIENT_PIXELS_PRESENTED',start)
                    close_editor(cli=True)
                    wait('linux-bash-5[.]3[#$] ',start)
                passed('code opens tmp and usr outside the former project restriction under UID 1000 and closes cleanly')
                display.exit()
                report['personal_files']=vm.command('cat /home/user/Projects/editor-created.txt')
                assert 'Projects file created in Code OSS' in report['personal_files']
            report['processes'] = vm.command('ps')
            for executable in ['/opt/electron', 'codeoss-wayland', 'g2-wayland-launch']:
                assert executable not in report['processes'], report['processes']
            report['health'] = vm.command('check')
            assert 'USERLAND CHECK PASS' in report['health']
            faults=re.findall(r'ELECTRON_USER_FAULT[^\n]*',raw())
            assert all(re.search(r'pending_signal=(?:9|15)\b',fault) for fault in faults),'An Electron helper faulted without pending termination'
            report['terminating_fault_receipts']=faults
            passed('saved bytes verified outside editor; guest health passes and no editor tasks remain')
            report['state'] = 'passed Code OSS desktop launch, edit, restore, relaunch and cleanup'
        except BaseException as error:
            report['error'] = repr(error)
            (out / 'exception.txt').write_text(traceback.format_exc())
            if display:
                try:
                    display.screenshot('failure')
                except Exception as screenshot_error:
                    report['screenshot_error'] = repr(screenshot_error)
            raise
        finally:
            if vm:
                raw()
                vm.close()
                report['qemu_exit'] = vm.process.returncode
                report['owned_vm_stopped'] = vm.process.poll() is not None
            report['image_unchanged'] = sha(iso) == report['iso_sha256']
            (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()

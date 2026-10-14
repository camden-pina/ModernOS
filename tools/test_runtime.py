#!/usr/bin/env python3
"""Boot integration tests using generated disk images, never host block devices.

Requires QEMU, mkfs.fat, mkfs.ext4, mkfs.ext2, mtools, e2fsprogs, and xorriso.
Logs and disposable fixtures live under build/runtime-tests/.
"""
import argparse
import hashlib
import json
import os
import pathlib
import re
import select
import selectors
import shlex
import shutil
import struct
import subprocess
import time
import uuid
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
WORK = ROOT / "build" / "runtime-tests"
SECTOR = 512
ANSI = re.compile(rb"\x1b\[[0-9;=?]*[A-Za-z]")


def default_iso():
    return pathlib.Path(os.environ.get('MODERNOS_ISO') or ROOT / 'build/os.iso').resolve()


def normalize_checkpoint_firmware(configuration):
    hashes = configuration.get('image_hashes', {})
    legacy = str(ROOT / 'RELEASEX86_64_OVMF.fd')
    current = str(ROOT / 'firmware/ovmf/OVMF.fd')
    if legacy in hashes and current not in hashes:
        hashes[current] = hashes.pop(legacy)
    return configuration


def run(*args):
    command = [str(a) for a in args]

    # Host tools must not ask hidden questions on the invoking terminal.
    # mcopy's -o only suppresses DOS destination prompts; Unix files need -D o.
    try:
        result = subprocess.run(command, stdin=subprocess.DEVNULL,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                start_new_session=True, timeout=60, check=True)
    except subprocess.TimeoutExpired as error:
        output = (error.stdout or b"").decode(errors="replace")
        raise RuntimeError(
            f"Host tool timed out after 60 seconds: {shlex.join(command)}\n{output}") from error
    except subprocess.CalledProcessError as error:
        output = (error.stdout or b"").decode(errors="replace")
        raise RuntimeError(
            f"Host tool exited {error.returncode}: {shlex.join(command)}\n{output}") from error
    return result.stdout


def check_run_recipe():
    # Exercise Make's actual recipe as well as QEMU itself. Commas in QEMU
    # options must survive Make expansion, including with no disks attached.
    names = ("LINUX_DISK", "FAT_DISK", "SATA_DISK")
    cases = ({}, *({name: f"/tmp/{name.lower()} image.img"} for name in names),
             {name: f"/tmp/{name.lower()} image.img" for name in names})
    for case in cases:
        variables = [f"{name}={case.get(name, '')}" for name in names]
        output = run("make", "-n", "run", "QEMU_EXTRA_ARGS=", *variables).decode()
        lines = output.replace("\\\n", " ").splitlines()
        command = next(line for line in lines if line.startswith(
            ("qemu-system-x86_64 ", "env ")) and "qemu-system-x86_64 " in line)
        arguments = shlex.split(command)
        assert arguments.count("-drive") == len(case), command
        for path in case.values():
            assert any(arg.startswith(f"file={path},format=raw,") for arg in arguments), command

    print("PASS make run recipe: optional disk arguments and paths with spaces", flush=True)


def gpt_image(volume, output, type_guid):
    size = volume.stat().st_size
    assert size % SECTOR == 0
    first, count = 2048, size // SECTOR
    sectors = first + count + 2048
    table = bytearray(128 * 128)
    table[:16] = uuid.UUID(type_guid).bytes_le
    table[16:32] = uuid.uuid5(uuid.NAMESPACE_URL, output.name).bytes_le
    struct.pack_into("<QQQ", table, 32, first, first + count - 1, 0)
    name = output.stem.encode("utf-16le")[:72]
    table[56:56 + len(name)] = name
    disk_guid = uuid.uuid5(uuid.NAMESPACE_DNS, output.name).bytes_le

    def header(current, backup, entries):
        h = bytearray(SECTOR)
        struct.pack_into("<8sIIIIQQQQ16sQIII", h, 0, b"EFI PART", 0x10000, 92, 0, 0,
                         current, backup, 34, sectors - 34, disk_guid, entries, 128, 128,
                         zlib.crc32(table))
        struct.pack_into("<I", h, 16, zlib.crc32(h[:92]))
        return h

    with output.open("wb") as disk:
        disk.truncate(sectors * SECTOR)
        mbr = bytearray(SECTOR)
        mbr[446 + 4] = 0xEE
        struct.pack_into("<II", mbr, 446 + 8, 1, min(sectors - 1, 0xFFFFFFFF))
        mbr[510:] = b"\x55\xaa"
        disk.write(mbr)
        disk.write(header(1, sectors - 1, 2))
        disk.write(table)
        disk.seek(first * SECTOR)
        with volume.open("rb") as source:
            shutil.copyfileobj(source, disk, 1024 * 1024)
        disk.seek((sectors - 33) * SECTOR)
        disk.write(table)
        disk.write(header(sectors - 1, 1, sectors - 33))
    return first * SECTOR, size


def prepare():
    WORK.mkdir(parents=True, exist_ok=True)
    tree = WORK / "source"
    tree.mkdir(exist_ok=True)
    for name in ("hello", "check"):
        shutil.copy2(ROOT / "userland" / "bin" / name, tree / name)
    (tree / "note.txt").write_text("SOURCE_EXT4_READ_OK\n")
    payload = bytes((i * 31 + 7) % 256 for i in range(180000))
    (tree / "long source filename.bin").write_bytes(payload)
    sparse = tree / "sparse.bin"
    with sparse.open("wb") as f:
        f.write(b"START")
        f.seek(300000)
        f.write(b"END")

    link = tree / "hello-link"
    if not link.exists():
        link.symlink_to("hello")

    for fs, mib in (("ext4", 64), ("ext2", 64), ("fat", 96)):
        volume = WORK / (fs + ".volume")
        with volume.open("wb") as f:
            f.truncate(mib * 1024 * 1024)

        if fs == "fat":
            run("mkfs.fat", "-F", "32", "-n", "MODERNOS", volume)
        else:
            run("mkfs." + fs, "-q", "-F", "-b", "4096", "-d", tree,
                "-E", "lazy_itable_init=0", volume)

    gpt_image(WORK / "ext4.volume", WORK / "linux.img", "0fc63daf-8483-4772-8e79-3d69d8477de4")
    gpt_image(WORK / "fat.volume", WORK / "fat.img", "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7")
    return payload


class VM:
    def __init__(self, cpus, bios=False, storage=True, iso=None, buffered=True, memory="2G", label=None, network=True, resume=None, cpu=None, accel="tcg", display="none", trace_display=False, make_run=False, trace_pointer=False, audio_capture=None, audio_device="ac97", network_model="e1000e"):
        iso = pathlib.Path(iso).resolve() if iso is not None else default_iso()
        cpu = cpu or ('host' if accel == 'kvm' else 'max')
        WORK.mkdir(parents=True, exist_ok=True)
        self.configuration = dict(cpus=cpus, bios=bios, storage=storage, buffered=buffered,
                                  memory=memory, network=network,
                                  iso=str(iso))
        if network_model != 'e1000e':
            self.configuration['network_model'] = network_model
        if audio_capture:
            self.configuration['audio_capture'] = str(pathlib.Path(audio_capture).resolve())
            self.configuration['audio_device'] = audio_device
        if cpu != 'max':
            self.configuration['cpu'] = cpu
        if accel != 'tcg':
            self.configuration['accel'] = accel

        if resume:
            resume = pathlib.Path(resume).resolve()
            manifest = json.loads(pathlib.Path(str(resume) + '.json').read_text())
            if normalize_checkpoint_firmware(manifest['configuration']) != self.checkpoint_configuration():
                raise AssertionError(
                    'Checkpoint machine, QEMU, firmware or ISO differs from this VM')
            if resume.stat().st_size != manifest['bytes']:
                raise AssertionError('Checkpoint is incomplete')

        self.name = f"{'bios' if bios else 'uefi'}-{cpus}cpu"
        if label:
            assert re.fullmatch(r'[a-zA-Z0-9_-]+', label), label
            self.name += '-' + label
        self.qmp_path = WORK / (self.name + ".qmp")
        self.qmp_pipes = [pathlib.Path(str(self.qmp_path) + suffix) for suffix in (".in", ".out")]
        for path in self.qmp_pipes:
            path.unlink(missing_ok=True)
            os.mkfifo(path, 0o600)
        self.qmp_input, self.qmp_output = [
            os.open(path, os.O_RDWR | os.O_NONBLOCK) for path in self.qmp_pipes]
        self.qmp_pending = bytearray()
        self.qmp_initialized = False
        cmd = ["qemu-system-x86_64", "-accel", accel, "-cpu", cpu, "-M", "q35", "-smp", str(cpus),
               "-m", memory, "-cdrom", str(iso),
               "-display", display, "-monitor", "none", "-serial", "stdio", "-no-reboot",
               "-qmp", f"pipe:{self.qmp_path}"]
        cmd += ["-vga", "none", "-device",
                "VGA,vgamem_mb=32,refresh_rate=120" + ("" if buffered else ",mmio=off")]

        assert network_model in ('e1000e', 'e1000', 'igb'), network_model
        cmd += ["-nic", "user,model=" + network_model if network else "none"]

        if audio_capture:
            assert audio_device in ('ac97', 'hda')
            cmd += ['-audiodev', f'wav,id=audio0,path={pathlib.Path(audio_capture).resolve()}']
            if audio_device == 'hda':
                cmd += ['-device', 'intel-hda', '-device', 'hda-duplex,audiodev=audio0']
            else:
                cmd += ['-device', 'AC97,audiodev=audio0']

        if not bios:
            cmd += ["-bios", str(ROOT / "firmware/ovmf/OVMF.fd")]
        cmd += ["-device", "qemu-xhci,id=xhci", "-device", "usb-kbd,bus=xhci.0",
                "-device", "usb-mouse,bus=xhci.0"]

        if storage:
            cmd += ["-drive", f"file={WORK / 'linux.img'},format=raw,if=none,id=source",
                    "-device", "nvme,drive=source,serial=TEST-SOURCE",
                    "-drive", f"file={WORK / 'fat.img'},format=raw,if=none,id=destination",
                    "-device", "nvme,drive=destination,serial=TEST-DESTINATION",
                    "-drive", f"file={WORK / 'ext2.volume'},format=raw,if=none,id=sata",
                    "-device", "ide-hd,drive=sata,bus=ide.0"]

        if resume:
            cmd += ['-incoming', 'defer']

        if make_run:
            assert not bios and not storage and not resume and buffered
            assert not audio_capture
            output = run('make', '-n', 'run', f'SMP={cpus}', f'MEMORY={memory}',
                         f'ISO_IMAGE={self.configuration["iso"]}',
                         f'QEMU_ACCEL={accel}', f'QEMU_CPU={cpu}',
                         f'QEMU_DISPLAY={display}', 'QEMU_EXTRA_ARGS=',
                         'LINUX_DISK=', 'FAT_DISK=', 'SATA_DISK=').decode()
            lines = output.replace('\\\n', ' ').splitlines()
            recipe = next(line for line in lines
                          if line.startswith(('qemu-system-x86_64 ', 'env '))
                          and 'qemu-system-x86_64 ' in line)
            cmd = shlex.split(recipe)

            # Keep the launcher's stdio chardev and default monitor, while
            # giving this disposable VM its own serial log and QMP pipes.
            serial = cmd.index('-chardev') + 1
            cmd[serial] = re.sub(r'logfile=[^,]+',
                                 f'logfile={WORK / (self.name + ".serial.log")}',
                                 cmd[serial])
            cmd += ['-qmp', f'pipe:{self.qmp_path}']

        if trace_display or trace_pointer:
            events = WORK / (self.name + '.display-events')
            trace_events = ''
            if trace_display:
                trace_events += ('console_refresh\ndisplaysurface_create_from\ngd_switch\n'
                                 'gd_motion_event\ngd_grab\ngd_keymap_windowing\n')
            if trace_pointer:
                trace_events += 'input_event_rel\ninput_event_btn\ninput_event_abs\n'
            events.write_text(trace_events)
            cmd += ['-msg', 'timestamp=on', '-trace',
                    f'events={events},file={WORK / (self.name + ".display-trace")}']

        self.process = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, bufsize=0)
        self.log = (WORK / (self.name + ".log")).open("wb")
        self.pending = bytearray()
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.process.stdout, selectors.EVENT_READ)

        try:
            if resume:
                self.qmp('migrate-incoming', {'channels': self.migration_file(resume)})
                self.wait_migration(180)
                if not self.qmp('query-status')['running']:
                    self.qmp('cont')

                # The saved shell is already at its prompt; request a new one.
                self.send('\n')
            self.until_prompt(40)
        except BaseException:
            self.close()
            raise

    def until_prompt(self, timeout=30):
        return self.until(rb"os:[^\n]*\$ $", timeout)

    def until(self, pattern, timeout=30):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            plain = ANSI.sub(b"", bytes(self.pending)).replace(b"\r", b"")
            if b"PANIC:" in plain:
                raise AssertionError(plain.decode(errors="replace"))
            if re.search(pattern, plain):
                self.pending.clear()
                return plain.decode(errors="replace")

            if self.process.poll() is not None:
                raise AssertionError(f"{self.name}: QEMU exited\n{plain.decode(errors='replace')}")

            if self.selector.select(0.2):
                data = os.read(self.process.stdout.fileno(), 65536)
                self.pending.extend(data)
                self.log.write(data)
                self.log.flush()
        raise AssertionError(f"{self.name}: missing {pattern!r}\n{plain.decode(errors='replace')}")

    def send(self, text):
        self.process.stdin.write(text.encode())
        self.process.stdin.flush()

    def command(self, line, expect=None, timeout=30):
        self.send(line + "\n")
        output = self.until_prompt(timeout)
        if expect is not None and expect not in output:
            raise AssertionError(f"{self.name}: {line!r} expected {expect!r}\n{output}")
        return output

    def shell_command(self, script, expect=None, timeout=30):
        """Wait for this guest shell command, even after a saved old prompt.

        The markers are formatted by the actual guest shell. Their complete
        bytes never occur in the echoed command, and a subshell preserves the
        result even when the supplied script exits early.
        """
        token = 'p' + uuid.uuid4().hex[:8]
        wrapped = ("printf '\\n%sB\\n' " + token + '; (' + script +
                   "); _probe_rc=$?; printf '\\n%sE%d\\n' " + token + ' "$_probe_rc"')
        line = 'build-sh -c ' + shlex.quote(wrapped)
        if len(line.encode()) >= 511 or '\n' in line or '\r' in line:
            raise ValueError('Guest shell probe exceeds the console input limit')
        pattern = (rb'\n' + token.encode() + rb'B\n([\s\S]*)\n' +
                   token.encode() + rb'E([0-9]+)\n[\s\S]*os:[^\n]*\$ $')
        self.send(line + '\n')
        output = self.until(pattern, timeout)
        match = re.search(pattern, output.encode())
        result = '\n' + match[1].decode() + '\n'
        if match[2] != b'0':
            raise AssertionError(
                f'{self.name}: guest command exited {match[2].decode()}: {script!r}\n{result}')
        if expect is not None and expect not in result:
            raise AssertionError(f'{self.name}: {script!r} expected {expect!r}\n{result}')
        return result

    def qmp(self, command, arguments=None):
        # Local FIFOs work in sandboxes that prohibit even Unix socket binds.
        def receive():
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                if b"\n" in self.qmp_pending:
                    line, _, rest = self.qmp_pending.partition(b"\n")
                    self.qmp_pending = bytearray(rest)
                    return json.loads(line)
                if select.select([self.qmp_output], [], [], 0.2)[0]:
                    self.qmp_pending.extend(os.read(self.qmp_output, 65536))
            raise AssertionError("QEMU monitor response timed out")

        requests = []
        if not self.qmp_initialized:
            assert "QMP" in receive()
            requests.append({"execute": "qmp_capabilities"})
        requests.append({"execute": command, "arguments": arguments or {}})

        for request in requests:
            os.write(self.qmp_input, json.dumps(request).encode() + b"\n")
            while True:
                response = receive()
                if "error" in response:
                    raise AssertionError(response)
                if "return" in response:
                    break

        self.qmp_initialized = True
        return response["return"]

    def checkpoint_configuration(self):
        if self.configuration['storage']:
            raise AssertionError(
                'Checkpoints support RAM-only test VMs; disk fixtures are not snapshotted')

        result = dict(self.configuration)
        paths = [pathlib.Path(result['iso'])]
        if not result['bios']:
            paths.append(ROOT / 'firmware/ovmf/OVMF.fd')
        result['image_hashes'] = {}
        for path in paths:
            with path.open('rb') as file:
                result['image_hashes'][str(path)] = hashlib.file_digest(file, 'sha256').hexdigest()
        result['qemu'] = subprocess.check_output(
            ['qemu-system-x86_64', '--version'], text=True).splitlines()[0]
        return result

    @staticmethod

    def migration_file(path):
        # QMP structured arguments avoid shell commands or network listeners.
        # https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html#migrate
        return [{'channel-type': 'main',
                 'addr': {'transport': 'file', 'filename': str(path), 'offset': 0}}]

    def wait_migration(self, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            migration = self.qmp('query-migrate')
            if migration.get('status') == 'completed':
                return
            if migration.get('status') in ('failed', 'cancelled'):
                raise AssertionError(migration)
            time.sleep(.1)
        raise AssertionError('QEMU file migration exceeded its deadline')

    def checkpoint(self, path, timeout=180):
        configuration = self.checkpoint_configuration()
        path = pathlib.Path(path).resolve()
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = pathlib.Path(str(path) + '.pending')
        manifest_path = pathlib.Path(str(path) + '.json')
        manifest_pending = pathlib.Path(str(manifest_path) + '.pending')
        was_running = self.qmp('query-status')['running']
        self.qmp('stop')
        complete = False
        try:
            self.qmp('migrate-set-parameters', {'max-bandwidth': 1024 * 1024 * 1024})
            self.qmp('migrate', {'channels': self.migration_file(temporary)})
            self.wait_migration(timeout)
            manifest = dict(configuration=configuration, bytes=temporary.stat().st_size)
            manifest_pending.write_text(json.dumps(manifest, indent=2) + '\n')
            temporary.replace(path)
            manifest_pending.replace(manifest_path)
            complete = True
            return manifest
        finally:
            if not complete:
                self.qmp('migrate_cancel')
                temporary.unlink(missing_ok=True)
                manifest_pending.unlink(missing_ok=True)
            if was_running:
                self.qmp('cont')

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()

        self.selector.close()
        self.log.close()
        os.close(self.qmp_input)
        os.close(self.qmp_output)
        for path in self.qmp_pipes:
            path.unlink(missing_ok=True)


def assert_success(output):
    if re.search(r" \(-\d+\)|FAIL:|exited [1-9]|PANIC:", output):
        raise AssertionError(output)


def shell_and_desktop(vm):
    print(f"TEST {vm.name}: command help, text tools, editor, and display lifecycle", flush=True)
    vm.command("help", "help COMMAND")
    vm.command("help text", "edit [-r] FILE")
    vm.command("help mount", "DIRECTORY must exist and be empty")
    vm.command("cp --help", "Usage: cp SOURCE DEST")

    for command, expected in (("cp", "Usage: cp SOURCE DEST"),
                              ("pwd extra", "incorrect number of arguments"),
                              ("chmod 999 /tmp/none", "mode must be octal"),
                              ("sleep nope", "SECONDS must be"),
                              ("kill 1", "PID must be greater than 1"),
                              ("mount -o bad disk /tmp", "mount options must be ro or rw"),
                              ("ls -z", "unknown option"),
                              ("echo 'unterminated", "unmatched quote"),
                              ("echo hello | cat", "unsupported shell operator"),
                              ("moun", "Did you mean 'mount'?")):
        vm.command(command, expected)

    vm.command("echo '&'", "&\n")
    vm.command("echo " + "x" * 600, "511-byte console line limit")
    vm.command("echo after-long-line", "after-long-line\n")
    vm.send("echo eof-commits-line\x04")

    assert "eof-commits-line\n" in vm.until_prompt()
    vm.command("echo clean\x1b[D", "clean\n")
    vm.command("uname -a", "ModernOS modernos 0.2")
    vm.command("uptime", "up ")
    vm.command("which edit head gui", "/bin/gui")
    assert_success(vm.command("mkdir -p /tmp/docs/drafts"))
    assert_success(vm.command("write /tmp/docs/drafts/note.txt first line"))
    assert_success(vm.command("append /tmp/docs/drafts/note.txt second line"))
    vm.command("head -n 1 /tmp/docs/drafts/note.txt", "first line\n")
    output = vm.command("tail -n 1 /tmp/docs/drafts/note.txt", "second line\n")
    assert "first line\n" not in output, output
    vm.command("wc /tmp/docs/drafts/note.txt", "2 4 23 ")
    vm.command("grep -n second /tmp/docs/drafts/note.txt", "2:second line")
    vm.command("find /tmp/docs", "/tmp/docs/drafts/note.txt")
    vm.command("hexdump /tmp/docs/drafts/note.txt", "66 69 72 73 74")
    vm.command("ls -l /tmp/docs/drafts", "644 23 note.txt")
    vm.command("history", "help mount")

    path = "/tmp/docs/drafts/document.txt"
    vm.send(f"edit {path}\n")
    vm.until(rb"Ready\. Ctrl-S saves; Ctrl-Q quits\.")
    vm.send("Alpha draft\nBeta line\n\x13")
    vm.until(rb"Saved\. Previous version: FILE.bak\.")
    vm.send("\x11")
    vm.until_prompt()
    vm.command(f"cat {path}", "Alpha draft\nBeta line\n")

    vm.send(f"edit {path}\n")
    vm.until(rb"Ready\. Ctrl-S saves; Ctrl-Q quits\.")
    vm.send("\x1b[F updated\x13")
    vm.until(rb"Saved\. Previous version: FILE.bak\.")
    vm.send("\x11")
    vm.until_prompt()
    vm.command(f"cat {path}", "Alpha draft updated\nBeta line\n")
    vm.command(f"cat {path}.bak", "Alpha draft\nBeta line\n")

    vm.send(f"edit {path}\n")
    vm.until(rb"Ready\. Ctrl-S saves; Ctrl-Q quits\.")
    vm.send("discard me\x11")
    vm.until(rb"Unsaved changes: Ctrl-S saves")
    vm.send("\x11")
    vm.until_prompt()
    output = vm.command(f"cat {path}", "Alpha draft updated")
    assert "discard me" not in output, output

    assert_success(vm.command(f"write {path}.edit-save KEEP_RECOVERY_DATA"))

    vm.send(f"edit {path}\n")
    vm.until(rb"Ready\. Ctrl-S saves; Ctrl-Q quits\.")
    vm.send("cannot-save\x13")
    vm.until(rb"Destination already exists")
    vm.send("\x11")
    vm.until(rb"Unsaved changes: Ctrl-S saves")
    vm.send("\x11")
    vm.until_prompt()
    vm.command(f"cat {path}.edit-save", "KEEP_RECOVERY_DATA")
    vm.command(f"cat {path}", "Alpha draft updated\nBeta line\n")

    assert_success(vm.command(f"rm {path}.edit-save"))
    assert_success(vm.command(f"chmod 444 {path}"))

    vm.send(f"edit {path}\n")
    vm.until(rb"Read-only view\.")
    vm.send("X\x13")
    vm.until(rb"Read-only filesystem")
    vm.send("\x11")
    vm.until_prompt()
    assert_success(vm.command(f"chmod 644 {path}"))
    vm.command(f"cat {path}", "Alpha draft updated\nBeta line\n")

    vm.command("gui --info", "XRGB8888")
    assert_success(vm.command("mkdir -p /home/user/Projects"))
    assert_success(vm.command("write /home/user/Projects/gui-note.txt GUI_PREVIEW_MARKER"))
    vm.send("gui\n")
    vm.until(rb"GUI_READY: ModernOS desktop")
    desktop = WORK / (vm.name + "-desktop.ppm")
    vm.qmp("screendump", {"filename": str(desktop)})
    width, height = map(int, desktop.read_bytes().split(b"\n", 3)[1].split())

    # Legacy serial input follows the same focus and application dispatch path.
    vm.send("\x06gui-note.txt\n\n")
    time.sleep(0.3)
    preview = WORK / (vm.name + "-preview.ppm")
    vm.qmp("screendump", {"filename": str(preview)})
    before = desktop.read_bytes().split(b"\n", 3)[3]
    after = preview.read_bytes().split(b"\n", 3)[3]
    differences = sum(a != b for a, b in zip(before, after))
    assert differences > 1000, "Opening the actual document did not create an editor window"

    def key_event(key, down):
        return {"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": key}}}

    vm.qmp("input-send-event", {"events": [key_event(k, True) for k in ("ctrl", "alt", "esc")]})
    time.sleep(0.1)
    vm.qmp("input-send-event", {"events": [key_event(k, False) for k in ("esc", "alt", "ctrl")]})

    assert "gui: returned to terminal." in vm.until_prompt()
    vm.command("check --display-exit")
    vm.command("echo restored-after-display-exit", "restored-after-display-exit\n")
    vm.command("check --display-fault", "terminated by signal 4")
    vm.command("echo restored-after-display-fault", "restored-after-display-fault\n")
    vm.command("check", "USERLAND CHECK PASS")


def runtime(cpus, storage=True, bios=False):
    name = f"{'bios' if bios else 'uefi'}-{cpus}cpu"
    print(f"TEST {name}: booting QEMU", flush=True)
    vm = VM(cpus, bios=bios, storage=storage, label='runtime')
    try:
        print(f"TEST {name}: userland, terminal, and scheduler checks", flush=True)
        vm.command("hello static ELF", "Hello from ring 3!")
        vm.command("check", "USERLAND CHECK PASS")
        vm.command("exec-test", "argv[1]=execve")
        vm.command("fault", "terminated by signal 11")
        vm.command("hello after-fault", "argv[1]=after-fault")
        vm.send("check --tty-read\n")
        vm.until(rb"TTY_READ_READY\n")
        vm.send("canonical-input\n")

        assert "TTY_READ_OK: canonical-input" in vm.until_prompt()
        output = vm.command("sh -c 'check --tty-denied' &", "started pid")
        background = int(re.search(r"started pid (\d+)", output).group(1))
        vm.command(f"wait {background}", "status=0")

        # Script invocations may be background jobs. Explicit interactive and
        # stdin shells keep the terminal requirement; descendants never acquire
        # the parent's input/display ownership merely by invoking /bin/sh.
        for line in ("ash -ec 'printf SCRIPT_BG_OK > /tmp/background-script' &",
                     "build-sh -c 'check --tty-denied' &"):
            output = vm.command(line, "started pid")
            background = int(re.search(r"started pid (\d+)", output).group(1))
            vm.command(f"wait {background}", "status=0")

        vm.command("cat /tmp/background-script", "SCRIPT_BG_OK")
        for line in ("sh &", "ash -i -c 'check --tty-denied' &", "build-sh -s ignored &"):
            vm.command(line, "requires the foreground terminal")
        vm.send("spin 4000000000\n")
        vm.until(rb"spin pid=\d+ started\n")
        vm.send("\x03")

        assert "terminated by signal 2" in vm.until_prompt()
        vm.send("discard-partial-line\x03")
        vm.until_prompt()
        vm.command("echo editorx\b", "editor\n")

        cpus_text = vm.command("cpus")
        assert len(re.findall(r"cpu\d+ lapic=\d+ online=1", cpus_text)) == cpus, cpus_text

        output = vm.command("spin 4000000000 &", "started pid")
        pid = int(re.search(r"started pid (\d+)", output).group(1))
        vm.command("hello while-spinning", "argv[1]=while-spinning")
        vm.command("ps", "/bin/spin")
        assert_success(vm.command(f"kill {pid}"))
        vm.command(f"wait {pid}", "status=9")

        shell_and_desktop(vm)

        if storage:
            print(f"TEST {name}: storage reads, copies, and mutations", flush=True)
            mounts = vm.command("mounts")
            for line in ("/mnt/nvme0n1p1 ext ro", "/mnt/nvme1n1p1 fat32 rw", "/mnt/sata0 ext ro"):
                assert line in mounts, mounts
            vm.command("cat /mnt/nvme0n1p1/note.txt", "SOURCE_EXT4_READ_OK")
            vm.command("cat /mnt/sata0/note.txt", "SOURCE_EXT4_READ_OK")
            vm.command("/mnt/nvme0n1p1/hello-link symlink", "argv[1]=symlink")

            dst = "/mnt/nvme1n1p1"
            vm.command("lsblk", "NAME TYPE BYTES SECTOR RO FSTYPE MOUNTPOINT")
            vm.command("mv /mnt /tmp/moved-mounts", "busy")
            vm.command(f"check --storage {dst}", "STORAGE SAFETY PASS")

            assert_success(vm.command(f"cd {dst}"))
            vm.command(f"umount {dst}", "busy")

            assert_success(vm.command("cd /"))
            assert_success(vm.command(f"umount {dst}"))
            assert_success(vm.command("mkdir /mnt/data"))
            assert_success(vm.command("mount -t fat32 -o ro nvme1n1p1 /mnt/data"))
            vm.command("mounts", "/mnt/data fat32 ro")
            vm.command("write /mnt/data/denied.txt blocked", "Read-only filesystem")
            vm.command("mount nvme1n1p1 /mnt/data", "busy")

            assert_success(vm.command("umount /mnt/data"))
            assert_success(vm.command(f"mount -o rw nvme1n1p1 {dst}"))
            assert_success(vm.command("rmdir /mnt/data"))

            vm.send(f'edit "{dst}/saved document.txt"\n')
            vm.until(rb"Ready\. Ctrl-S saves; Ctrl-Q quits\.")
            vm.send("PERSISTENT_EDITOR_OK\n\x13")
            vm.until(rb"Saved\. Previous version: FILE.bak\.")
            vm.send("\x11")
            vm.until_prompt()
            vm.command(f'cat "{dst}/saved document.txt"', "PERSISTENT_EDITOR_OK")

            assert_success(vm.command(f"cp /mnt/nvme0n1p1/hello {dst}/copied executable.elf".replace(
                f"{dst}/copied executable.elf", f"\"{dst}/copied executable.elf\"")))
            vm.command(f'"{dst}/copied executable.elf" from-fat32', "argv[1]=from-fat32")
            assert_success(vm.command(
                f'cp "/mnt/nvme0n1p1/long source filename.bin" "{dst}/long destination filename.bin"'))
            assert_success(vm.command(f"cp /mnt/nvme0n1p1/sparse.bin {dst}/sparse.bin"))

            assert_success(vm.command(f"mkdir {dst}/subdirectory"))
            assert_success(vm.command(f"write {dst}/subdirectory/message.txt PERSISTENT_FAT32_OK"))
            vm.command(f"cat {dst}/subdirectory/message.txt", "PERSISTENT_FAT32_OK")
            assert_success(vm.command(f"mv {dst}/subdirectory/message.txt {dst}/renamed.txt"))
            assert_success(vm.command(f"rmdir {dst}/subdirectory"))

            assert_success(vm.command(f"mkdir {dst}/outer"))
            assert_success(vm.command(f"mkdir {dst}/inner"))
            assert_success(vm.command(f"mv {dst}/inner {dst}/outer/inner"))
            assert_success(vm.command(f"write {dst}/outer/inner/test.txt DIRECTORY_MOVE_OK"))
            assert_success(vm.command(f"rm {dst}/outer/inner/test.txt"))
            assert_success(vm.command(f"rmdir {dst}/outer/inner"))
            assert_success(vm.command(f"rmdir {dst}/outer"))

            vm.command("write /mnt/nvme0n1p1/forbidden.txt nope", "Read-only filesystem")
            assert_success(vm.command("sync"))

        print(f"PASS {vm.name}: userland, isolation, scheduler" + (", NVMe, AHCI, ext2/ext4, FAT32" if storage else ""), flush=True)
    finally:
        print(f"TEST {name}: stopping QEMU", flush=True)
        vm.close()


def persistence(payload):
    print("TEST persistence: extracting copied files from FAT32", flush=True)
    image = str(WORK / "fat.img") + "@@" + str(2048 * 512)
    for source, destination in (("/copied executable.elf", "copied.elf"),
                                ("/long destination filename.bin", "copied.bin"),
                                ("/sparse.bin", "sparse-copy.bin"),
                                ("/renamed.txt", "renamed-copy.txt"),
                                ("/saved document.txt", "editor-copy.txt")):
        output = WORK / destination

        # Exercise overwriting even on the first run, and detect a silent skip
        # instead of comparing stale exports left by an earlier successful run.
        output.write_bytes(b"STALE TEST EXPORT: mcopy must replace this file\n")
        run("mcopy", "-D", "o", "-i", image, "::" + source, output)

    assert (WORK / "copied.elf").read_bytes() == (ROOT / "userland/bin/hello").read_bytes()
    assert (WORK / "copied.bin").read_bytes() == payload
    assert (WORK / "sparse-copy.bin").read_bytes() == (WORK / "source/sparse.bin").read_bytes()
    assert (WORK / "renamed-copy.txt").read_text() == "PERSISTENT_FAT32_OK\n"
    assert (WORK / "editor-copy.txt").read_text() == "PERSISTENT_EDITOR_OK\n"

    with (WORK / "fat.img").open("rb") as disk, (WORK / "checked-fat.volume").open("wb") as volume:
        disk.seek(2048 * 512)
        remaining = (WORK / "fat.volume").stat().st_size
        while remaining:
            chunk = disk.read(min(1024 * 1024, remaining))
            volume.write(chunk)
            remaining -= len(chunk)

    print("TEST persistence: checking FAT32 consistency with fsck.fat", flush=True)
    run("fsck.fat", "-n", WORK / "checked-fat.volume")

    print("PASS persistence: copied ELF/data/sparse bytes match; FAT32 passes fsck.fat", flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cpus", type=int, nargs="+", default=[1, 4])
    parser.add_argument("--no-storage", action="store_true")
    parser.add_argument("--bios", action="store_true")
    args = parser.parse_args()

    WORK.mkdir(parents=True, exist_ok=True)
    check_run_recipe()

    for count in args.cpus:
        if not args.no_storage:
            print(f"TEST {count} CPU(s): preparing disposable storage images", flush=True)
        payload = prepare() if not args.no_storage else None
        before = hashlib.sha256((WORK / "linux.img").read_bytes()
                                ).digest() if payload is not None else None
        runtime(count, not args.no_storage, args.bios)

        if payload is not None:
            print("TEST persistence: verifying the source drive stayed unchanged", flush=True)
            assert hashlib.sha256((WORK / "linux.img").read_bytes()
                                  ).digest() == before, "Source ext4 drive changed"
            persistence(payload)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Exercise the real desktop via emulated USB input and validate guest files.

No reference/demo rows exist in the production application. Every displayed
fixture here is created through the guest's ordinary filesystem APIs.
"""
import argparse
import hashlib
import re
import time
from pathlib import Path
from test_runtime import VM, WORK, prepare, run

LONG_DOCUMENT = b"first\tcolumn\r\n" + b"L" * 2600 + b"TAIL"


class DesktopTest:
    def __init__(self, vm):
        self.vm = vm
        self.x = self.y = 0
        self.width = self.height = 0

    def key(self, name, *modifiers):
        def event(key, down):
            return {"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": key}}}
        self.vm.qmp("input-send-event", {"events": [event(k, True) for k in (*modifiers, name)]})
        time.sleep(0.05)
        self.vm.qmp("input-send-event", {"events": [event(k, False)
                    for k in (name, *reversed(modifiers))]})
        time.sleep(0.07)

    def move(self, x, y, delay=0.08):
        self.vm.qmp("input-send-event", {"events": [
            {"type": "rel", "data": {"axis": "x", "value": x - self.x}},
            {"type": "rel", "data": {"axis": "y", "value": y - self.y}}]})
        self.x, self.y = x, y
        time.sleep(delay)

    def button(self, down, button="left"):
        self.vm.qmp("input-send-event",
                    {"events": [{"type": "btn", "data": {"button": button, "down": down}}]})
        time.sleep(0.06)

    def click(self, x, y, button="left"):
        self.move(x, y)
        self.button(True, button)
        self.button(False, button)

    def screenshot(self, name):
        path = WORK / f"{name}.ppm"
        time.sleep(0.12)
        self.vm.qmp("screendump", {"filename": str(path)})
        parts = path.read_bytes().split(b"\n", 3)
        width, height = map(int, parts[1].split())

        assert parts[0] == b"P6" and parts[2] == b"255"
        assert len(parts[3]) == width * height * 3
        if not self.width:
            self.width, self.height = width, height
            self.x, self.y = width // 2, height // 2

        try:
            from PIL import Image
            Image.open(path).save(path.with_suffix(".png"))
        except ImportError:
            pass
        return parts[3]

    def expect(self, action, detail=""):
        return self.vm.until(rb"DESKTOP " + action.encode() + rb"[^\n]*" + re.escape(detail.encode()) + rb"[^\n]*\n", 20)

    def path(self, path):
        self.key("l", "ctrl")
        self.vm.send(path + "\n")
        self.expect("NAVIGATE", path)
        time.sleep(0.1)

    def find(self, name):
        self.key("f", "ctrl")
        self.vm.send(name + "\n")
        time.sleep(0.12)

    def launch(self, name):
        self.key("spc", "meta_l")
        self.vm.send(name)
        time.sleep(0.08)
        self.key("ret")
        output = self.expect("OPEN", name)
        return int(re.findall(r"DESKTOP OPEN window=(\d+)", output)[-1])

    def exit(self):
        self.key("esc", "ctrl", "alt")

        # A Terminal trace may end with its own os:...$ prompt. Wait for the
        # GUI's actual completion before sending commands to the console.
        return self.vm.until(rb"gui: returned to terminal\.[\s\S]*os:[^\n]*\$ $", 20)


def fixtures(vm):
    for folder in ("Artwork", "Documents", "Source", "Tools"):
        vm.command("mkdir -p /home/user/Projects/" + folder)
    for name, text in (("journal.txt", "A real document"), ("build.c", "int entry(void) { return 0; }"),
                       ("events.h", "void on_event(void);"), ("settings.json", '{"accent":"jade"}'),
                       ("notes.md", "ModernOS workspace"), ("tasks.txt", "Write and test")):
        vm.command(f"write /home/user/Projects/{name} '{text}'")

    vm.command("mkdir -p /home/user/Projects/Source/Nested")
    vm.command("write /home/user/Projects/Source/Nested/proof.txt RECURSIVE_COPY_OK")


def disk_operations(d, left, top, cpus):
    dst = "/mnt/nvme1n1p1"
    d.path("/mnt/nvme0n1p1")
    d.key("n", "ctrl", "shift")
    d.vm.send("forbidden\n")
    d.expect("ERROR", "New folder")
    d.key("esc")
    d.find("hello-link")
    d.key("c", "ctrl")
    d.expect("ERROR", "Copy symbolic link")
    d.key("esc")
    d.find("note.txt")
    d.key("ret")
    d.expect("OPEN", "note.txt")
    d.key("s", "ctrl", "shift")
    d.vm.send(dst + "/saved-copy.txt\n")
    d.expect("SAVE", "saved-copy.txt")
    d.key("a", "ctrl")
    d.vm.send("GUI_DISK_DOCUMENT_OK")
    time.sleep(.15)
    d.key("s", "ctrl")
    d.expect("SAVE", "saved-copy.txt")
    d.key("f4", "alt")
    d.expect("CLOSE")

    # Copy a read-only file without changing its permissions or losing bytes.
    d.path("/home/user/Projects")
    d.find("events.h")
    d.key("c", "ctrl")
    d.expect("COPY")
    d.path(dst)
    d.key("v", "ctrl")
    d.expect("PASTE_COMPLETE")
    d.path("/home/user/Projects")
    d.find("Source")
    d.key("c", "ctrl")
    d.expect("COPY")
    d.path(dst)
    d.key("v", "ctrl")
    d.expect("PASTE_COMPLETE")
    d.path("/home/user/Projects")
    d.find("tasks.txt")
    d.key("x", "ctrl")
    d.expect("CUT")
    d.path(dst)
    d.key("v", "ctrl")
    d.expect("PASTE_COMPLETE")

    # Trash keeps real original-path records and supports restoring disk files.
    d.find("saved-copy.txt")
    d.key("delete")
    d.expect("TRASH")
    d.path("@Trash")
    d.find("saved-copy.txt")
    d.click(left + 300, top + 217, "right")
    for _ in range(4):
        d.key("down")
    d.key("ret")
    d.expect("RESTORE_FILE", "saved-copy.txt")

    # CRLF/tab bytes survive editing and lines beyond 2048 bytes remain usable.
    d.path(dst)
    d.find("long-line.txt")
    d.key("ret")
    d.expect("OPEN", "long-line.txt")
    d.key("end", "ctrl")
    d.vm.send("-EDIT")
    time.sleep(.15)
    d.key("s", "ctrl")
    d.expect("SAVE", "long-line.txt")
    d.screenshot(f"disk-editor-{cpus}cpu")
    d.key("f4", "alt")
    d.expect("CLOSE")
    d.path("/home/user/Projects")

    print("PASS GUI disk operations: read-only errors, Save As, recursive copy, cross-mount cut, Trash restore", flush=True)


SETTINGS_CONTROLS = [0, 1, 2, 4, 5, 6, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32,
                     8, 9, 10, 11, 12, 13, 14, 16, 17, 20, 33]


def settings_focus(d, focus, target, image=False):
    order = [control for control in SETTINGS_CONTROLS if image or not 24 <= control <= 30]
    assert target in order
    while focus != target:
        d.key("tab")
        focus = order[(order.index(focus) + 1) % len(order)]
    return focus


def settings_operations(d, focus, cpus):
    def choose(target, action):
        nonlocal focus
        focus = settings_focus(d, focus, target)
        d.key("ret")
        d.expect(action)

    choose(5, "WALLPAPER")
    choose(9, "SCALE")
    d.screenshot(f"scale-125-{cpus}cpu")
    choose(11, "SCALE")
    d.screenshot(f"scale-200-{cpus}cpu")

    # Reducing the count moves existing windows, preserving their application state.
    choose(12, "WORKSPACE_COUNT")
    choose(14, "WORKSPACE_COUNT")
    choose(8, "SCALE")
    choose(17, "APPEARANCE")
    d.screenshot(f"settings-dark-{cpus}cpu")
    d.key("s", "ctrl")
    d.expect("CONFIG_SAVED")
    d.key("esc")

    print("PASS Settings: wallpaper, dark mode, 125%/200% scale, workspace configuration, keyboard navigation", flush=True)


def functional(cpus, storage, iso=None, accel='tcg'):
    vm = VM(cpus, storage=storage, label='desktop-' + accel, iso=iso, accel=accel)
    try:
        vm.command("check", "USERLAND CHECK PASS")
        fixtures(vm)
        if storage:
            vm.command("chmod 444 /home/user/Projects/events.h")
        vm.send("gui --trace --stats\n")
        vm.until(rb"GUI_READY: ModernOS desktop", 40)
        d = DesktopTest(vm)
        first = d.screenshot(f"desktop-{cpus}cpu")
        width, height = d.width, d.height
        left, top, ww, wh = (width - width * 78 // 100) // 2, height * 8 // 100, width * 78 // 100, height * 788 // 1000

        def pixel(image, x, y):
            return tuple(image[(y * width + x) * 3:(y * width + x) * 3 + 3])

        assert pixel(first, left + 210, top + 120) == (255, 255, 255)
        assert pixel(first, left + 80, top + 55) == (245, 247, 246)
        assert pixel(first, width // 2, 20) != (255, 255, 255), "Unexpected top system bar"

        # Hover damage covers whole compact controls and clears after departure.
        d.move(148, height - 29)
        hovered = d.screenshot(f"taskbar-hover-{cpus}cpu")
        for x in (145, 174):
            assert pixel(hovered, x, height - 33) != pixel(first, x, height - 33)
        d.move(width - 45, height - 80)
        cleared = d.screenshot(f"taskbar-hover-cleared-{cpus}cpu")
        for x in (145, 174):
            assert pixel(cleared, x, height - 33) == pixel(first, x, height - 33)

        # Window dragging and resizing operate on real geometry, not a skin.
        d.move(left + ww // 2, top + 20)
        d.button(True)
        d.move(left + ww // 2 + 24, top + 38)
        d.button(False)
        geometry = d.expect("GEOMETRY")
        match = re.search(r"x=(\d+) y=(\d+) width=(\d+) height=(\d+)", geometry)
        assert tuple(map(int, match.groups())) == (left + 24, top + 18, ww, wh), geometry
        left, top = left + 24, top + 18
        d.move(left + ww - 2, top + wh - 2)
        d.button(True)
        d.move(left + ww + 18, top + wh + 12)
        d.button(False)
        geometry = d.expect("GEOMETRY")
        match = re.search(r"width=(\d+) height=(\d+)", geometry)
        assert tuple(map(int, match.groups())) == (ww + 20, wh + 14), geometry
        ww, wh = ww + 20, wh + 14
        d.click(left + ww - 44, top + 20)
        d.expect("MAXIMIZE")
        d.click(width - 44, 20)
        d.expect("RESTORE")
        d.click(left + ww - 68, top + 20)
        d.expect("MINIMIZE")
        d.click(82, height - 21)
        d.expect("FOCUS", "Files")

        # Keyboard operations and guest-side verification cover actual files.
        d.find("journal.txt")
        d.key("f2")
        vm.send("draft.txt\n")
        d.expect("RENAME", "draft.txt")
        d.key("esc")
        d.key("n", "ctrl")
        vm.send("created.txt\n")
        d.expect("CREATE_FILE", "created.txt")
        d.key("n", "ctrl", "shift")
        vm.send("New folder\n")
        d.expect("CREATE_FOLDER", "New folder")
        d.key("end")
        d.find("draft.txt")

        # A range anchor from an earlier, larger view cannot select hidden files.
        d.key("home", "shift")
        d.key("c", "ctrl")
        d.expect("COPY")
        d.path("/home/user/Downloads")
        d.key("v", "ctrl")
        d.expect("PASTE_COMPLETE")

        # Destination collisions must produce an error and preserve old data.
        d.key("v", "ctrl")
        d.expect("ERROR", "Copy / move stopped")
        d.key("esc")
        d.find("draft.txt")
        d.key("delete")
        d.expect("TRASH")
        d.path("/home/user/Projects")
        d.find("Source")
        d.key("c", "ctrl")
        d.expect("COPY")
        d.path("/home/user/Downloads")
        d.key("v", "ctrl")
        d.expect("PASTE_COMPLETE")

        # Native editor writes documents and refuses silently losing edits.
        d.path("/home/user/Projects")
        d.find("draft.txt")
        d.key("ret")
        d.expect("OPEN", "draft.txt")
        d.key("a", "ctrl")
        vm.send("GUI_EDITOR_WRITE_OK")
        time.sleep(0.12)
        d.key("s", "ctrl")
        d.expect("SAVE", "draft.txt")
        d.screenshot(f"editor-{cpus}cpu")
        vm.send(" pending")
        time.sleep(0.12)

        # Cancelling desktop exit keeps unrelated windows and native processes.
        # An unsaved document in another workspace is brought forward to save.
        d.key("2", "meta_l")
        d.expect("WORKSPACE")
        d.key("esc", "ctrl", "alt")
        d.expect("WORKSPACE")
        d.key("esc")
        d.key("f4", "alt")
        d.key("esc")
        d.key("s", "ctrl")
        d.expect("SAVE", "draft.txt")
        d.key("f4", "alt")
        d.expect("CLOSE")

        # Closing a dialog owner must dismiss its callbacks before its state
        # is freed or the window slot is reused by another application.
        assert d.launch("Files") > 1
        d.path("/home/user/Projects")
        d.key("n", "ctrl")
        d.key("f4", "alt")
        d.expect("CLOSE")
        d.path("/home/user/Projects")

        if storage:
            disk_operations(d, left, top, cpus)

        # New workspace starts empty and retains the old arrangement.
        d.key("2", "meta_l")
        d.expect("WORKSPACE")
        empty = d.screenshot(f"workspace-2-empty-{cpus}cpu")
        assert pixel(empty, width // 2, height // 2) != (255, 255, 255)

        second = d.launch("Files")
        assert second > 1
        d.key("right", "meta_l")
        d.expect("SNAP", "right")
        d.key("3", "meta_l", "shift")
        d.expect("MOVE_WORKSPACE")
        d.key("3", "meta_l")
        d.expect("WORKSPACE")
        d.key("left", "meta_l")
        d.expect("SNAP", "left")
        d.key("1", "meta_l")
        d.expect("WORKSPACE")
        d.screenshot(f"workspace-1-restored-{cpus}cpu")

        # A real persistent shell and a spawned native program use pipes.
        d.click(121, height - 21)
        d.expect("TERMINAL_SHELL")
        time.sleep(.2)
        vm.send("write /home/user/Projects/from-terminal.txt TERMINAL_BACKEND_OK\n")
        d.expect("TERMINAL_COMMAND", "TERMINAL_BACKEND_OK")
        vm.send("hello gui-terminal\n")
        d.expect("TERMINAL_COMMAND", "hello gui-terminal")
        time.sleep(.4)
        d.screenshot(f"terminal-{cpus}cpu")
        d.click(198, height - 21)
        settings_open = d.expect("OPEN", "Settings")
        settings_id = int(re.findall(r"DESKTOP OPEN window=(\d+)", settings_open)[-1])

        # Settings window starts centered, 680x550, inside the work area.
        offset = (settings_id - 1) % 5 * 16
        sx = min(width - 680, (width - 680) // 2 + offset)
        sy = min(height - 42 - 550, (height - 42 - 550) // 2 + offset)
        d.click(sx + 26 + 148 + 9 + 55, sy + 40 + 108)
        d.expect("ACCENT")
        d.key("s", "ctrl")
        d.expect("CONFIG_SAVED")
        d.key("esc")
        d.screenshot(f"settings-{cpus}cpu")
        settings_operations(d, 1, cpus)
        d.click(82, height - 21, "right")
        d.key("ret")
        chosen = d.expect("FOCUS", "Files")
        assert f"FOCUS window=1 " in chosen
        d.click(82, height - 21, "right")
        d.key("down")
        d.key("ret")
        chosen = d.expect("FOCUS", "Files")
        assert f"FOCUS window={second} " in chosen
        output = d.exit()
        assert "gui: returned to terminal." in output
        vm.command("cat /home/user/Projects/draft.txt", "GUI_EDITOR_WRITE_OK pending")
        vm.command("cat /home/user/Projects/from-terminal.txt", "TERMINAL_BACKEND_OK")
        vm.command("cat /home/user/Downloads/Source/Nested/proof.txt", "RECURSIVE_COPY_OK")
        vm.command("cat /home/user/.Trash/draft.txt", "A real document")
        vm.command("stat /home/user/Downloads/draft.txt", "No such file")
        vm.command("ls /home/user/Projects", "created.txt")

        if storage:
            vm.command("cat /mnt/nvme1n1p1/saved-copy.txt", "GUI_DISK_DOCUMENT_OK")
            vm.command("cat /mnt/nvme1n1p1/events.h", "void on_event(void);")
            vm.command("stat /mnt/nvme1n1p1/events.h", "mode=555")
            vm.command("cat /mnt/nvme1n1p1/Source/Nested/proof.txt", "RECURSIVE_COPY_OK")
            vm.command("cat /mnt/nvme1n1p1/tasks.txt", "Write and test")
            vm.command("stat /home/user/Projects/tasks.txt", "No such file")
            vm.command("stat /mnt/nvme1n1p1/.Trash/saved-copy.txt", "No such file")
            vm.command("sync")

        vm.command("check", "USERLAND CHECK PASS")

        print(
            f"PASS desktop {cpus} CPU: windows, workspaces, files, editor, native shell, settings", flush=True)
    finally:
        vm.close()


def disk_persistence(cpus, source_hash, iso=None, accel='tcg'):
    assert hashlib.sha256((WORK / "linux.img").read_bytes()).digest() == source_hash
    image = str(WORK / "fat.img") + "@@" + str(2048 * 512)
    for source, expected in (
        ("/saved-copy.txt", b"GUI_DISK_DOCUMENT_OK"),
        ("/events.h", b"void on_event(void);\n"),
        ("/Source/Nested/proof.txt", b"RECURSIVE_COPY_OK\n"),
        ("/tasks.txt", b"Write and test\n"),
        ("/long-line.txt", LONG_DOCUMENT + b"-EDIT"),
        ("/.modernos/desktop.ini", b"accent=1\nwallpaper=1\nscale=0\nworkspaces=3\ndark=1\n"
         b"wallpaper_mode=0\nwallpaper_background=0\nwallpaper_path=\n")):
        exported = WORK / "gui-export.bin"
        run("mcopy", "-D", "o", "-i", image, "::" + source, exported)
        assert exported.read_bytes() == expected, source

    checked = WORK / "gui-checked-fat.volume"
    with (WORK / "fat.img").open("rb") as disk, checked.open("wb") as out:
        disk.seek(2048 * 512)
        for _ in range((WORK / "fat.volume").stat().st_size // (1024 * 1024)):
            out.write(disk.read(1024 * 1024))

    run("fsck.fat", "-n", checked)
    vm = VM(cpus, storage=True, label='desktop-' + accel, iso=iso, accel=accel)
    try:
        vm.command("cat /mnt/nvme1n1p1/saved-copy.txt", "GUI_DISK_DOCUMENT_OK")
        vm.send("gui --trace\n")
        vm.until(rb"GUI_READY: ModernOS desktop", 40)
        d = DesktopTest(vm)
        image = d.screenshot(f"desktop-reboot-preferences-{cpus}cpu")
        left = (d.width - d.width * 78 // 100) // 2
        y = d.height * 8 // 100 + 40 + 42 + 3 * 30 + 10
        at = (y * d.width + left + 12) * 3
        assert tuple(image[at:at + 3]) == (37, 63, 84), "Saved dark accent was not loaded"
        at = ((d.height * 8 // 100 + 120) * d.width + left + 210) * 3
        assert tuple(image[at:at + 3]) == (25, 37, 45), "Saved dark appearance was not loaded"
        d.exit()
    finally:
        vm.close()

    print("PASS GUI persistence: exact document bytes, preferences after reboot, source unchanged, FAT32 fsck", flush=True)


def performance(cpus, iso=None, accel='tcg'):
    vm = VM(cpus, storage=False, label='desktop-' + accel, iso=iso, accel=accel)
    try:
        vm.send("gui --stats\n")
        vm.until(rb"GUI_READY: ModernOS desktop", 40)
        d = DesktopTest(vm)
        d.screenshot(f"desktop-performance-{cpus}cpu")
        d.move(d.width - 45, d.height - 80)
        for i in range(60):
            d.move(d.width - 50 + (i % 7) * 3, d.height - 78 + (i % 5) * 2, 0.03)
        output = d.exit()
        m = re.search(
            r"GUI_STATS frames=(\d+) presents=(\d+) pixels=(\d+) full_frames=(\d+) screen_pixels=(\d+)", output)
        assert m, output
        frames, presents, pixels, full, screen = map(int, m.groups())
        assert frames >= 40 and presents == frames, output
        assert full == 1 and pixels < screen * 3, output

        print(f"PASS cursor damage: {frames} frames, {pixels:,} pixels, {full} full frame; "
              f"{100*(1-pixels/(screen*frames)):.2f}% fewer pixels than full repaint", flush=True)
    finally:
        vm.close()


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('--cpus', type=int, default=4)
    p.add_argument('--storage', action='store_true',
                   help='Use the disposable test_runtime disk fixtures')
    p.add_argument('--performance-only', action='store_true')
    p.add_argument('--iso')
    p.add_argument('--accel', choices=('tcg', 'kvm'), default='tcg')
    a = p.parse_args()

    WORK.mkdir(parents=True, exist_ok=True)
    source_hash = None

    if a.storage and not a.performance_only:
        print("TEST desktop: preparing disposable storage images", flush=True)
        prepare()
        source_hash = hashlib.sha256((WORK / "linux.img").read_bytes()).digest()
        long_document = WORK / "gui-long-line.txt"
        long_document.write_bytes(LONG_DOCUMENT)
        run("mcopy", "-D", "o", "-i", str(WORK / "fat.img") + "@@" + str(2048 * 512), long_document, "::/long-line.txt")

    if not a.performance_only:
        functional(a.cpus, a.storage, a.iso, a.accel)
        if a.storage:
            disk_persistence(a.cpus, source_hash, a.iso, a.accel)
    performance(a.cpus, a.iso, a.accel)

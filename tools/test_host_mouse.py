#!/usr/bin/env python3
"""Verify host GTK mouse events through XTest or the Wayland desktop portal.

This briefly focuses a disposable QEMU window and moves the host pointer.
Mouse events go through the X server, not QMP. The pointer is restored on exit.
XTest does not verify physical pointer capture on a Wayland compositor;
use diagnose_mouse.py for that check.
The Wayland backend requests temporary pointer-only permission through the
desktop portal. X11 is used to inspect the GTK/XWayland window and position;
Wayland pointer input is sent through the portal, rather than XTest.
Requires an X11/XWayland display, libX11, libXtst and KVM. The Wayland backend
also requires dbus-python, PyGObject, libei and the RemoteDesktop desktop portal.
"""
import argparse
import ctypes as C
import ctypes.util
import os
import re
import sys
import time
from test_runtime import VM
from test_desktop import DesktopTest, fixtures


class HostPointer:
    input_name = 'XTest'

    def __init__(self):
        self.x = C.CDLL(ctypes.util.find_library('X11'))
        self.xtest = C.CDLL(ctypes.util.find_library('Xtst'))
        p, u, i = C.c_void_p, C.c_ulong, C.c_int
        signatures = {
            'XOpenDisplay': (p, [C.c_char_p]),
            'XDefaultRootWindow': (u, [p]),
            'XInternAtom': (u, [p, C.c_char_p, i]),
            'XGetWindowProperty': (i, [p, u, u, C.c_long, C.c_long, i, u,
                                       C.POINTER(u), C.POINTER(i), C.POINTER(u),
                                       C.POINTER(u), C.POINTER(p)]),
            'XQueryTree': (i, [p, u, C.POINTER(u), C.POINTER(u),
                               C.POINTER(C.POINTER(u)), C.POINTER(C.c_uint)]),
            'XQueryPointer': (i, [p, u, C.POINTER(u), C.POINTER(u),
                                  C.POINTER(i), C.POINTER(i), C.POINTER(i),
                                  C.POINTER(i), C.POINTER(C.c_uint)]),
            'XGetGeometry': (i, [p, u, C.POINTER(u), C.POINTER(i), C.POINTER(i),
                                 C.POINTER(C.c_uint), C.POINTER(C.c_uint),
                                 C.POINTER(C.c_uint), C.POINTER(C.c_uint)]),
            'XTranslateCoordinates': (i, [p, u, u, i, i, C.POINTER(i),
                                          C.POINTER(i), C.POINTER(u)]),
            'XRaiseWindow': (i, [p, u]),
            'XSetInputFocus': (i, [p, u, i, u]),
            'XFlush': (i, [p]),
            'XFree': (i, [p]),
            'XCloseDisplay': (i, [p]),
        }
        for name, (result, arguments) in signatures.items():
            function = getattr(self.x, name)
            function.restype, function.argtypes = result, arguments

        for name, arguments in {
            'XTestFakeMotionEvent': [p, i, i, i, u],
            'XTestFakeRelativeMotionEvent': [p, i, i, u],
            'XTestFakeButtonEvent': [p, C.c_uint, i, u],
        }.items():
            function = getattr(self.xtest, name)
            function.restype, function.argtypes = i, arguments

        self.display = self.x.XOpenDisplay(None)
        if not self.display:
            raise RuntimeError('An accessible X11/XWayland DISPLAY is required')
        self.root = self.x.XDefaultRootWindow(self.display)
        self.pid_atom = self.x.XInternAtom(self.display, b'_NET_WM_PID', 0)
        root, child = C.c_ulong(), C.c_ulong()
        rx, ry, wx, wy, mask = C.c_int(), C.c_int(), C.c_int(), C.c_int(), C.c_uint()
        self.x.XQueryPointer(self.display, self.root, C.byref(root), C.byref(child),
                             C.byref(rx), C.byref(ry), C.byref(wx), C.byref(wy), C.byref(mask))
        self.original = rx.value, ry.value
        self.gain = (1., 1.)

    def window(self, pid):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            pending = [self.root]
            while pending:
                window = pending.pop()
                kind, form, count, after, data = C.c_ulong(), C.c_int(), C.c_ulong(), C.c_ulong(), C.c_void_p()
                self.x.XGetWindowProperty(self.display, window, self.pid_atom, 0, 1, 0, 6,
                                          C.byref(kind), C.byref(form), C.byref(count),
                                          C.byref(after), C.byref(data))

                try:
                    if data and form.value == 32 and count.value == 1:
                        if C.cast(data, C.POINTER(C.c_ulong))[0] == pid:
                            geometry = self.geometry(window)
                            if geometry[2] >= 640 and geometry[3] >= 480:
                                return window, geometry
                finally:
                    if data:
                        self.x.XFree(data)

                root, parent, children, number = C.c_ulong(), C.c_ulong(), C.POINTER(C.c_ulong)(), C.c_uint()
                if self.x.XQueryTree(self.display, window, C.byref(root), C.byref(parent),
                                     C.byref(children), C.byref(number)):
                    pending.extend(children[index] for index in range(number.value))
                    if children:
                        self.x.XFree(children)
            time.sleep(.05)
        raise AssertionError('QEMU did not create an X11 window; check GDK_BACKEND=x11')

    def geometry(self, window):
        root, child, x, y = C.c_ulong(), C.c_ulong(), C.c_int(), C.c_int()
        width, height, border, depth = C.c_uint(), C.c_uint(), C.c_uint(), C.c_uint()
        assert self.x.XGetGeometry(self.display, window, C.byref(root), C.byref(x), C.byref(y),
                                   C.byref(width), C.byref(height), C.byref(border), C.byref(depth))
        self.x.XTranslateCoordinates(self.display, window, self.root, 0, 0,
                                     C.byref(x), C.byref(y), C.byref(child))
        return x.value, y.value, width.value, height.value

    def move(self, x, y):
        assert self.xtest.XTestFakeMotionEvent(self.display, -1, x, y, 0)
        self.x.XFlush(self.display)
        time.sleep(.15)

        # XWayland's XTest/libei bridge may apply fractional monitor scaling.
        # Correct in root coordinates rather than assuming synthetic units.
        actual_x, actual_y = self.position()
        if (actual_x, actual_y) != (x, y):
            self.relative(round((x - actual_x) / self.gain[0]),
                          round((y - actual_y) / self.gain[1]))

    def position(self):
        root, child = C.c_ulong(), C.c_ulong()
        rx, ry, wx, wy, mask = C.c_int(), C.c_int(), C.c_int(), C.c_int(), C.c_uint()
        present = self.x.XQueryPointer(self.display, self.root, C.byref(root), C.byref(child),
                                       C.byref(rx), C.byref(ry), C.byref(wx), C.byref(wy), C.byref(mask))
        assert present, 'The pointer is on a different X screen'
        return rx.value, ry.value

    def calibrate(self):
        x, y = self.position()
        self.relative(80, 80)
        after_x, after_y = self.position()
        gain = (after_x - x) / 80, (after_y - y) / 80
        assert all(axis > 0 for axis in gain), (
            f'{self.input_name} motion was not delivered: before={(x, y)} after={(after_x, after_y)} '
            f'gain={gain} DISPLAY={os.environ.get("DISPLAY")} '
            f'XDG_SESSION_TYPE={os.environ.get("XDG_SESSION_TYPE")}. '
            'Check the desktop input-emulation connection before testing the guest.')

        # Keep the last usable calibration if delivery fails. Cleanup must not
        # divide by a failed zero-gain measurement and replace this assertion.
        self.gain = gain
        self.relative(round((x - after_x) / self.gain[0]),
                      round((y - after_y) / self.gain[1]))

    def relative(self, dx, dy):
        assert self.xtest.XTestFakeRelativeMotionEvent(self.display, dx, dy, 0)
        self.x.XFlush(self.display)
        time.sleep(.15)

    def button(self, down):
        assert self.xtest.XTestFakeButtonEvent(self.display, 1, down, 0)
        self.x.XFlush(self.display)
        time.sleep(.1)

    def close(self):
        if not self.display:
            return
        try:
            self.button(False)
            self.move(*self.original)
        except Exception as error:
            # Restoration is best effort, including when XTest itself failed.
            # Preserve the test's original failure and always close the X fd.
            print(f'HOST_POINTER_CLEANUP: {error}', file=sys.stderr, flush=True)
        finally:
            self.x.XCloseDisplay(self.display)
            self.display = None


class WaylandPointer(HostPointer):
    input_name = 'Wayland portal'

    def __init__(self):
        super().__init__()
        self.portal = None
        try:
            from wayland_pointer import PointerSession
            self.portal = PointerSession()
        except BaseException:
            self.x.XCloseDisplay(self.display)
            self.display = None
            raise

    def move(self, x, y):
        # A pointer-only session has no screen-capture stream for absolute
        # coordinates. Position through relative input, using measured gains.
        for _ in range(8):
            actual_x, actual_y = self.position()
            if abs(x - actual_x) <= 1 and abs(y - actual_y) <= 1:
                return
            self.relative(round((x - actual_x) / self.gain[0]),
                          round((y - actual_y) / self.gain[1]))
        raise AssertionError(f'Wayland pointer did not reach {(x, y)}: {self.position()}')

    def relative(self, dx, dy):
        self.portal.motion(dx, dy)
        time.sleep(.15)

    def button(self, down):
        self.portal.button(down)
        time.sleep(.1)

    def close(self):
        try:
            if self.portal is not None and self.portal.granted:
                try:
                    self.button(False)
                    _, _, width, height = self.geometry(self.root)
                    self.portal.restore(*self.original, width, height)
                except Exception as error:
                    print(f'HOST_POINTER_CLEANUP: {error}', file=sys.stderr, flush=True)
            if self.display:
                self.x.XCloseDisplay(self.display)
                self.display = None
        finally:
            if self.portal is not None:
                try:
                    self.portal.close()
                except Exception as error:
                    print(f'WAYLAND_PORTAL_CLEANUP: {error}', file=sys.stderr, flush=True)
                self.portal = None


def test(cpus, input_backend='xtest'):
    if os.environ.get('GDK_BACKEND') != 'x11':
        raise RuntimeError("Run with GDK_BACKEND=x11 to test the launcher's X11 mouse path")
    if input_backend == 'wayland' and os.environ.get('XDG_SESSION_TYPE') != 'wayland':
        raise RuntimeError('The Wayland portal backend requires a Wayland desktop session')

    host = WaylandPointer() if input_backend == 'wayland' else HostPointer()
    vm = None
    try:
        vm = VM(cpus, storage=False, accel='kvm', display='gtk,gl=on',
                label='host-mouse-x11', trace_display=True)
        assert vm.qmp('query-kvm')['enabled']
        fixtures(vm)
        vm.send('gui --stats --trace\n')
        vm.until(b'GUI_READY', 30)
        d = DesktopTest(vm)
        d.screenshot(f'host-mouse-initial-{cpus}cpu')
        window, (x, y, width, height) = host.window(vm.process.pid)
        print(f'HOST_WINDOW id={window} geometry={(x, y, width, height)}', flush=True)
        if input_backend == 'wayland':
            host.portal.authorize(f'x11:{window:x}')
        host.x.XRaiseWindow(host.display, window)
        host.x.XSetInputFocus(host.display, window, 2, 0)
        host.move(x + width // 2, y + height // 2)
        host.calibrate()
        host.move(x + width // 2, y + height // 2)

        # The first click asks GTK to capture a relative mouse.
        host.button(True)
        host.button(False)
        scale = width / d.width
        scale_x, scale_y = scale / host.gain[0], scale / host.gain[1]
        left, top = (d.width - d.width * 78 // 100) // 2, d.height * 8 // 100

        # Use empty title-bar space, clear of the Files breadcrumb controls.
        host.relative(round((left + 550 - d.width // 2) * scale_x),
                      round((top + 20 - d.height // 2) * scale_y))
        first = d.screenshot(f'host-mouse-start-{cpus}cpu')

        for dx, dy in ((32, 24), (-32, -24)):
            host.button(True)
            host.relative(round(dx * scale_x), round(dy * scale_y))
            host.button(False)
            geometry = d.expect('GEOMETRY')
            actual = tuple(map(int, re.search(r'x=(\d+) y=(\d+)', geometry).groups()))
            expected = (left + 32, top + 24) if dx > 0 else (left, top)
            assert all(abs(a - e) <= (1 if dx > 0 else 0)
                       for a, e in zip(actual, expected)), (actual, expected, geometry)

        final = d.screenshot(f'host-mouse-end-{cpus}cpu')
        assert first[:d.width * (d.height - 42) * 3] == final[:d.width * (d.height - 42) * 3], 'Host drag left stale pixels'
        output = d.exit()
        assert re.search(r'drag_frames=[1-9]\d*', output), output

        print(
            f'PASS synthetic host mouse: {host.input_name} motion/buttons, USB HID, window dragging and pixel restoration', flush=True)
    finally:
        if vm:
            vm.close()
        host.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cpus', type=int, default=4)
    parser.add_argument('--input-backend', choices=('xtest', 'wayland'), default='xtest',
                        help='Wayland requests temporary mouse-control permission through the desktop portal')
    args = parser.parse_args()
    test(args.cpus, args.input_backend)

#!/usr/bin/env python3
"""Temporary, explicitly approved pointer input through the desktop portal."""
import ctypes as C
import ctypes.util
import os
import select
import time
import uuid


class EISPointer:
    """Use the authorized portal's private EIS socket, never a global socket."""
    POINTER = 1
    ABSOLUTE = 2
    BUTTON = 32

    def __init__(self, fd):
        self.context = None
        self.fd_owned = False
        try:
            self._initialize(fd)
        except BaseException:
            try:
                self.close()
            finally:
                if not self.fd_owned:
                    os.close(fd)
            raise

    def _initialize(self, fd):
        library = ctypes.util.find_library('ei')
        if not library:
            raise RuntimeError('The Wayland pointer backend requires libei')
        self.lib = C.CDLL(library)
        p, i, u = C.c_void_p, C.c_int, C.c_uint32
        signatures = {
            'ei_new_sender': (p, [p]),
            'ei_unref': (p, [p]),
            'ei_configure_name': (None, [p, C.c_char_p]),
            'ei_setup_backend_fd': (i, [p, i]),
            'ei_get_fd': (i, [p]),
            'ei_dispatch': (None, [p]),
            'ei_get_event': (p, [p]),
            'ei_event_get_type': (i, [p]),
            'ei_event_get_seat': (p, [p]),
            'ei_event_get_device': (p, [p]),
            'ei_event_unref': (p, [p]),
            'ei_seat_bind_capabilities': (None, [p]),
            'ei_device_ref': (p, [p]),
            'ei_device_unref': (p, [p]),
            'ei_device_has_capability': (C.c_bool, [p, i]),
            'ei_device_get_name': (C.c_char_p, [p]),
            'ei_device_get_region': (p, [p, C.c_size_t]),
            'ei_region_get_x': (u, [p]),
            'ei_region_get_y': (u, [p]),
            'ei_region_get_width': (u, [p]),
            'ei_region_get_height': (u, [p]),
            'ei_region_get_physical_scale': (C.c_double, [p]),
            'ei_device_start_emulating': (None, [p, u]),
            'ei_device_stop_emulating': (None, [p]),
            'ei_device_pointer_motion': (None, [p, C.c_double, C.c_double]),
            'ei_device_pointer_motion_absolute': (None, [p, C.c_double, C.c_double]),
            'ei_device_button_button': (None, [p, u, C.c_bool]),
            'ei_device_frame': (None, [p, C.c_uint64]),
            'ei_now': (C.c_uint64, [p]),
        }
        for name, (result, arguments) in signatures.items():
            function = getattr(self.lib, name)
            function.restype, function.argtypes = result, arguments
        self.devices = set()
        self.resumed = set()
        self.sequence = 0
        self.context = self.lib.ei_new_sender(None)
        if not self.context:
            raise RuntimeError('Cannot create the libei sender context')

        try:
            self.lib.ei_configure_name(self.context, b'ModernOS mouse test')
            self.fd_owned = True
            result = self.lib.ei_setup_backend_fd(self.context, fd)
            if result:
                raise RuntimeError(f'Cannot connect the authorized EIS socket: {result}')
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                self.dispatch()
                if self.device(self.POINTER) and self.device(self.BUTTON) and self.device(self.ABSOLUTE):
                    print('WAYLAND_EIS_READY: relative/absolute pointer and buttons', flush=True)
                    return
                select.select([self.lib.ei_get_fd(self.context)], [], [], .05)

            raise TimeoutError(
                'Authorized compositor did not provide resumed pointer/button devices')
        except BaseException:
            self.close()
            raise

    def dispatch(self):
        self.lib.ei_dispatch(self.context)
        while event := self.lib.ei_get_event(self.context):
            try:
                kind = self.lib.ei_event_get_type(event)
                if kind == 2:
                    raise RuntimeError('Compositor disconnected the authorized EIS input session')

                if kind == 3:
                    seat = self.lib.ei_event_get_seat(event)
                    self.lib.ei_seat_bind_capabilities(seat, C.c_int(self.POINTER),
                                                       C.c_int(self.ABSOLUTE), C.c_int(self.BUTTON), C.c_void_p(None))
                elif kind in (5, 6, 7, 8):
                    device = self.lib.ei_event_get_device(event)
                    if kind == 5:
                        self.devices.add(self.lib.ei_device_ref(device))
                    elif kind == 6:
                        self.resumed.discard(device)
                        if device in self.devices:
                            self.devices.remove(device)
                            self.lib.ei_device_unref(device)
                    elif kind == 7:
                        self.resumed.discard(device)
                    elif kind == 8:
                        self.resumed.add(device)
                        self.sequence += 1
                        self.lib.ei_device_start_emulating(device, self.sequence)
                        name = self.lib.ei_device_get_name(device)
                        print('WAYLAND_EIS_DEVICE_RESUMED: ' + (name or b'unnamed').decode(errors='replace'), flush=True)
            finally:
                # Unknown protocol events must also be released.
                self.lib.ei_event_unref(event)

    def device(self, capability):
        return next((device for device in self.resumed
                     if self.lib.ei_device_has_capability(device, capability)), None)

    def send(self, capability, method, *arguments):
        self.dispatch()
        device = self.device(capability)
        if device is None:
            raise RuntimeError('The authorized pointer device is paused or unavailable')
        getattr(self.lib, method)(device, *arguments)
        self.lib.ei_device_frame(device, self.lib.ei_now(self.context))
        self.lib.ei_dispatch(self.context)

    def motion(self, dx, dy):
        self.send(self.POINTER, 'ei_device_pointer_motion', float(dx), float(dy))

    def button(self, down):
        self.send(self.BUTTON, 'ei_device_button_button', 0x110, bool(down))

    def restore(self, x, y, root_width, root_height):
        # X-root feedback may freeze after the XWayland QEMU surface closes.
        # Restore through native EIS absolute input instead. Only accept an
        # unambiguous, single-output mapping validated against X-root size.
        self.dispatch()
        device = self.device(self.ABSOLUTE)
        if device is None:
            raise RuntimeError('Absolute pointer unavailable for restoration')
        region = self.lib.ei_device_get_region(device, 0)
        if not region or self.lib.ei_device_get_region(device, 1):
            raise RuntimeError('Cannot map X-root coordinates to multiple EIS outputs')
        scale = self.lib.ei_region_get_physical_scale(region)
        width, height = self.lib.ei_region_get_width(region), self.lib.ei_region_get_height(region)
        if (scale <= 0 or self.lib.ei_region_get_x(region) or self.lib.ei_region_get_y(region)
                or abs(width * scale - root_width) > 2 or abs(height * scale - root_height) > 2
                or not (0 <= x < root_width and 0 <= y < root_height)):
            raise RuntimeError(
                'EIS output and X-root geometry do not provide a validated restoration mapping')

        self.send(self.ABSOLUTE, 'ei_device_pointer_motion_absolute', x / scale, y / scale)
        time.sleep(.15)
        self.dispatch()

        print(
            f'WAYLAND_POINTER_RESTORE: absolute logical=({x/scale:.3f},{y/scale:.3f}) scale={scale}', flush=True)

    def close(self):
        if not self.context:
            return
        for device in self.resumed:
            self.lib.ei_device_stop_emulating(device)
        self.lib.ei_dispatch(self.context)
        for device in self.devices:
            self.lib.ei_device_unref(device)
        self.devices.clear()
        self.resumed.clear()
        self.lib.ei_unref(self.context)
        self.context = None


class PointerSession:
    def __init__(self, timeout=300):
        # Keep these optional dependencies out of the existing XTest backend.
        import dbus
        from dbus.mainloop.glib import DBusGMainLoop
        from gi.repository import GLib

        DBusGMainLoop(set_as_default=True)
        self.dbus, self.GLib = dbus, GLib
        self.bus = dbus.SessionBus(private=True)
        self.portal_name = 'org.freedesktop.portal.Desktop'
        self.portal_path = '/org/freedesktop/portal/desktop'
        self.interface = 'org.freedesktop.portal.RemoteDesktop'
        self.portal = dbus.Interface(self.bus.get_object(self.portal_name, self.portal_path),
                                     self.interface)
        self.timeout = timeout
        self.session = None
        self.granted = False
        self.eis = None

    def _request(self, method, *arguments, options=None):
        dbus, GLib = self.dbus, self.GLib
        token = 'modernos_mouse_' + uuid.uuid4().hex
        sender = self.bus.get_unique_name()[1:].replace('.', '_')
        expected = f'{self.portal_path}/request/{sender}/{token}'
        options = dbus.Dictionary(options or {}, signature='sv')
        options['handle_token'] = dbus.String(token)
        loop = GLib.MainLoop()
        reply = []

        def response(code, results):
            reply.append((int(code), results))
            loop.quit()

        subscription = self.bus.add_signal_receiver(response, signal_name='Response',
                                                    dbus_interface='org.freedesktop.portal.Request', bus_name=self.portal_name,
                                                    path=expected)
        handle = None
        timer = None
        try:
            handle = str(getattr(self.portal, method)(*arguments, options))
            if handle != expected:
                raise RuntimeError('Desktop portal returned an unexpected request handle')

            def expired():
                loop.quit()
                return False

            timer = GLib.timeout_add_seconds(self.timeout, expired)
            if not reply:
                loop.run()

            if not reply:
                raise TimeoutError(f'{method}: desktop permission request timed out')
            code, results = reply[0]
            print(f'WAYLAND_PORTAL {method} response={code}', flush=True)

            if code != 0:
                raise PermissionError(
                    f'{method}: desktop permission was cancelled or denied (response={code})')
            return results
        finally:
            subscription.remove()
            if timer is not None and reply:
                GLib.source_remove(timer)
            if handle and not reply:
                dbus.Interface(self.bus.get_object(self.portal_name, handle),
                               'org.freedesktop.portal.Request').Close()

    def authorize(self, parent_window=''):
        dbus = self.dbus
        results = self._request('CreateSession', options={
            'session_handle_token': dbus.String('modernos_mouse_' + uuid.uuid4().hex)})
        self.session = dbus.ObjectPath(results['session_handle'])

        # Request pointer control only. No keyboard, clipboard, screen capture,
        # saved permission, or restore token is requested.
        self._request('SelectDevices', self.session, options={
            'types': dbus.UInt32(2), 'persist_mode': dbus.UInt32(0)})
        print('WAYLAND_PERMISSION_PENDING: approve temporary mouse control in the desktop dialog', flush=True)
        results = self._request('Start', self.session, dbus.String(parent_window))
        if not int(results.get('devices', 0)) & 2:
            raise PermissionError('Desktop portal did not grant pointer control')
        print('WAYLAND_PERMISSION_GRANTED: pointer only, permission not persisted', flush=True)

        # EIS is the compositor's direct authorized input transport. Do not
        # use Notify* after connecting this socket, as required by the portal.
        descriptor = self.portal.ConnectToEIS(self.session,
                                              dbus.Dictionary({}, signature='sv'))
        self.eis = EISPointer(descriptor.take())
        self.granted = True

    def motion(self, dx, dy):
        if not self.granted:
            raise PermissionError('Pointer input requires desktop approval')
        self.eis.motion(dx, dy)

    def button(self, down):
        if not self.granted:
            raise PermissionError('Pointer input requires desktop approval')
        self.eis.button(down)

    def restore(self, x, y, width, height):
        if not self.granted:
            raise PermissionError('Pointer input requires desktop approval')
        self.eis.restore(x, y, width, height)

    def close(self):
        try:
            try:
                if self.eis is not None:
                    self.eis.close()
                    self.eis = None
            finally:
                if self.session is not None:
                    self.dbus.Interface(self.bus.get_object(self.portal_name, self.session),
                                        'org.freedesktop.portal.Session').Close()
        finally:
            self.session = None
            self.granted = False
            self.bus.close()

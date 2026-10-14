#!/usr/bin/env python3
"""Validate shared dark surfaces, live switching, saved preferences, popup cleanup."""
import argparse
from test_runtime import VM
from test_desktop import DesktopTest, fixtures, settings_focus


def exercise(cpus):
    vm = VM(cpus, storage=False)
    try:
        fixtures(vm)
        vm.send('gui --trace --stats\n')
        vm.until(b'GUI_READY', 30)
        d = DesktopTest(vm)
        d.screenshot(f'appearance-initial-{cpus}cpu')
        width, height = d.width, d.height
        left, top = (width - width * 78 // 100) // 2, height * 8 // 100

        def pixel(data, x, y):
            at = (y * width + x) * 3
            return tuple(data[at:at + 3])

        def geometry(app, ident):
            w, h = (680, 550) if app == 'Settings' else (840, 560)
            offset = (ident - 1) % 5 * 16
            return min(width - w, (width - w) // 2 + offset), min(height - 42 - h, (height - 42 - h) // 2 + offset), w, h

        ident = d.launch('Settings')
        sx, sy, sw, sh = geometry('Settings', ident)
        focus = settings_focus(d, 0, 17)
        d.key('ret')
        d.expect('APPEARANCE', 'dark')
        image = d.screenshot(f'appearance-settings-dark-{cpus}cpu')
        assert pixel(image, sx + 8, sy + 55) == (25, 37, 45), 'Settings did not change'
        assert pixel(image, left + 80, top + 55) == (28, 41, 49), 'Sidebar did not change'

        focus = settings_focus(d, focus, 16)
        d.key('ret')
        d.expect('APPEARANCE', 'light')
        image = d.screenshot(f'appearance-settings-light-{cpus}cpu')
        assert pixel(image, sx + 8, sy + 55) == (255, 255, 255), 'Light mode was not restored'

        focus = settings_focus(d, focus, 17)
        d.key('ret')
        d.expect('APPEARANCE', 'dark')

        d.key('s', 'ctrl')
        d.expect('CONFIG_SAVED')
        image = d.screenshot(f'appearance-dialog-dark-{cpus}cpu')
        assert pixel(image, (width - 500) // 2 + 12, (height - 42 - 228) //
                     2 + 80) == (25, 37, 45), 'Dialog stayed light'
        d.key('esc')
        d.key('f4', 'alt');
        d.expect('CLOSE')
        image = d.screenshot(f'appearance-files-dark-{cpus}cpu')
        assert pixel(image, left + 210, top + 120) == (25, 37, 45), 'Files stayed light'
        assert pixel(image, left + 80, top + 55) == (28, 41, 49), 'Files sidebar stayed light'

        # Closing the launcher must remove its complete elevation shadow.
        d.move(width - 45, height - 70)
        before = d.screenshot(f'appearance-before-launcher-{cpus}cpu')
        d.key('spc', 'meta_l');
        d.screenshot(f'appearance-launcher-dark-{cpus}cpu')
        d.key('esc')
        after = d.screenshot(f'appearance-after-launcher-{cpus}cpu')
        assert before[:width * (height - 42) * 3] == after[:width * (height - 42) * 3], 'Launcher left stale shadow pixels'

        ident = d.launch('Text Editor')
        x, y, w, h = geometry('Text Editor', ident)
        image = d.screenshot(f'appearance-editor-dark-{cpus}cpu')
        assert pixel(image, x + 300, y + 170) == (25, 37, 45), 'Editor stayed light'
        d.key('f4', 'alt');
        d.expect('CLOSE')

        ident = d.launch('Terminal')
        x, y, w, h = geometry('Terminal', ident)
        image = d.screenshot(f'appearance-terminal-dark-{cpus}cpu')
        assert pixel(image, x + 300, y + 170) == (23, 35, 43), 'Terminal stayed light'
        d.exit()

        vm.command('cat /home/user/.config/desktop.ini', 'dark=1')
        vm.send('gui --trace\n');
        vm.until(b'GUI_READY', 30)
        d = DesktopTest(vm)
        image = d.screenshot(f'appearance-relaunch-{cpus}cpu')
        assert pixel(image, left + 210, top + 120) == (25, 37, 45), 'Saved dark mode did not reload'
        d.exit()

        print('PASS dark mode: Files, Settings, Editor, Terminal, dialog, launcher cleanup, light switch and saved reload', flush=True)
    finally:
        vm.close()


if __name__ == '__main__':
    p = argparse.ArgumentParser();
    p.add_argument('--cpus', type=int, default=4)
    exercise(p.parse_args().cpus)

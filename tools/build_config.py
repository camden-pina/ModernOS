#!/usr/bin/env python3
"""Configure Coast's build and virtual machine without running either."""
import argparse
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import sys
import tempfile
import textwrap
import tomllib

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CONFIG = ROOT / '.config'
GROUPS = {
    'build': 'Build resources and output',
    'desktop': 'Build features and verification',
    'sources': 'Verified sources and package trust',
    'vm': 'Virtual machine resources',
    'display': 'Display and boot firmware',
    'devices': 'Networking, audio and disks',
    'advanced': 'Advanced arguments',
}


@dataclass(frozen=True)
class Option:
    key: str
    label: str
    group: str
    default: object
    help: str
    kind: str = 'text'
    choices: tuple = ()
    minimum: int = 1
    maximum: int = 256


OPTIONS = (
    Option('JOBS', 'Parallel build jobs', 'build', 4,
           'Workers for component builds, including Electron. Range: 1..24. Lower this to reduce RAM use.',
           'int', maximum=24),
    Option('O', 'Output parent directory', 'build', 'build',
           'Kernel output directory and parent of the default OS build directory. Kbuild requires a path without whitespace.'),
    Option('ISO_BUILD', 'OS build directory', 'build', None,
           'Automatic: O/coast. Contains component receipts, logs and image/Coast.iso. Choose a fresh directory when recorded source inputs change.'),
    Option('SOURCE_MIRRORS', 'Source download mirrors', 'build', '',
           'Optional space-separated HTTPS mirrors for the base source manifest. Checksums are always verified.'),
    Option('DIAGNOSTICS', 'Code OSS diagnostics', 'desktop', False,
           'Include opt-in editor diagnostics. This affects the build identity.', 'bool'),
    Option('QUALIFY', 'Run guest qualification after building', 'desktop', False,
           'Enable diagnostics and all eleven guest qualification suites. Requires KVM, OVMF, passt, GnuPG, Pillow and Tesseract.', 'bool'),
    Option('WITH_MICROSOFT_AUTH', 'Microsoft sign-in helpers', 'desktop', False,
           'Include upstream proprietary Microsoft authentication binaries. Their provenance is recorded.', 'bool'),
    Option('ELECTRON_CHECKOUT', 'Verified Electron checkout', 'sources', None,
           'Optional existing, verified Electron source checkout, reused read-only. Empty means fetch pinned sources.'),
    Option('RUNTIME_BUILD', 'Verified Electron/Node receipt', 'sources', None,
           'Optional complete runtime BUILD.json. Also set the matching Electron checkout. Artifacts and pins are verified.'),
    Option('COMPONENTS', 'Component receipt map', 'sources', None,
           'Optional JSON map of component names to source-build receipts. Reuse is hash-verified; every OS integration remains included.'),
    Option('PACKAGE_CONFIG', 'Public package repository trust', 'sources', None,
           'Optional public JSON containing url, signing_fingerprint and public_key. Empty leaves repository setup to the OS owner.'),
    Option('MEMORY', 'Guest RAM', 'vm', '16G',
           'RAM for make run, such as 8192M, 16G or 32G. This does not limit host build RAM.', 'memory'),
    Option('SMP', 'Guest CPUs', 'vm', 4,
           'CPU count for make run. Range: 1..256.', 'int'),
    Option('QEMU_ACCEL', 'CPU acceleration', 'vm', 'auto',
           'Automatic uses KVM when accessible and TCG otherwise. Explicit KVM requires access to /dev/kvm.',
           choices=('auto', 'kvm', 'tcg')),
    Option('QEMU_CPU', 'QEMU CPU model', 'vm', None,
           'Automatic selects host with KVM and max with TCG. Set a model supported by your QEMU if needed.'),
    Option('QEMU_DISPLAY', 'QEMU display backend', 'display', 'gtk,gl=off',
           'Examples: gtk,gl=off; sdl,gl=on; none. GTK without GL is the portable default.'),
    Option('FIRMWARE', 'Boot firmware', 'display', 'bios',
           'BIOS uses QEMU firmware. UEFI uses a host OVMF file. Coast supports both.',
           choices=('bios', 'uefi')),
    Option('OVMF', 'OVMF firmware file', 'display', None,
           'Automatic discovers installed host OVMF. Used only with UEFI.'),
    Option('QEMU_VIDEO', 'Video device arguments', 'display',
           '-vga none -device VGA,vgamem_mb=32,refresh_rate=120',
           'Full QEMU video arguments. The default is VGA with 32 MiB video RAM and 120 Hz EDID metadata.', 'args'),
    Option('QEMU_SDL_DRIVER', 'SDL video driver', 'display', None,
           'Automatic inherits SDL_VIDEODRIVER or selects Wayland when available. Examples: wayland, x11.'),
    Option('QEMU_GTK_BACKEND', 'GTK display backend', 'display', None,
           'Automatic inherits GDK_BACKEND. Examples: wayland, x11.'),
    Option('QEMU_NETWORK', 'Network device arguments', 'devices',
           '-nic user,model=e1000e',
           'Full QEMU network arguments. Use -nic none to disable networking, or configure a supported passt backend.', 'args'),
    Option('QEMU_AUDIO', 'Audio device arguments', 'devices',
           '-audiodev sdl,id=audio0 -device AC97,audiodev=audio0',
           'Full QEMU audio arguments. Clear this field to disable the sound device.', 'args'),
    Option('LINUX_DISK', 'Linux raw disk image', 'devices', None,
           'Optional raw disk file attached as NVMe. The VM has write access; use a disposable image for tests.'),
    Option('FAT_DISK', 'FAT raw disk image', 'devices', None,
           'Optional raw disk file attached as NVMe, with write access.'),
    Option('SATA_DISK', 'SATA raw disk image', 'devices', None,
           'Optional raw disk file attached as SATA, with write access.'),
    Option('ISO_IMAGE', 'Coast ISO launch path', 'advanced', None,
           'Automatic boots ISO_BUILD/image/Coast.iso. An explicit path affects make run; it does not rename the build output.'),
    Option('ISO_ARGS', 'Additional builder arguments', 'advanced', '',
           'Extra build_desktop.py options, such as --plan. Prefer the named settings above for saved build features.', 'args'),
    Option('QEMU_EXTRA_ARGS', 'Additional QEMU arguments', 'advanced', '',
           'Extra QEMU arguments appended to make run. Arguments are parsed as shell words, without executing shell expressions.', 'args'),
    Option('HOST_ARGS', 'Host setup arguments', 'advanced', '',
           'Extra setup_host.py arguments, used only when make setup-host is explicitly run. The configurator installs nothing.', 'args'),
)
BY_KEY = {option.key: option for option in OPTIONS}


def defaults():
    return {option.key: option.default for option in OPTIONS}


def convert(option, value):
    if option.kind == 'bool':
        if isinstance(value, bool):
            return value
        if str(value).lower() in ('y', 'yes', 'true', '1'):
            return True
        if str(value).lower() in ('n', 'no', 'false', '0'):
            return False
        raise ValueError(option.key + ': enter yes or no')
    if option.kind == 'int':
        if isinstance(value, bool) or not str(value).isdigit():
            raise ValueError(option.key + ': enter a whole number')
        value = int(value)
        if not option.minimum <= value <= option.maximum:
            raise ValueError(f'{option.key}: range is {option.minimum}..{option.maximum}')
        return value
    if value is None and option.default is None:
        return None
    if not isinstance(value, str) or any(ord(c) < 32 or ord(c) == 127 for c in value):
        raise ValueError(option.key + ': enter a single line of text')
    if not value and option.default is None:
        return None
    if option.choices and value not in option.choices:
        raise ValueError(option.key + ': choose ' + ', '.join(option.choices))
    if option.key in ('O', 'ISO_BUILD') and not re.fullmatch(r'[A-Za-z0-9_./+-]+', value):
        raise ValueError(option.key + ': use letters, numbers, /, dot, underscore, + or - in build directory paths')
    if option.kind == 'memory':
        if not re.fullmatch(r'[1-9][0-9]*[MGT]', value):
            raise ValueError('MEMORY: use a positive size such as 8192M, 16G or 32G')
    if option.kind == 'args':
        try:
            shlex.split(value)
        except ValueError as error:
            raise ValueError(option.key + ': ' + str(error)) from error
    return value


def validate(values):
    values = {option.key: convert(option, values[option.key]) for option in OPTIONS}
    if values['RUNTIME_BUILD'] and not values['ELECTRON_CHECKOUT']:
        raise ValueError('RUNTIME_BUILD also needs the matching ELECTRON_CHECKOUT')
    return values


def load(path):
    path = Path(path)
    values = defaults()
    if not path.exists():
        return values
    try:
        data = tomllib.loads(path.read_text())
    except tomllib.TOMLDecodeError as error:
        raise ValueError(f'{path}: {error}') from error
    version = data.pop('format_version', None)
    if type(version) is not int or version != 1:
        raise ValueError(f'{path}: expected format_version = 1')
    for group, entries in data.items():
        if group not in GROUPS or not isinstance(entries, dict):
            raise ValueError(f'{path}: unknown configuration section {group}')
        for key, value in entries.items():
            if key not in BY_KEY or BY_KEY[key].group != group:
                raise ValueError(f'{path}: unknown option {group}.{key}')
            values[key] = convert(BY_KEY[key], value)
    return validate(values)


def serialize(values):
    values = validate(values)
    lines = ['# Coast build configuration. Edit with make menuconfig.',
             '# Empty optional paths use automatic defaults.', 'format_version = 1']
    for group in GROUPS:
        lines += ['', '[' + group + ']']
        for option in OPTIONS:
            if option.group != group:
                continue
            value = values[option.key]
            lines.append(option.key + ' = ' + json.dumps('' if value is None else value, ensure_ascii=False))
    return '\n'.join(lines) + '\n'


def atomic_write(path, text):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix='.' + path.name + '-', dir=path.parent)
    try:
        with os.fdopen(descriptor, 'wb') as stream:
            stream.write(text.encode('utf-8') if isinstance(text, str) else text)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)


def save(path, values):
    path = Path(path).expanduser()
    text = serialize(values)
    if path.exists() and path.read_bytes() != text.encode('utf-8'):
        atomic_write(Path(str(path) + '.old'), path.read_bytes())
    atomic_write(path, text)


def makefile(values):
    """Return inert Make values; data never becomes Make or shell code."""
    lines = ['# Generated from a validated Coast configuration; do not edit.']
    for option in OPTIONS:
        value = values[option.key]
        if value is None or option.choices and value == 'auto':
            continue
        if isinstance(value, bool):
            value = str(int(value))
        else:
            value = str(value)
        if option.kind == 'args':
            value = shlex.join(shlex.split(value))
        marker = '__COAST_LITERAL_' + hashlib.sha256((option.key + value).encode()).hexdigest() + '__'
        while marker in value:
            marker += '_'
        temporary = '__coast_config_' + option.key
        # Prefix/suffix also prevent define/endef and trailing-backslash syntax.
        lines += [f'define {temporary}', marker + value + marker, 'endef',
                  f'{option.key} := $(subst {marker},,$(value {temporary}))',
                  f'undefine {temporary}']
    return '\n'.join(lines) + '\n'


def display(value):
    if value is None:
        return '<automatic>'
    if isinstance(value, bool):
        return '[*]' if value else '[ ]'
    return str(value) or '<empty>'


def text_config(path):
    try:
        values = load(path)
    except ValueError as error:
        print(str(error) + '\nShowing defaults; saving will keep the old file as .old.')
        values = defaults()
    print('Coast build configuration\nEnter keeps a value, ? shows help, - clears a field, q cancels.')
    previous = None
    for option in OPTIONS:
        if previous != option.group:
            print('\n' + GROUPS[option.group])
            previous = option.group
        while True:
            current = values[option.key]
            shown = ('yes' if current else 'no') if option.kind == 'bool' else display(current)
            answer = input(f'{option.label} ({option.key}) [{shown}]: ')
            if answer.lower() == 'q':
                return False
            if answer == '?':
                print(option.help)
                continue
            if not answer:
                break
            try:
                values[option.key] = convert(option, '' if answer == '-' else answer)
                break
            except ValueError as error:
                print(error)
    validate(values)
    destination = input(f'Save configuration to [{path}]: ') or str(path)
    save(destination, values)
    print('Saved configuration: ' + destination)
    return Path(destination).expanduser()


class Menu:
    def __init__(self, screen, path):
        import curses
        self.curses, self.screen = curses, screen
        self.path = Path(path)
        self.selected, self.top = 0, 0
        self.message = 'All Coast components are included. Configure resources, build features and the VM below.'
        try:
            self.values = load(path)
            self.saved = dict(self.values)
        except ValueError as error:
            self.values, self.saved = defaults(), None
            self.message = 'Cannot load config; defaults shown. L loads another file; S saves. ' + str(error)

    def write(self, row, text, attribute=0):
        height, width = self.screen.getmaxyx()
        if 0 <= row < height and width > 1:
            self.screen.addnstr(row, 0, text, width - 1, attribute)

    def prompt(self, title, initial=''):
        c = self.curses
        height, width = self.screen.getmaxyx()
        value, position = str(initial), len(str(initial))
        c.curs_set(1)
        try:
            while True:
                height, width = self.screen.getmaxyx()
                if height < 4 or width < 8:
                    return None
                self.screen.move(height - 3, 0)
                self.screen.clrtobot()
                self.write(height - 3, title)
                offset = max(0, position - width + 3)
                self.write(height - 2, value[offset:offset + width - 2])
                self.write(height - 1, 'Enter: accept  Esc/Ctrl+G: cancel  Ctrl+U: clear')
                self.screen.move(height - 2, position - offset)
                self.screen.refresh()
                key = self.screen.get_wch()
                if key in ('\n', '\r', c.KEY_ENTER):
                    return value.strip()
                if key in ('\x07', '\x1b'):
                    return None
                if key in ('\x08', '\x7f', c.KEY_BACKSPACE) and position:
                    value, position = value[:position - 1] + value[position:], position - 1
                elif key == c.KEY_DC:
                    value = value[:position] + value[position + 1:]
                elif key == c.KEY_LEFT:
                    position = max(0, position - 1)
                elif key == c.KEY_RIGHT:
                    position = min(len(value), position + 1)
                elif key in (c.KEY_HOME, '\x01'):
                    position = 0
                elif key in (c.KEY_END, '\x05'):
                    position = len(value)
                elif key == '\x15':
                    value, position = '', 0
                elif isinstance(key, str) and key.isprintable():
                    value, position = value[:position] + key + value[position:], position + 1
        finally:
            c.curs_set(0)

    def help(self):
        option = OPTIONS[self.selected]
        height, width = self.screen.getmaxyx()
        self.screen.erase()
        self.write(0, option.label + ' (' + option.key + ')', self.curses.A_BOLD)
        lines = textwrap.wrap(option.help, max(10, width - 3))
        lines += ['', 'Default: ' + display(option.default)]
        if option.choices:
            lines += ['Choices: ' + ', '.join(option.choices)]
        for row, line in enumerate(lines[:height - 3], 2):
            self.write(row, line)
        self.write(height - 1, 'Press any key to return')
        self.screen.refresh()
        self.screen.getch()

    def run(self):
        c = self.curses
        c.curs_set(0)
        self.screen.keypad(True)
        while True:
            height, width = self.screen.getmaxyx()
            self.screen.erase()
            if height < 14 or width < 55:
                self.write(0, 'Enlarge the terminal to at least 55 columns and 14 rows; Q exits.')
                self.screen.refresh()
                if self.screen.getch() in (ord('q'), ord('Q'), 27):
                    return self.path
                continue
            capacity = height - 9
            self.top = min(self.top, self.selected)
            if self.selected >= self.top + capacity:
                self.top = self.selected - capacity + 1
            dirty = ' * unsaved' if self.values != self.saved else ''
            self.write(0, 'Coast Build Configuration' + dirty, c.A_BOLD)
            self.write(1, 'File: ' + str(self.path))
            self.write(2, GROUPS[OPTIONS[self.selected].group], c.A_BOLD)
            for row, index in enumerate(range(self.top, min(len(OPTIONS), self.top + capacity)), 4):
                option = OPTIONS[index]
                label = f' {">" if index == self.selected else " "} {option.label}: {display(self.values[option.key])}'
                self.write(row, label, c.A_REVERSE if index == self.selected else 0)
            self.write(height - 4, self.message)
            self.write(height - 3, 'Arrows: move  Enter: edit  Space: toggle/cycle  Tab: next group')
            self.write(height - 2, 'S/F2: save  L/F3: load  /: search  ?: help  D: defaults  Q: exit')
            self.screen.refresh()
            key = self.screen.getch()
            option = OPTIONS[self.selected]
            try:
                if key in (c.KEY_DOWN, ord('j')):
                    self.selected = (self.selected + 1) % len(OPTIONS)
                elif key in (c.KEY_UP, ord('k')):
                    self.selected = (self.selected - 1) % len(OPTIONS)
                elif key in (c.KEY_NPAGE, c.KEY_PPAGE):
                    self.selected = max(0, min(len(OPTIONS) - 1, self.selected + (capacity if key == c.KEY_NPAGE else -capacity)))
                elif key == 9:
                    self.selected = next((i for i in range(self.selected + 1, len(OPTIONS))
                                          if OPTIONS[i].group != option.group), 0)
                elif key in (ord('?'), c.KEY_F1):
                    self.help()
                elif key == ord('/'):
                    query = self.prompt('Search option name, label or help')
                    if query:
                        matches = [i for i, item in enumerate(OPTIONS) if query.lower() in
                                   (item.key + ' ' + item.label + ' ' + item.help).lower()]
                        self.selected = next((i for i in matches if i > self.selected), matches[0] if matches else self.selected)
                        self.message = 'No matching option' if not matches else str(len(matches)) + ' matching options'
                elif key in (ord(' '), 10, 13, c.KEY_ENTER):
                    value = self.values[option.key]
                    if option.kind == 'bool':
                        self.values[option.key] = not value
                    elif option.choices:
                        self.values[option.key] = option.choices[(option.choices.index(value) + 1) % len(option.choices)]
                    else:
                        answer = self.prompt(option.label, '' if value is None else value)
                        if answer is not None:
                            self.values[option.key] = convert(option, answer)
                elif key in (ord('s'), ord('S'), c.KEY_F2):
                    destination = self.prompt('Save configuration to', self.path)
                    if destination:
                        save(destination, self.values)
                        self.path, self.saved = Path(destination).expanduser(), dict(self.values)
                        self.message = 'Saved ' + str(self.path) + '. Q exits.'
                elif key in (ord('l'), ord('L'), c.KEY_F3):
                    source = self.prompt('Load configuration from', self.path)
                    if source:
                        if not Path(source).expanduser().is_file():
                            raise ValueError('Configuration not found: ' + source)
                        self.values = load(Path(source).expanduser())
                        self.message = 'Loaded ' + source + '. S saves these settings to your selected configuration file.'
                elif key in (ord('d'), ord('D')):
                    if (self.prompt('Reset all settings? Type yes') or '').lower() == 'yes':
                        self.values = defaults()
                        self.message = 'Defaults loaded. S saves them.'
                elif key in (ord('q'), ord('Q'), 27):
                    if self.values != self.saved:
                        answer = self.prompt('Unsaved changes: save, discard or cancel?', 'save')
                        if answer == 'save':
                            save(self.path, self.values)
                            self.saved = dict(self.values)
                        elif answer != 'discard':
                            continue
                    return self.path
            except (ValueError, OSError) as error:
                self.message = str(error)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('menu', 'text', 'defaults', 'show', 'emit-make'))
    parser.add_argument('--config', type=Path, default=DEFAULT_CONFIG)
    parser.add_argument('--cache-dir', type=Path, default=ROOT / 'build/config')
    args = parser.parse_args()
    path = args.config.expanduser()
    if args.action == 'defaults':
        save(path, defaults())
        print('Saved default configuration: ' + str(path))
    elif args.action == 'show':
        print(serialize(load(path)), end='')
    elif args.action == 'emit-make':
        if not path.exists():
            return
        content = makefile(load(path))
        target = args.cache_dir / (hashlib.sha256(content.encode()).hexdigest() + '.mk')
        if not target.exists() or target.read_bytes() != content.encode('utf-8'):
            atomic_write(target, content)
        print(target.resolve())
    else:
        if not sys.stdin.isatty() or not sys.stdout.isatty():
            parser.error('Use an interactive terminal for configuration, or use make defconfig for defaults')
        if args.action == 'text':
            path = text_config(path) or path
        else:
            try:
                import curses
            except ImportError:
                path = text_config(path) or path
            else:
                try:
                    path = curses.wrapper(lambda screen: Menu(screen, path).run())
                except curses.error as error:
                    raise ValueError('Cannot initialize the terminal menu; use make config for the text questionnaire') from error
        suffix = '' if path.resolve() == DEFAULT_CONFIG else ' CONFIG=' + shlex.quote(str(path))
        print('Build with make iso' + suffix + ', then boot with make run' + suffix + '.')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError) as error:
        raise SystemExit(str(error))
    except (KeyboardInterrupt, EOFError):
        raise SystemExit('\nConfiguration cancelled; unsaved changes were not written.')

#!/usr/bin/env python3
"""Read selected RAMFS test artifacts through QEMU's read-only memory monitor.

This is a host-side diagnostic/export helper, not a guest filesystem backend.
The paused guest remains the source of all bytes; DWARF from its exact boot
kernel supplies structure offsets. No kernel code or guest memory is changed.
"""
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

from test_runtime import ROOT, WORK


def boot_kernel(iso):
    iso = Path(iso).resolve()
    with iso.open('rb') as source:
        digest = hashlib.file_digest(source, 'sha256').hexdigest()
    directory = WORK / 'boot-kernels' / digest
    directory.mkdir(parents=True, exist_ok=True)
    elf = directory / 'kernel.elf'
    if not elf.exists():
        result = subprocess.run(['xorriso', '-osirrox', 'on', '-indev', str(iso),
                                 '-extract', '/boot/kernel.elf', str(elf)],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if result.returncode:
            raise AssertionError(result.stdout.decode(errors='replace'))
    return elf


class GuestRamFS:
    def __init__(self, vm, elf, directory):
        if vm.qmp('query-status')['running']:
            raise AssertionError('Pause the guest before inspecting RAMFS')
        self.vm = vm
        self.directory = Path(directory).resolve()
        self.directory.mkdir(parents=True, exist_ok=True)
        expressions = {
            'root_data': '&mounts[0].data',
            'node_bytes': 'sizeof(struct ram_node)',
        }
        for field in ('name', 'mode', 'size', 'pages', 'children', 'next'):
            expressions[field] = '&((struct ram_node *)0)->' + field

        # Older checkpoints store every file in pages and have no boot_data
        # member. Discover optional fields from the checkpoint kernel's type,
        # while retaining strict failures for missing required layout data.
        # The unified boot ABI also works with the EFI entry path. Retain
        # access to older checkpoint kernels that exposed Limine responses.
        symbols = subprocess.check_output(['nm', str(elf)], text=True)
        unified_boot = bool(re.search(r'\bboot_info$', symbols, re.M))
        if unified_boot:
            expressions['hhdm_address'] = '&boot_info.hhdm_offset'
        else:
            expressions['hhdm_response'] = '&hhdm_request.response'
            expressions['hhdm_offset'] = '&((struct limine_hhdm_response *)0)->offset'

        commands = ['gdb', '-nx', '-batch', str(elf), '-ex', 'ptype struct ram_node']
        for key, expression in expressions.items():
            commands += ['-ex', 'printf "' + key + '=%llu\\n", (unsigned long long)(' + expression + ')']
        output = subprocess.check_output(commands, text=True, stderr=subprocess.STDOUT)
        self.offsets = {key: int(value)
                        for key, value in re.findall(r'^(\w+)=(\d+)$', output, re.M)}
        if self.offsets.keys() != expressions.keys() or not 1 <= self.offsets['node_bytes'] <= 4096:
            raise AssertionError('Cannot resolve the exact boot kernel RAMFS layout: ' + output)

        if re.search(r'\bboot_data\b', output):
            output = subprocess.check_output(
                ['gdb', '-nx', '-batch', str(elf), '-ex',
                 'printf "boot_data=%llu\\n", (unsigned long long)(&((struct ram_node *)0)->boot_data)'],
                text=True, stderr=subprocess.STDOUT)
            match = re.fullmatch(r'boot_data=(\d+)\n', output)
            if not match or int(match[1]) + 8 > self.offsets['node_bytes']:
                raise AssertionError(
                    'Cannot resolve the boot kernel RAMFS boot_data offset: ' + output)
            self.offsets['boot_data'] = int(match[1])

        if unified_boot:
            self.hhdm = self.word(self.read(self.offsets['hhdm_address'], 8))
        else:
            response = self.word(self.read(self.offsets['hhdm_response'], 8))
            self.hhdm = self.word(self.read(response + self.offsets['hhdm_offset'], 8))
        self.root = self.word(self.read(self.offsets['root_data'], 8))

    @staticmethod

    def word(data, offset=0):
        return struct.unpack_from('<Q', data, offset)[0]

    def read(self, address, count, physical=False):
        if count < 0 or count > 16 * 1024 * 1024 or address < 0 or address + count > 1 << 64:
            raise AssertionError('Invalid diagnostic read extent')
        path = self.directory / 'memory.bin'
        command = ('pmemsave' if physical else 'memsave') + f' 0x{address:x} {count} ' + json.dumps(str(path))
        result = self.vm.qmp('human-monitor-command', {'command-line': command})
        if result:
            raise AssertionError(result)
        data = path.read_bytes()
        if len(data) != count:
            raise AssertionError('Incomplete QEMU memory export')
        return data

    def node(self, address):
        if address < self.hhdm:
            raise AssertionError('Invalid RAMFS kernel pointer')
        data = self.read(address, self.offsets['node_bytes'])
        name = data[self.offsets['name']:self.offsets['name'] + 256].split(b'\0', 1)[0].decode()
        return dict(name=name, mode=struct.unpack_from('<I', data, self.offsets['mode'])[0],
                    boot_data=self.word(
                        data, self.offsets['boot_data']) if 'boot_data' in self.offsets else 0,
                    **{field: self.word(data, self.offsets[field])
                       for field in ('size', 'pages', 'children', 'next')})

    def find(self, path):
        if not path.startswith('/') or any(part in ('.', '..') for part in path.split('/')):
            raise ValueError('Use an absolute, already normalized RAMFS path')
        current = self.node(self.root)
        for part in filter(None, path.split('/')):
            if current['mode'] & 0o170000 != 0o040000:
                raise NotADirectoryError(path)
            address, seen = current['children'], set()
            while address:
                if address in seen or len(seen) >= 500000:
                    raise AssertionError('Invalid RAMFS sibling chain')
                seen.add(address)
                entry = self.node(address)
                if entry['name'] == part:
                    current = entry
                    break
                address = entry['next']
            else:
                raise FileNotFoundError(path)
        return current

    def export(self, path, destination):
        file = self.find(path)
        if file['mode'] & 0o170000 != 0o100000:
            raise ValueError('Export supports regular RAMFS files only')
        if file['size'] > 256 * 1024 * 1024:
            raise AssertionError('RAMFS file exceeds its real size limit')

        count = (file['size'] + 4095) // 4096
        pages = self.read(file['pages'], count * 8) if count and not file['boot_data'] else b''
        pages = struct.unpack('<' + 'Q' * count, pages) if pages else ()
        digest = hashlib.sha256()
        remaining, index = file['size'], 0
        destination = Path(destination)
        temporary = destination.with_name(destination.name + '.pending')
        try:
            with temporary.open('wb') as output:
                while remaining:
                    if file['boot_data']:
                        length = min(remaining, 16 * 1024 * 1024)
                        data = self.read(file['boot_data'] + file['size'] - remaining, length)
                        output.write(data);
                        digest.update(data);
                        remaining -= length
                        continue

                    first, length = pages[index], 4096
                    if not first or first & 4095:
                        raise AssertionError('Invalid RAMFS physical page')
                    index += 1
                    while index < count and pages[index] == first + length and length < 16 * 1024 * 1024:
                        index += 1
                        length += 4096
                    length = min(length, remaining)
                    data = self.read(first, length, physical=True)
                    output.write(data);
                    digest.update(data);
                    remaining -= length
            temporary.replace(destination)
        finally:
            temporary.unlink(missing_ok=True)
        return digest.hexdigest()

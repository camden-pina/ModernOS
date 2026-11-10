#!/usr/bin/env python3
"""Produce source/version-bound Code OSS bytecode caches in an isolated guest."""
from build_state import atomic_write
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import time
import tarfile
import traceback
import test_runtime as runtime
import test_desktop as desktop
import guest_ramfs

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--iso', type=Path, required=True)
p.add_argument('--output-root', type=Path, required=True)
p.add_argument('--measure-only', action='store_true')
a = p.parse_args()
iso, out = a.iso.resolve(), a.output_root.resolve()
out.mkdir(parents=True, exist_ok=False)
runtime.WORK = desktop.WORK = guest_ramfs.WORK = out
manifest = json.loads((iso.parent / 'MANIFEST.json').read_text())
record = dict(state='failed', iso=str(iso), runtime=manifest['canonical_electron_sha256'],
              sources={name: digest for name, digest in manifest['files'].items()
                       if name.startswith('usr/share/codeoss-workbench/')}, first_pixels_ms=[])
vm = None

def raw():
    while vm.selector.select(.002):
        b = os.read(vm.process.stdout.fileno(), 65536)
        if not b: break
        vm.pending.extend(b); vm.log.write(b); vm.log.flush()
    return Path(vm.log.name).read_text(errors='replace')

def wait(marker, start, timeout=90):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if marker in raw()[start:]: return
        time.sleep(.02)
    raise AssertionError('Missing '+marker+'\n'+raw()[-2500:])

with Path(str(iso)+'.lock').open('a') as lock:
    fcntl.flock(lock, fcntl.LOCK_SH | fcntl.LOCK_NB)
    try:
        vm = runtime.VM(2, storage=False, network=False, memory='16G', accel='kvm', iso=iso, label='startup-cache')
        if not a.measure_only:
            fixture = vm.command('vector-io-check')
            assert 'VECTOR IO PASS' in fixture, fixture
            record['vector_io_check'] = fixture
        vm.send('gui --trace --stats\n'); vm.until(rb'GUI_READY:', 60)
        d = desktop.DesktopTest(vm); d.screenshot('00-desktop')
        for launch in range(2):
            start = len(raw()); began = time.monotonic(); d.launch('Code OSS')
            wait('ELECTRON_CLIENT_PIXELS_PRESENTED', start)
            elapsed = (time.monotonic()-began)*1000
            record['first_pixels_ms'].append(elapsed)
            print('PASS launch', launch+1, 'first pixels', round(elapsed), 'ms', flush=True)
            # Let the delayed cache flush finish, with serial output drained.
            end = time.monotonic()+12
            while time.monotonic() < end: raw(); time.sleep(.1)
            start = len(raw()); d.key('f4', 'alt')
            wait('C1_RUNTIME_DESCENDANTS_REAPED', start)
            time.sleep(.4)
        d.exit()
        if a.measure_only:
            record['state'] = 'measured two isolated launches'
            raise SystemExit(0)
        vm.shell_command('/bin/busybox tar -cf /tmp/codeoss-compile.tar -C /home/port-test/.cache/codeoss-compile .')
        vm.shell_command('/bin/busybox tar -cf /tmp/fontconfig.tar -C /home/port-test/.cache/fontconfig .')
        vm.qmp('stop')
        reader = guest_ramfs.GuestRamFS(vm, guest_ramfs.boot_kernel(iso), out / 'memory-export')
        font_cache = out / 'fontconfig.tar'
        reader.export('/tmp/fontconfig.tar', font_cache)
        record['font_cache'] = dict(archive=str(font_cache), archive_sha256=hashlib.sha256(font_cache.read_bytes()).hexdigest(),
            sources={name: digest for name, digest in manifest['files'].items()
                     if name.startswith('usr/share/fonts/coast/') or name == 'etc/fonts/fonts.conf'})
        cache = out / 'codeoss-compile.tar'
        reader.export('/tmp/codeoss-compile.tar', cache)
        with tarfile.open(cache) as archive:
            files = [entry for entry in archive if entry.isfile() and entry.size]
        if not files or not any(entry.size > 1024 for entry in files):
            raise AssertionError('The guest did not persist usable module bytecode')
        record['cache_files'] = {entry.name: entry.size for entry in files}
        record.update(state='generated in isolated guest', archive=str(cache),
                      archive_sha256=hashlib.sha256(cache.read_bytes()).hexdigest())
    except SystemExit:
        raise
    except BaseException as error:
        record['error'] = repr(error); (out/'exception.txt').write_text(traceback.format_exc()); raise
    finally:
        if vm: vm.close()
        atomic_write(out/'CACHE.json', json.dumps(record, indent=2)+'\n')

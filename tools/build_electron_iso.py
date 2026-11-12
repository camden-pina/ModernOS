#!/usr/bin/env python3
"""Build this Coast source tree with the preserved compiled Electron runtime.

The artifact directory supplies the original ISO/runtime modules, Electron build
and pinned Wayland/glibc build dependencies. Kernel and display adapter sources
come from this main development tree. The original R2 image is preserved.

After booting to the desktop, open a Terminal (Super+T), and
run `electron-wayland-demo --electron`. This runs the real Electron test app;
the full Code OSS workbench is not yet packaged or validated.
"""
import argparse
import fcntl
from pathlib import Path
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__,formatter_class=argparse.RawDescriptionHelpFormatter)
p.add_argument('--artifacts',type=Path,default=Path.home()/'Documents/Codex/2026-10-24/task-2/codeoss-partial-page-r2')
p.add_argument('--iso',type=Path,default=ROOT/'build/Coast-electron.iso')
p.add_argument('--mode',choices=['on','verify','off'],default='on')
p.add_argument('--selftest',action='store_true')
p.add_argument('--namespace-selftest',action='store_true')
a=p.parse_args();artifacts=a.artifacts.resolve()
for name in ['test/build_bridge_local.py','package_local_runtime.py','rebuild_local.py']:
    if not (artifacts/name).is_file():p.error('Missing preserved build dependency: '+str(artifacts/name))
def run(script,*options):
    subprocess.run([sys.executable,'-B',str(artifacts/script),*map(str,options)],check=True)
with (artifacts/'test/bridge-build.lock').open('a') as build_lock:
    fcntl.flock(build_lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
    run('test/build_bridge_local.py','--source',ROOT/'userland/electron-wayland','--kernel-source',ROOT)
    run('package_local_runtime.py')
    options=['--source',ROOT,'--iso',a.iso.resolve(),'--mode',a.mode]
    if a.selftest:options+=['--selftest']
    if a.namespace_selftest:options+=['--namespace-selftest']
    run('rebuild_local.py',*options)
print('ISO ready:',a.iso.resolve())
print('Full Code OSS qualification uses 16 GiB RAM and 2 vCPUs; runtime testing is still required.')

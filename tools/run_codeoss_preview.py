#!/usr/bin/env python3
"""Open an isolated, visible Code OSS development VM with native pixels."""
from pathlib import Path
import argparse,fcntl,json,os,time
import test_runtime as runtime
import test_desktop as desktop

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--iso',required=True,type=Path)
parser.add_argument('--output-root',required=True,type=Path)
args=parser.parse_args()
output=args.output_root.resolve();output.mkdir(parents=True,exist_ok=False)
runtime.WORK=desktop.WORK=output
image_lock=Path(str(args.iso.resolve())+'.lock').open('a');fcntl.flock(image_lock,fcntl.LOCK_SH)
# This process's X11 frontend uses one host pixel per guest pixel. These
# settings do not change the host desktop or other applications.
os.environ.update(GDK_BACKEND='x11',GDK_SCALE='1',GDK_DPI_SCALE='1')
vm=None
record={'state':'starting','iso':str(args.iso.resolve()),'network':False,'extra_disks':False,'preview':True}
def save(): (output/'RUN.json').write_text(json.dumps(record,indent=2)+'\n')
save()
try:
    vm=runtime.VM(2,storage=False,network=False,memory='12G',accel='kvm',iso=args.iso,
        label='codeoss-preview',display='gtk,zoom-to-fit=off',
        extra_args=['-name','Coast Code OSS - Development Preview','-full-screen'])
    record.update(qemu_pid=vm.process.pid,framebuffer={'width':2560,'height':1600},host_pixel_scale=1);save()
    vm.command('mkdir -p /home/user/.config')
    vm.shell_command("printf 'scale=3\\nterminal_shell=1\\n' > /home/user/.config/desktop.ini")
    vm.send('gui --trace\n');vm.until(rb'GUI_READY:',60)
    display=desktop.DesktopTest(vm);display.screenshot('00-desktop')
    assert (display.width,display.height)==(2560,1600)
    display.key('t','meta_l');vm.until(rb'TERMINAL_MODE[^\n]*raw',30)
    vm.send('codeoss-wayland --codeoss\n')
    vm.until(rb'ELECTRON_CLIENT_PIXELS_PRESENTED',90)
    vm.until(rb'CODEOSS_DOM[^\n]*"ready":"complete"[^\n]*"fonts":"loaded"',90)
    display.screenshot('01-codeoss-preview')
    record['state']='running; user controls the preview';save()
    print('Code OSS preview ready. Ctrl+Alt+F toggles full screen; Ctrl+Alt+G releases input.',flush=True)
    while vm.process.poll() is None:
        if vm.selector.select(.1):
            data=os.read(vm.process.stdout.fileno(),65536)
            if data:vm.log.write(data);vm.log.flush()
        else:time.sleep(.02)
    record.update(state='closed',qemu_exit=vm.process.returncode)
except BaseException as error:
    record.update(state='failed',error=repr(error));raise
finally:
    if vm:vm.close()
    save()

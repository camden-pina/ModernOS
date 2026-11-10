#!/usr/bin/env python3
"""Verify scaled dark Coast menus, native app actions and compact window bounds.
Only disposable guest RAM is written; no disks or network are attached.
"""
from pathlib import Path
import sys,json,re,time,argparse,traceback,hashlib
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'));import test_runtime as rt
p=argparse.ArgumentParser();p.add_argument('--iso',type=Path,required=True);p.add_argument('--output-root',type=Path,default=ROOT/'build/coast-desktop-tests');p.add_argument('--compact',action='store_true');p.add_argument('--label',required=True);a=p.parse_args()
OUT=a.output_root.resolve()/a.label;OUT.mkdir(parents=True,exist_ok=False);rt.WORK=OUT
import test_desktop as desktop;desktop.WORK=OUT
from test_desktop import DesktopTest
iso=a.iso.resolve()
def digest(p):
 with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def raw(vm):vm.log.flush();return Path(vm.log.name).read_text(errors='replace')
def menu(d,letter,name,down=0,action=None,detail=''):
 d.key(letter,'alt');d.expect('MENU',name)
 for _ in range(down):d.key('down')
 d.key('ret')
 if action:return d.expect(action,detail)
r={'iso':str(iso),'iso_sha256':digest(iso),'cpus':1 if a.compact else 4,'dark':True,'scale':200 if a.compact else 150,'cases':[],'result':'failed'};vm=None
try:
 vm=rt.VM(r['cpus'],storage=False,iso=iso,label=a.label,network=False,accel='tcg');r['serial_log']=str(vm.log.name)
 vm.command('mkdir -p /home/user/.config')
 scale=3 if a.compact else 2
 vm.shell_command("printf 'scale="+str(scale)+"\\ndark=1\\nworkspaces=3\\n' > /home/user/.config/desktop.ini")
 vm.send('gui --trace --stats\n');vm.until(rb'GUI_READY: Coast desktop',60)
 d=DesktopTest(vm);d.screenshot('01-dark-desktop');g=d.geometry(1)
 top=28*r['scale']//100;bottom=76*r['scale']//100
 assert g['body']==40*r['scale']//100 and g['y']>=top and g['y']+g['height']<=d.height-bottom,g
 r['cases'].append('Dark scaled desktop boots with bounded menu/Dock work area')
 if a.compact:
  assert (d.width,d.height)==(640,480)
  diskid=d.launch('Disks');g=d.geometry(diskid);d.screenshot('02-empty-disks')
  assert 'count=0' in raw(vm)
  d.key('end');d.screenshot('03-empty-disks-operations');d.key('home');d.key('f5');d.expect('DISKS_INVENTORY')
  d.key('m','ctrl','alt');d.expect('MENU','File');d.screenshot('04-compact-global-menu');d.key('esc')
  d.key('f1');d.expect('MODAL');d.key('pgdn');d.expect('MODAL_SCROLL');d.screenshot('05-compact-help');d.key('esc')
  settings=d.launch('Settings');menu(d,'v','View',2,'SETTINGS_SECTION','Display and scaling');d.key('ret');d.expect('SCALE')
  menu(d,'v','View',2,'SETTINGS_SECTION','Display and scaling')
  for _ in range(3):d.key('right')
  d.key('ret');d.expect('SCALE')
  for window in (1,diskid,settings):
   live=d.geometry(window);assert live['scale']==200 and live['y']>=56 and live['y']+live['height']<=328,live
  d.screenshot('06-live-scale200');d.key('f4','alt');d.expect('CLOSE')
  r['cases'].append('Live200-to100-to200% scaling keeps all three windows inside the compact work area')
  d.key('spc','meta_l');d.expect('LAUNCHER');d.key('end');d.screenshot('07-compact-launcher')
  # The last entry is the real Return to console command.
  d.key('ret');vm.until(rb'gui: returned to terminal\.[\s\S]*os:[^\n]*\$ $',30)
  r['cases'].append('640x480 at200%: empty inventory, refresh, scrollable operations, keyboard menus/help, full launcher and console return')
 else:
  editor=d.launch('Text Editor');vm.send('write /tmp/coast-terminal-proof COAST_MENU_ROUNDTRIP')
  d.key('s','ctrl','shift');d.expect('MODAL');vm.send('/tmp/coast-editor-proof.txt\n');d.expect('SAVE','coast-editor-proof')
  menu(d,'e','Edit',0);menu(d,'e','Edit',1,'CLIPBOARD_COPY','Editor')
  d.key('t','meta_l');d.expect('OPEN','Terminal');d.key('m','ctrl','alt');d.expect('MENU','File');d.key('right');d.expect('MENU','Edit');d.key('ret');d.expect('CLIPBOARD_PASTE','Terminal command field')
  d.key('ret');time.sleep(.3);d.screenshot('02-terminal-menu-paste')
  d.key('spc','alt');d.key('ret');d.expect('MINIMIZE');d.key('w','meta_l');d.expect('WINDOW_LIST');d.key('ret');d.expect('FOCUS')
  r['cases'].append('Real global Editor save/copy and Terminal menu paste dispatch; minimize/window-list restoration')
  disks=d.launch('Disks');d.screenshot('03-dark-disks');d.key('f4','alt');d.expect('CLOSE')
  d.exit();vm.command('cat /tmp/coast-terminal-proof','COAST_MENU_ROUNDTRIP');vm.command('cat /tmp/coast-editor-proof.txt','write /tmp/coast-terminal-proof COAST_MENU_ROUNDTRIP')
  r['cases'].append('Menu paste executed a real native-shell command; saved Editor bytes verified after desktop exit')
 vm.command('echo COAST_MODES_OK','COAST_MODES_OK\n');r['result']='passed'
except BaseException as e:r['error']=repr(e);(OUT/'exception.txt').write_text(traceback.format_exc());raise
finally:
 if vm:vm.close()
 r['iso_unchanged']=digest(iso)==r['iso_sha256'];(OUT/'report.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r,indent=2),flush=True)

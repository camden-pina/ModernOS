#!/usr/bin/env python3
"""Verify Coast desktop input/damage and read-only disk inventory on an owned disposable image.
Uses the existing VM/USB helpers; no host block devices or network are attached.
"""
from pathlib import Path
import sys,json,time,re,hashlib,traceback,argparse
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import test_runtime as runtime
parser=argparse.ArgumentParser();parser.add_argument('--iso',type=Path,required=True);parser.add_argument('--output-root',type=Path,default=ROOT/'build/coast-desktop-tests');parser.add_argument('--label',required=True);a=parser.parse_args()
OUT=a.output_root.resolve()/a.label;OUT.mkdir(parents=True,exist_ok=False);runtime.WORK=OUT
import test_desktop as desktop
desktop.WORK=OUT
from test_desktop import DesktopTest
iso=a.iso.resolve()
def digest(p):
 with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
volume=OUT/'disposable-fat.volume'
with volume.open('wb') as f:f.truncate(64*1024*1024)
runtime.run('mkfs.fat','-F','32','-n','COAST-QA',volume)
disk=OUT/'disposable-gpt.img';runtime.gpt_image(volume,disk,'ebd0a0a2-b9e5-4433-87c0-68b6b72699c7')
before=digest(disk);report={'iso':str(iso),'iso_sha256':digest(iso),'disposable_disk':str(disk),'disk_sha256_before':before,'cases':[],'result':'failed'};vm=None
try:
 vm=runtime.VM(4,storage=False,iso=iso,label=a.label,network=False,accel='tcg',extra_args=['-drive',f'file={disk},format=raw,if=none,id=coastqa,readonly=on','-device','nvme,drive=coastqa,serial=COAST-QA'])
 report['serial_log']=str(vm.log.name)
 listing=vm.command('lsblk');(OUT/'guest-lsblk.txt').write_text(listing);assert '67108864' in listing and 'nvme0n1p1' in listing,listing
 vm.send('gui --trace --stats\n');vm.until(rb'GUI_READY: Coast desktop',60)
 d=DesktopTest(vm);d.screenshot('01-desktop')
 report['cases'].append('GUI boots; actual device/partition byte capacity checked through guest lsblk')
 diskid=d.launch('Disks');None
 d.screenshot('02-disks-device');g=d.geometry(diskid);assert g['body']==40 and g['y']>=28 and g['y']+g['height']<=d.height-76,g
 d.key('down');d.expect('DISKS_SELECT','nvme0n1p1');d.screenshot('03-disks-partition')
 d.key('f5');d.expect('DISKS_INVENTORY')
 d.click(g['x']+g['width']-64,g['y']+20);d.expect('DISKS_INVENTORY');d.screenshot('04-disks-refreshed')
 d.key('pgdn');d.key('pgdn');d.screenshot('05-disks-operations');d.key('home')
 report['cases'].append('Disks launch, real partition selection, F5 refresh and scrollable unavailable operations')
 # Real global File menu, followed by actual window controls.
 d.click(125,13);d.expect('MENU','File');d.screenshot('06-global-menu');d.key('esc')
 d.click(g['x']+41,g['y']+20);d.expect('MINIMIZE')
 # Dock item index4 is Disks (launcher, Files, Browser, Terminal, Disks...).
 step=min(54,(d.width-28)//8);left=(d.width-(step*8+12))//2
 d.click(left+6+4*step+step//2,d.height-40);d.expect('FOCUS','Disks');d.screenshot('07-dock-restore')
 d.click(g['x']+62,g['y']+20);d.expect('MAXIMIZE');full=d.geometry(diskid);assert full['y']==28 and full['height']==d.height-104,full
 d.screenshot('08-maximized');d.click(62,48);d.expect('RESTORE')
 report['cases'].append('Global menu dispatch, left minimize control, Dock restoration and work-area maximize/restore')
 # Drag away and back; compare pixels with a stationary pointer, excluding only the clock.
 g=d.geometry(diskid);px=g['x']+90;py=g['y']+20
 d.move(d.width-10,d.height-90);first=d.screenshot('09-before-drag')
 d.move(px,py);d.button(True);d.move(px+40,py+20,.25);d.button(False);d.expect('GEOMETRY')
 d.move(px+40,py+20);d.button(True);d.move(px,py,.25);d.button(False);d.expect('GEOMETRY')
 d.move(d.width-10,d.height-90);last=d.screenshot('10-after-drag')
 assert first[d.width*28*3:]==last[d.width*28*3:],'Window drag left stale pixels below menu bar'
 report['cases'].append('Real USB window drag returns to exact original pixels below menu bar')
 d.click(g['x']+20,g['y']+20);d.expect('CLOSE');d.screenshot('11-close-control')
 report['cases'].append('Left close, minimize and maximize controls dispatch real window actions')
 d.exit();vm.command('echo COAST_CONSOLE_OK','COAST_CONSOLE_OK\n')
 report['cases'].append('Desktop exits cleanly and console remains usable')
 report['result']='passed'
except BaseException as e:
 report['error']=repr(e);(OUT/'exception.txt').write_text(traceback.format_exc());raise
finally:
 if vm:vm.close()
 report['disk_sha256_after']=digest(disk);report['disk_unchanged']=report['disk_sha256_after']==before
 report['iso_unchanged']=digest(iso)==report['iso_sha256']
 if not(report['disk_unchanged'] and report['iso_unchanged']):report['result']='failed'
 (OUT/'report.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2),flush=True)

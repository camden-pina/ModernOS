#!/usr/bin/env python3
"""Verify actual layered glass, native input, repaint equivalence and measured drag cost.
All optional disks are new disposable images attached read-only; no host raw disks.
"""
from pathlib import Path
import sys,json,time,re,argparse,traceback,hashlib
R=Path(__file__).resolve().parents[1];W=R/'build/coast-clarity-tests';sys.path.insert(0,str(R/'tools'))
import test_runtime as rt
p=argparse.ArgumentParser();p.add_argument('--iso',type=Path,required=True);p.add_argument('--label',required=True);p.add_argument('--output-root',type=Path,default=W/'qa');p.add_argument('--cpus',type=int,default=4);p.add_argument('--scale',type=int,choices=[100,125,150,200],default=100);p.add_argument('--dark',action='store_true');p.add_argument('--flat',action='store_true');p.add_argument('--dynamic',action='store_true');p.add_argument('--disk',action='store_true');p.add_argument('--live-scale',action='store_true');p.add_argument('--layout',action='store_true');p.add_argument('--image-defaults',action='store_true');a=p.parse_args()
OUT=a.output_root.resolve()/a.label;OUT.mkdir(parents=True,exist_ok=False);rt.WORK=OUT
import test_desktop as desk;desk.WORK=OUT
from test_desktop import DesktopTest
initial_iso_hash=hashlib.sha256(a.iso.read_bytes()).hexdigest()
r={'iso_sha256':initial_iso_hash,'result':'failed','iso':str(a.iso.resolve()),'cpus':a.cpus,'scale':a.scale,'dark':a.dark,'glass':not a.flat,'cases':[]};vm=None
try:
 extra=[]
 if a.disk:
  volume=OUT/'disposable-fat.volume'
  with volume.open('wb') as f:f.truncate(64*1024*1024)
  rt.run('mkfs.fat','-F','32','-n','GLASS-QA',volume)
  image=OUT/'disposable-gpt.img';rt.gpt_image(volume,image,'ebd0a0a2-b9e5-4433-87c0-68b6b72699c7')
  r['disk_before']=hashlib.sha256(image.read_bytes()).hexdigest()
  extra=['-drive',f'file={image},format=raw,if=none,id=glassqa,readonly=on','-device','nvme,drive=glassqa,serial=GLASS-QA']
 vm=rt.VM(a.cpus,storage=False,iso=a.iso,label=a.label,network=False,accel='tcg',extra_args=extra);r['serial_log']=str(vm.log.name)
 # Ordinary real guest fixtures create useful text/icon detail behind glass.
 desk.fixtures(vm)
 if not a.image_defaults:
  vm.command('mkdir -p /home/user/.config')
  vm.shell_command("printf 'scale="+str([100,125,150,200].index(a.scale))+"\\ndark="+str(int(a.dark))+"\\n' > /home/user/.config/desktop.ini")
 r['display_info']=vm.shell_command('gui --info')
 vm.send('gui --trace --stats'+(' --no-glass' if a.flat else '')+'\n');vm.until(rb'GUI_READY: Coast desktop',60)
 d=DesktopTest(vm);d.screenshot('01-compact-desktop');r['framebuffer']={'width':d.width,'height':d.height,'logical_width':d.width*100/a.scale,'logical_height':d.height*100/a.scale};g=d.geometry(1);assert g['body']==round(32*a.scale/100) and g['scale']==a.scale,g
 disk=d.launch('Disks');g=d.geometry(disk);d.screenshot('02-overlapping-glass')
 d.move(d.width-10,d.height-90);before=d.screenshot('03-before-force')
 d.key('f12','ctrl','alt');d.expect('GLASS_RECOMPOSE');after=d.screenshot('04-after-force')
 top=round(22*a.scale/100)
 assert before[d.width*top*3:]==after[d.width*top*3:],'Cached glass differs from complete lower-layer/halo recomposition'
 r['cases'].append('Actual overlapping window glass is pixel-identical to forced full material recomposition')
 d.key('f','alt');d.expect('MENU','File');menu_before=d.screenshot('05-menu-glass')
 d.key('f12','ctrl','alt');d.expect('GLASS_RECOMPOSE');menu_after=d.screenshot('05b-menu-force')
 assert menu_before[d.width*top*3:]==menu_after[d.width*top*3:],'Popup material differs from full recomposition'
 d.key('esc')
 d.key('spc','meta_l');d.expect('LAUNCHER');launcher_before=d.screenshot('06-launcher-glass')
 d.key('f12','ctrl','alt');d.expect('GLASS_RECOMPOSE');launcher_after=d.screenshot('06b-launcher-force')
 assert launcher_before[d.width*top*3:]==launcher_after[d.width*top*3:],'Launcher material differs from fresh lower layers'
 d.key('esc')
 plain=d.screenshot('06c-before-modal');d.key('f1');d.expect('MODAL');d.screenshot('06d-modal-glass');d.key('esc')
 restored=d.screenshot('06e-after-modal');assert plain[d.width*top*3:]==restored[d.width*top*3:],'Modal dismissal left stale glass'
 d.move(d.width//2,d.height-round(30*a.scale/100));time.sleep(.6);d.screenshot('06f-dock-hover');d.move(d.width-10,d.height-90);time.sleep(.65)
 restored=d.screenshot('06g-dock-restored');assert plain[d.width*top*3:]==restored[d.width*top*3:],'Dock hover failed to clear exactly'
 r['cases'].append('Global menu/launcher match fresh layers; modal dismissal and Dock hover restore exact pixels')
 # Multiple captured drag frames, followed by exact geometry/pixel restoration.
 g=d.geometry(disk);px=g['x']+round(90*a.scale/100);py=g['y']+round(18*a.scale/100)
 d.move(d.width-10,d.height-90);first=d.screenshot('07-drag-before')
 d.move(px,py);d.button(True)
 start=time.monotonic()
 for i in range(30):d.move(px+(i+1)*2,py+((i+1)%10),delay=.012)
 d.button(False);d.expect('GEOMETRY');endg=d.geometry(disk)
 r['drag_wall_seconds']=time.monotonic()-start
 # Start at the actual released pointer, then put the window back in one motion.
 d.move(px+60,py);d.button(True);d.move(px,py,delay=.12);d.button(False);d.expect('GEOMETRY')
 d.move(d.width-10,d.height-90);last=d.screenshot('08-drag-restored')
 # The vertical release position above was py; the final actual position is checked below.
 live=d.geometry(disk)
 if live['x']!=g['x'] or live['y']!=g['y']:
  dx=g['x']-live['x'];dy=g['y']-live['y'];cx=live['x']+round(90*a.scale/100);cy=live['y']+round(18*a.scale/100)
  d.move(cx,cy);d.button(True);d.move(cx+dx,cy+dy,.12);d.button(False);d.expect('GEOMETRY');d.move(d.width-10,d.height-90);last=d.screenshot('08b-drag-restored')
 assert first[d.width*top*3:]==last[d.width*top*3:],'Glass/shadows left stale pixels after drag'
 d.key('f12','ctrl','alt');d.expect('GLASS_RECOMPOSE');fresh=d.screenshot('09-restored-force')
 assert last[d.width*top*3:]==fresh[d.width*top*3:],'Restored glass cache differs from fresh reconstruction'
 r['cases'].append('30 live drag steps with overlapping glass restore exact pixels; cache matches fresh reconstruction')
 if a.dynamic:
  terminal=d.launch('Terminal')
  vm.send("build-sh -c 'sleep 4; echo GLASS_BACKGROUND_CHANGED'\n")
  overlay=d.launch('Disks');g2=d.geometry(overlay);d.move(d.width-10,d.height-90)
  b1=d.screenshot('10-lower-before');time.sleep(4.5);b2=d.screenshot('11-lower-after')
  left=g2['x']+2;right=left+min(round(160*a.scale/100),g2['width']*30//100)-4
  y1=g2['y']+g2['body']+2;y2=g2['y']+g2['height']-4
  changed=0
  for y in range(y1,y2):
   for x in range(left,right):
    i=(y*d.width+x)*3
    if b1[i:i+3]!=b2[i:i+3]:changed+=1
  assert changed>10,'Underlying real Terminal update failed to reach upper glass'
  r['dynamic_backdrop_changed_pixels']=changed
  d.key('f12','ctrl','alt');d.expect('GLASS_RECOMPOSE');b3=d.screenshot('12-lower-fresh')
  assert b2[d.width*top*3:]==b3[d.width*top*3:],'Updated lower content left an incorrect glass cache'
  r['cases'].append('Delayed real Terminal output updates the unfocused lower window and upper glass, pixel-equal to fresh3-layer reconstruction')
 if a.disk:
  d.key('down');d.expect('DISKS_SELECT','nvme0n1p1');d.screenshot('13-real-disk-partition');d.key('f5');d.expect('DISKS_INVENTORY')
  r['cases'].append('Real device/partition inventory remains selectable and refreshable on an attached read-only disposable GPT/FAT image')
 if a.layout:
  assert d.width*100/a.scale>=1280,'Layout pointer case requires a1280px logical desktop'
  for _ in range(3 if a.dynamic else 1):d.key('f4','alt');d.expect('CLOSE')
  g=d.geometry(1);scl=a.scale/100
  d.click(g['x']+round(45*scl),g['y']+g['body']+round(45*scl));d.expect('NAVIGATE','/home/user')
  d.click(g['x']+round(45*scl),g['y']+g['body']+round(111*scl));d.expect('NAVIGATE','/home/user/Projects')
  # Actual fixture notes.md is the fourth file after two folder-card rows.
  x=g['x']+round(300*scl);y=g['y']+g['body']+round((60+25+2*52+25+25+24+3*24+12)*scl)
  d.click(x,y);d.click(x,y);opened=d.expect('OPEN','notes.md');eid=int(re.findall(r'DESKTOP OPEN window=(\d+)',opened)[-1]);d.screenshot('15-native-editor')
  eg=d.geometry(eid);d.click(eg['x']+round(20*scl),eg['y']+round(16*scl));d.expect('CLOSE')
  d.click(g['x']+round(60*scl),g['y']+round(16*scl));d.expect('MAXIMIZE');mg=d.geometry(1)
  assert mg['x']==0 and mg['y']==round(22*scl) and mg['width']==d.width and mg['height']==d.height-round(76*scl),mg
  d.click(mg['x']+round(60*scl),mg['y']+round(16*scl));d.expect('RESTORE');assert d.geometry(1)==g
  d.click(g['x']+round(40*scl),g['y']+round(16*scl));d.expect('MINIMIZE')
  step=min(round(40*scl),(d.width-round(28*scl))//8);dockwidth=step*8+round(12*scl)
  d.click((d.width-dockwidth)//2+round(6*scl)+step+step//2,d.height-round(30*scl));d.expect('FOCUS');d.screenshot('16-controls-and-dock-restored')
  r['cases'].append('Actual close/maximize/restore/minimize controls and Dock restore; real pointer Home/Projects sidebar rows and fourth compact file row open the correct unchanged notes.md document')
 if a.live_scale:
  settings=d.launch('Settings')
  for target in [0,3]:
   d.key('v','alt');d.expect('MENU','View');d.key('down');d.key('down');d.key('ret');d.expect('SETTINGS_SECTION','Display and scaling')
   for _ in range(target):d.key('right')
   d.key('ret');d.expect('SCALE')
   current=100 if target==0 else 200
   for wid in [1,disk,settings]:
    live=d.geometry(wid);assert live['scale']==current and live['body']==round(32*current/100) and live['y']>=round(22*current/100) and live['y']+live['height']<=d.height-round(54*current/100),live
   d.screenshot('14-live-scale'+str(current))
  r['cases'].append('Real Settings menus switch200-to100-to200%, keeping all three windows within the work area')
 d.exit();vm.command('echo GLASS_CONSOLE_OK','GLASS_CONSOLE_OK\n')
 if a.layout:vm.command('cat /home/user/Projects/notes.md','Coast workspace')
 vm.log.flush();text=Path(vm.log.name).read_text(errors='replace')
 m=re.findall(r'GLASS_STATS enabled=(\d+) updates=(\d+) pixels=(\d+) time_us=(\d+) fallbacks=(\d+)',text);assert m
 r['glass_stats']=dict(zip(['enabled','updates','pixels','time_us','fallbacks'],map(int,m[-1])))
 assert r['glass_stats']['enabled']==int(not a.flat) and r['glass_stats']['fallbacks']==0
 t=re.findall(r'GUI_TIMING compose_us=(\d+) upload_us=(\d+) drag_frames=(\d+) drag_us=(\d+) drag_max_us=(\d+) surface_updates=(\d+)',text);assert t
 r['timing']=dict(zip(['compose_us','upload_us','drag_frames','drag_us','drag_max_us','surface_updates'],map(int,t[-1])))
 r['result']='passed'
except BaseException as e:r['error']=repr(e);(OUT/'exception.txt').write_text(traceback.format_exc());raise
finally:
 if vm:vm.close()
 r['iso_unchanged']=hashlib.sha256(a.iso.read_bytes()).hexdigest()==initial_iso_hash
 if not r['iso_unchanged']:r['result']='failed'
 if a.disk:
  r['disk_after']=hashlib.sha256(image.read_bytes()).hexdigest();r['disk_unchanged']=r['disk_after']==r['disk_before']
  if not r['disk_unchanged']:r['result']='failed'
 (OUT/'report.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r,indent=2),flush=True)

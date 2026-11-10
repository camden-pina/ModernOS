#!/usr/bin/env python3
"""Exercise the actual static Qt 6 Widgets app in an isolated Coast guest."""
import argparse,hashlib,json,os,re,time,traceback
from pathlib import Path
from PIL import Image,ImageChops
from source_tree import digest
import test_runtime as runtime
import test_desktop as desktop
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--iso',type=Path,required=True);p.add_argument('--output-root',type=Path,required=True);p.add_argument('--scale',type=int,choices=[100,125,150,200],default=200);a=p.parse_args()
out=a.output_root.resolve();out.mkdir(parents=True,exist_ok=False);runtime.WORK=desktop.WORK=out
v=d=None;image_sha256=digest(a.iso);report=dict(state='failed',checks=[],iso=str(a.iso.resolve()),iso_sha256=image_sha256,guest_program_execution_on_host=False)
def raw():
 while v.selector.select(.002):
  b=os.read(v.process.stdout.fileno(),65536)
  if not b:break
  v.pending.extend(b);v.log.write(b);v.log.flush()
 return Path(v.log.name).read_text(errors='replace')
def wait(pattern,start=0,timeout=25):
 end=time.monotonic()+timeout
 while time.monotonic()<end:
  s=raw()[start:];assert 'PANIC:' not in s
  m=re.search(pattern,s)
  if m:return m
  time.sleep(.02)
 raise AssertionError('Missing '+pattern+'\n'+raw()[start:][-6000:])
def passed(s):report['checks'].append(s);print('PASS '+s,flush=True)
def type_text(text):
 for c in text:
  raw()
  if c=='\n':d.key('ret')
  elif c==' ':d.key('spc')
  elif c=='.':d.key('dot')
  else:d.key(c.lower(),*(['shift'] if c.isupper() else []))
try:
 v=runtime.VM(2,storage=False,network=False,memory='16G',accel='kvm',iso=a.iso,label='qt6-desktop')
 v.user_command('mkdir -p /home/user/.config')
 index=[100,125,150,200].index(a.scale)
 v.shell_command("printf 'scale="+str(index)+"\\n' > /home/user/.config/desktop.ini",as_user=True)
 v.send('gui --trace\n');v.until(b'GUI_READY:',60);d=desktop.DesktopTest(v);d.screenshot('00-desktop')
 original=d.move
 def move(x,y,delay=.08):
  while abs(x-d.x)>96 or abs(y-d.y)>96:original(d.x+max(-96,min(96,x-d.x)),d.y+max(-96,min(96,y-d.y)),delay=.025);raw()
  original(x,y,delay);raw()
 d.move=move
 start=len(raw());t=time.monotonic();ident=d.launch('Qt 6 Demo');wait(r'QT6_READY version=6.8.3 uid=1000',start);wait('QT6_PRESENT[^\n]*dpr='+re.escape(f'{a.scale/100:.1f}'),start)
 report['launch_to_first_frame_seconds']=round(time.monotonic()-t,3);time.sleep(.3);d.screenshot('01-widgets')
 type_text('Qt6 typed input\n');wait('QT6_ADD text=Qt6 typed input count=4',start);d.screenshot('02-keyboard-input')
 type_text('Mouse action');g=d.geometry(ident)
 d.click(g['x']+1+round(85*a.scale/100),g['y']+g['body']+round(215*a.scale/100))
 wait('QT6_ADD text=Mouse action count=5',start)
 passed(f'Real Qt 6.8.3 Core/Gui/Widgets starts through Spotlight at UID 1000, paints sharp {a.scale/100:g}x text and accepts USB input through its native event queue')
 g=d.geometry(ident);d.move(g['x']+g['width']-2,g['y']+g['height']-2);d.screenshot('03-before-resize')
 box=(g['x']+60,g['y']+g['body']+25,g['x']+500,g['y']+g['body']+120)
 with Image.open(out/'03-before-resize.png') as im:reference=im.crop(box).copy()
 start=len(raw());d.button(True);x,y=d.x,d.y
 for i in range(1,25):original(x-round(320*i/24*a.scale/200),y-round(180*i/24*a.scale/200),delay=.035);raw()
 d.button(False);wait('QT6_CONFIGURE',start);time.sleep(.4);d.screenshot('04-after-resize')
 with Image.open(out/'04-after-resize.png') as im:assert ImageChops.difference(reference,im.crop(box)).getbbox() is None,'Qt text changed size or position during resize'
 d.key('up','meta_l');time.sleep(.4);d.screenshot('05-maximized');wait('QT6_CONFIGURE[^\n]*states=1',start)
 passed('Native resize and maximize reflow the Qt layout while the title pixels retain their size')
 start=len(raw());d.key('tab','ctrl');time.sleep(.2);d.key('a','ctrl');type_text('Qt6 document saved through the guest filesystem.\n');time.sleep(.2);d.key('z','ctrl');d.key('z','ctrl','shift');d.key('s','alt');wait('QT6_SAVE success=1 path=/home/user/Documents/Qt6-notes.txt',start);d.screenshot('06-saved-document')
 start=len(raw());d.key('f4','alt');wait('DESKTOP CLOSE window='+str(ident),start);wait(r'DESKTOP GAME_EXIT pid=\d+ status=0 reaped=1',start);d.exit()
 saved=v.user_command('cat /home/user/Documents/Qt6-notes.txt');assert '\nQt6 document saved through the guest filesystem.\n' in saved,saved
 passed('Qt tab navigation, editing, undo/redo and Save write the expected bytes into the active profile home; the process exits cleanly')
 report['state']='passed'
except BaseException as e:
 report['error']=repr(e);(out/'exception.txt').write_text(traceback.format_exc())
 if d:
  try:d.screenshot('failure')
  except Exception:pass
 raise
finally:
 if v:v.close()
 report['owned_vm_stopped']=bool(v and v.process.poll() is not None)
 report['image_unchanged']=digest(a.iso)==image_sha256
 (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')

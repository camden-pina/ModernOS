"""Check actual Code OSS cursor artwork and exposed resize pixels in one isolated guest."""
import argparse, fcntl, hashlib, json, os, re, struct, time, traceback
from pathlib import Path
from PIL import Image
import test_runtime as runtime
import test_desktop as desktop
from build_cursor_theme import build as build_theme
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--iso',type=Path,required=True);p.add_argument('--output-root',type=Path,required=True);a=p.parse_args()
iso,out=a.iso.resolve(),a.output_root.resolve();out.mkdir(parents=True,exist_ok=False)
runtime.WORK=desktop.WORK=out
(out/'HARNESS.py').write_bytes(Path(__file__).read_bytes())
report=dict(state='failed',iso=str(iso),checks=[],resize_edges=[]);vm=d=None
def raw():
 while vm.selector.select(.002):
  b=os.read(vm.process.stdout.fileno(),65536)
  if not b:break
  vm.pending.extend(b);vm.log.write(b);vm.log.flush()
 return Path(vm.log.name).read_text(errors='replace')
def wait(pattern,start=0,timeout=45):
 end=time.monotonic()+timeout
 while time.monotonic()<end:
  source=raw()[start:];assert 'PANIC:' not in source
  m=re.search(pattern,source)
  if m:return m
  time.sleep(.02)
 raise AssertionError('Missing '+pattern+'\n'+raw()[start:][-2500:])
def passed(s):report['checks'].append(s);print('PASS '+s,flush=True)
def recompose():
 start=len(raw());d.key('f12','ctrl','alt');m=wait(r'DESKTOP RECOMPOSE_CHECK error=0 pixels=\d+ mismatches=(\d+)',start);assert m[1]=='0',m[0]
def theme_cursor(data):
 _,_,position=struct.unpack_from('<III',data,28)
 header=struct.unpack_from('<9I',data,position);w,h,hx,hy=header[4:8]
 pixels=struct.unpack_from('<'+str(w*h)+'I',data,position+36)
 return w,h,hx,hy,pixels
with Path(str(iso)+'.lock').open('a') as lock:
 fcntl.flock(lock,fcntl.LOCK_SH|fcntl.LOCK_NB)
 try:
  assets=build_theme(out/'theme-reference')
  vm=runtime.VM(2,storage=False,network=False,memory='16G',accel='kvm',iso=iso,label='resize-cursor')
  result=vm.command('vector-io-check');assert 'VECTOR IO PASS' in result and 'CURSOR SHM PASS' in result
  vm.command('mkdir -p /home/user/.config');vm.shell_command("printf 'scale=3\\nterminal_shell=1\\n' > /home/user/.config/desktop.ini")
  vm.send("build-sh -c 'COAST_INPUT_TRACE=1 exec /bin/gui --trace --stats'\n");vm.until(rb'GUI_READY:',60)
  d=desktop.DesktopTest(vm);d.screenshot('00-desktop')
  original_move=d.move
  def move(x,y,delay=.08):
   while abs(x-d.x)>96 or abs(y-d.y)>96:
    original_move(d.x+max(-96,min(96,x-d.x)),d.y+max(-96,min(96,y-d.y)),delay=.025);raw()
   original_move(x,y,delay=max(.04,delay));raw()
  d.move=move
  ident=d.launch('Code OSS');wait('ELECTRON_CLIENT_PIXELS_PRESENTED');time.sleep(5);raw();g=d.geometry(ident)
  d.key('n','ctrl');time.sleep(.7)
  for name,x,y in [('text',g['x']+1200,g['y']+600),('col-resize',g['x']+695,g['y']+600),('pointer',g['x']+20,g['y']+200)]:
   d.move(x,y,delay=.6);raw()
   w,h,hx,hy,pixels=theme_cursor(assets['usr/share/icons/Coast/cursors/'+name])
   wait(r'DESKTOP NATIVE_CURSOR window='+str(ident)+r' mode=1 width='+str(w)+r' height='+str(h)+r' hotspot='+str(hx)+','+str(hy))
   time.sleep(.3);d.screenshot('01-cursor-'+name)
   with Image.open(out/('01-cursor-'+name+'.png')) as im:
    matches=total=0
    for cy in range(h):
     for cx in range(w):
      pixel=pixels[cy*w+cx]
      if pixel>>24!=255 or (pixel&255)<180:continue
      total+=1;actual=im.getpixel((x-hx+cx,y-hy+cy));expected=((pixel>>16)&255,(pixel>>8)&255,pixel&255)
      matches+=actual==expected
    assert total>=10 and matches/total>.94,(name,matches,total)
   report.setdefault('cursor_matches',{})[name]=dict(matching_opaque_pixels=matches,total_opaque_pixels=total)
  passed('Real Electron text, column-resize and hand cursors match the generated 2x artwork at their hotspots')
  d.move(g['x']+g['width']-50,g['y']+20);d.screenshot('01-font-reference')
  with Image.open(out/'01-font-reference.png') as im:
   reference=im.crop((g['x']+10,g['y']+g['body']+80,g['x']+270,g['y']+g['body']+260)).copy()
  border_samples=[]
  for iteration,(dx,dy) in enumerate([(-360,-180),(360,180)]):
   raw();g=d.geometry(ident);x,y=g['x']+g['width']-2,g['y']+g['height']-2
   d.move(x,y);start=len(raw());d.button(True)
   press=wait(r'DESKTOP POINTER position=\((\d+),(\d+)\) buttons=1 pressed=1',start);x,y=d.x,d.y=int(press[1]),int(press[2])
   for i in range(1,49):
    target_x=x+round(dx*i/48);target_y=y+round(dy*i/48);original_move(target_x,target_y,delay=.025);raw()
    shown=d.geometry(ident)
    expected=(g['width']+target_x-x,g['height']+target_y-y)
    border_samples.append(dict(width=shown['width'],height=shown['height'],expected=expected))
    if i%12==0:
     name=f'02-resize-{iteration}-{i}';raw();shown=d.geometry(ident);d.screenshot(name)
     width=shown['width']-2;height=shown['height']-shown['body']-1
     bx,by=shown['x']+1,shown['y']+shown['body']
     with Image.open(out/(name+'.png')) as im:
      areas=[im.crop((bx+width-10,by+10,bx+width-2,by+height-10)),im.crop((bx+10,by+height-10,bx+width-10,by+height-2))]
      pixels=[pixel for area in areas for pixel in area.get_flattened_data()]
      blank=sum(pixel in [(17,22,27),(24,32,42)] for pixel in pixels)
      ratio=blank/len(pixels);report['resize_edges'].append(dict(sample=name,blank_ratio=ratio))
      assert ratio<.005,(name,ratio)
      current=im.crop((shown['x']+10,shown['y']+shown['body']+80,shown['x']+270,shown['y']+shown['body']+260))
      same=sum(a==b for a,b in zip(current.get_flattened_data(),reference.get_flattened_data()))/(260*180)
      report['resize_edges'][-1]['fixed_ui_pixel_match']=same
      assert same>.985,('Client UI changed scale',name,same)
     if i==24:recompose()
   d.button(False);time.sleep(.7);raw()
  recompose();d.screenshot('03-settled')
  views=re.findall(r'DESKTOP GAME_VIEW window='+str(ident)+r'[^\n]*width=(\d+) height=(\d+) source=(\d+)x(\d+) native=1 unscaled=1',raw())
  assert len(views)>=6 and all(w==sw and h==sh for w,h,sw,sh in views),views
  report['native_unscaled_views']=views
  report['border_samples']=border_samples
  unique=sum((s['width'],s['height'])!=(border_samples[i-1]['width'],border_samples[i-1]['height']) for i,s in enumerate(border_samples) if i)
  report['distinct_border_updates']=unique
  assert unique>=80,('Border stalls on client frames',unique)
  error=max(max(abs(s['width']-s['expected'][0]),abs(s['height']-s['expected'][1])) for s in border_samples)
  report['maximum_border_error_px']=error
  assert error<=16,('Border trails pointer',error)
  passed('Eight in-drag shrink/grow screenshots keep fixed UI pixels at the same size and have no exposed blank bands; border tracks input independently of client layout')
  terminal=d.launch('Terminal');raw()
  d.key('z','meta_l');d.key('down');d.key('down');start=len(raw());d.key('ret')
  wait(r'DESKTOP SNAP_ASSIST window='+str(terminal)+r'[^\n]* choose next window',start)
  d.screenshot('04-native-snap-preview');recompose()
  d.click(d.width//2,d.height//2);time.sleep(1);raw()
  wait(r'DESKTOP SNAP window='+str(ident),start)
  current=d.geometry(ident);assert current['width']<=d.width//3+2,current
  d.screenshot('05-native-snapped');recompose()
  raw();other=d.geometry(terminal)
  d.click(other['x']+other['width']//2,other['y']+other['body']//2);d.key('f4','alt');time.sleep(.3);raw()
  current=d.geometry(ident);d.click(current['x']+current['width']//2,current['y']+current['body']//2)
  d.key('down','meta_l');time.sleep(.8);recompose()
  passed('Snap Assist renders the real native Code OSS preview, pointer selection snaps it without zoom and the final peer auto-fills')
  start=len(raw());d.key('f4','alt');wait(r'C1_RUNTIME_WAIT status=0 exit=0 signal=0 timeout=0',start)
  wait(r'DESKTOP GAME_EXIT pid=\d+ status=0 reaped=1',start)
  passed('Cursor pools grow and the editor and bridge close cleanly without protocol or mapping errors')
  d.exit();assert 'USERLAND CHECK PASS' in vm.command('check',timeout=90)
  source=raw();assert 'failed mremap' not in source and 'error in client communication' not in source
  faults=re.findall(r'ELECTRON_USER_FAULT[^\n]*',source);assert all(re.search(r'pending_signal=(?:9|15)\b',f) for f in faults),faults
  report['state']='passed'
 except BaseException as error:
  report['error']=repr(error);(out/'exception.txt').write_text(traceback.format_exc())
  if d:
   try:d.screenshot('failure')
   except Exception:pass
  raise
 finally:
  if vm:raw();vm.close();report['owned_vm_stopped']=vm.process.poll() is not None
  with iso.open('rb') as f:report['iso_sha256']=hashlib.file_digest(f,'sha256').hexdigest()
  (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')

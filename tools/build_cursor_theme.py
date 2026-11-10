"""Build deterministic Xcursor assets from the native desktop's cursor paths."""
import struct
import subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
NAMES={
 0:'left_ptr default arrow top_left_arrow context-menu help',
 1:'text xterm vertical-text',
 2:'pointer hand hand2',
 3:'grab openhand hand1',
 4:'grabbing closedhand all-scroll fleur move',
 5:'ew-resize col-resize sb_h_double_arrow e-resize w-resize left_side right_side',
 6:'ns-resize row-resize sb_v_double_arrow n-resize s-resize top_side bottom_side',
 7:'nwse-resize size_fdiag bd_double_arrow nw-resize se-resize top_left_corner bottom_right_corner',
 8:'nesw-resize size_bdiag fd_double_arrow ne-resize sw-resize top_right_corner bottom_left_corner',
 9:'wait watch progress left_ptr_watch'}
def build(out):
 out=Path(out);out.mkdir(parents=True,exist_ok=False)
 binary=out/'rasterize'
 subprocess.run(['cc','-std=gnu11','-O2','-ffreestanding','-fno-builtin','-ffunction-sections','-fdata-sections',
  '-I'+str(ROOT/'userland/include'),'-I'+str(ROOT/'include'),str(ROOT/'tools/generate_cursor_theme.c'),'-Wl,--gc-sections','-o',str(binary)],check=True)
 files={'usr/share/icons/default/index.theme':b'[Icon Theme]\nName=Coast\nInherits=Coast\n',
        'usr/share/icons/Coast/index.theme':b'[Icon Theme]\nName=Coast\nComment=Native Coast cursor paths\n'}
 for kind,names in NAMES.items():
  chunks=[subprocess.check_output([str(binary),str(kind),str(scale)]) for scale in [100,200]]
  position=16+12*len(chunks);toc=[]
  for size,chunk in zip([24,48],chunks):
   toc.append(struct.pack('<III',0xfffd0002,size,position));position+=len(chunk)
  data=struct.pack('<IIII',0x72756358,16,0x10000,len(chunks))+b''.join(toc)+b''.join(chunks)
  for name in names.split():files['usr/share/icons/Coast/cursors/'+name]=data
 return files

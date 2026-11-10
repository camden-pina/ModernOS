#!/usr/bin/env python3
"""Sanitize the actual synth and parse every packaged Freedoom music lump."""
from pathlib import Path
import os
import struct
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
PORT=ROOT/'userland/doom'

def main():
    with tempfile.TemporaryDirectory(prefix='coast-midi-host-') as name:
        work=Path(name);binary=work/'midi-test'
        subprocess.run(['gcc','-O1','-g','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(PORT),str(PORT/'tests/midi_harness.c'),str(PORT/'coast_midi.c'),'-lm','-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
        data=(ROOT/'build/doom-freedoom/freedoom2.wad').read_bytes()
        count,offset=struct.unpack_from('<II',data,4);scores=[]
        for index in range(count):
            start,size,label=struct.unpack_from('<II8s',data,offset+16*index)
            label=label.rstrip(b'\0').decode('ascii')
            if label.startswith('D_') and data[start:start+4] in (b'MThd',b'MUS\x1a'):
                target=work/(label+'.score');target.write_bytes(data[start:start+size]);scores.append(target)
        assert len(scores)>=32,len(scores)
        for target in scores:
            result=subprocess.run([str(binary),str(target)],check=True,capture_output=True,text=True)
            print(target.stem,result.stdout.splitlines()[0],flush=True)
        print('PASS all',len(scores),'Freedoom score decodes and real synthesized PCM (ASan/UBSan)',flush=True)

if __name__=='__main__':main()

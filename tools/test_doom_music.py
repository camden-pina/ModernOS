#!/usr/bin/env python3
"""Prove Freedoom music-only HDA output, pause/resume and a silent control."""
import argparse
from array import array
import hashlib
import json
from pathlib import Path
import re
import struct
import time
from test_runtime import VM, WORK, assert_success
from test_doom import frame, key

MUSIC = re.compile(r'DOOM_MUSIC playing=(\d+) paused=(\d+) notes=(\d+) frames=(\d+) nonzero=(\d+)')

def score_hash():
    data=(Path(__file__).resolve().parents[1]/'build/doom-freedoom/freedoom2.wad').read_bytes()
    count,offset=struct.unpack_from('<II',data,4)
    for i in range(count):
        start,size,name=struct.unpack_from('<II8s',data,offset+16*i)
        if name.rstrip(b'\0')==b'D_RUNNIN':
            value=2166136261
            for byte in data[start:start+size]:value=((value^byte)*16777619)&0xffffffff
            return f'{value:08x}'
    raise AssertionError('Freedoom MAP01 score missing')

def music_state(vm, paused):
    output=vm.until(fr'DOOM_MUSIC playing=1 paused={paused} notes=[1-9][0-9]* frames=[1-9][0-9]* nonzero=[1-9][0-9]*\r?\n'.encode(),25)
    matches=list(MUSIC.finditer(output));assert matches,output
    return dict(zip(('playing','paused','notes','frames','nonzero'),map(int,matches[-1].groups())))

def capture(path,enabled):
    data=bytearray(path.read_bytes())
    assert data[:4]==b'RIFF' and data[8:16]==b'WAVEfmt ' and data[36:40]==b'data'
    assert struct.unpack_from('<HHI',data,20)==(1,2,48000)
    assert struct.unpack_from('<H',data,34)[0]==16
    pcm=array('h',data[44:]);nonzero=sum(abs(x)>100 for x in pcm)
    if enabled:assert nonzero>4000,(nonzero,len(pcm))
    else:assert not any(pcm),'Music-disabled and SFX-disabled capture was not silent'
    struct.pack_into('<I',data,4,len(data)-8);struct.pack_into('<I',data,40,len(data)-44)
    final=path.with_suffix('.finalized.wav');final.write_bytes(data)
    return {'path':str(final),'frames':len(pcm)//2,'nonzero_samples':nonzero,'sha256':hashlib.sha256(data).hexdigest()}

def exercise(iso,cpus,accel,enabled,label):
    WORK.mkdir(parents=True,exist_ok=True)
    wav=WORK/(label+'.wav');assert not wav.exists(),wav
    result={'iso':str(Path(iso).resolve()),'label':label,'music_enabled':enabled,'result':'failed'}
    vm=None
    try:
        vm=VM(cpus,storage=False,network=False,iso=iso,label=label,accel=accel,
              audio_capture=wav,audio_device='hda',audio_capture_rate=48000)
        vm.send('doom --trace -nosfx -warp 1 -skill 1 -nomonsters'+('' if enabled else ' -nomusic')+'\n')
        vm.until(rb'DOOM_READY',45);frame(vm,state=0,menu=0,timeout=45)
        if enabled:
            result['before_pause']=music_state(vm,0)
            key(vm,'pause',observe=False)
            first=music_state(vm,1);second=music_state(vm,1)
            assert first['frames']==second['frames'],(first,second)
            result['paused']=[first,second]
            key(vm,'pause',observe=False)
            resumed=music_state(vm,0);assert resumed['frames']>second['frames'],(resumed,second)
            result['resumed']=resumed
        else:
            initial=frame(vm)
            deadline=time.monotonic()+15
            while frame(vm)['tic']<initial['tic']+70:
                assert time.monotonic()<deadline
        key(vm,'esc');frame(vm,menu=1)
        key(vm,'q');key(vm,'ret');key(vm,'y',observe=False)
        vm.until(rb'DOOM_EXIT[^\n]*returned to console[\s\S]*os:[^\n]*\$ $',25)
        assert_success(vm.command('echo MUSIC_SHELL_RESTORED','MUSIC_SHELL_RESTORED\n'))
        assert_success(vm.command('check --display-exit',timeout=20))
        info=vm.command('doom --info');assert_success(info);assert 'queued=0' in info,info
        vm.log.flush();result['serial_log']=str(vm.log.name)
        serial=Path(vm.log.name).read_text(errors='replace')
        assert 'DOOM_SFX start=' not in serial,'Sound effects contaminated music-only test'
        if enabled:
            assert 'DOOM_MUSIC score hash='+score_hash() in serial,'Actual MAP01 score not registered'
        result['result']='passed'
    except BaseException as error:
        result['error']=str(error);raise
    finally:
        if vm:vm.close()
        try:
            if result['result']=='passed':result['pcm']=capture(wav,enabled)
        except BaseException as error:
            result['result']='failed';result['capture_error']=str(error);raise
        finally:
            (WORK/(label+'.json')).write_text(json.dumps(result,indent=2)+'\n')
    print('PASS Doom music-only HDA '+('positive/pause/resume' if enabled else 'silent negative')+' '+label,flush=True)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--iso',required=True)
    p.add_argument('--cpus',type=int,default=4);p.add_argument('--accel',choices=('tcg','kvm'),default='kvm')
    p.add_argument('--label',required=True);a=p.parse_args()
    exercise(a.iso,a.cpus,a.accel,True,a.label+'-music')
    exercise(a.iso,a.cpus,a.accel,False,a.label+'-silent')

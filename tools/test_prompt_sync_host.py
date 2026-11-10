#!/usr/bin/env python3
"""Recognize actual idle prompts, excluding split redraws and allowing background output."""
import os,selectors,tempfile,threading,time
from test_runtime import VM
class Running:
 def poll(self):return None

def exercise(first,second,expected,pattern=None):
 readfd,writefd=os.pipe();vm=VM.__new__(VM);vm.name='host-prompt-split';vm.pending=bytearray();vm.process=Running();vm.process.stdout=os.fdopen(readfd,'rb',buffering=0);vm.selector=selectors.DefaultSelector();vm.selector.register(vm.process.stdout,selectors.EVENT_READ)
 try:
  with tempfile.TemporaryFile() as output:
   vm.log=output
   def writer():
    os.write(writefd,first)
    if second:
     time.sleep(.02);os.write(writefd,second)
   thread=threading.Thread(target=writer);thread.start();actual=vm.until(pattern,2) if pattern is not None else vm.until_prompt(2);thread.join(2)
   assert not thread.is_alive() and expected in actual,actual
 finally:
  vm.selector.close();vm.process.stdout.close();os.close(writefd)
exercise(b'\r\x1b[2Kos:/$ ',b'echo marker\nEXECUTED_MARKER\nos:/$ ','EXECUTED_MARKER\n')
exercise(b'started pid 123\nos:/$ BACKGROUND_WAIT\n',b'','BACKGROUND_WAIT\n')
exercise(b'POWER_DIRTY_CONTENTos:/$ ',b'','POWER_DIRTY_CONTENT')
exercise(b'\r\x1b[2Kos:/$ ',b'cat no-newline\nCONTENTos:/$ ','CONTENT')
exercise(b'\r\x1b[2Kos:/$ ',b'doom --trace\nDOOM_READY display=2560x1600\n','DOOM_READY',rb'DOOM_READY|os:[^\n]*\$ $')
print('PASS split redraw is not completion; real prompts survive background output and output without a newline')

#!/usr/bin/env python3
"""Geometry remains readable when an earlier wait consumes a record prefix."""
import pathlib,os,tempfile,threading,selectors,time
from test_runtime import VM
from test_desktop import DesktopTest
class Running:
    def poll(self):return None
with tempfile.TemporaryDirectory(prefix='coast-geometry-') as name:
    log=pathlib.Path(name)/'serial.log';log.write_bytes(b'DESKTOP OPEN window=8 workspace=1 Settings\r\nD')
    readfd,writefd=os.pipe();vm=VM.__new__(VM);vm.name='geometry-split';vm.pending=bytearray();vm.process=Running();vm.process.stdout=os.fdopen(readfd,'rb',buffering=0);vm.log=log.open('ab');vm.selector=selectors.DefaultSelector();vm.selector.register(vm.process.stdout,selectors.EVENT_READ)
    def writer():
        time.sleep(.02);os.write(writefd,b'ESKTOP GEOMETRY window=8 workspace=1 x=972 y=536 width=680 height=550 body=64 scale=100\r\n')
    thread=threading.Thread(target=writer);thread.start()
    try:
        value=DesktopTest(vm).geometry(8);thread.join(2);assert value=={'x':972,'y':536,'width':680,'height':550,'body':64,'scale':100}
    finally:vm.selector.close();vm.process.stdout.close();vm.log.close();os.close(writefd)
print('PASS actual desktop harness: geometry survives split records and consumed prefixes')

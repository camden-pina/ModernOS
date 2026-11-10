#!/usr/bin/env python3
"""PTY trace framing must preserve Doom lines across arbitrary read boundaries."""
from test_doom_window import decode_terminal_stream
records = (
    b'DESKTOP TERMINAL_OUTPUT window=2 workspace=1 DOOM_IN\r\n'
    b'DESKTOP GAME_FRAME window=3 seq=1\r\n'
    b'DESKTOP TERMINAL_OUTPUT window=2 workspace=1 PUT focus=1\r\n\r\n'
    b'DESKTOP TERMINAL_OUTPUT window=9 workspace=1 ignored\r\n'
    b'DESKTOP TERMINAL_OUTPUT window=2 workspace=1 DOOM_FRAME a=1\r\n\r\n'
)
expected = b'DOOM_INPUT focus=1\nDOOM_FRAME a=1\n'
assert decode_terminal_stream(records, 2) == expected
previous = b''
for length in range(len(records) + 1):
    result = decode_terminal_stream(records[:length], 2)
    assert expected.startswith(result), (length, result)
    assert result.startswith(previous), (length, previous, result)
    previous = result
print('PASS exact GUI PTY reconstruction across every byte split, repaint and foreign-window records')


# Actual serial helper, with a prior complete viewport and every possible
# consumed prefix of the changed record. Do not accept an unchanged old view.
import argparse,importlib.util,os,pathlib,selectors,tempfile,threading,time,types
import test_doom_window as viewport_module
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--viewport-module',type=pathlib.Path)
parser.add_argument('--one-split',action='store_true')
arguments=parser.parse_args()
if arguments.viewport_module:
    spec=importlib.util.spec_from_file_location('viewport_regression_baseline',arguments.viewport_module)
    viewport_module=importlib.util.module_from_spec(spec);spec.loader.exec_module(viewport_module)
old=b'DESKTOP GAME_VIEW window=3 x=1000 y=595 width=624 height=468 body_x=893 body_y=595 body_w=838 body_h=495 source=320x200\r\n'
new=b'DESKTOP GAME_VIEW window=3 x=1006 y=595 width=657 height=493 body_x=893 body_y=595 body_w=883 body_h=520 source=320x200\r\n'
previous={'window':3,'x':1000,'y':595,'width':624,'height':468,'body_x':893,'body_y':595,'body_w':838,'body_h':495}
expected_view=dict(previous,x=1006,width=657,height=493,body_w=883,body_h=520)
class Running:
    def poll(self):return None

def split_viewport(split,unchanged=False):
    with tempfile.TemporaryDirectory(prefix='coast-doom-viewport-') as temporary:
        log=pathlib.Path(temporary)/'serial.log';log.write_bytes(old+new[:split])
        readfd,writefd=os.pipe();vm=viewport_module.WindowVM.__new__(viewport_module.WindowVM)
        vm.name='doom-viewport-split';vm.pending=bytearray();vm.decode_terminal=False
        vm.process=Running();vm.process.stdout=os.fdopen(readfd,'rb',buffering=0)
        vm.log=log.open('ab');vm.selector=selectors.DefaultSelector();vm.selector.register(vm.process.stdout,selectors.EVENT_READ)
        actual_until=vm.until
        vm.until=lambda pattern,timeout=30:actual_until(pattern,min(timeout,.25))
        def writer():
            time.sleep(.002)
            if not unchanged:os.write(writefd,new[split:])
        thread=threading.Thread(target=writer);thread.start()
        try:
            value=viewport_module.changed_view(vm,previous)
            assert not unchanged,'Stale geometry was accepted'
            assert value==expected_view,(split,value)
        finally:
            thread.join(2);vm.selector.close();vm.process.stdout.close();vm.log.close();os.close(writefd)
for split in ([new.index(b'=893') ] if arguments.one_split else range(len(new)+1)):
    split_viewport(split)
try:
    split_viewport(0,unchanged=True)
except AssertionError as error:
    assert 'Stale geometry was accepted' not in str(error),error
else:
    raise AssertionError('Missing changed geometry did not fail')
print('PASS actual managed-Doom viewport helper: every byte split and consumed prefix; unchanged view rejected')

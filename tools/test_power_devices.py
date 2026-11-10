#!/usr/bin/env python3
"""Optional unchanged-ISO e1000e/HDA terminal pair; NOT part of the 39+18 suite.

Default prints a plan. --execute requires its direct reserved cloud/local supervisor.
No guest binary, AML, boot token, kernel or driver is changed. Runtime unproven.
"""
import argparse
from array import array
import contextlib
import ctypes
import hashlib
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
import os
from pathlib import Path
import re
import select
import signal
import struct
import subprocess
import sys
import threading
import time
import uuid

from test_power import PowerVM, digest, drain, plain
from test_power_release import verify_image
from test_runtime import run

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'build/power-device-tests'
TRACE_EVENTS = ('e1000e_rx_written_to_guest', 'e1000e_rx_set_rctl',
                'e1000e_core_write', 'e1000e_core_ctrl_sw_reset',
                'hda_audio_running', 'pci_cfg_write')


def write_json(path, value):
    with Path(path).open('w') as output:
        json.dump(value, output, indent=2, sort_keys=True); output.write('\n')
        output.flush(); os.fsync(output.fileno())


def identity(pid):
    try:
        data = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
        return dict(pid=pid, state=data[0], parent=int(data[1]), group=int(data[2]), start=int(data[19]))
    except (FileNotFoundError, ProcessLookupError):
        return None


def admitted(environment=None, root=ROOT, mode='cloud'):
    environment = os.environ if environment is None else environment
    if mode == 'local':
        from run_power_devices_local import admit_child
        return admit_child(environment, root)
    if mode != 'cloud':
        raise RuntimeError('Unknown power-device admission mode')
    parent = environment.get('COAST_QEMU_FILE_RAM_SUPERVISOR_PID', '')
    if (environment.get('COAST_QEMU_FILE_RAM') != '1' or not parent.isdecimal() or
            int(parent) != os.getppid() or os.getpgrp() != os.getpid()):
        raise RuntimeError('Execution requires the reserved pilot direct owned harness group')
    marker = root/'build/browser-guests-active'
    if (not marker.is_file() or marker.is_symlink() or
            marker.read_text().strip() != environment.get('COAST_QEMU_FILE_RAM_SLOT_TOKEN') or
            not environment.get('COAST_QEMU_FILE_RAM_SLOT_TOKEN', '').startswith('qemu-file-ram:')):
        raise RuntimeError('Actual pilot marker/token mismatch')
    blocker = root/'build/release-file-ram-recovery-required.json'
    if blocker.exists() or blocker.is_symlink():
        raise RuntimeError('Unresolved owned-process cleanup blocks execution')
    return dict(mode='cloud', supervisor_pid=int(parent))


class Interrupted(KeyboardInterrupt):
    pass


@contextlib.contextmanager
def owned_children(recovery_folder=None):
    """Keep existing host-tool sessions in this pilot-owned group; drain only it."""
    group = os.getpid()
    if os.getpgrp() != group or not hasattr(os, 'pidfd_open') or not hasattr(signal, 'pidfd_send_signal'):
        raise RuntimeError('Fresh owned group and Linux pidfds are required')
    libc = ctypes.CDLL(None, use_errno=True)
    previous = ctypes.c_int()
    if libc.prctl(37, ctypes.byref(previous), 0, 0, 0) or libc.prctl(36, 1, 0, 0, 0):
        raise RuntimeError('Cannot establish process-local child reaping')
    original = subprocess.Popen
    children = []
    def contained(*arguments, **options):
        if options.get('preexec_fn') is not None:
            raise RuntimeError('Unreviewed preexec hook refused')
        options['start_new_session'] = False; options['process_group'] = None
        child = original(*arguments, **options); children.append(child)
        return child
    subprocess.Popen = contained
    try:
        yield
    finally:
        subprocess.Popen = original
        try:
            # A stable pidfd plus start/group check prevents unrelated PID signals.
            for number, seconds in ((signal.SIGTERM, 2), (signal.SIGKILL, 1)):
                end = time.monotonic() + seconds
                while True:
                    live = []
                    for entry in Path('/proc').iterdir():
                        if not entry.name.isdecimal() or int(entry.name) == group: continue
                        current = identity(int(entry.name))
                        if not current or current['group'] != group: continue
                        if current['state'] != 'Z':
                            live.append(current)
                            try:
                                fd = os.pidfd_open(current['pid'])
                                try:
                                    if (observed := identity(current['pid'])) and (observed['start'], observed['group']) == (current['start'], current['group']):
                                        signal.pidfd_send_signal(fd, number)
                                finally: os.close(fd)
                            except ProcessLookupError: pass
                    for child in children: child.poll()
                    while True:
                        try: pid, _ = os.waitpid(-group, os.WNOHANG)
                        except ChildProcessError: break
                        if not pid: break
                    if not live: break
                    if time.monotonic() >= end: break
                    time.sleep(.02)
                if not live: break
            if live:
                if recovery_folder is not None: record_cleanup_blocker(recovery_folder,live)
                raise RuntimeError('Owned descendants remain alive; recovery blocker requires manual verification')
        finally:
            libc.prctl(36, previous.value, 0, 0, 0)


def record_cleanup_blocker(folder, members):
    value=dict(result='cleanup-blocked',run=str(folder),owned_group=os.getpid(),members=members,
               manual_recovery_required=True,instruction='Hold all compiler/guest admission until owned identities are verified dead; never infer ownership or auto-delete this blocker')
    write_json(folder/'cleanup-blocked.json',value)
    path=ROOT/'build/release-file-ram-recovery-required.json'
    try:
        fd=os.open(path,os.O_WRONLY|os.O_CREAT|os.O_EXCL|getattr(os,'O_NOFOLLOW',0),0o600)
    except FileExistsError:
        return # Existing blocker already forbids every cooperating admission.
    with os.fdopen(fd,'w') as output:
        json.dump(value,output,indent=2);output.write('\n');output.flush();os.fsync(output.fileno())


def pci_devices(value):
    if isinstance(value, list):
        for item in value: yield from pci_devices(item)
    elif isinstance(value, dict):
        if all(key in value for key in ('bus','slot','function','id','class_info')):
            yield value
        for key, item in value.items():
            if key not in ('id','class_info','regions'): yield from pci_devices(item)


def device_locations(value):
    devices = list(pci_devices(value)); result = {}
    for label, matches in (
        ('ethernet', [d for d in devices if d['id'].get('vendor') == 0x8086 and d['id'].get('device') == 0x10d3]),
        ('hda', [d for d in devices if d['id'].get('vendor') == 0x8086 and d['class_info'].get('class') == 0x403])):
        if len(matches) != 1: raise AssertionError('Expected one initialized '+label)
        d = matches[0]
        bars = [bar for bar in d['regions'] if bar.get('bar') == 0 and bar.get('type') == 'memory' and bar.get('address',0) > 0]
        if len(bars) != 1: raise AssertionError('Expected assigned MMIO BAR0 for '+label)
        result[label] = dict(bus=d['bus'],slot=d['slot'],function=d['function'],bar=bars[0]['address'])
    return result


def parse_xp(text):
    values = re.findall(r':\s*(0x[0-9a-fA-F]+)\s*$', text, re.M)
    if len(values) != 1: raise AssertionError('Unrecognized read-only HMP xp result: '+text)
    return int(values[0],16)


def registers(vm, devices):
    def read(base, offset):
        return parse_xp(vm.qmp('human-monitor-command', {'command-line':f'xp /1wx {base+offset:#x}'}))
    nic, hda = devices['ethernet']['bar'], devices['hda']['bar']
    gcap = read(hda, 0); stream = 0x80 + ((gcap >> 8) & 15)*0x20
    return dict(ethernet=dict(rctl=read(nic,0x100),tctl=read(nic,0x400),ctrl=read(nic,0)),
                hda=dict(gcap=gcap,stream_offset=stream,stream_ctl=read(hda,stream),
                         corb_ctl=read(hda,0x4c),rirb_ctl=read(hda,0x5c),
                         position_base=read(hda,0x70),intctl=read(hda,0x20)))


def check_registers(before, after):
    for key in ('rctl','tctl'):
        assert before['ethernet'][key] & 2, ('NIC not enabled before request',before)
        assert not after['ethernet'][key] & 2, ('NIC engine still enabled',after)
    for key in ('stream_ctl','corb_ctl','rirb_ctl'):
        assert before['hda'][key] & 2, ('HDA not active before request',before)
        assert not after['hda'][key] & 2, ('HDA DMA still enabled',after)
    assert not after['ethernet']['ctrl'] & (1<<26), after
    assert not after['hda']['position_base'] & 1 and after['hda']['intctl'] == 0, after
    assert before['hda']['stream_offset'] == after['hda']['stream_offset'], after


def parse_counters(text):
    match = re.search(r'Packets: rx=(\d+) tx=(\d+) dropped=(\d+)',text)
    if not match: raise AssertionError('Missing real network counters')
    return dict(zip(('rx','tx','dropped'),map(int,match.groups())))


def parse_audio(text):
    match = re.search(r'PCM: 48000 Hz, 2 channels, 16 bits, queued=(\d+) played=(\d+) frames',text)
    if not match: raise AssertionError('Missing actual HDA PCM counters')
    return dict(queued=int(match[1]),played=int(match[2]))


def trace_evidence(text, devices, request_line=0):
    lines = text.splitlines()
    def matching(pattern):
        return [i for i,line in enumerate(lines) if re.search(pattern,line,re.I)]
    rx = matching(r'e1000e_rx_written_to_guest ')
    rctl = [i for i in matching(r'e1000e_rx_set_rctl RCTL = 0x0\b') if i >= request_line]
    tx = [i for i in matching(r'e1000e_core_write Write to register 0x400, 4 byte\(s\), value: 0x0\b') if i >= request_line]
    reset = [i for i in matching(r'e1000e_core_ctrl_sw_reset(?: |$)') if i >= request_line]
    audio = []
    for i,line in enumerate(lines):
        match = re.search(r'hda_audio_running st (\S+), nr (\d+), run ([01])\b',line,re.I)
        if match and match[1].lower() == 'dac':
            audio.append((i,int(match[2]),int(match[3])))
    starts = [item for item in audio if item[2] == 1]
    assert rx and rctl and tx and reset and starts, 'Required device trace events missing'
    stop_boundary = max(rctl)
    assert any(i < stop_boundary for i in rx), 'No receive DMA before controller stop'
    assert not any(i > stop_boundary for i in rx), 'Receive DMA continued after disable'
    assert max(reset) > max(tx) and max(reset) >= stop_boundary, 'No final reset after engine disable'
    bus_master = {}
    for name,device in devices.items():
        bdf = f"{device['bus']:02x}:{device['slot']:02x}.{device['function']:x}"
        writes = []
        for i,line in enumerate(lines):
            match = re.search(r'pci_cfg_write .*? '+re.escape(bdf)+r' @0x4 <- 0x([0-9a-fA-F]+)\b',line,re.I)
            if match: writes.append((i,int(match[1],16)))
        assert writes and any(value & 4 for _,value in writes), ('No prior bus mastering',name)
        assert writes[-1][0] >= request_line and not writes[-1][1] & 4, ('No terminal PCI bus-master disable',name)
        bus_master[name] = dict(bdf=bdf,last_command=writes[-1][1],trace_line=writes[-1][0])
    assert bus_master['ethernet']['trace_line'] > max(reset), 'NIC bus-master disable did not follow final reset'
    assert bus_master['hda']['trace_line'] > bus_master['ethernet']['trace_line'], 'Controller stop ordering does not match coordinator'
    playback = starts[-1][1]
    states = [(i,state) for i,number,state in audio if number == playback]
    assert states[-1][1] == 0 and states[-1][0] < bus_master['hda']['trace_line'], 'Playback stream did not stop before HDA bus-master disable'
    assert not any(state for i,state in states if i > bus_master['hda']['trace_line']), 'Playback restarted after device stop'
    at_boundary = [state for i,state in states if i < stop_boundary]
    assert at_boundary, 'No playback-stream state before NIC stop'
    return dict(receive_dma_events=len(rx),audio_run_events=len(starts),playback_stream=playback,
                pci_bus_master=bus_master,audio_running_at_network_stop=bool(at_boundary[-1]),
                audio_drained_before_network_stop=not bool(at_boundary[-1]),active_audio_at_stop=None,
                active_audio_at_stop_reason='Trace playback state is reported separately; earlier PCM and final disabled registers do not establish an instantaneous DMA-at-stop sample')


def partial_pcm(path):
    data = path.read_bytes()
    assert len(data) >= 44 and data[:4] == b'RIFF' and data[8:16] == b'WAVEfmt ' and data[36:40] == b'data'
    assert struct.unpack_from('<HHI',data,20) == (1,2,48000) and struct.unpack_from('<H',data,34)[0] == 16
    assert (len(data)-44)%4 == 0
    samples = array('h',data[44:])
    if sys.byteorder != 'little': samples.byteswap()
    nonzero = sum(abs(n)>100 for n in samples)
    assert nonzero >= 480, 'No meaningful captured device PCM'
    return dict(frames=len(samples)//2,nonzero_samples=nonzero,sha256=digest(path),
                scope='Interrupted output only; not the complete speaker-check sequence or physical sound')


class Stream:
    """One synthetic response, loopback only, <=256KiB and <=15 seconds."""
    def __init__(self):
        self.stop = threading.Event(); self.stats = dict(requests=0,bytes_sent=0,active=False,completed=False,last_write=None)
        owner = self
        class Handler(BaseHTTPRequestHandler):
            def log_message(self,*args): pass
            def do_GET(self):
                if self.path != '/power-stream': self.send_error(404); return
                owner.stats['requests'] += 1; owner.stats['active']=True; self.connection.settimeout(1)
                self.send_response(200); self.send_header('Content-Length',str(256*1024)); self.end_headers()
                end = time.monotonic()+15
                try:
                    for index in range(128):
                        if owner.stop.is_set() or time.monotonic() >= end: break
                        block = bytes([index%251])*2048
                        self.wfile.write(block); self.wfile.flush(); owner.stats['bytes_sent'] += len(block); owner.stats['last_write']=time.monotonic()
                        owner.stop.wait(.1)
                except (BrokenPipeError,ConnectionResetError,TimeoutError): pass
                finally: owner.stats['active']=False; owner.stats['completed']=True
        class BoundedServer(HTTPServer):
            def get_request(self):
                connection,address=super().get_request();connection.settimeout(1)
                return connection,address
        self.server = BoundedServer(('127.0.0.1',0),Handler)
        self.thread = threading.Thread(target=self.server.serve_forever,kwargs={'poll_interval':.05},daemon=True)
        self.thread.start()
    @property
    def url(self): return f'http://10.0.2.2:{self.server.server_port}/power-stream'
    def close(self):
        self.stop.set(); self.server.shutdown(); self.server.server_close(); self.thread.join(2)
        if self.thread.is_alive(): raise RuntimeError('Owned loopback fixture did not stop')


def close_vm(vm):
    if vm.process.poll() is None:
        vm.process.terminate()
        try: vm.process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            vm.process.kill(); vm.process.wait(timeout=2)
    vm.close()


def is_terminal_event(event, kind):
    expected = 'guest-reset' if kind == 'reboot' else 'guest-shutdown'
    return (event.get('event') == 'SHUTDOWN' and event.get('data',{}).get('reason') == expected and event.get('data',{}).get('guest') is True)


def check_terminal_log(text):
    assert all(s in text for s in ('Power request accepted','reversible park acknowledged','flush complete','power: COMMIT')), text
    assert not any(s in text for s in ('PANIC:','terminal failure','failed stage=')), text


def terminal(vm, kind, timeout, evidence=None):
    expected = 'guest-reset' if kind == 'reboot' else 'guest-shutdown'
    end = time.monotonic()+timeout; events=[]; found=False; status=None
    evidence = {} if evidence is None else evidence
    evidence['qmp_events']=events
    while time.monotonic() < end:
        drain(vm)
        if select.select([vm.qmp_output],[],[],.03)[0]: vm.qmp_pending.extend(os.read(vm.qmp_output,65536))
        while b'\n' in vm.qmp_pending:
            line,_,rest=vm.qmp_pending.partition(b'\n');vm.qmp_pending=bytearray(rest)
            if not line.strip(): continue
            event=json.loads(line);events.append(event)
            if is_terminal_event(event,kind):
                found=True
        if found:
            assert vm.process.poll() is None, 'Read-only post-action register inspection requires -no-shutdown'
            status=vm.qmp('query-status');evidence['qmp_status_after_event']=status
            if not status['running']: break
        if vm.process.poll() is not None: break
    text=plain(vm)
    assert found and status is not None and not status['running'], (expected,events,text)
    # Serial and QMP are independent pipes. The status reply may arrive after
    # guest serial bytes became readable, following this iteration's drain.
    # Keep stopped emulation intact and use only the remaining terminal budget.
    serial=dict(characters_at_pause=len(text));started=time.monotonic()
    evidence['terminal_serial_drain']=serial
    required=('Power request accepted','reversible park acknowledged','flush complete','power: COMMIT')
    while time.monotonic()<end:
        drain(vm);text=plain(vm)
        if all(marker in text for marker in required): break
        assert vm.process.poll() is None, 'Stopped emulator exited during terminal serial observation'
    serial.update(characters_after_drain=len(text),seconds=time.monotonic()-started,
                  required_markers_observed=all(marker in text for marker in required))
    check_terminal_log(text)
    return events


def check_boot_admission(args):
    admission = getattr(args, '_admission', {})
    if admission.get('mode') == 'local':
        from run_power_devices_local import verify_lease
        verify_lease(admission, ROOT)


def scenario(args,folder,report):
    check_boot_admission(args)
    disk=folder/'persist.volume'
    with disk.open('wb') as output: output.truncate(64*1024*1024)
    run('mkfs.fat','-F','32',disk)
    trace=folder/'devices.trace'; events=folder/'trace-events'; events.write_text('\n'.join(TRACE_EVENTS)+'\n')
    capture=folder/'partial.wav'
    extra=['-drive','file='+str(disk).replace(',',',,')+',format=raw,if=none,id=pwr',
           '-device','nvme,drive=pwr,serial=POWER-DEVICES', '-no-shutdown',
           '-trace','events='+str(events)+',file='+str(trace)]
    label='power-devices-'+folder.name; vm=None; fixture=None
    payload='POWER_DEVICES_'+folder.name
    try:
        fixture=Stream()
        report['serial_log']=str(ROOT/'build/runtime-tests'/('uefi-4cpu-'+label+'.log'))
        vm=PowerVM(4,storage=False,network=True,network_model='e1000e',audio_capture=capture,
                   audio_device='hda',audio_capture_rate=48000,iso=args.iso,memory=args.memory,label=label,extra_args=extra)
        report['vm_configuration']=vm.configuration
        report['qemu_extra_args']=extra;report['serial_log']=str(vm.log.name)
        assert 'Network: Intel Ethernet 10d3' in plain(vm) and re.search(r'Audio: HDA .* analog outputs',plain(vm)), plain(vm)
        devices=device_locations(vm.qmp('query-pci'));report['devices']=devices
        vm.command('check','USERLAND CHECK PASS',60)
        vm.command('write /mnt/nvme0n1/power-devices.txt '+payload)
        readiness=vm.command('power','power_state=idle')
        assert 'poweroff_ready=1' in readiness and 'reset_ready=1' in readiness,readiness
        vm.shell_command('i=0; while test ! -s /etc/resolv.conf && test "$i" -lt 20; do sleep 1; i=$((i+1)); done; test -s /etc/resolv.conf',timeout=30)
        network_before=parse_counters(vm.command('netinfo','Packets:'))
        vm.command('curl -fsS --max-time 20 '+fixture.url+' -o /tmp/power-stream.bin &','started pid')
        deadline=time.monotonic()+5
        while True:
            network_after=parse_counters(vm.command('netinfo','Packets:'))
            if network_after['rx']>network_before['rx'] and network_after['tx']>network_before['tx'] and fixture.stats['bytes_sent']>=4096: break
            assert time.monotonic()<deadline,'No real RX/TX workload progress';time.sleep(.05)
        vm.command('speaker-check &','started pid')
        audio_before=parse_audio(vm.shell_command('speaker-check --info','PCM:'))
        deadline=time.monotonic()+8
        while True:
            audio_after=parse_audio(vm.shell_command('speaker-check --info','PCM:'))
            if audio_after['queued']>0 and audio_after['played']>audio_before['played']: break
            assert time.monotonic()<deadline,'No active PCM progress';time.sleep(.03)
        before=registers(vm,devices);report['registers_before_request']=before
        assert before['hda']['stream_ctl'] & 2, 'Playback drained before request snapshot'
        assert (fixture.stats['active'] and not fixture.stats['completed'] and fixture.stats['last_write'] is not None and
                time.monotonic()-fixture.stats['last_write']<1.5 and fixture.stats['bytes_sent']<256*1024),'No recent active network response'
        report['trace_request_line']=len(trace.read_text().splitlines())
        report['workload']=dict(network_before=network_before,network_after=network_after,
                                audio_before=audio_before,audio_after=audio_after,stream_before=dict(fixture.stats))
        vm.command(args.kind,'Power request accepted')
        report['qmp_events']=terminal(vm,args.kind,args.timeout,report)
        after=registers(vm,devices);report['registers_after_action']=after
        check_registers(before,after)
        # Freeze the device-stop evidence before host teardown can reset devices.
        terminal_trace=trace.read_text();(folder/'terminal.trace').write_text(terminal_trace)
        report['device_trace']=trace_evidence(terminal_trace,devices,report['trace_request_line'])
        close_vm(vm);vm=None
        fixture.close();report['stream_after']=dict(fixture.stats);fixture=None
        report['partial_pcm']=partial_pcm(capture)
        run('fsck.fat','-n',disk)
        persisted=run('mtype','-i',disk,'::power-devices.txt');assert persisted==(payload+'\n').encode()
        report['persisted_sha256']=hashlib.sha256(persisted).hexdigest()
        # Fresh boot remains the supplied ISO. Reopen is persistence evidence only.
        reopen_extra=extra[:4]
        report['reopen_serial_log']=str(ROOT/'build/runtime-tests'/('uefi-4cpu-'+label+'-reopen.log'))
        check_boot_admission(args)
        vm=PowerVM(4,storage=False,network=False,iso=args.iso,memory=args.memory,
                   label=label+'-reopen',extra_args=reopen_extra)
        report['reopen_serial_log']=str(vm.log.name)
        vm.command('cat /mnt/nvme0n1/power-devices.txt',payload)
        vm.command('check','USERLAND CHECK PASS',60)
        report['result']='passed'
    finally:
        try:
            if vm: close_vm(vm)
        finally:
            if fixture: fixture.close()


def helper_state():
    return {name:digest(ROOT/'tools'/name) for name in ('test_power_devices.py','run_power_devices_local.py','test_power.py','test_power_release.py','test_runtime.py')}


def evidence_manifest(folder,report):
    for key in ('serial_log','reopen_serial_log'):
        if key in report:
            source=Path(report[key])
            if source.is_file():
                destination=folder/(key+'.log');destination.write_bytes(source.read_bytes())
    records=[]
    for path in sorted(folder.rglob('*')):
        if path.is_symlink(): raise RuntimeError('Evidence symlink refused')
        if path.is_file() and path.name!='evidence-manifest.json':
            records.append(dict(path=str(path.relative_to(folder)),bytes=path.stat().st_size,sha256=digest(path)))
    write_json(folder/'evidence-manifest.json',records)


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    for key in ('iso','kernel','shell','gui'): parser.add_argument('--'+key,type=Path,required=True)
    parser.add_argument('--kind',choices=('poweroff','reboot'),required=True)
    parser.add_argument('--memory',default='2G');parser.add_argument('--timeout',type=int,default=60)
    parser.add_argument('--execute',action='store_true')
    parser.add_argument('--admission',choices=('cloud','local'),default='cloud')
    args=parser.parse_args(argv)
    if args.memory!='2G' or args.timeout<=0: parser.error('Exactly2G RAM and a positive terminal timeout are required')
    if not args.execute:
        print(json.dumps(dict(result='not-run',case=args.kind,cpus=4,transport='nvme',iso=str(args.iso),
            unchanged_iso=True,network='e1000e user networking; synthetic loopback HTTP',audio='HDA WAV',
            boots=2,active_audio_at_stop='unproven',inventory='optional supplement; outside39+18'),indent=2));return 0
    args._admission=admitted(mode=args.admission)
    for key in ('iso','kernel','shell','gui'):setattr(args,key,getattr(args,key).resolve(strict=True))
    WORK.mkdir(parents=True,exist_ok=True)
    folder=WORK/(args.kind+'-'+str(time.time_ns())+'-'+uuid.uuid4().hex[:8]);folder.mkdir()
    report=dict(result='failed',kind=args.kind,cpus=4,memory=args.memory,transport='nvme',
                source_unchanged_iso=True,inventory='optional supplement; outside39+18',
                host_exit_policy='-no-reboot -no-shutdown retains stopped emulator for read-only BAR inspection',
                physical_hardware_validated=False,admission=args._admission)
    old={};before_helpers=helper_state();report['helpers_before']=before_helpers
    def interrupted(number,frame):
        for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,signal.SIG_IGN)
        raise Interrupted('Interrupted with signal '+str(number))
    try:
        for sig in (signal.SIGINT,signal.SIGTERM):old[sig]=signal.signal(sig,interrupted)
        with owned_children(folder):
            inspect=folder/'inspection';inspect.mkdir();report['inputs']=verify_image(args,inspect)
            write_json(folder/'input-provenance.json',report['inputs'])
            # Only this fresh inspection's generated full archive/kernel copies.
            for path in inspect.iterdir():
                if path.name not in ('kernel.elf','initramfs.tar','coast.conf','limine.conf') or not path.is_file() or path.is_symlink():
                    raise RuntimeError('Unexpected generated inspection entry')
                path.unlink()
            scenario(args,folder,report)
    except BaseException as error:
        report['result']='failed';report['error']=str(error);raise
    finally:
        for sig,handler in old.items():signal.signal(sig,handler)
        if 'inputs' in report:
            report['read_only_inputs_unchanged']=all(digest(Path(report['inputs'][key]['path']))==report['inputs'][key]['sha256'] for key in ('iso','kernel','shell','gui'))
            if not report['read_only_inputs_unchanged']:report['result']='failed'
        report['helpers_after']=helper_state()
        report['helpers_unchanged']=report['helpers_after']==before_helpers
        if not report['helpers_unchanged']:report['result']='failed'
        write_json(folder/'report.json',report)
        try: evidence_manifest(folder,report)
        except BaseException as error:
            report['result']='failed';report['evidence_error']=str(error)
            write_json(folder/'report.json',report)
            raise
    assert report['result']=='passed' and report['read_only_inputs_unchanged']
    print('POWER DEVICES PASSED',args.kind,'report='+str(folder/'report.json'),flush=True)
    return 0


if __name__=='__main__':raise SystemExit(main())

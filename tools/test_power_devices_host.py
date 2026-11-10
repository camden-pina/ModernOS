#!/usr/bin/env python3
"""Supplemental power-device oracle and local supervision checks. No compiler or guest."""
import contextlib
import copy
import importlib.util
import io
import json
import os
import selectors
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

MODULE=Path(__file__).with_name('test_power_devices.py')
spec=importlib.util.spec_from_file_location('power_devices',MODULE)
D=importlib.util.module_from_spec(spec);spec.loader.exec_module(D)
import run_power_devices_local as L
import test_runtime as R
DEVICES={'ethernet':dict(bus=0,slot=2,function=0,bar=0xf0000000),'hda':dict(bus=0,slot=3,function=0,bar=0xf1000000)}

def trace(drained=False):
    audio='hda_audio_running st dac, nr 1, run 0\n' if drained else ''
    return ('123@1:pci_cfg_write e1000e 00:02.0 @0x4 <- 0x406\n'
            '123@2:pci_cfg_write intel-hda 00:03.0 @0x4 <- 0x6\n'
            '123@3:hda_audio_running st dac, nr 1, run 1\n'
            '123@4:e1000e_rx_written_to_guest Received packet written to guest (queue 0)\n'+audio+
            '123@5:e1000e_rx_set_rctl RCTL = 0x0\n'
            '123@6:e1000e_core_write Write to register 0x400, 4 byte(s), value: 0x0\n'
            '123@7:e1000e_core_ctrl_sw_reset Doing SW reset\n'
            '123@8:pci_cfg_write e1000e 00:02.0 @0x4 <- 0x402\n'+
            ('' if drained else '123@9:hda_audio_running st dac, nr 1, run 0\n')+
            '123@10:pci_cfg_write intel-hda 00:03.0 @0x4 <- 0x2\n')


class OracleTests(unittest.TestCase):
    def test_positive_trace_does_not_claim_instantaneous_dma(self):
        value=D.trace_evidence(trace(),DEVICES,4)
        self.assertTrue(value['audio_running_at_network_stop'])
        self.assertIsNone(value['active_audio_at_stop'])
        self.assertEqual(value['receive_dma_events'],1)

    def test_drain_during_grace_is_honestly_classified(self):
        value=D.trace_evidence(trace(True),DEVICES,4)
        self.assertTrue(value['audio_drained_before_network_stop'])
        self.assertFalse(value['audio_running_at_network_stop'])
        self.assertIsNone(value['active_audio_at_stop'])

    def test_boot_stop_cannot_replace_terminal_stop(self):
        with self.assertRaises(AssertionError):D.trace_evidence(trace(),DEVICES,100)

    def test_boot_rx_disable_is_not_terminal_disable(self):
        boot='e1000e_rx_set_rctl RCTL = 0x0\ne1000e_core_ctrl_sw_reset Doing SW reset\n'
        actual='\n'.join(line for line in trace().splitlines() if 'e1000e_rx_set_rctl' not in line)
        with self.assertRaises(AssertionError):D.trace_evidence(boot+actual,DEVICES,2)

    def test_missing_rx_reset_tx_or_bus_master_ack_rejected(self):
        for remove in ('e1000e_rx_written_to_guest','e1000e_core_ctrl_sw_reset','e1000e_core_write','pci_cfg_write intel-hda'):
            with self.subTest(remove=remove),self.assertRaises(AssertionError):
                D.trace_evidence('\n'.join(line for line in trace().splitlines() if remove not in line),DEVICES,4)

    def test_wrong_playback_stream_input_restart_and_enabled_pci_rejected(self):
        bad=[trace().replace('st dac, nr 1, run 0','st adc, nr 1, run 0'),
             trace().replace('st dac, nr 1, run 0','st dac, nr 2, run 0'),
             trace()+'hda_audio_running st dac, nr 1, run 1\n',
             trace().replace('intel-hda 00:03.0 @0x4 <- 0x2','intel-hda 00:03.0 @0x4 <- 0x6')]
        for value in bad:
            with self.subTest(value=value),self.assertRaises(AssertionError):D.trace_evidence(value,DEVICES,4)

    def test_receive_after_disable_rejected(self):
        with self.assertRaises(AssertionError):D.trace_evidence(trace()+'e1000e_rx_written_to_guest extra\n',DEVICES,4)

    def test_registers_prove_prior_enabled_and_final_disabled(self):
        before=dict(ethernet=dict(rctl=2,tctl=2,ctrl=0),hda=dict(stream_ctl=2,corb_ctl=2,rirb_ctl=2,stream_offset=0x100,position_base=1,intctl=1))
        after=dict(ethernet=dict(rctl=0,tctl=0,ctrl=0),hda=dict(stream_ctl=0,corb_ctl=0,rirb_ctl=0,stream_offset=0x100,position_base=0,intctl=0))
        D.check_registers(before,after)
        for group,key,value in (('ethernet','rctl',2),('ethernet','tctl',2),('ethernet','ctrl',1<<26),('hda','stream_ctl',2),('hda','corb_ctl',2),('hda','rirb_ctl',2),('hda','position_base',1),('hda','intctl',1)):
            broken=copy.deepcopy(after);broken[group][key]=value
            with self.subTest(key=key),self.assertRaises(AssertionError):D.check_registers(before,broken)
        before['hda']['stream_ctl']=0
        with self.assertRaises(AssertionError):D.check_registers(before,after)

    def test_actual_guest_terminal_reasons_and_kernel_chain_required(self):
        for kind,reason in (('poweroff','guest-shutdown'),('reboot','guest-reset')):
            good={'event':'SHUTDOWN','data':{'reason':reason,'guest':True}}
            self.assertTrue(D.is_terminal_event(good,kind))
            for event in ({'event':'RESET','data':good['data']},
                          {'event':'SHUTDOWN','data':{'reason':'host-qmp-quit','guest':False}},
                          {'event':'SHUTDOWN','data':{'reason':reason,'guest':False}},
                          {'event':'SHUTDOWN','data':{'reason':'wrong','guest':True}}):
                self.assertFalse(D.is_terminal_event(event,kind))
        log='Power request accepted\nreversible park acknowledged\nflush complete\npower: COMMIT'
        D.check_terminal_log(log)
        for bad in (log+'\nPANIC:',log+'\nterminal failure',log.replace('flush complete','AE_OK')):
            with self.assertRaises(AssertionError):D.check_terminal_log(bad)

    def test_controller_stop_order_cannot_be_reversed(self):
        lines=trace().splitlines()
        nic=next(line for line in lines if '00:02.0 @0x4 <- 0x402' in line)
        lines.remove(nic);lines.insert(4,nic)
        with self.assertRaises(AssertionError):D.trace_evidence('\n'.join(lines),DEVICES,4)

    def test_read_only_physical_word_parser(self):
        self.assertEqual(D.parse_xp('00000000f1000080: 0x00000002\r\n'),2)
        for text in ('error','0x123','a: 0x1\nb: 0x2'):
            with self.assertRaises(AssertionError):D.parse_xp(text)

    def test_device_discovery_requires_unique_assigned_supported_devices(self):
        values=[dict(bus=0,slot=d['slot'],function=0,id=dict(vendor=0x8086,device=0x10d3 if label=='ethernet' else 0x2668),
                     class_info={'class':0x200 if label=='ethernet' else 0x403},regions=[dict(bar=0,type='memory',address=d['bar'])]) for label,d in DEVICES.items()]
        self.assertEqual(D.device_locations([dict(bus=0,devices=values)]),DEVICES)
        with self.assertRaises(AssertionError):D.device_locations(values+[values[0]])
        values[0]['regions'][0]['address']=0
        with self.assertRaises(AssertionError):D.device_locations(values)

    def test_real_counter_formats_required(self):
        self.assertEqual(D.parse_counters('Packets: rx=8 tx=9 dropped=0')['rx'],8)
        self.assertEqual(D.parse_audio('PCM: 48000 Hz, 2 channels, 16 bits, queued=1024 played=8192 frames')['played'],8192)
        for function,text in ((D.parse_counters,'link up'),(D.parse_audio,'speaker started'),(D.parse_audio,'PCM: 44100 Hz, 2 channels, 16 bits, queued=1 played=2 frames')):
            with self.assertRaises(AssertionError):function(text)

    def test_partial_pcm_checks_actual_samples_without_full_sequence(self):
        with tempfile.TemporaryDirectory(prefix='power-device-pcm-') as temporary:
            path=Path(temporary)/'partial.wav'
            header=bytearray(44);header[:4]=b'RIFF';header[8:16]=b'WAVEfmt ';header[36:40]=b'data'
            struct.pack_into('<HHI',header,20,1,2,48000);struct.pack_into('<H',header,34,16)
            path.write_bytes(header+struct.pack('<1024h',*([500,-500]*512)))
            self.assertEqual(D.partial_pcm(path)['frames'],512)
            path.write_bytes(header+bytes(2048))
            with self.assertRaises(AssertionError):D.partial_pcm(path)


class AudioCaptureLaunchTests(unittest.TestCase):
    def test_opt_in_capture_rate_and_legacy_default_commands_and_configuration(self):
        for rate in (None,48000):
            with self.subTest(rate=rate),tempfile.TemporaryDirectory(prefix='power-device-audio-launch-') as temporary,contextlib.ExitStack() as resources:
                folder=Path(temporary);(folder/'unread.iso').write_bytes(b'opaque mocked ISO command fixture');reader,writer=os.pipe()
                stdout=resources.enter_context(os.fdopen(reader,'rb',buffering=0));resources.callback(os.close,writer)
                process=mock.Mock(stdout=stdout);process.poll.return_value=0
                with (mock.patch.object(R,'WORK',folder),mock.patch.object(R.subprocess,'Popen',return_value=process) as launch,
                      mock.patch.object(R.VM,'until_prompt')):
                    options={} if rate is None else {'audio_capture_rate':rate}
                    vm=R.VM(4,iso=folder/'unread.iso',storage=False,network=False,
                            audio_capture=folder/'capture.wav',audio_device='hda',**options)
                    try:
                        command=launch.call_args.args[0];backend=command[command.index('-audiodev')+1]
                        if rate is None:
                            self.assertEqual(backend,'wav,id=audio0,path='+str(folder/'capture.wav'))
                            self.assertNotIn('audio_capture_rate',vm.configuration)
                        else:
                            self.assertIn('out.fixed-settings=on,out.frequency=48000,out.channels=2,out.format=s16',backend)
                            self.assertEqual(vm.configuration['audio_capture_rate'],48000)
                        self.assertIn('hda-duplex,audiodev=audio0',command)
                    finally:vm.close()


class TerminalSerialTests(unittest.TestCase):
    @contextlib.contextmanager
    def vm(self, tail):
        with tempfile.TemporaryDirectory(prefix='power-device-terminal-pipes-') as temporary,contextlib.ExitStack() as resources:
            serial_read,serial_write=os.pipe();qmp_read,qmp_write=os.pipe()
            for fd in (serial_write,qmp_read,qmp_write):resources.callback(os.close,fd)
            reader=resources.enter_context(os.fdopen(serial_read,'rb',buffering=0))
            log=resources.enter_context((Path(temporary)/'serial.log').open('wb'))
            log.write(b'Power request accepted\nos:/$ ');log.flush()
            selector=resources.enter_context(selectors.DefaultSelector());selector.register(reader,selectors.EVENT_READ)
            event={'event':'SHUTDOWN','data':{'reason':'guest-shutdown','guest':True}}
            os.write(qmp_write,json.dumps(event).encode()+b'\n')
            sent=[]
            def qmp(command):
                self.assertEqual(command,'query-status')
                if not sent and tail:
                    # Bytes arrive during QMP status, after the initial drain.
                    os.write(serial_write,tail)
                sent.append(command)
                return {'running':False,'status':'shutdown'}
            process=types.SimpleNamespace(stdout=reader,poll=lambda:None)
            vm=types.SimpleNamespace(process=process,selector=selector,pending=bytearray(),log=log,
                                     qmp_pending=bytearray(),qmp_output=qmp_read,qmp=qmp)
            yield vm,event

    def test_actual_pipes_drain_serial_that_arrives_during_stopped_status_query(self):
        tail=b'power: preparation BSP0 IF=1\nreversible park acknowledged\nflush complete\npower: COMMIT\n'
        with self.vm(tail) as (vm,event):
            evidence={};self.assertEqual(D.terminal(vm,'poweroff',.5,evidence),[event])
            D.check_terminal_log(D.plain(vm))
        observed=evidence['terminal_serial_drain']
        self.assertTrue(observed['required_markers_observed'])
        self.assertGreater(observed['characters_after_drain'],observed['characters_at_pause'])
        self.assertEqual(evidence['qmp_status_after_event']['running'],False)

    def test_paused_event_without_serial_markers_still_fails_and_preserves_event(self):
        with self.vm(b'') as (vm,event):
            evidence={}
            with self.assertRaises(AssertionError):D.terminal(vm,'poweroff',.08,evidence)
        self.assertEqual(evidence['qmp_events'],[event])
        self.assertFalse(evidence['terminal_serial_drain']['required_markers_observed'])

    def test_late_serial_failure_is_rejected_even_with_all_markers(self):
        tail=b'reversible park acknowledged\nflush complete\npower: COMMIT\nPANIC: late fault\n'
        with self.vm(tail) as (vm,event):
            with self.assertRaises(AssertionError):D.terminal(vm,'poweroff',.5)


class SafetyTests(unittest.TestCase):
    def arguments(self):
        return ['--iso','/FINAL/os.iso','--kernel','/FINAL/kernel','--shell','/FINAL/sh','--gui','/FINAL/gui','--kind','poweroff']
    def test_default_is_print_only_no_images_servers_processes(self):
        with mock.patch.object(D,'scenario') as scenario,mock.patch.object(D,'verify_image') as verify,mock.patch.object(D,'Stream') as stream,contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(D.main(self.arguments()),0)
        scenario.assert_not_called();verify.assert_not_called();stream.assert_not_called()
        self.assertEqual(json.loads(output.getvalue())['result'],'not-run')

    def test_execute_without_pilot_fails_before_output_or_input_access(self):
        with mock.patch.dict(os.environ,{},clear=True),mock.patch.object(D.WORK.__class__,'mkdir') as mkdir,mock.patch.object(D,'verify_image') as verify:
            with self.assertRaisesRegex(RuntimeError,'reserved pilot'):D.main(self.arguments()+['--execute'])
        mkdir.assert_not_called();verify.assert_not_called()

    def test_fixed_resource_shape_cannot_be_silently_reduced(self):
        with self.assertRaises(SystemExit),contextlib.redirect_stderr(io.StringIO()):D.main(self.arguments()+['--memory','384M'])

    def test_pilot_actual_marker_ownership_and_recovery(self):
        with tempfile.TemporaryDirectory(prefix='power-device-marker-') as temporary:
            root=Path(temporary);(root/'build').mkdir();marker=root/'build/browser-guests-active';marker.write_text('qemu-file-ram:owned\n')
            env=dict(COAST_QEMU_FILE_RAM='1',COAST_QEMU_FILE_RAM_SUPERVISOR_PID='123',COAST_QEMU_FILE_RAM_SLOT_TOKEN='qemu-file-ram:owned')
            with mock.patch.object(D.os,'getppid',return_value=123),mock.patch.object(D.os,'getpid',return_value=456),mock.patch.object(D.os,'getpgrp',return_value=456):
                D.admitted(env,root)
                marker.write_text('another owner')
                with self.assertRaises(RuntimeError):D.admitted(env,root)
                marker.write_text('qemu-file-ram:owned\n');(root/'build/release-file-ram-recovery-required.json').symlink_to(root/'missing')
                with self.assertRaisesRegex(RuntimeError,'cleanup'):D.admitted(env,root)

    def test_owned_teardown_does_not_close_resources_before_child_reaped(self):
        process=mock.Mock();process.poll.return_value=None;process.wait.side_effect=[D.subprocess.TimeoutExpired('fake',2),0]
        vm=types.SimpleNamespace(process=process,close=mock.Mock())
        D.close_vm(vm)
        process.terminate.assert_called_once();process.kill.assert_called_once();self.assertEqual(process.wait.call_count,2);vm.close.assert_called_once()

    def test_nested_host_tools_keep_owned_group_and_reject_preexec(self):
        child=mock.Mock();child.poll.return_value=0
        libc=mock.Mock();libc.prctl.return_value=0
        with (mock.patch.object(D.os,'getpid',return_value=456),mock.patch.object(D.os,'getpgrp',return_value=456),
              mock.patch.object(D.ctypes,'CDLL',return_value=libc),mock.patch.object(D.subprocess,'Popen',return_value=child) as launch,
              mock.patch.object(D.Path,'iterdir',return_value=iter([])),mock.patch.object(D.os,'waitpid',side_effect=ChildProcessError)):
            with D.owned_children():
                D.subprocess.Popen(['fake-owned-tool'],start_new_session=True,process_group=0)
                with self.assertRaisesRegex(RuntimeError,'preexec'):D.subprocess.Popen(['not-launched'],preexec_fn=lambda:None)
        self.assertEqual(launch.call_count,1)
        self.assertFalse(launch.call_args.kwargs['start_new_session'])
        self.assertIsNone(launch.call_args.kwargs['process_group'])
        self.assertEqual(libc.prctl.call_count,3)

    def test_cleanup_blocker_never_overwrites_another_owner(self):
        with tempfile.TemporaryDirectory(prefix='power-device-blocker-') as temporary:
            root=Path(temporary);(root/'build').mkdir();folder=root/'case';folder.mkdir()
            path=root/'build/release-file-ram-recovery-required.json'
            with mock.patch.object(D,'ROOT',root):
                D.record_cleanup_blocker(folder,[{'pid':123,'start':456,'group':789}])
                first=path.read_bytes();D.record_cleanup_blocker(folder,[])
            self.assertEqual(first,path.read_bytes())
            self.assertTrue(json.loads(first)['manual_recovery_required'])

    def test_mocked_whole_workflow_keeps_final_iso_and_snapshots_before_teardown(self):
        with tempfile.TemporaryDirectory(prefix='power-device-flow-') as temporary:
            folder=Path(temporary);(folder/'devices.trace').write_text('')
            args=types.SimpleNamespace(iso=folder/'FINAL.iso',kind='poweroff',memory='2G',timeout=60)
            args.iso.write_bytes(b'fake read-only final ISO');before_iso=D.digest(args.iso)
            calls=[];closed=[];infos=[0];net=[0]
            class FakeVM:
                def __init__(self,*pos,**options):
                    calls.append((pos,options));self.configuration=dict(options);self.log=types.SimpleNamespace(name=str(folder/(str(len(calls))+'.log')))
                    Path(self.log.name).write_text('Network: Intel Ethernet 10d3\nAudio: HDA 8086:2668, 1 analog outputs')
                def qmp(self,command):return []
                def command(self,line,*pos):
                    if line=='netinfo':
                        net[0]+=1;return 'Packets: rx=%d tx=%d dropped=0'%(net[0]*8,net[0]*9)
                    if line=='power':return 'power_state=idle poweroff_ready=1 reset_ready=1'
                    if line=='poweroff':(folder/'devices.trace').write_text(trace())
                    return 'started pid 4'
                def shell_command(self,line,*pos,**options):
                    if 'speaker-check --info' in line:
                        infos[0]+=1;return 'PCM: 48000 Hz, 2 channels, 16 bits, queued=1024 played=%d frames'%(infos[0]*8192)
                    return 'done'
            fixture=types.SimpleNamespace(url='http://10.0.2.2:123/stream',stats=dict(bytes_sent=4096,active=True,completed=False,last_write=D.time.monotonic()),close=mock.Mock())
            regs_before=dict(ethernet=dict(rctl=2,tctl=2,ctrl=0),hda=dict(stream_ctl=2,corb_ctl=2,rirb_ctl=2,stream_offset=0x100,position_base=1,intctl=1))
            regs_after=copy.deepcopy(regs_before)
            for group in regs_after.values():
                for key in group:
                    if key!='stream_offset':group[key]=0
            original_trace=D.trace_evidence
            def checked_trace(*arguments):
                self.assertFalse(closed,'Terminal trace was read after host teardown')
                return original_trace(*arguments)
            payload=('POWER_DEVICES_'+folder.name+'\n').encode()
            def fake_run(*cmd):return payload if cmd[0]=='mtype' else b''
            with (mock.patch.object(D,'PowerVM',FakeVM),mock.patch.object(D,'Stream',return_value=fixture),
                  mock.patch.object(D,'run',side_effect=fake_run),mock.patch.object(D,'plain',side_effect=lambda vm:Path(vm.log.name).read_text()),
                  mock.patch.object(D,'device_locations',return_value=DEVICES),mock.patch.object(D,'registers',side_effect=[regs_before,regs_after]),
                  mock.patch.object(D,'terminal',return_value=[{'event':'SHUTDOWN'}]),mock.patch.object(D,'trace_evidence',side_effect=checked_trace),
                  mock.patch.object(D,'partial_pcm',return_value={'frames':512}),mock.patch.object(D,'close_vm',side_effect=lambda vm:closed.append(vm))):
                report={};D.scenario(args,folder,report)
            self.assertEqual(report['result'],'passed');self.assertEqual(D.digest(args.iso),before_iso)
            self.assertEqual(len(calls),2);self.assertEqual(len(closed),2)
            self.assertTrue(calls[0][1]['network']);self.assertEqual(calls[0][1]['network_model'],'e1000e')
            self.assertEqual(calls[0][1]['audio_device'],'hda');self.assertEqual(calls[0][1]['audio_capture_rate'],48000);self.assertEqual(calls[0][0],(4,))
            self.assertIn('-no-shutdown',calls[0][1]['extra_args']);self.assertFalse(calls[1][1]['network'])
            self.assertTrue((folder/'terminal.trace').is_file())

    def test_evidence_hashes_and_original_serial_are_preserved(self):
        with tempfile.TemporaryDirectory(prefix='power-device-evidence-') as temporary:
            folder=Path(temporary)/'case';folder.mkdir();serial=Path(temporary)/'original.log';serial.write_text('real guest log')
            (folder/'report.json').write_text('{"result":"failed"}')
            D.evidence_manifest(folder,{'serial_log':str(serial)})
            rows=json.loads((folder/'evidence-manifest.json').read_text())
            self.assertEqual({r['path'] for r in rows},{'report.json','serial_log.log'})
            self.assertTrue(all(D.digest(folder/r['path'])==r['sha256'] for r in rows));self.assertEqual(serial.read_text(),'real guest log')


class LocalSupervisorTests(unittest.TestCase):
    """Host processes only: private IPC, owned sleep fixtures, no guest/server."""
    def arguments(self):
        return ['--iso','/FINAL/os.iso','--kernel','/FINAL/kernel','--shell','/FINAL/sh',
                '--gui','/FINAL/gui','--kind','poweroff']

    def test_local_plan_does_not_reserve_read_inputs_or_launch(self):
        with (mock.patch.object(L,'supervise') as supervise,
              mock.patch.object(L.Slot,'acquire') as acquire,
              contextlib.redirect_stdout(io.StringIO()) as output):
            self.assertEqual(L.main(self.arguments()),0)
        supervise.assert_not_called();acquire.assert_not_called()
        value=json.loads(output.getvalue());self.assertEqual(value['result'],'not-run')
        self.assertIn('local',value['command'])

    def test_execution_requires_attestation_and_realistic_memory_floor(self):
        for extra in (['--execute'],['--execute','--exclusive-ready'],
                      ['--execute','--exclusive-ready','--min-available-kib','1']):
            with self.subTest(extra=extra),contextlib.redirect_stderr(io.StringIO()),self.assertRaises(SystemExit):
                L.arguments(self.arguments()+extra)
        self.assertEqual(L.arguments(self.arguments()+['--execute','--exclusive-ready',
                         '--min-available-kib',str(L.MIN_AVAILABLE_KIB)]).min_available_kib,L.MIN_AVAILABLE_KIB)

    def test_local_environment_strings_cannot_replace_inherited_admission(self):
        with tempfile.TemporaryDirectory(prefix='power-device-local-admission-') as temporary:
            root=Path(temporary);(root/'build').mkdir()
            (root/'build/browser-guests-active').write_text('power-devices-local:invented\n')
            with self.assertRaisesRegex(RuntimeError,'inherited'):
                D.admitted({'COAST_QEMU_FILE_RAM':'1'},root,mode='local')
        with (mock.patch.dict(os.environ,{},clear=True),mock.patch.object(D.Path,'mkdir') as mkdir,
              mock.patch.object(D,'verify_image') as verify):
            with self.assertRaisesRegex(RuntimeError,'inherited'):
                D.main(self.arguments()+['--execute','--admission','local'])
        mkdir.assert_not_called();verify.assert_not_called()

    def test_local_socket_peer_must_be_direct_parent(self):
        # A socket made by this process is not a capability from our parent.
        parent,child=L.socket.socketpair(L.socket.AF_UNIX,L.socket.SOCK_STREAM)
        try:
            fd=os.dup(child.fileno())
            with self.assertRaisesRegex(RuntimeError,'direct parent'):
                L.admit_child({L.LOCAL_FD:str(fd)})
        finally:
            parent.close();child.close()

    def test_local_lease_rejects_stale_parent_child_session_marker_and_memory(self):
        with tempfile.TemporaryDirectory(prefix='power-device-local-lease-') as temporary:
            root=Path(temporary);(root/'build').mkdir();slot=L.Slot(root);slot.acquire()
            child=dict(pid=456,parent=123,group=456,session=456,start=100,state='R')
            parent=dict(pid=123,parent=1,group=123,session=123,start=90,state='S')
            lease=dict(protocol=1,root=str(root),child=child,supervisor=parent,
                       slot=slot.identity,min_available_kib=L.MIN_AVAILABLE_KIB)
            def identity(pid):return copy.deepcopy(child if pid==456 else parent)
            with (mock.patch.object(L.os,'getpid',return_value=456),
                  mock.patch.object(L.os,'getppid',return_value=123),
                  mock.patch.object(L,'process_identity',side_effect=identity),
                  mock.patch.object(L,'available_kib',return_value=L.MIN_AVAILABLE_KIB)):
                self.assertEqual(L.verify_lease(lease,root),L.MIN_AVAILABLE_KIB)
                for group,key in (('child','start'),('child','session'),('supervisor','start')):
                    broken=copy.deepcopy(lease);broken[group][key]+=1
                    with self.subTest(group=group,key=key),self.assertRaises(RuntimeError):
                        L.verify_lease(broken,root)
                broken=copy.deepcopy(lease);broken['slot']['token']+='wrong'
                with self.assertRaisesRegex(RuntimeError,'marker/token'):L.verify_lease(broken,root)
                with mock.patch.object(L,'available_kib',return_value=L.MIN_AVAILABLE_KIB-1):
                    with self.assertRaisesRegex(RuntimeError,'below required'):L.verify_lease(lease,root)
            slot.release()

    def test_atomic_slot_refuses_existing_file_and_dangling_symlink(self):
        with tempfile.TemporaryDirectory(prefix='power-device-local-slot-') as temporary:
            root=Path(temporary);(root/'build').mkdir();path=root/'build/browser-guests-active'
            path.write_text('another stage owns this');before=path.read_bytes()
            with self.assertRaises(FileExistsError):L.Slot(root).acquire()
            self.assertEqual(path.read_bytes(),before)
            path.unlink();path.symlink_to(root/'missing')
            with self.assertRaises(FileExistsError):L.Slot(root).acquire()
            self.assertTrue(path.is_symlink());self.assertFalse((root/'missing').exists())

    def test_slot_releases_only_exact_owned_inode_and_token(self):
        with tempfile.TemporaryDirectory(prefix='power-device-local-slot-') as temporary:
            root=Path(temporary);(root/'build').mkdir();path=root/'build/browser-guests-active'
            slot=L.Slot(root);slot.acquire();slot.release();self.assertFalse(path.exists())
            slot=L.Slot(root);slot.acquire();path.write_text('another owner')
            with self.assertRaisesRegex(RuntimeError,'ownership changed'):slot.release()
            self.assertEqual(path.read_text(),'another owner')
            path.unlink();slot=L.Slot(root);slot.acquire()
            original=root/'saved-marker';path.rename(original);path.write_text(slot.token+'\n')
            with self.assertRaisesRegex(RuntimeError,'ownership changed'):slot.release()
            self.assertTrue(original.exists());self.assertTrue(path.exists())

    def test_recovery_blocker_and_memory_refuse_before_acquiring_or_launching(self):
        with tempfile.TemporaryDirectory(prefix='power-device-local-refusal-') as temporary:
            root=Path(temporary);(root/'build').mkdir();blocker=root/'build/release-file-ram-recovery-required.json'
            blocker.symlink_to(root/'missing')
            with mock.patch.object(L.subprocess,'Popen') as launch:
                with self.assertRaisesRegex(RuntimeError,'cleanup'):
                    L.supervise(['never-launched'],root=root,folder=root/'report',floor=L.MIN_AVAILABLE_KIB,timeout=1)
            launch.assert_not_called();self.assertTrue(blocker.is_symlink())
            self.assertFalse((root/'build/browser-guests-active').exists())
            blocker.unlink()
            with mock.patch.object(L,'available_kib',return_value=L.MIN_AVAILABLE_KIB-1),mock.patch.object(L.Slot,'acquire') as acquire:
                with self.assertRaisesRegex(RuntimeError,'below required'):
                    L.supervise(['never-launched'],root=root,folder=root/'report',floor=L.MIN_AVAILABLE_KIB,timeout=1)
            acquire.assert_not_called()

    def worker(self,root,mode):
        # Child code first installs its handler, then records its stable identity.
        descendant=("import signal,time;from pathlib import Path;"
                    "import run_power_devices_local as L;"
                    "signal.signal(signal.SIGTERM,signal.SIG_IGN);"
                    "L.write_json(Path("+repr(str(root/'descendant.json'))+"),L.process_identity(__import__('os').getpid()));"
                    "time.sleep(60)")
        script='''import json,os,signal,subprocess,sys,time
from pathlib import Path
import run_power_devices_local as L
root=Path(sys.argv[1]);lease=L.admit_child(root=root)
print(json.dumps({'admitted':lease}),flush=True)
'''
        if mode=='success':
            return [sys.executable,'-B','-u','-c',script,str(root)]
        script+='child=subprocess.Popen([sys.executable,"-B","-c",'+repr(descendant)+'],start_new_session='+str(mode=='escape')+')\n'
        script+='deadline=time.monotonic()+3\nwhile not (root/"descendant.json").is_file():\n if time.monotonic()>deadline: raise RuntimeError("Host fixture not ready")\n time.sleep(.01)\n'
        if mode=='interrupt':script+='os.kill(os.getppid(),signal.SIGTERM)\n'
        script+=('raise SystemExit(7)\n' if mode in ('orphan','escape') else 'time.sleep(60)\n')
        return [sys.executable,'-B','-u','-c',script,str(root)]

    def run_worker(self,root,mode,timeout=5):
        folder=root/'report'
        # PYTHONPATH only locates the reviewed host admission helper in fixtures.
        with (mock.patch.dict(os.environ,{'PYTHONPATH':str(MODULE.parent)}),
              mock.patch.object(L,'TERM_GRACE',.15),mock.patch.object(L,'KILL_GRACE',1.0)):
            return L.supervise(self.worker(root,mode),root=root,folder=folder,
                               floor=L.MIN_AVAILABLE_KIB,timeout=timeout)

    def test_real_direct_admission_and_successful_owned_release(self):
        with tempfile.TemporaryDirectory(prefix='power-device-local-real-') as temporary:
            root=Path(temporary);(root/'build').mkdir();report=self.run_worker(root,'success')
            self.assertEqual(report['result'],'passed',report)
            self.assertTrue(report['cleanup_verified']);self.assertTrue(report['marker_released'])
            lease=json.loads((root/'report/harness.log').read_text())['admitted']
            self.assertEqual(lease['child']['parent'],os.getpid())
            self.assertEqual(lease['child']['group'],lease['child']['pid'])
            self.assertEqual(lease['child']['session'],lease['child']['pid'])
            self.assertFalse((root/'build/browser-guests-active').exists())
            self.assertFalse((root/'build/release-file-ram-recovery-required.json').exists())

    def test_real_orphan_and_escaped_descendant_cleanup_spares_unrelated_child(self):
        outsider=subprocess.Popen([sys.executable,'-B','-c','import time;time.sleep(60)'],start_new_session=True)
        try:
            for mode in ('orphan','escape'):
                with self.subTest(mode=mode),tempfile.TemporaryDirectory(prefix='power-device-local-orphan-') as temporary:
                    root=Path(temporary);(root/'build').mkdir();report=self.run_worker(root,mode)
                    self.assertEqual(report['result'],'failed',report)
                    self.assertEqual(report.get('child_returncode'),7,report)
                    self.assertTrue(report['cleanup_verified'],report)
                    descendant=json.loads((root/'descendant.json').read_text())
                    self.assertIsNone(L.process_identity(descendant['pid']))
                    self.assertIsNone(outsider.poll(),'Unrelated pre-existing child was signaled')
                    self.assertFalse((root/'build/browser-guests-active').exists())
        finally:
            outsider.terminate();outsider.wait(timeout=3)

    def test_real_timeout_and_sigterm_cleanup(self):
        for mode in ('timeout','interrupt'):
            with self.subTest(mode=mode),tempfile.TemporaryDirectory(prefix='power-device-local-interrupt-') as temporary:
                root=Path(temporary);(root/'build').mkdir()
                report=self.run_worker(root,mode,timeout=.5 if mode=='timeout' else 5)
                self.assertEqual(report['result'],'failed',report);self.assertTrue(report['cleanup_verified'],report)
                if mode=='timeout':self.assertTrue(report.get('timed_out'),report)
                else:self.assertIn('signal 15',report['error'])
                descendant=json.loads((root/'descendant.json').read_text())
                self.assertIsNone(L.process_identity(descendant['pid']))
                self.assertFalse((root/'build/browser-guests-active').exists())

    def test_unconfirmed_cleanup_retains_reservation_and_never_overwrites_blocker(self):
        with tempfile.TemporaryDirectory(prefix='power-device-local-blocked-') as temporary:
            root=Path(temporary);(root/'build').mkdir()
            unresolved=[dict(pid=987654,start=123,group=987654,session=987654,parent=1,state='S')]
            with mock.patch.object(L.OwnedProcesses,'cleanup',return_value=unresolved),mock.patch.object(L.OwnedProcesses,'observe',return_value=unresolved):
                report=self.run_worker(root,'success')
            self.assertEqual(report['result'],'failed');self.assertFalse(report['cleanup_verified'])
            self.assertTrue((root/'build/browser-guests-active').exists())
            blocker=root/'build/release-file-ram-recovery-required.json';before=blocker.read_bytes()
            L.record_blocker(root,root/'report',[], 'different failure')
            self.assertEqual(blocker.read_bytes(),before)
            self.assertTrue(json.loads(before)['manual_recovery_required'])

    def test_reopen_rechecks_local_lease_and_cloud_calls_stay_unchanged(self):
        with mock.patch.object(L,'verify_lease') as verify:
            D.check_boot_admission(types.SimpleNamespace(_admission={'mode':'local'}))
        verify.assert_called_once_with({'mode':'local'},D.ROOT)
        with mock.patch.object(L,'verify_lease') as verify:
            D.check_boot_admission(types.SimpleNamespace(_admission={'mode':'cloud'}))
        verify.assert_not_called()


if __name__=='__main__':unittest.main(verbosity=2)

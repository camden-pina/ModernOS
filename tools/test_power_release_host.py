#!/usr/bin/env python3
"""Pure Python release-runner checks; no ISO tooling, guests or compilers."""
import argparse,io,json,pathlib,struct,subprocess,tarfile,tempfile,unittest
from unittest.mock import patch
import test_power_release as R
import run_power_validation as P

def elf(tag):
    data=bytearray(64);data[:7]=b'\x7fELF\x02\x01\x01';struct.pack_into('<HH',data,16,2,62)
    return bytes(data)+tag.encode()
class ReleaseChecks(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.root=pathlib.Path(self.temp.name)
        self.args=argparse.Namespace(iso=self.root/'release.iso',kernel=self.root/'kernel',shell=self.root/'shell',gui=self.root/'gui')
        self.args.iso.write_bytes(b'read-only release iso')
        for key in ('kernel','shell','gui'):getattr(self.args,key).write_bytes(elf(key))
        self.shell_bytes=self.args.shell.read_bytes();self.gui_bytes=self.args.gui.read_bytes()
        self.kernel=self.args.kernel.read_bytes();self.config='kernel=/boot/kernel.elf\nmodule=/boot/initramfs.tar\ncmdline=\n'
        self.limine='/Coast\n protocol: limine\n cmdline: \n';self.members=['bin/coast-sh','bin/gui']
        self.folder=self.root/'inspect';self.folder.mkdir()
    def tearDown(self):self.temp.cleanup()
    def fake_run(self,*args):
        source=args[-2];target=pathlib.Path(args[-1])
        if source=='/boot/kernel.elf':target.write_bytes(self.kernel)
        elif source=='/boot/coast.conf':target.write_text(self.config)
        elif source=='/boot/limine/limine.conf':target.write_text(self.limine)
        elif source=='/boot/initramfs.tar':
            with tarfile.open(target,'w') as archive:
                for name in self.members:
                    data=self.shell_bytes if name.endswith('coast-sh') else self.gui_bytes
                    item=tarfile.TarInfo(name);item.size=len(data);archive.addfile(item,io.BytesIO(data))
        else:raise AssertionError(args)
        return b''
    def verify(self):
        with patch.object(R,'run',side_effect=self.fake_run):return R.verify_image(self.args,self.folder)
    def test_provenance_no_source_modification(self):
        before={key:R.digest(getattr(self.args,key)) for key in ('iso','kernel','shell','gui')}
        result=self.verify();self.assertFalse(result['image_modified']);self.assertIn('initramfs.tar',result['embedded'])
        self.assertEqual(before,{key:R.digest(getattr(self.args,key)) for key in before})
    def test_kernel_mismatch(self):
        self.kernel=elf('wrong kernel')
        with self.assertRaisesRegex(AssertionError,'embedded kernel'):self.verify()
    def test_limine_test_token_rejected(self):
        self.limine+=' power.test=dry-run\n'
        with self.assertRaisesRegex(AssertionError,'test boot token'):self.verify()
    def test_native_test_token_rejected(self):
        self.config+='cmdline=acpi.selftest\n'
        with self.assertRaisesRegex(AssertionError,'test boot token'):self.verify()
    def test_gui_mismatch(self):
        self.args.gui.write_bytes(elf('stale gui expectation'))
        with self.assertRaisesRegex(AssertionError,'mismatch'):self.verify()
    def test_duplicate_and_missing_gui(self):
        self.members.append('bin/gui')
        with self.assertRaisesRegex(AssertionError,'duplicate'):self.verify()
        self.members=['bin/coast-sh']
        with self.assertRaisesRegex(AssertionError,'mismatch'):self.verify()
    def test_terminal_requires_guest_reason_and_real_phases(self):
        vm=argparse.Namespace(qmp_output=1,qmp_pending=bytearray(),process=argparse.Namespace(poll=lambda:0))
        event={'event':'SHUTDOWN','data':{'reason':'guest-reset','guest':True}}
        phases='power: COMMIT flush complete reversible park acknowledged Power request accepted'
        with patch.object(R,'drain'),patch.object(R,'plain',return_value=phases),patch.object(R.select,'select',return_value=([1],[],[])),patch.object(R.os,'read',return_value=(json.dumps(event)+'\n').encode()):
            self.assertEqual(R.terminal_event(vm,'guest-reset',1),[event])
        vm.qmp_pending=bytearray()
        with patch.object(R,'drain'),patch.object(R,'plain',return_value='Power request accepted'),patch.object(R.select,'select',return_value=([1],[],[])),patch.object(R.os,'read',return_value=(json.dumps(event)+'\n').encode()):
            with self.assertRaises(AssertionError):R.terminal_event(vm,'guest-reset',1)
    def test_gui_captures_foreground_power_before_review(self):
        calls=[]
        class FakeDesktop:
            def __init__(self,vm):pass
            def screenshot(self,name):calls.append(('screenshot',name))
            def launch(self,name):calls.append(('launch',name));return 7
            def key(self,*keys):calls.append(('key',keys))
            def expect(self,*details):calls.append(('expect',details))
        vm=argparse.Namespace(send=lambda *x:None,until=lambda *x:None)
        with patch.object(R,'DesktopTest',FakeDesktop),patch.object(R.time,'sleep'),patch.object(R,'plain',return_value=''):
            self.assertEqual(R.gui_request(vm,'unique-release'),7)
        launch=calls.index(('launch','System & Power'))
        self.assertEqual(calls[launch+1],('screenshot','unique-release-system-power'))
        self.assertLess(launch+1,calls.index(('key',('tab',))))
    def test_pending_cancel_checks_real_recovery_before_teardown_sync(self):
        args=argparse.Namespace(transport='ahci',kind='cancel',cpus=4,memory='2G',iso=self.args.iso,timeout=1)
        commands=[];report={};folder=self.root/'cancel';folder.mkdir()
        class FakeVM:
            log=argparse.Namespace(name='release.log')
            def command(self,line,*unused):
                commands.append(line)
                if line=='power':return 'power_state=idle\npoweroff_ready=1\nreset_ready=1\nfirmware_prep_attempted=0\n'
                return ''
            def qmp(self,*unused):return {'running':True}
            def close(self):pass
        def command(*items):
            return ('COAST_RELEASE_POWER_cancel\n').encode() if items[0]=='mtype' else b''
        with patch.object(R,'PowerVM',return_value=FakeVM()),patch.object(R,'run',side_effect=command),patch.object(R.time,'sleep'),patch.object(R,'plain',return_value=''):
            R.scenario(args,folder,report)
        self.assertEqual(report['result'],'passed');self.assertTrue(report['teardown_sync'])
        self.assertLess(commands.index('power cancel'),commands.index('thread-check'))
        self.assertEqual(commands[-1],'sync');self.assertEqual(commands.count('poweroff'),1)
class PlanChecks(unittest.TestCase):
    def options(self):
        result=[]
        for name in ('base','iso','kernel','shell','gui','workload','vfork-workload','thread-check','ipc-check','telemetry-aml'):
            result+=['--'+name,'/tmp/final image/'+name]
        return result+['--iasl','/bin/true','--memory','2G']
    def test_exact_release_vs_enriched_matrix(self):
        args=P.parse_arguments(self.options());steps=P.command_manifest(args)
        self.assertEqual(len(steps),14)
        self.assertEqual([sum(category==x for category,_,_ in steps) for x in (P.DIRECT,P.FIXTURE,P.CONFIG)],[6,6,2])
        for category,_,command in steps:
            self.assertEqual(command[command.index('--memory')+1],'2G')
            if category==P.DIRECT:
                self.assertIn('test_power_release.py',command[1]);self.assertNotIn('--base',command)
                self.assertEqual(command[command.index('--iso')+1],str(args.iso))
        self.assertIn('test_power_desktop.py',steps[11][2][1])
    def test_execution_requires_explicit_admission(self):
        with patch('sys.stderr',io.StringIO()):
            with self.assertRaises(SystemExit):P.parse_arguments(self.options()+['--execute'])
        args=P.parse_arguments(self.options()+['--execute','--exclusive-ready','--guest-marker','/tmp/guest-marker','--min-available-kib','3145728'])
        with patch.object(P,'available_kib',return_value=1):
            with self.assertRaisesRegex(RuntimeError,'below required'):P.check_resources(args)
    def test_execution_failure_removes_only_owned_marker(self):
        with tempfile.TemporaryDirectory() as root:
            marker=pathlib.Path(root)/'guest-active'
            args=argparse.Namespace(guest_marker=marker,memory='2G',min_available_kib=3145728)
            steps=[(P.DIRECT,'test',['fake-python','fake-harness'])]
            def fail(*unused,**kwargs):
                self.assertTrue(marker.is_file())
                raise subprocess.CalledProcessError(1,'fake-harness')
            with patch.object(P,'validate_execution_inputs'),patch.object(P,'check_resources'),patch.object(P.subprocess,'run',side_effect=fail),patch('sys.stdout',io.StringIO()):
                with self.assertRaises(subprocess.CalledProcessError):P.execute_manifest(args,steps)
            self.assertFalse(marker.exists())
    def test_existing_marker_is_never_overwritten(self):
        with tempfile.TemporaryDirectory() as root:
            marker=pathlib.Path(root)/'guest-active';marker.write_text('another guest')
            args=argparse.Namespace(guest_marker=marker,memory='2G',min_available_kib=3145728)
            with patch.object(P,'validate_execution_inputs'),patch.object(P,'check_resources'),patch.object(P.subprocess,'run') as command:
                with self.assertRaises(FileExistsError):P.execute_manifest(args,[])
                command.assert_not_called()
            self.assertEqual(marker.read_text(),'another guest')
    def test_full_base_must_match_embedded_release_initramfs(self):
        with tempfile.TemporaryDirectory() as root:
            root=pathlib.Path(root);options=[]
            for name in ('base','iso','kernel','shell','gui','workload','vfork-workload','thread-check','ipc-check','telemetry-aml'):
                options+=['--'+name,str(root/name)]
            args=P.parse_arguments(options+['--iasl','/bin/true','--guest-marker',str(root/'guest-active')])
            (args.base/'iso_root').mkdir(parents=True);(args.base/'initramfs.tar').write_bytes(b'stale lean archive')
            for name in ('iso','kernel','shell','gui','workload','vfork_workload','thread_check','ipc_check','telemetry_aml'):
                getattr(args,name).write_bytes(b'fixture input')
            script=root/'harness.py';script.write_text('# no execution')
            provenance={'embedded':{'initramfs.tar':{'sha256':'different full archive'}}}
            with patch.object(P,'ROOT',root),patch.object(R,'verify_image',return_value=provenance),patch.object(P.subprocess,'run') as command:
                with self.assertRaisesRegex(ValueError,'does not match'):
                    P.validate_execution_inputs(args,[(P.FIXTURE,'test',['python',str(script)])])
                command.assert_not_called()
            self.assertFalse(args.guest_marker.exists())
if __name__=='__main__':unittest.main(verbosity=2)

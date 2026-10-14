#!/usr/bin/env python3
"""Check opt-in JSC exception observation without changing actual execution."""
import argparse
from pathlib import Path
import shlex
from test_runtime import VM, assert_success
from test_desktop import DesktopTest
from web_fixture import WebFixture


def exercise(iso, cpus, accel, label, enabled):
    fixture = WebFixture(webcore=True)
    vm = None
    try:
        child_origin = f'https://10.0.2.2:{fixture.cross_https}'
        fixture.assets['/exception-child'] = (b'''<!doctype html><script>
const original = new Error('MODERNOS_EXCEPTION_TRACE_CHILD');
try { throw original; } catch (value) {
  if (value !== original) throw Error('child exception identity changed');
}
parent.postMessage('exception-child-resumed', '*');
</script>''', 'text/html')
        fixture.assets['/exception-main'] = (('''<!doctype html>
<title>Exception execution starting</title><style>body{margin:0;background:#dd2233}</style>
<script>
const childOrigin = ''' + repr(child_origin) + ''';
let caught = 0, getters = 0, childReady = false, timerReady = false;
function observe(original) {
  try { throw original; } catch (value) {
    if (value !== original) throw Error('exception identity changed');
    ++caught;
  }
}
observe(new TypeError('MODERNOS_EXCEPTION_TRACE_CAUGHT'));
const accessorError = new Error();
Object.defineProperty(accessorError, 'name', {get() { ++getters; return 'getter'; }});
Object.defineProperty(accessorError, 'message', {get() { ++getters; return 'getter'; }});
observe(accessorError);
observe('primitive exception');
observe(new Proxy({}, {get() { ++getters; throw Error('proxy trap executed'); }}));
function indexedProbe() {
  const registers = [undefined, 'prototype', new Proxy({}, {
    get() { ++getters; throw Error('array proxy trap executed'); }
  }), 'PRIVATE_VALUE_MUST_NOT_BE_LOGGED'];
  const base = 0, key = 1;
  try { registers[base][registers[key]]; } catch (value) {
    if (!(value instanceof TypeError)) throw value;
    ++caught;
  }
}
indexedProbe();
// Keep running after the diagnostic's bounded per-process message budget.
for (let i = 0; i < 140; ++i) observe(new Error('bounded exception loop ' + i));
function complete() {
  if (!childReady || !timerReady) return;
  if (caught !== 145 || getters !== 0) throw Error('observer changed execution');
  document.body.style.background = '#11aa33';
  document.title = 'Exception execution resumed';
}
addEventListener('message', event => {
  if (event.origin === childOrigin && event.data === 'exception-child-resumed') {
    childReady = true; complete();
  }
});
setTimeout(() => { timerReady = true; complete(); }, 40);
addEventListener('DOMContentLoaded', () => {
  const child = document.createElement('iframe');
  child.style.display = 'none'; child.src = childOrigin + '/exception-child';
  document.body.appendChild(child);
});
</script>''').encode(), 'text/html')

        vm = VM(cpus, storage=False, iso=iso, label=label, accel=accel)
        assert_success(vm.command('build-sh -c ' + shlex.quote(
            'i=0; while test ! -s /etc/resolv.conf && test "$i" -lt 30; do '
            'sleep 1; i=$((i+1)); done; test -s /etc/resolv.conf'), timeout=45))
        assert_success(vm.command('build-sh -c ' + shlex.quote(
            f'curl -fsS --max-time 30 {fixture.url("ca.pem")} >> /etc/ssl/cert.pem')))
        command = 'gui --trace --stats'
        if enabled:
            command += ' --browser-exception-trace'
        vm.send(command + '\n');
        vm.until(b'GUI_READY', 40)
        desktop = DesktopTest(vm);
        desktop.screenshot(label + '-start')

        window = desktop.launch('Browser')
        for tls in (True, False):
            desktop.key('l', 'ctrl');
            vm.send(fixture.url('exception-main', tls=tls) + '\n')
            desktop.expect('BROWSER_TITLE', 'Exception execution resumed')
            pixels = desktop.screenshot(label + ('-https' if tls else '-http'))
            left = (desktop.width - 840) // 2 + (window - 1) % 5 * 16
            top = (desktop.height - 42 - 560) // 2 + (window - 1) % 5 * 16 + 40
            at = ((top + 80) * desktop.width + left + 80) * 3
            assert pixels[at:at + 3] == bytes((17, 170, 51)), 'Resumed document was not rendered'
            desktop.key('f5');
            desktop.expect('BROWSER_TITLE', 'Exception execution resumed')

        desktop.key('f4', 'alt');
        desktop.expect('CLOSE');
        desktop.exit()
        log = Path(vm.log.name).read_text(errors='replace')
        if enabled:
            for marker in ('TypeError: MODERNOS_EXCEPTION_TRACE_CAUGHT',
                           'Error: MODERNOS_EXCEPTION_TRACE_CHILD', 'JavaScript diagnostic frame',
                           'JavaScript diagnostic array registers[0]: undefined', 'web-name=prototype'):
                assert marker in log, marker
            assert 'PRIVATE_VALUE_MUST_NOT_BE_LOGGED' not in log, 'Private local value was logged'
        else:
            assert 'JavaScript diagnostic exception' not in log, 'Exception tracing enabled by default'

        for marker in ('PANIC:', 'GLib-CRITICAL', 'BROWSER_FORCED_CLOSE', 'Uncaught error in JS:'):
            assert marker not in log, marker

        print(f'PASS exception tracing {"on" if enabled else "off"}: real catch identity, '
              f'no getter/proxy calls, timer/foreign frame progress, log budget, '
              f'HTTPS/origin change/reload/cleanup ({cpus} CPUs, {accel})', flush=True)
    finally:
        if vm:
            vm.close()
        fixture.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', required=True)
    parser.add_argument('--cpus', type=int, default=4)
    parser.add_argument('--accel', choices=('kvm', 'tcg'), default='kvm')
    parser.add_argument('--label', default='native-exception-trace')
    args = parser.parse_args()

    for enabled in (False, True):
        exercise(args.iso, args.cpus, args.accel, args.label + ('-on' if enabled else '-off'), enabled)

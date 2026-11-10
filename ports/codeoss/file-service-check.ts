import { host, NativeDocument, runtime, absolutePath } from './native-model';
declare function print(value: string): void;
function check(value: boolean, message: string): void { if (!value) { throw new Error(message); } }
function expect(code: string, action: () => void): void {
	let detected = false;
	try { action(); } catch (e) { detected = (e as Error & { code?: string }).code === code; }
	check(detected, `Expected ${code}`);
}
const clock = (globalThis as typeof globalThis & { performance: { now(): number } }).performance;
const before = clock.now();
check(before >= 0 && clock.now() >= before, 'Native monotonic performance.now');
check(host.args.length === 1, 'Pass an existing empty QA directory.');
const root = absolutePath(host.args[0]);
const p = (name: string): string => `${root}/${name}`;
check(runtime.platform === 'Coast' && !runtime.node && !runtime.dom && !runtime.electron, 'Explicit truthful runtime');
check(typeof (globalThis as { process?: object }).process === 'undefined' && typeof (globalThis as { navigator?: object }).navigator === 'undefined', 'No fake Node or browser globals');
const original = '\uFEFFfirst café 😀\r\nsecond\n';
host.saveText(p('unicode.txt'), original, null);
const doc = NativeDocument.open(p('unicode.txt'));
check(doc.content === original.slice(1), 'BOM excluded from model');
check(doc.snapshot() === original, 'Upstream snapshot preserves BOM and mixed line endings');
const stable = doc.tree.createSnapshot('');
doc.insert(0, 'Edited: ');
const emoji = doc.content.indexOf('😀');
let splitRejected = false;
try { doc.delete(emoji + 1, 1); } catch (e) { splitRejected = e instanceof RangeError; }
check(splitRejected, 'Surrogate split rejected without changing model');
let prior = '';
for (let part = stable.read(); part !== null; part = stable.read()) { prior += part; }
check(prior === original.slice(1), 'Actual upstream stable snapshot');
const saved = doc.save();
check(!doc.dirty && host.readText(p('unicode.txt')) === '\uFEFFEdited: first café 😀\r\nsecond\n', 'Edit/save/read round trip');
check(!!saved.backupPath && host.readText(saved.backupPath) === original, 'Original retained byte-exact');
const first = NativeDocument.open(p('unicode.txt')), stale = NativeDocument.open(p('unicode.txt'));
first.insert(0, 'A '); const firstSave = first.save();
stale.insert(0, 'B '); const edited = stale.content;
expect('ECONFLICT', () => stale.save());
check(stale.dirty && stale.content === edited, 'Conflict retains unsaved model');
check(host.readText(p('unicode.txt')) === '\uFEFFA Edited: first café 😀\r\nsecond\n', 'Conflict preserves changed disk content');
check(host.readText(firstSave.backupPath) === '\uFEFFEdited: first café 😀\r\nsecond\n', 'Previous saved version retained');
expect('EEXIST', () => stale.saveAs(p('unicode.txt')));
stale.saveAs(p('recovered.txt'));check(!stale.dirty && host.readText(p('recovered.txt')) === stale.snapshot(), 'Save As recovers unsaved edits');
const fresh = NativeDocument.create(p('new.txt'));fresh.insert(0, 'new\n');fresh.save();check(host.readText(p('new.txt')) === 'new\n', 'Create new regular file');
expect('ENOENT', () => NativeDocument.open(p('missing.txt')));
expect('ENOENT', () => host.saveText(p('missing-parent/child.txt'), 'safe', null));
expect('EILSEQ', () => host.saveText(p('bad-surrogate.txt'), '\ud800', null));
expect('EILSEQ', () => host.saveText(p('bad-nul.txt'), 'a\0b', null));
expect('EFBIG', () => host.saveText(p('too-large.txt'), 'x'.repeat(host.maxTextBytes + 1), null));
expect('EINVAL', () => host.readText('relative-not-native'));
expect('EISDIR', () => host.readText(root));
expect('ENOTSUP', () => host.readText('/dev/null'));
print('CODEOSS_FILE_SERVICE_PASS: real native reads, upstream edit/snapshot/save/reopen, Unicode/BOM/CRLF, retained originals, conflicts, Save As recovery and checked failures.');
print(JSON.stringify(runtime));

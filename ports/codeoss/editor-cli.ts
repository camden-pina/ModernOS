import { host, NativeDocument, runtime } from './native-model';
declare function print(value: string): void;
if (host.args.length !== 1) { throw new Error('Usage: codeoss-core FILE (opens UTF-8, or starts a new file).'); }
let document: NativeDocument;
try { document = NativeDocument.open(host.args[0]); }
catch (e) { if ((e as Error & { code?: string }).code !== 'ENOENT') { throw e; } document = NativeDocument.create(host.args[0]); }
function status(): void { print(JSON.stringify({ path: document.path, characters: document.content.length, lines: document.tree.getLineCount(), dirty: document.dirty, exists: document.exists, offsets: 'UTF-16' })); }
function saved(result: { backupPath: string }): void { print('SAVED ' + document.path); if (result.backupPath) { print('Previous version retained: ' + result.backupPath); } }
print('Code OSS upstream text core on Coast — experimental CLI, not the full IDE.');
print(JSON.stringify(runtime));
print('Commands: show, status, insert OFFSET "JSON text", delete OFFSET LENGTH, save, save-as PATH, quit, quit!');
status(); print('CODEOSS_CORE_READY');
for (;;) {
	const line = host.readLine();
	if (line === null) { if (document.dirty) { throw new Error('Input ended with unsaved edits. No file was written.'); } break; }
	try {
		if (line === 'show') { print(JSON.stringify(document.content)); }
		else if (line === 'status') { status(); }
		else if (line === 'save') { saved(document.save()); }
		else if (line.startsWith('save-as ')) { saved(document.saveAs(line.slice(8))); }
		else if (line === 'quit' || line === 'quit!') { if (document.dirty && line !== 'quit!') { print('Unsaved edits. Save first, or use quit! to discard.'); } else { break; } }
		else {
			let match = /^insert (\d+) (".*")$/.exec(line);
			if (match) { document.insert(Number(match[1]), JSON.parse(match[2])); status(); }
			else { match = /^delete (\d+) (\d+)$/.exec(line); if (!match) { throw new Error('Unknown command. See the command list above.'); } document.delete(Number(match[1]), Number(match[2])); status(); }
		}
	} catch (e) {
		const error = e as Error & { code?: string; recoveryPath?: string };
		print('ERROR ' + (error.code || error.name || 'Error') + ': ' + error.message);
		if (error.recoveryPath) { print('Original retained: ' + error.recoveryPath); }
	}
}
print('CODEOSS_CORE_CLOSED');

/* Coast integration check for unmodified MIT-licensed Code OSS modules.
 * This is an editor-core port milestone, not the Electron or web workbench. */
import { PieceTreeBase, StringBuffer, createLineStartsFast } from './upstream/vscode-211a00dbedf7a224e0da0a2db3cf9cb6cb7a4bfb/src/vs/editor/common/model/pieceTreeTextBuffer/pieceTreeBase';

declare function print(value: string): void;
function check(actual: string | number, expected: string | number, name: string): void {
	if (actual !== expected) {
		throw new Error(`${name}: ${JSON.stringify(actual)} != ${JSON.stringify(expected)}`);
	}
}
function tree(text: string, eol: '\n' | '\r\n' = '\n'): PieceTreeBase {
	return new PieceTreeBase([new StringBuffer(text, createLineStartsFast(text))], eol, true);
}
const text = tree('Coast\nupstream Code OSS\nUnicode: café 😀\n');
check(text.getLineCount(), 4, 'line count');
text.insert(9, 'real ', true);
text.delete(9, 5);
check(text.getLinesRawContent(), 'Coast\nupstream Code OSS\nUnicode: café 😀\n', 'insert/delete');
const snapshot = text.createSnapshot('');
text.insert(0, 'changed\n', true);
let saved = '';
for (let part = snapshot.read(); part !== null; part = snapshot.read()) {
	saved += part;
}
check(saved, 'Coast\nupstream Code OSS\nUnicode: café 😀\n', 'stable snapshot');
const windows = tree('a\r\nb\r\n', '\r\n');
windows.insert(3, 'c\r\n', true);
check(windows.getLinesRawContent(), 'a\r\nc\r\nb\r\n', 'CRLF edits');
check(windows.getLineCount(), 4, 'CRLF lines');
const random = tree('');
let model = '', state = 123456789;
const next = (): number => { state = (state * 1664525 + 1013904223) >>> 0; return state; };
for (let i = 0; i < 2000; i++) {
	const at = next() % (model.length + 1);
	if (i % 3 === 0 && at < model.length) {
		const count = Math.min(1 + next() % 7, model.length - at);
		random.delete(at, count);
		model = model.slice(0, at) + model.slice(at + count);
	} else {
		const values = ['text', '\n', '😀', 'café', '\t'];
		const value = values[next() % values.length];
		random.insert(at, value, true);
		model = model.slice(0, at) + value + model.slice(at);
	}
	check(random.getLinesRawContent(), model, `edit ${i}`);
}
check(random.getLineCount(), model.split('\n').length, 'final lines');
for (let i = 0; i <= model.length; i += 11) {
	const p = random.getPositionAt(i);
	check(random.getOffsetAt(p.lineNumber, p.column), i, 'offset/position');
}
print('CODEOSS_CORE_CHECK_PASS: real upstream PieceTree, 2000 model-checked edits, UTF-16, CRLF, snapshots and position mapping on native JavaScriptCore.');

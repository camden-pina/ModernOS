/* Original Coast integration; the PieceTree engine remains upstream MIT code. */
import { PieceTreeBase, StringBuffer, createLineStartsFast } from './upstream/vscode-211a00dbedf7a224e0da0a2db3cf9cb6cb7a4bfb/src/vs/editor/common/model/pieceTreeTextBuffer/pieceTreeBase';
import { PlatformToString, platform, isNative, isCoast, isWeb, isElectron, OS, OperatingSystem } from './upstream/vscode-211a00dbedf7a224e0da0a2db3cf9cb6cb7a4bfb/src/vs/base/common/platform';
import { cwd, arch, platform as processPlatform } from './upstream/vscode-211a00dbedf7a224e0da0a2db3cf9cb6cb7a4bfb/src/vs/base/common/process';
export interface SaveResult { backupPath: string; }
export interface NativeHost {
	readonly protocol: number;
	readonly platform: string;
	readonly runtime: string;
	readonly arch: string;
	readonly env: Record<string, string>;
	readonly args: string[];
	readonly maxTextBytes: number;
	readonly maxPathBytes: number;
	cwd(): string;
	readText(path: string): string;
	saveText(path: string, text: string, expected: string | null): SaveResult;
	readLine(): string | null;
}
const nativeHost = (globalThis as typeof globalThis & { Coast?: NativeHost }).Coast;
if (!nativeHost || nativeHost.protocol !== 1 || !isCoast || !isNative || isWeb || isElectron || processPlatform !== 'coast' || arch !== 'x64' || OS !== OperatingSystem.Coast) {
	throw new Error('The native Coast host and explicit upstream platform adapter are required.');
}
export const host: NativeHost = nativeHost;
export const runtime = { platform: PlatformToString(platform), arch, cwd: cwd(), node: false, electron: false, dom: false, fileWatching: false, eventLoop: false };
export function absolutePath(input: string): string {
	if (!input || input.includes('\0')) { throw new Error('A nonempty file path is required.'); }
	const path = input.startsWith('/') ? input : `${cwd()}/${input}`;
	const parts: string[] = [];
	for (const part of path.split('/')) {
		if (!part || part === '.') { continue; }
		if (part === '..') { parts.pop(); } else { parts.push(part); }
	}
	return '/' + parts.join('/');
}
export class NativeDocument {
	readonly tree: PieceTreeBase;
	path: string;
	private original: string | null;
	private readonly bom: string;
	private savedContent: string;
	private constructor(path: string, original: string | null, initial: string | null = null) {
		this.path = absolutePath(path);
		this.original = original;
		this.bom = (original ?? initial)?.startsWith('\uFEFF') ? '\uFEFF' : '';
		const content = (original ?? initial ?? '').slice(this.bom.length);
		const eol = content.includes('\r\n') ? '\r\n' : '\n';
		this.tree = new PieceTreeBase([new StringBuffer(content, createLineStartsFast(content))], eol, false);
		this.savedContent = original === null ? '' : content;
	}
	static open(path: string): NativeDocument { const p = absolutePath(path); return new NativeDocument(p, host.readText(p)); }
	static create(path: string): NativeDocument { return new NativeDocument(path, null); }
	static createWithText(path: string, text: string): NativeDocument { return new NativeDocument(path, null, text); }
	get hasBOM(): boolean { return this.bom.length !== 0; }
	get content(): string { return this.tree.getLinesRawContent(); }
	get dirty(): boolean { return this.content !== this.savedContent; }
	get exists(): boolean { return this.original !== null; }
	private boundary(at: number): void {
		const content = this.content;
		if (!Number.isSafeInteger(at) || at < 0 || at > content.length) { throw new RangeError('Offset is outside the UTF-16 text.'); }
		if (at && at < content.length && /[\uD800-\uDBFF]/.test(content[at - 1]) && /[\uDC00-\uDFFF]/.test(content[at])) { throw new RangeError('Offset splits a Unicode surrogate pair.'); }
	}
	insert(at: number, value: string): void {
		this.boundary(at);
		if (typeof value !== 'string' || value.includes('\0') || /[\uD800-\uDBFF](?![\uDC00-\uDFFF])|(?<![\uD800-\uDBFF])[\uDC00-\uDFFF]/.test(value)) { throw new Error('Only valid Unicode text without NUL is supported.'); }
		if (this.content.length + value.length > host.maxTextBytes) { throw new RangeError('Text exceeds the bounded model limit.'); }
		this.tree.insert(at, value, false);
	}
	delete(at: number, count: number): void {
		this.boundary(at); this.boundary(at + count);
		if (!Number.isSafeInteger(count) || count < 0) { throw new RangeError('Delete length is invalid.'); }
		this.tree.delete(at, count);
	}
	snapshot(): string {
		const snapshot = this.tree.createSnapshot(this.bom);
		let result = '';
		for (let chunk = snapshot.read(); chunk !== null; chunk = snapshot.read()) { result += chunk; }
		return result;
	}
	save(): SaveResult {
		const value = this.snapshot();
		const result = host.saveText(this.path, value, this.original);
		this.original = value; this.savedContent = this.content;
		return result;
	}
	saveAs(path: string): SaveResult {
		const target = absolutePath(path), value = this.snapshot();
		const result = host.saveText(target, value, null);
		this.path = target; this.original = value; this.savedContent = this.content;
		return result;
	}
}

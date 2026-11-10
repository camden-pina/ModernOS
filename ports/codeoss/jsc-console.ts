/* Native JavaScriptCore shell supplies print, not a browser console.
 * Bridge logging only. Do not pretend that Node, Electron or a DOM exists. */
declare function print(value: string): void;
(globalThis as any).console = {
	log: (...values: unknown[]) => print(values.map(String).join(' ')),
	warn: (...values: unknown[]) => print(values.map(String).join(' ')),
	error: (...values: unknown[]) => print(values.map(String).join(' '))
};

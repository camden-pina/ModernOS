#!/usr/bin/env python3
"""Exercise document clocks and global aliases in real imported JavaScript."""
from browser_protocol import Browser, ROOT


def main():
    work = ROOT / 'build/browser-script-test'
    work.mkdir(parents=True, exist_ok=True)
    (work / 'clock.js').write_text('''
if (globalThis !== window || self !== window) throw new Error("Global aliases");
var origin=performance.timeOrigin, start=performance.now();
if (Math.abs(Date.now()-origin-start)>1000) throw new Error("Clock origin");
performance.timeOrigin=0;
if (performance.timeOrigin!==origin) throw new Error("Mutable clock origin");
var rejected=false;
try { performance.now.call({}); } catch (error) { rejected=error instanceof TypeError; }
if (!rejected) throw new Error("Clock receiver");
var previous=start;
while (performance.now()-start<20) {
    var current=performance.now();
    if (current<previous) throw new Error("Nonmonotonic clock");
    previous=current;
}
document.write("<title>Document clock PASS</title>");
''')
    pages = []
    for name in ('first', 'second'):
        path = work / (name + '.html')
        path.write_text(
            '<!doctype html><html><head><script src="clock.js"></script></head><body>Real script</body></html>')
        pages.append(path)

    browser = Browser(work, pages[0].as_uri())
    try:
        browser.ready('Document clock PASS', timeout=5)
        browser.title = ''
        browser.navigate(pages[1].as_uri(), 'Document clock PASS')
        print('PASS browser JavaScript: imported scripts, global aliases, real monotonic clocks, origin and receiver checks')
    finally:
        browser.close()


if __name__ == '__main__':
    main()

'use strict';
const assert = require('node:assert/strict');
const { navigationURL, downloadName } = require('../userland/chromium-browser/policy.cjs');
assert.equal(navigationURL(' https://example.com/a?q=b '), 'https://example.com/a?q=b');
assert.equal(navigationURL('http://10.0.2.2:8080/'), 'http://10.0.2.2:8080/');
assert.equal(navigationURL('example.com/path'), 'https://example.com/path');
assert.equal(navigationURL('localhost:8000'), 'https://localhost:8000/');
assert.equal(navigationURL('coast://home/'), 'coast://home/');
assert.equal(navigationURL('two words & more'), 'https://www.google.com/search?q=two%20words%20%26%20more');
for (const input of ['javascript:alert(1)', 'file:///etc/passwd', 'data:text/html,hello', 'about:blank', 'blob:https://example.com/x', 'chrome://settings', 'https://user:pass@example.com/', 'coast://elsewhere/', 'https://', '', 'x'.repeat(8193)]) {
  assert.equal(navigationURL(input), null, input);
}
assert.equal(navigationURL('unqualified words', false), null);
assert.equal(downloadName('../../etc/passwd', () => false), '_.._etc_passwd');
assert.equal(downloadName('proof.txt', name => ['proof.txt', '1-proof.txt'].includes(name)), '2-proof.txt');
assert.equal(downloadName('\u0000\\/\n', () => false), '____');
assert.equal(downloadName('.', () => false), 'download');
assert(downloadName('x'.repeat(1000), () => false).length <= 180);
assert.throws(() => downloadName('proof', () => true), /full/);
console.log('PASS Chromium address policy, unsafe schemes, credentials, download traversal and collision handling');

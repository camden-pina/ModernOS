// Load the exact source-built native modules and exercise SQLite's worker API.
const fs = require('node:fs');
const crypto = require('node:crypto');
const assert = require('node:assert/strict');
const path = require('node:path');
const wanted = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
const loaded = {};
for (const [relative, artifact] of Object.entries(wanted)) {
  const filename = path.join('/usr/share/codeoss-workbench', relative);
  assert.equal(crypto.createHash('sha256').update(fs.readFileSync(filename)).digest('hex'), artifact.sha256);
  // Its upstream binding.gyp supplies sources only for OS==win. Linux's
  // JavaScript device-id path never dlopens this intentionally empty output.
  if (relative === 'node_modules.asar.unpacked/@vscode/deviceid/build/Release/windows.node') {
    console.log('CODEOSS_ADDON_WINDOWS_ONLY ' + relative);
    continue;
  }
  const module = require(filename);
  assert.ok(module && ['object', 'function'].includes(typeof module));
  loaded[relative] = module;
  console.log('CODEOSS_ADDON_LOADED ' + relative);
}
assert.equal(Object.keys(wanted).length, 11);
assert.equal(Object.keys(loaded).length, 10);
console.log('CODEOSS_ADDONS_LOAD_PASS 10');
const sqlite = loaded['node_modules.asar.unpacked/@vscode/sqlite3/build/Release/vscode-sqlite3.node'];
// Match sqlite3.js's EventEmitter setup before using its raw native binding.
const { EventEmitter } = require('node:events');
for (const name of ['Database', 'Statement', 'Backup']) {
  Object.assign(sqlite[name].prototype, EventEmitter.prototype);
}
const database = new sqlite.Database('/home/user/source-addons-check.sqlite', 6, error => {
  assert.ifError(error);
  database.exec('CREATE TABLE coast(value INTEGER); INSERT INTO coast VALUES(42);', failure => {
    assert.ifError(failure);
    database.close(closeError => {
      assert.ifError(closeError);
      const data = fs.readFileSync('/home/user/source-addons-check.sqlite');
      assert.equal(data.subarray(0, 16).toString(), 'SQLite format 3\0');
      assert.ok(data.length >= 4096);
      console.log('CODEOSS_SQLITE_PASS');
      console.log('CODEOSS_ADDONS_ALL_PASS');
    });
  });
});

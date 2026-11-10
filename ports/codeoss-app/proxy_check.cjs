// Exercise the actual rebuilt Rust binding without making a network connection.
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const assert = require('node:assert/strict');

async function main() {
  const wanted = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
  assert.equal(Object.keys(wanted).length, 1);
  const [relative, artifact] = Object.entries(wanted)[0];
  const filename = path.join('/usr/share/codeoss-workbench', relative);
  assert.equal(crypto.createHash('sha256').update(fs.readFileSync(filename)).digest('hex'), artifact.sha256);
  const binding = require(filename);
  assert.equal(typeof binding.ProxyResolver, 'function');
  assert.equal(typeof binding.resolveProxy, 'function');
  assert.equal(typeof binding.readProxyConfig, 'function');
  for (const name of ['ALL_PROXY', 'all_proxy', 'HTTP_PROXY', 'http_proxy']) delete process.env[name];
  process.env.HTTPS_PROXY = process.env.https_proxy = 'http://127.0.0.1:8123';
  process.env.NO_PROXY = process.env.no_proxy = 'direct.test';
  const resolver = new binding.ProxyResolver();
  try {
    const config = await resolver.readProxyConfig();
    assert.equal(typeof config.autoDetect, 'boolean');
    const proxied = await resolver.resolve('https://proxy.test/resource');
    assert.ok(proxied.some(proxy => proxy.kind === 'http' && proxy.host === '127.0.0.1:8123'), JSON.stringify(proxied));
    const direct = await resolver.resolve('https://direct.test/resource');
    assert.ok(direct.some(proxy => proxy.kind === 'direct'), JSON.stringify(direct));
    resolver.reportProxyFailed({ kind: 'direct' });
    console.log('CODEOSS_PROXY_ALL_PASS exact_hash native_load configuration static_proxy no_proxy');
  } finally {
    resolver.close();
  }
}

main().catch(error => { console.error(error); process.exitCode = 1; });

'use strict';
function navigationURL(input, search = true) {
  const text = String(input).trim();
  if (!text || text.length > 8192) return null;
  if (text === 'coast://home/') return text;
  // A scheme is explicit only with :// or a known opaque URL prefix. A host
  // followed by a port is an address, never an executable/protocol operand.
  if (/^[a-z][a-z\d+.-]*:\/\//i.test(text) || /^(?:javascript|data|file|about|blob|mailto):/i.test(text)) {
    try { const u = new URL(text); return ['https:', 'http:'].includes(u.protocol) && !u.username && !u.password ? u.href : null; } catch { return null; }
  }
  if (!search) return null;
  if (!/\s/.test(text) && /^(?:localhost|[\w.-]+\.[\w.-]+)(?::\d+)?(?:[/?#].*)?$/.test(text)) {
    try { return new URL('https://' + text).href; } catch { return null; }
  }
  return 'https://www.google.com/search?q=' + encodeURIComponent(text);
}
function downloadName(input, exists) {
  let name = String(input).replace(/[\x00-\x1f\x7f/\\]/g, '_').replace(/^\.+/, '').trim().slice(0, 180) || 'download';
  for (let n = 0; n < 10000; n++) {
    const candidate = n ? n + '-' + name : name;
    if (!exists(candidate)) return candidate;
  }
  throw new Error('Download directory is full');
}
module.exports = { navigationURL, downloadName };

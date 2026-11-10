// Build-time generated Oniguruma is an ordinary JS module in jitless guests.
// Verify the caller's complete WASM input before selecting its translated twin.
async function coastOnigVerifiedInstance(input, imports) {
  let data = input && input.data !== undefined ? input.data : input;
  if (data && typeof data.arrayBuffer === 'function') data = await data.arrayBuffer();
  const bytes = data instanceof ArrayBuffer ? new Uint8Array(data) :
    ArrayBuffer.isView(data) ? new Uint8Array(data.buffer, data.byteOffset, data.byteLength) : null;
  if (!bytes || !globalThis.crypto?.subtle)
    throw new Error('Coast Oniguruma requires pinned input and SHA256 support');
  const actual = Array.from(new Uint8Array(await globalThis.crypto.subtle.digest('SHA-256', bytes)),
    byte => byte.toString(16).padStart(2, '0')).join('');
  if (actual !== '@WASM_SHA256@')
    throw new Error('Coast Oniguruma input differs from the translated module');
  return { instance: { exports: coastOnigInstantiate(imports) } };
}

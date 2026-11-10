'use strict';
const { app, BrowserWindow, WebContentsView, Menu, ipcMain, session, protocol } = require('electron');
const fs = require('node:fs');
const path = require('node:path');
const { pathToFileURL } = require('node:url');
const { navigationURL, downloadName } = require('./policy.cjs');

const HOME = 'coast://home/';
const TOP = 104;
const MAX_TABS = 4;
const qa = process.env.COAST_BROWSER_QA === '1';
const profile = qa && process.env.COAST_BROWSER_PROFILE === '1';
let window, webSession, active, nextId = 0, closing = false;
const tabs = new Map(), downloads = [];
const uiURL = pathToFileURL(path.join(__dirname, 'browser.html')).href;
protocol.registerSchemesAsPrivileged([{ scheme: 'coast', privileges: { standard: true, secure: true } }]);
app.setPath('userData', '/home/port-test/.config/CoastBrowser');
app.setName('Coast Browser');
// Use the engine's standard Linux-ABI browser identity. Electron's application
// product token and uname(Coast) otherwise send sites down unsupported-client
// paths even though this is their actual Chromium renderer.
app.userAgentFallback = `Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/${process.versions.chrome} Safari/537.36`;
// This live system loses downloaded dictionaries at reboot. Seed Chromium's
// normal dictionary directory from the pinned offline asset before any session
// starts its spellchecker, preserving spelling without startup network traffic.
const dictionaryDirectory = path.join(app.getPath('sessionData'), 'Dictionaries');
fs.mkdirSync(dictionaryDirectory, { recursive: true });
const dictionary = path.join(dictionaryDirectory, 'en-US-10-1.bdic');
if (!fs.existsSync(dictionary)) fs.copyFileSync(path.join(__dirname, 'dictionaries/en-US-10-1.bdic'), dictionary);
app.enableSandbox();
Menu.setApplicationMenu(null);

function log(event, value) {
  if (qa) console.log('COAST_BROWSER ' + JSON.stringify({ event, ...value, at: Date.now() }));
}
function current() { return tabs.get(active); }
function state() {
  return {
    active, maxTabs: MAX_TABS, tabs: [...tabs.values()].map(t => ({
      id: t.id, title: t.title, url: t.url, loading: t.loading,
      back: t.view.webContents.navigationHistory.canGoBack(),
      forward: t.view.webContents.navigationHistory.canGoForward(), error: t.error,
      secure: !t.loading && !t.error && t.url === t.view.webContents.getURL() && t.url.startsWith('https:')
    })), downloads: downloads.map(d => ({ name: d.name, state: d.state, received: d.received, total: d.total }))
  };
}
function publish() {
  if (!closing && window && !window.isDestroyed() && !window.webContents.isDestroyed()) {
    try { window.webContents.send('browser:state', state()); } catch { /* UI renderer is exiting. */ }
  }
}
function layout() {
  if (!window || window.isDestroyed()) return;
  const [width, height] = window.getContentSize();
  for (const tab of tabs.values()) tab.view.setBounds({ x: 0, y: TOP, width: Math.max(1, width), height: Math.max(1, height - TOP) });
}
function select(id, focus = true) {
  if (!tabs.has(id)) return;
  active = id;
  for (const tab of tabs.values()) tab.view.setVisible(tab.id === id && !tab.error);
  if (focus) current().view.webContents.focus();
  log('selection', { id });
  publish();
}
function focusAddress() {
  window.webContents.focus();
  window.webContents.send('browser:address');
  if (qa) window.webContents.executeJavaScript(`new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(() => resolve(document.hasFocus() && document.activeElement.id === 'address'))))`)
    .then(focused => log('address-ready', { focused }))
    .catch(error => log('address-observation-error', { message: error.message }));
}
async function navigate(tab, input) {
  const url = navigationURL(input);
  if (!url) { log('blocked-navigation', { input: String(input).slice(0, 100) }); return; }
  tab.error = ''; tab.url = url; publish();
  log('navigation-start', { id: tab.id, url });
  if (tab.id === active) { tab.view.setVisible(true); tab.view.webContents.focus(); }
  try { await tab.view.webContents.loadURL(url); }
  catch (error) {
    // The did-fail-load handler presents main-frame network failures. Aborted
    // requests during navigation/downloads are expected Chromium behavior.
    if (error.code !== 'ERR_ABORTED') log('navigation-rejected', { id: tab.id, code: error.code });
  }
}
function shortcut(wc, event, input) {
  if (input.type !== 'keyDown') return;
  const key = input.key.toLowerCase(), ctrl = input.control || input.meta;
  let handled = true;
  // Frameless Electron windows have no application Edit menu to dispatch
  // these accelerators. Route the actual user key to its focused contents.
  if (ctrl && key === 'c' || ctrl && key === 'insert') wc.copy();
  else if (ctrl && key === 'x') wc.cut();
  else if (ctrl && key === 'v' || input.shift && key === 'insert') wc.paste();
  else if (ctrl && key === 'a') wc.selectAll();
  else if (ctrl && key === 'z') input.shift ? wc.redo() : wc.undo();
  else if (ctrl && key === 'y') wc.redo();
  else if (ctrl && key === 'l') focusAddress();
  else if (ctrl && key === 't') { createTab(HOME); focusAddress(); }
  else if (ctrl && key === 'w') closeTab(active);
  else if (ctrl && (key === 'r' || key === 'f5')) reload();
  else if (key === 'f5') reload();
  else if (key === 'f11') window.setFullScreen(!window.isFullScreen());
  else if (key === 'escape') current()?.view.webContents.stop();
  else if (input.alt && key === 'arrowleft') go(-1);
  else if (input.alt && key === 'arrowright') go(1);
  else if (ctrl && key === 'tab') {
    const ids = [...tabs.keys()], index = ids.indexOf(active);
    select(ids[(index + (input.shift ? ids.length - 1 : 1)) % ids.length]);
  } else if (ctrl && /^[1-4]$/.test(key)) select([...tabs.keys()][Number(key) - 1]);
  else if (ctrl && ['+', '=', '-', '0'].includes(key)) {
    const wc = current()?.view.webContents;
    if (wc) wc.setZoomLevel(key === '0' ? 0 : Math.max(-5, Math.min(5, wc.getZoomLevel() + (key === '-' ? -0.5 : 0.5))));
  } else handled = false;
  if (handled) event.preventDefault();
}
function go(direction) {
  const history = current()?.view.webContents.navigationHistory;
  if (direction < 0 && history?.canGoBack()) history.goBack();
  if (direction > 0 && history?.canGoForward()) history.goForward();
}
function reload() {
  const tab = current(); if (!tab) return;
  if (tab.error) navigate(tab, tab.url); else tab.view.webContents.reload();
}
function observePage(tab) {
  if (qa && !tab.view.webContents.isDestroyed()) tab.view.webContents.executeJavaScript(`JSON.stringify({title:document.title,url:location.href,node:typeof process,require:typeof require,text:document.body.innerText.slice(0,500)})`)
    .then(result => log('page', { id: tab.id, result: JSON.parse(result) })).catch(() => {});
}
function observeTiming(tab, phase) {
  if (!qa || tab.view.webContents.isDestroyed()) return;
  tab.view.webContents.executeJavaScript(`JSON.stringify({url:location.href,now:performance.now(),navigation:performance.getEntriesByType('navigation').map(n=>Object.fromEntries(['duration','domainLookupStart','domainLookupEnd','connectStart','connectEnd','requestStart','responseStart','responseEnd','domInteractive','domContentLoadedEventEnd','loadEventEnd','transferSize','encodedBodySize','decodedBodySize','nextHopProtocol'].map(k=>[k,n[k]]))),paint:performance.getEntriesByType('paint').map(p=>({name:p.name,startTime:p.startTime})),resources:performance.getEntriesByType('resource').map(r=>({name:r.name.split('?')[0].slice(0,96),duration:r.duration,bytes:r.transferSize})).sort((a,b)=>b.duration-a.duration).slice(0,6)})`)
    .then(result => log('timing', { id: tab.id, phase, ...JSON.parse(result) })).catch(() => {});
}
function createTab(url = HOME) {
  if (tabs.size >= MAX_TABS) { log('tab-limit', { max: MAX_TABS }); return; }
  const view = new WebContentsView({ webPreferences: {
    session: webSession, sandbox: true, contextIsolation: true,
    nodeIntegration: false, nodeIntegrationInWorker: false, nodeIntegrationInSubFrames: false,
    webSecurity: true, allowRunningInsecureContent: false, webviewTag: false,
    navigateOnDragDrop: false, safeDialogs: true, devTools: false
  } });
  const tab = { id: ++nextId, view, title: 'New tab', url, loading: true, error: '' };
  tabs.set(tab.id, tab); window.contentView.addChildView(view); layout(); select(tab.id);
  log('tabs', { count: tabs.size });
  const wc = view.webContents;
  installEditing(wc);
  wc.on('will-navigate', (event, target) => { if (!navigationURL(target, false)) event.preventDefault(); });
  wc.on('will-redirect', (event, target) => { if (!navigationURL(target, false)) event.preventDefault(); });
  wc.on('will-frame-navigate', event => {
    // Unknown native schemes must not acquire a frame, including subframes.
    try { if (!['http:', 'https:', 'coast:', 'data:', 'blob:', 'about:'].includes(new URL(event.url).protocol)) event.preventDefault(); }
    catch { event.preventDefault(); }
  });
  wc.setWindowOpenHandler(({ url: target }) => {
    if (navigationURL(target, false)) createTab(target);
    return { action: 'deny' };
  });
  wc.on('page-title-updated', (_event, title) => { tab.title = title.slice(0, 256); publish(); observePage(tab); });
  wc.on('did-start-loading', () => { tab.loading = true; publish(); });
  wc.on('did-stop-loading', () => { tab.loading = false; publish(); });
  const locationChanged = (_event, target) => {
    tab.url = target; tab.error = '';
    if (tab.id === active) tab.view.setVisible(true);
    publish(); log('navigation', { id: tab.id, url: target });
  };
  wc.on('did-navigate', locationChanged);
  wc.on('did-navigate-in-page', (event, target, mainFrame) => { if (mainFrame) locationChanged(event, target); });
  wc.on('did-finish-load', () => {
    log('loaded', { id: tab.id, url: wc.getURL(), title: wc.getTitle(), preferences: wc.getLastWebPreferences() });
    observePage(tab);
    observeTiming(tab, 'loaded');
  });
  wc.on('dom-ready', () => observeTiming(tab, 'dom-ready'));
  wc.on('did-fail-load', (_event, code, description, target, mainFrame) => {
    if (!mainFrame || code === -3 || closing) return;
    tab.error = description; tab.loading = false; tab.url = target;
    if (tab.id === active) { tab.view.setVisible(false); focusAddress(); }
    publish(); log('load-error', { id: tab.id, code, description, url: target });
  });
  wc.on('render-process-gone', (_event, details) => {
    if (closing || !tabs.has(tab.id)) return;
    tab.error = 'Page process stopped: ' + details.reason; tab.loading = false; publish();
    if (tab.id === active) { tab.view.setVisible(false); focusAddress(); }
    log('renderer-gone', { id: tab.id, ...details });
  });
  navigate(tab, url); return tab;
}
function closeTab(id) {
  const tab = tabs.get(id); if (!tab) return;
  if (tabs.size === 1) { window.close(); return; }
  window.contentView.removeChildView(tab.view); tabs.delete(id); tab.view.webContents.close();
  if (active === id) select([...tabs.keys()].at(-1));
  log('tabs', { count: tabs.size });
  publish();
}
function installEditing(wc) {
  wc.on('before-input-event', (event, input) => shortcut(wc, event, input));
}
function trusted(event) {
  return window && !window.isDestroyed() && event.sender === window.webContents &&
    event.senderFrame === window.webContents.mainFrame && event.senderFrame.url === uiURL;
}
ipcMain.on('browser:command', (event, command, value) => {
  if (!trusted(event) || typeof command !== 'string') return;
  if (command === 'navigate' && typeof value === 'string' && value.length <= 8192) navigate(current(), value);
  else if (command === 'new') { createTab(); focusAddress(); }
  else if (command === 'select' && Number.isInteger(value)) select(value);
  else if (command === 'close' && Number.isInteger(value)) closeTab(value);
  else if (command === 'back') go(-1);
  else if (command === 'forward') go(1);
  else if (command === 'reload') reload();
  else if (command === 'stop') current()?.view.webContents.stop();
  else if (command === 'home') navigate(current(), HOME);
  else if (command === 'ready') publish();
});
app.whenReady().then(async () => {
  // Use Chromium's real resolver with the DHCP-provided system nameservers.
  // The port's glibc resolver is incomplete; no host/site overrides are used.
  app.configureHostResolver({ enableBuiltInResolver: true, enableHappyEyeballs: true,
    // Some DHCP DNS relays drop HTTPS/SVCB queries instead of answering them.
    // Do not put that timeout on the critical path of ordinary IPv4 browsing.
    enableAdditionalDnsQueryTypes: false });
  webSession = session.fromPartition('persist:coast-browser');
  if (profile) {
    const filter = { urls: ['http://*/*', 'https://*/*'] };
    for (const stage of ['onSendHeaders', 'onResponseStarted', 'onCompleted', 'onErrorOccurred']) {
      webSession.webRequest[stage](filter, details => log('network', {
        stage, request: details.id, type: details.resourceType, url: details.url.slice(0, 8192),
        timestamp: details.timestamp, status: details.statusCode, error: details.error, cache: details.fromCache
      }));
    }
    const timer = setInterval(() => {
      if (closing) { clearInterval(timer); return; }
      try { log('cpu', { processes: app.getAppMetrics().map(({ pid, type, cpu }) => ({ pid, type, cpu })) }); } catch {}
    }, 1000);
    timer.unref();
  }
  webSession.setPermissionRequestHandler((_wc, permission, callback) => { log('permission-request-denied', { permission }); callback(false); });
  webSession.setPermissionCheckHandler((_wc, permission) => { log('permission-check-denied', { permission }); return false; });
  // Block local files in the network service's URL filter. Ordinary web
  // requests need no synchronous trip through the main JavaScript process.
  // Frame navigation is separately restricted above; unregistered protocols
  // have no handler. Chromium's normal origin/CORS checks remain enabled.
  webSession.webRequest.onBeforeRequest({ urls: ['file://*/*'] }, (_details, callback) => callback({ cancel: true }));
  webSession.protocol.handle('coast', request => {
    if (request.url !== HOME) return new Response('Not found', { status: 404 });
    return new Response(fs.readFileSync(path.join(__dirname, 'home.html')), { headers: {
      'Content-Type': 'text/html; charset=utf-8',
      'Content-Security-Policy': "default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; form-action https: http:"
    } });
  });
  webSession.on('will-download', (_event, item) => {
    const directory = '/home/user/Downloads';
    log('download-start', { name: item.getFilename() });
    try {
      fs.mkdirSync(directory, { recursive: true, mode: 0o755 });
      log('download-directory-ready', {});
      const name = downloadName(item.getFilename(), candidate => fs.existsSync(path.join(directory, candidate)));
      log('download-name-ready', { name });
      item.setSavePath(path.join(directory, name));
      const record = { name, state: 'Downloading', received: 0, total: item.getTotalBytes() };
      downloads.unshift(record); if (downloads.length > 30) downloads.pop();
      item.on('updated', (_e, status) => {
        record.state = status === 'interrupted' ? 'Interrupted' : 'Downloading'; record.received = item.getReceivedBytes(); publish(); log('download-progress', record);

      });
      item.once('done', (_e, status) => { record.state = status; record.received = item.getReceivedBytes(); publish(); log('download', record); });
      publish();
    } catch (error) {
      item.cancel();
      downloads.unshift({ name: 'Download failed', state: error.message, received: 0, total: 0 });
      if (downloads.length > 30) downloads.pop();
      publish(); log('download-error', { message: error.message });
    }
  });
  window = new BrowserWindow({ width: 1200, height: 650, title: 'Chromium Browser', frame: false,
    backgroundColor: '#f4f6f9', show: true, webPreferences: {
      preload: path.join(__dirname, 'preload.cjs'), sandbox: true, contextIsolation: true,
      nodeIntegration: false, webSecurity: true, devTools: false, spellcheck: false
    } });
  installEditing(window.webContents);
  window.webContents.on('will-navigate', event => event.preventDefault());
  window.webContents.setWindowOpenHandler(() => ({ action: 'deny' }));
  window.on('resize', layout);
  window.on('close', () => {
    closing = true;
    for (const tab of tabs.values()) tab.view.webContents.close();
    tabs.clear();
  });
  await window.loadFile(path.join(__dirname, 'browser.html'));
  createTab(); focusAddress();
  log('ready', { versions: process.versions, uid: process.getuid(), sandbox: true });
  if (qa) {
    const check = require('node:child_process').spawn('/bin/chromium-network-check', [], { stdio: ['ignore', 'pipe', 'pipe'] });
    check.stdout.on('data', data => console.log(data.toString()));
    check.stderr.on('data', data => console.error(data.toString()));
    check.on('error', error => log('network-check-error', { message: error.message }));
    check.on('close', code => log('network-check-exit', { code }));
  }
}).catch(error => { console.error('COAST_BROWSER_FATAL', error); app.exit(1); });
app.on('window-all-closed', () => app.quit());

'use strict';
const $ = id => document.getElementById(id);
let active, latest, downloadsOpen = false;
for (const command of ['new', 'back', 'forward', 'home']) $(command).onclick = () => window.browser.command(command);
$('reload').onclick = () => window.browser.command(latest?.tabs.find(t => t.id === active)?.loading ? 'stop' : 'reload');
$('retry').onclick = () => window.browser.command('reload');
$('location').onsubmit = event => { event.preventDefault(); window.browser.command('navigate', $('address').value); $('address').blur(); };
window.browser.onAddress(() => { $('address').focus(); $('address').select(); });
$('downloads').onclick = () => {
  // The native web view occupies the content area. Downloads are shown in the
  // browser chrome status line so it never obscures the active native view.
  downloadsOpen = !downloadsOpen; renderStatus();
};
function renderStatus() {
  const current = latest?.tabs.find(t => t.id === active);
  if (downloadsOpen) $('status').textContent = latest.downloads.length ? latest.downloads.map(d => `${d.name}: ${d.state} (${d.received} bytes)`).join(' · ') : 'Downloads are saved in /home/user/Downloads. No downloads yet.';
  else $('status').textContent = current?.error ? `Could not open this page: ${current.error}. Check the address or reload.` : current?.loading ? 'Loading…' : 'Ready';
}
window.browser.onState(state => {
  latest = state; active = state.active;
  const current = state.tabs.find(t => t.id === active);
  $('tabs').replaceChildren(...state.tabs.map(tab => {
    const group = document.createElement('div'); group.className = 'tab' + (tab.id === active ? ' active' : '');
    const label = document.createElement('button'); label.className = 'label'; label.textContent = (tab.loading ? '◌ ' : '') + tab.title; label.title = tab.url; label.onclick = () => window.browser.command('select', tab.id);
    const close = document.createElement('button'); close.className = 'close'; close.textContent = '×'; close.setAttribute('aria-label', 'Close ' + tab.title); close.onclick = () => window.browser.command('close', tab.id);
    group.append(label, close); return group;
  }));
  if (document.activeElement !== $('address')) $('address').value = current?.url === 'coast://home/' ? '' : current?.url || '';
  $('back').disabled = !current?.back; $('forward').disabled = !current?.forward;
  $('reload').textContent = current?.loading ? '×' : '↻';
  $('security').textContent = current?.secure ? '▣' : '○';
  $('security').title = current?.secure ? 'HTTPS — certificate verified' : current?.loading ? 'Connecting…' : 'No verified HTTPS connection';
  $('error').hidden = !current?.error;
  $('error-url').textContent = current?.url || '';
  $('error-detail').textContent = current?.error || '';
  $('new').disabled = state.tabs.length >= state.maxTabs; renderStatus();
});
window.browser.command('ready');

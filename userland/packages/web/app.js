'use strict';
const search = document.querySelector('#search');
const results = document.querySelector('#results');
const count = document.querySelector('#count');
let packages = [], kind = 'all', pending = null, debounce = null;
async function render() {
  if (pending) pending.abort();
  const request = pending = new AbortController();
  try {
    const query = new URLSearchParams({q: search.value, kind});
    const response = await fetch(`/api/v1/search?${query}`, {signal: request.signal});
    if (!response.ok) throw Error('Repository unavailable');
    const data = await response.json();
    if (request !== pending) return;
    packages = data.results;
    document.querySelector('#fingerprint').textContent = data.fingerprint;
  } catch (error) {
    if (error.name === 'AbortError' || request !== pending) return;
    count.textContent = 'Repository unavailable';
    results.textContent = 'The local repository could not be loaded. Check the server and reload this page.';
    return;
  }
  const terms = search.value.toLocaleLowerCase().trim().split(/\s+/).filter(Boolean);
  const matches = packages;
  const fragment = document.createDocumentFragment();
  for (const p of matches) {
    const card = document.createElement('article'); card.className = `package ${p.Kind}`;
    const title = document.createElement('h2'); title.textContent = p.Name;
    const version = document.createElement('span'); version.className = 'version'; version.textContent = p.Version; title.append(version);
    const badge = document.createElement('span'); badge.className = 'badge'; badge.textContent = p.Kind === 'binary' ? 'Signed package' : 'Source recipe';
    const description = document.createElement('p'); description.className = 'description'; description.textContent = p.Description;
    card.append(title,badge,description);
    const commands = p.Commands.filter(command => terms.some(term => command.toLocaleLowerCase().includes(term)));
    if (commands.length) { const line = document.createElement('div'); line.className='commands'; line.textContent='Commands: '; for (const command of commands) {const code=document.createElement('code');code.textContent=command;line.append(code);}card.append(line); }
    fragment.append(card);
  }
  if (!matches.length) {const empty=document.createElement('p');empty.className='empty';empty.textContent='No matching packages. Try a shorter name or another command.';fragment.append(empty);}
  results.replaceChildren(fragment); count.textContent=`${matches.length} ${matches.length===1?'result':'results'}`;
}
search.addEventListener('input',()=>{clearTimeout(debounce);debounce=setTimeout(render,80);});
for(const button of document.querySelectorAll('[data-kind]'))button.addEventListener('click',()=>{kind=button.dataset.kind;for(const other of document.querySelectorAll('[data-kind]'))other.setAttribute('aria-pressed',String(other===button));render();});
render();

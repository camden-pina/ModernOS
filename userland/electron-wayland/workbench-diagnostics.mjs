// Optional startup observation. Preserve the original dialog and error behavior.
import { dialog as coastDiagnosticDialog, app as coastDiagnosticApp, utilityProcess as coastUtilityProcess } from 'electron';
import * as coastDiagnosticFS from 'node:fs';
import { createHash as coastDiagnosticHash } from 'node:crypto';
import coastDiagnosticCP from 'node:child_process';
import { syncBuiltinESMExports as coastSyncExports } from 'node:module';
const coastFixtureEnabled = process.env.COAST_CODEOSS_QA === '1';
const coastUserFilesEnabled = coastFixtureEnabled && process.env.COAST_USER_FILES_CHECK === '1';
if (coastUserFilesEnabled) {
    if (process.getuid() !== 1000 || process.getgid() !== 1000 || process.env.HOME !== '/home/user')
        throw new Error('Unexpected personal-file credentials or HOME');
    for (const path of ['/home/user', '/home/user/Projects']) {
        const stat = coastDiagnosticFS.statSync(path);
        if (stat.uid !== 1000 || stat.gid !== 1000 ||
            (stat.mode & 0o022) || (path === '/home/user' && (stat.mode & 0o777) !== 0o700))
            throw new Error('Incorrect profile ownership or directory permissions: ' + path);
    }
    const probes = [];
    for (const parent of ['/home/user/Projects', '/home/user/Documents', '/home/user/Downloads', '/home/user/Desktop', '/tmp']) {
        const folder = parent + '/.codeoss-permission-check';
        coastDiagnosticFS.mkdirSync(folder, {mode:0o700});
        const first=folder+'/first.txt', second=folder+'/renamed.txt';
        coastDiagnosticFS.writeFileSync(first,'permission check',{flag:'wx',mode:0o600});
        coastDiagnosticFS.renameSync(first,second);
        const stat=coastDiagnosticFS.statSync(second);
        if (stat.uid!==1000 || coastDiagnosticFS.readFileSync(second,'utf8')!=='permission check') throw new Error('Personal file mismatch');
        coastDiagnosticFS.unlinkSync(second);coastDiagnosticFS.rmdirSync(folder);
        probes.push({parent,create:true,rename:true,remove:true,uid:stat.uid});
    }
    for (const directory of ['/etc','/usr','/home','/tmp']) coastDiagnosticFS.readdirSync(directory);
    const permissionDenials=[];
    const expectDenied=(name,operation)=>{
        try { operation(); }
        catch(error) {
            if(!['EACCES','EPERM','EROFS'].includes(error.code))throw error;
            permissionDenials.push({name,code:error.code});return;
        }
        throw new Error('Permission unexpectedly granted: '+name);
    };
    if(!(coastDiagnosticFS.statSync('/tmp').mode&0o1000))throw new Error('Shared tmp lacks sticky bit');
    expectDenied('overwrite foreign file',()=>coastDiagnosticFS.writeFileSync('/tmp/codeoss-root-owned','must fail'));
    expectDenied('chmod foreign file',()=>coastDiagnosticFS.chmodSync('/tmp/codeoss-root-owned',0o666));
    expectDenied('delete foreign tmp entry',()=>coastDiagnosticFS.unlinkSync('/tmp/codeoss-root-owned'));
    expectDenied('rename foreign tmp entry',()=>coastDiagnosticFS.renameSync('/tmp/codeoss-root-owned','/tmp/codeoss-root-renamed'));
    expectDenied('browse private root directory',()=>coastDiagnosticFS.readdirSync('/tmp/codeoss-root-private'));
    const own='/tmp/codeoss-owned-replacement';
    coastDiagnosticFS.writeFileSync(own,'owned',{flag:'wx',mode:0o600});
    expectDenied('replace foreign tmp entry',()=>coastDiagnosticFS.renameSync(own,'/tmp/codeoss-root-owned'));
    coastDiagnosticFS.unlinkSync(own);
    const escape='/home/user/Projects/codeoss-private-link';
    coastDiagnosticFS.symlinkSync('/tmp/codeoss-root-private',escape);
    expectDenied('symlink into private directory',()=>coastDiagnosticFS.writeFileSync(escape+'/forbidden','must fail'));
    coastDiagnosticFS.unlinkSync(escape);
    if(coastDiagnosticFS.readFileSync('/tmp/codeoss-root-owned','utf8')!=='root preserved')throw new Error('Foreign file changed');

    let denied=false;
    try { coastDiagnosticFS.writeFileSync('/etc/.codeoss-permission-check','must fail',{flag:'wx'}); }
    catch(error) { if(error.code!=='EACCES' && error.code!=='EROFS') throw error; denied=true; }
    if (!denied) throw new Error('Unexpected write permission on system directory');
    for (const path of ['/home/user/Projects/native-created','/home/user/Projects/from-native.txt']) {
        const stat=coastDiagnosticFS.statSync(path);
        if(stat.uid!==1000 || stat.gid!==1000) throw new Error('Native desktop created mismatched owner: '+path);
    }
    coastDiagnosticFS.writeFileSync('/home/user/Projects/from-native.txt','Native file edited by Code OSS');
    console.error('CODEOSS_USER_FILES_PASS',JSON.stringify({uid:process.getuid(),home:process.env.HOME,probes,permissionDenials,systemWriteDenied:denied,nativeFilesEditable:true}));
}
const coastUtilityFork = coastUtilityProcess.fork;
coastUtilityProcess.fork = function (...args) {
    const child = Reflect.apply(coastUtilityFork, this, args);
    let buffered = '';
    child.stderr?.on('data', chunk => {
        buffered += chunk.toString();
        if (buffered.length > 65536) buffered = buffered.slice(-8192);
        const lines = buffered.split('\n'); buffered = lines.pop();
        for (const line of lines) if (line.startsWith('COAST_METADATA_')) console.error(line);
    });
    return child;
};
if (coastFixtureEnabled) {
    delete process.env.COAST_CODEOSS_QA;
    coastDiagnosticFS.mkdirSync('/home/port-test/project', { recursive: true, mode: 0o700 });
    for(const [name,text] of Object.entries({
        'hello.txt':'Coast original\n',
        'unicode.txt':'Καλημέρα κόσμε\n日本語のテキスト\nCafé ☕\n',
        'clipboard-64k.txt':'x\n'.repeat(32768),
        'clipboard-overflow.txt':'y'.repeat(65537),
        'crlf.txt':'first\r\nsecond\r\n',
        'large.txt':'Coast large first\n'+('actual file content\n'.repeat(16000))+'Coast large last\n',
        'format.js':'function sum(a,b){return a+b;}\nconst answer=sum(1,2);\n',
        'bad.ts':'const count: number = "wrong";\n',
        'completion.js':'console\n',
        'watch-target.txt':'External original\n',
        'replace-target.txt':'Workspace replace original\n',
        'format.json':'{\"name\":\"Coast\",\"items\":[1,2],\"enabled\":true}\n',
        'invalid.json':'{\"enabled\": tru}\n',
        'preview.md':'# Coast Markdown preview\n\n**Native editor preview** with 日本語 and café.\n\n- First item\n- Second item\n',
        'debug.js':'let value = 41;\nvalue += 1;\nconsole.log("debug value", value);\nrequire("node:fs").writeFileSync("/home/port-test/project/debug-result.txt",String(value));\n'
    })) {
        try { coastDiagnosticFS.writeFileSync('/home/port-test/project/'+name,text,{flag:'wx',mode:0o600}); }
        catch(error) { if(error.code!=='EEXIST')throw error; }
    }
    console.error('CODEOSS_QA_FIXTURE /home/port-test/project/hello.txt uid=' + process.getuid());
}
const coastSpawn = coastDiagnosticCP.spawn;
coastDiagnosticCP.spawn = function (command, args, options) {
    console.error('CODEOSS_SPAWN', JSON.stringify({command,args,shell:options?.env?.SHELL,nodeOptions:options?.env?.NODE_OPTIONS}));
    return coastSpawn.apply(this, arguments);
};
coastSyncExports();
setTimeout(() => console.error('CODEOSS_EFFECTIVE_SANDBOX', JSON.stringify({
    noSandbox: coastDiagnosticApp.commandLine.hasSwitch('no-sandbox'),
    disableGpuSandbox: coastDiagnosticApp.commandLine.hasSwitch('disable-gpu-sandbox')
})), 0);
const coastDiagnosticDlopen = process.dlopen;
process.dlopen = function (...args) {
    try { return coastDiagnosticDlopen.apply(this, args); }
    catch (error) { console.error('CODEOSS_DLOPEN_ERROR', args[1], error?.message); throw error; }
};
coastDiagnosticApp.on('web-contents-created', (_event, contents) => {
    const observe = async () => {
            try {
                try {
                    const descriptors = coastDiagnosticFS.readdirSync('/proc/self/fd');
                    const groups = {};
                    for (const fd of descriptors) {
                        let target;
                        try { target = coastDiagnosticFS.readlinkSync('/proc/self/fd/' + fd); }
                        catch { target = 'closed-during-snapshot'; }
                        const group = target.replace(/\[[^\]]*\]/g, '[]').replace(/\.[A-Za-z0-9]{6}$/, '.XXXXXX');
                        groups[group] = (groups[group] ?? 0) + 1;
                    }
                    console.error('CODEOSS_FDS', JSON.stringify({ count: descriptors.length, max: Math.max(...descriptors.map(Number)), groups, relay:globalThis.__coastRPCRelay??null }));
                } catch (error) { console.error('CODEOSS_FDS_ERROR', error.code); }
                if (coastFixtureEnabled) {
                    try { console.error('CODEOSS_QA_FILE', JSON.stringify(coastDiagnosticFS.readFileSync('/home/port-test/project/hello.txt', 'utf8'))); }
                    catch(error) { console.error('CODEOSS_QA_FILE_MISSING',error.code); }
                    const files={};
                    const gitMetadata=new Set(['.git','.git/HEAD','.git/config','.git/index','.git/logs','.git/logs/HEAD']);
                    const collect=(prefix,depth)=>{
                        for(const name of coastDiagnosticFS.readdirSync('/home/port-test/project/'+prefix)) {
                            if(Object.keys(files).length>=256)return;
                            const relative=prefix+name;
                            // Keep observation below the traced terminal's
                            // bounded drain rate. Git hooks and object blobs
                            // are not inputs to the editor assertions.
                            if(relative.startsWith('.git/') && !gitMetadata.has(relative))continue;
                            const path='/home/port-test/project/'+relative,stat=coastDiagnosticFS.lstatSync(path);
                            files[relative]={bytes:stat.size,directory:stat.isDirectory()};
                            if(stat.isFile()&&stat.size<65536&&relative!=='.git/index')files[relative].text=coastDiagnosticFS.readFileSync(path,'utf8');
                            if(stat.isFile()&&(stat.size===65536||stat.size===65537||(relative==='.git/index'&&stat.size<=65536)))files[relative].sha256=coastDiagnosticHash('sha256').update(coastDiagnosticFS.readFileSync(path)).digest('hex');
                            if(stat.isDirectory()&&depth<2)collect(relative+'/',depth+1);
                        }
                    };
                    collect('',0);
                    console.error('CODEOSS_QA_FILES',JSON.stringify(files));
                    if(coastUserFilesEnabled) {
                        const personal={};
                        for(const path of ['/home/user/Projects/editor-created.txt','/home/user/Projects/native-created','/home/user/Projects/from-native.txt','/tmp/codeoss-created.txt','/home/user/Documents/editor-created.txt']) {
                            try {
                                const stat=coastDiagnosticFS.statSync(path);
                                personal[path]={uid:stat.uid,gid:stat.gid,mode:stat.mode,directory:stat.isDirectory()};
                                if(stat.isFile()&&stat.size<4096)personal[path].text=coastDiagnosticFS.readFileSync(path,'utf8');
                            } catch(error) { if(error.code!=='ENOENT')throw error; }
                        }
                        console.error('CODEOSS_QA_PERSONAL_FILES',JSON.stringify(personal));
                    }
                }
                console.error('CODEOSS_DOM', contents.id, await contents.executeJavaScript(`JSON.stringify({
                    url: location.href, ready: document.readyState, fonts: document.fonts.status, visibility:document.visibilityState, focused:document.hasFocus(),
                    rpcTrace:globalThis.__coastRPCTail??null,rpcWire:globalThis.__coastRPCWire??null,
                    body: document.body?.innerText?.slice(0, 3000),
                    inputs: [...document.querySelectorAll('input,textarea')].map(e => e.value),
                    inputFields: [...document.querySelectorAll('input,textarea,[role="textbox"],.monaco-editor,.settings-search-input')].filter(e=>{const r=e.getBoundingClientRect();return r.width>4&&r.height>4;}).slice(0,80).map(e=>{const rect=e.getBoundingClientRect(),hit=document.elementFromPoint(rect.x+rect.width/2,rect.y+rect.height/2);return {value:e.value,valueText:e.querySelector('.view-lines')?.textContent,label:e.getAttribute('aria-label')??e.querySelector('[aria-label]')?.getAttribute('aria-label'),placeholder:e.getAttribute('placeholder'),className:e.className,ancestor:e.closest('.settings-editor,.keybindings-editor,.scm-input')?.className,focused:e===document.activeElement||e.contains(document.activeElement),hit:!!hit&&(e===hit||e.contains(hit)),hitClass:hit?.className,rect:rect.toJSON()};}),
                    quickInputVisible: [...document.querySelectorAll('.quick-input-widget')].some(e => getComputedStyle(e).display !== 'none' && e.getBoundingClientRect().height > 0),
                    quickPickRows: [...document.querySelectorAll('.quick-input-list .monaco-list-row')].filter(e=>e.getBoundingClientRect().height>0).slice(0,100).map(e=>({text:e.innerText,label:e.querySelector('.label-name')?.textContent,description:e.querySelector('.label-description')?.textContent,selected:e.getAttribute('aria-selected')==='true',focused:e.classList.contains('focused'),disabled:e.getAttribute('aria-disabled')==='true'||e.classList.contains('disabled'),className:e.className,rect:e.getBoundingClientRect().toJSON()})),
                    findWidget: [...document.querySelectorAll('.find-widget')].map(e => ({visible:e.classList.contains('visible'),inputs:[...e.querySelectorAll('input,textarea')].map(i => i.value)})),
                    activeElement: {tag:document.activeElement?.tagName,role:document.activeElement?.getAttribute('role'),label:document.activeElement?.getAttribute('aria-label')},
                    buttons: [...document.querySelectorAll('button,[role="button"],.monaco-button')].filter(e=>e.getBoundingClientRect().height>0).slice(0,100).map(e=>{
                        const rect=e.getBoundingClientRect(),hit=document.elementFromPoint(rect.x+rect.width/2,rect.y+rect.height/2);
                        return {text:e.innerText,label:e.getAttribute('aria-label'),title:e.title,disabled:!!e.disabled||e.getAttribute('aria-disabled')==='true'||e.classList.contains('disabled'),hit:!!hit&&(e===hit||e.contains(hit)),rect:rect.toJSON()};
                    }),
                    scmRows: [...document.querySelectorAll('.scm-view .resource')].filter(e=>e.getBoundingClientRect().height>0).slice(0,80).map(e=>({text:e.innerText,title:e.title,label:e.getAttribute('aria-label'),rect:e.getBoundingClientRect().toJSON()})),
                    scmTreeRows: [...document.querySelectorAll('.scm-view .monaco-list-row')].filter(e=>e.getBoundingClientRect().height>0).slice(0,80).map(e=>({text:e.innerText,label:e.getAttribute('aria-label'),className:e.className,rect:e.getBoundingClientRect().toJSON()})),
                    explorerRows: [...document.querySelectorAll('.explorer-item')].filter(e=>e.getBoundingClientRect().height>0).slice(0,100).map(e=>{
                        const row=e.closest('.monaco-list-row'),rect=e.getBoundingClientRect(),hit=document.elementFromPoint(rect.x+rect.width/2,rect.y+rect.height/2);
                        return {name:e.querySelector('.label-name')?.textContent??e.textContent,expanded:row?.getAttribute('aria-expanded'),selected:row?.getAttribute('aria-selected')==='true'||!!row?.classList.contains('selected'),focused:!!row?.classList.contains('focused'),hit:!!hit&&(e===hit||e.contains(hit)),rect:rect.toJSON()};
                    }),
                    explorerTrees:[...document.querySelectorAll('.explorer-viewlet [role="tree"],.explorer-folders-view [role="tree"]')].slice(0,4).map(e=>({label:e.getAttribute('aria-label'),focused:e===document.activeElement||e.contains(document.activeElement),rect:e.getBoundingClientRect().toJSON()})),
                    menuItems:[...document.querySelectorAll('[role="menuitem"]')].filter(e=>e.getBoundingClientRect().height>0).slice(0,60).map(e=>{const rect=e.getBoundingClientRect(),hit=document.elementFromPoint(rect.x+rect.width/2,rect.y+rect.height/2);return {text:e.innerText,label:e.getAttribute('aria-label'),disabled:e.getAttribute('aria-disabled')==='true',hit:!!hit&&(e===hit||e.contains(hit)),rect:rect.toJSON()}}),
                    menuAncestors:[...document.querySelectorAll('.menubar-menu-items-holder')].slice(0,1).map(menu=>{const chain=[];for(let e=menu;e&&chain.length<16;e=e.parentElement){const s=getComputedStyle(e);chain.push({tag:e.tagName,className:e.className,style:e.getAttribute('style'),overflow:s.overflow,zIndex:s.zIndex,transform:s.transform,contain:s.contain,rect:e.getBoundingClientRect().toJSON()});}return chain;}),
                    windowControls:[...document.querySelectorAll('.window-controls-container')].filter(e=>e.getBoundingClientRect().width>0&&e.getBoundingClientRect().height>0).map(e=>({text:e.innerText,rect:e.getBoundingClientRect().toJSON()})),
                    devicePixelRatio,
                    editors: (document.getElementById('workbench.parts.editor')??document).querySelectorAll('.monaco-editor').length,
                    editorLines: [...(document.getElementById('workbench.parts.editor')??document).querySelectorAll('.view-lines')].map(e => [...e.querySelectorAll('.view-line')].sort((a,b)=>a.getBoundingClientRect().top-b.getBoundingClientRect().top).map(e=>e.textContent.replace(/\u00a0/g,' ')).join('')),
                    lineTexts: [...(document.getElementById('workbench.parts.editor')??document).querySelectorAll('.view-lines')].flatMap(group=>[...group.querySelectorAll('.view-line')].sort((a,b)=>a.getBoundingClientRect().top-b.getBoundingClientRect().top)).map(e => e.textContent.replace(/\u00a0/g, ' ')),
                    renderedTokenStyles: [...(document.getElementById('workbench.parts.editor')??document).querySelectorAll('.view-lines .view-line span[class*="mtk"]')].filter(e=>e.getBoundingClientRect().height>0).slice(0,80).map(e=>({text:e.textContent,color:getComputedStyle(e).color,fontFamily:getComputedStyle(e).fontFamily,fontSize:getComputedStyle(e).fontSize})),
                    lineGeometry: [...(document.getElementById('workbench.parts.editor')??document).querySelectorAll('.view-lines')].flatMap(group=>[...group.querySelectorAll('.view-line')].sort((a,b)=>a.getBoundingClientRect().top-b.getBoundingClientRect().top)).slice(0,3).map(e => {
                        const walker=document.createTreeWalker(e,NodeFilter.SHOW_TEXT),node=walker.nextNode();
                        if(!node||!node.length)return null;
                        const range=document.createRange();range.setStart(node,0);range.setEnd(node,1);
                        return {text:e.textContent,character:range.getBoundingClientRect().toJSON()};
                    }),
                    editorRects: [...(document.getElementById('workbench.parts.editor')??document).querySelectorAll('.view-lines')].map(e => e.getBoundingClientRect().toJSON()),
                    dialogs: [...document.querySelectorAll('[role="dialog"],.monaco-dialog-box')].slice(0, 8).map(e => ({text:e.innerText,rect:e.getBoundingClientRect().toJSON()})),
                    workbench: document.querySelector('.monaco-workbench')?.getBoundingClientRect().toJSON(),
                    parts: [...document.querySelectorAll('.part')].map(e => ({id:e.id,display:getComputedStyle(e).display,rect:e.getBoundingClientRect().toJSON()}))
                })`));
                // Markdown previews live in isolated child frames. Observe
                // their real rendered DOM, never their model or extension API.
                for (const frame of contents.mainFrame.framesInSubtree.slice(0, 8)) {
                    if (frame === contents.mainFrame) continue;
                    try {
                        console.error('CODEOSS_FRAME', contents.id, await frame.executeJavaScript(`JSON.stringify({url:location.href,ready:document.readyState,fonts:document.fonts.status,body:document.body?.innerText?.slice(0,3000),heading:[...document.querySelectorAll('h1,h2')].slice(0,8).map(e=>e.innerText)})`));
                    } catch(error) { console.error('CODEOSS_FRAME_ERROR',contents.id,error?.message); }
                }
            } catch (error) { console.error('CODEOSS_DOM_ERROR', contents.id, error?.message); }
    };
    const first = setTimeout(observe, 12000);
    const interval = setInterval(observe, 6000);
    contents.once('destroyed', () => { clearTimeout(first); clearInterval(interval); });
    contents.on('page-title-updated', (_event, title) => console.error('CODEOSS_TITLE', title));
});
process.on('uncaughtExceptionMonitor', (error, origin) => {
    console.error('CODEOSS_UNCAUGHT', origin, error?.stack ?? String(error));
});
for (const name of ['showMessageBox', 'showMessageBoxSync', 'showErrorBox']) {
    const original = coastDiagnosticDialog[name];
    coastDiagnosticDialog[name] = function (...args) {
        console.error('CODEOSS_DIALOG', name, JSON.stringify(args.map(value =>
            typeof value === 'string' ? value : value && {
                title: value.title, message: value.message, detail: value.detail
            })));
        return original.apply(this, args);
    };
}

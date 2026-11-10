// Run only inside the owned Coast guest through verify_electron_local.py.
// Exercise the same direct renderer/utility MessagePorts used by Code OSS RPC.
'use strict';
const {app, BrowserWindow, ipcMain, MessageChannelMain, utilityProcess} = require('electron');
const fs = require('node:fs/promises');
const directory = '/home/port-test/runtime';
const rounds = 3, messages = 256;
const workerSource = `
process.parentPort.on('message', event => {
    if (event.data.quit) process.exit(0);
    const port = event.ports[0];
    port.on('message', async event => {
        await Promise.resolve();
        await new Promise(resolve=>process.nextTick(resolve));
        const bytes = event.data;
        const reply = Buffer.allocUnsafe(bytes.length+64).subarray(17,17+bytes.length);
        reply.set(bytes);port.postMessage(reply);
    });
    port.start();
});
`;
const preloadSource = `
const {ipcRenderer} = require('electron');
ipcRenderer.on('test-port', (event, round) => {
    const port = event.ports[0];
    let sequence = 0, received = 0;
    const seen = new Set();
    function send() {
        while(sequence < ${messages} && sequence-received < 64) {
            const size=sequence % 64 === 0 ? 65536 : 1024;
            const bytes = new Uint8Array(size+64).subarray(31,31+size);
            new DataView(bytes.buffer,bytes.byteOffset,bytes.byteLength).setUint32(0,sequence,true);
            bytes[4]=round;for(let i=5;i<bytes.length;i++)bytes[i]=(i*31+sequence)&255;
            sequence++;port.postMessage(bytes);
        }
    }
    port.onmessage = event => {
        const value = event.data;
        const seq=new DataView(value.buffer,value.byteOffset,value.byteLength).getUint32(0,true);
        let valid=Number.isInteger(seq)&&seq>=0&&seq<sequence&&!seen.has(seq)&&
            value.length===(seq%64===0?65536:1024)&&value[4]===round;
        for(let i=5;valid&&i<value.length;i++)valid=value[i]===((i*31+seq)&255);
        if(!valid){
            ipcRenderer.send('test-failure',JSON.stringify({round,sequence,seq,bytes:value.length}));
            port.close();return;
        }
        seen.add(seq);
        if (++received === ${messages}) {
            port.close(); ipcRenderer.send('test-done', round, received); return;
        }
        if (received % 64 === 0) ipcRenderer.send('test-progress', round, received);
        send();
    };
    port.start(); send();
});
`;
let window;
app.whenReady().then(async () => {
    await fs.writeFile(directory+'/message-port-worker.cjs', workerSource);
    await fs.writeFile(directory+'/message-port-preload.cjs', preloadSource);
    window = new BrowserWindow({width:400,height:300,useContentSize:true,frame:false,show:true,
        backgroundColor:'#154d36',webPreferences:{sandbox:true,nodeIntegration:false,
            contextIsolation:true,preload:directory+'/message-port-preload.cjs'}});
    window.setMenu(null);
    window.on('closed',()=>console.log('GENUINE_ELECTRON_WINDOW_CLOSED'));
    await window.loadURL('data:text/html,<body style="margin:0;background:%23154d36;color:white"><div style="width:280px;height:180px;background:%2344d68c;border:12px solid %2312356a">Coast native MessagePort check</div></body>');
    console.log('GENUINE_ELECTRON_DOCUMENT_LOADED renderer='+window.webContents.getOSProcessId());
    ipcMain.on('test-progress', (_event, round, sequence) => console.log('MESSAGE_PORT_PROGRESS '+round+' '+sequence));
    const env={...process.env}; delete env.ELECTRON_RUN_AS_NODE; delete env.NODE_OPTIONS;
    const child=utilityProcess.fork(directory+'/message-port-worker.cjs',[],{env,stdio:'pipe'});
    await new Promise((resolve,reject)=>{child.once('spawn',resolve);child.once('error',reject);});
    for (let round=0;round<rounds;round++) {
        await new Promise((resolve,reject) => {
            const timer=setTimeout(() => {cleanup();reject(new Error('MessagePort round '+round+' timed out'));},30000);
            const failure=(_event,error) => {cleanup();reject(new Error(error));};
            const exited=code => {cleanup();reject(new Error('Utility exited early '+code));};
            const cleanup=()=>{
                clearTimeout(timer);ipcMain.removeListener('test-failure',failure);
                ipcMain.removeListener('test-done',done);child.removeListener('exit',exited);
            };
            const done=(_event, actualRound,count) => {
                if(actualRound!==round || count!==messages)return;
                cleanup();console.log('MESSAGE_PORT_ROUND_PASS '+round);resolve();
            };
            ipcMain.once('test-failure',failure);ipcMain.once('test-done',done);child.once('exit',exited);
            const {port1,port2}=new MessageChannelMain();
            child.postMessage({round},[port1]);
            window.webContents.postMessage('test-port',round,[port2]);
        });
    }
    await new Promise((resolve,reject)=>{
        child.once('exit',code=>code===0?resolve():reject(new Error('Utility exit '+code)));
        child.postMessage({quit:true});
    });
    console.log('REAL_RENDERER_UTILITY_MESSAGE_PORT_PASS rounds='+rounds+' messages='+(rounds*messages));
    console.log('GENUINE_ELECTRON_MAIN_READY '+JSON.stringify({electron:process.versions.electron,uid:process.getuid()}));
}).catch(error=>{console.error('MESSAGE_PORT_FAIL '+error.stack);app.exit(1);});
app.on('window-all-closed',()=>app.quit());

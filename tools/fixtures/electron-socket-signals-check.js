"use strict";
const {app,BrowserWindow}=require('electron');
const {execFile}=require('node:child_process');
function privateChecks(){return new Promise((resolve,reject)=>execFile('/bin/private-stream-check',['--signals-only'],{timeout:15000},(error,stdout,stderr)=>{console.log(stdout);console.error(stderr);if(error||!stdout.includes('UNIX_SIGNAL_READY_PASS'))reject(error||new Error('private stream marker absent'));else resolve();}));}
app.whenReady().then(async()=>{
 await privateChecks();
 console.log('GENUINE_ELECTRON_MAIN_READY '+JSON.stringify({electron:process.versions.electron,node:process.versions.node,uid:process.getuid(),pid:process.pid}));
 const win=new BrowserWindow({width:400,height:300,useContentSize:true,frame:false,show:true,backgroundColor:'#154d36',webPreferences:{sandbox:true,nodeIntegration:false,contextIsolation:true}});
 win.setMenu(null);
 win.webContents.on('did-finish-load',()=>console.log('GENUINE_ELECTRON_DOCUMENT_LOADED renderer='+win.webContents.getOSProcessId()));
 win.webContents.on('render-process-gone',(_,details)=>console.log('GENUINE_ELECTRON_RENDERER_GONE '+JSON.stringify(details)));
 win.on('closed',()=>console.log('GENUINE_ELECTRON_WINDOW_CLOSED'));
 win.loadURL('data:text/html,'+encodeURIComponent('<!doctype html><html><head><title>Coast upstream Electron</title><style>html,body{margin:0;width:100%;height:100%;background:#154d36}div{position:absolute;left:40px;top:40px;width:240px;height:160px;background:#49d997;border:8px solid #143a75}</style></head><body><div></div></body></html>'));
}).catch(e=>{console.error(e);app.exit(2)});
app.on('window-all-closed',()=>app.quit());

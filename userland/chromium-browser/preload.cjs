'use strict';
const { contextBridge, ipcRenderer } = require('electron');
contextBridge.exposeInMainWorld('browser', {
  command: (name, value) => ipcRenderer.send('browser:command', name, value),
  onState: callback => ipcRenderer.on('browser:state', (_event, state) => callback(state)),
  onAddress: callback => ipcRenderer.on('browser:address', () => callback())
});

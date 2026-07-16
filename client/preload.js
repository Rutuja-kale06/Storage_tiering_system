const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('electronAPI', {
  getConfig: () => ipcRenderer.invoke('get-config'),
  saveConfig: (cfg) => ipcRenderer.invoke('save-config', cfg),
  getEngineState: () => ipcRenderer.invoke('get-engine-state'),

  onEngineState: (callback) => {
    ipcRenderer.on('engine-state', (e, data) => callback(data));
  },
  onConfigChanged: (callback) => {
    ipcRenderer.on('config-changed', (e, data) => callback(data));
  },
  onRunCycle: (callback) => {
    ipcRenderer.on('run-cycle', () => callback());
  },
});

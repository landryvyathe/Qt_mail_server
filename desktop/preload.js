const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('bykiva', {
  select: (id) => ipcRenderer.send('accounts:select', id),
  add: () => ipcRenderer.send('accounts:add'),
  remove: (id) => ipcRenderer.send('accounts:remove', id),
  onUpdate: (callback) => ipcRenderer.on('accounts:update', (_event, state) => callback(state)),
});

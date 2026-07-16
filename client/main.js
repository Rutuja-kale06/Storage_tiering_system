const { app, BrowserWindow, Tray, Menu, Notification, ipcMain, nativeImage } = require('electron');
const path = require('path');
const fs = require('fs');

let mainWindow = null;
let tray = null;
let engineState = 'unknown';
let pollInterval = null;

const CONFIG_PATH = path.join(app.getPath('userData'), 'config.json');

function loadConfig() {
  try {
    if (fs.existsSync(CONFIG_PATH)) {
      return JSON.parse(fs.readFileSync(CONFIG_PATH, 'utf8'));
    }
  } catch (e) { /* ignore */ }
  return { serverUrl: 'http://localhost:3000', apiKey: '', refreshInterval: 5 };
}

function saveConfig(config) {
  fs.writeFileSync(CONFIG_PATH, JSON.stringify(config, null, 2));
}

let config = loadConfig();

function createWindow() {
  mainWindow = new BrowserWindow({
    width: 1100,
    height: 750,
    minWidth: 800,
    minHeight: 500,
    title: 'Storage Tiering Client',
    icon: path.join(__dirname, 'assets', 'icon.png'),
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false,
    },
    show: false,
  });

  mainWindow.loadFile(path.join(__dirname, 'src', 'index.html'));

  mainWindow.once('ready-to-show', () => {
    mainWindow.show();
  });

  mainWindow.on('close', (e) => {
    if (!app.isQuitting) {
      e.preventDefault();
      mainWindow.hide();
    }
  });
}

function createTray() {
  const iconPath = path.join(__dirname, 'assets', 'icon.png');
  let icon;
  try {
    icon = nativeImage.createFromPath(iconPath);
    if (icon.isEmpty()) icon = nativeImage.createEmpty();
  } catch (e) {
    icon = nativeImage.createEmpty();
  }
  tray = new Tray(icon);
  tray.setToolTip('Storage Tiering Client');

  const contextMenu = Menu.buildFromTemplate([
    { label: 'Show Window', click: () => mainWindow && mainWindow.show() },
    { label: 'Run Cycle', click: () => {
      if (mainWindow) mainWindow.webContents.send('run-cycle');
    }},
    { type: 'separator' },
    { label: 'Quit', click: () => { app.isQuitting = true; app.quit(); } },
  ]);
  tray.setContextMenu(contextMenu);
  tray.on('click', () => mainWindow && mainWindow.show());
}

function getStatusColor(state) {
  switch (state) {
    case 'idle': return '#22c55e';
    case 'analysing': case 'planning': return '#f59e0b';
    case 'migrating': return '#3b82f6';
    case 'paused': return '#a855f7';
    case 'error': return '#ef4444';
    default: return '#6b7280';
  }
}

function updateTrayIcon(state) {
  engineState = state;
  if (!tray) return;
  const color = getStatusColor(state);
  tray.setToolTip(`Storage Tiering — ${state}`);
}

function sendNotification(title, body) {
  if (Notification.isSupported()) {
    new Notification({ title, body, icon: path.join(__dirname, 'assets', 'icon.png') }).show();
  }
}

// IPC handlers
ipcMain.handle('get-config', () => config);
ipcMain.handle('save-config', (e, newConfig) => {
  config = { ...config, ...newConfig };
  saveConfig(config);
  if (mainWindow) mainWindow.webContents.send('config-changed', config);
  return config;
});
ipcMain.handle('get-engine-state', () => engineState);

// Poll engine state periodically and push to renderer
function startPolling() {
  pollInterval = setInterval(async () => {
    if (!mainWindow || mainWindow.isDestroyed()) return;
    try {
      const url = `${config.serverUrl}/api/v1/engine`;
      const headers = { 'Content-Type': 'application/json' };
      if (config.apiKey) headers['X-API-Key'] = config.apiKey;
      const resp = await fetch(url, { headers });
      if (resp.ok) {
        const data = await resp.json();
        updateTrayIcon(data.state || 'unknown');
        mainWindow.webContents.send('engine-state', data);
      } else if (engineState !== 'error') {
        updateTrayIcon('error');
      }
    } catch (e) {
      if (engineState !== 'error') {
        updateTrayIcon('error');
        mainWindow.webContents.send('engine-state', { state: 'error', error: e.message });
      }
    }
  }, (config.refreshInterval || 5) * 1000);
}

app.whenReady().then(() => {
  createWindow();
  createTray();
  startPolling();
});

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') app.quit();
});

app.on('before-quit', () => {
  app.isQuitting = true;
  if (pollInterval) clearInterval(pollInterval);
});

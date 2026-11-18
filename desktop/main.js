const { app, BrowserWindow, Menu, shell, session } = require('electron');
const fs = require('fs');
const path = require('path');

const APP_URL = 'https://mail.bykiva.com/webmail/';
const APP_ORIGIN = new URL(APP_URL).origin;
const ICON = path.join(__dirname, 'build', 'icon.png');
const DEFAULT_BOUNDS = { width: 1280, height: 820 };

let mainWindow = null;

const boundsFile = () => path.join(app.getPath('userData'), 'window.json');

function loadBounds() {
  try { return { ...DEFAULT_BOUNDS, ...JSON.parse(fs.readFileSync(boundsFile(), 'utf8')) }; }
  catch { return DEFAULT_BOUNDS; }
}

function saveBounds(win) {
  if (win.isMinimized() || win.isFullScreen()) return;
  try { fs.writeFileSync(boundsFile(), JSON.stringify({ ...win.getNormalBounds(), maximized: win.isMaximized() })); }
  catch { /* window size is a convenience; never block closing on it */ }
}

const isAppUrl = (url) => { try { return new URL(url).origin === APP_ORIGIN; } catch { return false; } };

// Links in emails are untrusted: anything outside the webmail opens in the system browser, and
// only plain web/mail links are handed over (never file:, javascript: or custom app schemes).
function openExternal(url) {
  try {
    if (['https:', 'http:', 'mailto:'].includes(new URL(url).protocol)) shell.openExternal(url);
  } catch { /* malformed URL: ignore */ }
}

function createWindow() {
  const bounds = loadBounds();
  mainWindow = new BrowserWindow({
    ...bounds,
    minWidth: 900,
    minHeight: 600,
    title: 'Bykiva Mail',
    icon: ICON,
    show: false,
    autoHideMenuBar: true,
    webPreferences: {
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
      spellcheck: true,
    },
  });
  if (bounds.maximized) mainWindow.maximize();

  mainWindow.webContents.setWindowOpenHandler(({ url }) => {
    openExternal(url);
    return { action: 'deny' };
  });
  mainWindow.webContents.on('will-navigate', (event, url) => {
    if (isAppUrl(url) || url.startsWith('file:')) return;
    event.preventDefault();
    openExternal(url);
  });
  // -3 is ERR_ABORTED (a navigation replaced by another), not a real failure.
  mainWindow.webContents.on('did-fail-load', (_e, code, _desc, _url, isMainFrame) => {
    if (isMainFrame && code !== -3) mainWindow.loadFile(path.join(__dirname, 'offline.html'));
  });

  mainWindow.once('ready-to-show', () => mainWindow.show());
  mainWindow.on('close', () => saveBounds(mainWindow));
  mainWindow.on('closed', () => { mainWindow = null; });
  mainWindow.loadURL(APP_URL);
}

function buildMenu() {
  const reload = () => mainWindow && mainWindow.loadURL(APP_URL);
  Menu.setApplicationMenu(Menu.buildFromTemplate([
    { label: 'File', submenu: [{ label: 'Reload', accelerator: 'CmdOrCtrl+R', click: reload }, { type: 'separator' }, { role: 'quit' }] },
    { role: 'editMenu' },
    { label: 'View', submenu: [{ role: 'zoomIn' }, { role: 'zoomOut' }, { role: 'resetZoom' }, { type: 'separator' }, { role: 'togglefullscreen' }] },
    { label: 'Help', submenu: [{ label: 'Open in browser', click: () => shell.openExternal(APP_URL) }] },
  ]));
}

if (!app.requestSingleInstanceLock()) {
  app.quit();
} else {
  app.on('second-instance', () => {
    if (!mainWindow) return;
    if (mainWindow.isMinimized()) mainWindow.restore();
    mainWindow.focus();
  });

  app.whenReady().then(() => {
    // The webmail needs no camera, microphone, location etc.; only allow notifications.
    session.defaultSession.setPermissionRequestHandler((_wc, permission, callback) => callback(permission === 'notifications'));
    buildMenu();
    createWindow();
    app.on('activate', () => { if (!mainWindow) createWindow(); });
  });

  app.on('window-all-closed', () => { if (process.platform !== 'darwin') app.quit(); });
}

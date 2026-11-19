const { app, BaseWindow, WebContentsView, Menu, Notification, ipcMain, shell, session, dialog } = require('electron');
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');

const APP_URL = 'https://mail.bykiva.com/webmail/';
const APP_ORIGIN = new URL(APP_URL).origin;
const ICON = path.join(__dirname, 'build', 'icon.png');
const SIDEBAR_WIDTH = 68;
const STATUS_POLL_MS = 15000;
const DEFAULT_BOUNDS = { width: 1320, height: 840 };

let win = null;
let sidebar = null;
let accounts = [];             // [{ id, email }]
const views = new Map();       // id -> WebContentsView
const status = new Map();      // id -> { email, unread }
let activeId = null;

const dataFile = (name) => path.join(app.getPath('userData'), name);
const readJson = (name, fallback) => { try { return JSON.parse(fs.readFileSync(dataFile(name), 'utf8')); } catch { return fallback; } };
const writeJson = (name, value) => { try { fs.writeFileSync(dataFile(name), JSON.stringify(value)); } catch { /* best effort */ } };
const saveAccounts = () => writeJson('accounts.json', accounts.map(({ id, email }) => ({ id, email })));

const isAppUrl = (url) => { try { return new URL(url).origin === APP_ORIGIN; } catch { return false; } };

// Links in emails are untrusted: anything outside the webmail opens in the system browser, and
// only plain web/mail links are handed over (never file:, javascript: or custom app schemes).
function openExternal(url) {
  try {
    if (['https:', 'http:', 'mailto:'].includes(new URL(url).protocol)) shell.openExternal(url);
  } catch { /* malformed URL: ignore */ }
}

function layout() {
  if (!win) return;
  const { width, height } = win.getContentBounds();
  sidebar.setBounds({ x: 0, y: 0, width: SIDEBAR_WIDTH, height });
  for (const [id, view] of views) {
    view.setBounds({ x: SIDEBAR_WIDTH, y: 0, width: Math.max(0, width - SIDEBAR_WIDTH), height });
    view.setVisible(id === activeId);
  }
}

function pushState() {
  if (!sidebar) return;
  sidebar.webContents.send('accounts:update', {
    activeId,
    accounts: accounts.map(a => ({ id: a.id, email: status.get(a.id)?.email || a.email || '', unread: status.get(a.id)?.unread || 0 })),
  });
  const total = [...status.values()].reduce((n, s) => n + (s.unread || 0), 0);
  app.setBadgeCount(total);
}

function createAccountView(account) {
  // A persistent partition per account keeps every account signed in at the same time.
  const view = new WebContentsView({
    webPreferences: {
      partition: `persist:account-${account.id}`,
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
      spellcheck: true,
      backgroundThrottling: false, // keep polling the server while another account is shown
    },
  });
  const wc = view.webContents;
  wc.setWindowOpenHandler(({ url }) => { openExternal(url); return { action: 'deny' }; });
  wc.on('will-navigate', (event, url) => {
    if (isAppUrl(url) || url.startsWith('file:')) return;
    event.preventDefault();
    openExternal(url);
  });
  // -3 is ERR_ABORTED (a navigation replaced by another), not a real failure.
  wc.on('did-fail-load', (_e, code, _desc, _url, isMainFrame) => {
    if (isMainFrame && code !== -3) wc.loadFile(path.join(__dirname, 'offline.html'));
  });
  // Refreshes the sidebar as soon as this account's page loads, instead of waiting for the
  // next 15s poll tick — otherwise a newly added or just-signed-in account sits on "?" too long.
  wc.on('did-finish-load', () => { pollStatus(); });
  win.contentView.addChildView(view);
  views.set(account.id, view);
  wc.loadURL(APP_URL);
  return view;
}

// Reads who is signed in and the inbox unread count from each account's page.
async function pollStatus() {
  for (const account of accounts) {
    const view = views.get(account.id);
    if (!view || view.webContents.isDestroyed() || !isAppUrl(view.webContents.getURL())) continue;
    let info;
    try {
      info = await view.webContents.executeJavaScript(`(() => {
        const who = document.querySelector('.user-chip .email');
        const inbox = [...document.querySelectorAll('.folder')].find(f => (f.querySelector('.name') || {}).textContent === 'Inbox');
        const count = inbox && inbox.querySelector('.count');
        return { email: who ? who.textContent.trim() : '', unread: count ? parseInt(count.textContent, 10) || 0 : 0 };
      })()`, true);
    } catch { continue; }
    const before = status.get(account.id);
    status.set(account.id, info);
    if (info.email && info.email !== account.email) { account.email = info.email; saveAccounts(); }
    if (before && info.unread > before.unread && Notification.isSupported()) {
      const n = new Notification({ title: 'New mail', body: `${info.email}: ${info.unread} unread`, icon: ICON });
      n.on('click', () => selectAccount(account.id));
      n.show();
    }
  }
  pushState();
}

function selectAccount(id) {
  if (!views.has(id)) return;
  activeId = id;
  writeJson('active.json', { id });
  layout();
  views.get(id).webContents.focus();
  if (win.isMinimized()) win.restore();
  win.focus();
  pushState();
}

function addAccount() {
  const account = { id: crypto.randomUUID(), email: '' };
  accounts.push(account);
  saveAccounts();
  createAccountView(account);
  selectAccount(account.id);
}

async function removeAccount(id) {
  const account = accounts.find(a => a.id === id);
  if (!account) return;
  const label = status.get(id)?.email || account.email || 'this account';
  const { response } = await dialog.showMessageBox(win, {
    type: 'question', buttons: ['Remove', 'Cancel'], defaultId: 1, cancelId: 1,
    message: `Remove ${label} from Bykiva Mail?`,
    detail: 'You will be signed out on this computer. The mailbox and its mail on the server are not affected.',
  });
  if (response !== 0) return;
  const view = views.get(id);
  win.contentView.removeChildView(view);
  view.webContents.close();
  views.delete(id);
  status.delete(id);
  await session.fromPartition(`persist:account-${id}`).clearStorageData();
  accounts = accounts.filter(a => a.id !== id);
  saveAccounts();
  if (!accounts.length) addAccount();
  else if (activeId === id) selectAccount(accounts[0].id);
  else pushState();
}

function createWindow() {
  const bounds = { ...DEFAULT_BOUNDS, ...readJson('window.json', {}) };
  win = new BaseWindow({ ...bounds, minWidth: 960, minHeight: 600, title: 'Bykiva Mail', icon: ICON, autoHideMenuBar: true });
  if (bounds.maximized) win.maximize();

  sidebar = new WebContentsView({
    webPreferences: { preload: path.join(__dirname, 'preload.js'), contextIsolation: true, nodeIntegration: false, sandbox: true },
  });
  sidebar.webContents.setWindowOpenHandler(() => ({ action: 'deny' }));
  sidebar.webContents.on('will-navigate', (e) => e.preventDefault());
  win.contentView.addChildView(sidebar);
  sidebar.webContents.loadFile(path.join(__dirname, 'sidebar.html'));
  sidebar.webContents.on('did-finish-load', pushState);

  accounts = readJson('accounts.json', []).filter(a => a && typeof a.id === 'string');
  if (!accounts.length) accounts = [{ id: crypto.randomUUID(), email: '' }];
  saveAccounts();
  for (const account of accounts) createAccountView(account);
  const remembered = readJson('active.json', {}).id;
  selectAccount(accounts.some(a => a.id === remembered) ? remembered : accounts[0].id);

  // 'resize' alone misses some window managers' maximize/tile/fullscreen transitions
  // (e.g. GNOME edge-snap), which left the panes at their old size inside a bigger window.
  for (const evt of ['resize', 'maximize', 'unmaximize', 'enter-full-screen', 'leave-full-screen'])
    win.on(evt, layout);
  win.on('close', () => {
    if (!win.isMinimized() && !win.isFullScreen()) writeJson('window.json', { ...win.getNormalBounds(), maximized: win.isMaximized() });
  });
  win.on('closed', () => { win = null; sidebar = null; views.clear(); });
  layout();
  pollStatus();
  setInterval(pollStatus, STATUS_POLL_MS);
}

function buildMenu() {
  const reloadActive = () => activeId && views.get(activeId)?.webContents.loadURL(APP_URL);
  Menu.setApplicationMenu(Menu.buildFromTemplate([
    { label: 'File', submenu: [
      { label: 'Add account', accelerator: 'CmdOrCtrl+N', click: addAccount },
      { label: 'Reload', accelerator: 'CmdOrCtrl+R', click: reloadActive },
      { type: 'separator' }, { role: 'quit' },
    ] },
    { role: 'editMenu' },
    { label: 'View', submenu: [{ role: 'togglefullscreen' }] },
    { label: 'Help', submenu: [{ label: 'Open webmail in browser', click: () => shell.openExternal(APP_URL) }] },
  ]));
}

ipcMain.on('accounts:select', (_e, id) => selectAccount(String(id)));
ipcMain.on('accounts:add', () => addAccount());
ipcMain.on('accounts:remove', (_e, id) => { removeAccount(String(id)); });

if (!app.requestSingleInstanceLock()) {
  app.quit();
} else {
  app.on('second-instance', () => {
    if (!win) return;
    if (win.isMinimized()) win.restore();
    win.focus();
  });

  app.whenReady().then(() => {
    // The webmail needs no camera, microphone, location etc.; only allow notifications, in every account.
    app.on('session-created', (s) => s.setPermissionRequestHandler((_wc, permission, cb) => cb(permission === 'notifications')));
    session.defaultSession.setPermissionRequestHandler((_wc, permission, cb) => cb(permission === 'notifications'));
    if (process.platform === 'win32') app.setAppUserModelId('com.bykiva.mail');
    buildMenu();
    createWindow();
    app.on('activate', () => { if (!win) createWindow(); });
  });

  app.on('window-all-closed', () => { if (process.platform !== 'darwin') app.quit(); });
}

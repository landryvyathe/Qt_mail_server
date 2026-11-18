import PostalMime, { decodeWords, addressParser } from './vendor/postal-mime/postal-mime.js';

const PAGE_SIZE = 50;
const POLL_MS = 60000;
const MAX_SEND_BYTES = 25 * 1024 * 1024;
const FOLDER_ORDER = ['INBOX', 'Drafts', 'Sent', 'Trash', 'Junk'];
const FOLDER_LABELS = { INBOX: 'Inbox', Drafts: 'Drafts', Sent: 'Sent Items', Trash: 'Trash', Junk: 'Spam' };
const IMPORTANT_LABEL = 'important';

// Gmail-style views that aren't real IMAP folders: they reuse All-Mail search (folder=ALL)
// with a fixed query the user's own search terms are appended to.
const VIRTUAL_NAV = [
  { key: 'Starred', label: 'Starred', navIcon: 'star', folder: 'ALL', baseQuery: 'is:starred' },
  { key: 'Important', label: 'Important', navIcon: 'important', folder: 'ALL', baseQuery: `label:${IMPORTANT_LABEL}` },
];
const ALL_MAIL_NAV = { key: 'AllMail', label: 'All Mail', navIcon: 'allmail', folder: 'ALL', baseQuery: '' };

const state = {
  token: null, email: null, folders: [], usedBytes: 0, quotaBytes: 0,
  folder: 'INBOX', navKey: 'INBOX', baseQuery: '', userQuery: '',
  messages: [], total: 0, offset: 0, query: '',
  selected: new Set(), openUid: null, showImages: false, blobUrls: [],
};

// ---------- tiny DOM helpers ----------
function h(tag, attrs = {}, ...children) {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (v === false || v == null) continue;
    if (k === 'class') el.className = v;
    else if (k.startsWith('on')) el.addEventListener(k.slice(2), v);
    else if (k === 'dataset') Object.assign(el.dataset, v);
    else el.setAttribute(k, v === true ? '' : v);
  }
  for (const c of children.flat()) if (c != null && c !== false) el.append(c instanceof Node ? c : String(c));
  return el;
}

const ICONS = {
  mail: 'M3 6h18v12H3z M3 7l9 6 9-6', inbox: 'M3 13l3-8h12l3 8v6H3z M3 13h5l1 2h6l1-2h5',
  send: 'M4 12l16-8-6 16-2-7z', drafts: 'M4 20h4L19 9l-4-4L4 16z', trash: 'M5 7h14 M9 7V4h6v3 M7 7l1 13h8l1-13',
  junk: 'M12 3l9 16H3z M12 10v4 M12 17v.5', folder: 'M3 6h6l2 2h10v11H3z', plus: 'M12 5v14 M5 12h14',
  refresh: 'M20 11a8 8 0 10-2 6 M20 5v6h-6', reply: 'M10 8L4 13l6 5 M4 13h10a6 6 0 016 6',
  replyAll: 'M8 8L3 13l5 5 M13 8l-5 5 5 5 M8 13h7a6 6 0 016 6', forward: 'M14 8l6 5-6 5 M20 13H10a6 6 0 00-6 6',
  flag: 'M5 21V4 M5 4h11l-2 4 2 4H5', read: 'M3 9l9-6 9 6v10H3z M3 9l9 6 9-6', move: 'M3 6h6l2 2h10v11H3z M10 14h6 M13 11l3 3-3 3',
  clip: 'M8 12l7-7a3 3 0 014 4l-9 9a5 5 0 01-7-7l8-8', search: 'M11 4a7 7 0 100 14 7 7 0 000-14z M20 20l-4-4',
  menu: 'M4 7h16 M4 12h16 M4 17h16', back: 'M15 5l-7 7 7 7', close: 'M6 6l12 12 M18 6L6 18', logout: 'M15 4h4v16h-4 M10 8l-4 4 4 4 M6 12h10',
  more: 'M5 12h.01 M12 12h.01 M19 12h.01', download: 'M12 4v11 M7 10l5 5 5-5 M5 20h14',
  star: 'M12 3l2.6 5.6 6.1.6-4.6 4.2 1.3 6-5.4-3.2-5.4 3.2 1.3-6-4.6-4.2 6.1-.6z',
  important: 'M4 6h11l5 6-5 6H4z', allmail: 'M3 6h18v12H3z M3 7l9 6 9-6 M3 17l6-5 M21 17l-6-5',
  gear: 'M12 9a3 3 0 100 6 3 3 0 000-6z M19 12a7 7 0 00-.2-1.6l2-1.6-2-3.4-2.4.8a7 7 0 00-2.8-1.6L13 2h-4l-.6 2.6a7 7 0 00-2.8 1.6l-2.4-.8-2 3.4 2 1.6A7 7 0 003 12a7 7 0 00.2 1.6l-2 1.6 2 3.4 2.4-.8a7 7 0 002.8 1.6L9 22h4l.6-2.6a7 7 0 002.8-1.6l2.4.8 2-3.4-2-1.6c.13-.52.2-1.06.2-1.6z',
  snoozed: 'M12 7v5l3 3 M12 2a10 10 0 100 20 10 10 0 000-20z',
};
function icon(name) {
  const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
  svg.setAttribute('viewBox', '0 0 24 24');
  svg.setAttribute('class', 'icon');
  svg.setAttribute('aria-hidden', 'true');
  for (const d of ICONS[name].split(' M').map((p, i) => (i ? 'M' + p : p))) {
    const path = document.createElementNS('http://www.w3.org/2000/svg', 'path');
    path.setAttribute('d', d);
    svg.append(path);
  }
  return svg;
}

let toastTimer;
function toast(text) {
  const el = document.getElementById('toast');
  el.textContent = text;
  el.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { el.hidden = true; }, 3500);
}

const store = {
  get(k) { try { return localStorage.getItem(k); } catch { return null; } },
  set(k, v) { try { localStorage.setItem(k, v); } catch { /* storage unavailable */ } },
  del(k) { try { localStorage.removeItem(k); } catch { /* storage unavailable */ } },
};

// ---------- API ----------
async function api(method, path, body, raw = false) {
  const res = await fetch(path, {
    method,
    headers: { Authorization: `Bearer ${state.token}`, ...(body ? { 'Content-Type': 'application/json' } : {}) },
    body: body ? JSON.stringify(body) : undefined,
  });
  if (res.status === 401) { signOut(true); throw new Error('Your session expired, please sign in again'); }
  if (!res.ok) {
    let msg = `Request failed (${res.status})`;
    try { msg = (await res.json()).error || msg; } catch { /* non-JSON error */ }
    throw new Error(msg);
  }
  return raw ? res.arrayBuffer() : res.json();
}

// ---------- formatting ----------
function decode(s) { try { return decodeWords(s || ''); } catch { return s || ''; } }
function parseAddresses(s) {
  try {
    return addressParser(decode(s)).flatMap(a => (a.group ? a.group : [a])).filter(a => a.address || a.name);
  } catch { return []; }
}
function displayName(a) { return a ? (a.name || a.address || '') : ''; }
function addrText(list) { return (list || []).map(a => (a.name ? `${a.name} <${a.address}>` : a.address)).join(', '); }
function initials(text) {
  const parts = (text || '?').replace(/[<>"@.]/g, ' ').trim().split(/\s+/);
  return ((parts[0] || '?')[0] + (parts[1] ? parts[1][0] : '')).toUpperCase();
}
function avatarColor(text) {
  let n = 0;
  for (const c of text || '') n = (n * 31 + c.charCodeAt(0)) >>> 0;
  return `hsl(${n % 360} 45% 45%)`;
}
function formatWhen(dateHeader, received) {
  const d = new Date(dateHeader || received);
  const when = isNaN(d) ? new Date(received) : d;
  const now = new Date();
  if (when.toDateString() === now.toDateString()) return when.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
  if (when.getFullYear() === now.getFullYear()) return when.toLocaleDateString([], { day: 'numeric', month: 'short' });
  return when.toLocaleDateString();
}
function formatSize(n) {
  if (n < 1024) return `${n} B`;
  if (n < 1048576) return `${(n / 1024).toFixed(0)} KB`;
  return `${(n / 1048576).toFixed(1)} MB`;
}
function folderLabel(name) { return FOLDER_LABELS[name] || name.split('.').pop(); }
function navLabel() {
  const v = [...VIRTUAL_NAV, ALL_MAIL_NAV].find(d => d.key === state.navKey);
  return v ? v.label : folderLabel(state.navKey);
}
// Messages can come from mixed real folders when viewing All Mail/Starred/Important;
// group by each message's own folder so bulk actions hit the right IMAP mailbox.
function groupByFolder(uids) {
  const groups = new Map();
  for (const uid of uids) {
    const m = state.messages.find(x => x.uid === uid);
    const folder = m ? m.folder : state.folder;
    if (!groups.has(folder)) groups.set(folder, []);
    groups.get(folder).push(uid);
  }
  return groups;
}
function folderIcon(f) {
  return { inbox: 'inbox', sent: 'send', drafts: 'drafts', trash: 'trash', junk: 'junk', snoozed: 'snoozed' }[f.role] || 'folder';
}

// ---------- login ----------
function renderLogin(message = '') {
  const app = document.getElementById('app');
  const err = h('div', { class: 'error', role: 'alert' }, message);
  const email = h('input', { class: 'input', id: 'email', type: 'email', autocomplete: 'username', required: true,
                             placeholder: 'name@bykiva.com', value: store.get('lastEmail') || '' });
  const pass = h('input', { class: 'input', id: 'pass', type: 'password', autocomplete: 'current-password', required: true });
  const submit = h('button', { class: 'btn btn-primary btn-block', type: 'submit' }, 'Sign in');
  const form = h('form', {
    onsubmit: async (e) => {
      e.preventDefault();
      submit.disabled = true;
      err.textContent = '';
      try {
        const res = await fetch('/api/mail/login', {
          method: 'POST', headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ email: email.value.trim(), password: pass.value }),
        });
        const data = await res.json();
        if (!res.ok) throw new Error(data.error || 'Sign-in failed');
        state.token = data.token;
        state.email = data.email;
        store.set('token', data.token);
        store.set('lastEmail', data.email);
        startApp();
      } catch (ex) {
        err.textContent = ex.message;
        submit.disabled = false;
      }
    },
  },
  h('div', { class: 'field' }, h('label', { for: 'email' }, 'Email address'), email),
  h('div', { class: 'field' }, h('label', { for: 'pass' }, 'Password'), pass),
  err, submit);

  app.replaceChildren(h('main', { class: 'login' }, h('div', { class: 'login-card' },
    h('div', { class: 'brand' }, h('span', { class: 'brand-mark' }, icon('mail')), 'Bykiva Mail'),
    h('h1', {}, 'Sign in'), h('p', {}, 'Use your @bykiva.com email account.'), form)));
  (email.value ? pass : email).focus();
}

async function signOut(expired = false) {
  if (!expired && state.token) { try { await api('POST', '/api/mail/logout'); } catch { /* already gone */ } }
  state.token = null;
  store.del('token');
  clearInterval(pollTimer);
  renderLogin(expired ? 'Your session expired, please sign in again.' : '');
}

// ---------- shell ----------
let pollTimer;
const ui = {};

function startApp() {
  const app = document.getElementById('app');
  ui.nav = h('nav', { class: 'nav', 'aria-label': 'Folders' });
  ui.list = h('section', { class: 'list-pane', 'aria-label': 'Messages' });
  ui.read = h('section', { class: 'read-pane', 'aria-label': 'Reading pane' });
  const searchInput = h('input', { type: 'search', placeholder: 'Search mail', 'aria-label': 'Search mail' });
  let searchTimer;
  searchInput.addEventListener('input', () => {
    clearTimeout(searchTimer);
    searchTimer = setTimeout(() => { state.userQuery = searchInput.value.trim(); loadMessages(true); }, 350);
  });
  ui.shell = h('div', { class: 'shell' },
    h('header', { class: 'topbar' },
      h('button', { class: 'menu-btn', 'aria-label': 'Folders', onclick: () => ui.nav.classList.toggle('open') }, icon('menu')),
      h('span', { class: 'brand-mark' }, icon('mail')), h('span', { class: 'title' }, 'Bykiva Mail'),
      h('label', { class: 'search' }, icon('search'), searchInput),
      h('button', { class: 'user-chip', title: 'Account', onclick: (e) => userMenu(e.currentTarget) },
        h('span', { class: 'avatar', style: `background:${avatarColor(state.email)}` }, initials(state.email)),
        h('span', { class: 'email' }, state.email))),
    h('div', { class: 'main' }, ui.nav, ui.list, ui.read));
  app.replaceChildren(ui.shell);
  renderReaderPlaceholder();
  refreshAll();
  clearInterval(pollTimer);
  pollTimer = setInterval(() => refreshAll(true), POLL_MS);
}

function userMenu(anchor) {
  popover(anchor, [
    { label: state.email, disabled: true },
    { sep: true },
    { label: 'Sign out', icon: 'logout', action: () => signOut() },
  ]);
}

async function refreshAll(quiet = false) {
  try {
    await loadFolders();
    if (!quiet || (state.offset === 0 && !state.query)) await loadMessages(!quiet, quiet);
  } catch (e) { if (!quiet) toast(e.message); }
}

// ---------- folders ----------
async function loadFolders() {
  const data = await api('GET', '/api/mail/folders');
  state.folders = data.folders.sort((a, b) => {
    const ia = FOLDER_ORDER.indexOf(a.name), ib = FOLDER_ORDER.indexOf(b.name);
    if (ia !== -1 || ib !== -1) return (ia === -1 ? 99 : ia) - (ib === -1 ? 99 : ib);
    return a.name.localeCompare(b.name);
  });
  state.usedBytes = data.usedBytes;
  state.quotaBytes = data.quotaBytes;
  renderNav();
}

// Real folders Gmail groups away from the main list (still real IMAP folders, just ordered last).
const TAIL_FOLDERS = ['Junk', 'Trash'];

function virtualNavItem(def, extra = {}) {
  return h('li', {
    class: `folder${def.key === state.navKey ? ' active' : ''}`,
    tabindex: 0, role: 'button', 'aria-current': def.key === state.navKey ? 'true' : false,
    onclick: () => selectNav(def),
    onkeydown: (e) => { if (e.key === 'Enter') selectNav(def); },
  }, icon(def.navIcon), h('span', { class: 'name' }, def.label), extra.count ? h('span', { class: 'count' }, extra.count) : null);
}

function renderNav() {
  const byName = (n) => state.folders.find(f => f.name === n);

  const realItem = (f) => {
    const li = h('li', {
      class: `folder${f.name === state.navKey ? ' active' : ''}${f.name.includes('.') ? ' child' : ''}`,
      tabindex: 0, role: 'button', 'aria-current': f.name === state.navKey ? 'true' : false,
      onclick: () => selectFolder(f.name),
      onkeydown: (e) => { if (e.key === 'Enter') selectFolder(f.name); },
      oncontextmenu: (e) => { if (!f.role) { e.preventDefault(); folderMenu(e.currentTarget, f); } },
    }, icon(folderIcon(f)), h('span', { class: 'name' }, folderLabel(f.name)),
    f.unseen && f.role !== 'sent' && f.role !== 'drafts' ? h('span', { class: 'count' }, f.unseen)
      : f.role === 'drafts' && f.total ? h('span', { class: 'count' }, f.total) : null);
    li.addEventListener('dragover', (e) => { if (f.name !== state.folder) { e.preventDefault(); li.classList.add('drop-target'); } });
    li.addEventListener('dragleave', () => li.classList.remove('drop-target'));
    li.addEventListener('drop', (e) => {
      e.preventDefault();
      li.classList.remove('drop-target');
      const uids = JSON.parse(e.dataTransfer.getData('application/x-uids') || '[]');
      if (uids.length) moveTo(uids, f.name);
    });
    return li;
  };

  const inbox = byName('INBOX');
  const sent = byName('Sent');
  const drafts = byName('Drafts');
  const snoozed = byName('Snoozed');
  const custom = state.folders.filter(f => !['INBOX', 'Sent', 'Drafts', 'Snoozed', ...TAIL_FOLDERS].includes(f.name));
  const tail = TAIL_FOLDERS.map(byName).filter(Boolean);

  const items = [
    inbox ? realItem(inbox) : null,
    virtualNavItem(VIRTUAL_NAV[0]), // Starred
    snoozed ? realItem(snoozed) : null,
    virtualNavItem(VIRTUAL_NAV[1]), // Important
    sent ? realItem(sent) : null,
    drafts ? realItem(drafts) : null,
    virtualNavItem(ALL_MAIL_NAV),
    ...custom.map(realItem),
    ...tail.map(realItem),
  ].filter(Boolean);

  const pct = state.quotaBytes ? Math.min(100, (state.usedBytes / state.quotaBytes) * 100) : 0;
  ui.nav.replaceChildren(
    h('button', { class: 'btn btn-primary', onclick: () => openCompose('new') }, icon('plus'), 'New mail'),
    h('ul', { class: 'folders' }, items),
    h('div', { class: 'nav-footer' },
      h('button', { class: 'btn btn-ghost', onclick: createFolder }, icon('plus'), 'New folder'),
      h('button', { class: 'btn btn-ghost', onclick: openSettings }, icon('gear'), 'Settings'),
      h('div', { class: 'quota' }, `${formatSize(state.usedBytes)} of ${formatSize(state.quotaBytes)} used`,
        h('div', { class: 'quota-bar' }, h('span', { style: `width:${pct}%` })))));
}

function selectFolder(name) {
  state.folder = name;
  state.navKey = name;
  state.baseQuery = '';
  applyQuery();
}

function selectNav(def) {
  state.folder = def.folder;
  state.navKey = def.key;
  state.baseQuery = def.baseQuery;
  applyQuery();
}

function applyQuery() {
  state.userQuery = '';
  state.openUid = null;
  ui.nav.classList.remove('open');
  ui.shell.classList.remove('reading');
  const search = ui.shell.querySelector('.search input');
  if (search) search.value = '';
  renderNav();
  renderReaderPlaceholder();
  loadMessages(true).catch(e => toast(e.message));
}

async function folderAction(body, okText) {
  try {
    const data = await api('POST', '/api/mail/folders', body);
    state.folders = data.folders;
    await loadFolders();
    toast(okText);
  } catch (e) { toast(e.message); }
}

function createFolder() {
  const name = prompt('Folder name (use "." for a sub-folder, e.g. Projects.2026)');
  if (name && name.trim()) folderAction({ action: 'create', name: name.trim() }, 'Folder created');
}

function folderMenu(anchor, f) {
  popover(anchor, [
    { label: 'Rename', action: () => {
      const n = prompt('New name', f.name);
      if (n && n.trim() && n.trim() !== f.name) {
        folderAction({ action: 'rename', name: f.name, newName: n.trim() }, 'Folder renamed')
          .then(() => { if (state.folder === f.name) selectFolder(n.trim()); });
      }
    } },
    { label: 'Delete', icon: 'trash', action: () => {
      if (confirm(`Delete "${f.name}" and all its messages?`)) {
        folderAction({ action: 'delete', name: f.name }, 'Folder deleted')
          .then(() => { if (state.folder === f.name) selectFolder('INBOX'); });
      }
    } },
  ]);
}

// ---------- labels & settings ----------
let importantLabelReady = false;
async function ensureImportantLabel() {
  if (importantLabelReady) return;
  try { await api('POST', '/api/mail/labels', { action: 'create', name: IMPORTANT_LABEL, color: '#e8710a' }); }
  catch { /* already exists */ }
  importantLabelReady = true;
}

async function openSettings() {
  let data;
  try { data = await api('GET', '/api/mail/settings'); } catch (e) { toast(e.message); return; }
  const vac = data.vacation || {};
  const sigInput = h('textarea', { rows: 5 }, data.signature || '');
  const vacEnabled = h('input', { type: 'checkbox', checked: !!vac.enabled });
  const vacSubject = h('input', { value: vac.subject || '' });
  const vacBody = h('textarea', { rows: 5 }, vac.body || '');
  const close = () => backdrop.remove();
  const save = async () => {
    try {
      await api('POST', '/api/mail/settings', {
        signature: sigInput.value,
        vacation: { enabled: vacEnabled.checked, subject: vacSubject.value, body: vacBody.value, start: vac.start || 0, end: vac.end || 0 },
      });
      toast('Settings saved');
      close();
    } catch (e) { toast(e.message); }
  };
  const backdrop = h('div', { class: 'compose-backdrop', onkeydown: (e) => { if (e.key === 'Escape') close(); } },
    h('div', { class: 'compose settings-dialog', role: 'dialog', 'aria-modal': 'true', 'aria-label': 'Settings' },
      h('div', { class: 'compose-head' }, h('h2', {}, 'Settings'),
        h('button', { class: 'btn btn-ghost', 'aria-label': 'Close', onclick: close }, icon('close'))),
      h('div', { class: 'compose-row' }, h('label', {}, 'Signature'), sigInput),
      h('div', { class: 'compose-row' }, h('label', {}, h('span', {}, vacEnabled, ' Vacation responder')),),
      h('div', { class: 'compose-row' }, h('label', {}, 'Subject'), vacSubject),
      h('div', { class: 'compose-row' }, h('label', {}, 'Message'), vacBody),
      h('div', { class: 'compose-foot' }, h('button', { class: 'btn btn-primary', onclick: save }, 'Save'))));
  document.body.append(backdrop);
}

// ---------- message list ----------
async function loadMessages(reset = false, quiet = false) {
  if (reset) { state.offset = 0; state.selected.clear(); }
  state.query = [state.baseQuery, state.userQuery].filter(Boolean).join(' ').trim();
  const q = new URLSearchParams({ folder: state.folder, offset: state.offset, limit: PAGE_SIZE });
  if (state.query) q.set('q', state.query);
  if (!quiet && reset) ui.list.replaceChildren(h('div', { class: 'empty' }, 'Loading…'));
  const data = await api('GET', `/api/mail/messages?${q}`);
  state.messages = data.messages;
  state.total = data.total;
  renderList();
}

function renderList() {
  const isTrashLike = state.folder === 'Trash' || state.folder === 'Junk';
  const sel = [...state.selected];
  const tb = (label, iconName, action, disabled = false) =>
    h('button', { class: 'btn btn-ghost', title: label, disabled, onclick: action }, icon(iconName), h('span', {}, label));

  const allBox = h('input', { type: 'checkbox', 'aria-label': 'Select all', title: 'Select all',
    checked: state.messages.length > 0 && sel.length === state.messages.length,
    onchange: (e) => { state.selected = new Set(e.target.checked ? state.messages.map(m => m.uid) : []); renderList(); } });

  const toolbar = h('div', { class: 'toolbar' },
    tb('Delete', 'trash', () => deleteMessages(sel), !sel.length),
    tb('Move', 'move', (e) => moveMenu(e.currentTarget, sel), !sel.length),
    tb('Read', 'read', () => setFlags(sel, 'S', ''), !sel.length),
    tb('Unread', 'mail', () => setFlags(sel, '', 'S'), !sel.length),
    tb('Star', 'star', () => setFlags(sel, 'F', ''), !sel.length),
    tb('Important', 'important', () => toggleImportant(sel, true), !sel.length),
    isTrashLike ? tb('Empty', 'trash', emptyFolder, !state.total) : null,
    tb('Refresh', 'refresh', () => refreshAll()));

  const rows = state.messages.map(m => {
    const fromList = parseAddresses(m.folder === 'Sent' || m.folder === 'Drafts' ? m.to : m.from);
    const who = fromList.map(displayName).join(', ') || (m.folder === 'Drafts' ? '(no recipients)' : '(unknown)');
    const important = (m.labels || []).some(l => l.toLowerCase() === IMPORTANT_LABEL);
    const li = h('li', {
      class: `msg${m.seen ? '' : ' unread'}${m.uid === state.openUid ? ' active' : ''}`, draggable: 'true',
      onclick: (e) => { if (e.target.type !== 'checkbox' && !e.target.closest('.star-btn')) openMessage(m.uid); },
      ondragstart: (e) => {
        const uids = state.selected.has(m.uid) ? [...state.selected] : [m.uid];
        e.dataTransfer.setData('application/x-uids', JSON.stringify(uids));
      },
    },
    h('input', { type: 'checkbox', 'aria-label': 'Select', checked: state.selected.has(m.uid),
      onchange: (e) => { e.target.checked ? state.selected.add(m.uid) : state.selected.delete(m.uid); renderList(); } }),
    h('button', { class: `star-btn${m.flagged ? ' on' : ''}`, title: m.flagged ? 'Unstar' : 'Star', 'aria-label': 'Star',
      onclick: (e) => { e.stopPropagation(); setFlags([m.uid], m.flagged ? '' : 'F', m.flagged ? 'F' : ''); } }, icon('star')),
    h('span', { class: 'who' }, who), h('span', { class: 'when' }, formatWhen(m.date, m.received)),
    h('span', { class: 'subj' }, important ? h('span', { class: 'important-dot', title: 'Important' }, icon('important')) : null,
      decode(m.subject) || '(no subject)'),
    h('span', { class: 'marks' }, m.attachment ? icon('clip') : null, m.answered ? icon('reply') : null,
      m.snoozeUntil ? h('span', { class: 'flag', title: `Snoozed until ${new Date(m.snoozeUntil).toLocaleString()}` }, icon('snoozed')) : null));
    return li;
  });

  const pageEnd = Math.min(state.offset + state.messages.length, state.total);
  ui.list.replaceChildren(
    h('div', { class: 'list-head' }, allBox,
      h('h2', {}, state.userQuery ? `Search: ${state.userQuery}` : navLabel())),
    toolbar,
    rows.length ? h('ul', { class: 'messages' }, rows)
      : h('div', { class: 'empty' }, state.query ? 'No messages match your search.' : 'This folder is empty.'),
    h('div', { class: 'list-foot' },
      h('span', {}, state.total ? `${state.offset + 1}–${pageEnd} of ${state.total}` : ''),
      h('span', {},
        h('button', { class: 'btn btn-ghost', disabled: state.offset === 0,
          onclick: () => { state.offset = Math.max(0, state.offset - PAGE_SIZE); loadMessages().catch(e => toast(e.message)); } }, '‹ Newer'),
        h('button', { class: 'btn btn-ghost', disabled: pageEnd >= state.total,
          onclick: () => { state.offset += PAGE_SIZE; loadMessages().catch(e => toast(e.message)); } }, 'Older ›'))));
}

async function setFlags(uids, add, remove) {
  try {
    for (const [folder, groupUids] of groupByFolder(uids))
      await api('POST', '/api/mail/flags', { folder, uids: groupUids, add, remove });
    await refreshAll(true);
    await loadMessages();
  } catch (e) { toast(e.message); }
}

async function toggleImportant(uids, makeImportant) {
  try {
    await ensureImportantLabel();
    const items = uids.map(uid => {
      const m = state.messages.find(x => x.uid === uid);
      return { folder: m ? m.folder : state.folder, uid };
    });
    await api('POST', '/api/mail/labels', {
      action: 'apply', items,
      add: makeImportant ? [IMPORTANT_LABEL] : [], remove: makeImportant ? [] : [IMPORTANT_LABEL],
    });
    await loadMessages();
  } catch (e) { toast(e.message); }
}

async function deleteMessages(uids) {
  if (!uids.length) return;
  const groups = groupByFolder(uids);
  const permanent = [...groups.keys()].every(f => f === 'Trash');
  if (permanent && !confirm(`Permanently delete ${uids.length} message(s)?`)) return;
  try {
    for (const [folder, groupUids] of groups) await api('POST', '/api/mail/delete', { folder, uids: groupUids });
    if (uids.includes(state.openUid)) renderReaderPlaceholder();
    toast(permanent ? 'Deleted permanently' : 'Moved to Trash');
    state.selected.clear();
    await refreshAll(true);
    await loadMessages();
  } catch (e) { toast(e.message); }
}

async function moveTo(uids, to) {
  try {
    for (const [folder, groupUids] of groupByFolder(uids)) {
      if (folder === to) continue;
      await api('POST', '/api/mail/move', { folder, uids: groupUids, to });
    }
    if (uids.includes(state.openUid)) renderReaderPlaceholder();
    toast(`Moved to ${folderLabel(to)}`);
    state.selected.clear();
    await refreshAll(true);
    await loadMessages();
  } catch (e) { toast(e.message); }
}

function moveMenu(anchor, uids) {
  popover(anchor, state.folders.filter(f => f.name !== state.folder)
    .map(f => ({ label: folderLabel(f.name), icon: folderIcon(f), action: () => moveTo(uids, f.name) })));
}

async function emptyFolder() {
  if (!confirm(`Permanently delete everything in ${folderLabel(state.folder)}?`)) return;
  await folderAction({ action: 'empty', name: state.folder }, 'Folder emptied');
  renderReaderPlaceholder();
  await loadMessages(true);
}

// ---------- reading ----------
function releaseBlobs() {
  state.blobUrls.forEach(u => URL.revokeObjectURL(u));
  state.blobUrls = [];
}

function renderReaderPlaceholder() {
  releaseBlobs();
  state.openUid = null;
  ui.read.replaceChildren(h('div', { class: 'placeholder' },
    h('div', {}, icon('mail'), h('p', {}, 'Select a message to read it.'))));
}

async function fetchParsed(folder, uid, peek = false) {
  const raw = await api('GET', `/api/mail/message?folder=${encodeURIComponent(folder)}&uid=${uid}${peek ? '&peek=1' : ''}`, null, true);
  return PostalMime.parse(raw);
}

async function openMessage(uid) {
  const meta = state.messages.find(m => m.uid === uid);
  const folder = meta ? meta.folder : state.folder;
  if (folder === 'Drafts') {
    try { openCompose('draft', { email: await fetchParsed('Drafts', uid, true), uid }); } catch (e) { toast(e.message); }
    return;
  }
  state.openUid = uid;
  state.showImages = false;
  ui.shell.classList.add('reading');
  ui.read.replaceChildren(h('div', { class: 'placeholder' }, 'Loading…'));
  try {
    const email = await fetchParsed(folder, uid);
    if (state.openUid !== uid) return;
    renderReader(email, uid, folder);
    if (meta && !meta.seen) { meta.seen = true; loadFolders().catch(() => {}); }
    renderList();
  } catch (e) {
    ui.read.replaceChildren(h('div', { class: 'placeholder' }, `Could not open this message: ${e.message}`));
  }
}

function attachmentUrl(att) {
  const url = URL.createObjectURL(new Blob([att.content], { type: att.mimeType || 'application/octet-stream' }));
  state.blobUrls.push(url);
  return url;
}

function renderReader(email, uid, folder) {
  releaseBlobs();
  const from = email.from || {};
  const fromName = displayName(from);
  const files = email.attachments.filter(a => a.disposition !== 'inline' || !a.contentId);
  const cidMap = {};
  for (const a of email.attachments) if (a.contentId) cidMap[a.contentId.replace(/^<|>$/g, '')] = attachmentUrl(a);

  const body = h('div', { class: 'reader-body' });
  const reader = h('article', { class: 'reader' },
    h('header', { class: 'reader-head' },
      h('h1', {}, email.subject || '(no subject)'),
      h('div', { class: 'sender' },
        h('span', { class: 'avatar', style: `background:${avatarColor(from.address)}` }, initials(fromName)),
        h('div', { class: 'meta' },
          h('div', { class: 'from' }, fromName, ' ', from.name ? h('small', {}, `<${from.address}>`) : null),
          h('div', { class: 'rcpt' }, `To: ${addrText(email.to) || '—'}`),
          email.cc && email.cc.length ? h('div', { class: 'rcpt' }, `Cc: ${addrText(email.cc)}`) : null,
          h('div', { class: 'rcpt' }, email.date ? new Date(email.date).toLocaleString() : '')))),
    h('div', { class: 'reader-actions' },
      h('button', { class: 'btn btn-ghost back-btn', onclick: () => { ui.shell.classList.remove('reading'); renderReaderPlaceholder(); } }, icon('back'), 'Back'),
      h('button', { class: 'btn', onclick: () => openCompose('reply', { email, uid, folder }) }, icon('reply'), 'Reply'),
      h('button', { class: 'btn', onclick: () => openCompose('replyAll', { email, uid, folder }) }, icon('replyAll'), 'Reply all'),
      h('button', { class: 'btn', onclick: () => openCompose('forward', { email, uid, folder }) }, icon('forward'), 'Forward'),
      h('button', { class: 'btn btn-ghost', onclick: () => deleteMessages([uid]) }, icon('trash'), 'Delete'),
      h('button', { class: 'btn btn-ghost', onclick: (e) => moveMenu(e.currentTarget, [uid]) }, icon('move'), 'Move'),
      h('button', { class: 'btn btn-ghost', onclick: () => setFlags([uid], '', 'S') }, icon('mail'), 'Mark unread')),
    body,
    files.length ? h('div', { class: 'attachments' }, files.map(a => h('a', {
      class: 'att', href: attachmentUrl(a), download: a.filename || 'attachment', title: a.filename || 'attachment',
    }, icon('clip'), h('span', {}, a.filename || 'attachment'), h('small', {}, formatSize(a.content.byteLength || 0))))) : null);
  ui.read.replaceChildren(reader);

  if (email.html) {
    const { html, hasRemote } = sanitizeHtml(email.html, cidMap, state.showImages);
    if (hasRemote && !state.showImages) {
      reader.insertBefore(h('div', { class: 'notice' }, 'Pictures from the internet are blocked to protect your privacy.',
        h('button', { class: 'btn', onclick: () => { state.showImages = true; renderReader(email, uid, folder); } }, 'Show pictures')), body);
    }
    body.append(mailFrame(html, state.showImages));
  } else {
    body.append(h('pre', {}, linkify(email.text || '')));
  }
}

// Strips active content from untrusted email HTML; the iframe sandbox is the second layer.
function sanitizeHtml(html, cidMap, allowRemote) {
  const doc = new DOMParser().parseFromString(html, 'text/html');
  doc.querySelectorAll('script,iframe,frame,frameset,object,embed,applet,form,input,button,textarea,select,meta,link,base,noscript')
    .forEach(n => n.remove());
  let hasRemote = false;
  for (const el of doc.querySelectorAll('*')) {
    for (const attr of [...el.attributes]) {
      const name = attr.name.toLowerCase();
      const value = attr.value.trim();
      if (name.startsWith('on') || name === 'srcdoc' || name === 'formaction') { el.removeAttribute(attr.name); continue; }
      if (['href', 'src', 'xlink:href', 'action', 'background', 'poster'].includes(name) && /^\s*(javascript|vbscript|data:text\/html)/i.test(value)) {
        el.removeAttribute(attr.name);
        continue;
      }
      if (name === 'src' && /^cid:/i.test(value)) {
        const url = cidMap[value.slice(4)];
        if (url) el.setAttribute('src', url); else el.removeAttribute('src');
      } else if ((name === 'src' || name === 'background') && /^https?:/i.test(value)) {
        hasRemote = true;
      }
    }
    if (el.tagName === 'A') { el.setAttribute('target', '_blank'); el.setAttribute('rel', 'noopener noreferrer'); }
  }
  if (/url\(\s*['"]?https?:/i.test(doc.documentElement.innerHTML)) hasRemote = true;
  const styles = [...doc.querySelectorAll('style')].map(s => s.outerHTML).join('');
  return { html: styles + doc.body.innerHTML, hasRemote: hasRemote && !allowRemote ? true : hasRemote };
}

function mailFrame(html, allowRemote) {
  const csp = `default-src 'none'; style-src 'unsafe-inline'; font-src data:; img-src data: blob: ${allowRemote ? 'https: http:' : ''}`;
  const frame = h('iframe', {
    sandbox: 'allow-same-origin allow-popups allow-popups-to-escape-sandbox', referrerpolicy: 'no-referrer',
    title: 'Message body',
  });
  frame.srcdoc = `<!doctype html><html><head><meta charset="utf-8"><meta http-equiv="Content-Security-Policy" content="${csp}">`
    + '<base target="_blank"><style>body{margin:0;font:14px/1.5 "Segoe UI",system-ui,sans-serif;color:#1f2328;background:#fff;overflow-wrap:anywhere}'
    + 'img{max-width:100%;height:auto}table{max-width:100%}</style></head><body>' + html + '</body></html>';
  frame.addEventListener('load', () => {
    const resize = () => {
      try { frame.style.height = `${frame.contentDocument.documentElement.scrollHeight + 8}px`; } catch { /* cross-origin */ }
    };
    resize();
    try { new ResizeObserver(resize).observe(frame.contentDocument.body); } catch { /* unsupported */ }
  });
  return frame;
}

function linkify(text) {
  const frag = document.createDocumentFragment();
  const re = /\bhttps?:\/\/[^\s<>"]+/g;
  let last = 0;
  for (const m of text.matchAll(re)) {
    frag.append(text.slice(last, m.index), h('a', { href: m[0], target: '_blank', rel: 'noopener noreferrer' }, m[0]));
    last = m.index + m[0].length;
  }
  frag.append(text.slice(last));
  return frag;
}

// ---------- compose ----------
function bufferToBase64(buf) {
  const bytes = buf instanceof Uint8Array ? buf : new Uint8Array(buf);
  let bin = '';
  for (let i = 0; i < bytes.length; i += 0x8000) bin += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
  return btoa(bin);
}

function splitAddresses(value) {
  return value.split(/[,;]/).map(s => s.trim()).filter(Boolean).map(s => {
    const m = s.match(/<([^>]+)>/);
    return (m ? m[1] : s).trim();
  });
}

function quoteBlock(email) {
  const header = h('p', {}, `On ${email.date ? new Date(email.date).toLocaleString() : ''}, ${addrText(email.from ? [email.from] : [])} wrote:`);
  const quote = h('blockquote', {});
  if (email.html) {
    const { html } = sanitizeHtml(email.html, {}, false);
    const tmp = new DOMParser().parseFromString(html, 'text/html');
    quote.append(...tmp.body.childNodes);
  } else {
    quote.append(h('pre', { style: 'white-space:pre-wrap;font:inherit' }, email.text || ''));
  }
  return [h('p', {}, h('br')), h('p', {}, h('br')), header, quote];
}

function openCompose(mode, ctx = {}) {
  const src = ctx.email;
  const attachments = [];
  const fields = { to: '', cc: '', bcc: '', subject: '' };
  const reSubject = (p, s) => (new RegExp(`^${p}:`, 'i').test(s || '') ? s : `${p}: ${s || ''}`);
  const self = (state.email || '').toLowerCase();

  if (mode === 'reply' || mode === 'replyAll') {
    const target = (src.replyTo && src.replyTo.length ? src.replyTo : [src.from]).filter(Boolean);
    fields.to = target.map(a => a.address).join(', ');
    if (mode === 'replyAll') {
      const others = [...(src.to || []), ...(src.cc || [])].map(a => a.address)
        .filter(a => a && a.toLowerCase() !== self && !fields.to.includes(a));
      fields.cc = others.join(', ');
    }
    fields.subject = reSubject('Re', src.subject);
  } else if (mode === 'forward') {
    fields.subject = reSubject('Fwd', src.subject);
    for (const a of src.attachments) {
      if (a.disposition === 'inline' && a.contentId) continue;
      attachments.push({ filename: a.filename || 'attachment', contentType: a.mimeType, size: a.content.byteLength,
                         content: bufferToBase64(a.content) });
    }
  } else if (mode === 'draft') {
    fields.to = addrText(src.to);
    fields.cc = addrText(src.cc);
    fields.subject = src.subject || '';
    for (const a of src.attachments) {
      attachments.push({ filename: a.filename || 'attachment', contentType: a.mimeType, size: a.content.byteLength,
                         content: bufferToBase64(a.content) });
    }
  }

  const input = (name, label) => h('div', { class: 'compose-row' }, h('label', { for: `c-${name}` }, label),
    h('input', { id: `c-${name}`, value: fields[name], autocomplete: 'off', spellcheck: name === 'subject' ? 'true' : 'false' }));
  const rows = { to: input('to', 'To'), cc: input('cc', 'Cc'), bcc: input('bcc', 'Bcc'), subject: input('subject', 'Subject') };
  if (!fields.cc && mode !== 'replyAll') rows.cc.classList.add('hidden');
  rows.bcc.classList.add('hidden');

  const editor = h('div', { class: 'editor', contenteditable: 'true', role: 'textbox', 'aria-multiline': 'true', 'aria-label': 'Message' });
  if (mode === 'draft') {
    const { html } = src.html ? sanitizeHtml(src.html, {}, false) : { html: '' };
    if (html) editor.append(...new DOMParser().parseFromString(html, 'text/html').body.childNodes);
    else editor.append(h('pre', { style: 'white-space:pre-wrap;font:inherit' }, src.text || ''));
  } else if (src) {
    editor.append(...quoteBlock(src));
  }

  const attBox = h('div', { class: 'compose-atts' });
  const renderAtts = () => attBox.replaceChildren(...attachments.map((a, i) => h('span', { class: 'att' }, icon('clip'),
    h('span', {}, a.filename), h('small', {}, formatSize(a.size)),
    h('button', { 'aria-label': `Remove ${a.filename}`, onclick: () => { attachments.splice(i, 1); renderAtts(); } }, '×'))));
  renderAtts();

  const fileInput = h('input', { type: 'file', multiple: true, class: 'hidden', onchange: async (e) => {
    for (const f of e.target.files) {
      attachments.push({ filename: f.name, contentType: f.type || 'application/octet-stream', size: f.size,
                         content: bufferToBase64(await f.arrayBuffer()) });
    }
    e.target.value = '';
    renderAtts();
  } });

  const fmt = (cmd, label, arg) => h('button', { class: 'btn btn-ghost', type: 'button', title: label,
    onmousedown: (e) => e.preventDefault(), onclick: () => document.execCommand(cmd, false, arg) }, label);

  let draftUid = mode === 'draft' ? ctx.uid : 0;
  const payload = () => ({
    to: splitAddresses(rows.to.querySelector('input').value),
    cc: splitAddresses(rows.cc.querySelector('input').value),
    bcc: splitAddresses(rows.bcc.querySelector('input').value),
    subject: rows.subject.querySelector('input').value,
    html: editor.innerHTML, text: editor.innerText,
    attachments: attachments.map(({ filename, contentType, content }) => ({ filename, contentType, content })),
    draftUid,
    inReplyTo: mode.startsWith('reply') ? src.messageId || '' : '',
    references: mode.startsWith('reply') ? [src.references, src.messageId].filter(Boolean).join(' ') : '',
    replyTo: mode.startsWith('reply') ? { folder: ctx.folder || state.folder, uid: ctx.uid } : undefined,
  });
  const close = () => backdrop.remove();
  const sendBtn = h('button', { class: 'btn btn-primary', onclick: async () => {
    const p = payload();
    if (!p.to.length && !p.cc.length && !p.bcc.length) { toast('Add at least one recipient'); return; }
    if (attachments.reduce((n, a) => n + a.size, 0) > MAX_SEND_BYTES) { toast('Attachments are larger than 25 MB'); return; }
    sendBtn.disabled = true;
    try {
      const r = await api('POST', '/api/mail/send', p);
      close();
      toast(r.failed && r.failed.length ? `Sent, but not delivered to: ${r.failed.join(', ')}` : 'Message sent');
      refreshAll(true).then(() => loadMessages()).catch(() => {});
    } catch (e) { toast(e.message); sendBtn.disabled = false; }
  } }, icon('send'), 'Send');
  const saveDraft = async () => {
    try {
      const r = await api('POST', '/api/mail/draft', payload());
      draftUid = r.uid;
      toast('Draft saved');
      refreshAll(true).catch(() => {});
    } catch (e) { toast(e.message); }
  };

  const backdrop = h('div', { class: 'compose-backdrop', onkeydown: (e) => { if (e.key === 'Escape') close(); } },
    h('div', { class: 'compose', role: 'dialog', 'aria-modal': 'true', 'aria-label': 'New message' },
      h('div', { class: 'compose-head' }, h('h2', {}, { new: 'New message', reply: 'Reply', replyAll: 'Reply all', forward: 'Forward', draft: 'Draft' }[mode]),
        h('button', { class: 'btn btn-ghost', onclick: () => { rows.cc.classList.remove('hidden'); rows.bcc.classList.remove('hidden'); } }, 'Cc/Bcc'),
        h('button', { class: 'btn btn-ghost', 'aria-label': 'Close', onclick: close }, icon('close'))),
      rows.to, rows.cc, rows.bcc, rows.subject,
      h('div', { class: 'fmt' }, fmt('bold', 'B'), fmt('italic', 'I'), fmt('underline', 'U'),
        fmt('insertUnorderedList', '• List'), fmt('insertOrderedList', '1. List'),
        h('button', { class: 'btn btn-ghost', type: 'button', onmousedown: (e) => e.preventDefault(), onclick: () => {
          const url = prompt('Link address (https://…)');
          if (url && /^https?:\/\//i.test(url)) document.execCommand('createLink', false, url);
        } }, 'Link')),
      editor, attBox,
      h('div', { class: 'compose-foot' }, sendBtn,
        h('button', { class: 'btn', onclick: () => fileInput.click() }, icon('clip'), 'Attach'), fileInput,
        h('button', { class: 'btn', onclick: saveDraft }, 'Save draft'),
        h('span', { class: 'spacer' }),
        h('button', { class: 'btn btn-ghost', onclick: () => { if (confirm('Discard this message?')) close(); } }, icon('trash'), 'Discard'))));
  document.body.append(backdrop);
  (mode === 'new' || mode === 'forward' ? rows.to.querySelector('input') : editor).focus();
}

// ---------- popover ----------
function popover(anchor, items) {
  document.querySelectorAll('.popover').forEach(p => p.remove());
  const r = anchor.getBoundingClientRect();
  const pop = h('div', { class: 'popover', role: 'menu' }, items.map(it => (it.sep ? h('div', { class: 'sep' })
    : h('button', { role: 'menuitem', disabled: it.disabled, onclick: () => { pop.remove(); it.action && it.action(); } },
        it.icon ? icon(it.icon) : null, it.label))));
  document.body.append(pop);
  const left = Math.min(r.left, window.innerWidth - pop.offsetWidth - 8);
  pop.style.left = `${Math.max(8, left)}px`;
  pop.style.top = `${Math.min(r.bottom + 4, window.innerHeight - pop.offsetHeight - 8)}px`;
  setTimeout(() => document.addEventListener('click', function off(e) {
    if (!pop.contains(e.target)) { pop.remove(); document.removeEventListener('click', off); }
  }), 0);
}

// ---------- boot ----------
(async function boot() {
  state.token = store.get('token');
  if (!state.token) { renderLogin(); return; }
  try {
    state.email = (await api('GET', '/api/mail/me')).email;
    startApp();
  } catch { renderLogin(); }
})();

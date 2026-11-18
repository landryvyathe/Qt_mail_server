const list = document.getElementById('list');

function initials(email) {
  if (!email) return '?';
  const local = email.split('@')[0].split(/[._-]+/).filter(Boolean);
  return ((local[0] || '?')[0] + (local[1] ? local[1][0] : '')).toUpperCase();
}

function render({ activeId, accounts }) {
  list.replaceChildren(...accounts.map((a) => {
    const btn = document.createElement('button');
    btn.className = 'acct' + (a.id === activeId ? ' active' : '');
    const label = a.email || 'New account (sign in)';
    btn.title = `${label}\nRight-click to remove`;
    btn.setAttribute('aria-label', a.unread ? `${label}, ${a.unread} unread` : label);
    btn.textContent = initials(a.email);
    if (a.unread) {
      const badge = document.createElement('span');
      badge.className = 'badge';
      badge.textContent = a.unread > 99 ? '99+' : String(a.unread);
      btn.append(badge);
    }
    btn.addEventListener('click', () => window.bykiva.select(a.id));
    btn.addEventListener('contextmenu', (e) => { e.preventDefault(); window.bykiva.remove(a.id); });
    return btn;
  }));
}

document.getElementById('add').addEventListener('click', () => window.bykiva.add());
window.bykiva.onUpdate(render);

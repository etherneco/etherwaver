import { h, clear, badge, fmtAgo, fmtTime, busy, toast, errorText, banner } from '../ui.js';
import { pageHead, loading, errorBlock } from './common.js';

function statusBadge(d) {
  if (d.rejected) return badge('refused', 'danger');
  if (d.connected === null) return badge('unknown', 'warn');
  if (d.connected) return badge(d.isServer ? 'server' : 'connected', 'ok');
  return badge('disconnected');
}

async function activate(ctx, btn, name) {
  return busy(btn, async () => {
    try {
      const r = await ctx.api.post(`devices/${encodeURIComponent(name)}/activate`, {});
      toast(`Control switched to ${r.active.screen || name}`);
    } catch (e) {
      toast(errorText(e), e.isUncertain ? 'warn' : 'danger', 8000);
    }
    ctx.refreshStatus();
  });
}

async function addToLayout(ctx, btn, name) {
  return busy(btn, async () => {
    try {
      const cur = await ctx.api.get('config/layout');
      await ctx.api.post('config/layout/screens', { name }, { etag: cur.etag });
      toast(`${name} added to the layout. Apply the configuration so the server accepts it.`);
    } catch (e) {
      toast(errorText(e), 'danger', 8000);
    }
  });
}

export function render(ctx, root, params) {
  if (params && params[0]) return renderDetail(ctx, root, params[0]);
  const search = h('input', { type: 'search', placeholder: 'Search by name or alias', 'aria-label': 'Search devices' });
  const filter = h('select', { 'aria-label': 'Filter by state' },
    h('option', { value: '' }, 'All devices'), h('option', { value: 'connected' }, 'Connected'),
    h('option', { value: 'disconnected' }, 'Disconnected'), h('option', { value: 'unconfigured' }, 'Not in layout'));
  const host = h('div');
  const count = h('span', { class: 'muted small', 'aria-live': 'polite' });
  clear(root, pageHead('Devices', 'Computers in the layout and computers that tried to connect'),
    h('div', { class: 'toolbar' }, search, filter, count), host);
  let data = null;
  let timer = null;

  async function load() {
    if (!data) clear(host, loading());
    try { data = await ctx.api.get('devices'); draw(); }
    catch (e) { clear(host, errorBlock(e, load)); }
  }

  function draw() {
    const q = search.value.trim().toLowerCase();
    const f = filter.value;
    let list = data.devices.filter(d => !q || d.name.toLowerCase().includes(q) || d.aliases.some(a => a.toLowerCase().includes(q)));
    if (f === 'connected') list = list.filter(d => d.connected);
    if (f === 'disconnected') list = list.filter(d => d.connected === false);
    if (f === 'unconfigured') list = list.filter(d => !d.configured);
    count.textContent = `${list.length} of ${data.devices.length}`;
    const notice = data.stale ? banner('warn', 'Connection status unknown', 'The Etherwaver server is not connected. Only configured devices are listed; their status is not current.') : null;
    if (!list.length) {
      clear(host, notice, h('div', { class: 'card empty' }, data.devices.length ? 'No devices match the filter.' : 'No devices configured yet. Add screens in "Screens & layout".'));
      return;
    }
    clear(host, notice, h('div', { class: 'table-wrap' }, h('table', {},
      h('caption', { class: 'sr-only' }, 'Devices'),
      h('thead', {}, h('tr', {}, ['Name', 'Status', 'Controlled', 'Connected since', 'Last activity', 'Actions'].map(t => h('th', { scope: 'col' }, t)))),
      h('tbody', {}, list.map(d => h('tr', {},
        h('td', {}, h('a', { href: `#/devices/${encodeURIComponent(d.name)}` }, d.name),
          d.aliases.length ? h('div', { class: 'muted small' }, 'aliases: ', d.aliases.join(', ')) : null,
          !d.configured ? h('div', { class: 'muted small' }, 'not in the layout') : null),
        h('td', {}, statusBadge(d), d.rejected ? h('div', { class: 'small muted' }, `${d.rejected.attempts} attempt(s)`) : null),
        h('td', {}, d.active ? badge('active', 'accent') : d.active === null ? '—' : ''),
        h('td', { class: 'nowrap' }, d.connected ? fmtTime(d.connectedAt) : '—'),
        h('td', { class: 'nowrap' }, fmtAgo(d.lastActivity)),
        h('td', {}, h('div', { class: 'btn-row' },
          d.configured && !d.rejected ? h('button', { type: 'button',
            disabled: !ctx.can('control:switch') || !d.connected || d.active,
            onclick: e => activate(ctx, e.currentTarget, d.name) }, 'Switch here') : null,
          !d.configured && ctx.can('config:write') ? h('button', { type: 'button',
            onclick: e => addToLayout(ctx, e.currentTarget, d.name) }, 'Add to layout') : null))))))));
  }

  search.addEventListener('input', () => data && draw());
  filter.addEventListener('change', () => data && draw());
  const schedule = () => { clearTimeout(timer); timer = setTimeout(load, 200); };
  const offs = [ctx.live.on('server.status', schedule), ctx.live.on('config.changed', schedule), ctx.live.on('resync', schedule)];
  load();
  return () => { offs.forEach(f => f()); clearTimeout(timer); };
}

function renderDetail(ctx, root, name) {
  const host = h('div');
  clear(root, h('p', {}, h('a', { href: '#/devices' }, '← Devices')), host);
  async function load() {
    clear(host, loading());
    try {
      const r = await ctx.api.get(`devices/${encodeURIComponent(name)}`);
      const d = r.device, s = r.screen;
      clear(host, pageHead(d.name, d.isServer ? 'This computer (server)' : 'Client computer',
        d.configured ? h('button', { type: 'button', disabled: !ctx.can('control:switch') || !d.connected || d.active,
          onclick: e => activate(ctx, e.currentTarget, d.name) }, 'Switch control here') : null,
        d.configured ? h('a', { class: 'btn', href: '#/layout' }, 'Edit in layout') : null),
      h('div', { class: 'grid-2' },
        h('section', { class: 'card' }, h('h2', {}, 'Status'), h('dl', { class: 'kv' },
          h('dt', {}, 'State'), h('dd', {}, statusBadge(d)),
          h('dt', {}, 'Controlled'), h('dd', {}, d.active === null ? 'unknown' : d.active ? 'yes' : 'no'),
          h('dt', {}, 'Connected since'), h('dd', {}, d.connected ? fmtTime(d.connectedAt) : '—'),
          h('dt', {}, 'Last activity'), h('dd', {}, fmtTime(d.lastActivity)),
          h('dt', {}, 'Last disconnect'), h('dd', {}, fmtTime(d.lastDisconnect)),
          h('dt', {}, 'Screen size'), h('dd', {}, d.shape ? `${d.shape.width} × ${d.shape.height}` : '—'))),
        s ? h('section', { class: 'card' }, h('h2', {}, 'Configuration'), h('dl', { class: 'kv' },
          h('dt', {}, 'Aliases'), h('dd', {}, s.aliases.join(', ') || '—'),
          h('dt', {}, 'Neighbours'), h('dd', {}, ['left', 'right', 'up', 'down'].map(k => s.links[k] ? `${k}: ${s.links[k]}` : null).filter(Boolean).join(', ') || 'from layout position'),
          h('dt', {}, 'Modifier mapping'), h('dd', {}, Object.entries(s.modifiers).filter(([k, v]) => k !== v).map(([k, v]) => `${k}→${v}`).join(', ') || 'default'),
          h('dt', {}, 'Dead corners'), h('dd', {}, Object.entries(s.switchCorners).filter(([, v]) => v).map(([k]) => k).join(', ') || 'none'),
          h('dt', {}, 'Fixes'), h('dd', {}, Object.entries(s.fixes).filter(([, v]) => v).map(([k]) => k).join(', ') || 'none')))
          : h('section', { class: 'card' }, h('h2', {}, 'Configuration'), h('p', {}, 'This computer is not in the layout, so the server refuses it.'),
            ctx.can('config:write') ? h('button', { type: 'button', onclick: e => addToLayout(ctx, e.currentTarget, d.name) }, 'Add to layout') : null)));
    } catch (e) {
      clear(host, errorBlock(e, load));
    }
  }
  const off = ctx.live.on('server.status', load);
  load();
  return off;
}

import { h, clear, badge, fmtDuration, fmtAgo, fmtTime, banner, busy, toast, errorText } from '../ui.js';
import { pageHead, runJob } from './common.js';

export function render(ctx, root) {
  const body = h('div');
  const events = [];
  const eventsList = h('ol', { class: 'small', style: 'padding-left:1.2rem;margin:0', 'aria-live': 'polite' });
  const applyBtn = h('button', { type: 'button', disabled: !ctx.can('config:apply'),
    onclick: e => runJob(ctx, e.currentTarget, 'config/apply', 'Apply configuration') }, 'Apply configuration');
  clear(root, pageHead('Dashboard', 'Current state of this Etherwaver installation', applyBtn), body);

  async function draw() {
    let st = ctx.status();
    let fp = null;
    try { fp = await ctx.api.get('security/fingerprints'); } catch { /* optional */ }
    st = ctx.status();
    const server = st && st.server;
    const stale = !server;
    const last = st && st.lastKnownServer;
    const s = server || last;
    const clients = s ? s.clients.filter(c => !c.primary) : [];
    const service = st ? st.service : null;

    const cards = h('div', { class: 'grid' },
      h('section', { class: `card ${stale ? 'stale' : ''}`, 'aria-labelledby': 'c-server' },
        h('h2', { id: 'c-server' }, 'Server'),
        s ? h('dl', { class: 'kv' },
          h('dt', {}, 'Screen'), h('dd', {}, s.server),
          h('dt', {}, 'Version'), h('dd', {}, s.version),
          h('dt', {}, 'Listening'), h('dd', {}, s.listenAddress || 'all interfaces'),
          h('dt', {}, 'IP addresses'), h('dd', {}, (ctx.meta.platform.addresses || []).join(', ') || 'unknown'),
          h('dt', {}, 'Uptime'), h('dd', {}, stale ? 'unknown (not connected)' : fmtDuration(s.uptimeSeconds)),
          h('dt', {}, 'State'), h('dd', {}, stale ? badge('not reachable', 'danger') : badge('running', 'ok')))
          : h('p', { class: 'muted' }, 'The Etherwaver server is not running or not reachable from the web panel.')),
      h('section', { class: `card ${stale ? 'stale' : ''}`, 'aria-labelledby': 'c-active' },
        h('h2', { id: 'c-active' }, 'Controlled screen'),
        s ? [h('div', { class: 'stat' }, s.active.screen || s.active.client || '—'),
          h('p', { class: 'muted' }, s.lockedToScreen ? 'Cursor is locked to this screen' : 'Cursor moves freely between screens'),
          stale ? badge('last known', 'warn') : null]
          : h('p', { class: 'muted' }, 'Unknown')),
      h('section', { class: `card ${stale ? 'stale' : ''}`, 'aria-labelledby': 'c-clients' },
        h('h2', { id: 'c-clients' }, 'Connected clients'),
        h('div', { class: 'stat' }, stale ? '—' : String(clients.length)),
        h('p', { class: 'muted' }, clients.map(c => c.name).join(', ') || 'No clients connected'),
        h('a', { href: '#/devices' }, 'View devices')),
      h('section', { class: 'card', 'aria-labelledby': 'c-service' },
        h('h2', { id: 'c-service' }, 'Service'),
        service && service.enabled
          ? h('dl', { class: 'kv' },
            h('dt', {}, 'Managed by'), h('dd', {}, 'web panel'),
            h('dt', {}, 'Process'), h('dd', {}, service.running ? badge(`running (pid ${service.pid})`, 'ok') : badge('stopped')),
            h('dt', {}, 'Mode'), h('dd', {}, service.mode || '—'))
          : h('p', { class: 'muted' }, server ? `Running as pid ${server.pid}, started by the desktop panel or the system service manager.`
            : 'Not managed by the web panel. Start Etherwaver from the desktop panel or the system service.'),
        h('a', { href: '#/settings' }, 'Service settings')));

    const alerts = [];
    if (fp && fp.pending && fp.pending.length) {
      alerts.push(banner('warn', `${fp.pending.length} unknown certificate fingerprint(s)`,
        'A computer with an untrusted certificate tried to connect.', h('a', { class: 'btn', href: '#/security' }, 'Review')));
    }
    const rejected = [];
    if (alerts.length === 0 && st && st.web) { /* nothing */ }

    const quick = h('section', { class: 'card', 'aria-labelledby': 'c-quick' },
      h('h2', { id: 'c-quick' }, 'Switch control'),
      h('p', { class: 'muted small' }, 'Moves the shared keyboard and mouse to another screen. Only one screen is controlled at a time.'),
      s && !stale ? h('div', { class: 'btn-row' },
        s.clients.map(c => h('button', {
          type: 'button', disabled: !ctx.can('control:switch') || c.active, 'aria-pressed': String(!!c.active),
          onclick: e => busy(e.currentTarget, async () => {
            try { await ctx.api.post(`devices/${encodeURIComponent(c.name)}/activate`, {}); toast(`Control switched to ${c.name}`); }
            catch (err) { toast(errorText(err), err.isUncertain ? 'warn' : 'danger', 8000); }
            ctx.refreshStatus();
          }) }, c.primary ? `${c.name} (server)` : c.name)),
        h('button', { type: 'button', disabled: !ctx.can('control:lock'),
          onclick: e => busy(e.currentTarget, async () => {
            try { const r = await ctx.api.post('control/lock', { mode: s.lockedToScreen ? 'off' : 'on' });
              toast(r.locked ? 'Cursor locked to the current screen' : 'Cursor unlocked'); }
            catch (err) { toast(errorText(err), err.isUncertain ? 'warn' : 'danger', 8000); }
            ctx.refreshStatus();
          }) }, s.lockedToScreen ? 'Unlock cursor' : 'Lock cursor to screen'))
        : h('p', { class: 'muted' }, 'Available when the Etherwaver server is connected.'));

    renderEvents();
    clear(body, alerts, cards, h('div', { class: 'grid-2', style: 'margin-top:1rem' }, quick,
      h('section', { class: 'card', 'aria-labelledby': 'c-events' }, h('h2', { id: 'c-events' }, 'Recent events'), eventsList)));
  }

  function renderEvents() {
    clear(eventsList, events.length ? events.slice(-12).reverse().map(e => h('li', {}, h('span', { class: 'muted' }, fmtAgo(e.ts)), ' ', describe(e)))
      : h('li', { class: 'muted', style: 'list-style:none;margin-left:-1.2rem' }, 'No events since this page was opened.'));
  }

  function describe(e) {
    const d = e.data || {};
    switch (e.event) {
      case 'client.connected': return `${d.name} connected`;
      case 'client.disconnected': return `${d.name} disconnected`;
      case 'client.rejected': return `${d.name} was refused (${d.reason === 'unknown_screen' ? 'not in the layout' : 'already connected'})`;
      case 'screen.active': return `control moved to ${d.screen || d.client}`;
      case 'cursor.lock': return d.locked ? 'cursor locked to screen' : 'cursor unlocked';
      case 'config.applied': return 'configuration applied to the server';
      case 'server.stopping': return 'server is stopping';
      default: return e.event;
    }
  }

  const offs = [
    ctx.live.on('server.status', () => draw()),
    ctx.live.on('server.event', d => { events.push({ ...d, ts: Date.now() / 1000 }); renderEvents(); }),
    ctx.live.on('security.pending', () => draw()),
    ctx.live.on('service.state', () => ctx.refreshStatus()),
  ];
  draw();
  return () => offs.forEach(f => f());
}

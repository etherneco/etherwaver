import { h, clear, badge, fmtTime, fmtAgo, busy, toast, errorText, banner } from '../ui.js';
import { pageHead, loading, errorBlock } from './common.js';

export function render(ctx, root) {
  const host = h('div');
  clear(root, pageHead('Sessions', 'Who is connected and which screen holds the keyboard and mouse'), host);

  async function load() {
    try {
      const r = await ctx.api.get('sessions');
      draw(r);
    } catch (e) { clear(host, errorBlock(e, load)); }
  }

  function draw(r) {
    const kvm = r.kvm;
    const parts = [];
    parts.push(h('section', { class: 'card', style: 'margin-bottom:1rem' },
      h('h2', {}, 'Keyboard and mouse session'),
      h('p', { class: 'muted small' },
        'Etherwaver shares the physical keyboard and mouse of the server. Exactly one screen is controlled at a time; ',
        'switching moves control (the previous screen is released immediately). Locking keeps the cursor on the current screen.'),
      h('p', { class: 'limitation' }, 'Remote screen view, audio and typing from the browser are not part of Etherwaver: ',
        'there is no video or audio stream in the server or the desktop panel, so the web panel controls focus only.'),
      kvm ? [
        h('dl', { class: 'kv' },
          h('dt', {}, 'Controlled screen'), h('dd', {}, h('strong', {}, kvm.active.screen || kvm.active.client)),
          h('dt', {}, 'Cursor'), h('dd', {}, kvm.lockedToScreen ? badge('locked to screen', 'warn') : badge('free', 'ok'))),
        h('div', { class: 'btn-row', style: 'margin-top:.75rem' },
          h('button', { type: 'button', disabled: !ctx.can('control:lock'),
            onclick: e => busy(e.currentTarget, async () => {
              try { await ctx.api.post('control/lock', { mode: kvm.lockedToScreen ? 'off' : 'on' }); }
              catch (err) { toast(errorText(err), err.isUncertain ? 'warn' : 'danger', 8000); }
              await ctx.refreshStatus(); load();
            }) }, kvm.lockedToScreen ? 'Release cursor lock' : 'Lock cursor to this screen'))]
        : banner('warn', 'No live session information', 'The Etherwaver server is not connected.')));

    if (kvm) {
      parts.push(h('div', { class: 'table-wrap', style: 'margin-bottom:1rem' }, h('table', {},
        h('caption', { class: 'sr-only' }, 'Connected screens'),
        h('thead', {}, h('tr', {}, ['Screen', 'Role', 'Control', 'Connected since', ''].map(t => h('th', { scope: 'col' }, t)))),
        h('tbody', {}, kvm.clients.map(c => h('tr', {},
          h('td', {}, c.name), h('td', {}, c.primary ? 'server' : 'client'),
          h('td', {}, c.active ? badge('has control', 'accent') : '—'),
          h('td', {}, c.primary ? '—' : fmtTime(c.connectedAt)),
          h('td', {}, h('button', { type: 'button', disabled: !ctx.can('control:switch') || c.active,
            onclick: e => busy(e.currentTarget, async () => {
              try { await ctx.api.post(`devices/${encodeURIComponent(c.name)}/activate`, {}); toast(`Control switched to ${c.name}`); }
              catch (err) { toast(errorText(err), err.isUncertain ? 'warn' : 'danger', 8000); }
              await ctx.refreshStatus(); load();
            }) }, 'Give control'))))))));
    }

    if (r.web) {
      parts.push(h('h2', {}, 'Web panel sessions'),
        h('div', { class: 'table-wrap' }, h('table', {},
          h('thead', {}, h('tr', {}, ['User', 'Role', 'Address', 'Signed in', 'Last seen'].map(t => h('th', { scope: 'col' }, t)))),
          h('tbody', {}, r.web.map(w => h('tr', {}, h('td', {}, w.user, w.current ? ' (you)' : ''), h('td', {}, w.role),
            h('td', {}, w.ip), h('td', {}, fmtTime(w.created)), h('td', {}, fmtAgo(w.lastSeen))))))));
    }
    clear(host, parts);
  }

  clear(host, loading());
  const offs = [ctx.live.on('server.status', load), ctx.live.on('resync', load)];
  load();
  return () => offs.forEach(f => f());
}

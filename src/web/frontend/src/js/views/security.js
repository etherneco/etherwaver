import { h, clear, busy, toast, errorText, confirmDialog, fmtAgo, field, select, badge } from '../ui.js';
import { pageHead, loading, errorBlock } from './common.js';

export function render(ctx, root) {
  const host = h('div');
  const canWrite = ctx.can('security:write');
  clear(root, pageHead('Security', 'TLS certificate fingerprints used to pair computers'), host);

  async function load() {
    try { draw(await ctx.api.get('security/fingerprints')); }
    catch (e) { clear(host, errorBlock(e, load)); }
  }

  const trust = (kind, sha256, btn) => busy(btn, async () => {
    try { await ctx.api.post(`security/fingerprints/${kind}`, { sha256 }); toast('Fingerprint trusted.'); }
    catch (e) { toast(errorText(e), 'danger', 8000); }
    load();
  });

  function table(kind, title, entries) {
    return h('section', { class: 'card', style: 'margin-bottom:1rem' }, h('h2', {}, title),
      entries.length ? h('div', { class: 'table-wrap' }, h('table', {},
        h('thead', {}, h('tr', {}, ['Algorithm', 'Fingerprint', ''].map(t => h('th', { scope: 'col' }, t)))),
        h('tbody', {}, entries.map(e => h('tr', {}, h('td', {}, e.algorithm.toUpperCase()),
          h('td', { class: 'mono small' }, e.display),
          h('td', {}, canWrite ? h('button', { type: 'button', class: 'danger', onclick: async ev => {
            const btn = ev.currentTarget;
            if (!(await confirmDialog('Revoke trust?', 'The computer with this certificate will have to be accepted again.', 'Revoke', true))) return;
            await busy(btn, async () => {
              try { await ctx.api.del(`security/fingerprints/${kind}/${e.hex}`); toast('Trust revoked.'); }
              catch (err) { toast(errorText(err), 'danger', 8000); }
            });
            load();
          } }, 'Revoke') : null)))))) : h('p', { class: 'muted' }, 'None.'));
  }

  function draw(r) {
    const local = r.local.find(e => e.algorithm === 'sha256');
    const local1 = r.local.find(e => e.algorithm === 'sha1');
    const kindSel = select('kind', [['clients', 'Trusted clients (this computer is the server)'], ['servers', 'Trusted servers (this computer is a client)']], 'clients');
    const manual = h('input', { type: 'text', name: 'sha256', placeholder: 'AA:BB:… (SHA-256, 32 bytes)', class: 'mono' });
    clear(host,
      h('section', { class: 'card', style: 'margin-bottom:1rem' }, h('h2', {}, 'This computer'),
        local ? h('dl', { class: 'kv' }, h('dt', {}, 'SHA-256'), h('dd', { class: 'mono small' }, local.display),
          local1 ? [h('dt', {}, 'SHA-1'), h('dd', { class: 'mono small' }, local1.display)] : null)
          : h('p', { class: 'muted' }, 'No certificate yet. It is created when Etherwaver starts with encryption enabled.'),
        h('p', { class: 'limitation' }, 'Generating a new certificate is done by the desktop panel; the web panel shows and pairs fingerprints only.')),
      r.pending.length ? h('section', { class: 'card', style: 'margin-bottom:1rem', 'aria-live': 'polite' }, h('h2', {}, 'Waiting for approval'),
        h('p', { class: 'muted small' }, 'Compare the fingerprint with the one shown on the other computer before trusting it.'),
        r.pending.map(p => h('div', { class: 'banner warn' }, h('div', {}, h('strong', {}, 'Unknown certificate '), badge(p.kind),
          h('div', { class: 'mono small' }, p.display), h('div', { class: 'small muted' }, 'seen ', fmtAgo(p.seen))),
          canWrite ? h('button', { type: 'button', class: 'primary', onclick: e => trust(p.kind, p.sha256, e.currentTarget) }, 'Trust') : null)))
        : null,
      table('clients', 'Trusted clients', r.trustedClients),
      table('servers', 'Trusted servers', r.trustedServers),
      canWrite ? h('section', { class: 'card' }, h('h2', {}, 'Trust a fingerprint manually'),
        field({ label: 'List', input: kindSel }), field({ label: 'SHA-256 fingerprint', input: manual }),
        h('button', { type: 'button', onclick: e => trust(kindSel.value, manual.value, e.currentTarget) }, 'Trust fingerprint')) : null);
  }

  clear(host, loading());
  const offs = [ctx.live.on('security.pending', load), ctx.live.on('security.changed', load)];
  load();
  return () => offs.forEach(f => f());
}

import { h, clear, badge, fmtTime, fmtDuration, busy } from '../ui.js';
import { pageHead, loading, errorBlock } from './common.js';

const kv = obj => h('dl', { class: 'kv' }, Object.entries(obj).map(([k, v]) => [h('dt', {}, k), h('dd', {}, v === null || v === undefined || v === '' ? '—' : v)]));

export function render(ctx, root) {
  const host = h('div');
  const refresh = h('button', { type: 'button', onclick: e => busy(e.currentTarget, load) }, 'Refresh');
  clear(root, pageHead('Diagnostics', 'Connection between the web panel and Etherwaver, files and runtime state', refresh), host);

  async function load() {
    try { draw(await ctx.api.get('diagnostics')); }
    catch (e) { clear(host, errorBlock(e, load)); }
  }

  function draw(d) {
    const c = d.control;
    const files = Object.entries(d.files).map(([k, f]) => h('tr', {}, h('td', {}, k), h('td', { class: 'mono small' }, f.path),
      h('td', {}, f.exists ? badge(f.mode, 'ok') : badge('missing', 'warn')), h('td', {}, f.exists ? fmtTime(f.modified) : '—')));
    clear(host,
      h('div', { class: 'grid-2' },
        h('section', { class: 'card' }, h('h2', {}, 'Control connection'), kv({
          State: c.state === 'connected' ? badge('connected', 'ok') : badge(c.state, 'danger'),
          Address: c.address, 'Last error': c.lastError, 'Round trip': c.latencyMs !== null ? `${c.latencyMs.toFixed(1)} ms` : null,
          'Connected since': fmtTime(c.connectedSince), Reconnects: String(c.reconnects),
          'Pending / waiting': `${c.pending} / ${c.waiting} (max ${c.maxInflight} in flight)`,
          Server: c.server ? `${c.server} ${c.serverVersion}` : null,
          Messages: `${c.stats.sent} sent, ${c.stats.responses} responses, ${c.stats.events} events`,
          Problems: `${c.stats.timeouts} timeouts, ${c.stats.uncertain} uncertain, ${c.stats.late_responses} late, ${c.stats.protocol_errors} protocol errors`,
        })),
        h('section', { class: 'card' }, h('h2', {}, 'Web panel'), kv({
          Version: d.web.version, Python: d.web.python, Uptime: fmtDuration(d.web.uptimeSeconds),
          'Signed-in sessions': String(d.web.sessions), 'Live subscribers': String(d.web.liveSubscribers), 'Event sequence': String(d.web.eventSeq),
          'Service management': d.service.enabled ? (d.service.running ? `running, pid ${d.service.pid}` : 'enabled, stopped') : 'disabled',
        }))),
      d.server ? h('section', { class: 'card', style: 'margin-top:1rem' }, h('h2', {}, 'Server runtime layout'),
        h('p', { class: 'muted small' }, `Configuration file ${d.server.configFile || '—'}, layout file ${d.server.layoutFile}`),
        h('div', { class: 'table-wrap' }, h('table', {},
          h('thead', {}, h('tr', {}, ['Screen', 'Host', 'Position', 'Size', 'Links'].map(t => h('th', { scope: 'col' }, t)))),
          h('tbody', {}, d.server.layout.map(s => h('tr', {}, h('td', {}, s.name || s.id), h('td', {}, s.host),
            h('td', {}, `${s.x}, ${s.y}`), h('td', {}, `${s.width} × ${s.height}`),
            h('td', { class: 'small' }, Object.entries(s.links).filter(([, v]) => v).map(([k, v]) => `${k}: ${v}`).join(', ') || '—'))))))) : null,
      h('section', { class: 'card', style: 'margin-top:1rem' }, h('h2', {}, 'Files'),
        h('div', { class: 'table-wrap' }, h('table', {}, h('thead', {}, h('tr', {}, ['', 'Path', 'Access', 'Modified'].map(t => h('th', { scope: 'col' }, t)))), h('tbody', {}, files)))),
      d.uhid.length ? h('section', { class: 'card', style: 'margin-top:1rem' }, h('h2', {}, 'UHID backend'),
        d.uhid.map(u => h('div', {}, h('div', { class: 'mono small' }, u.path), h('pre', { class: 'log', style: 'height:auto' }, u.status)))) : null,
      d.recentProcessOutput.length ? h('section', { class: 'card', style: 'margin-top:1rem' }, h('h2', {}, 'Output of the managed process'),
        h('pre', { class: 'log', style: 'height:16rem' }, d.recentProcessOutput.join('\n'))) : null,
      h('p', { class: 'limitation' }, 'The desktop panel\'s "Screen info" debug window reads monitor geometry and the cursor from the local desktop session; ',
        'the web panel shows the geometry reported by the running server instead.'));
  }
  clear(host, loading());
  load();
  const off = ctx.live.on('server.status', () => {});
  return off;
}

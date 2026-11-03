import { h, clear, badge, fmtTime } from '../ui.js';
import { pageHead, loading, errorBlock } from './common.js';

const STATE_KIND = { succeeded: 'ok', failed: 'danger', uncertain: 'warn', running: 'accent', queued: '', cancelled: 'warn' };
const RESULT_KIND = { success: 'ok', failure: 'danger', denied: 'danger', uncertain: 'warn' };

export function render(ctx, root) {
  const jobsHost = h('div');
  const auditHost = h('div');
  const userFilter = h('input', { type: 'search', placeholder: 'User', 'aria-label': 'Filter by user' });
  const actionFilter = h('input', { type: 'search', placeholder: 'Action prefix, e.g. config.', 'aria-label': 'Filter by action' });
  clear(root, pageHead('Activity & jobs', 'Background operations and the audit trail of administrative actions'),
    h('h2', {}, 'Jobs'), jobsHost,
    ctx.can('audit:read') ? [h('h2', { style: 'margin-top:1.5rem' }, 'Audit log'),
      h('div', { class: 'toolbar' }, userFilter, actionFilter), auditHost] : null);

  let jobs = [];
  async function loadJobs() {
    try { jobs = (await ctx.api.get('jobs')).jobs; drawJobs(); }
    catch (e) { clear(jobsHost, errorBlock(e, loadJobs)); }
  }
  function drawJobs() {
    clear(jobsHost, jobs.length ? h('div', { class: 'table-wrap' }, h('table', {},
      h('thead', {}, h('tr', {}, ['Started', 'Operation', 'User', 'State', 'Progress', 'Message'].map(t => h('th', { scope: 'col' }, t)))),
      h('tbody', {}, jobs.map(j => h('tr', {}, h('td', { class: 'nowrap' }, fmtTime(j.created)), h('td', {}, j.kind), h('td', {}, j.user),
        h('td', {}, badge(j.state, STATE_KIND[j.state] || '')),
        h('td', {}, h('div', { class: 'progress', role: 'progressbar', 'aria-valuenow': j.progress, 'aria-valuemin': 0, 'aria-valuemax': 100 },
          h('span', { style: `width:${j.progress}%` }))),
        h('td', { class: 'small' }, j.error ? j.error.message : j.message))))))
      : h('div', { class: 'card empty' }, 'No background jobs since the web panel started.'));
  }

  let timer;
  async function loadAudit() {
    if (!ctx.can('audit:read')) return;
    const q = new URLSearchParams({ limit: '300' });
    if (userFilter.value.trim()) q.set('user', userFilter.value.trim());
    if (actionFilter.value.trim()) q.set('action', actionFilter.value.trim());
    try {
      const r = await ctx.api.get('audit?' + q);
      clear(auditHost, r.entries.length ? h('div', { class: 'table-wrap' }, h('table', {},
        h('thead', {}, h('tr', {}, ['Time (UTC)', 'User', 'Action', 'Resource', 'Result', 'Address', 'Request'].map(t => h('th', { scope: 'col' }, t)))),
        h('tbody', {}, r.entries.map(e => h('tr', {}, h('td', { class: 'nowrap small' }, e.ts), h('td', {}, e.user || '—'),
          h('td', { class: 'mono small' }, e.action), h('td', { class: 'small' }, e.resource),
          h('td', {}, badge(e.result, RESULT_KIND[e.result] || '')), h('td', { class: 'small' }, e.ip),
          h('td', { class: 'mono small' }, e.requestId))))))
        : h('div', { class: 'card empty' }, 'No entries.'));
    } catch (e) { clear(auditHost, errorBlock(e, loadAudit)); }
  }
  const debounced = () => { clearTimeout(timer); timer = setTimeout(loadAudit, 250); };
  userFilter.addEventListener('input', debounced);
  actionFilter.addEventListener('input', debounced);

  clear(jobsHost, loading());
  clear(auditHost, loading());
  const offs = [ctx.live.on('job.updated', j => {
    const i = jobs.findIndex(x => x.id === j.id);
    if (i >= 0) jobs[i] = j; else jobs.unshift(j);
    drawJobs();
    if (['succeeded', 'failed', 'uncertain'].includes(j.state)) debounced();
  }), ctx.live.on('config.changed', debounced)];
  loadJobs(); loadAudit();
  return () => { offs.forEach(f => f()); clearTimeout(timer); };
}

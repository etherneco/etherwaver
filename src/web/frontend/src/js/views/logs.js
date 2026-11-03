import { h, clear, select } from '../ui.js';
import { pageHead, errorBlock } from './common.js';

const LEVELS = ['FATAL', 'ERROR', 'WARNING', 'NOTE', 'INFO', 'DEBUG', 'DEBUG1', 'DEBUG2'];

export function render(ctx, root) {
  const levelSel = select('level', [['', 'All levels'], ...LEVELS.map(l => [l, `${l} and more severe`])], '');
  levelSel.setAttribute('aria-label', 'Minimum level');
  const search = h('input', { type: 'search', placeholder: 'Filter text', 'aria-label': 'Filter log text' });
  let paused = false;
  const pauseBtn = h('button', { type: 'button', 'aria-pressed': 'false', onclick: () => {
    paused = !paused; pauseBtn.setAttribute('aria-pressed', String(paused)); pauseBtn.textContent = paused ? 'Resume' : 'Pause';
    if (!paused) draw();
  } }, 'Pause');
  const download = h('button', { type: 'button', onclick: () => {
    const text = entries.map(e => `${new Date(e.ts * 1000).toISOString()} [${e.source}] ${e.message}`).join('\n');
    const a = h('a', { href: URL.createObjectURL(new Blob([text], { type: 'text/plain' })), download: 'etherwaver-web-log.txt' });
    document.body.append(a); a.click(); a.remove();
  } }, 'Download');
  const box = h('div', { class: 'log', tabindex: '0', role: 'log', 'aria-label': 'Server log', 'aria-live': 'off' });
  const host = h('div');
  clear(root, pageHead('Logs', 'Live log of the Etherwaver server and of processes started by the web panel'),
    h('div', { class: 'toolbar' }, levelSel, search, pauseBtn, download), host, box,
    h('p', { class: 'hint' }, 'The desktop panel keeps its own log window; enable "Write the log to a file" in Settings for a persistent log.'));
  let entries = [];

  function visible(e) {
    const lv = levelSel.value;
    if (lv && LEVELS.indexOf(e.level) > LEVELS.indexOf(lv)) return false;
    const q = search.value.trim().toLowerCase();
    return !q || e.message.toLowerCase().includes(q);
  }
  function line(e) { return h('div', { class: e.level }, `[${e.source}] ${e.message}`); }
  function draw() {
    const atBottom = box.scrollHeight - box.scrollTop - box.clientHeight < 40;
    clear(box, entries.filter(visible).slice(-1500).map(line));
    if (!box.childElementCount) box.append(h('div', { class: 'muted' }, 'No log lines yet.'));
    if (atBottom || !box.dataset.scrolled) { box.scrollTop = box.scrollHeight; box.dataset.scrolled = '1'; }
  }
  async function load() {
    try { entries = (await ctx.api.get('logs?limit=2000')).entries; clear(host); draw(); }
    catch (e) { clear(host, errorBlock(e, load)); }
  }
  levelSel.addEventListener('change', draw);
  search.addEventListener('input', draw);
  const offs = [
    ctx.live.on('log', e => { entries.push(e); if (entries.length > 3000) entries = entries.slice(-2000); if (!paused) draw(); }),
    ctx.live.on('resync', load),
  ];
  load();
  return () => offs.forEach(f => f());
}

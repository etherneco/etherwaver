import { h, clear, field, checkbox, select, banner, uid, confirmDialog } from '../ui.js';
import { pageHead, ConfigEditor, runJob } from './common.js';

const DIRS = ['left', 'right', 'up', 'down'];
const MODS = ['shift', 'ctrl', 'alt', 'meta', 'super'];
const CORNER_LABELS = { topLeft: 'Top left', topRight: 'Top right', bottomLeft: 'Bottom left', bottomRight: 'Bottom right' };
const FIX_LABELS = {
  halfDuplexCapsLock: 'Half-duplex Caps Lock', halfDuplexNumLock: 'Half-duplex Num Lock',
  halfDuplexScrollLock: 'Half-duplex Scroll Lock', xtestIsXineramaUnaware: "XTest is Xinerama unaware",
  preserveFocus: 'Preserve focus (Windows clients)',
};

function newScreen(name, x, y) {
  return { name, aliases: [], x, y, width: 240, height: 140, links: { left: '', right: '', up: '', down: '' },
    modifiers: { shift: 'shift', ctrl: 'ctrl', alt: 'alt', meta: 'meta', super: 'super' },
    switchCorners: { topLeft: false, topRight: false, bottomLeft: false, bottomRight: false },
    switchCornerSize: 0, fixes: Object.fromEntries(Object.keys(FIX_LABELS).map(k => [k, false])) };
}

export function render(ctx, root) {
  const host = h('div');
  const canWrite = ctx.can('config:write');
  const applyBtn = h('button', { type: 'button', disabled: !ctx.can('config:apply'),
    onclick: e => {
      if (editor.dirty) { ctx.toast('Save your changes first; only saved configuration can be applied.', 'warn'); return; }
      runJob(ctx, e.currentTarget, 'config/apply', 'Apply configuration');
    } }, 'Apply to running server');
  clear(root, pageHead('Screens & layout', 'Arrange computers like their monitors stand on your desk',
    h('a', { class: 'btn', href: 'api/v1/config/export?format=conf', download: 'etherwaver.conf' }, 'Download server .conf'),
    applyBtn), host);

  let draft = null;
  let serverScreen = '';
  let selected = null;

  const editor = new ConfigEditor(ctx, {
    resource: 'layout', root: host, readOnly: !canWrite,
    load: async () => {
      const r = await ctx.api.get('config/layout');
      serverScreen = r.serverScreen;
      editor.external = r.configSource === 'external';
      return { data: r.layout, etag: r.etag };
    },
    save: async (data, etag) => {
      const r = await ctx.api.put('config/layout', data, { etag });
      return { data: r.layout, etag: r.etag };
    },
    collect: () => draft,
    render: data => {
      draft = structuredClone(data);
      if (!draft.screens.find(s => s.name === selected)) selected = draft.screens[0] ? draft.screens[0].name : null;
      return build();
    },
  });

  const canvasInner = h('div', { class: 'canvas-inner' });
  const canvas = h('div', { class: 'canvas', role: 'application', 'aria-label': 'Screen layout. Select a screen, then use the arrow keys to move it (Shift for larger steps).' }, canvasInner);
  const detail = h('div');
  const SCALE = 0.5;

  function build() {
    const opts = draft.options;
    const external = editor.external ? banner('info', 'External configuration file in use',
      'The desktop panel is set to use an external configuration file, so the server uses that file instead of this layout. ' +
      'Switch "Configuration source" to internal in Settings to use the layout below.') : null;
    const toolbar = h('div', { class: 'toolbar' },
      h('button', { type: 'button', disabled: !canWrite, onclick: addScreen }, 'Add screen'),
      h('button', { type: 'button', disabled: !canWrite, onclick: autoLayout }, 'Auto layout'),
      h('span', { class: 'muted small' }, `${draft.screens.length} screen(s). The server screen is "${serverScreen}".`));
    drawCanvas();
    drawDetail();
    const options = h('fieldset', {}, h('legend', {}, 'Server options'),
      h('div', { class: 'grid-2' },
        h('div', {},
          numberToggle('Heartbeat', 'heartbeat', opts.heartbeat, 1000, 3600000, 'ms', 'Detect lost clients by sending heartbeats.'),
          numberToggle('Switch delay', 'switchDelay', opts.switchDelay, 10, 10000, 'ms', 'Wait at the screen edge before switching.'),
          numberToggle('Switch on double tap', 'switchDoubleTap', opts.switchDoubleTap, 10, 10000, 'ms', 'Tap the edge twice within this time to switch.'),
          h('div', { class: 'field' }, h('span', { style: 'font-weight:600' }, 'Dead corners (no switching)'),
            h('div', { class: 'inline-fields' }, Object.entries(CORNER_LABELS).map(([k, l]) =>
              bindCheck(checkbox({ label: l, name: 'opt.corner.' + k, checked: opts.switchCorners[k] }), v => { opts.switchCorners[k] = v; })))),
          bindNumber(field({ label: 'Corner size (px)', input: h('input', { type: 'number', name: 'options.switchCornerSize', min: 0, max: 1000, value: opts.switchCornerSize }) }),
            v => { opts.switchCornerSize = v; })),
        h('div', {},
          bindCheck(checkbox({ label: 'Use relative mouse moves', name: 'opt.rel', checked: opts.relativeMouseMoves, hint: 'Useful for games on the server screen.' }), v => { opts.relativeMouseMoves = v; }),
          bindCheck(checkbox({ label: 'Synchronize screen savers', name: 'opt.ss', checked: opts.screenSaverSync }), v => { opts.screenSaverSync = v; }),
          bindCheck(checkbox({ label: 'Keep the Windows server in the foreground', name: 'opt.w32', checked: opts.win32KeepForeground,
            limitation: 'Has an effect only when the server runs on Windows.' }), v => { opts.win32KeepForeground = v; }),
          bindCheck(checkbox({ label: 'Share clipboard', name: 'opt.clip', checked: opts.clipboardSharing }), v => { opts.clipboardSharing = v; }),
          bindCheck(checkbox({ label: 'Enable file drag and drop', name: 'opt.dnd', checked: opts.enableDragAndDrop,
            limitation: 'Not supported when the server runs on Linux.' }), v => { opts.enableDragAndDrop = v; }),
          bindCheck(checkbox({ label: 'Do not ask about new auto-configured clients', name: 'opt.ignore', checked: opts.ignoreAutoConfigClient,
            limitation: 'Auto-configuration (Bonjour) prompts appear only in the desktop panel; in the web panel refused clients are listed under Devices.' }),
            v => { opts.ignoreAutoConfigClient = v; }),
          bindCheck(checkbox({ label: 'Use custom neighbour links', name: 'opt.links', checked: opts.useCustomLinks,
            hint: 'Off: neighbours follow the layout order. On: set left/right/up/down for each screen.' }),
            v => { opts.useCustomLinks = v; drawDetail(); }))));
    return h('div', {}, external, toolbar,
      h('div', { class: 'split' }, h('div', {}, canvas, h('p', { class: 'hint' }, 'Drag a screen with the mouse or select it and use the arrow keys.')), detail),
      options);
  }

  function numberToggle(label, key, value, min, max, unit, hint) {
    const id = uid('n');
    const cb = h('input', { type: 'checkbox', id: id + '-on', name: `options.${key}.enabled`, checked: value.enabled,
      onchange: e => { value.enabled = e.target.checked; num.disabled = !e.target.checked; } });
    const num = h('input', { type: 'number', id, name: `options.${key}.ms`, min, max, value: value.ms, disabled: !value.enabled,
      'aria-describedby': id + '-err', oninput: e => { value.ms = parseInt(e.target.value, 10); } });
    return h('div', { class: 'field' }, h('label', { class: 'check', for: id + '-on' }, cb, label),
      h('div', { class: 'inline-fields' }, num, h('span', { class: 'muted' }, unit)),
      h('div', { class: 'hint' }, hint), h('div', { class: 'field-error', id: id + '-err' }));
  }

  function bindCheck(wrapper, fn) {
    const input = wrapper.querySelector('input');
    input.addEventListener('change', () => fn(input.checked));
    return wrapper;
  }
  function bindNumber(wrapper, fn) {
    const input = wrapper.querySelector('input');
    input.addEventListener('input', () => fn(parseInt(input.value, 10)));
    return wrapper;
  }

  function drawCanvas() {
    const maxX = Math.max(800, ...draft.screens.map(s => (s.x + s.width) * SCALE + 40));
    const maxY = Math.max(340, ...draft.screens.map(s => (s.y + s.height) * SCALE + 40));
    canvasInner.style.width = maxX + 'px';
    canvasInner.style.height = maxY + 'px';
    const st = ctx.status();
    const active = st && st.server ? (st.server.active.screen || '').toLowerCase() : '';
    clear(canvasInner, draft.screens.map(s => {
      const card = h('button', {
        type: 'button', class: `screen-card ${s.name === selected ? 'selected' : ''} ${s.name.toLowerCase() === serverScreen.toLowerCase() ? 'server' : ''} ${active === s.name.toLowerCase() ? 'active' : ''}`,
        style: `left:${s.x * SCALE}px;top:${s.y * SCALE}px;width:${Math.max(60, s.width * SCALE)}px;height:${Math.max(40, s.height * SCALE)}px`,
        'aria-pressed': String(s.name === selected), 'aria-label': `${s.name} at ${s.x}, ${s.y}`,
        onclick: () => { selected = s.name; drawCanvas(); drawDetail(); },
        onkeydown: e => moveKey(e, s),
        onpointerdown: e => startDrag(e, s),
      }, h('span', { class: 'name' }, s.name), h('span', { class: 'muted' }, `${s.x}, ${s.y}`));
      return card;
    }));
  }

  function moveKey(e, s) {
    if (!canWrite) return;
    const step = e.shiftKey ? 100 : 10;
    const d = { ArrowLeft: [-step, 0], ArrowRight: [step, 0], ArrowUp: [0, -step], ArrowDown: [0, step] }[e.key];
    if (!d) return;
    e.preventDefault();
    s.x = Math.max(0, s.x + d[0]); s.y = Math.max(0, s.y + d[1]);
    selected = s.name;
    editor.markDirty();
    drawCanvas(); drawDetail();
    const btn = [...canvasInner.querySelectorAll('.screen-card')].find(b => b.querySelector('.name').textContent === s.name);
    if (btn) btn.focus();
  }

  function startDrag(e, s) {
    if (!canWrite || e.button !== 0) return;
    const startX = e.clientX, startY = e.clientY, ox = s.x, oy = s.y;
    const el = e.currentTarget;
    let moved = false;
    el.setPointerCapture(e.pointerId);
    const move = ev => {
      const dx = Math.round((ev.clientX - startX) / SCALE / 10) * 10;
      const dy = Math.round((ev.clientY - startY) / SCALE / 10) * 10;
      if (!moved && Math.abs(dx) + Math.abs(dy) < 10) return;
      moved = true;
      s.x = Math.max(0, ox + dx); s.y = Math.max(0, oy + dy);
      el.style.left = s.x * SCALE + 'px'; el.style.top = s.y * SCALE + 'px';
    };
    const up = () => {
      el.removeEventListener('pointermove', move);
      el.removeEventListener('pointerup', up);
      if (moved) { selected = s.name; editor.markDirty(); drawCanvas(); drawDetail(); }
    };
    el.addEventListener('pointermove', move);
    el.addEventListener('pointerup', up);
  }

  function drawDetail() {
    const s = draft.screens.find(x => x.name === selected);
    if (!s) { clear(detail, h('div', { class: 'card empty' }, 'Select a screen to edit it.')); return; }
    const isServer = s.name.toLowerCase() === serverScreen.toLowerCase();
    const others = draft.screens.filter(x => x !== s).map(x => x.name);
    const nameInput = h('input', { type: 'text', name: 'name', value: s.name, disabled: isServer || !canWrite, maxlength: 255,
      onchange: e => {
        const old = s.name; const nv = e.target.value.trim();
        if (!nv) return;
        s.name = nv; selected = nv;
        for (const o of draft.screens) for (const d of DIRS) if (o.links[d] === old) o.links[d] = nv;
        drawCanvas();
      } });
    const aliases = h('input', { type: 'text', name: 'aliases', value: s.aliases.join(', '), disabled: !canWrite,
      oninput: e => { s.aliases = e.target.value.split(',').map(a => a.trim()).filter(Boolean); } });
    const num = (label, key, min, max) => bindNumber(field({ label, input: h('input', { type: 'number', name: key, min, max, value: s[key], disabled: !canWrite }) }),
      v => { s[key] = v; drawCanvas(); });
    clear(detail, h('section', { class: 'card' },
      h('h2', {}, isServer ? `${s.name} (this server)` : s.name),
      field({ label: 'Screen name', input: nameInput, hint: isServer ? 'The server screen follows the "Screen name" setting.' : 'Must match the client\'s screen name. A "-1" suffix is added when missing.' }),
      field({ label: 'Aliases', input: aliases, hint: 'Comma separated alternative names.' }),
      h('div', { class: 'inline-fields' }, num('X', 'x', 0, 100000), num('Y', 'y', 0, 100000), num('Width', 'width', 1, 100000), num('Height', 'height', 1, 100000)),
      draft.options.useCustomLinks
        ? h('fieldset', {}, h('legend', {}, 'Neighbours'), h('div', { class: 'inline-fields' }, DIRS.map(d => {
          const sel = select(`links.${d}`, [['', '— none —'], ...others.map(o => [o, o])], s.links[d]);
          sel.disabled = !canWrite;
          sel.addEventListener('change', () => { s.links[d] = sel.value; });
          return field({ label: d[0].toUpperCase() + d.slice(1), input: sel });
        })))
        : h('p', { class: 'hint' }, 'Neighbours follow the layout order (enable custom links to set them explicitly).'),
      h('details', {}, h('summary', {}, 'Keyboard modifiers, corners and fixes'),
        h('div', { class: 'inline-fields' }, MODS.map(m => {
          const sel = select(`modifiers.${m}`, ['shift', 'ctrl', 'alt', 'meta', 'super', 'none'], s.modifiers[m]);
          sel.disabled = !canWrite;
          sel.addEventListener('change', () => { s.modifiers[m] = sel.value; });
          return field({ label: `${m} sends`, input: sel });
        })),
        h('div', { class: 'inline-fields' }, Object.entries(CORNER_LABELS).map(([k, l]) =>
          bindCheck(checkbox({ label: `Dead corner: ${l}`, name: 'corner.' + k, checked: s.switchCorners[k], disabled: !canWrite }), v => { s.switchCorners[k] = v; }))),
        num('Corner size (px)', 'switchCornerSize', 0, 1000),
        Object.entries(FIX_LABELS).map(([k, l]) =>
          bindCheck(checkbox({ label: l, name: 'fix.' + k, checked: s.fixes[k], disabled: !canWrite }), v => { s.fixes[k] = v; }))),
      canWrite && !isServer ? h('div', { class: 'btn-row', style: 'margin-top:.75rem' },
        h('button', { type: 'button', class: 'danger', onclick: async () => {
          if (!(await confirmDialog(`Remove ${s.name}?`, 'The screen is removed from the layout when you save.', 'Remove', true))) return;
          draft.screens = draft.screens.filter(x => x !== s);
          for (const o of draft.screens) for (const d of DIRS) if (o.links[d] === s.name) o.links[d] = '';
          selected = draft.screens[0] ? draft.screens[0].name : null;
          editor.markDirty(); drawCanvas(); drawDetail();
        } }, 'Remove screen')) : null));
  }

  function addScreen() {
    let n = 1;
    while (draft.screens.some(s => s.name.toLowerCase() === `new-screen-${n}`)) n++;
    const maxX = Math.max(0, ...draft.screens.map(s => s.x + s.width));
    const s = newScreen(`new-screen-${n}`, maxX + 20, 20);
    draft.screens.push(s);
    selected = s.name;
    editor.markDirty(); drawCanvas(); drawDetail();
    const input = detail.querySelector('input[name="name"]');
    if (input) { input.focus(); input.select(); }
  }

  function autoLayout() {
    draft.screens.forEach((s, i) => { s.x = 20 + i * (s.width + 20); s.y = 20; });
    editor.markDirty(); drawCanvas(); drawDetail();
  }

  editor.extraActions.append(h('span', { class: 'hint' }, 'Saved changes take effect after "Apply to running server" (or Reload in the desktop panel).'));
  const offStatus = ctx.live.on('server.status', () => { if (draft) drawCanvas(); });
  editor.reload(false);
  return () => { editor.destroy(); offStatus(); };
}

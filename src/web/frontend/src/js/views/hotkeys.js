import { h, clear, field, checkbox, select, dialog, confirmDialog, uid } from '../ui.js';
import { pageHead, ConfigEditor } from './common.js';

const TYPE_LABELS = {
  keystroke: 'Press and release keys', keyDown: 'Press keys', keyUp: 'Release keys',
  switchToScreen: 'Switch to screen', toggleScreen: 'Switch to the next screen',
  switchInDirection: 'Switch in direction', lockCursorToScreen: 'Lock cursor to screen',
};

// Browser key names -> Etherwaver key names. Presentation only: the backend
// validates every key name and rejects unknown ones.
const KEYMAP = {
  ' ': 'Space', Escape: 'Escape', Tab: 'Tab', Backspace: 'BackSpace', Enter: 'Return', Insert: 'Insert',
  Delete: 'Delete', Pause: 'Pause', PrintScreen: 'Print', Home: 'Home', End: 'End', ArrowLeft: 'Left',
  ArrowUp: 'Up', ArrowRight: 'Right', ArrowDown: 'Down', PageUp: 'PageUp', PageDown: 'PageDown',
  CapsLock: 'CapsLock', NumLock: 'NumLock', ScrollLock: 'ScrollLock', ContextMenu: 'Menu', Help: 'Help',
  Clear: 'Clear', ',': 'Comma', ';': 'Semicolon', AudioVolumeDown: 'AudioDown', AudioVolumeMute: 'AudioMute',
  AudioVolumeUp: 'AudioUp', MediaPlayPause: 'AudioPlay', MediaStop: 'AudioStop', MediaTrackPrevious: 'AudioPrev',
  MediaTrackNext: 'AudioNext', BrowserBack: 'WWWBack', BrowserForward: 'WWWForward', BrowserHome: 'WWWHome',
};

function seqText(keys, mouse) {
  if (!keys || !keys.length) return '—';
  return mouse ? `mouse button ${keys[0]}` : keys.join(' + ');
}

function actionText(a) {
  switch (a.type) {
    case 'keystroke': case 'keyDown': case 'keyUp':
      return `${TYPE_LABELS[a.type]}: ${seqText(a.keys, a.mouse)}${a.screens.length ? ' on ' + a.screens.join(', ') : ''}`;
    case 'switchToScreen': return `Switch to ${a.screen}`;
    case 'switchInDirection': return `Switch ${a.direction}`;
    case 'lockCursorToScreen': return `Lock cursor (${a.lockMode})`;
    default: return TYPE_LABELS[a.type] || a.type;
  }
}

// Captures a key combination. Records only which keys were pressed in this
// input, never anything typed elsewhere, and nothing is sent until saved.
function keyCapture(initial, onChange) {
  const input = h('input', { type: 'text', class: 'key-capture', readonly: true, value: (initial.keys || []).join('+'),
    'aria-label': 'Key combination. Focus and press the keys; Backspace alone clears.' });
  input.addEventListener('keydown', e => {
    if (e.key === 'Tab') return;
    e.preventDefault();
    const mods = [];
    if (e.shiftKey && e.key !== 'Shift') mods.push('Shift');
    if (e.ctrlKey && e.key !== 'Control') mods.push('Control');
    if (e.altKey && e.key !== 'Alt') mods.push('Alt');
    if (e.metaKey && e.key !== 'Meta') mods.push('Meta');
    if (['Shift', 'Control', 'Alt', 'Meta', 'AltGraph'].includes(e.key)) return;
    if (e.key === 'Backspace' && !mods.length && input.value) { input.value = ''; onChange([]); return; }
    let name = KEYMAP[e.key];
    if (!name && /^F\d{1,2}$/.test(e.key)) name = e.key;
    if (!name && e.key.length === 1) name = e.key.toLowerCase();
    if (!name) return;
    const keys = [...mods, name];
    input.value = keys.join('+');
    onChange(keys);
  });
  return input;
}

function sequenceEditor(obj, label) {
  const id = uid('seq');
  const mouseBox = h('input', { type: 'checkbox', id: id + '-m', checked: obj.mouse });
  const capture = keyCapture(obj.mouse ? { keys: [] } : obj, keys => { obj.keys = keys; obj.mouse = false; mouseBox.checked = false; });
  const button = select('', [['1', 'Left (1)'], ['2', 'Right (2)'], ['3', 'Middle (3)'], ['4', 'Other (4)']], obj.mouse ? obj.keys[0] : '1');
  button.setAttribute('aria-label', 'Mouse button');
  const manual = h('input', { type: 'text', value: obj.mouse ? '' : (obj.keys || []).join('+'), 'aria-label': 'Key names separated by +',
    oninput: e => { obj.keys = e.target.value.split('+').map(s => s.trim()).filter(Boolean); obj.mouse = false; capture.value = obj.keys.join('+'); } });
  const sync = () => {
    const m = mouseBox.checked;
    capture.disabled = m; manual.disabled = m; button.disabled = !m;
    if (m) { obj.mouse = true; obj.keys = [button.value]; }
  };
  mouseBox.addEventListener('change', sync);
  button.addEventListener('change', sync);
  sync();
  return h('fieldset', {}, h('legend', {}, label),
    field({ label: 'Press the keys', input: capture, hint: 'Focus the box and press the combination, e.g. Ctrl+Alt+Right.' }),
    field({ label: 'Or type key names', input: manual, hint: 'Examples: Control+Alt+Left, Shift+F12, a' }),
    h('label', { class: 'check', for: id + '-m' }, mouseBox, 'Use a mouse button instead'), button);
}

function editHotkey(hk, screens, onDone) {
  const work = structuredClone(hk);
  const actionsHost = h('div');
  const errors = h('div', { class: 'field-error', 'aria-live': 'polite' });

  function drawActions() {
    clear(actionsHost, work.actions.map((a, i) => {
      const typeSel = select('', Object.entries(TYPE_LABELS), a.type);
      typeSel.setAttribute('aria-label', `Action ${i + 1} type`);
      typeSel.addEventListener('change', () => { a.type = typeSel.value; drawActions(); });
      const specific = [];
      if (['keystroke', 'keyDown', 'keyUp'].includes(a.type)) {
        specific.push(sequenceEditor(a, 'Keys to send'));
        const scr = h('div', { class: 'pill-list', role: 'group', 'aria-label': 'Send to screens' },
          screens.map(s => {
            const id = uid('s');
            const cb = h('input', { type: 'checkbox', id, checked: a.screens.includes(s),
              onchange: e => { a.screens = e.target.checked ? [...a.screens, s] : a.screens.filter(x => x !== s); } });
            return h('label', { class: 'check', for: id }, cb, s);
          }));
        specific.push(h('div', { class: 'field' }, h('span', { style: 'font-weight:600' }, 'Send to screens'),
          h('span', { class: 'hint' }, 'None selected = all screens.'), scr));
      } else if (a.type === 'switchToScreen') {
        const sel = select('', screens, a.screen || screens[0]);
        a.screen = a.screen || screens[0] || '';
        sel.addEventListener('change', () => { a.screen = sel.value; });
        specific.push(field({ label: 'Screen', input: sel }));
      } else if (a.type === 'switchInDirection') {
        const sel = select('', ['left', 'right', 'up', 'down'], a.direction);
        sel.addEventListener('change', () => { a.direction = sel.value; });
        specific.push(field({ label: 'Direction', input: sel }));
      } else if (a.type === 'lockCursorToScreen') {
        const sel = select('', [['toggle', 'Toggle'], ['on', 'Lock'], ['off', 'Unlock']], a.lockMode);
        sel.addEventListener('change', () => { a.lockMode = sel.value; });
        specific.push(field({ label: 'Mode', input: sel }));
      }
      const rel = checkbox({ label: 'Run when the hotkey is released', checked: a.activeOnRelease });
      rel.querySelector('input').addEventListener('change', e => { a.activeOnRelease = e.target.checked; });
      return h('fieldset', {}, h('legend', {}, `Action ${i + 1}`),
        field({ label: 'Action', input: typeSel }), specific, rel,
        h('button', { type: 'button', class: 'danger', onclick: () => { work.actions.splice(i, 1); drawActions(); } }, 'Remove action'));
    }), h('button', { type: 'button', onclick: () => {
      work.actions.push({ type: 'switchToScreen', keys: [], mouse: false, screens: [], screen: screens[0] || '', direction: 'left', lockMode: 'toggle', activeOnRelease: false });
      drawActions();
    } }, 'Add action'));
  }
  drawActions();
  const ok = h('button', { class: 'primary', type: 'button', onclick: () => {
    if (!work.keys.length) { errors.textContent = 'Choose the hotkey first.'; return; }
    if (!work.actions.length) { errors.textContent = 'Add at least one action.'; return; }
    onDone(work); dlg.close();
  } }, 'Done');
  const dlg = dialog({ title: hk.keys.length ? 'Edit hotkey' : 'New hotkey', wide: true,
    body: h('div', {}, sequenceEditor(work, 'Hotkey'), h('h3', {}, 'Actions'), actionsHost, errors,
      h('p', { class: 'hint' }, 'Changes are kept in the page until you press Save.')),
    actions: [h('button', { type: 'button', onclick: () => dlg.close() }, 'Cancel'), ok] });
}

export function render(ctx, root) {
  const host = h('div');
  const canWrite = ctx.can('config:write');
  clear(root, pageHead('Hotkeys', 'Keyboard shortcuts handled by the Etherwaver server'), host);
  let draft = null;
  const editor = new ConfigEditor(ctx, {
    resource: 'layout', root: host, readOnly: !canWrite,
    load: async () => { const r = await ctx.api.get('config/layout'); return { data: r.layout, etag: r.etag }; },
    save: async (data, etag) => { const r = await ctx.api.put('config/layout', data, { etag }); return { data: r.layout, etag: r.etag }; },
    collect: () => draft,
    render: data => { draft = structuredClone(data); return build(); },
  });

  function build() {
    const screens = draft.screens.map(s => s.name);
    const list = h('div');
    const redraw = () => clear(list, draft.hotkeys.length ? h('div', { class: 'table-wrap' }, h('table', {},
      h('thead', {}, h('tr', {}, ['Hotkey', 'Actions', ''].map(t => h('th', { scope: 'col' }, t)))),
      h('tbody', {}, draft.hotkeys.map((hk, i) => h('tr', {},
        h('td', { class: 'mono' }, seqText(hk.keys, hk.mouse)),
        h('td', {}, h('ul', { style: 'margin:0;padding-left:1rem' }, hk.actions.map(a => h('li', {}, actionText(a))))),
        h('td', {}, h('div', { class: 'btn-row' },
          h('button', { type: 'button', disabled: !canWrite, onclick: () => editHotkey(hk, screens, w => { draft.hotkeys[i] = w; editor.markDirty(); redraw(); }) }, 'Edit'),
          h('button', { type: 'button', class: 'danger', disabled: !canWrite, onclick: async () => {
            if (await confirmDialog('Remove hotkey?', `${seqText(hk.keys, hk.mouse)} will be removed when you save.`, 'Remove', true)) {
              draft.hotkeys.splice(i, 1); editor.markDirty(); redraw();
            }
          } }, 'Remove')))))))) : h('div', { class: 'card empty' }, 'No hotkeys. Scroll Lock locks the cursor to the screen by default.'));
    redraw();
    return h('div', {}, h('div', { class: 'toolbar' },
      h('button', { type: 'button', disabled: !canWrite, onclick: () => editHotkey({ keys: [], mouse: false, actions: [] }, screens, w => { draft.hotkeys.push(w); editor.markDirty(); redraw(); }) }, 'Add hotkey')),
      list);
  }

  editor.reload(false);
  return () => editor.destroy();
}

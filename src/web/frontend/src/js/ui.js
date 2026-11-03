// Small DOM helpers (no framework).

export function h(tag, attrs = {}, ...children) {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (v === undefined || v === null || v === false) continue;
    if (k === 'class') el.className = v;
    else if (k === 'dataset') Object.assign(el.dataset, v);
    else if (k === 'style') el.style.cssText = v; // CSSOM is allowed by the strict CSP
    else if (k.startsWith('on') && typeof v === 'function') el.addEventListener(k.slice(2), v);
    else if (k === 'value') el.value = v;
    else if (k === 'checked') el.checked = !!v;
    else if (v === true) el.setAttribute(k, '');
    else el.setAttribute(k, v);
  }
  append(el, children);
  return el;
}

function append(el, children) {
  for (const c of children.flat(Infinity)) {
    if (c === null || c === undefined || c === false) continue;
    el.appendChild(c instanceof Node ? c : document.createTextNode(String(c)));
  }
}

export function clear(el, ...children) {
  el.replaceChildren();
  append(el, children);
  return el;
}

let idCounter = 0;
export const uid = p => `${p || 'id'}-${++idCounter}`;

export function fmtTime(ts) {
  if (!ts) return '—';
  const d = new Date(ts * 1000);
  return d.toLocaleString();
}

export function fmtAgo(ts) {
  if (!ts) return '—';
  const s = Math.max(0, Math.round(Date.now() / 1000 - ts));
  if (s < 5) return 'just now';
  if (s < 60) return `${s} s ago`;
  if (s < 3600) return `${Math.round(s / 60)} min ago`;
  if (s < 86400) return `${Math.round(s / 3600)} h ago`;
  return new Date(ts * 1000).toLocaleDateString();
}

export function fmtDuration(seconds) {
  if (seconds === null || seconds === undefined) return '—';
  const s = Math.round(seconds);
  const d = Math.floor(s / 86400), hh = Math.floor((s % 86400) / 3600), m = Math.floor((s % 3600) / 60);
  if (d) return `${d} d ${hh} h`;
  if (hh) return `${hh} h ${m} min`;
  if (m) return `${m} min`;
  return `${s} s`;
}

export function badge(text, kind = '') {
  return h('span', { class: `badge ${kind}` }, text);
}

// toasts are for confirmations that arrived from the backend; failures that
// need action are shown inline next to the affected control
export function toast(message, kind = 'ok', timeout = 5000) {
  const box = document.getElementById('toasts');
  const el = h('div', { class: `toast ${kind}`, role: kind === 'danger' ? 'alert' : 'status' }, message);
  box.appendChild(el);
  while (box.childElementCount > 3) box.firstElementChild.remove();
  setTimeout(() => el.remove(), timeout);
}

export function errorText(err) {
  if (!err) return '';
  let text = err.message || String(err);
  if (err.requestId) text += ` (request ${err.requestId})`;
  return text;
}

export function banner(kind, title, text, ...actions) {
  return h('div', { class: `banner ${kind}`, role: kind === 'danger' || kind === 'warn' ? 'alert' : 'status' },
    h('div', {}, h('strong', {}, title), text ? h('div', { class: 'small' }, text) : null),
    actions.length ? h('div', { class: 'btn-row' }, actions) : null);
}

// Runs an async action from a button: disables it while in flight so the
// same action cannot be submitted twice, and reports errors inline.
export async function busy(button, fn) {
  if (button.getAttribute('aria-busy') === 'true') return undefined;
  button.setAttribute('aria-busy', 'true');
  const wasDisabled = button.disabled;
  button.disabled = true;
  try {
    return await fn();
  } finally {
    button.removeAttribute('aria-busy');
    button.disabled = wasDisabled;
  }
}

export function dialog({ title, body, actions = [], onClose, wide = false }) {
  const titleId = uid('dlg');
  const dlg = h('dialog', { 'aria-labelledby': titleId, style: wide ? 'width:min(900px, calc(100vw - 2rem))' : null },
    h('div', { class: 'dialog-body' }, h('h2', { id: titleId }, title), body),
    h('div', { class: 'dialog-foot' }, actions));
  document.body.appendChild(dlg);
  const previous = document.activeElement;
  dlg.addEventListener('close', () => {
    dlg.remove();
    if (previous && previous.focus) previous.focus();
    onClose && onClose(dlg.returnValue);
  });
  dlg.showModal();
  const first = dlg.querySelector('input, select, textarea, button');
  if (first) first.focus();
  return dlg;
}

export function confirmDialog(title, text, confirmLabel = 'Confirm', danger = false) {
  return new Promise(resolve => {
    let result = false;
    const ok = h('button', { class: danger ? 'danger' : 'primary', type: 'button',
      onclick: () => { result = true; dlg.close('ok'); } }, confirmLabel);
    const cancel = h('button', { type: 'button', onclick: () => dlg.close('cancel') }, 'Cancel');
    const dlg = dialog({ title, body: h('p', {}, text), actions: [cancel, ok], onClose: () => resolve(result) });
    cancel.focus();
  });
}

export function field({ label, id, input, hint, error, limitation }) {
  id = id || input.id || uid('f');
  input.id = id;
  const hintId = hint ? id + '-hint' : null;
  const errId = id + '-err';
  if (hintId) input.setAttribute('aria-describedby', hintId + ' ' + errId);
  else input.setAttribute('aria-describedby', errId);
  return h('div', { class: 'field', dataset: { field: input.name || '' } },
    h('label', { for: id }, label),
    input,
    hint ? h('div', { class: 'hint', id: hintId }, hint) : null,
    limitation ? h('div', { class: 'limitation' }, limitation) : null,
    h('div', { class: 'field-error', id: errId, 'aria-live': 'polite' }, error || ''));
}

export function checkbox({ label, name, checked, hint, disabled, limitation }) {
  const id = uid('c');
  const input = h('input', { type: 'checkbox', id, name, checked, disabled });
  return h('div', {},
    h('label', { class: 'check', for: id }, input,
      h('span', {}, label, hint ? h('div', { class: 'hint' }, hint) : null)),
    limitation ? h('div', { class: 'limitation' }, limitation) : null);
}

export function select(name, options, value) {
  return h('select', { name }, options.map(o => {
    const [v, t] = Array.isArray(o) ? o : [o, o];
    return h('option', { value: v, selected: String(v) === String(value) }, t);
  }));
}

// Marks fields that the backend rejected (422) and focuses the first one.
export function showFieldErrors(form, err) {
  for (const el of form.querySelectorAll('.field-error')) el.textContent = '';
  for (const el of form.querySelectorAll('[aria-invalid]')) el.removeAttribute('aria-invalid');
  const errors = err && err.fieldErrors ? err.fieldErrors : {};
  let first = null;
  for (const [fieldName, message] of Object.entries(errors)) {
    const input = form.querySelector(`[name="${CSS.escape(fieldName)}"]`);
    if (!input) continue;
    input.setAttribute('aria-invalid', 'true');
    const errEl = document.getElementById(input.id + '-err');
    if (errEl) errEl.textContent = message;
    first = first || input;
  }
  if (first) first.focus();
  return Object.keys(errors);
}

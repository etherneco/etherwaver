import { api } from '../api.js';
import { h, banner, field, errorText, busy } from '../ui.js';

export function renderLogin({ message, setupRequired, onSuccess }) {
  const user = h('input', { type: 'text', name: 'username', autocomplete: 'username', required: true });
  const pass = h('input', { type: 'password', name: 'password', autocomplete: 'current-password', required: true });
  const status = h('div', { 'aria-live': 'assertive' });
  const btn = h('button', { class: 'primary', type: 'submit' }, 'Sign in');
  const form = h('form', {
    onsubmit: async e => {
      e.preventDefault();
      status.replaceChildren();
      await busy(btn, async () => {
        try {
          const s = await api.post('session', { username: user.value.trim(), password: pass.value });
          pass.value = '';
          onSuccess(s);
        } catch (err) {
          pass.value = '';
          status.replaceChildren(banner('danger', err.status === 429 ? 'Too many attempts' : 'Sign-in failed', errorText(err)));
          pass.focus();
        }
      });
    },
  },
  field({ label: 'User name', input: user }),
  field({ label: 'Password', input: pass }),
  btn);
  const card = h('div', { class: 'card login-card' },
    h('div', { class: 'brand', style: 'margin-bottom:1rem' }, h('span', { class: 'brand-mark', 'aria-hidden': 'true' }), 'Etherwaver'),
    h('h1', {}, 'Sign in to the web panel'),
    message ? banner('info', message) : null,
    setupRequired ? banner('warn', 'No accounts yet',
      'Create the first administrator on this computer: waver-web user add <name> --role admin') : null,
    status, form);
  setTimeout(() => user.focus(), 0);
  return h('main', { class: 'login-wrap', id: 'main' }, card);
}

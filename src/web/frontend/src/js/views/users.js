import { h, clear, field, select, busy, toast, errorText, confirmDialog, dialog, fmtTime, badge, banner, showFieldErrors } from '../ui.js';
import { pageHead, loading, errorBlock } from './common.js';

const ROLE_HELP = {
  viewer: 'read status and configuration',
  operator: 'also switch screens, lock the cursor, apply the configuration, start/stop the service, read logs',
  admin: 'also change the configuration, trust certificates, import, manage users, read the audit log',
};

export function render(ctx, root) {
  const host = h('div');
  const addBtn = h('button', { type: 'button', class: 'primary', onclick: () => addUser() }, 'Add user');
  clear(root, pageHead('Users', 'Accounts of the web panel', addBtn),
    h('div', { class: 'card', style: 'margin-bottom:1rem' }, h('ul', { class: 'small', style: 'margin:0' },
      Object.entries(ROLE_HELP).map(([r, t]) => h('li', {}, h('strong', {}, r), ': ', t)))), host);

  async function load() {
    try {
      const r = await ctx.api.get('users');
      clear(host, h('div', { class: 'table-wrap' }, h('table', {},
        h('thead', {}, h('tr', {}, ['User', 'Role', 'State', 'Created', 'Password changed', ''].map(t => h('th', { scope: 'col' }, t)))),
        h('tbody', {}, r.users.map(u => {
          const roleSel = select('role', r.roles, u.role);
          roleSel.setAttribute('aria-label', `Role of ${u.username}`);
          roleSel.addEventListener('change', async () => {
            try { await ctx.api.patch(`users/${encodeURIComponent(u.username)}`, { role: roleSel.value }); toast(`${u.username} is now ${roleSel.value}`); }
            catch (e) { toast(errorText(e), 'danger', 8000); }
            load();
          });
          return h('tr', {}, h('td', {}, u.username, u.username === ctx.session.user ? ' (you)' : ''), h('td', {}, roleSel),
            h('td', {}, u.disabled ? badge('disabled', 'warn') : badge('active', 'ok')),
            h('td', {}, fmtTime(u.created)), h('td', {}, fmtTime(u.passwordChanged)),
            h('td', {}, h('div', { class: 'btn-row' },
              h('button', { type: 'button', onclick: e => busy(e.currentTarget, async () => {
                try { await ctx.api.patch(`users/${encodeURIComponent(u.username)}`, { disabled: !u.disabled }); }
                catch (err) { toast(errorText(err), 'danger', 8000); }
                load();
              }) }, u.disabled ? 'Enable' : 'Disable'),
              h('button', { type: 'button', onclick: () => resetPassword(u.username) }, 'Reset password'),
              h('button', { type: 'button', class: 'danger', onclick: async e => {
                const btn = e.currentTarget;
                if (!(await confirmDialog(`Delete ${u.username}?`, 'The account and its sessions are removed.', 'Delete', true))) return;
                await busy(btn, async () => {
                  try { await ctx.api.del(`users/${encodeURIComponent(u.username)}`); toast('User deleted.'); }
                  catch (err) { toast(errorText(err), 'danger', 8000); }
                });
                load();
              } }, 'Delete'))));
        })))));
    } catch (e) { clear(host, errorBlock(e, load)); }
  }

  function addUser() {
    const name = h('input', { type: 'text', name: 'username', autocomplete: 'off' });
    const pass = h('input', { type: 'password', name: 'password', autocomplete: 'new-password' });
    const role = select('role', ['viewer', 'operator', 'admin'], 'viewer');
    const form = h('form', {}, field({ label: 'User name', input: name }), field({ label: 'Password', input: pass, hint: 'At least 10 characters.' }),
      field({ label: 'Role', input: role }));
    const ok = h('button', { class: 'primary', type: 'button', onclick: () => busy(ok, async () => {
      try {
        await ctx.api.post('users', { username: name.value.trim(), password: pass.value, role: role.value });
        toast('User created.'); dlg.close(); load();
      } catch (e) {
        if (!showFieldErrors(form, e).length) form.prepend(banner('danger', 'Not created', errorText(e)));
      }
    }) }, 'Create');
    const dlg = dialog({ title: 'Add user', body: form, actions: [h('button', { type: 'button', onclick: () => dlg.close() }, 'Cancel'), ok] });
  }

  function resetPassword(username) {
    const pass = h('input', { type: 'password', name: 'password', autocomplete: 'new-password' });
    const form = h('form', {}, field({ label: `New password for ${username}`, input: pass, hint: 'The user is signed out everywhere.' }));
    const ok = h('button', { class: 'primary', type: 'button', onclick: () => busy(ok, async () => {
      try { await ctx.api.patch(`users/${encodeURIComponent(username)}`, { password: pass.value }); toast('Password changed.'); dlg.close(); }
      catch (e) { form.querySelector('.field-error').textContent = errorText(e); }
    }) }, 'Set password');
    const dlg = dialog({ title: 'Reset password', body: form, actions: [h('button', { type: 'button', onclick: () => dlg.close() }, 'Cancel'), ok] });
  }

  clear(host, loading());
  load();
}

export function renderAccount(ctx, root) {
  const cur = h('input', { type: 'password', name: 'currentPassword', autocomplete: 'current-password' });
  const nw = h('input', { type: 'password', name: 'newPassword', autocomplete: 'new-password' });
  const nw2 = h('input', { type: 'password', name: 'confirm', autocomplete: 'new-password' });
  const status = h('div', { 'aria-live': 'polite' });
  const btn = h('button', { class: 'primary', type: 'submit' }, 'Change password');
  const form = h('form', { onsubmit: e => { e.preventDefault(); busy(btn, async () => {
    clear(status);
    if (nw.value !== nw2.value) { showFieldErrors(form, { fieldErrors: { confirm: 'The passwords do not match.' } }); return; }
    try {
      await ctx.api.post('session/password', { currentPassword: cur.value, newPassword: nw.value });
      cur.value = nw.value = nw2.value = '';
      showFieldErrors(form, null);
      clear(status, banner('ok', 'Password changed'));
    } catch (err) {
      if (!showFieldErrors(form, err).length) clear(status, banner('danger', 'Not changed', errorText(err)));
    }
  }); } },
  field({ label: 'Current password', input: cur }), field({ label: 'New password', input: nw, hint: 'At least 10 characters.' }),
  field({ label: 'Repeat new password', input: nw2 }), btn);
  clear(root, pageHead('Your account', `${ctx.session.user} · ${ctx.session.role}`),
    h('section', { class: 'card', style: 'max-width:34rem' }, h('h2', {}, 'Password'), status, form),
    h('section', { class: 'card', style: 'max-width:34rem;margin-top:1rem' }, h('h2', {}, 'Session'),
      h('p', {}, `You are signed out after ${Math.round(ctx.session.idleTimeout / 60)} minutes without activity.`)));
}

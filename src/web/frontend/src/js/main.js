import { api, setCsrfToken, setUnauthenticatedHandler } from './api.js';
import { LiveChannel } from './live.js';
import { h, clear, banner, errorText, busy, confirmDialog, toast } from './ui.js';
import * as dashboard from './views/dashboard.js';
import * as devices from './views/devices.js';
import * as sessions from './views/sessions.js';
import * as layout from './views/layout.js';
import * as hotkeys from './views/hotkeys.js';
import * as settings from './views/settings.js';
import * as security from './views/security.js';
import * as logs from './views/logs.js';
import * as diagnostics from './views/diagnostics.js';
import * as activity from './views/activity.js';
import * as users from './views/users.js';
import { renderLogin } from './views/login.js';

const NAV = [
  { group: 'Overview' },
  { path: '', label: 'Dashboard', view: dashboard, perm: 'status:read' },
  { path: 'devices', label: 'Devices', view: devices, perm: 'devices:read' },
  { path: 'sessions', label: 'Sessions', view: sessions, perm: 'status:read' },
  { group: 'Configuration' },
  { path: 'layout', label: 'Screens & layout', view: layout, perm: 'config:read' },
  { path: 'hotkeys', label: 'Hotkeys', view: hotkeys, perm: 'config:read' },
  { path: 'settings', label: 'Settings', view: settings, perm: 'config:read' },
  { path: 'security', label: 'Security', view: security, perm: 'security:read' },
  { group: 'Operations' },
  { path: 'logs', label: 'Logs', view: logs, perm: 'logs:read' },
  { path: 'diagnostics', label: 'Diagnostics', view: diagnostics, perm: 'diagnostics:read' },
  { path: 'activity', label: 'Activity & jobs', view: activity, perm: 'jobs:read' },
  { path: 'users', label: 'Users', view: users, perm: 'users:manage' },
];

const THEMES = ['system', 'light', 'dark'];

function storageGet(key) { try { return localStorage.getItem(key); } catch { return null; } }
function storageSet(key, v) { try { localStorage.setItem(key, v); } catch { /* ignore */ } }

function applyTheme(theme) {
  if (theme === 'light' || theme === 'dark') document.documentElement.dataset.theme = theme;
  else delete document.documentElement.dataset.theme;
}

class App {
  constructor() {
    this.root = document.getElementById('app');
    this.live = new LiveChannel();
    this.session = null;
    this.meta = null;
    this.status = null;
    this.dirty = new Set();
    this.cleanup = null;
    this.theme = storageGet('ew-theme') || 'system';
    applyTheme(this.theme);
    setUnauthenticatedHandler(() => this.signedOut('Your session has ended. Please sign in again.'));
    window.addEventListener('hashchange', () => this.onHashChange());
    window.addEventListener('beforeunload', e => {
      if (this.dirty.size) { e.preventDefault(); e.returnValue = ''; }
    });
    this.live.on('channel', s => this.renderConnection());
    this.live.on('server.status', s => { this.status = s; this.renderConnection(); });
    this.live.on('backend.state', () => this.refreshStatus());
    this.live.on('session.expired', () => this.signedOut('Your session expired. Please sign in again.'));
    this.live.on('resync', () => this.refreshStatus());
    this.currentHash = location.hash;
  }

  // ---------------------------------------------------------- context
  can(perm) { return !!(this.session && this.session.permissions.includes(perm)); }
  setDirty(key, isDirty) {
    if (isDirty) this.dirty.add(key); else this.dirty.delete(key);
  }

  async start() {
    try {
      const s = await api.get('session');
      await this.signedIn(s);
    } catch (e) {
      if (e.status === 401) this.showLogin(null, e.details && e.details.setupRequired);
      else this.fatal(e);
    }
  }

  fatal(e) {
    clear(this.root, h('main', { id: 'main' }, banner('danger', 'The web panel is not available', errorText(e),
      h('button', { onclick: () => location.reload() }, 'Retry'))));
  }

  showLogin(message, setupRequired) {
    this.live.stop();
    clear(this.root, renderLogin({ message, setupRequired, onSuccess: s => this.signedIn(s) }));
  }

  signedOut(message) {
    if (!this.session) return;
    this.session = null;
    this.dirty.clear();
    setCsrfToken(null);
    if (this.cleanup) { this.cleanup(); this.cleanup = null; }
    this.showLogin(message);
  }

  async signedIn(session) {
    this.session = session;
    setCsrfToken(session.csrfToken);
    try {
      this.meta = await api.get('meta');
      this.status = await api.get('status');
    } catch (e) { this.fatal(e); return; }
    this.renderShell();
    this.live.start();
    this.route();
  }

  async refreshStatus() {
    try { this.status = await api.get('status'); this.renderConnection(); } catch { /* shown by channel */ }
  }

  // ---------------------------------------------------------- shell
  renderShell() {
    const themeBtn = h('button', { class: 'ghost', type: 'button', id: 'theme-btn',
      onclick: () => {
        this.theme = THEMES[(THEMES.indexOf(this.theme) + 1) % THEMES.length];
        storageSet('ew-theme', this.theme); applyTheme(this.theme); this.updateThemeLabel();
      } });
    const menuBtn = h('button', { class: 'ghost menu-toggle', type: 'button', 'aria-controls': 'sidebar',
      'aria-expanded': 'false', onclick: () => this.toggleMenu() }, '☰', h('span', { class: 'sr-only' }, 'Menu'));
    const logout = h('button', { class: 'ghost', type: 'button', onclick: e => busy(e.currentTarget, () => this.logout()) }, 'Sign out');
    this.connEl = h('span', { id: 'conn', 'aria-live': 'polite' });
    this.backendBanner = h('div', { id: 'backend-banner' });
    const sidebar = h('aside', { class: 'sidebar', id: 'sidebar' },
      h('nav', { 'aria-label': 'Main' }, h('ul', {}, NAV.filter(n => n.group || this.can(n.perm)).map(n =>
        n.group ? h('li', { class: 'nav-group', 'aria-hidden': 'true' }, n.group)
          : h('li', {}, h('a', { href: '#/' + n.path, dataset: { path: n.path },
            onclick: () => this.toggleMenu(false) }, n.label))))));
    this.main = h('main', { id: 'main', tabindex: '-1' });
    clear(this.root,
      h('a', { class: 'skip-link', href: '#main', onclick: e => { e.preventDefault(); this.main.focus(); } }, 'Skip to content'),
      h('div', { class: 'app' },
        h('header', { class: 'topbar' }, menuBtn,
          h('span', { class: 'brand' }, h('span', { class: 'brand-mark', 'aria-hidden': 'true' }), 'Etherwaver'),
          h('span', { class: 'spacer' }), this.connEl,
          h('a', { class: 'btn ghost', href: '#/account' }, this.session.user, ' · ', this.session.role),
          themeBtn, logout),
        sidebar,
        h('div', { style: 'min-width:0' }, h('div', { style: 'padding: 1rem 1.5rem 0' }, this.backendBanner), this.main)));
    this.updateThemeLabel();
    this.renderConnection();
  }

  toggleMenu(force) {
    const sb = document.getElementById('sidebar');
    const btn = document.querySelector('.menu-toggle');
    const open = force === undefined ? !sb.classList.contains('open') : force;
    sb.classList.toggle('open', open);
    if (btn) btn.setAttribute('aria-expanded', String(open));
  }

  updateThemeLabel() {
    const b = document.getElementById('theme-btn');
    if (b) b.textContent = `Theme: ${this.theme}`;
  }

  renderConnection() {
    if (!this.connEl) return;
    const ch = this.live.state;
    const backend = this.status && this.status.backend ? this.status.backend.state : 'unknown';
    let kind = 'ok', text = 'Live';
    if (ch !== 'live') { kind = 'warn'; text = ch === 'offline' ? 'Web panel offline' : 'Reconnecting…'; }
    else if (backend !== 'connected') { kind = 'danger'; text = 'Etherwaver server unreachable'; }
    clear(this.connEl, h('span', { class: `conn ${kind}`, title: `live channel: ${ch}, server: ${backend}` },
      h('span', { class: 'dot', 'aria-hidden': 'true' }), text));
    if (this.backendBanner) {
      const st = this.status;
      if (ch !== 'live') {
        clear(this.backendBanner, banner('warn', 'Live updates interrupted',
          'Reconnecting to the web panel. Values on screen may be out of date until the connection is restored.'));
      } else if (st && st.backend && st.backend.state !== 'connected') {
        clear(this.backendBanner, banner('danger', 'Etherwaver server is not reachable',
          `${st.backend.error || 'No connection to the control endpoint'} (${st.backend.address}). ` +
          'Status shown below is not current. Configuration can still be edited; it is applied when the server runs.'));
      } else {
        clear(this.backendBanner);
      }
    }
  }

  async logout() {
    if (this.dirty.size && !(await confirmDialog('Discard unsaved changes?', 'You have unsaved changes.', 'Sign out', true))) return;
    try { await api.del('session'); } catch { /* already gone */ }
    this.dirty.clear();
    this.session = null;
    setCsrfToken(null);
    if (this.cleanup) { this.cleanup(); this.cleanup = null; }
    this.showLogin('You have signed out.');
  }

  // ---------------------------------------------------------- routing
  async onHashChange() {
    if (location.hash === this.currentHash) return;
    if (this.dirty.size) {
      const target = location.hash;
      history.replaceState(null, '', this.currentHash || '#/');
      const ok = await confirmDialog('Leave without saving?', 'This page has unsaved changes that will be lost.', 'Discard changes', true);
      if (!ok) return;
      this.dirty.clear();
      history.pushState(null, '', target);
    }
    this.route();
  }

  route() {
    if (!this.session) return;
    this.currentHash = location.hash;
    const path = (location.hash.replace(/^#\/?/, '') || '').split('/');
    let entry = NAV.find(n => !n.group && n.path === path[0]);
    let params = path.slice(1).map(decodeURIComponent);
    if (path[0] === 'account') entry = { path: 'account', view: users, perm: 'status:read', account: true };
    if (!entry) entry = NAV[1];
    for (const a of document.querySelectorAll('.sidebar a')) {
      if (a.dataset.path === entry.path) a.setAttribute('aria-current', 'page'); else a.removeAttribute('aria-current');
    }
    if (this.cleanup) { this.cleanup(); this.cleanup = null; }
    this.dirty.clear();
    clear(this.main);
    if (!this.can(entry.perm)) {
      this.main.append(banner('warn', 'Not available for your role', `This area requires the "${entry.perm}" permission.`));
      return;
    }
    const ctx = this.context();
    const fn = entry.account ? entry.view.renderAccount : entry.view.render;
    try {
      this.cleanup = fn(ctx, this.main, params) || null;
    } catch (e) {
      console.error(e);
      this.main.append(banner('danger', 'This page failed to load', errorText(e)));
    }
    document.title = `${(entry.label || 'Account')} · Etherwaver`;
  }

  context() {
    return {
      api, live: this.live, session: this.session, meta: this.meta,
      status: () => this.status,
      can: p => this.can(p),
      setDirty: (k, d) => this.setDirty(k, d),
      navigate: hash => { location.hash = hash; },
      refreshStatus: () => this.refreshStatus(),
      toast,
    };
  }
}

const app = new App();
app.start();

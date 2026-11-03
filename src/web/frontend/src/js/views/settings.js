import { h, clear, field, checkbox, select, banner, badge, busy, toast, errorText, confirmDialog } from '../ui.js';
import { pageHead, ConfigEditor, runJob } from './common.js';

export function render(ctx, root) {
  const meta = ctx.meta;
  const canWrite = ctx.can('config:write');
  const formHost = h('div');
  const serviceHost = h('section', { class: 'card', style: 'margin-top:1.5rem' });
  const backupHost = h('section', { class: 'card', style: 'margin-top:1rem' });
  clear(root, pageHead('Settings', 'Shared with the desktop panel: changes saved here appear there and vice versa'),
    formHost, serviceHost, backupHost);

  let current = null;
  const editor = new ConfigEditor(ctx, {
    resource: 'general', root: formHost, readOnly: !canWrite,
    load: async () => { const r = await ctx.api.get('config/general'); return { data: r.general, etag: r.etag }; },
    save: async (data, etag) => {
      const changed = {};
      for (const [k, v] of Object.entries(data)) if (JSON.stringify(v) !== JSON.stringify(current[k])) changed[k] = v;
      const r = await ctx.api.patch('config/general', changed, { etag });
      return { data: r.general, etag: r.etag };
    },
    collect: () => collect(),
    render: data => { current = data; return build(data); },
  });
  editor.onSaved = () => drawService();

  const linux = meta.platform.linux, windows = meta.platform.windows;
  let form;

  function build(g) {
    const t = (name, value, attrs = {}) => h('input', { type: 'text', name, value, disabled: !canWrite, ...attrs });
    const n = (name, value, attrs = {}) => h('input', { type: 'number', name, value, disabled: !canWrite, ...attrs });
    const c = (name, label, checked, extra = {}) => checkbox({ label, name, checked, disabled: !canWrite || extra.disabled, hint: extra.hint, limitation: extra.limitation });
    const sel = (name, opts, value) => { const s = select(name, opts, value); s.disabled = !canWrite; return s; };
    form = h('div', {},
      h('fieldset', {}, h('legend', {}, 'Role of this computer'),
        field({ label: 'Mode', input: sel('mode', [['server', 'Server — share this keyboard and mouse'], ['client', 'Client — use another computer\'s keyboard and mouse']], g.mode) }),
        field({ label: 'Server host name or address (client mode)', input: t('serverHostname', g.serverHostname, { autocomplete: 'off' }) }),
        c('autoConfig', 'Find the server automatically (Bonjour)', g.autoConfig,
          { limitation: 'Automatic discovery runs only in the desktop panel. When the web panel starts the client, the host name above is used.' }),
        field({ label: 'Configuration source (server mode)', input: sel('configSource', [['internal', 'Layout edited in the panels'], ['external', 'External configuration file']], g.configSource) }),
        field({ label: 'External configuration file', input: t('externalConfigFile', g.externalConfigFile), hint: 'Absolute path on the server computer.' })),
      h('fieldset', {}, h('legend', {}, 'Identity and network'),
        field({ label: 'Screen name', input: t('screenName', g.screenName, { maxlength: 255 }), hint: 'How this computer is called in the layout.' }),
        field({ label: 'Port', input: n('port', g.port, { min: 1, max: 65535 }) }),
        field({ label: 'Listen on interface (server)', input: t('interface', g.interface), hint: 'IP address; empty = all interfaces.' })),
      h('fieldset', {}, h('legend', {}, 'Security'),
        c('cryptoEnabled', 'Encrypt connections (TLS)', g.cryptoEnabled),
        c('requireClientCertificate', 'Require client certificates', g.requireClientCertificate, { hint: 'Clients must be trusted under Security.' })),
      h('fieldset', {}, h('legend', {}, 'Logging'),
        field({ label: 'Log level', input: sel('logLevel', meta.enums.logLevels, g.logLevel) }),
        c('logToFile', 'Write the log to a file', g.logToFile),
        field({ label: 'Log file', input: t('logFilename', g.logFilename) })),
      h('fieldset', {}, h('legend', {}, 'Input (client)'),
        c('uhidEnabled', 'Use the Linux UHID virtual input device', g.uhidEnabled,
          { disabled: !linux, limitation: linux ? 'Requires access to /dev/uhid (see dist/udev).' : 'Available only on Linux.' }),
        field({ label: 'UHID device name', input: t('uhidName', g.uhidName, { disabled: !canWrite || !linux }) })),
      h('fieldset', {}, h('legend', {}, 'Desktop panel behaviour'),
        h('p', { class: 'limitation' }, 'These options change how the desktop panel window behaves on this computer; they do not affect the web panel.'),
        field({ label: 'Desktop panel language', input: sel('language', meta.languages.map(l => [l.code, l.name]).concat(
          meta.languages.some(l => l.code === g.language) ? [] : [[g.language, g.language]]), g.language) }),
        c('autoHide', 'Hide the window on startup', g.autoHide),
        c('autoStart', 'Start Etherwaver when the desktop panel opens', g.autoStart),
        c('minimizeToTray', 'Minimize to the system tray', g.minimizeToTray),
        field({ label: 'Elevation (Windows)', input: sel('elevateMode', [['as_needed', 'As needed'], ['always', 'Always'], ['never', 'Never']], g.elevateMode),
          limitation: windows ? null : 'Used only by the Windows service.' })));
    return form;
  }

  function collect() {
    const out = {};
    for (const el of form.querySelectorAll('input[name], select[name]')) {
      if (el.type === 'checkbox') out[el.name] = el.checked;
      else if (el.type === 'number') out[el.name] = el.value === '' ? null : Number(el.value);
      else out[el.name] = el.value;
    }
    return out;
  }

  async function drawService() {
    const st = ctx.status();
    const svc = st ? st.service : null;
    const server = st && st.server;
    const can = ctx.can('service:manage');
    const job = async (e, action, label) => {
      const button = e.currentTarget; // not available any more after an await
      if (action !== 'start' && !(await confirmDialog(`${label}?`, 'Keyboard and mouse sharing is interrupted while Etherwaver restarts or stops.', label, true))) return;
      await runJob(ctx, button, 'service/' + action, label);
      await ctx.refreshStatus(); drawService();
    };
    clear(serviceHost, h('h2', {}, 'Service'),
      svc && svc.enabled ? [
        h('p', {}, 'The web panel manages the Etherwaver process on this computer. ',
          svc.running ? badge(`running, pid ${svc.pid}`, 'ok') : badge('stopped'),
          svc.owner === 'external' ? [' ', badge('another instance is running', 'warn')] : null),
        h('div', { class: 'btn-row' },
          h('button', { type: 'button', disabled: !can || svc.running, onclick: e => job(e, 'start', 'Start') }, 'Start'),
          h('button', { type: 'button', disabled: !can || !svc.running, onclick: e => job(e, 'restart', 'Restart') }, 'Restart'),
          h('button', { type: 'button', class: 'danger', disabled: !can || !svc.running, onclick: e => job(e, 'stop', 'Stop') }, 'Stop'))]
        : [h('p', {}, server ? `Etherwaver is running (pid ${server.pid}) under the desktop panel or the system service manager.`
          : 'Etherwaver is not running, or the web panel cannot reach it.'),
        h('p', { class: 'limitation' }, 'Starting and stopping is done where Etherwaver is managed: the desktop panel (Start/Stop) or the system service. ',
          'Enable "[service] manage = true" in web.ini to let the web panel run Etherwaver on a headless computer.')],
      h('div', { class: 'btn-row', style: 'margin-top:.75rem' },
        h('button', { type: 'button', disabled: !ctx.can('config:apply') || !server,
          onclick: e => runJob(ctx, e.currentTarget, 'config/apply', 'Apply configuration') }, 'Apply saved configuration to the running server')),
      h('p', { class: 'hint' }, 'Updates of Etherwaver are installed with the system package manager or installer; settings and accounts are kept.'));
  }

  function drawBackup() {
    const fileInput = h('input', { type: 'file', accept: 'application/json,.json', id: 'import-file' });
    const keepName = checkbox({ label: 'Also import the screen name (only when restoring this same computer)', name: 'includeScreenName' });
    const importBtn = h('button', { type: 'button', disabled: !canWrite, onclick: e => busy(e.currentTarget, async () => {
      const f = fileInput.files[0];
      if (!f) { toast('Choose an exported settings file first.', 'warn'); return; }
      let data;
      try { data = JSON.parse(await f.text()); } catch { toast('The file is not valid JSON.', 'danger'); return; }
      if (!(await confirmDialog('Replace the configuration?', 'All settings, screens and hotkeys are replaced by the file contents.', 'Import', true))) return;
      try {
        const [g, l] = await Promise.all([ctx.api.get('config/general'), ctx.api.get('config/layout')]);
        await ctx.api.post('config/import', { ...data, includeScreenName: keepName.querySelector('input').checked,
          expectedEtags: { general: g.etag, layout: l.etag } });
        toast('Configuration imported.');
        editor.reload(true);
      } catch (err) {
        const details = Array.isArray(err.details) ? ': ' + err.details.map(d => `${d.field} ${d.message}`).join('; ') : '';
        toast(errorText(err) + details, 'danger', 12000);
      }
    }) }, 'Import');
    clear(backupHost, h('h2', {}, 'Import and export'),
      h('div', { class: 'btn-row' },
        h('a', { class: 'btn', href: 'api/v1/config/export?format=json', download: 'etherwaver-settings.json' }, 'Export settings (JSON)'),
        h('a', { class: 'btn', href: 'api/v1/config/export?format=conf', download: 'etherwaver.conf' }, 'Export server .conf'),
        h('a', { class: 'btn', href: 'api/v1/config/export?format=layout', download: 'etherwaver-layout.json' }, 'Export layout file')),
      canWrite ? h('div', { style: 'margin-top:1rem' }, field({ label: 'Settings file to import', input: fileInput }), keepName, importBtn) : null,
      h('p', { class: 'limitation' }, 'Import accepts files exported by the web panel. Importing a raw .conf file is not supported; ',
        'use "Configuration source: external file" to run the server from an existing .conf.'));
  }

  const offs = [ctx.live.on('service.state', async () => { await ctx.refreshStatus(); drawService(); }),
    ctx.live.on('server.status', () => drawService())];
  editor.reload(false);
  drawService();
  drawBackup();
  return () => { editor.destroy(); offs.forEach(f => f()); };
}

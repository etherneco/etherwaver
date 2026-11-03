import { h, clear, banner, errorText, busy, toast, showFieldErrors } from '../ui.js';

export function pageHead(title, subtitle, ...actions) {
  return h('div', { class: 'page-head' },
    h('div', {}, h('h1', {}, title), subtitle ? h('p', { class: 'muted', style: 'margin:0' }, subtitle) : null),
    actions.length ? h('div', { class: 'actions' }, actions) : null);
}

export function loading(text = 'Loading…') {
  return h('div', { class: 'loading', role: 'status' }, h('div', { class: 'skeleton', style: 'width:40%' }),
    h('div', { class: 'skeleton', style: 'width:70%' }), h('span', { class: 'sr-only' }, text));
}

export function errorBlock(err, retry) {
  return banner('danger', err && err.isBackendDown ? 'Etherwaver server is not reachable' : 'Could not load data',
    errorText(err), retry ? h('button', { type: 'button', onclick: retry }, 'Retry') : null);
}

// ETag-guarded editor for one configuration resource ("general" or "layout").
//
//   load()           -> { data, etag }
//   save(data, etag) -> { data, etag }  (backend validates and returns the
//                                         accepted configuration)
//   render(data)     -> builds the form; call editor.markDirty() on change
//   collect()        -> returns the draft to send
export class ConfigEditor {
  constructor(ctx, { resource, root, load, save, render, collect, readOnly }) {
    Object.assign(this, { ctx, resource, root, load, save, renderForm: render, collect, readOnly });
    this.etag = null;
    this.data = null;
    this.dirty = false;
    this.notice = h('div', { 'aria-live': 'polite' });
    this.formHost = h('div');
    this.status = h('span', { class: 'muted small', 'aria-live': 'polite' });
    this.saveBtn = h('button', { class: 'primary', type: 'submit', disabled: true }, 'Save');
    this.resetBtn = h('button', { type: 'button', disabled: true, onclick: () => this.reload(true) }, 'Discard changes');
    this.extraActions = h('span', { class: 'btn-row' });
    this.form = h('form', { novalidate: true, onsubmit: e => { e.preventDefault(); this.submit(); },
      oninput: () => this.markDirty(), onchange: () => this.markDirty() },
      this.formHost,
      readOnly ? null : h('div', { class: 'form-actions' }, this.saveBtn, this.resetBtn, this.status, this.extraActions));
    clear(root, this.notice, this.form);
    this.off = ctx.live.on('config.changed', d => this.onRemoteChange(d));
    this.offResync = ctx.live.on('resync', () => { if (!this.dirty) this.reload(false); });
  }

  destroy() { this.off(); this.offResync(); this.ctx.setDirty(this.resource, false); }

  markDirty() {
    if (this.readOnly || !this.data) return;
    if (!this.dirty) {
      this.dirty = true;
      this.ctx.setDirty(this.resource, true);
      this.saveBtn.disabled = false;
      this.resetBtn.disabled = false;
      this.status.textContent = 'Unsaved changes';
    }
  }

  setClean() {
    this.dirty = false;
    this.ctx.setDirty(this.resource, false);
    this.saveBtn.disabled = true;
    this.resetBtn.disabled = true;
  }

  async reload(userRequested) {
    // discarding is immediate; the fresh values follow from the server
    if (userRequested) this.setClean();
    clear(this.notice);
    if (!this.data) clear(this.formHost, loading());
    try {
      const { data, etag } = await this.load();
      this.data = data; this.etag = etag;
      this.setClean();
      clear(this.formHost, this.renderForm(data));
      this.status.textContent = userRequested ? 'Showing the current configuration' : '';
    } catch (e) {
      clear(this.formHost, errorBlock(e, () => this.reload(true)));
    }
  }

  onRemoteChange(d) {
    if (!d.resources || !d.resources.includes(this.resource)) return;
    if (d.etags && d.etags[this.resource] === this.etag) return; // our own save
    if (!this.dirty) {
      this.reload(false);
      toast(`Configuration updated ${d.by ? 'by ' + d.by : 'in another panel'}; showing the new values.`, 'warn');
      return;
    }
    clear(this.notice, banner('warn', 'Changed in another panel',
      'Someone changed this configuration (desktop panel or another browser) after you started editing. ' +
      'Saving now will be refused unless you choose to overwrite.',
      h('button', { type: 'button', onclick: () => this.reload(true) }, 'Load current values')));
  }

  async submit(force = false) {
    if (this.readOnly) return;
    const draft = this.collect();
    if (!draft) return;
    await busy(this.saveBtn, async () => {
      this.status.textContent = 'Saving…';
      clear(this.notice);
      try {
        const { data, etag } = await this.save(draft, this.etag);
        this.data = data; this.etag = etag;
        this.setClean();
        clear(this.formHost, this.renderForm(data));
        this.status.textContent = 'Saved';
        toast('Configuration saved. It is now visible in the desktop panel as well.', 'ok');
        this.onSaved && this.onSaved(data);
      } catch (e) {
        this.status.textContent = '';
        if (e.isConflict) {
          const current = e.details && e.details.etag;
          clear(this.notice, banner('warn', 'Not saved: the configuration was changed elsewhere',
            'Another panel saved a newer version. Load it to review, or overwrite it with your values.',
            h('button', { type: 'button', onclick: () => this.reload(true) }, 'Load current values'),
            h('button', { type: 'button', class: 'danger', onclick: () => { this.etag = current; this.submit(true); } },
              'Overwrite with my values')));
        } else if (e.status === 422) {
          const fields = showFieldErrors(this.form, e);
          const details = Array.isArray(e.details) ? e.details.map(d => `${d.field}: ${d.message}`) : [];
          clear(this.notice, banner('danger', 'Not saved: please correct the highlighted values',
            fields.length ? '' : details.join('; ') || errorText(e)));
        } else {
          clear(this.notice, banner('danger', 'Not saved', errorText(e)));
        }
      }
    });
    // busy() restores the pre-click state; a clean form has nothing to save
    this.saveBtn.disabled = !this.dirty;
  }
}

export function serverStateNote(ctx) {
  const st = ctx.status();
  if (!st || !st.stale) return null;
  return banner('warn', 'Showing last known state', 'The Etherwaver server is not connected; device status below is not current.');
}

export async function runJob(ctx, button, path, label) {
  const { waitForJob } = await import('../api.js');
  return busy(button, async () => {
    let r;
    try {
      r = await ctx.api.post(path, {});
    } catch (e) {
      toast(`${label}: ${errorText(e)}`, 'danger', 8000);
      return null;
    }
    const job = await waitForJob(r.job, ctx.live, j => { button.title = `${j.progress}% ${j.message || ''}`; });
    button.title = '';
    if (job.state === 'succeeded') toast(`${label}: done`, 'ok');
    else if (job.state === 'uncertain') toast(`${label}: result unknown — ${job.error ? job.error.message : ''}. Check the status.`, 'warn', 10000);
    else toast(`${label}: ${job.error ? job.error.message : job.state}`, 'danger', 10000);
    return job;
  });
}

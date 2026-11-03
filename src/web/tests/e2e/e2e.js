// Browser end-to-end scenarios for the Etherwaver web panel.
// Run through run_e2e.py, which prepares the environment variables.
const { chromium } = require('playwright');
const { spawn, execFileSync } = require('child_process');
const path = require('path');

const BASE = process.env.BASE_URL;
const PASSWORD = process.env.PASSWORD;
const SHOTS = process.env.SHOTS;
const results = [];
let failed = 0;

async function step(name, fn) {
  const t = Date.now();
  try {
    await fn();
    results.push(`PASS ${name} (${Date.now() - t} ms)`);
    console.log(`PASS ${name}`);
  } catch (e) {
    failed++;
    results.push(`FAIL ${name}: ${e.message.split('\n')[0]}`);
    if (SHOTS && global.currentPage) {
      try { await global.currentPage.screenshot({ path: path.join(SHOTS, 'fail-' + name.replace(/\W+/g, '_').slice(0, 40) + '.png') }); } catch { /* ignore */ }
    }
    console.log(`FAIL ${name}\n${e.stack}`);
  }
}

function assert(cond, msg) { if (!cond) throw new Error(msg); }
async function until(fn, timeout = 10000, msg = 'condition') {
  const end = Date.now() + timeout;
  let last;
  while (Date.now() < end) {
    try { last = await fn(); if (last) return last; } catch (e) { last = e; }
    await new Promise(r => setTimeout(r, 150));
  }
  throw new Error(`timed out waiting for ${msg} (${last})`);
}
async function shot(page, name) { if (SHOTS) await page.screenshot({ path: path.join(SHOTS, name + '.png'), fullPage: false }); }

function desktop(...args) {
  // the real desktop panel classes (AppConfig/ServerConfig) via tests/qt_compat
  const out = execFileSync(process.env.HARNESS, [args[0], process.env.SETTINGS_FILE, ...args.slice(1)],
    { env: { ...process.env, QT_QPA_PLATFORM: 'offscreen' }, encoding: 'utf8' });
  return args[0] === 'dump' ? JSON.parse(out) : null;
}

async function login(context, user) {
  const page = await context.newPage();
  await page.goto(BASE);
  await page.fill('input[name=username]', user);
  await page.fill('input[name=password]', PASSWORD);
  await page.click('button[type=submit]');
  await page.waitForSelector('.sidebar');
  return page;
}

async function apiGet(page, p) {
  return page.evaluate(async u => { const r = await fetch(u, { credentials: 'same-origin' }); return { status: r.status, body: await r.json() }; }, 'api/v1/' + p);
}

async function toastText(page, re, timeout = 15000) {
  return until(async () => {
    const texts = await page.locator('.toast').allInnerTexts();
    return texts.find(t => re.test(t));
  }, timeout, `toast ${re}`);
}

(async () => {
  const browser = await chromium.launch();
  const context = await browser.newContext({ viewport: { width: 1366, height: 900 } });
  let page, client;
  const consoleErrors = [];

  await step('1a unauthenticated: login page and API 401', async () => {
    const ctx = await browser.newContext();
    const p = await ctx.newPage();
    await p.goto(BASE);
    await p.waitForSelector('input[name=username]');
    const r = await p.evaluate(async () => (await fetch('api/v1/status')).status);
    assert(r === 401, 'status without session should be 401, got ' + r);
    await p.fill('input[name=username]', 'admin');
    await p.fill('input[name=password]', 'wrong-password');
    await p.click('button[type=submit]');
    await p.waitForSelector('text=Sign-in failed');
    await ctx.close();
  });

  await step('1b admin login', async () => {
    page = await login(context, 'admin');
    global.currentPage = page;
    page.on('pageerror', e => consoleErrors.push(e.message));
    page.on('console', m => { if (m.type() === 'error' && !/401|409|412|503|504/.test(m.text())) consoleErrors.push(m.text()); });
    await page.waitForSelector('.conn.ok, .conn.danger');
  });

  await step('14a configure as server through the web UI (settings form)', async () => {
    await page.goto(BASE + '#/settings');
    await page.waitForSelector('select[name=mode]');
    await page.selectOption('select[name=mode]', 'server');
    await page.fill('input[name=screenName]', 'e2e-srv');
    await page.fill('input[name=port]', process.env.KVM_PORT);
    await page.fill('input[name=interface]', '0.0.0.0');
    await page.uncheck('input[name=cryptoEnabled]');
    await page.click('form button[type=submit]');
    await toastText(page, /Configuration saved/);
    const g = (await apiGet(page, 'config/general')).body.general;
    assert(g.mode === 'server' && g.screenName === 'e2e-srv' && g.cryptoEnabled === false, JSON.stringify(g));
  });

  await step('unsaved-changes guard on navigation', async () => {
    await page.fill('input[name=uhidName]', 'Changed but not saved');
    await page.click('.sidebar a[href="#/devices"]');
    await page.waitForSelector('dialog >> text=Leave without saving?');
    await page.click('dialog button:has-text("Cancel")');
    assert(page.url().endsWith('#/settings'), 'should stay on settings, at ' + page.url());
    await page.click('form button:has-text("Discard changes")');
  });

  await step('add a client screen in the layout editor; double submit sends one request', async () => {
    await page.goto(BASE + '#/layout');
    await page.waitForSelector('.screen-card');
    await page.click('button:has-text("Add screen")');
    const name = page.locator('input[name=name]');
    await name.fill('e2e-cli');
    await name.press('Tab');
    let puts = 0;
    const onReq = r => { if (r.method() === 'PUT' && r.url().includes('config/layout')) puts++; };
    page.on('request', onReq);
    // two clicks in the same task, i.e. faster than any network round trip
    await page.evaluate(() => { const b = document.querySelector('form button[type=submit]'); b.click(); b.click(); });
    await toastText(page, /Configuration saved/);
    page.off('request', onReq);
    assert(puts === 1, `expected one PUT, saw ${puts}`);
    const lay = (await apiGet(page, 'config/layout')).body.layout;
    assert(lay.screens.map(s => s.name).includes('e2e-cli-1'), JSON.stringify(lay.screens.map(s => s.name)));
    await shot(page, 'layout');
  });

  await step('12 long-running operation: start the service (job with progress)', async () => {
    await page.goto(BASE + '#/settings');
    const start = page.locator('section:has(h2:text("Service")) button:text-is("Start")');
    await start.waitFor();
    await start.click();
    await toastText(page, /Start: done/, 30000);
    const st = (await apiGet(page, 'status')).body;
    assert(st.server && st.service.running && st.server.pid === st.service.pid, JSON.stringify(st.service));
    const jobs = (await apiGet(page, 'jobs')).body.jobs;
    assert(jobs.some(j => j.kind === 'service.start' && j.state === 'succeeded'), 'job not recorded');
  });

  await step('2 read devices; client connection appears live (no reload)', async () => {
    await page.goto(BASE + '#/devices');
    const row = page.locator('tr:has(a:text-is("e2e-cli-1"))');
    await row.waitFor();
    assert(/disconnected/.test(await row.innerText()), 'client should be disconnected first');
    client = spawn(path.join(process.env.BIN_DIR, 'waverc'),
      ['-f', '--no-tray', '--debug', 'INFO', '--name', 'e2e-cli', '--disable-crypto', '127.0.0.1:' + process.env.KVM_PORT],
      { env: { ...process.env, DISPLAY: process.env.CLIENT_DISPLAY }, stdio: 'ignore' });
    await until(async () => /connected/.test(await row.locator('td').nth(1).innerText()) &&
      !/disconnected/.test(await row.locator('td').nth(1).innerText()), 15000, 'client connected badge');
    await shot(page, 'devices');
  });

  await step('13 KVM control: give control to the client, lock and release the cursor', async () => {
    await page.goto(BASE + '#/sessions');
    const row = page.locator('tr:has(td:text-is("e2e-cli-1"))');
    await row.locator('button:has-text("Give control")').click();
    await toastText(page, /Control switched to e2e-cli-1/);
    await until(async () => /has control/.test(await row.innerText()), 8000, 'client has control');
    await page.click('button:has-text("Lock cursor to this screen")');
    await page.waitForSelector('text=locked to screen');
    assert((await apiGet(page, 'status')).body.server.lockedToScreen === true, 'lock not applied');
    await page.click('button:has-text("Release cursor lock")');
    await until(async () => (await apiGet(page, 'status')).body.server.lockedToScreen === false, 5000, 'unlock');
    await shot(page, 'sessions');
  });

  await step('8 two tabs: second tab updates live; parallel requests from both', async () => {
    const page2 = await context.newPage();
    await page2.goto(BASE + '#/');
    await page2.waitForSelector('#c-active');
    await page.goto(BASE + '#/sessions');
    await page.locator('tr:has(td:text-is("e2e-srv-1")) button:has-text("Give control")').click();
    await until(async () => (await page2.locator('section:has(#c-active) .stat').innerText()) === 'e2e-srv-1', 8000, 'tab 2 shows new active screen');
    const tokens = [];
    for (const p of [page, page2]) tokens.push(await p.evaluate(async () => (await (await fetch('api/v1/session')).json()).csrfToken));
    const calls = [];
    [page, page2].forEach((p, n) => [0, 1, 2].forEach(i => calls.push(p.evaluate(async ([name, token]) => {
      const r = await fetch(`api/v1/devices/${name}/activate`, { method: 'POST', headers: { 'X-CSRF-Token': token, 'Content-Type': 'application/json' }, body: '{}' });
      return r.status;
    }, [i % 2 ? 'e2e-cli-1' : 'e2e-srv-1', tokens[n]]))));
    const codes = await Promise.all(calls);
    assert(codes.every(c => c === 200), 'parallel requests: ' + codes.join(','));
    await page2.close();
  });

  await step('3 change in the web panel is read by the desktop panel classes', async () => {
    await page.goto(BASE + '#/layout');
    await page.check('input[name="options.heartbeat.enabled"]');
    await page.fill('input[name="options.heartbeat.ms"]', '4000');
    await page.click('form button[type=submit]');
    await toastText(page, /Configuration saved/);
    const out = desktop('dump');
    assert(out.heartbeat === 4000, 'desktop sees heartbeat ' + out.heartbeat);
    assert(out.screens.some(s => s.name === 'e2e-cli-1'), 'desktop sees the client screen');
    assert(out.general.screenName === 'e2e-srv', 'desktop sees screen name ' + out.general.screenName);
  });

  await step('apply the saved configuration to the running server (job)', async () => {
    await page.click('button:has-text("Apply to running server")');
    await toastText(page, /Apply configuration: done/, 20000);
    const st = (await apiGet(page, 'status')).body.server;
    assert(st.configuredScreens.includes('e2e-cli-1'), JSON.stringify(st.configuredScreens));
  });

  await step('4 change in the desktop panel updates the open web form', async () => {
    await page.goto(BASE + '#/settings');
    await page.waitForSelector('select[name=logLevel]');
    desktop('set-log-level', '1'); // SettingsDialog::accept -> WARNING
    await until(async () => (await page.inputValue('select[name=logLevel]')) === 'WARNING', 10000, 'log level WARNING in web form');
    await toastText(page, /updated in another panel/);
  });

  await step('5 concurrent edit conflict is detected and resolved', async () => {
    await page.fill('input[name=uhidName]', 'Edited in browser');
    desktop('set-log-level', '2'); // NOTE, saved while the web form is dirty
    await page.waitForSelector('text=Changed in another panel');
    await page.click('form button[type=submit]');
    await page.waitForSelector('text=Not saved: the configuration was changed elsewhere');
    assert((await page.inputValue('input[name=uhidName]')) === 'Edited in browser', 'draft must be kept');
    await shot(page, 'conflict');
    await page.click('button:has-text("Load current values") >> nth=0');
    await until(async () => (await page.inputValue('select[name=logLevel]')) === 'NOTE', 5000, 'reloaded NOTE');
    const g = (await apiGet(page, 'config/general')).body.general;
    assert(g.logLevel === 'NOTE' && g.uhidName !== 'Edited in browser', 'desktop change must win: ' + JSON.stringify(g));
  });

  await step('7 timeout of a state change is never shown as success', async () => {
    const pid = (await apiGet(page, 'status')).body.server.pid;
    await page.goto(BASE + '#/sessions');
    await page.waitForSelector('tr:has(td:text-is("e2e-cli-1"))');
    process.kill(pid, 'SIGSTOP');
    try {
      await page.locator('tr:has(td:text-is("e2e-cli-1")) button:has-text("Give control")').click();
      const t = await toastText(page, /may or may not have been applied|not reachable|not connected/, 15000);
      const all = (await page.locator('.toast').allInnerTexts()).join('|');
      assert(!/Control switched to e2e-cli-1/.test(all), 'success shown although unconfirmed: ' + all);
      console.log('   toast:', t);
    } finally {
      process.kill(pid, 'SIGCONT');
    }
  });

  await step('6 control connection lost and restored (server crash, auto restart)', async () => {
    await page.goto(BASE + '#/');
    await until(async () => (await apiGet(page, 'status')).body.backend.state === 'connected', 15000, 'connected again after SIGCONT');
    const pid = (await apiGet(page, 'status')).body.server.pid;
    process.kill(pid, 'SIGKILL');
    await page.waitForSelector('text=Etherwaver server is not reachable', { timeout: 10000 });
    await shot(page, 'server-down');
    const stale = (await apiGet(page, 'status')).body;
    assert(stale.stale === true && stale.server === null, 'status must be marked stale');
    await until(async () => { const s = (await apiGet(page, 'status')).body; return s.server && s.server.pid !== pid; }, 20000, 'restarted server');
    await page.waitForSelector('.conn.ok', { timeout: 10000 });
    await until(async () => !(await page.locator('text=Etherwaver server is not reachable').count()), 10000, 'banner cleared');
  });

  await step('1c viewer cannot change anything (UI and API)', async () => {
    const ctx = await browser.newContext();
    const v = await login(ctx, 'viewer');
    await v.goto(BASE + '#/devices');
    await v.waitForSelector('tr:has(a:text-is("e2e-cli-1"))');
    assert(await v.locator('tr:has(a:text-is("e2e-cli-1")) button:has-text("Switch here")').isDisabled(), 'button must be disabled');
    const code = await v.evaluate(async () => {
      const s = await (await fetch('api/v1/session')).json();
      return (await fetch('api/v1/devices/e2e-cli-1/activate', { method: 'POST', headers: { 'X-CSRF-Token': s.csrfToken } })).status;
    });
    assert(code === 403, 'viewer switch should be 403, got ' + code);
    assert(!(await v.locator('.sidebar a[href="#/users"]').count()), 'viewer must not see Users');
    await ctx.close();
  });

  await step('logs, diagnostics and audit pages show real data', async () => {
    await page.goto(BASE + '#/logs');
    await until(async () => (await page.locator('.log div').count()) > 3, 8000, 'log lines');
    await page.goto(BASE + '#/diagnostics');
    await page.waitForSelector('text=Control connection');
    await page.waitForSelector('.badge:text-is("connected")');
    await page.goto(BASE + '#/activity');
    await page.waitForSelector('td:text-is("control.switch")');
    await page.waitForSelector('td:text-is("config.layout.update")');
  });

  await step('tablet layout, dark theme and keyboard navigation', async () => {
    const ctx = await browser.newContext({ viewport: { width: 820, height: 1180 }, hasTouch: true });
    const t = await login(ctx, 'admin');
    for (const r of ['', 'devices', 'layout', 'settings']) {
      await t.goto(BASE + '#/' + r);
      await t.waitForTimeout(400);
      const overflow = await t.evaluate(() => document.documentElement.scrollWidth - window.innerWidth);
      assert(overflow <= 0, `horizontal overflow ${overflow}px on #/${r}`);
    }
    await shot(t, 'tablet-settings');
    await t.goto(BASE + '#/');
    await t.click('.menu-toggle');
    await t.waitForSelector('.sidebar.open');
    await shot(t, 'tablet-menu');
    await t.click('.sidebar a[href="#/devices"]');
    assert(!(await t.locator('.sidebar.open').count()), 'menu closes after navigation');
    // theme: system -> light -> dark
    await t.click('#theme-btn');
    await t.click('#theme-btn');
    assert((await t.evaluate(() => document.documentElement.dataset.theme)) === 'dark', 'dark theme');
    await t.waitForTimeout(300);
    await shot(t, 'tablet-dark');
    await t.reload();
    await t.waitForSelector('.sidebar');
    assert((await t.evaluate(() => document.documentElement.dataset.theme)) === 'dark', 'theme remembered');
    await t.keyboard.press('Tab');
    assert((await t.evaluate(() => document.activeElement.className)) === 'skip-link', 'first Tab reaches skip link');
    await t.keyboard.press('Enter');
    assert((await t.evaluate(() => document.activeElement.id)) === 'main', 'skip link moves focus to content');
    await ctx.close();
  });

  await step('stop the service from the UI', async () => {
    await page.goto(BASE + '#/settings');
    await page.locator('section:has(h2:text("Service")) button:text-is("Stop")').click();
    await page.click('dialog button:has-text("Stop")');
    await toastText(page, /Stop: done/, 20000);
    await page.waitForSelector('text=Etherwaver server is not reachable', { timeout: 10000 });
  });

  await step('no unexpected browser errors', async () => {
    assert(consoleErrors.length === 0, consoleErrors.join(' | '));
  });

  if (client) client.kill();
  await browser.close();
  console.log('\n' + results.join('\n'));
  console.log(failed ? `\n${failed} scenario(s) failed` : '\nall scenarios passed');
  process.exit(failed ? 1 : 0);
})();

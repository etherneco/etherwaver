# Etherwaver web panel (`waver-web`)

A browser interface that manages Etherwaver to the same extent as the desktop
panel (`waver`): role and network settings, screens and layout, hotkeys,
server options, trusted certificates, switching control between computers,
live status, logs and diagnostics. Both panels work at the same time on the
same configuration.

* [Coverage of desktop functions](coverage.md)
* [HTTP API (OpenAPI 3)](openapi.yaml)
* [Etherwaver Control Protocol](control-protocol.md) (server side)

## Architecture

```
 browser (HTML/CSS/ES modules, no framework)
    │  HTTP/1.1 JSON API /api/v1 + Server-Sent Events /api/v1/events
    ▼
 waver-web (Python 3 standard library only)                   desktop panel (Qt)
    ├─ API, sessions, CSRF, roles, audit, jobs                       │
    ├─ shared configuration store ── QSettings INI file ◀──────────────┤
    │    (Qt-compatible encoding + QLockFile, ETag per resource)       │  file watcher,
    ├─ ECP adapter ── TCP 127.0.0.1:24803, token auth ──┐              │  conflict prompt
    └─ optional supervisor (starts wavers/waverc)        ▼              │
                                                 wavers (C++ server) ◀─┘ (started by either panel)
```

* **Frontend** (`src/web/frontend/src`) – talks only to the web backend.
  It never decides what is valid; it shows what the backend accepted.
* **HTTP API** (`src/web/waver_web/app.py`) – versioned under `/api/v1`, GET
  never changes state, consistent error format with request id.
* **Adapter** (`waver_web/control_client.py`) – implements the Etherwaver
  Control Protocol: framing independent of TCP segmentation, id correlation,
  events vs responses, timeouts, bounded concurrency, reconnection; state
  changes are never resent automatically ("outcome unknown" instead).
* **Server side** (`src/lib/server/ControlServer.*`) – the control endpoint
  added to `wavers`; commands run on the server's event loop.
* **Live updates** – SSE (one direction is enough: all changes go through the
  HTTP API). Event ids `<epoch>-<seq>`; reconnecting browsers get the missed
  events or a `resync` that makes them reload all state.
* **Shared configuration** – the desktop panel's own QSettings file
  (`~/.config/Debauchee/EtherWaver.conf`) and `~/.local/share/etherwaver/gui-state.ini`.
  The web backend reads and writes them byte-compatibly with Qt (verified
  against Qt 5) and takes Qt's lock file. The generated server `.conf` is
  byte-identical to what the desktop panel produces (verified with the real
  `ServerConfig` code). The desktop panel now watches the file, reloads
  changes made by the web panel, and asks before overwriting them.

## Install

The panel is built and installed with Etherwaver (CMake option
`BARRIER_BUILD_WEB`, on by default on Linux/macOS). It needs Python ≥ 3.9 and
nothing else.

```bash
cmake -S . -B build && cmake --build build -j
sudo cmake --install build            # or the .deb/.rpm built from debian/ and dist/rpm
```

Installed files: `bin/waver-web`, `share/etherwaver/web/` (code + built
frontend), `lib/systemd/user/waver-web.service`,
`share/doc/etherwaver/web.ini.example`, `share/doc/etherwaver/nginx.conf.example`.

From a source checkout without installing:

```bash
cd src/web
python3 -m waver_web.build          # builds the frontend into waver_web/static
python3 -m waver_web --help
```

## First start

Run everything as the user that uses Etherwaver (the desktop panel's
configuration and the control token are per user).

1. Create the first administrator (there is no default account or password):

   ```bash
   waver-web user add alice --role admin
   ```

2. Start the panel:

   ```bash
   systemctl --user enable --now waver-web      # or: waver-web serve
   ```

3. Start Etherwaver as a server as usual (desktop panel *Start*, or a
   service). `wavers` opens the control endpoint `127.0.0.1:24803` and
   creates `~/.local/share/etherwaver/control-token` (mode 0600).

4. Open **http://127.0.0.1:24880/** and sign in.

`waver-web check` prints the effective paths and settings.

### Headless computers

Set `[service] manage = true` (and `display = :0` if needed) in
`~/.config/etherwaver/web.ini`. The panel can then start, stop and restart
`wavers`/`waverc` itself with the same command line the desktop panel uses,
and restarts it after a crash. Run `loginctl enable-linger <user>` so the user
unit starts at boot. When Etherwaver is already started by the desktop panel
or another service manager, the web panel detects the foreign process and
refuses to start/stop it.

## Configuration (`web.ini`)

See `web.ini.example`. Main keys:

| Key | Default | Meaning |
|---|---|---|
| `[web] listen` | `127.0.0.1:24880` | address of the panel |
| `[web] base_path` | – | URL prefix behind a reverse proxy |
| `[web] trusted_proxies` | – | proxies allowed to set `X-Forwarded-*` |
| `[web] cookie_secure` | `auto` | Secure cookies when served over HTTPS |
| `[web] session_idle_minutes` / `session_max_hours` | 30 / 12 | session expiry |
| `[etherwaver] control_address` | `127.0.0.1:24803` | `wavers --control-address` |
| `[etherwaver] request_timeout` | 5 | seconds before a command is reported as timed out / outcome unknown |
| `[service] manage`, `bin_dir`, `display` | false | let the panel run Etherwaver |

Environment variables `WAVER_WEB_<SECTION>_<KEY>` override the file.

`wavers` options: `--control-address host:port` (`none` disables the
endpoint), `--control-token-file path`.

## Reverse proxy and HTTPS

The panel is not meant to be exposed to the Internet. To reach it from other
computers in a trusted network, keep it on localhost and put an HTTPS reverse
proxy in front (`nginx.conf.example`): set `base_path`, `trusted_proxies =
127.0.0.1`, keep `cookie_secure = auto`. SSE needs `proxy_buffering off`.

## Security model

* Accounts in `~/.local/share/etherwaver/web/users.json` (0600), passwords
  hashed with scrypt. Roles: **viewer** (read), **operator** (switch control,
  lock cursor, apply configuration, start/stop the managed service, logs,
  diagnostics), **admin** (configuration, certificates, import, users, audit).
  Every check is done by the backend; the UI only hides what is not allowed.
* Session cookie `ew_session`: HttpOnly, SameSite=Strict, Secure over HTTPS,
  idle and absolute expiry. State-changing requests need the `X-CSRF-Token`
  header and a same-origin `Origin`/`Referer`. Sign-in is rate-limited.
* Strict Content-Security-Policy (no inline scripts or styles), `nosniff`,
  `frame-ancestors 'none'`, `Referrer-Policy: no-referrer`, HSTS over HTTPS.
* The control token never leaves the backend; no secrets in API responses.
* Audit log `~/.local/share/etherwaver/web/audit.log` (JSON lines): time, user,
  role, IP, action, resource, result (success / failure / denied /
  uncertain), request id. Passwords, tokens, cookies and keystrokes are never
  written.

## Concurrent editing

Each configuration resource (`general`, `layout`) has an ETag computed from
its values. Saves must send `If-Match`; if the desktop panel or another
browser saved in the meantime the backend answers 412 with the current values
and the form offers "Load current values" or an explicit "Overwrite". An open
form that is not being edited refreshes automatically. The desktop panel
compares the file with what it loaded when its dialog opened and asks before
overwriting a change made in the browser.

## Updates

Etherwaver has no self-updater (the desktop version check is disabled in the
code). The web panel is part of the package: upgrading the package replaces
`share/etherwaver/web` and the launcher only. `web.ini`, accounts, the audit
log, the control token and the shared Etherwaver configuration live in the
user's home directory and are never touched by an upgrade. Restart with
`systemctl --user restart waver-web`; open browser sessions are signed out
(sessions are kept in memory).

## Tests

```bash
cd src/web
python3 -m unittest discover -s tests            # unit, API, adapter, compat, real-server
WAVER_BIN_DIR=../../build/bin python3 -m unittest tests.test_real_server
NODE_PATH=$(npm root -g) python3 tests/e2e/run_e2e.py --bin-dir ../../build/bin \
    --harness /tmp/waver-web-qt-harness/desktop_config
```

* `test_control_client` – framing (single bytes, random chunks, coalesced
  messages), events between responses, concurrency, timeouts, uncertain
  outcomes, no resend after disconnect, reconnection, queue limits.
* `test_api` – authentication, roles, CSRF/Origin, cookies behind a proxy,
  ETag conflicts, validation, error format, GET without side effects, jobs,
  backend down/up, parallel tabs, SSE replay/resync, audit without secrets.
* `test_desktop_compat` – builds the real desktop classes
  (`tests/qt_compat`) and checks both directions and conflict detection.
* `test_real_server` – the C++ control endpoint with real `wavers`/`waverc`
  under Xvfb.
* `e2e/` – Playwright: the full path browser → API → adapter → `wavers`, with
  a real `waverc` client, desktop-side edits, server crash and freeze, two
  tabs, tablet size, dark theme and keyboard navigation.

`ctest` runs the Python suite when `BARRIER_BUILD_TESTS` is on.

## Limitations

* No remote screen view, audio or typing in the browser: Etherwaver shares
  the physical keyboard and mouse of the server and has no media stream; the
  web panel moves control between computers instead.
* Bonjour discovery, certificate generation, the desktop "Screen info" probe
  and window/tray behaviour exist only in the desktop panel (settings for them
  are still editable and marked as such).
* Start/stop from the browser only for processes the web panel manages.
* Not packaged for Windows (the panel itself runs where Python 3 runs, but
  process management uses POSIX process groups).
* Importing raw `.conf` files is not supported; use "external configuration file".

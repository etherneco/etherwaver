# Desktop panel → web panel coverage

Every function of the desktop panel (`src/gui`), including menus, the tray menu,
dialogs and advanced settings, with its web counterpart.

Status: **Done** – implemented and verified; **Partial** – implemented with a
stated difference; **N/A** – not applicable in a browser, with the reason;
**Web only** – new in the web panel.

Verification keys: `E2E n` = browser scenario in `src/web/tests/e2e/e2e.js`
against the real `wavers`/`waverc`; `API` = `tests/test_api.py`; `ADP` =
`tests/test_control_client.py`; `REAL` = `tests/test_real_server.py`;
`DESK` = `tests/test_desktop_compat.py` (real desktop classes); `MAN` = checked by hand.

## Main window

| Desktop function | Web screen | API operation | Status | Verification |
|---|---|---|---|---|
| Server / client mode (group boxes) | Settings › Role of this computer | `PATCH /config/general` `mode` | Done (written to the settings file and `gui-state.ini` like `MainWindow::saveSettings`) | E2E 14a, API |
| Server: "Configure Server…" (internal configuration) | Screens & layout, Hotkeys | `GET/PUT /config/layout` | Done | E2E 3, DESK |
| Server: use external configuration file + Browse | Settings › Configuration source, External configuration file | `PATCH /config/general` `configSource`, `externalConfigFile` | Partial – path is typed; a browser cannot browse the server's file system | API |
| Client: server host name | Settings › Server host name | `PATCH /config/general` `serverHostname` | Done | API |
| Client: auto config (Bonjour) and discovered servers list | Settings › "Find the server automatically" | `PATCH /config/general` `autoConfig` | Partial – setting is shared; discovery itself runs only in the desktop panel (limitation shown next to the option). A client started by the web panel uses the host name | MAN |
| Screen name label | Dashboard › Server › Screen | `GET /status` | Done | E2E 12 |
| IP addresses label | Dashboard › Server › IP addresses | `GET /meta` `platform.addresses` | Done | MAN |
| SSL fingerprint (SHA-1, SHA-256, randomart) | Security › This computer | `GET /security/fingerprints` | Partial – SHA-256 and SHA-1 shown, randomart picture not drawn | API |
| Start / Stop button, tray Start/Stop | Settings › Service (Start, Restart, Stop) | `POST /service/{start,stop,restart}` (job) | Partial – only when `[service] manage = true`; when the desktop panel or a system service owns the process the web panel says so and refuses (409 `not_managed` / `external_owner`) | E2E 12, E2E "stop", API |
| Reload (restart with new configuration) | "Apply to running server" (Dashboard, Screens & layout, Settings) | `POST /config/apply` (job → ECP `config.apply`, no restart needed) | Done | E2E "apply", REAL |
| Status line ("EtherWaver is running/starting/not running") and padlock | Top bar connection badge, Dashboard cards, banner when unreachable | `GET /status`, SSE `server.status`, `backend.state` | Done – encryption state is in Settings › Security | E2E 6 |
| Automatic restart after a crash (`barrierFinished`) | Dashboard / Service | supervisor | Done for processes managed by the web panel | E2E 6 |

## Menus and tray

| Desktop function | Web screen | API operation | Status | Verification |
|---|---|---|---|---|
| EtherWaver › Show Log / tray Show Log (LogWindow) | Logs (live tail, level filter, search, pause, download) | `GET /logs`, SSE `log` | Done | E2E "logs" |
| EtherWaver › Settings (SettingsDialog) | Settings | `GET/PATCH /config/general` | Done – see table below | E2E 4, 5 |
| EtherWaver › Save configuration as… | Settings › Import and export; Screens & layout › Download server .conf | `GET /config/export?format=conf\|layout\|json` | Done (plus JSON export of all settings) | API |
| EtherWaver › Minimize / tray Minimize, Restore | – | – | N/A – window management of the desktop app | – |
| EtherWaver › Quit / tray Quit | Sign out | `DELETE /session` | N/A for quitting; signing out ends the web session | API |
| Debug › Screen info | Diagnostics (runtime layout, UHID status, control connection) | `GET /diagnostics` | Partial – the desktop window queries the local compositor and cursor; the web panel shows the geometry reported by the running server | E2E "diagnostics" |
| Help › About | Diagnostics › Web panel / Control connection (versions) | `GET /meta`, `GET /diagnostics` | Done | MAN |
| Tray icon state | Top bar badge "Live / Reconnecting / server unreachable" | SSE | Done | E2E 6 |

## Settings dialog (`SettingsDialog`)

| Desktop function | Web screen | API operation | Status | Verification |
|---|---|---|---|---|
| Screen name | Settings › Identity and network | `PATCH /config/general` `screenName` | Done | E2E 14a, DESK |
| Port | same | `port` | Done | E2E 14a, DESK |
| Network interface | same | `interface` | Done | E2E 14a |
| Enable SSL encryption | Settings › Security | `cryptoEnabled` | Done | E2E 14a |
| Require client certificates | Settings › Security | `requireClientCertificate` | Done | API |
| Logging level | Settings › Logging | `logLevel` | Done | E2E 4, DESK |
| Log to file, file name, Browse | Settings › Logging | `logToFile`, `logFilename` | Partial – path typed, no file browser | API |
| Language | Settings › Desktop panel behaviour | `language` | Done (affects the desktop panel's language) | API |
| Elevate (Windows) | Settings › Desktop panel behaviour | `elevateMode` | Done – marked "Windows service only" | API |
| Hide on startup, Start EtherWaver when GUI starts, Minimize to tray | Settings › Desktop panel behaviour | `autoHide`, `autoStart`, `minimizeToTray` | Done – marked as desktop-panel behaviour | API |
| Linux UHID backend, UHID device name | Settings › Input | `uhidEnabled`, `uhidName` | Done – disabled with explanation on non-Linux | DESK |

## Server configuration dialog (`ServerConfigDialog`, `LayoutEditorWidget`, `ScreenSettingsDialog`)

| Desktop function | Web screen | API operation | Status | Verification |
|---|---|---|---|---|
| Layout canvas: drag screens | Screens & layout canvas (pointer drag, arrow keys) | `PUT /config/layout` | Done | E2E "add screen", MAN |
| Add Screen | "Add screen" | `PUT /config/layout` or `POST /config/layout/screens` | Done | E2E, API |
| Edit Selected / double click (screen settings) | Screen panel next to the canvas | `PUT /config/layout` | Done | DESK |
| Delete Selected / trash | "Remove screen" (confirmation) | `PUT` or `DELETE /config/layout/screens/{name}` | Done (refused for the server screen and for screens a hotkey switches to) | API |
| Auto Layout | "Auto layout" | `PUT /config/layout` | Done | MAN |
| Zoom − / + | – | – | Partial – fixed 50 % scale; the canvas scrolls | MAN |
| Link handles (custom neighbour links) | "Use custom neighbour links" + left/right/up/down selects | `options.useCustomLinks`, `screens[].links` | Done (selects instead of dragging handles) | DESK |
| Screen name + number suffix | Screen name (a `-1` suffix is added like the GUI) | validation | Done | API |
| Aliases | Aliases | `screens[].aliases` | Done | DESK |
| Modifier key mapping (Shift/Ctrl/Alt/Meta/Super) | Screen panel › Keyboard modifiers | `screens[].modifiers` | Done | DESK |
| Dead corners and corner size (per screen) | Screen panel | `screens[].switchCorners`, `switchCornerSize` | Done | DESK |
| Fixes (Caps/Num/Scroll Lock half duplex, XTest Xinerama, preserve focus) | Screen panel | `screens[].fixes` | Done | DESK |
| Hotkeys: New / Edit / Remove | Hotkeys | `PUT /config/layout` `hotkeys` | Done | DESK |
| Hotkey key capture (`KeySequenceWidget`), mouse buttons | Hotkey dialog: capture box, typed names, mouse button | validation of key names server side | Done | DESK, unit tests |
| Actions (`ActionDialog`): press/release/type keys with target screens, switch to screen, toggle screen, switch in direction, lock cursor, run on release | Hotkey dialog › Actions | `hotkeys[].actions[]` | Done | DESK |
| Advanced: heartbeat, relative mouse moves, screen saver sync, keep Windows server in foreground, switch delay, double tap, dead corners, corner size, ignore auto-config clients, drag and drop, clipboard sharing | Screens & layout › Server options | `options` | Done | E2E 3, DESK |
| OK / Cancel | Save / Discard changes, unsaved-changes guard | `If-Match` | Done – conflict detection with the desktop panel | E2E 5, DESK |

## Other dialogs and background behaviour

| Desktop function | Web screen | API operation | Status | Verification |
|---|---|---|---|---|
| Fingerprint accept dialog (new peer certificate) | Security › Waiting for approval, Trust / Revoke, manual entry | `GET/POST/DELETE /security/fingerprints…` | Done for the server (fingerprints seen in the server log); for a client it needs the client to run under the web panel | API |
| Add client dialog (auto-config client placement) | Devices › refused computers › "Add to layout" | `POST /config/layout/screens` | Done (computers refused by the server are listed with attempts) | API, MAN |
| Setup wizard (first start) | `waver-web user add <name> --role admin` + login screen hint | CLI | Partial – there is no web wizard; the first administrator is created on the computer | E2E 1a |
| Version check / updates (`VersionChecker`) | Settings › Service note | – | N/A – the desktop check is disabled in the code; updates come with the system package, which keeps user configuration | – |
| Windows service mode (IPC daemon, `--ipc`, elevation) | – | – | N/A – web panel is packaged for Linux/macOS; the IPC protocol is Windows-service specific | – |
| Bonjour download/install (Windows) | – | – | N/A | – |

## Control, sessions and KVM

| Desktop function | Web screen | API operation | Status | Verification |
|---|---|---|---|---|
| Choose the controlled computer (server feature, legacy `/set/screen`) | Dashboard › Switch control, Devices › Switch here, Sessions › Give control | `POST /devices/{name}/activate` → ECP `switch` | Done (exclusive: one screen has control) | E2E 13, REAL |
| Lock cursor to screen (Scroll Lock / hotkey) | Sessions / Dashboard button | `POST /control/lock` | Done | E2E 13, REAL |
| Connected clients, connection time, last activity | Devices (search, filter), device details, Sessions | `GET /devices`, `GET /sessions`, SSE | Done | E2E 2 |
| Remote screen image, audio, typing in the browser | Sessions › limitation note | – | N/A – Etherwaver has no video/audio stream and the desktop panel has no remote view; there is no browser input-capture mechanism in the repository to integrate with | – |

## Web only

| Function | Web screen | API operation | Verification |
|---|---|---|---|
| Accounts and roles (viewer / operator / admin) | Users, Your account | `/users…`, `/session/password` | API, E2E 1c |
| Audit trail of administrative operations | Activity & jobs | `GET /audit` | API, E2E |
| Background jobs with progress | Activity & jobs | `GET /jobs…` | API, E2E 12 |
| Health / readiness | – | `/healthz`, `/readyz` | API |

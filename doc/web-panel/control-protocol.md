# Etherwaver Control Protocol (ECP) v1

Local management channel of the Etherwaver server (`wavers`), implemented in
`src/lib/server/ControlServer.{h,cpp}` and used by the web panel backend
(`src/web/waver_web/control_client.py`). Browsers never connect to it.

The older one-shot endpoint on port 24802 (`/set/screen/<name>`, `/get/config`,
`/set/config`) is unchanged, so existing scripts keep working. ECP exists
because that endpoint has no authentication, no request ids, no events and
closes the connection after every request.

## Transport

| Item | Value |
|---|---|
| Socket | TCP, default `127.0.0.1:24803` (`wavers --control-address host:port`, `none` disables it) |
| Framing | one JSON object per line (`\n`), UTF-8, max 1 MiB per line |
| Concurrency | many requests may be outstanding on one connection (max 16); responses carry the request `id` and may arrive in any order |
| Limits | 16 connections; a connection whose unsent output exceeds 8 MiB is closed |
| Threading | network I/O runs on its own thread; commands run on the server's main event loop |

Messages can be split across TCP segments or arrive several per segment;
receivers must buffer until `\n`.

## Session

```
S: {"type":"hello","protocol":"etherwaver-control","version":1,"server":"srv-1","serverVersion":"2.4.0","instance":"40ae…","auth":"token"}
C: {"id":1,"cmd":"auth","token":"<64 hex chars>"}
S: {"type":"response","id":1,"ok":true,"result":{"authenticated":true}}
C: {"id":2,"cmd":"subscribe","log":true}
S: {"type":"response","id":2,"ok":true,"result":{"subscribed":true,"log":true,"seq":17}}
S: {"type":"event","seq":18,"event":"client.connected","data":{"name":"laptop-1"}}
```

The token lives in `<profile>/control-token` (Linux: `~/.local/share/etherwaver/control-token`),
created by `wavers` with mode 0600 on first start (`--control-token-file` overrides the path).
Only the user running Etherwaver can read it. Three wrong tokens close the connection.

## Messages

* Request: `{"id": <int|string>, "cmd": "<name>", ...flat parameters}`
* Response: `{"type":"response","id":…,"ok":true,"result":{…}}` or
  `{"type":"response","id":…,"ok":false,"error":{"code":"…","message":"…"}}`
* Event: `{"type":"event","seq":<n>,"event":"<name>","data":{…}}` – only after `subscribe`.
  `seq` grows by one per event on the server; a gap means events were lost and the
  receiver must re-read the state.

Responses with `"id": null` are connection-level errors (malformed JSON, line too long, too many connections).

## Commands

| Command | Parameters | Result | Errors |
|---|---|---|---|
| `auth` | `token` | `{authenticated}` | `unauthorized` |
| `ping` | – | `{pong:true}` | – |
| `subscribe` | `log` (bool) | `{subscribed, log, seq}` | – |
| `unsubscribe` | – | `{subscribed:false}` | – |
| `status` | – | server name, version, pid, uptime, listen address, config and layout file, `active {client, screen, layoutId}`, `lockedToScreen`, `clients[] {name, primary, active, shape}`, `configuredScreens[]`, `layout[]` | – |
| `layout` | – | `{screens[]}` runtime object layout | – |
| `config.get` | – | `{config, layout, configFile, layoutFile}` | – |
| `config.apply` | `config` (text), `layout` (optional JSON text), `persist` (default true) | `{applied, persisted}` | `bad_request`, `invalid_config`, `missing_primary_screen`, `io_error`, `rejected` |
| `switch` | `screen` | `{active{client, screen}}` | `not_found` (unknown name), `not_connected` |
| `lock` | `mode`: `on`, `off`, `toggle` | `{locked}` | `bad_request` |

Unknown commands return `unknown_command`; unauthenticated commands `unauthorized`;
more than 16 outstanding requests `busy`.

## Events

| Event | Data |
|---|---|
| `client.connected` | `{name}` |
| `client.disconnected` | `{name}` |
| `client.rejected` | `{name, reason: unknown_screen \| already_connected}` |
| `screen.active` | `{client, screen, layoutId, previous}` – the controlled screen changed |
| `cursor.lock` | `{locked}` |
| `config.applied` | `{persisted}` |
| `server.stopping` | `{}` |
| `log` | `{level, message}` – only with `subscribe log:true` |

## Client rules (as implemented by the web backend)

* One long-lived connection, reconnected with jittered exponential backoff (0.5 s → 10 s).
* Requests are correlated by `id`; late responses (after a timeout) are dropped and counted.
* Read commands (`status`, `layout`, `config.get`, `ping`) that time out or lose the connection fail
  plainly. State-changing commands are **never resent**: a timeout or disconnect yields
  "outcome unknown" and the caller must re-read the state.
* At most 8 requests in flight, 64 waiting; beyond that callers get "busy" immediately.

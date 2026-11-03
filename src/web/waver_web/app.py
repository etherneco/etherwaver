"""The web panel backend: HTTP API v1, live events and static frontend."""

from __future__ import annotations

import asyncio
import glob
import json
import logging
import mimetypes
import os
import platform
import re
import socket
import time
from collections import deque
from typing import Any, Awaitable, Callable, Deque, Dict, List, Optional, Union

from . import __version__, model
from .audit import AuditLog
from .auth import ROLES, SessionManager, UserStore, permissions_for, role_allows
from .control_client import (BackendBusy, BackendUnavailable, ControlClient, ControlError,
                             OutcomeUnknown, RequestTimeout)
from .events import EventHub
from .fingerprints import FINGERPRINT_LOG_RE, FingerprintStore, normalize_hex
from .http import HTTPError, HTTPServer, Request, Response, Router, StreamContext, StreamResponse
from .jobs import Job, JobContext, JobFailure, JobManager
from .qsettings import LockTimeout
from .settings import WebSettings
from .store import ConflictError, ReadOnlyError, SharedConfigStore
from .supervisor import Supervisor, SupervisorError

log = logging.getLogger("waver_web.app")

API = "/api/v1"
COOKIE = "ew_session"
CSP = ("default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; "
       "connect-src 'self'; font-src 'self'; object-src 'none'; base-uri 'none'; "
       "frame-ancestors 'none'; form-action 'self'")

CONTROL_ERROR_STATUS = {
    "not_found": (404, "not_found"),
    "not_connected": (409, "device_not_connected"),
    "invalid_config": (422, "invalid_config"),
    "missing_primary_screen": (422, "invalid_config"),
    "rejected": (422, "invalid_config"),
    "bad_request": (400, "bad_request"),
    "busy": (503, "backend_busy"),
    "io_error": (502, "backend_io_error"),
}


def ipv4_addresses() -> List[str]:
    """Non-loopback IPv4 addresses (MainWindow::getIPAddresses)."""
    out: List[str] = []
    try:
        import fcntl
        import struct
        for _idx, name in socket.if_nameindex():
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            try:
                packed = fcntl.ioctl(s.fileno(), 0x8915, struct.pack("256s", name[:15].encode()))  # SIOCGIFADDR
                ip = socket.inet_ntoa(packed[20:24])
            except OSError:
                continue
            finally:
                s.close()
            if not ip.startswith("127.") and ip not in out:
                out.append(ip)
    except (ImportError, OSError, AttributeError):
        try:
            for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
                ip = info[4][0]
                if not ip.startswith("127.") and ip not in out:
                    out.append(ip)
        except OSError:
            pass
    # like the desktop panel, list 192.168.x.x first: usually the useful one
    return sorted(out, key=lambda ip: (not ip.startswith("192.168."), ip))


def load_languages() -> List[Dict[str, str]]:
    here = os.path.dirname(os.path.abspath(__file__))
    candidates = [os.path.join(here, "languages.json")]
    for path in candidates:
        try:
            with open(path, "r", encoding="utf-8") as fh:
                return json.load(fh)
        except (OSError, ValueError):
            continue
    return [{"code": "en", "name": "English"}]


class DeviceTracker:
    """Connection history derived from control events (last activity etc.)."""

    def __init__(self) -> None:
        self.devices: Dict[str, Dict[str, Any]] = {}
        self.rejected: Dict[str, Dict[str, Any]] = {}
        self.pending_fingerprints: Dict[str, Dict[str, Any]] = {}

    def _entry(self, name: str) -> Dict[str, Any]:
        return self.devices.setdefault(name, {"connectedAt": None, "lastActivity": None,
                                              "lastDisconnect": None, "connections": 0})

    def connected(self, name: str) -> None:
        e = self._entry(name)
        now = time.time()
        e["connectedAt"] = now
        e["lastActivity"] = now
        e["connections"] += 1
        self.rejected.pop(name.lower(), None)

    def disconnected(self, name: str) -> None:
        e = self._entry(name)
        e["connectedAt"] = None
        e["lastDisconnect"] = time.time()
        e["lastActivity"] = e["lastDisconnect"]

    def activity(self, name: str) -> None:
        if name:
            self._entry(name)["lastActivity"] = time.time()

    def sync_connected(self, names: List[str]) -> None:
        now = time.time()
        for n in names:
            e = self._entry(n)
            if e["connectedAt"] is None:
                e["connectedAt"] = now
                e["lastActivity"] = e["lastActivity"] or now
        for n, e in self.devices.items():
            if n not in names and e["connectedAt"] is not None:
                e["connectedAt"] = None
                e["lastDisconnect"] = now

    def reject(self, name: str, reason: str) -> None:
        r = self.rejected.setdefault(name.lower(), {"name": name, "attempts": 0, "reason": reason, "lastAttempt": None})
        r["attempts"] += 1
        r["reason"] = reason
        r["lastAttempt"] = time.time()


class WebApp:
    def __init__(self, settings: WebSettings) -> None:
        self.settings = settings
        os.makedirs(settings.data_dir, mode=0o700, exist_ok=True)
        self.store = SharedConfigStore(settings.settings_file, settings.gui_state_file)
        self.hub = EventHub()
        self.users = UserStore(os.path.join(settings.data_dir, "users.json"))
        self.sessions = SessionManager(settings.session_idle_minutes * 60.0, settings.session_max_hours * 3600.0)
        self.audit = AuditLog(os.path.join(settings.data_dir, "audit.log"))
        self.fingerprints = FingerprintStore(settings.profile_dir)
        self.jobs = JobManager(self._publish_job)
        self.tracker = DeviceTracker()
        self.logs: Deque[Dict[str, Any]] = deque(maxlen=2000)
        self.languages = load_languages()
        host, port = settings.control_hostport
        self.control = ControlClient(host, port, self._load_token, on_event=self._on_control_event,
                                     on_state=self._on_control_state, request_timeout=settings.request_timeout)
        self.supervisor = Supervisor(settings.manage_service, settings.bin_dir or None,
                                     os.path.join(settings.data_dir, "server"), settings.control_address,
                                     self._on_process_output, self._on_process_state,
                                     display=settings.display or None)
        self.server_status: Optional[Dict[str, Any]] = None
        self.server_status_at: Optional[float] = None
        self._refresh_task: Optional[asyncio.Task] = None
        self._tasks: List[asyncio.Task] = []
        self.started_at = time.time()
        self.router = Router()
        self._routes()
        self.http = HTTPServer(self.handle, trusted_proxies=settings.trusted_proxies, base_path=settings.base_path)

    # ------------------------------------------------------------- lifecycle
    async def start(self) -> None:
        host, port = self.settings.listen_hostport
        await self.http.listen(host, port)
        self.control.start()
        self._tasks.append(asyncio.ensure_future(self.store.watch(self._on_config_changed)))
        self._tasks.append(asyncio.ensure_future(self._periodic_refresh()))
        log.info("waver-web %s listening on http://%s:%d%s/", __version__, host, port, self.settings.base_path)

    async def stop(self) -> None:
        log.info("shutting down")
        self.hub.close_all()
        await self.http.close(grace=3.0)
        await self.jobs.shutdown(grace=5.0)
        for t in self._tasks:
            t.cancel()
        for t in self._tasks:
            try:
                await t
            except (asyncio.CancelledError, Exception):
                pass
        await self.supervisor.shutdown()
        await self.control.stop()

    def _load_token(self) -> Optional[str]:
        try:
            with open(self.settings.control_token_file, "r", encoding="utf-8") as fh:
                return fh.readline().strip() or None
        except OSError:
            return None

    # ------------------------------------------------------- backend events
    def _publish_job(self, event: str, data: Dict[str, Any]) -> None:
        self.hub.publish(event, data, "jobs:read")

    async def _on_control_state(self, state: str, info: Dict[str, Any]) -> None:
        self.hub.publish("backend.state", {"state": state, "error": info.get("error"),
                                           "server": (info.get("hello") or {}).get("server")}, "status:read")
        if state == "connected":
            await self.refresh_status()
        else:
            # never present cached data as current while disconnected
            self.hub.publish("server.status", self.status_payload(), "status:read")

    async def _on_control_event(self, event: str, data: Dict[str, Any], seq: int) -> None:
        if event == "log":
            entry = {"ts": time.time(), "source": "server", "level": data.get("level", "INFO"),
                     "message": data.get("message", "")}
            self._append_log(entry)
            return
        if event == "client.connected":
            self.tracker.connected(data.get("name", ""))
        elif event == "client.disconnected":
            self.tracker.disconnected(data.get("name", ""))
        elif event == "client.rejected":
            self.tracker.reject(data.get("name", ""), data.get("reason", ""))
        elif event == "screen.active":
            self.tracker.activity(data.get("client", ""))
        elif event == "server.stopping":
            self.control.reconnect_now()
        self.hub.publish("server.event", {"event": event, "data": data}, "status:read")
        self._schedule_refresh()

    def _append_log(self, entry: Dict[str, Any]) -> None:
        m = FINGERPRINT_LOG_RE.search(entry["message"])
        if m:
            sha256 = normalize_hex(m.group(2))
            kind = "clients" if (self.server_status is not None) else "servers"
            if not self.fingerprints.is_trusted(kind, sha256):
                self.tracker.pending_fingerprints[sha256] = {
                    "sha256": sha256, "sha1": normalize_hex(m.group(1)), "kind": kind,
                    "seen": time.time()}
                self.hub.publish("security.pending", {"sha256": sha256, "kind": kind}, "security:read")
        self.logs.append(entry)
        self.hub.publish("log", entry, "logs:read")

    def _on_process_output(self, source: str, text: str) -> None:
        level = "INFO"
        m = re.search(r"\] (FATAL|ERROR|WARNING|NOTE|INFO|DEBUG\d?):", text)
        if m:
            level = m.group(1)
        # server log lines arrive through the control channel too; only keep
        # process output that is not also delivered there
        if source == "process" and self.control.connected and self.supervisor.mode == "server":
            if m:
                return
        self._append_log({"ts": time.time(), "source": source, "level": level, "message": text})

    def _on_process_state(self, info: Dict[str, Any]) -> None:
        self.hub.publish("service.state", info, "status:read")
        if info.get("running") and info.get("mode") == "server":
            self.control.reconnect_now()

    async def _on_config_changed(self, snapshot: Dict[str, Any], changed: Dict[str, bool]) -> None:
        self.hub.publish("config.changed", {"resources": [k for k, v in changed.items() if v],
                                            "etags": snapshot["etags"]}, "config:read")

    def _schedule_refresh(self) -> None:
        if self._refresh_task and not self._refresh_task.done():
            return

        async def later() -> None:
            await asyncio.sleep(0.15)
            await self.refresh_status()

        self._refresh_task = asyncio.ensure_future(later())

    async def _periodic_refresh(self) -> None:
        while True:
            await asyncio.sleep(10.0)
            if self.control.connected:
                await self.refresh_status()

    async def refresh_status(self) -> None:
        try:
            status = await self.control.request("status")
        except (BackendUnavailable, RequestTimeout, BackendBusy, ControlError) as e:
            log.debug("status refresh failed: %s", e)
            return
        self.server_status = status
        self.server_status_at = time.time()
        self.tracker.sync_connected([c["name"] for c in status.get("clients", []) if not c.get("primary")])
        self.hub.publish("server.status", self.status_payload(), "status:read")

    # ------------------------------------------------------------ payloads
    def status_payload(self) -> Dict[str, Any]:
        connected = self.control.connected
        server = self.server_status if connected else None
        external = False
        if server is not None and self.supervisor.pid is not None:
            external = int(server.get("pid", 0)) != self.supervisor.pid
        elif server is not None:
            external = True
        return {
            "backend": {"state": self.control.state, "error": self.control.last_error,
                        "address": self.settings.control_address},
            "server": server,
            "serverAsOf": self.server_status_at if connected else None,
            "stale": not connected,
            "lastKnownServer": None if connected else self.server_status,
            "service": dict(self.supervisor.info(), owner=("web" if self.supervisor.running else
                                                           ("external" if external else None))),
            "web": {"version": __version__, "startedAt": self.started_at,
                    "liveSubscribers": self.hub.subscriber_count},
        }

    def devices_payload(self, snap: Dict[str, Any]) -> List[Dict[str, Any]]:
        status = self.server_status if self.control.connected else None
        clients = {c["name"].lower(): c for c in (status or {}).get("clients", [])}
        active_screen = ((status or {}).get("active") or {}).get("screen", "")
        active_client = ((status or {}).get("active") or {}).get("client", "")
        out = []
        seen = set()
        for s in snap["layout"]["screens"]:
            name = s["name"]
            base = model.base_host_name(name)
            client = clients.get(name.lower()) or clients.get(base.lower())
            history = self.tracker.devices.get(client["name"] if client else name) or \
                self.tracker.devices.get(base) or {}
            out.append({
                "name": name, "host": base, "aliases": s["aliases"], "configured": True,
                "isServer": name.lower() == snap["serverScreen"].lower(),
                "connected": bool(client) if status is not None else None,
                "active": (active_screen.lower() == name.lower() or
                           (client is not None and client["name"] == active_client and not active_screen))
                if status is not None else None,
                "shape": client.get("shape") if client else None,
                "connectedAt": history.get("connectedAt"), "lastActivity": history.get("lastActivity"),
                "lastDisconnect": history.get("lastDisconnect"),
                "layout": {"x": s["x"], "y": s["y"], "width": s["width"], "height": s["height"]},
                "links": s["links"],
            })
            seen.add(name.lower())
            seen.add(base.lower())
        for key, c in clients.items():
            if key in seen:
                continue
            history = self.tracker.devices.get(c["name"], {})
            out.append({"name": c["name"], "host": model.base_host_name(c["name"]), "aliases": [],
                        "configured": False, "isServer": bool(c.get("primary")), "connected": True,
                        "active": c.get("active"), "shape": c.get("shape"),
                        "connectedAt": history.get("connectedAt"), "lastActivity": history.get("lastActivity"),
                        "lastDisconnect": history.get("lastDisconnect"), "layout": None, "links": None})
        for r in self.tracker.rejected.values():
            if r["name"].lower() in seen:
                continue
            out.append({"name": r["name"], "host": model.base_host_name(r["name"]), "aliases": [],
                        "configured": False, "isServer": False, "connected": False, "active": False,
                        "rejected": {"reason": r["reason"], "attempts": r["attempts"], "lastAttempt": r["lastAttempt"]},
                        "shape": None, "connectedAt": None, "lastActivity": r["lastAttempt"],
                        "lastDisconnect": None, "layout": None, "links": None})
        return out

    # --------------------------------------------------------------- routing
    def _routes(self) -> None:
        r = self.router.add
        r("GET", "/healthz", self.h_health)
        r("GET", "/readyz", self.h_ready)
        r("GET", API + "/session", self.h_session_get)
        r("POST", API + "/session", self.h_login)
        r("DELETE", API + "/session", self.h_logout)
        r("POST", API + "/session/password", self.h_change_password)
        r("GET", API + "/meta", self.h_meta)
        r("GET", API + "/status", self.h_status)
        r("GET", API + "/devices", self.h_devices)
        r("GET", API + "/devices/{name}", self.h_device)
        r("POST", API + "/devices/{name}/activate", self.h_activate)
        r("GET", API + "/sessions", self.h_sessions)
        r("POST", API + "/control/lock", self.h_lock)
        r("GET", API + "/config/general", self.h_general_get)
        r("PATCH", API + "/config/general", self.h_general_patch)
        r("GET", API + "/config/layout", self.h_layout_get)
        r("PUT", API + "/config/layout", self.h_layout_put)
        r("POST", API + "/config/layout/screens", self.h_screen_add)
        r("DELETE", API + "/config/layout/screens/{name}", self.h_screen_delete)
        r("GET", API + "/config/server-text", self.h_server_text)
        r("GET", API + "/config/export", self.h_export)
        r("POST", API + "/config/import", self.h_import)
        r("POST", API + "/config/apply", self.h_apply)
        r("POST", API + "/service/{action}", self.h_service)
        r("GET", API + "/security/fingerprints", self.h_fingerprints)
        r("POST", API + "/security/fingerprints/{kind}", self.h_trust)
        r("DELETE", API + "/security/fingerprints/{kind}/{sha256}", self.h_untrust)
        r("GET", API + "/logs", self.h_logs)
        r("GET", API + "/diagnostics", self.h_diagnostics)
        r("GET", API + "/audit", self.h_audit)
        r("GET", API + "/jobs", self.h_jobs)
        r("GET", API + "/jobs/{id}", self.h_job)
        r("GET", API + "/users", self.h_users)
        r("POST", API + "/users", self.h_user_add)
        r("PATCH", API + "/users/{username}", self.h_user_patch)
        r("DELETE", API + "/users/{username}", self.h_user_delete)
        r("GET", API + "/events", self.h_events)

    async def handle(self, req: Request) -> Union[Response, StreamResponse]:
        if req.state.get("redirect_base"):
            return Response(301, b"", headers={"Location": self.settings.base_path + "/"})
        handler, params, allowed = self.router.match(req.method, req.path)
        if handler is None:
            if req.path.startswith(API + "/") or req.path == API:
                if allowed:
                    raise HTTPError(405, "method_not_allowed", "method not allowed",
                                    headers={"Allow": ", ".join(sorted(set(allowed)))})
                raise HTTPError(404, "not_found", "no such endpoint")
            if req.method not in ("GET", "HEAD"):
                raise HTTPError(405, "method_not_allowed", "method not allowed")
            return self.static(req)
        req.params = params
        try:
            resp = await handler(req)
        except HTTPError:
            raise
        except model.ValidationError as e:
            raise HTTPError(422, "validation_failed", "the request contains invalid values", e.errors)
        except ConflictError as e:
            raise HTTPError(412, "conflict", "%s was changed by another panel; reload to see the current values" % e.resource,
                            {"resource": e.resource, "current": e.current, "etag": e.etag}, {"ETag": e.etag})
        except LockTimeout as e:
            raise HTTPError(503, "config_locked", str(e), headers={"Retry-After": "1"})
        except ReadOnlyError as e:
            raise HTTPError(409, "read_only", str(e))
        except BackendUnavailable as e:
            raise HTTPError(503, "backend_unavailable", "Etherwaver server is not reachable: %s" % e,
                            headers={"Retry-After": "2"})
        except BackendBusy as e:
            raise HTTPError(503, "backend_busy", str(e), headers={"Retry-After": "1"})
        except RequestTimeout as e:
            raise HTTPError(504, "backend_timeout", str(e))
        except OutcomeUnknown as e:
            raise HTTPError(504, "outcome_unknown", str(e))
        except ControlError as e:
            status, code = CONTROL_ERROR_STATUS.get(e.code, (502, "backend_error"))
            raise HTTPError(status, code, e.message or e.code)
        except SupervisorError as e:
            status = 409 if e.code in ("not_managed", "already_running", "external_owner") else 422
            raise HTTPError(status, e.code, e.message)
        if isinstance(resp, Response) and req.path.startswith(API):
            resp.headers.setdefault("Cache-Control", "no-store")
        return resp

    # --------------------------------------------------------- auth helpers
    def _session(self, req: Request):
        return self.sessions.get(req.cookies.get(COOKIE))

    def _allowed_origins(self, req: Request) -> List[str]:
        return [req.origin] + self.settings.allowed_origins

    def require(self, req: Request, permission: str, mutating: bool = False):
        s = self._session(req)
        if s is None:
            raise HTTPError(401, "unauthenticated", "sign in required")
        if not role_allows(s.role, permission):
            self.audit.record(user=s.username, role=s.role, ip=req.client_ip, action="denied:" + permission,
                              resource=req.path, result="denied", request_id=req.request_id)
            raise HTTPError(403, "forbidden", "your role (%s) does not allow this operation" % s.role)
        if mutating:
            self._check_csrf(req, s.csrf)
        return s

    def _check_csrf(self, req: Request, expected: str) -> None:
        origin = req.headers.get("origin")
        if origin and origin not in self._allowed_origins(req):
            raise HTTPError(403, "bad_origin", "cross-origin request refused")
        if not origin:
            referer = req.headers.get("referer", "")
            if referer and not any(referer.startswith(o + "/") or referer == o for o in self._allowed_origins(req)):
                raise HTTPError(403, "bad_origin", "cross-origin request refused")
        token = req.headers.get("x-csrf-token", "")
        import hmac as _hmac
        if not token or not _hmac.compare_digest(token, expected):
            raise HTTPError(403, "csrf_failed", "missing or invalid CSRF token")

    def record(self, req: Request, s, action: str, resource: str, result: str, detail=None) -> None:
        self.audit.record(user=s.username if s else None, role=s.role if s else None, ip=req.client_ip,
                          action=action, resource=resource, result=result, request_id=req.request_id,
                          detail=detail)

    async def audited(self, req: Request, s, action: str, resource: str, fn: Callable[[], Awaitable[Any]],
                      detail=None) -> Any:
        try:
            result = await fn()
        except (OutcomeUnknown,) as e:
            self.record(req, s, action, resource, "uncertain", detail)
            raise
        except HTTPError as e:
            self.record(req, s, action, resource, "failure", dict(detail or {}, error=e.code))
            raise
        except Exception as e:
            code = getattr(e, "code", type(e).__name__)
            self.record(req, s, action, resource, "failure", dict(detail or {}, error=str(code)))
            raise
        self.record(req, s, action, resource, "success", detail)
        return result

    def _cookie(self, req: Request, value: str, max_age: int) -> str:
        secure = self.settings.cookie_secure == "always" or (
            self.settings.cookie_secure == "auto" and req.scheme == "https")
        parts = ["%s=%s" % (COOKIE, value), "Path=%s/" % self.settings.base_path, "HttpOnly", "SameSite=Strict",
                 "Max-Age=%d" % max_age]
        if secure:
            parts.append("Secure")
        return "; ".join(parts)

    def _if_match(self, req: Request) -> str:
        value = req.headers.get("if-match")
        if not value:
            raise HTTPError(428, "precondition_required",
                            "send If-Match with the ETag you edited, so newer changes are not overwritten")
        return value.strip()

    def _run_blocking(self, fn, *args):
        return asyncio.get_running_loop().run_in_executor(None, fn, *args)

    # --------------------------------------------------------- endpoints
    async def h_health(self, req: Request) -> Response:
        return Response.json({"status": "ok", "version": __version__})

    async def h_ready(self, req: Request) -> Response:
        ok = self.control.connected
        return Response.json({"ready": ok, "backend": self.control.state}, 200 if ok else 503)

    def _session_body(self, s) -> Dict[str, Any]:
        return {"user": s.username, "role": s.role, "permissions": permissions_for(s.role),
                "csrfToken": s.csrf, "expiresAt": self.sessions.expires_at(s),
                "idleTimeout": self.sessions.idle_timeout}

    async def h_session_get(self, req: Request) -> Response:
        s = self._session(req)
        if s is None:
            setup = not self.users.list()
            raise HTTPError(401, "unauthenticated", "sign in required", {"setupRequired": setup})
        return Response.json(self._session_body(s))

    async def h_login(self, req: Request) -> Response:
        origin = req.headers.get("origin")
        if origin and origin not in self._allowed_origins(req):
            raise HTTPError(403, "bad_origin", "cross-origin request refused")
        body = req.json()
        if not isinstance(body, dict):
            raise HTTPError(400, "bad_request", "object expected")
        username = body.get("username")
        password = body.get("password")
        if not isinstance(username, str) or not isinstance(password, str) or not username or not password:
            raise HTTPError(422, "validation_failed", "username and password are required",
                            [{"field": "username", "message": "required"}])
        key = "%s|%s" % (req.client_ip, username.lower())
        if self.sessions.throttled(key) or self.sessions.throttled(req.client_ip, limit=20):
            raise HTTPError(429, "too_many_attempts", "too many failed sign-in attempts, try again later",
                            headers={"Retry-After": "300"})
        user = await self._run_blocking(self.users.authenticate, username, password)
        if user is None:
            self.sessions.record_failure(key)
            self.sessions.record_failure(req.client_ip)
            self.audit.record(user=username[:64], role=None, ip=req.client_ip, action="session.login",
                              resource="session", result="failure", request_id=req.request_id)
            raise HTTPError(401, "invalid_credentials", "invalid user name or password")
        self.sessions.clear_failures(key)
        s = self.sessions.create(user["username"], user["role"], req.client_ip)
        self.record(req, s, "session.login", "session", "success")
        resp = Response.json(self._session_body(s), 201)
        resp.cookies.append(self._cookie(req, s.id, int(self.sessions.absolute_timeout)))
        return resp

    async def h_logout(self, req: Request) -> Response:
        s = self._session(req)
        resp = Response(204)
        if s is not None:
            self._check_csrf(req, s.csrf)
            self.sessions.destroy(s.id)
            self.record(req, s, "session.logout", "session", "success")
        resp.cookies.append(self._cookie(req, "", 0))
        return resp

    async def h_change_password(self, req: Request) -> Response:
        s = self.require(req, "status:read", mutating=True)
        body = req.json()
        if not isinstance(body, dict):
            raise HTTPError(400, "bad_request", "object expected")
        current = body.get("currentPassword", "")
        new = body.get("newPassword", "")
        user = await self._run_blocking(self.users.authenticate, s.username, current)
        if user is None:
            self.record(req, s, "user.password", "user:" + s.username, "failure")
            raise HTTPError(422, "validation_failed", "current password is wrong",
                            [{"field": "currentPassword", "message": "incorrect"}])
        try:
            await self._run_blocking(lambda: self.users.update(s.username, password=new))
        except ValueError as e:
            raise HTTPError(422, "validation_failed", str(e), [{"field": "newPassword", "message": str(e)}])
        self.record(req, s, "user.password", "user:" + s.username, "success")
        return Response(204)

    async def h_meta(self, req: Request) -> Response:
        self.require(req, "status:read")
        return Response.json({
            "version": __version__,
            "platform": {"system": platform.system(), "hostname": socket.gethostname(),
                         "addresses": ipv4_addresses(),
                         "linux": platform.system() == "Linux", "windows": platform.system() == "Windows"},
            "capabilities": {
                "serviceManagement": self.supervisor.enabled,
                "remoteView": False, "audio": False,
                "zeroconf": False,
                "certificateGeneration": False,
            },
            "enums": {
                "logLevels": model.LOG_LEVELS, "elevateModes": model.ELEVATE_MODES,
                "modifiers": model.MODIFIERS, "fixes": model.FIXES, "corners": model.CORNER_KEYS,
                "actionTypes": model.ACTION_TYPES, "switchDirections": model.SWITCH_DIRECTIONS,
                "lockModes": model.LOCK_MODES, "roles": ROLES,
                "specialKeys": [n for _, n in model.SPECIAL_KEYS] + ["F%d" % i for i in range(1, 36)],
                "modifierKeys": ["Shift", "Control", "Alt", "Meta"],
            },
            "languages": self.languages,
            "paths": {"settings": self.settings.settings_file, "profile": self.settings.profile_dir},
        })

    async def h_status(self, req: Request) -> Response:
        self.require(req, "status:read")
        return Response.json(self.status_payload())

    async def h_devices(self, req: Request) -> Response:
        self.require(req, "devices:read")
        snap = await self._run_blocking(self.store.snapshot)
        devices = self.devices_payload(snap)
        q = req.query.get("q", "").strip().lower()
        state = req.query.get("state", "")
        if q:
            devices = [d for d in devices if q in d["name"].lower() or any(q in a.lower() for a in d["aliases"])]
        if state == "connected":
            devices = [d for d in devices if d["connected"]]
        elif state == "disconnected":
            devices = [d for d in devices if d["connected"] is False]
        elif state == "unconfigured":
            devices = [d for d in devices if not d["configured"]]
        return Response.json({"devices": devices, "stale": not self.control.connected,
                              "asOf": self.server_status_at if self.control.connected else None})

    async def h_device(self, req: Request) -> Response:
        self.require(req, "devices:read")
        snap = await self._run_blocking(self.store.snapshot)
        name = req.params["name"].lower()
        for d in self.devices_payload(snap):
            if d["name"].lower() == name:
                screen = next((s for s in snap["layout"]["screens"] if s["name"].lower() == name), None)
                return Response.json({"device": d, "screen": screen, "layoutEtag": snap["etags"]["layout"]})
        raise HTTPError(404, "not_found", "no such device")

    async def h_activate(self, req: Request) -> Response:
        s = self.require(req, "control:switch", mutating=True)
        name = req.params["name"]
        if not model.SCREEN_NAME_RE.match(name):
            raise HTTPError(422, "validation_failed", "invalid screen name")

        async def do():
            result = await self.control.request("switch", {"screen": name})
            self._schedule_refresh()
            return result

        result = await self.audited(req, s, "control.switch", "device:" + name, do)
        return Response.json(result)

    async def h_sessions(self, req: Request) -> Response:
        s = self.require(req, "status:read")
        status = self.server_status if self.control.connected else None
        kvm = None
        if status is not None:
            kvm = {"active": status.get("active"), "lockedToScreen": status.get("lockedToScreen"),
                   "clients": [dict(c, connectedAt=self.tracker.devices.get(c["name"], {}).get("connectedAt"))
                               for c in status.get("clients", [])]}
        body: Dict[str, Any] = {"kvm": kvm, "stale": status is None}
        if role_allows(s.role, "users:manage"):
            body["web"] = [{"user": x.username, "role": x.role, "ip": x.ip, "created": x.created,
                            "lastSeen": x.last_seen, "current": x.id == s.id}
                           for x in list(self.sessions._sessions.values())]
        return Response.json(body)

    async def h_lock(self, req: Request) -> Response:
        s = self.require(req, "control:lock", mutating=True)
        body = req.json()
        mode = body.get("mode") if isinstance(body, dict) else None
        if mode not in ("on", "off", "toggle"):
            raise HTTPError(422, "validation_failed", "mode must be on, off or toggle",
                            [{"field": "mode", "message": "must be on, off or toggle"}])

        async def do():
            r = await self.control.request("lock", {"mode": mode})
            self._schedule_refresh()
            return r

        return Response.json(await self.audited(req, s, "control.lock", "cursor", do, {"mode": mode}))

    async def h_general_get(self, req: Request) -> Response:
        self.require(req, "config:read")
        snap = await self._run_blocking(self.store.snapshot)
        return Response.json({"general": snap["general"], "etag": snap["etags"]["general"]},
                             headers={"ETag": snap["etags"]["general"]})

    async def h_general_patch(self, req: Request) -> Response:
        s = self.require(req, "config:write", mutating=True)
        etag = self._if_match(req)
        body = req.json()

        async def do():
            return await self._run_blocking(self.store.update_general, body, etag)

        snap = await self.audited(req, s, "config.general.update", "config:general", do,
                                  {"fields": sorted(body.keys()) if isinstance(body, dict) else None})
        self.hub.publish("config.changed", {"resources": ["general"], "etags": snap["etags"], "by": s.username},
                         "config:read")
        return Response.json({"general": snap["general"], "etag": snap["etags"]["general"]},
                             headers={"ETag": snap["etags"]["general"]})

    async def h_layout_get(self, req: Request) -> Response:
        self.require(req, "config:read")
        snap = await self._run_blocking(self.store.snapshot)
        return Response.json({"layout": snap["layout"], "etag": snap["etags"]["layout"],
                              "serverScreen": snap["serverScreen"],
                              "configSource": snap["general"]["configSource"]},
                             headers={"ETag": snap["etags"]["layout"]})

    async def _save_layout(self, req: Request, s, layout: Dict[str, Any], etag: str, action: str,
                           resource: str, detail=None) -> Response:
        async def do():
            return await self._run_blocking(self.store.replace_layout, layout, etag)

        snap = await self.audited(req, s, action, resource, do, detail)
        self.hub.publish("config.changed", {"resources": ["layout"], "etags": snap["etags"], "by": s.username},
                         "config:read")
        return Response.json({"layout": snap["layout"], "etag": snap["etags"]["layout"],
                              "serverScreen": snap["serverScreen"]}, headers={"ETag": snap["etags"]["layout"]})

    async def h_layout_put(self, req: Request) -> Response:
        s = self.require(req, "config:write", mutating=True)
        etag = self._if_match(req)
        body = req.json()
        return await self._save_layout(req, s, body, etag, "config.layout.update", "config:layout",
                                       {"screens": len(body.get("screens", [])) if isinstance(body, dict) else None,
                                        "hotkeys": len(body.get("hotkeys", [])) if isinstance(body, dict) else None})

    async def h_screen_add(self, req: Request) -> Response:
        s = self.require(req, "config:write", mutating=True)
        etag = self._if_match(req)
        body = req.json()
        if not isinstance(body, dict) or not isinstance(body.get("name"), str):
            raise HTTPError(422, "validation_failed", "name is required", [{"field": "name", "message": "required"}])
        snap = await self._run_blocking(self.store.snapshot)
        layout = model.deep_copy(snap["layout"])
        screens = layout["screens"]
        max_x = max([sc["x"] + sc["width"] for sc in screens] or [0])
        screen = model.new_screen(body["name"], body.get("x", max_x + 20), body.get("y", 20))
        for key in ("aliases", "width", "height", "links", "modifiers", "switchCorners", "switchCornerSize", "fixes"):
            if key in body:
                screen[key] = body[key]
        screens.append(screen)
        resp = await self._save_layout(req, s, layout, etag, "config.screen.add", "screen:" + body["name"])
        resp.status = 201
        return resp

    async def h_screen_delete(self, req: Request) -> Response:
        s = self.require(req, "config:write", mutating=True)
        etag = self._if_match(req)
        name = req.params["name"]
        snap = await self._run_blocking(self.store.snapshot)
        layout = model.deep_copy(snap["layout"])
        before = len(layout["screens"])
        layout["screens"] = [sc for sc in layout["screens"] if sc["name"].lower() != name.lower()]
        if len(layout["screens"]) == before:
            raise HTTPError(404, "not_found", "no such screen")
        for sc in layout["screens"]:
            for d, target in sc["links"].items():
                if target.lower() == name.lower():
                    sc["links"][d] = ""
        for h in layout["hotkeys"]:
            for a in h["actions"]:
                a["screens"] = [x for x in a["screens"] if x.lower() != name.lower()]
                if a["type"] == "switchToScreen" and a["screen"].lower() == name.lower():
                    raise HTTPError(409, "in_use", "a hotkey switches to this screen; edit the hotkey first")
        return await self._save_layout(req, s, layout, etag, "config.screen.delete", "screen:" + name)

    async def h_server_text(self, req: Request) -> Response:
        self.require(req, "config:read")
        snap = await self._run_blocking(self.store.snapshot)
        return Response.json({"config": model.server_config_text(snap["layout"]),
                              "layout": model.layout_json_text(snap["layout"])})

    async def h_export(self, req: Request) -> Response:
        s = self.require(req, "config:read")
        snap = await self._run_blocking(self.store.snapshot)
        fmt = req.query.get("format", "json")
        self.record(req, s, "config.export", "config", "success", {"format": fmt})
        if fmt == "conf":
            return Response(200, model.server_config_text(snap["layout"]), "text/plain; charset=utf-8",
                            {"Content-Disposition": 'attachment; filename="etherwaver.conf"'})
        if fmt == "layout":
            return Response(200, model.layout_json_text(snap["layout"]), "application/json; charset=utf-8",
                            {"Content-Disposition": 'attachment; filename="etherwaver-layout.json"'})
        if fmt != "json":
            raise HTTPError(400, "bad_request", "format must be json, conf or layout")
        body = {"format": "etherwaver-settings", "version": 1, "exportedAt": time.time(),
                "general": snap["general"], "layout": snap["layout"]}
        return Response(200, json.dumps(body, indent=2), "application/json; charset=utf-8",
                        {"Content-Disposition": 'attachment; filename="etherwaver-settings.json"'})

    async def h_import(self, req: Request) -> Response:
        s = self.require(req, "config:write", mutating=True)
        body = req.json()
        if not isinstance(body, dict) or body.get("format") != "etherwaver-settings" or body.get("version") != 1:
            raise HTTPError(422, "validation_failed", "not an Etherwaver settings export (format/version)")
        expected = body.get("expectedEtags") or {}
        if not isinstance(expected, dict) or "general" not in expected or "layout" not in expected:
            raise HTTPError(428, "precondition_required", "expectedEtags.general and expectedEtags.layout are required")
        general = dict(body.get("general") or {})
        # host-specific value: keep this machine's identity
        include_identity = bool(body.get("includeScreenName"))
        if not include_identity:
            general.pop("screenName", None)
        model.validate_general(general)
        snap = await self._run_blocking(self.store.snapshot)
        model.validate_layout(body.get("layout") or {}, snap["serverScreen"] if not include_identity else
                              model.ensure_screen_number_suffix(general.get("screenName") or snap["serverScreen"]))

        async def do():
            return await self._run_blocking(self.store.import_all, general, body.get("layout"),
                                            expected["general"], expected["layout"])

        snap = await self.audited(req, s, "config.import", "config", do)
        self.hub.publish("config.changed", {"resources": ["general", "layout"], "etags": snap["etags"],
                                            "by": s.username}, "config:read")
        return Response.json({"general": snap["general"], "layout": snap["layout"], "etags": snap["etags"]})

    # ---- long running operations
    async def _job_apply(self, ctx: JobContext) -> Dict[str, Any]:
        ctx.progress(10, "reading shared configuration")
        snap = await self._run_blocking(self.store.snapshot)
        if snap["general"]["mode"] != "server":
            raise JobFailure("not_server", "this computer is configured as a client; nothing to apply")
        if snap["general"]["configSource"] == "external":
            path = snap["general"]["externalConfigFile"]
            try:
                with open(path, "r", encoding="utf-8") as fh:
                    text = fh.read()
            except OSError as e:
                raise JobFailure("io_error", "cannot read external configuration: %s" % e)
            params = {"config": text}
        else:
            params = {"config": model.server_config_text(snap["layout"]),
                      "layout": model.layout_json_text(snap["layout"])}
        ctx.progress(40, "sending configuration to the Etherwaver server")
        if not self.control.connected:
            raise JobFailure("backend_unavailable", "the Etherwaver server is not running or not reachable")
        try:
            result = await self.control.request("config.apply", params, timeout=10.0)
        except ControlError as e:
            raise JobFailure(e.code, e.message)
        ctx.progress(80, "verifying server state")
        await self.refresh_status()
        ctx.progress(100, "configuration applied")
        return dict(result, etags=snap["etags"])

    async def _job_service(self, action: str, ctx: JobContext) -> Dict[str, Any]:
        if not self.supervisor.enabled:
            raise JobFailure("not_managed", "process management by the web panel is disabled ([service] manage)")
        status = self.server_status if self.control.connected else None
        if status is not None and self.supervisor.pid != status.get("pid"):
            raise JobFailure("external_owner", "Etherwaver is running under another owner (desktop panel or "
                                               "system service, pid %s); manage it there" % status.get("pid"))
        if action in ("stop", "restart"):
            ctx.progress(20, "stopping Etherwaver")
            await self.supervisor.stop()
        if action in ("start", "restart"):
            ctx.progress(50, "starting Etherwaver")
            try:
                await self.supervisor.start(self.store.snapshot)
            except SupervisorError as e:
                raise JobFailure(e.code, e.message)
            if self.supervisor.mode == "server":
                ctx.progress(70, "waiting for the server to accept management connections")
                self.control.reconnect_now()
                deadline = time.monotonic() + 15.0
                while time.monotonic() < deadline:
                    if not self.supervisor.running:
                        raise JobFailure("start_failed", "the process exited (code %s); see Logs" % self.supervisor.exit_code)
                    if self.control.connected and self.server_status and \
                            self.server_status.get("pid") == self.supervisor.pid:
                        break
                    await asyncio.sleep(0.25)
                    if self.control.connected:
                        await self.refresh_status()
                else:
                    raise JobFailure("start_timeout", "the server did not become ready within 15 s")
        return self.supervisor.info()

    def _start_job(self, req: Request, s, kind: str, resource: str, fn) -> Response:
        existing = self.jobs.running(kind.split(".")[0])
        if existing:
            raise HTTPError(409, "job_running", "another %s operation is already running" % existing.kind,
                            {"jobId": existing.id})

        def finished(job: Job) -> None:
            self.record(req, s, kind, resource, {"succeeded": "success", "uncertain": "uncertain"}.get(job.state, "failure"),
                        {"jobId": job.id, "error": job.error["code"] if job.error else None})

        job = self.jobs.start(kind, s.username, resource, fn, exclusive_key=kind.split(".")[0], on_finished=finished)
        return Response.json({"job": job.to_dict()}, 202, {"Location": "%s%s/jobs/%s" % (self.settings.base_path, API, job.id)})

    async def h_apply(self, req: Request) -> Response:
        s = self.require(req, "config:apply", mutating=True)
        return self._start_job(req, s, "config.apply", "server", self._job_apply)

    async def h_service(self, req: Request) -> Response:
        s = self.require(req, "service:manage", mutating=True)
        action = req.params["action"]
        if action not in ("start", "stop", "restart"):
            raise HTTPError(404, "not_found", "unknown service action")
        if not self.supervisor.enabled:
            raise HTTPError(409, "not_managed", "process management by the web panel is disabled; Etherwaver "
                                                "is managed by the desktop panel or the system service manager")
        return self._start_job(req, s, "service." + action, "service", lambda ctx: self._job_service(action, ctx))

    async def h_jobs(self, req: Request) -> Response:
        self.require(req, "jobs:read")
        return Response.json({"jobs": self.jobs.list()})

    async def h_job(self, req: Request) -> Response:
        self.require(req, "jobs:read")
        job = self.jobs.get(req.params["id"])
        if job is None:
            raise HTTPError(404, "not_found", "no such job")
        return Response.json({"job": job.to_dict()})

    # ---- security
    async def h_fingerprints(self, req: Request) -> Response:
        self.require(req, "security:read")
        local = await self._run_blocking(self.fingerprints.read, "local")
        return Response.json({
            "local": local,
            "trustedClients": await self._run_blocking(self.fingerprints.read, "clients"),
            "trustedServers": await self._run_blocking(self.fingerprints.read, "servers"),
            "pending": [dict(p, display=":".join(p["sha256"][i:i + 2].upper() for i in range(0, 64, 2)))
                        for p in self.tracker.pending_fingerprints.values()],
        })

    async def h_trust(self, req: Request) -> Response:
        s = self.require(req, "security:write", mutating=True)
        kind = req.params["kind"]
        if kind not in ("clients", "servers"):
            raise HTTPError(404, "not_found", "kind must be clients or servers")
        body = req.json()
        sha = body.get("sha256") if isinstance(body, dict) else None
        if not isinstance(sha, str):
            raise HTTPError(422, "validation_failed", "sha256 is required", [{"field": "sha256", "message": "required"}])

        async def do():
            try:
                added = await self._run_blocking(self.fingerprints.add, kind, sha)
            except ValueError as e:
                raise HTTPError(422, "validation_failed", str(e), [{"field": "sha256", "message": str(e)}])
            self.tracker.pending_fingerprints.pop(normalize_hex(sha), None)
            return added

        added = await self.audited(req, s, "security.trust", "fingerprint:%s" % kind, do,
                                   {"sha256": normalize_hex(sha)[:16] + "…"})
        self.hub.publish("security.changed", {"kind": kind}, "security:read")
        return Response.json({"added": added}, 201 if added else 200)

    async def h_untrust(self, req: Request) -> Response:
        s = self.require(req, "security:write", mutating=True)
        kind = req.params["kind"]
        if kind not in ("clients", "servers"):
            raise HTTPError(404, "not_found", "kind must be clients or servers")
        sha = req.params["sha256"]

        async def do():
            removed = await self._run_blocking(self.fingerprints.remove, kind, sha)
            if not removed:
                raise HTTPError(404, "not_found", "fingerprint is not trusted")
            return removed

        await self.audited(req, s, "security.revoke", "fingerprint:%s" % kind, do, {"sha256": normalize_hex(sha)[:16] + "…"})
        self.hub.publish("security.changed", {"kind": kind}, "security:read")
        return Response(204)

    # ---- logs / diagnostics / audit
    async def h_logs(self, req: Request) -> Response:
        self.require(req, "logs:read")
        try:
            limit = max(1, min(2000, int(req.query.get("limit", "500"))))
        except ValueError:
            raise HTTPError(400, "bad_request", "limit must be a number")
        level = req.query.get("level", "")
        levels = ["FATAL", "ERROR", "WARNING", "NOTE", "INFO", "DEBUG", "DEBUG1", "DEBUG2"]
        entries = list(self.logs)
        if level in levels:
            max_index = levels.index(level)
            entries = [e for e in entries if (levels.index(e["level"]) if e["level"] in levels else 99) <= max_index]
        q = req.query.get("q", "").lower()
        if q:
            entries = [e for e in entries if q in e["message"].lower()]
        return Response.json({"entries": entries[-limit:]})

    async def h_diagnostics(self, req: Request) -> Response:
        self.require(req, "diagnostics:read")
        latency = None
        if self.control.connected:
            try:
                latency = await self.control.ping()
            except Exception:
                latency = None
        uhid = []
        for path in glob.glob("/tmp/etherwaver-uhid-cursor-*.status")[:5]:
            try:
                with open(path, "r", encoding="utf-8", errors="replace") as fh:
                    uhid.append({"path": path, "status": fh.read(2000).strip()})
            except OSError:
                pass

        def file_info(path: str) -> Dict[str, Any]:
            try:
                st = os.stat(path)
                return {"path": path, "exists": True, "mode": oct(st.st_mode & 0o777), "size": st.st_size,
                        "modified": st.st_mtime}
            except OSError:
                return {"path": path, "exists": False}

        return Response.json({
            "control": dict(self.control.diagnostics(), latencyMs=latency),
            "server": self.server_status if self.control.connected else None,
            "serverAsOf": self.server_status_at,
            "service": self.supervisor.info(),
            "recentProcessOutput": list(self.supervisor.recent_output)[-50:],
            "files": {"settings": file_info(self.settings.settings_file),
                      "guiState": file_info(self.settings.gui_state_file),
                      "controlToken": file_info(self.settings.control_token_file),
                      "users": file_info(self.users.path)},
            "uhid": uhid,
            "web": {"version": __version__, "python": platform.python_version(),
                    "sessions": self.sessions.count(), "liveSubscribers": self.hub.subscriber_count,
                    "eventSeq": self.hub.seq, "uptimeSeconds": int(time.time() - self.started_at)},
        })

    async def h_audit(self, req: Request) -> Response:
        self.require(req, "audit:read")
        try:
            limit = max(1, min(1000, int(req.query.get("limit", "200"))))
        except ValueError:
            raise HTTPError(400, "bad_request", "limit must be a number")
        entries = await self._run_blocking(lambda: self.audit.read(limit, req.query.get("user") or None,
                                                                   req.query.get("action") or None))
        return Response.json({"entries": entries})

    # ---- users
    async def h_users(self, req: Request) -> Response:
        self.require(req, "users:manage")
        return Response.json({"users": await self._run_blocking(self.users.list), "roles": ROLES})

    async def h_user_add(self, req: Request) -> Response:
        s = self.require(req, "users:manage", mutating=True)
        body = req.json()
        if not isinstance(body, dict):
            raise HTTPError(400, "bad_request", "object expected")
        username = body.get("username", "")

        async def do():
            try:
                await self._run_blocking(self.users.add, username, body.get("password"), body.get("role", "viewer"))
            except ValueError as e:
                raise HTTPError(422, "validation_failed", str(e), [{"field": str(e).split(":")[0], "message": str(e)}])
            except KeyError:
                raise HTTPError(409, "exists", "a user with this name already exists")

        await self.audited(req, s, "user.add", "user:%s" % str(username)[:64], do, {"role": body.get("role", "viewer")})
        return Response.json({"username": username, "role": body.get("role", "viewer")}, 201)

    async def h_user_patch(self, req: Request) -> Response:
        s = self.require(req, "users:manage", mutating=True)
        username = req.params["username"]
        body = req.json()
        if not isinstance(body, dict):
            raise HTTPError(400, "bad_request", "object expected")
        unknown = set(body) - {"role", "disabled", "password"}
        if unknown:
            raise HTTPError(422, "validation_failed", "unknown fields: " + ", ".join(sorted(unknown)))

        async def do():
            try:
                return await self._run_blocking(lambda: self.users.update(
                    username, role=body.get("role"), disabled=body.get("disabled"), password=body.get("password")))
            except LookupError:
                raise HTTPError(404, "not_found", "no such user")
            except PermissionError as e:
                raise HTTPError(409, "last_admin", str(e))
            except ValueError as e:
                raise HTTPError(422, "validation_failed", str(e))

        user = await self.audited(req, s, "user.update", "user:" + username, do,
                                  {"fields": sorted(k for k in body if k != "password"),
                                   "passwordReset": "password" in body})
        if body.get("disabled") or "password" in body:
            self.sessions.destroy_user(username)
        elif "role" in body:
            self.sessions.update_role(username, body["role"])
        return Response.json(user)

    async def h_user_delete(self, req: Request) -> Response:
        s = self.require(req, "users:manage", mutating=True)
        username = req.params["username"]

        async def do():
            try:
                await self._run_blocking(self.users.remove, username)
            except LookupError:
                raise HTTPError(404, "not_found", "no such user")
            except PermissionError as e:
                raise HTTPError(409, "last_admin", str(e))

        await self.audited(req, s, "user.delete", "user:" + username, do)
        self.sessions.destroy_user(username)
        return Response(204)

    # ---- live events (SSE)
    async def h_events(self, req: Request) -> StreamResponse:
        s = self.require(req, "status:read")
        last_id = req.headers.get("last-event-id") or req.query.get("lastEventId")
        sub, replay = self.hub.subscribe(s.role, last_id)
        sid = s.id

        async def produce(ctx: StreamContext) -> None:
            try:
                await ctx.write("retry: 2000\n\n")
                await ctx.write(self._sse("%s-%d" % (self.hub.epoch, self.hub.seq), "hello",
                                          json.dumps({"type": "hello", "ts": time.time(),
                                                      "data": {"epoch": self.hub.epoch, "user": s.username,
                                                               "role": s.role}})))
                for item in replay:
                    await ctx.write(self._sse(*item))
                while not ctx.closing:
                    try:
                        item = await asyncio.wait_for(sub.queue.get(), 15.0)
                    except asyncio.TimeoutError:
                        if self.sessions.get(sid, touch=False) is None:
                            await ctx.write(self._sse("%s-%d" % (self.hub.epoch, self.hub.seq), "session.expired",
                                                      json.dumps({"type": "session.expired", "data": {}})))
                            return
                        await ctx.write(": keep-alive\n\n")
                        continue
                    if item is None:
                        return
                    await ctx.write(self._sse(*item))
                    if sub.overflowed:
                        return
            finally:
                self.hub.unsubscribe(sub)

        return StreamResponse(produce)

    @staticmethod
    def _sse(event_id: str, event: str, payload: str) -> str:
        return "id: %s\nevent: %s\ndata: %s\n\n" % (event_id, event, payload)

    # ---- static frontend
    def static(self, req: Request) -> Response:
        root = os.path.realpath(self.settings.static_dir)
        rel = req.path.lstrip("/")
        candidate = os.path.realpath(os.path.join(root, rel)) if rel else os.path.join(root, "index.html")
        if not candidate.startswith(root + os.sep) and candidate != root:
            raise HTTPError(404, "not_found", "not found")
        if not os.path.isfile(candidate):
            if "." in os.path.basename(rel):
                raise HTTPError(404, "not_found", "not found")
            candidate = os.path.join(root, "index.html")  # client-side route
        if not os.path.isfile(candidate):
            return Response(503, "The web panel frontend is not built. Run: python3 -m waver_web.build\n")
        with open(candidate, "rb") as fh:
            data = fh.read()
        ctype = mimetypes.guess_type(candidate)[0] or "application/octet-stream"
        if ctype.startswith("text/") or ctype in ("application/javascript", "application/json"):
            ctype += "; charset=utf-8"
        if candidate.endswith(".js") or candidate.endswith(".mjs"):
            ctype = "text/javascript; charset=utf-8"
        headers = {"Content-Security-Policy": CSP}
        if "/assets/" in candidate.replace(os.sep, "/"):
            headers["Cache-Control"] = "public, max-age=31536000, immutable"
        else:
            headers["Cache-Control"] = "no-cache"
        return Response(200, data, ctype, headers)


async def serve(settings: WebSettings, ready: Optional[asyncio.Event] = None) -> None:
    import signal
    app = WebApp(settings)
    await app.start()
    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, stop.set)
        except (NotImplementedError, RuntimeError):
            pass
    if ready:
        ready.set()
    await stop.wait()
    await app.stop()

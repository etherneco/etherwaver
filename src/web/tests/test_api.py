import asyncio
import http.client
import json
import os
import shutil
import socket
import sys
import tempfile
import threading
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.dirname(__file__))

from waver_web.app import WebApp  # noqa: E402
from waver_web.auth import UserStore  # noqa: E402
from waver_web.settings import WebSettings  # noqa: E402
from waver_web.store import SharedConfigStore  # noqa: E402
from fakes import FakeControlServer  # noqa: E402

PASSWORD = "correct-horse-battery"


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Harness:
    """Runs a WebApp plus a fake control server on a background loop."""

    def __init__(self):
        self.tmp = tempfile.mkdtemp(prefix="waver-web-test-")
        self.loop = asyncio.new_event_loop()
        self.thread = threading.Thread(target=self.loop.run_forever, daemon=True)
        self.thread.start()
        self.fake = self.run(FakeControlServer().start())
        token_file = os.path.join(self.tmp, "control-token")
        with open(token_file, "w") as fh:
            fh.write(self.fake.token + "\n")
        self.port = free_port()
        s = WebSettings(listen="127.0.0.1:%d" % self.port, control_address="127.0.0.1:%d" % self.fake.port,
                        control_token_file=token_file, request_timeout=1.0,
                        settings_file=os.path.join(self.tmp, "cfg", "EtherWaver.conf"),
                        profile_dir=os.path.join(self.tmp, "profile"))
        s.resolve()
        os.makedirs(s.data_dir, exist_ok=True)
        users = UserStore(os.path.join(s.data_dir, "users.json"))
        users.add("admin", PASSWORD, "admin")
        users.add("operator", PASSWORD, "operator")
        users.add("viewer", PASSWORD, "viewer")
        self.settings = s
        self.app = self.run(self._make_app(s))
        deadline = time.time() + 5
        while not self.app.control.connected and time.time() < deadline:
            time.sleep(0.02)

    async def _make_app(self, s):
        app = WebApp(s)
        await app.start()
        return app

    def run(self, coro, timeout=10):
        return asyncio.run_coroutine_threadsafe(coro, self.loop).result(timeout)

    def close(self):
        self.run(self.app.stop())
        self.run(self.fake.stop())
        self.loop.call_soon_threadsafe(self.loop.stop)
        self.thread.join(5)
        shutil.rmtree(self.tmp, ignore_errors=True)


class Client:
    def __init__(self, port):
        self.port = port
        self.cookie = None
        self.csrf = None

    def request(self, method, path, body=None, headers=None, csrf=True):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        h = {"Host": "127.0.0.1:%d" % self.port}
        if body is not None:
            h["Content-Type"] = "application/json"
            body = json.dumps(body)
        if self.cookie:
            h["Cookie"] = self.cookie
        if csrf and self.csrf and method != "GET":
            h["X-CSRF-Token"] = self.csrf
        h.update(headers or {})
        conn.request(method, path, body=body, headers=h)
        resp = conn.getresponse()
        raw = resp.read()
        for k, v in resp.getheaders():
            if k.lower() == "set-cookie" and v.startswith("ew_session="):
                self.cookie = v.split(";")[0]
        conn.close()
        try:
            data = json.loads(raw) if raw else None
        except ValueError:
            data = raw.decode("utf-8", "replace")
        return resp.status, data, dict((k.lower(), v) for k, v in resp.getheaders())

    def login(self, user, password=PASSWORD):
        status, data, _ = self.request("POST", "/api/v1/session", {"username": user, "password": password})
        if status == 201:
            self.csrf = data["csrfToken"]
        return status, data


class ApiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.h = Harness()

    @classmethod
    def tearDownClass(cls):
        cls.h.close()

    def client(self, user=None):
        c = Client(self.h.port)
        if user:
            status, _ = c.login(user)
            self.assertEqual(status, 201)
        return c

    # -- 1. authentication and authorization -------------------------------
    def test_unauthenticated_requests_are_rejected(self):
        c = self.client()
        for method, path in (("GET", "/api/v1/status"), ("GET", "/api/v1/config/general"),
                             ("POST", "/api/v1/devices/srv-1/activate"), ("GET", "/api/v1/events")):
            status, data, headers = c.request(method, path, {} if method == "POST" else None)
            self.assertEqual(status, 401, path)
            self.assertEqual(data["error"]["code"], "unauthenticated")
            self.assertTrue(data["error"]["requestId"])
            self.assertEqual(headers["x-request-id"], data["error"]["requestId"])

    def test_wrong_password(self):
        status, data = self.client().login("admin", "nope-nope-nope")
        self.assertEqual(status, 401)
        self.assertEqual(data["error"]["code"], "invalid_credentials")

    def test_login_cookie_flags(self):
        c = Client(self.h.port)
        conn = http.client.HTTPConnection("127.0.0.1", self.h.port)
        conn.request("POST", "/api/v1/session", json.dumps({"username": "viewer", "password": PASSWORD}),
                     {"Content-Type": "application/json"})
        resp = conn.getresponse()
        resp.read()
        cookie = resp.getheader("Set-Cookie")
        self.assertIn("HttpOnly", cookie)
        self.assertIn("SameSite=Strict", cookie)
        self.assertNotIn("Secure", cookie)  # plain http, cookie_secure=auto
        conn.close()
        del c

    def test_secure_cookie_behind_https_proxy(self):
        self.h.app.http.trusted = []  # not trusted: header ignored
        c = Client(self.h.port)
        conn = http.client.HTTPConnection("127.0.0.1", self.h.port)
        conn.request("POST", "/api/v1/session", json.dumps({"username": "viewer", "password": PASSWORD}),
                     {"Content-Type": "application/json", "X-Forwarded-Proto": "https"})
        resp = conn.getresponse()
        resp.read()
        self.assertNotIn("Secure", resp.getheader("Set-Cookie"))
        import ipaddress
        self.h.app.http.trusted = [ipaddress.ip_network("127.0.0.1/32")]
        conn.request("POST", "/api/v1/session", json.dumps({"username": "viewer", "password": PASSWORD}),
                     {"Content-Type": "application/json", "X-Forwarded-Proto": "https", "X-Forwarded-Host": "kvm.example"})
        resp = conn.getresponse()
        resp.read()
        self.assertIn("Secure", resp.getheader("Set-Cookie"))
        self.assertIn("max-age", resp.getheader("Strict-Transport-Security"))
        self.h.app.http.trusted = []
        conn.close()
        del c

    def test_csrf_and_origin_are_enforced(self):
        c = self.client("operator")
        status, data, _ = c.request("POST", "/api/v1/control/lock", {"mode": "on"}, csrf=False)
        self.assertEqual((status, data["error"]["code"]), (403, "csrf_failed"))
        status, data, _ = c.request("POST", "/api/v1/control/lock", {"mode": "on"},
                                    headers={"Origin": "http://evil.example"})
        self.assertEqual((status, data["error"]["code"]), (403, "bad_origin"))
        status, data, _ = c.request("POST", "/api/v1/control/lock", {"mode": "off"},
                                    headers={"Origin": "http://127.0.0.1:%d" % self.h.port})
        self.assertEqual(status, 200)

    def test_roles(self):
        v = self.client("viewer")
        self.assertEqual(v.request("GET", "/api/v1/devices")[0], 200)
        status, data, _ = v.request("POST", "/api/v1/devices/srv-1/activate", {})
        self.assertEqual((status, data["error"]["code"]), (403, "forbidden"))
        self.assertEqual(v.request("GET", "/api/v1/logs")[0], 403)
        o = self.client("operator")
        self.assertEqual(o.request("POST", "/api/v1/devices/srv-1/activate", {})[0], 200)
        status, gen, _ = o.request("GET", "/api/v1/config/general")
        self.assertEqual(o.request("PATCH", "/api/v1/config/general", {"port": 24801},
                                   headers={"If-Match": gen["etag"]})[0], 403)
        self.assertEqual(o.request("GET", "/api/v1/users")[0], 403)
        self.assertEqual(o.request("GET", "/api/v1/audit")[0], 403)
        entries = self.h.app.audit.read(50, action="denied:")
        self.assertTrue(any(e["user"] == "operator" for e in entries))

    def test_logout_invalidates_session(self):
        c = self.client("viewer")
        self.assertEqual(c.request("DELETE", "/api/v1/session")[0], 204)
        self.assertEqual(c.request("GET", "/api/v1/status")[0], 401)

    def test_session_idle_expiry(self):
        c = self.client("viewer")
        sid = c.cookie.split("=", 1)[1]
        self.h.app.sessions._sessions[sid].last_seen -= 4000
        self.assertEqual(c.request("GET", "/api/v1/status")[0], 401)

    # -- 2. reading -----------------------------------------------------------
    def test_read_status_devices_sessions_config(self):
        c = self.client("viewer")
        status, st, _ = c.request("GET", "/api/v1/status")
        self.assertEqual(status, 200)
        self.assertEqual(st["backend"]["state"], "connected")
        self.assertFalse(st["stale"])
        status, dev, _ = c.request("GET", "/api/v1/devices")
        self.assertEqual(status, 200)
        self.assertTrue(any(d["isServer"] for d in dev["devices"]))
        status, ses, _ = c.request("GET", "/api/v1/sessions")
        self.assertEqual(status, 200)
        self.assertIn("kvm", ses)
        self.assertNotIn("web", ses)  # only admins see web sessions
        status, gen, headers = c.request("GET", "/api/v1/config/general")
        self.assertEqual(headers["etag"], gen["etag"])
        self.assertEqual(headers["cache-control"], "no-store")

    def test_get_does_not_change_state(self):
        c = self.client("admin")
        before = self.h.fake.received[:]
        before_file = os.path.exists(self.h.settings.settings_file) and open(self.h.settings.settings_file).read()
        for path in ("/api/v1/status", "/api/v1/devices", "/api/v1/config/general", "/api/v1/config/layout",
                     "/api/v1/config/export", "/api/v1/sessions", "/api/v1/security/fingerprints", "/api/v1/meta"):
            self.assertEqual(c.request("GET", path)[0], 200, path)
        mutating = [cmd for cmd, _ in self.h.fake.received[len(before):] if cmd not in ("status", "ping")]
        self.assertEqual(mutating, [])
        after_file = os.path.exists(self.h.settings.settings_file) and open(self.h.settings.settings_file).read()
        self.assertEqual(before_file, after_file)

    def test_unknown_endpoint_and_method(self):
        c = self.client("viewer")
        status, data, _ = c.request("GET", "/api/v1/nope")
        self.assertEqual((status, data["error"]["code"]), (404, "not_found"))
        status, data, headers = c.request("DELETE", "/api/v1/status")
        self.assertEqual(status, 405)
        self.assertIn("GET", headers["allow"])

    # -- configuration -------------------------------------------------------------
    def test_update_requires_if_match(self):
        c = self.client("admin")
        status, data, _ = c.request("PATCH", "/api/v1/config/general", {"port": 24810})
        self.assertEqual((status, data["error"]["code"]), (428, "precondition_required"))

    def test_update_and_conflict(self):
        c = self.client("admin")
        _, gen, _ = c.request("GET", "/api/v1/config/general")
        etag = gen["etag"]
        status, saved, _ = c.request("PATCH", "/api/v1/config/general", {"logLevel": "DEBUG"}, headers={"If-Match": etag})
        self.assertEqual(status, 200)
        self.assertEqual(saved["general"]["logLevel"], "DEBUG")
        self.assertNotEqual(saved["etag"], etag)
        # stale form: the old ETag must not overwrite the newer change
        status, data, _ = c.request("PATCH", "/api/v1/config/general", {"logLevel": "ERROR"}, headers={"If-Match": etag})
        self.assertEqual((status, data["error"]["code"]), (412, "conflict"))
        self.assertEqual(data["error"]["details"]["current"]["logLevel"], "DEBUG")
        self.assertEqual(data["error"]["details"]["etag"], saved["etag"])

    def test_conflict_with_desktop_style_write(self):
        c = self.client("admin")
        _, lay, _ = c.request("GET", "/api/v1/config/layout")
        # the desktop panel writes the shared file directly
        store = SharedConfigStore(self.h.settings.settings_file, self.h.settings.gui_state_file)
        snap = store.snapshot()
        layout = snap["layout"]
        layout["options"]["heartbeat"] = {"enabled": True, "ms": 4321}
        store.replace_layout(layout, snap["etags"]["layout"])
        status, data, _ = c.request("PUT", "/api/v1/config/layout", lay["layout"], headers={"If-Match": lay["etag"]})
        self.assertEqual(status, 412)
        self.assertEqual(data["error"]["details"]["current"]["options"]["heartbeat"]["ms"], 4321)

    def test_validation_errors(self):
        c = self.client("admin")
        _, gen, _ = c.request("GET", "/api/v1/config/general")
        status, data, _ = c.request("PATCH", "/api/v1/config/general", {"port": 70000, "logLevel": "LOUD", "bogus": 1},
                                    headers={"If-Match": gen["etag"]})
        self.assertEqual(status, 422)
        fields = {d["field"] for d in data["error"]["details"]}
        self.assertEqual(fields, {"port", "logLevel", "bogus"})
        _, lay, _ = c.request("GET", "/api/v1/config/layout")
        bad = json.loads(json.dumps(lay["layout"]))
        bad["screens"].append({"name": "bad name!"})
        bad["hotkeys"] = [{"keys": ["Control"], "actions": []}]
        status, data, _ = c.request("PUT", "/api/v1/config/layout", bad, headers={"If-Match": lay["etag"]})
        self.assertEqual(status, 422)
        fields = {d["field"] for d in data["error"]["details"]}
        self.assertIn("screens[1].name", fields)
        self.assertIn("hotkeys[0].keys", fields)

    def test_screen_add_delete_and_server_screen_protected(self):
        c = self.client("admin")
        _, lay, _ = c.request("GET", "/api/v1/config/layout")
        status, r, _ = c.request("POST", "/api/v1/config/layout/screens", {"name": "laptop"}, headers={"If-Match": lay["etag"]})
        self.assertEqual(status, 201)
        self.assertIn("laptop-1", [s["name"] for s in r["layout"]["screens"]])
        status, r2, _ = c.request("DELETE", "/api/v1/config/layout/screens/laptop-1", headers={"If-Match": r["etag"]})
        self.assertEqual(status, 200)
        server = r2["serverScreen"]
        status, data, _ = c.request("DELETE", "/api/v1/config/layout/screens/%s" % server, headers={"If-Match": r2["etag"]})
        self.assertEqual(status, 422)

    def test_export_import_roundtrip(self):
        c = self.client("admin")
        status, exported, headers = c.request("GET", "/api/v1/config/export?format=json")
        self.assertEqual(status, 200)
        self.assertIn("attachment", headers["content-disposition"])
        _, gen, _ = c.request("GET", "/api/v1/config/general")
        _, lay, _ = c.request("GET", "/api/v1/config/layout")
        exported["general"]["logLevel"] = "WARNING"
        status, data, _ = c.request("POST", "/api/v1/config/import", dict(exported, expectedEtags={"general": "x", "layout": lay["etag"]}))
        self.assertEqual(status, 412)
        status, data, _ = c.request("POST", "/api/v1/config/import", dict(exported, expectedEtags={"general": gen["etag"], "layout": lay["etag"]}))
        self.assertEqual(status, 200, data)
        self.assertEqual(data["general"]["logLevel"], "WARNING")
        status, conf, _ = c.request("GET", "/api/v1/config/export?format=conf")
        self.assertIn("section: screens", conf)

    # -- actions, jobs, backend failures -----------------------------------------
    def test_switch_errors_are_mapped(self):
        c = self.client("operator")
        status, data, _ = c.request("POST", "/api/v1/devices/nope-1/activate", {})
        self.assertEqual((status, data["error"]["code"]), (404, "not_found"))

    def test_timeout_is_reported_as_uncertain_never_success(self):
        c = self.client("operator")
        self.h.fake.drop = {"switch"}
        try:
            status, data, _ = c.request("POST", "/api/v1/devices/srv-1/activate", {})
        finally:
            self.h.fake.drop = set()
        self.assertEqual((status, data["error"]["code"]), (504, "outcome_unknown"))
        entries = self.h.app.audit.read(20, action="control.switch")
        self.assertEqual(entries[0]["result"], "uncertain")

    def test_long_running_apply_job(self):
        c = self.client("admin")
        _, gen, _ = c.request("GET", "/api/v1/config/general")
        c.request("PATCH", "/api/v1/config/general", {"mode": "server"}, headers={"If-Match": gen["etag"]})
        self.h.fake.delay = {"config.apply": 0.5}
        try:
            status, data, headers = c.request("POST", "/api/v1/config/apply", {})
            self.assertEqual(status, 202)
            self.assertIn("/api/v1/jobs/", headers["location"])
            job_id = data["job"]["id"]
            states = set()
            for _ in range(100):
                _, j, _ = c.request("GET", "/api/v1/jobs/" + job_id)
                states.add(j["job"]["state"])
                if j["job"]["state"] in ("succeeded", "failed", "uncertain"):
                    break
                time.sleep(0.05)
        finally:
            self.h.fake.delay = {}
        self.assertIn("running", states)
        self.assertEqual(j["job"]["state"], "succeeded", j)
        self.assertIn("section: screens", self.h.fake.state["config"])

    def test_parallel_requests_from_many_tabs(self):
        clients = [self.client("operator") for _ in range(4)]
        self.h.fake.state["clients"] = ["srv-1", "a-1", "b-1"]
        results = []

        def worker(c, target):
            for _ in range(5):
                results.append(c.request("POST", "/api/v1/devices/%s/activate" % target, {})[0])
                results.append(c.request("GET", "/api/v1/status")[0])

        threads = [threading.Thread(target=worker, args=(c, t)) for c, t in zip(clients, ["a-1", "b-1", "srv-1", "a-1"])]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        self.h.fake.state["clients"] = ["srv-1"]
        self.assertEqual(set(results), {200})
        self.assertEqual(self.h.app.control.stats["late_responses"], 0)

    def test_backend_down_is_503_and_status_is_stale(self):
        c = self.client("operator")
        self.h.run(self.h.fake.stop())
        self.h.run(self.h.fake.kick_all())
        time.sleep(0.3)
        try:
            status, data, headers = c.request("POST", "/api/v1/control/lock", {"mode": "on"})
            self.assertEqual((status, data["error"]["code"]), (503, "backend_unavailable"))
            self.assertIn("retry-after", headers)
            _, st, _ = c.request("GET", "/api/v1/status")
            self.assertTrue(st["stale"])
            self.assertIsNone(st["server"])
            self.assertEqual(c.request("GET", "/readyz")[0], 503)
        finally:
            port = self.h.fake.port

            async def restart():
                self.h.fake._server = await asyncio.start_server(self.h.fake._handle, "127.0.0.1", port)
            self.h.run(restart())
            self.h.app.control.reconnect_now()
            deadline = time.time() + 5
            while not self.h.app.control.connected and time.time() < deadline:
                time.sleep(0.05)
        self.assertTrue(self.h.app.control.connected)
        self.assertEqual(c.request("GET", "/readyz")[0], 200)

    def test_service_management_disabled(self):
        c = self.client("operator")
        status, data, _ = c.request("POST", "/api/v1/service/start", {})
        self.assertEqual((status, data["error"]["code"]), (409, "not_managed"))

    # -- users and audit ------------------------------------------------------
    def test_user_management_and_last_admin(self):
        c = self.client("admin")
        status, _, _ = c.request("POST", "/api/v1/users", {"username": "temp", "password": "short", "role": "viewer"})
        self.assertEqual(status, 422)
        self.assertEqual(c.request("POST", "/api/v1/users", {"username": "temp", "password": PASSWORD, "role": "viewer"})[0], 201)
        self.assertEqual(c.request("POST", "/api/v1/users", {"username": "temp", "password": PASSWORD, "role": "viewer"})[0], 409)
        self.assertEqual(c.request("DELETE", "/api/v1/users/temp")[0], 204)
        status, data, _ = c.request("PATCH", "/api/v1/users/admin", {"role": "viewer"})
        self.assertEqual((status, data["error"]["code"]), (409, "last_admin"))

    def test_audit_has_no_secrets(self):
        c = self.client("admin")
        c.request("POST", "/api/v1/session/password", {"currentPassword": "wrong-password", "newPassword": "x" * 12})
        with open(self.h.app.audit.path) as fh:
            text = fh.read()
        self.assertNotIn(PASSWORD, text)
        self.assertNotIn("wrong-password", text)
        self.assertNotIn(c.csrf, text)
        self.assertNotIn(c.cookie.split("=", 1)[1], text)
        status, data, _ = c.request("GET", "/api/v1/audit?limit=5")
        self.assertEqual(status, 200)
        e = data["entries"][0]
        for key in ("ts", "user", "action", "resource", "result", "requestId"):
            self.assertIn(key, e)

    def test_no_secrets_in_responses(self):
        c = self.client("admin")
        for path in ("/api/v1/meta", "/api/v1/status", "/api/v1/diagnostics", "/api/v1/users"):
            _, data, _ = c.request("GET", path)
            text = json.dumps(data)
            self.assertNotIn(self.h.fake.token, text, path)
            self.assertNotIn("scrypt$", text, path)

    # -- live events -----------------------------------------------------------
    def _sse(self, c, last_id=None, seconds=1.0):
        sock = socket.create_connection(("127.0.0.1", self.h.port))
        req = "GET /api/v1/events HTTP/1.1\r\nHost: 127.0.0.1\r\nCookie: %s\r\n" % c.cookie
        if last_id:
            req += "Last-Event-ID: %s\r\n" % last_id
        sock.sendall((req + "\r\n").encode())
        sock.settimeout(0.2)
        buf = b""
        deadline = time.time() + seconds
        while time.time() < deadline:
            try:
                chunk = sock.recv(65536)
            except (socket.timeout, TimeoutError):
                continue
            if not chunk:
                break
            buf += chunk
        sock.close()
        head, _, body = buf.partition(b"\r\n\r\n")
        self.assertIn(b"200 OK", head.split(b"\r\n")[0])
        self.assertIn(b"text/event-stream", head)
        events = []
        for block in body.decode().split("\n\n"):
            fields = dict(line.split(": ", 1) for line in block.split("\n") if ": " in line)
            if "event" in fields:
                events.append((fields.get("id"), fields["event"], json.loads(fields["data"])))
        return events

    def test_sse_initial_resync_and_replay(self):
        c = self.client("viewer")
        events = self._sse(c, seconds=0.5)
        names = [e[1] for e in events]
        self.assertEqual(names[:2], ["hello", "resync"])
        last_id = events[-1][0]
        self.h.app.hub.publish("server.event", {"event": "client.connected", "data": {"name": "x-1"}}, "status:read")
        self.h.app.hub.publish("log", {"message": "secret-ish"}, "logs:read")  # viewer must not get logs
        replay = self._sse(c, last_id=last_id, seconds=0.5)
        names = [e[1] for e in replay]
        self.assertIn("server.event", names)
        self.assertNotIn("resync", names)
        self.assertNotIn("log", names)
        stale = self._sse(c, last_id="deadbeef-1", seconds=0.5)
        self.assertIn("resync", [e[1] for e in stale])

    def test_static_security_headers(self):
        c = self.client()
        status, body, headers = c.request("GET", "/")
        if status == 200:
            self.assertIn("default-src 'self'", headers["content-security-policy"])
            self.assertIn("frame-ancestors 'none'", headers["content-security-policy"])
        self.assertEqual(headers["x-content-type-options"], "nosniff")
        status, _, _ = c.request("GET", "/../../etc/passwd")
        self.assertIn(status, (400, 404))

    def test_health(self):
        c = self.client()
        self.assertEqual(c.request("GET", "/healthz")[0], 200)
        status, data, _ = c.request("GET", "/readyz")
        self.assertEqual(status, 200)
        self.assertTrue(data["ready"])


if __name__ == "__main__":
    unittest.main()

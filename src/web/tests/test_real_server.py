"""Integration tests against the real Etherwaver server (wavers) and client.

Needs the built binaries and Xvfb.  Point WAVER_BIN_DIR at the directory
holding ``wavers`` and ``waverc`` (default: <repo>/build/bin).  Skipped when
they are not available.
"""

import asyncio
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from waver_web import model  # noqa: E402
from waver_web.control_client import ControlClient, ControlError  # noqa: E402

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
BIN = os.environ.get("WAVER_BIN_DIR", os.path.join(REPO, "build", "bin"))
WAVERS = os.path.join(BIN, "wavers")
WAVERC = os.path.join(BIN, "waverc")
AVAILABLE = os.path.exists(WAVERS) and os.path.exists(WAVERC) and shutil.which("Xvfb")


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def free_display():
    for n in range(140, 200):
        if not os.path.exists("/tmp/.X11-unix/X%d" % n) and not os.path.exists("/tmp/.X%d-lock" % n):
            return ":%d" % n
    raise RuntimeError("no free X display")


class Line:
    def __init__(self, port, timeout=5):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.buf = b""

    def send_raw(self, data):
        self.sock.sendall(data)

    def send(self, obj):
        self.send_raw((json.dumps(obj) + "\n").encode())

    def recv(self):
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("closed")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line)

    def recv_response(self, rid, events=None):
        while True:
            m = self.recv()
            if m.get("type") == "response" and m.get("id") == rid:
                return m
            if events is not None and m.get("type") == "event":
                events.append(m)

    def close(self):
        self.sock.close()


@unittest.skipUnless(AVAILABLE, "wavers/waverc binaries or Xvfb not available")
class RealServerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="ew-real-")
        cls.procs = []
        cls.d1, cls.d2 = free_display(), None
        cls.procs.append(subprocess.Popen(["Xvfb", cls.d1, "-screen", "0", "1920x1080x24"],
                                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        time.sleep(0.5)
        cls.d2 = free_display()
        cls.procs.append(subprocess.Popen(["Xvfb", cls.d2, "-screen", "0", "1280x800x24"],
                                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        time.sleep(0.5)
        cls.port = free_port()
        cls.cport = free_port()
        cls.conf = os.path.join(cls.tmp, "etherwaver.conf")
        layout = model.validate_layout({"screens": [model.new_screen("srv-1", 0, 0), model.new_screen("cli-1", 300, 0)]}, "srv")
        cls.layout = layout
        with open(cls.conf, "w") as fh:
            fh.write(model.server_config_text(layout))
        cls.profile = os.path.join(cls.tmp, "profile")
        env = dict(os.environ, DISPLAY=cls.d1)
        cls.server = subprocess.Popen([WAVERS, "-f", "--no-tray", "--debug", "INFO", "--name", "srv", "-c", cls.conf,
                                       "--disable-crypto", "--address", "127.0.0.1:%d" % cls.port,
                                       "--profile-dir", cls.profile, "--control-address", "127.0.0.1:%d" % cls.cport],
                                      stdout=open(os.path.join(cls.tmp, "server.log"), "w"), stderr=subprocess.STDOUT, env=env)
        cls.procs.append(cls.server)
        token_file = os.path.join(cls.profile, "control-token")
        deadline = time.time() + 10
        while time.time() < deadline:
            try:
                socket.create_connection(("127.0.0.1", cls.cport), timeout=0.2).close()
                if os.path.exists(token_file):
                    break
            except OSError:
                time.sleep(0.1)
        cls.token = open(token_file).read().strip()

    @classmethod
    def tearDownClass(cls):
        for p in reversed(cls.procs):
            p.terminate()
            try:
                p.wait(5)
            except subprocess.TimeoutExpired:
                p.kill()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def connect(self, auth=True):
        c = Line(self.cport)
        hello = c.recv()
        self.assertEqual(hello["protocol"], "etherwaver-control")
        if auth:
            c.send({"id": 0, "cmd": "auth", "token": self.token})
            self.assertTrue(c.recv_response(0)["ok"])
        return c

    def test_token_file_is_private(self):
        mode = os.stat(os.path.join(self.profile, "control-token")).st_mode & 0o777
        self.assertEqual(mode, 0o600)

    def test_requires_auth(self):
        c = self.connect(auth=False)
        c.send({"id": 1, "cmd": "status"})
        self.assertEqual(c.recv_response(1)["error"]["code"], "unauthorized")
        for i in range(3):
            c.send({"id": 10 + i, "cmd": "auth", "token": "wrong"})
            self.assertFalse(c.recv_response(10 + i)["ok"])
        with self.assertRaises((ConnectionError, OSError)):
            c.send({"id": 20, "cmd": "ping"})
            c.recv_response(20)

    def test_request_split_into_single_bytes(self):
        c = self.connect()
        data = (json.dumps({"id": 7, "cmd": "status"}) + "\n").encode()
        for i in range(len(data)):
            c.send_raw(data[i:i + 1])
            time.sleep(0.002)
        r = c.recv_response(7)
        self.assertTrue(r["ok"])
        self.assertEqual(r["result"]["server"], "srv-1")

    def test_many_requests_in_one_write(self):
        c = self.connect()
        burst = b"".join((json.dumps({"id": i, "cmd": "ping" if i % 2 else "status"}) + "\n").encode() for i in range(1, 11))
        c.send_raw(burst)
        seen = {}
        while len(seen) < 10:
            m = c.recv()
            if m["type"] == "response":
                seen[m["id"]] = m
        self.assertEqual(sorted(seen), list(range(1, 11)))
        self.assertEqual(seen[3]["result"], {"pong": True})
        self.assertIn("clients", seen[4]["result"])

    def test_errors(self):
        c = self.connect()
        c.send_raw(b"this is not json\n")
        self.assertEqual(c.recv()["error"]["code"], "bad_request")
        c.send({"id": "s", "cmd": "nope"})
        self.assertEqual(c.recv_response("s")["error"]["code"], "unknown_command")
        c.send({"id": 2, "cmd": "switch", "screen": "ghost"})
        self.assertEqual(c.recv_response(2)["error"]["code"], "not_found")
        c.send({"id": 3, "cmd": "switch", "screen": "cli-1"})
        self.assertEqual(c.recv_response(3)["error"]["code"], "not_connected")
        c.send({"id": 4, "cmd": "lock", "mode": "sideways"})
        self.assertEqual(c.recv_response(4)["error"]["code"], "bad_request")

    def test_line_too_long_closes_connection(self):
        c = self.connect()
        c.send_raw(b'{"id":1,"cmd":"ping","x":"' + b"a" * (1024 * 1024 + 10))
        self.assertEqual(c.recv()["error"]["code"], "bad_request")

    def test_config_apply_validation(self):
        c = self.connect()
        c.send({"id": 1, "cmd": "config.apply", "config": "section: screens\n\tother-1:\nend\n", "persist": False})
        self.assertEqual(c.recv_response(1)["error"]["code"], "missing_primary_screen")
        c.send({"id": 2, "cmd": "config.apply", "config": "section: bogus\n", "persist": False})
        self.assertEqual(c.recv_response(2)["error"]["code"], "invalid_config")
        layout = model.deep_copy(self.layout)
        layout["screens"].append(model.new_screen("tv-1", 600, 0))
        layout = model.validate_layout(layout, "srv")
        c.send({"id": 3, "cmd": "subscribe"})
        c.recv_response(3)
        c.send({"id": 4, "cmd": "config.apply", "config": model.server_config_text(layout),
                "layout": model.layout_json_text(layout)})
        events = []
        r = c.recv_response(4, events)
        self.assertTrue(r["ok"], r)
        self.assertTrue(r["result"]["persisted"])
        c.send({"id": 5, "cmd": "status"})
        st = c.recv_response(5, events)["result"]
        self.assertIn("tv-1", st["configuredScreens"])
        self.assertIn("config.applied", [e["event"] for e in events])
        with open(self.conf) as fh:
            self.assertIn("tv-1", fh.read())
        # restore
        c.send({"id": 6, "cmd": "config.apply", "config": model.server_config_text(self.layout),
                "layout": model.layout_json_text(self.layout)})
        self.assertTrue(c.recv_response(6)["ok"])

    def test_client_events_and_switch_with_adapter(self):
        async def run():
            events = []
            client = ControlClient("127.0.0.1", self.cport, lambda: self.token,
                                   on_event=lambda e, d, s: events.append((e, d)))
            client.start()
            self.assertTrue(await client.wait_connected(5))
            env = dict(os.environ, DISPLAY=self.d2)
            proc = subprocess.Popen([WAVERC, "-f", "--no-tray", "--debug", "INFO", "--name", "cli", "--disable-crypto",
                                     "127.0.0.1:%d" % self.port], stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT, env=env)
            try:
                for _ in range(100):
                    if any(e == "client.connected" for e, _ in events):
                        break
                    # keep requests flowing so events interleave with responses
                    await asyncio.gather(*[client.request("status") for _ in range(3)])
                    await asyncio.sleep(0.05)
                self.assertIn(("client.connected", {"name": "cli-1"}), events)
                r = await client.request("switch", {"screen": "cli-1"})
                self.assertEqual(r["active"]["client"], "cli-1")
                st = await client.request("status")
                self.assertEqual(st["active"]["client"], "cli-1")
                r = await client.request("lock", {"mode": "on"})
                self.assertTrue(r["locked"])
                r = await client.request("lock", {"mode": "off"})
                self.assertFalse(r["locked"])
                r = await client.request("switch", {"screen": "srv-1"})
                self.assertEqual(r["active"]["client"], "srv-1")
            finally:
                proc.terminate()
                proc.wait(5)
            for _ in range(100):
                if any(e == "client.disconnected" for e, _ in events):
                    break
                await asyncio.sleep(0.05)
            self.assertIn(("client.disconnected", {"name": "cli-1"}), events)
            self.assertIn("screen.active", [e for e, _ in events])
            self.assertEqual(client.stats["late_responses"], 0)
            await client.stop()

        asyncio.run(run())


if __name__ == "__main__":
    unittest.main()

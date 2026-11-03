"""Web <-> desktop panel compatibility, using the real desktop panel classes.

tests/qt_compat/build.sh compiles AppConfig, ServerConfig, Screen, Hotkey,
Action and KeySequence from src/gui/src into a small command line tool.  The
tests write the shared settings file with the web backend and read it with
the desktop code (and the other way round).  Skipped when Qt 5 development
files are not installed; set DESKTOP_CONFIG_HARNESS to a prebuilt binary to
avoid rebuilding.
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from waver_web import model  # noqa: E402
from waver_web.store import ConflictError, SharedConfigStore  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))


def harness():
    explicit = os.environ.get("DESKTOP_CONFIG_HARNESS")
    if explicit:
        return explicit if os.path.exists(explicit) else None
    if not shutil.which("moc") or subprocess.run(["pkg-config", "--exists", "Qt5Widgets"]).returncode != 0:
        return None
    out = os.path.join(tempfile.gettempdir(), "waver-web-qt-harness")
    binary = os.path.join(out, "desktop_config")
    if not os.path.exists(binary):
        r = subprocess.run(["sh", os.path.join(HERE, "qt_compat", "build.sh"), out], capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError("cannot build the desktop harness:\n" + r.stderr[-3000:])
    return binary


HARNESS = harness()


@unittest.skipUnless(HARNESS, "Qt 5 development files not available")
class DesktopCompatTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.path = os.path.join(self.dir, "EtherWaver.conf")
        self.store = SharedConfigStore(self.path, os.path.join(self.dir, "gui-state.ini"))

    def desktop(self, *args):
        env = dict(os.environ, QT_QPA_PLATFORM="offscreen", XDG_RUNTIME_DIR=self.dir)
        r = subprocess.run([HARNESS, args[0], self.path] + list(args[1:]), capture_output=True, text=True, env=env)
        self.assertEqual(r.returncode, 0, r.stderr)
        return json.loads(r.stdout) if args[0] == "dump" else None

    def test_web_change_is_read_by_desktop(self):
        snap = self.store.snapshot()
        snap = self.store.update_general({"screenName": "deskhost", "logLevel": "DEBUG", "port": 24811,
                                          "uhidName": "Pad, \"quoted\" ż"}, snap["etags"]["general"])
        layout = snap["layout"]
        laptop = model.new_screen("laptop", 300, 0)
        laptop["aliases"] = ["nb"]
        laptop["modifiers"]["alt"] = "super"
        layout["screens"].append(laptop)
        layout["options"]["useCustomLinks"] = True
        layout["screens"][0]["links"]["right"] = "laptop-1"
        laptop["links"]["left"] = layout["screens"][0]["name"]
        layout["hotkeys"] = [{"keys": ["Control", "Shift", "F5"], "actions": [{"type": "switchToScreen", "screen": "laptop-1"}]},
                             {"keys": ["2"], "mouse": True, "actions": [{"type": "lockCursorToScreen", "lockMode": "toggle"}]}]
        snap = self.store.replace_layout(layout, snap["etags"]["layout"])

        out = self.desktop("dump")
        self.assertEqual(out["general"]["screenName"], "deskhost")
        self.assertEqual(out["general"]["logLevel"], "DEBUG")
        self.assertEqual(out["general"]["port"], 24811)
        self.assertEqual(out["general"]["uhidName"], "Pad, \"quoted\" ż")
        names = [s["name"] for s in out["screens"]]
        self.assertEqual(names, ["deskhost-1", "laptop-1"])
        self.assertEqual(out["screens"][1]["aliases"], ["nb"])
        self.assertEqual(out["screens"][0]["linkRight"], "laptop-1")
        self.assertEqual(out["hotkeys"], ["keystroke(Control+Shift+F5) = switchToScreen(laptop-1)",
                                          "mousebutton(2) = lockCursorToScreen(toggle)"])
        # the .conf the desktop panel would give to wavers is identical
        self.assertEqual(out["serverConfigText"], model.server_config_text(snap["layout"]))

    def test_desktop_change_is_read_by_web_and_detected_as_conflict(self):
        before = self.store.snapshot()
        self.desktop("set-heartbeat", "7000")   # like ServerConfigDialog::accept
        self.desktop("set-log-level", "1")      # like SettingsDialog::accept
        self.desktop("add-screen", "tv-1")
        after = self.store.snapshot()
        self.assertEqual(after["layout"]["options"]["heartbeat"], {"enabled": True, "ms": 7000})
        self.assertEqual(after["general"]["logLevel"], "WARNING")
        self.assertIn("tv-1", [s["name"] for s in after["layout"]["screens"]])
        # a web form opened before the desktop change must not overwrite it
        with self.assertRaises(ConflictError):
            self.store.replace_layout(before["layout"], before["etags"]["layout"])
        with self.assertRaises(ConflictError):
            self.store.update_general({"logLevel": "INFO"}, before["etags"]["general"])

    def test_desktop_rewrite_does_not_change_etag(self):
        snap = self.store.update_general({"logLevel": "NOTE"}, None)
        self.desktop("dump")  # loads and (via QSettings) may rewrite nothing
        self.assertEqual(self.store.snapshot()["etags"]["layout"], snap["etags"]["layout"])


if __name__ == "__main__":
    unittest.main()

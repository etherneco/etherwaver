import os
import sys
import tempfile
import threading
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from waver_web import model  # noqa: E402
from waver_web.events import EventHub  # noqa: E402
from waver_web.qsettings import (LockTimeout, QSettingsDocument, QtLockFile, escape_key, escape_string,  # noqa: E402
                                 unescape_key, unescape_value)
from waver_web.store import ConflictError, SharedConfigStore  # noqa: E402

# Written by Qt 5.15 QSettings (IniFormat) - see tests/qt_compat
QT_SAMPLE = r"""[General]
Upper=u
at=@@home
b=true
bs=a\\b
comma="a,b"
empty=
eq="k=v"
i=42
lead=" sp "
nl=a\nb
quote=say \"hi\"
semi="x;y"
uni=za\x17c\xf3\x142\x107
weird%20key=1

[internalConfig]
screens\1\name=a-1
screens\2\name=
screens\size=2
"""


class QSettingsTests(unittest.TestCase):
    def test_parse_qt_written_file(self):
        doc = QSettingsDocument.parse(QT_SAMPLE)
        expect = {"Upper": "u", "at": "@home", "b": "true", "bs": "a\\b", "comma": "a,b", "empty": "",
                  "eq": "k=v", "i": "42", "lead": " sp ", "nl": "a\nb", "quote": 'say "hi"', "semi": "x;y",
                  "uni": "zażółć", "weird key": "1", "internalConfig/screens/1/name": "a-1",
                  "internalConfig/screens/size": "2"}
        for k, v in expect.items():
            self.assertEqual(doc.get(k), v, k)

    def test_serialize_matches_qt_byte_for_byte(self):
        doc = QSettingsDocument.parse(QT_SAMPLE)
        self.assertEqual(doc.serialize(), QT_SAMPLE)

    def test_string_roundtrip(self):
        for value in ["", "plain", "a,b", " lead", "trail ", "x=y", "q\"q", "back\\slash", "@at", "@@double",
                      "nl\nnl\tt", "ąęś1abc", "中文", "semi;colon", "1234", "é"]:
            self.assertEqual(unescape_value(escape_string(value)), value, repr(value))

    def test_key_roundtrip(self):
        for key in ["a/b/c", "weird key", "x%y", "ü"]:
            self.assertEqual(unescape_key(escape_key(key)), key)

    def test_lock_excludes_second_writer_and_detects_stale(self):
        d = tempfile.mkdtemp()
        path = os.path.join(d, "s.conf.lock")
        with QtLockFile(path):
            with self.assertRaises(LockTimeout):
                QtLockFile(path, timeout=0.2).acquire()
        # stale lock from a dead process is taken over
        with open(path, "w") as fh:
            fh.write("999999\nqt\n%s\n" % os.uname().nodename)
        with QtLockFile(path, timeout=0.5):
            pass
        self.assertFalse(os.path.exists(path))


class ModelTests(unittest.TestCase):
    def test_key_names_roundtrip(self):
        for name in ["Control", "Alt", "Shift", "Meta", "a", "z", "1", "F1", "F12", "F35", "Left", "Space",
                     "BackSpace", "Return", "KP_Enter", "Comma", "AudioMute", "\\u00e9"]:
            code = model.string_to_key(name)
            self.assertIsNotNone(code, name)
            self.assertEqual(model.key_to_string(code), name)

    def test_sequence_rules(self):
        errors = model._Errors()
        self.assertEqual(model.sequence_to_qt({"keys": ["Control", "Alt", "Left"]}, "f", errors),
                         [model.QT_CTRL, model.QT_ALT, 0x01000012])
        self.assertFalse(errors.items)
        for bad in (["Control"], ["a", "Control"], ["a", "b"], ["Control", "Control", "a"], ["nope"], []):
            e = model._Errors()
            model.sequence_to_qt({"keys": bad}, "f", e)
            self.assertTrue(e.items, bad)

    def test_screen_names_get_suffix(self):
        self.assertEqual(model.ensure_screen_number_suffix("laptop"), "laptop-1")
        self.assertEqual(model.ensure_screen_number_suffix("laptop-3"), "laptop-3")
        self.assertEqual(model.base_host_name("laptop-2-1"), "laptop")

    def test_layout_validation_rejects_bad_links_and_duplicates(self):
        lay = {"screens": [model.new_screen("srv-1"), model.new_screen("a-1"), model.new_screen("A-1")]}
        lay["screens"][0]["links"]["right"] = "ghost-1"
        with self.assertRaises(model.ValidationError) as cm:
            model.validate_layout(lay, "srv")
        fields = {e["field"] for e in cm.exception.errors}
        self.assertIn("screens[2].name", fields)
        self.assertIn("screens[0].links.right", fields)

    def test_grid_grows_to_hold_all_screens(self):
        lay = {"grid": {"columns": 2, "rows": 1},
               "screens": [model.new_screen("s%d-1" % i, i * 300, 0) for i in range(5)]}
        clean = model.validate_layout(lay, "s0")
        self.assertGreaterEqual(clean["grid"]["columns"] * clean["grid"]["rows"], 5)

    def test_conf_text_grid_links(self):
        lay = model.validate_layout({"screens": [model.new_screen("a-1", 0, 0), model.new_screen("b-1", 300, 0)],
                                     "grid": {"columns": 5, "rows": 3}}, "a")
        text = model.server_config_text(lay)
        self.assertIn("\ta-1:\n\t\tright = b-1\n", text)
        self.assertIn("\tb-1:\n\t\tleft = a-1\n", text)


class StoreTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.store = SharedConfigStore(os.path.join(self.dir, "cfg", "EtherWaver.conf"),
                                       os.path.join(self.dir, "profile", "gui-state.ini"))

    def test_etags_are_per_resource(self):
        snap = self.store.snapshot()
        snap2 = self.store.update_general({"logLevel": "DEBUG"}, snap["etags"]["general"])
        self.assertEqual(snap["etags"]["layout"], snap2["etags"]["layout"])
        self.assertNotEqual(snap["etags"]["general"], snap2["etags"]["general"])
        # a layout edit based on the old snapshot is still fine
        self.store.replace_layout(snap["layout"], snap["etags"]["layout"])
        with self.assertRaises(ConflictError):
            self.store.update_general({"logLevel": "INFO"}, snap["etags"]["general"])

    def test_reordering_by_qt_is_not_a_change(self):
        snap = self.store.update_general({"logLevel": "NOTE"}, None)
        path = self.store.settings_path
        lines = open(path).read().splitlines()
        # Qt may rewrite keys in another order; values are what matter
        general = [l for l in lines if "=" in l and not l.startswith("[")]
        with open(path, "w") as fh:
            fh.write("[General]\n" + "\n".join(reversed([l for l in general if "\\" not in l])) + "\n")
        snap2 = self.store.snapshot()
        self.assertEqual(snap["etags"]["general"], snap2["etags"]["general"])

    def test_mode_is_written_to_gui_state_too(self):
        self.store.update_general({"mode": "server"}, None)
        state = open(self.store.gui_state_path).read()
        self.assertIn("groupServerChecked=true", state)
        self.assertIn("groupClientChecked=false", state)

    def test_concurrent_writers_serialise(self):
        errors = []

        def writer(level):
            for _ in range(10):
                try:
                    snap = self.store.snapshot()
                    self.store.update_general({"logLevel": level}, snap["etags"]["general"])
                except ConflictError:
                    pass
                except Exception as e:  # pragma: no cover
                    errors.append(e)

        threads = [threading.Thread(target=writer, args=(lv,)) for lv in ("INFO", "DEBUG", "ERROR")]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        self.assertEqual(errors, [])
        self.assertIn(self.store.snapshot()["general"]["logLevel"], ("INFO", "DEBUG", "ERROR"))


class EventHubTests(unittest.TestCase):
    def test_replay_and_resync(self):
        import asyncio

        async def run():
            hub = EventHub(replay=3)
            for i in range(5):
                hub.publish("x", {"i": i})
            _, replay = hub.subscribe("viewer", "%s-%d" % (hub.epoch, 4))
            self.assertEqual([r[1] for r in replay], ["x"])
            _, replay = hub.subscribe("viewer", "%s-%d" % (hub.epoch, 0))  # older than buffer
            self.assertEqual(replay[-1][1], "resync")
            _, replay = hub.subscribe("viewer", "other-5")
            self.assertEqual(replay[-1][1], "resync")
            sub, _ = hub.subscribe("viewer", None)
            hub.queue_size = 1
            small, _ = hub.subscribe("viewer", "%s-%d" % (hub.epoch, hub.seq))
            for i in range(600):
                hub.publish("x", {"i": i})
            self.assertTrue(small.overflowed or sub.overflowed)

        asyncio.run(run())


if __name__ == "__main__":
    unittest.main()

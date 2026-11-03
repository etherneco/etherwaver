"""Etherwaver configuration model shared by the desktop and web panels.

The structures here mirror the desktop panel classes (``AppConfig``,
``ServerConfig``, ``Screen``, ``Hotkey``, ``Action``, ``KeySequence``) and
their persistence in QSettings.  The functions that turn the model into a
server ``.conf`` file and into ``etherwaver-layout.json`` are ports of
``operator<<(QTextStream&, const ServerConfig&)`` and
``ServerConfig::saveLayout`` so a configuration produced by the web panel is
byte-for-byte what the desktop panel would hand to ``wavers``.

All validation of user input happens here (server side).  The browser never
decides what is valid.
"""

from __future__ import annotations

import copy
import hashlib
import json
import re
from typing import Any, Dict, List, Optional, Tuple

from .qsettings import QSettingsDocument

# ---------------------------------------------------------------------------
# errors
# ---------------------------------------------------------------------------


class ValidationError(Exception):
    """Raised with a list of ``{"field": ..., "message": ...}`` problems."""

    def __init__(self, errors: List[Dict[str, str]]):
        super().__init__("; ".join("%s: %s" % (e["field"], e["message"]) for e in errors))
        self.errors = errors


class _Errors:
    def __init__(self) -> None:
        self.items: List[Dict[str, str]] = []

    def add(self, field: str, message: str) -> None:
        self.items.append({"field": field, "message": message})

    def raise_if_any(self) -> None:
        if self.items:
            raise ValidationError(self.items)


# ---------------------------------------------------------------------------
# constants mirrored from the desktop panel
# ---------------------------------------------------------------------------

LOG_LEVELS = ["ERROR", "WARNING", "NOTE", "INFO", "DEBUG", "DEBUG1", "DEBUG2"]
ELEVATE_MODES = ["as_needed", "always", "never"]  # ElevateAsNeeded/Always/Never
MODIFIERS = ["shift", "ctrl", "alt", "meta", "super", "none"]  # BaseConfig::Modifier
FIXES = ["halfDuplexCapsLock", "halfDuplexNumLock", "halfDuplexScrollLock",
         "xtestIsXineramaUnaware", "preserveFocus"]  # BaseConfig::Fix
CORNERS = ["top-left", "top-right", "bottom-left", "bottom-right"]  # SwitchCorner
CORNER_KEYS = ["topLeft", "topRight", "bottomLeft", "bottomRight"]
DIRECTIONS = ["right", "left", "up", "down"]  # Screen::LinkDirection order
LINK_KEYS = {"right": "linkRight", "left": "linkLeft", "up": "linkUp", "down": "linkDown"}

ACTION_TYPES = ["keyDown", "keyUp", "keystroke", "switchToScreen", "toggleScreen",
                "switchInDirection", "lockCursorToScreen"]
ACTION_MOUSE_NAMES = {"keyDown": "mouseDown", "keyUp": "mouseUp", "keystroke": "mousebutton"}
SWITCH_DIRECTIONS = ["left", "right", "up", "down"]  # Action::SwitchDirection
LOCK_MODES = ["toggle", "on", "off"]  # Action::LockCursorMode

SCREEN_NAME_RE = re.compile(r"^[a-z0-9._-]{1,255}$", re.IGNORECASE)
HOSTNAME_RE = re.compile(r"^[A-Za-z0-9._:\[\]%-]{1,253}$")

DEFAULT_CARD_W = 240
DEFAULT_CARD_H = 140

# Qt key codes (verified against Qt 5 headers)
QT_SHIFT = 0x02000000
QT_CTRL = 0x04000000
QT_ALT = 0x08000000
QT_META = 0x10000000
QT_KEYPAD = 0x20000000
QT_KEY_SPACE = 0x20
QT_KEY_F1 = 0x01000030
QT_KEY_F35 = 0x01000052

# KeySequence keyname[] table
SPECIAL_KEYS: List[Tuple[int, str]] = [
    (0x20, "Space"), (0x01000000, "Escape"), (0x01000001, "Tab"),
    (0x01000002, "LeftTab"), (0x01000003, "BackSpace"), (0x01000004, "Return"),
    (0x01000006, "Insert"), (0x01000007, "Delete"), (0x01000008, "Pause"),
    (0x01000009, "Print"), (0x0100000A, "SysReq"), (0x01000010, "Home"),
    (0x01000011, "End"), (0x01000012, "Left"), (0x01000013, "Up"),
    (0x01000014, "Right"), (0x01000015, "Down"), (0x01000016, "PageUp"),
    (0x01000017, "PageDown"), (0x01000024, "CapsLock"), (0x01000025, "NumLock"),
    (0x01000026, "ScrollLock"), (0x01000055, "Menu"), (0x01000058, "Help"),
    (0x01000005, "KP_Enter"), (0x0100000B, "Clear"), (0x2C, "Comma"),
    (0x3B, "Semicolon"), (0x01000061, "WWWBack"), (0x01000062, "WWWForward"),
    (0x01000063, "WWWStop"), (0x01000064, "WWWRefresh"), (0x01000070, "AudioDown"),
    (0x01000071, "AudioMute"), (0x01000072, "AudioUp"), (0x01000080, "AudioPlay"),
    (0x01000081, "AudioStop"), (0x01000082, "AudioPrev"), (0x01000083, "AudioNext"),
    (0x01000090, "WWWHome"), (0x01000091, "WWWFavorites"), (0x01000092, "WWWSearch"),
    (0x01000093, "Sleep"), (0x010000A0, "AppMail"), (0x010000A1, "AppMedia"),
    (0x010000A2, "AppUser1"), (0x010000A3, "AppUser2"), (0x01010000, "Select"),
]
_NAME_TO_SPECIAL = {name.lower(): code for code, name in SPECIAL_KEYS}
_SPECIAL_TO_NAME = {code: name for code, name in SPECIAL_KEYS}
MODIFIER_KEYS = [(QT_SHIFT, "Shift"), (QT_CTRL, "Control"), (QT_ALT, "Alt"), (QT_META, "Meta")]
_MOUSE_TO_QT = {"1": 1, "2": 2, "3": 4, "4": 8}


def key_to_string(key: int) -> str:
    """Port of KeySequence::keyToString."""
    if key == 0:
        return ""
    if key < QT_KEY_SPACE:
        return {1: "1", 2: "2", 4: "3"}.get(key, "4")
    if key & QT_SHIFT:
        return "Shift"
    if key & QT_CTRL:
        return "Control"
    if key & QT_ALT:
        return "Alt"
    if key & QT_META:
        return "Meta"
    key &= ~QT_KEYPAD
    if key in _SPECIAL_TO_NAME:
        return _SPECIAL_TO_NAME[key]
    if key < 0x80:
        return chr(key & 0x7F).lower()
    if QT_KEY_F1 <= key <= QT_KEY_F35:
        return "F%d" % (key - QT_KEY_F1 + 1)
    if key < 0x10000:
        return "\\u%04x" % ord(chr(key).lower())
    return ""


def string_to_key(name: str) -> Optional[int]:
    """Inverse of key_to_string (non-mouse keys)."""
    if not name:
        return None
    lower = name.lower()
    for code, mod in MODIFIER_KEYS:
        if lower == mod.lower() or (mod == "Control" and lower == "ctrl"):
            return code
    if lower in _NAME_TO_SPECIAL:
        return _NAME_TO_SPECIAL[lower]
    m = re.fullmatch(r"f([1-9]|[12][0-9]|3[0-5])", lower)
    if m:
        return QT_KEY_F1 + int(m.group(1)) - 1
    m = re.fullmatch(r"\\u([0-9a-f]{4})", lower)
    if m:
        return ord(chr(int(m.group(1), 16)).upper())
    if len(name) == 1:
        ch = name
        if ch in ",;":
            return ord(ch)
        if ord(ch) < 0x80 and ord(ch) > 0x20:
            return ord(ch.upper())
        if ord(ch) >= 0x80 and ord(ch) < 0x10000:
            return ord(ch.upper())
    return None


def sequence_from_qt(codes: List[int]) -> Dict[str, Any]:
    mouse = bool(codes) and codes[-1] < QT_KEY_SPACE
    return {"keys": [key_to_string(c) for c in codes if key_to_string(c)], "mouse": mouse}


def sequence_to_qt(seq: Dict[str, Any], field: str, errors: _Errors) -> List[int]:
    keys = seq.get("keys") if isinstance(seq, dict) else None
    mouse = bool(seq.get("mouse")) if isinstance(seq, dict) else False
    if not isinstance(keys, list) or not keys:
        errors.add(field, "at least one key is required")
        return []
    if len(keys) > 4:
        errors.add(field, "a key sequence may contain at most 4 keys")
        return []
    if mouse:
        if len(keys) != 1 or str(keys[0]) not in _MOUSE_TO_QT:
            errors.add(field, "a mouse button sequence is a single button 1-4")
            return []
        return [_MOUSE_TO_QT[str(keys[0])]]
    codes: List[int] = []
    seen_mods = 0
    for i, k in enumerate(keys):
        if not isinstance(k, str):
            errors.add(field, "key names must be strings")
            return []
        code = string_to_key(k)
        if code is None:
            errors.add(field, "unknown key \"%s\"" % k)
            return []
        is_mod = code in (QT_SHIFT, QT_CTRL, QT_ALT, QT_META)
        if is_mod:
            if i == len(keys) - 1:
                errors.add(field, "the last key must not be a modifier")
                return []
            if seen_mods & code:
                errors.add(field, "modifier \"%s\" repeated" % k)
                return []
            seen_mods |= code
        elif i != len(keys) - 1:
            errors.add(field, "only modifiers may precede the final key")
            return []
        codes.append(code)
    return codes


def sequence_text(codes: List[int]) -> str:
    return "+".join(key_to_string(c) for c in codes)


# ---------------------------------------------------------------------------
# screen name helpers (ports of the GUI's anonymous-namespace helpers)
# ---------------------------------------------------------------------------

def _split_trailing_number(name: str) -> Optional[Tuple[str, int]]:
    candidate = name.strip()
    dash = candidate.rfind("-")
    if dash <= 0 or dash + 1 >= len(candidate):
        return None
    suffix = candidate[dash + 1:]
    if not suffix.isdigit() or int(suffix) <= 0:
        return None
    return candidate[:dash], int(suffix)


def ensure_screen_number_suffix(name: str) -> str:
    trimmed = name.strip()
    if not trimmed:
        return trimmed
    if _split_trailing_number(trimmed):
        return trimmed
    return trimmed + "-1"


def base_host_name(name: str) -> str:
    candidate = name.strip()
    while candidate:
        split = _split_trailing_number(candidate)
        if not split:
            break
        candidate = split[0]
    return candidate if candidate else name


# ---------------------------------------------------------------------------
# general (AppConfig + MainWindow) settings
# ---------------------------------------------------------------------------

GENERAL_DEFAULTS: Dict[str, Any] = {
    "screenName": "",
    "port": 24800,
    "interface": "",
    "logLevel": "INFO",
    "logToFile": False,
    "logFilename": "/var/log/waver.log",
    "language": "en",
    "cryptoEnabled": True,
    "requireClientCertificate": False,
    "autoHide": False,
    "autoStart": False,
    "minimizeToTray": False,
    "uhidEnabled": False,
    "uhidName": "EtherWaver UHID",
    "elevateMode": "as_needed",
    "mode": "client",
    "serverHostname": "",
    "autoConfig": True,
    "configSource": "internal",
    "externalConfigFile": "",
}

# keys stored both in the QSettings file and gui-state.ini by MainWindow
GUI_STATE_KEYS = ["groupServerChecked", "groupClientChecked", "useExternalConfig",
                  "useInternalConfig", "configFile", "serverHostname"]


def general_from_settings(doc: QSettingsDocument, gui_state: QSettingsDocument,
                          hostname: str) -> Dict[str, Any]:
    level = doc.get_int("logLevel", 3)
    elevate = doc.get_int("elevateModeEnum", -1)
    if elevate < 0:
        elevate = 1 if doc.get_bool("elevateMode", False) else 0

    def state_bool(key: str, default: bool) -> bool:
        if gui_state.contains(key):
            return gui_state.get_bool(key, default)
        return doc.get_bool(key, default)

    def state_str(key: str, default: str) -> str:
        if gui_state.contains(key):
            return gui_state.get_str(key, default)
        return doc.get_str(key, default)

    server = state_bool("groupServerChecked", False)
    client = state_bool("groupClientChecked", True)
    external = state_bool("useExternalConfig", False)
    return {
        "screenName": doc.get_str("screenName", hostname),
        "port": doc.get_int("port", 24800),
        "interface": doc.get_str("interface", ""),
        "logLevel": LOG_LEVELS[level] if 0 <= level < len(LOG_LEVELS) else "INFO",
        "logToFile": doc.get_bool("logToFile", False),
        "logFilename": doc.get_str("logFilename", GENERAL_DEFAULTS["logFilename"]),
        "language": doc.get_str("language", "en"),
        "cryptoEnabled": doc.get_bool("cryptoEnabled", True),
        "requireClientCertificate": doc.get_bool("requireClientCertificate", False),
        "autoHide": doc.get_bool("autoHide", False),
        "autoStart": doc.get_bool("autoStart", False),
        "minimizeToTray": doc.get_bool("minimizeToTray", False),
        "uhidEnabled": doc.get_bool("uhidEnabled", False),
        "uhidName": doc.get_str("uhidName", "EtherWaver UHID"),
        "elevateMode": ELEVATE_MODES[elevate] if 0 <= elevate < 3 else "as_needed",
        "mode": "server" if server and not client else "client",
        "serverHostname": state_str("serverHostname", doc.get_str("serverHostname", "")).strip(),
        "autoConfig": doc.get_bool("autoConfig", True),
        "configSource": "external" if external else "internal",
        "externalConfigFile": state_str("configFile", ""),
    }


def validate_general(data: Dict[str, Any]) -> Dict[str, Any]:
    errors = _Errors()
    if not isinstance(data, dict):
        raise ValidationError([{"field": "", "message": "object expected"}])
    unknown = set(data.keys()) - set(GENERAL_DEFAULTS.keys())
    for key in sorted(unknown):
        errors.add(key, "unknown setting")
    out: Dict[str, Any] = {}

    def want(key: str, typ, check=None, message: str = "invalid value") -> None:
        if key not in data:
            return
        value = data[key]
        if typ is bool and not isinstance(value, bool):
            errors.add(key, "must be true or false")
            return
        if typ is int and (isinstance(value, bool) or not isinstance(value, int)):
            errors.add(key, "must be an integer")
            return
        if typ is str and not isinstance(value, str):
            errors.add(key, "must be a string")
            return
        if check is not None and not check(value):
            errors.add(key, message)
            return
        out[key] = value.strip() if isinstance(value, str) else value

    want("screenName", str, lambda v: v.strip() == "" or SCREEN_NAME_RE.match(v.strip().replace(" ", "-")) is not None,
         "use letters, digits, '.', '_' or '-' (max 255)")
    want("port", int, lambda v: 1 <= v <= 65535, "must be between 1 and 65535")
    want("interface", str, lambda v: v == "" or HOSTNAME_RE.match(v) is not None, "must be an IP address or empty")
    want("logLevel", str, lambda v: v in LOG_LEVELS, "must be one of " + ", ".join(LOG_LEVELS))
    want("logToFile", bool)
    want("logFilename", str, lambda v: 0 < len(v) <= 4096 and "\0" not in v, "invalid path")
    want("language", str, lambda v: re.fullmatch(r"[A-Za-z]{1,3}([-_][A-Za-z0-9]{2,8})?", v) is not None,
         "invalid language code")
    for key in ("cryptoEnabled", "requireClientCertificate", "autoHide", "autoStart",
                "minimizeToTray", "uhidEnabled", "autoConfig"):
        want(key, bool)
    want("uhidName", str, lambda v: 0 < len(v.strip()) <= 128, "must be 1-128 characters")
    want("elevateMode", str, lambda v: v in ELEVATE_MODES, "must be one of " + ", ".join(ELEVATE_MODES))
    want("mode", str, lambda v: v in ("server", "client"), "must be server or client")
    want("serverHostname", str, lambda v: v == "" or HOSTNAME_RE.match(v) is not None, "invalid host name")
    want("configSource", str, lambda v: v in ("internal", "external"), "must be internal or external")
    want("externalConfigFile", str, lambda v: len(v) <= 4096 and "\0" not in v, "invalid path")
    if "screenName" in out:
        out["screenName"] = out["screenName"].replace(" ", "-")
    errors.raise_if_any()
    return out


def apply_general(doc: QSettingsDocument, gui_state: QSettingsDocument, values: Dict[str, Any]) -> None:
    """Writes values exactly where AppConfig/MainWindow keep them."""
    simple = {
        "screenName": "screenName", "port": "port", "interface": "interface",
        "logToFile": "logToFile", "logFilename": "logFilename", "language": "language",
        "cryptoEnabled": "cryptoEnabled", "requireClientCertificate": "requireClientCertificate",
        "autoHide": "autoHide", "autoStart": "autoStart", "minimizeToTray": "minimizeToTray",
        "uhidEnabled": "uhidEnabled", "uhidName": "uhidName", "autoConfig": "autoConfig",
    }
    for key, skey in simple.items():
        if key in values:
            doc.set(skey, values[key])
    if "logLevel" in values:
        doc.set("logLevel", LOG_LEVELS.index(values["logLevel"]))
    if "elevateMode" in values:
        idx = ELEVATE_MODES.index(values["elevateMode"])
        doc.set("elevateModeEnum", idx)
        doc.set("elevateMode", idx == 1)
    state: Dict[str, Any] = {}
    if "mode" in values:
        state["groupServerChecked"] = values["mode"] == "server"
        state["groupClientChecked"] = values["mode"] == "client"
    if "configSource" in values:
        state["useExternalConfig"] = values["configSource"] == "external"
        state["useInternalConfig"] = values["configSource"] == "internal"
    if "externalConfigFile" in values:
        state["configFile"] = values["externalConfigFile"]
    if "serverHostname" in values:
        state["serverHostname"] = values["serverHostname"]
    for key, value in state.items():
        doc.set(key, value)
        gui_state.set(key, value)


# ---------------------------------------------------------------------------
# server layout (ServerConfig "internalConfig" group)
# ---------------------------------------------------------------------------

G = "internalConfig"


def _bool_array(doc: QSettingsDocument, prefix: str, name: str, count: int, default: bool = False) -> List[bool]:
    values = []
    for i in range(count):
        values.append(doc.get_bool("%sArray/%d/%s" % (prefix, i + 1, name), default))
    return values


def _screen_from_settings(doc: QSettingsDocument, p: str) -> Optional[Dict[str, Any]]:
    name = doc.get_str(p + "/name", "")
    if not name:
        return None
    aliases = []
    for i in range(doc.get_int(p + "/aliasArray/size", 0)):
        alias = doc.get_str("%s/aliasArray/%d/alias" % (p, i + 1), "")
        if alias:
            aliases.append(alias)
    modifiers = {}
    for i, mod in enumerate(MODIFIERS):
        raw = doc.get_int("%s/modifierArray/%d/modifier" % (p, i + 1), -1)
        target = mod if raw < 0 or raw >= len(MODIFIERS) else MODIFIERS[raw]
        modifiers[mod] = target
    corners = _bool_array(doc, p + "/switchCorner", "switchCorner", 4)
    fixes = _bool_array(doc, p + "/fix", "fix", 5)
    return {
        "name": name,
        "aliases": aliases,
        "x": doc.get_int(p + "/layoutX", 0),
        "y": doc.get_int(p + "/layoutY", 0),
        "width": doc.get_int(p + "/layoutWidth", DEFAULT_CARD_W),
        "height": doc.get_int(p + "/layoutHeight", DEFAULT_CARD_H),
        "links": {d: doc.get_str("%s/%s" % (p, LINK_KEYS[d]), "") for d in ("left", "right", "up", "down")},
        "modifiers": {m: modifiers[m] for m in MODIFIERS[:5]},
        "switchCorners": dict(zip(CORNER_KEYS, corners)),
        "switchCornerSize": doc.get_int(p + "/switchCornerSize", 0),
        "fixes": dict(zip(FIXES, fixes)),
    }


def _action_from_settings(doc: QSettingsDocument, p: str) -> Dict[str, Any]:
    codes = [doc.get_int("%s/keys/%d/key" % (p, i + 1), 0) for i in range(doc.get_int(p + "/keys/size", 0))]
    t = doc.get_int(p + "/type", 0)
    screens = [doc.get_str("%s/typeScreenNames/%d/typeScreenName" % (p, i + 1), "")
               for i in range(doc.get_int(p + "/typeScreenNames/size", 0))]
    has_screens = doc.get_bool(p + "/hasScreens", False)
    direction = doc.get_int(p + "/switchInDirection", 0)
    lock = doc.get_int(p + "/lockCursorToScreen", 0)
    seq = sequence_from_qt(codes)
    return {
        "type": ACTION_TYPES[t] if 0 <= t < len(ACTION_TYPES) else "keystroke",
        "keys": seq["keys"],
        "mouse": seq["mouse"],
        "screens": [s for s in screens if s] if has_screens else [],
        "screen": doc.get_str(p + "/switchScreenName", ""),
        "direction": SWITCH_DIRECTIONS[direction] if 0 <= direction < 4 else "left",
        "lockMode": LOCK_MODES[lock] if 0 <= lock < 3 else "toggle",
        "activeOnRelease": doc.get_bool(p + "/activeOnRelease", False),
    }


def layout_from_settings(doc: QSettingsDocument, server_name: str) -> Dict[str, Any]:
    columns = doc.get_int(G + "/numColumns", 5)
    rows = doc.get_int(G + "/numRows", 3)
    corners = _bool_array(doc, G + "/switchCorner", "switchCorner", 4)
    screens: List[Dict[str, Any]] = []
    for i in range(doc.get_int(G + "/screens/size", 0)):
        s = _screen_from_settings(doc, "%s/screens/%d" % (G, i + 1))
        screens.append(s)  # may be None (empty grid cell)
    # GUI normalizeScreenNames(): enforce "-N" suffix on names and links
    renamed = {}
    for s in screens:
        if s is None:
            continue
        normalized = ensure_screen_number_suffix(s["name"])
        if normalized.lower() != s["name"].lower():
            renamed[s["name"].lower()] = normalized
            s["name"] = normalized
    for s in screens:
        if s is None:
            continue
        for d, target in list(s["links"].items()):
            if target.strip():
                s["links"][d] = renamed.get(target.strip().lower(), ensure_screen_number_suffix(target))
    hotkeys = []
    for i in range(doc.get_int(G + "/hotkeys/size", 0)):
        p = "%s/hotkeys/%d" % (G, i + 1)
        codes = [doc.get_int("%s/keys/%d/key" % (p, j + 1), 0) for j in range(doc.get_int(p + "/keys/size", 0))]
        actions = [_action_from_settings(doc, "%s/actions/%d" % (p, j + 1))
                   for j in range(doc.get_int(p + "/actions/size", 0))]
        seq = sequence_from_qt(codes)
        hotkeys.append({"keys": seq["keys"], "mouse": seq["mouse"], "actions": actions})

    present = [s for s in screens if s is not None]
    # ServerConfig::loadSettings(): the server's own screen is always present
    server_screen = ensure_screen_number_suffix(server_name) if server_name else ""
    if server_screen and not any(s["name"].lower() == server_screen.lower() for s in present):
        present.insert(0, new_screen(server_screen))
    return {
        "grid": {"columns": columns, "rows": rows},
        "options": {
            "heartbeat": {"enabled": doc.get_bool(G + "/hasHeartbeat", False), "ms": doc.get_int(G + "/heartbeat", 5000)},
            "relativeMouseMoves": doc.get_bool(G + "/relativeMouseMoves", False),
            "screenSaverSync": doc.get_bool(G + "/screenSaverSync", True),
            "win32KeepForeground": doc.get_bool(G + "/win32KeepForeground", False),
            "switchDelay": {"enabled": doc.get_bool(G + "/hasSwitchDelay", False), "ms": doc.get_int(G + "/switchDelay", 250)},
            "switchDoubleTap": {"enabled": doc.get_bool(G + "/hasSwitchDoubleTap", False), "ms": doc.get_int(G + "/switchDoubleTap", 250)},
            "switchCorners": dict(zip(CORNER_KEYS, corners)),
            "switchCornerSize": doc.get_int(G + "/switchCornerSize", 0),
            "ignoreAutoConfigClient": doc.get_bool(G + "/ignoreAutoConfigClient", False),
            "enableDragAndDrop": doc.get_bool(G + "/enableDragAndDrop", True),
            "clipboardSharing": doc.get_bool(G + "/clipboardSharing", True),
            "useCustomLinks": doc.get_bool(G + "/useCustomLinks", False),
        },
        "screens": present,
        "hotkeys": hotkeys,
    }


def new_screen(name: str, x: int = 0, y: int = 0) -> Dict[str, Any]:
    return {
        "name": name, "aliases": [], "x": x, "y": y,
        "width": DEFAULT_CARD_W, "height": DEFAULT_CARD_H,
        "links": {"left": "", "right": "", "up": "", "down": ""},
        "modifiers": {m: m for m in MODIFIERS[:5]},
        "switchCorners": {k: False for k in CORNER_KEYS},
        "switchCornerSize": 0,
        "fixes": {f: False for f in FIXES},
    }


def _int_in(errors: _Errors, field: str, value: Any, lo: int, hi: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        errors.add(field, "must be an integer")
        return lo
    if value < lo or value > hi:
        errors.add(field, "must be between %d and %d" % (lo, hi))
    return value


def _bool(errors: _Errors, field: str, value: Any) -> bool:
    if not isinstance(value, bool):
        errors.add(field, "must be true or false")
        return False
    return value


def validate_layout(data: Dict[str, Any], server_name: str) -> Dict[str, Any]:
    """Validates and normalises a full server layout document."""
    errors = _Errors()
    if not isinstance(data, dict):
        raise ValidationError([{"field": "", "message": "object expected"}])
    out: Dict[str, Any] = {}

    opts = data.get("options", {})
    if not isinstance(opts, dict):
        errors.add("options", "object expected")
        opts = {}
    o: Dict[str, Any] = {}
    for key, lo, hi, default in (("heartbeat", 1000, 3600000, 5000), ("switchDelay", 10, 10000, 250),
                                 ("switchDoubleTap", 10, 10000, 250)):
        v = opts.get(key, {"enabled": False, "ms": default})
        if not isinstance(v, dict):
            errors.add("options." + key, "object expected")
            v = {}
        o[key] = {"enabled": _bool(errors, "options.%s.enabled" % key, v.get("enabled", False)),
                  "ms": _int_in(errors, "options.%s.ms" % key, v.get("ms", default), lo, hi)}
    for key, default in (("relativeMouseMoves", False), ("screenSaverSync", True), ("win32KeepForeground", False),
                         ("ignoreAutoConfigClient", False), ("enableDragAndDrop", True),
                         ("clipboardSharing", True), ("useCustomLinks", False)):
        o[key] = _bool(errors, "options." + key, opts.get(key, default))
    corners = opts.get("switchCorners", {})
    if not isinstance(corners, dict):
        errors.add("options.switchCorners", "object expected")
        corners = {}
    o["switchCorners"] = {k: _bool(errors, "options.switchCorners." + k, corners.get(k, False)) for k in CORNER_KEYS}
    o["switchCornerSize"] = _int_in(errors, "options.switchCornerSize", opts.get("switchCornerSize", 0), 0, 1000)
    unknown_opts = set(opts.keys()) - set(o.keys())
    for k in sorted(unknown_opts):
        errors.add("options." + k, "unknown option")
    out["options"] = o

    raw_screens = data.get("screens", [])
    if not isinstance(raw_screens, list):
        errors.add("screens", "array expected")
        raw_screens = []
    if len(raw_screens) > 64:
        errors.add("screens", "at most 64 screens are supported")
        raw_screens = raw_screens[:64]
    screens: List[Dict[str, Any]] = []
    names_seen: Dict[str, int] = {}
    alias_seen: Dict[str, str] = {}
    for idx, s in enumerate(raw_screens):
        f = "screens[%d]" % idx
        if not isinstance(s, dict):
            errors.add(f, "object expected")
            continue
        name = s.get("name", "")
        if not isinstance(name, str) or not SCREEN_NAME_RE.match(name.strip()):
            errors.add(f + ".name", "use 1-255 letters, digits, '.', '_' or '-'")
            continue
        name = ensure_screen_number_suffix(name)
        if len(name) > 255:
            errors.add(f + ".name", "too long")
            continue
        if name.lower() in names_seen:
            errors.add(f + ".name", "duplicate screen name \"%s\"" % name)
            continue
        names_seen[name.lower()] = idx
        sc = new_screen(name)
        aliases = s.get("aliases", [])
        if not isinstance(aliases, list):
            errors.add(f + ".aliases", "array expected")
            aliases = []
        for a_i, alias in enumerate(aliases):
            if not isinstance(alias, str) or not SCREEN_NAME_RE.match(alias):
                errors.add("%s.aliases[%d]" % (f, a_i), "invalid alias")
                continue
            if alias.lower() in alias_seen:
                errors.add("%s.aliases[%d]" % (f, a_i), "alias already used by %s" % alias_seen[alias.lower()])
                continue
            alias_seen[alias.lower()] = name
            sc["aliases"].append(alias)
        sc["x"] = _int_in(errors, f + ".x", s.get("x", 0), -100000, 100000)
        sc["y"] = _int_in(errors, f + ".y", s.get("y", 0), -100000, 100000)
        sc["width"] = _int_in(errors, f + ".width", s.get("width", DEFAULT_CARD_W), 1, 100000)
        sc["height"] = _int_in(errors, f + ".height", s.get("height", DEFAULT_CARD_H), 1, 100000)
        links = s.get("links", {})
        if not isinstance(links, dict):
            errors.add(f + ".links", "object expected")
            links = {}
        for d in ("left", "right", "up", "down"):
            v = links.get(d, "")
            if not isinstance(v, str):
                errors.add("%s.links.%s" % (f, d), "must be a screen name")
                v = ""
            sc["links"][d] = ensure_screen_number_suffix(v) if v.strip() else ""
        mods = s.get("modifiers", {})
        if not isinstance(mods, dict):
            errors.add(f + ".modifiers", "object expected")
            mods = {}
        for m in MODIFIERS[:5]:
            v = mods.get(m, m)
            if v not in MODIFIERS:
                errors.add("%s.modifiers.%s" % (f, m), "must be one of " + ", ".join(MODIFIERS))
                v = m
            sc["modifiers"][m] = v
        cs = s.get("switchCorners", {})
        if not isinstance(cs, dict):
            errors.add(f + ".switchCorners", "object expected")
            cs = {}
        sc["switchCorners"] = {k: _bool(errors, "%s.switchCorners.%s" % (f, k), cs.get(k, False)) for k in CORNER_KEYS}
        sc["switchCornerSize"] = _int_in(errors, f + ".switchCornerSize", s.get("switchCornerSize", 0), 0, 1000)
        fx = s.get("fixes", {})
        if not isinstance(fx, dict):
            errors.add(f + ".fixes", "object expected")
            fx = {}
        sc["fixes"] = {k: _bool(errors, "%s.fixes.%s" % (f, k), fx.get(k, False)) for k in FIXES}
        screens.append(sc)

    for name_lower, alias_owner in alias_seen.items():
        if name_lower in names_seen and alias_owner.lower() != name_lower:
            errors.add("screens", "alias \"%s\" collides with a screen name" % name_lower)
    for idx, sc in enumerate(screens):
        for d, target in sc["links"].items():
            if target and target.lower() not in names_seen:
                errors.add("screens[%d].links.%s" % (idx, d), "unknown screen \"%s\"" % target)

    server_screen = ensure_screen_number_suffix(server_name) if server_name else ""
    if server_screen and server_screen.lower() not in names_seen:
        errors.add("screens", "the server screen \"%s\" cannot be removed" % server_screen)

    # ServerConfigDialog::syncLegacyGrid(): order screens by position
    screens.sort(key=lambda sc: (sc["y"], sc["x"]))
    out["screens"] = screens

    grid = data.get("grid", {"columns": 5, "rows": 3})
    if not isinstance(grid, dict):
        grid = {}
    columns = _int_in(errors, "grid.columns", grid.get("columns", 5), 1, 64)
    rows = _int_in(errors, "grid.rows", grid.get("rows", 3), 1, 64)
    # the desktop panel indexes a fixed columns*rows array; keep it large enough
    while columns * rows < len(screens):
        rows += 1
    out["grid"] = {"columns": columns, "rows": rows}

    raw_hotkeys = data.get("hotkeys", [])
    if not isinstance(raw_hotkeys, list):
        errors.add("hotkeys", "array expected")
        raw_hotkeys = []
    if len(raw_hotkeys) > 128:
        errors.add("hotkeys", "at most 128 hotkeys are supported")
        raw_hotkeys = raw_hotkeys[:128]
    hotkeys = []
    for h_i, h in enumerate(raw_hotkeys):
        f = "hotkeys[%d]" % h_i
        if not isinstance(h, dict):
            errors.add(f, "object expected")
            continue
        codes = sequence_to_qt(h, f + ".keys", errors)
        actions = []
        raw_actions = h.get("actions", [])
        if not isinstance(raw_actions, list) or not raw_actions:
            errors.add(f + ".actions", "at least one action is required")
            raw_actions = []
        for a_i, a in enumerate(raw_actions[:32]):
            af = "%s.actions[%d]" % (f, a_i)
            if not isinstance(a, dict):
                errors.add(af, "object expected")
                continue
            t = a.get("type")
            if t not in ACTION_TYPES:
                errors.add(af + ".type", "must be one of " + ", ".join(ACTION_TYPES))
                continue
            act = {"type": t, "keys": [], "mouse": False, "screens": [], "screen": "",
                   "direction": "left", "lockMode": "toggle",
                   "activeOnRelease": _bool(errors, af + ".activeOnRelease", a.get("activeOnRelease", False))}
            if t in ("keyDown", "keyUp", "keystroke"):
                seq_codes = sequence_to_qt(a, af + ".keys", errors)
                seq = sequence_from_qt(seq_codes)
                act["keys"], act["mouse"] = seq["keys"], seq["mouse"]
                targets = a.get("screens", [])
                if not isinstance(targets, list):
                    errors.add(af + ".screens", "array expected")
                    targets = []
                for t_i, target in enumerate(targets):
                    if not isinstance(target, str) or target.lower() not in names_seen:
                        errors.add("%s.screens[%d]" % (af, t_i), "unknown screen")
                    else:
                        act["screens"].append(screens[[s["name"].lower() for s in screens].index(target.lower())]["name"])
            elif t == "switchToScreen":
                target = a.get("screen", "")
                if not isinstance(target, str) or target.lower() not in names_seen:
                    errors.add(af + ".screen", "unknown screen")
                else:
                    act["screen"] = [s["name"] for s in screens if s["name"].lower() == target.lower()][0]
            elif t == "switchInDirection":
                if a.get("direction") not in SWITCH_DIRECTIONS:
                    errors.add(af + ".direction", "must be one of " + ", ".join(SWITCH_DIRECTIONS))
                else:
                    act["direction"] = a["direction"]
            elif t == "lockCursorToScreen":
                if a.get("lockMode", "toggle") not in LOCK_MODES:
                    errors.add(af + ".lockMode", "must be one of " + ", ".join(LOCK_MODES))
                else:
                    act["lockMode"] = a.get("lockMode", "toggle")
            actions.append(act)
        seq = sequence_from_qt(codes)
        hotkeys.append({"keys": seq["keys"], "mouse": seq["mouse"], "actions": actions})
    out["hotkeys"] = hotkeys

    unknown = set(data.keys()) - {"options", "screens", "grid", "hotkeys"}
    for k in sorted(unknown):
        errors.add(k, "unknown field")
    errors.raise_if_any()
    return out


def _qt_codes(seq: Dict[str, Any]) -> List[int]:
    errs = _Errors()
    codes = sequence_to_qt(seq, "", errs)
    return codes


def apply_layout(doc: QSettingsDocument, layout: Dict[str, Any]) -> None:
    """Writes a validated layout as ServerConfig::saveSettings() would."""
    doc.remove_group(G)
    o = layout["options"]
    doc.set(G + "/numColumns", layout["grid"]["columns"])
    doc.set(G + "/numRows", layout["grid"]["rows"])
    doc.set(G + "/useCustomLinks", o["useCustomLinks"])
    doc.set(G + "/hasHeartbeat", o["heartbeat"]["enabled"])
    doc.set(G + "/heartbeat", o["heartbeat"]["ms"])
    doc.set(G + "/relativeMouseMoves", o["relativeMouseMoves"])
    doc.set(G + "/screenSaverSync", o["screenSaverSync"])
    doc.set(G + "/win32KeepForeground", o["win32KeepForeground"])
    doc.set(G + "/hasSwitchDelay", o["switchDelay"]["enabled"])
    doc.set(G + "/switchDelay", o["switchDelay"]["ms"])
    doc.set(G + "/hasSwitchDoubleTap", o["switchDoubleTap"]["enabled"])
    doc.set(G + "/switchDoubleTap", o["switchDoubleTap"]["ms"])
    doc.set(G + "/switchCornerSize", o["switchCornerSize"])
    doc.set(G + "/ignoreAutoConfigClient", o["ignoreAutoConfigClient"])
    doc.set(G + "/enableDragAndDrop", o["enableDragAndDrop"])
    doc.set(G + "/clipboardSharing", o["clipboardSharing"])
    for i, k in enumerate(CORNER_KEYS):
        doc.set("%s/switchCornerArray/%d/switchCorner" % (G, i + 1), o["switchCorners"][k])
    doc.set(G + "/switchCornerArray/size", 4)

    total = layout["grid"]["columns"] * layout["grid"]["rows"]
    screens = layout["screens"]
    for i in range(total):
        p = "%s/screens/%d" % (G, i + 1)
        if i >= len(screens):
            doc.set(p + "/name", "")
            continue
        s = screens[i]
        doc.set(p + "/name", s["name"])
        doc.set(p + "/switchCornerSize", s["switchCornerSize"])
        doc.set(p + "/layoutX", s["x"])
        doc.set(p + "/layoutY", s["y"])
        doc.set(p + "/layoutWidth", s["width"])
        doc.set(p + "/layoutHeight", s["height"])
        for d in ("right", "left", "up", "down"):
            doc.set("%s/%s" % (p, LINK_KEYS[d]), s["links"][d])
        for a_i, alias in enumerate(s["aliases"]):
            doc.set("%s/aliasArray/%d/alias" % (p, a_i + 1), alias)
        doc.set(p + "/aliasArray/size", len(s["aliases"]))
        for m_i, m in enumerate(MODIFIERS):
            target = s["modifiers"].get(m, m) if m_i < 5 else m
            doc.set("%s/modifierArray/%d/modifier" % (p, m_i + 1), MODIFIERS.index(target))
        doc.set(p + "/modifierArray/size", len(MODIFIERS))
        for c_i, k in enumerate(CORNER_KEYS):
            doc.set("%s/switchCornerArray/%d/switchCorner" % (p, c_i + 1), s["switchCorners"][k])
        doc.set(p + "/switchCornerArray/size", 4)
        for f_i, k in enumerate(FIXES):
            doc.set("%s/fixArray/%d/fix" % (p, f_i + 1), s["fixes"][k])
        doc.set(p + "/fixArray/size", len(FIXES))
    doc.set(G + "/screens/size", total)

    for h_i, h in enumerate(layout["hotkeys"]):
        p = "%s/hotkeys/%d" % (G, h_i + 1)
        codes = _qt_codes(h)
        for k_i, code in enumerate(codes):
            doc.set("%s/keys/%d/key" % (p, k_i + 1), code)
        doc.set(p + "/keys/size", len(codes))
        for a_i, a in enumerate(h["actions"]):
            ap = "%s/actions/%d" % (p, a_i + 1)
            a_codes = _qt_codes(a) if a["type"] in ("keyDown", "keyUp", "keystroke") else []
            for k_i, code in enumerate(a_codes):
                doc.set("%s/keys/%d/key" % (ap, k_i + 1), code)
            doc.set(ap + "/keys/size", len(a_codes))
            doc.set(ap + "/type", ACTION_TYPES.index(a["type"]))
            for s_i, sn in enumerate(a["screens"]):
                doc.set("%s/typeScreenNames/%d/typeScreenName" % (ap, s_i + 1), sn)
            doc.set(ap + "/typeScreenNames/size", len(a["screens"]))
            doc.set(ap + "/switchScreenName", a["screen"])
            doc.set(ap + "/switchInDirection", SWITCH_DIRECTIONS.index(a["direction"]))
            doc.set(ap + "/lockCursorToScreen", LOCK_MODES.index(a["lockMode"]))
            doc.set(ap + "/activeOnRelease", a["activeOnRelease"])
            doc.set(ap + "/hasScreens", bool(a["screens"]))
        doc.set(p + "/actions/size", len(h["actions"]))
    doc.set(G + "/hotkeys/size", len(layout["hotkeys"]))


# ---------------------------------------------------------------------------
# server configuration text (ports of the GUI's operator<<)
# ---------------------------------------------------------------------------

def _action_text(a: Dict[str, Any]) -> str:
    t = a["type"]
    codes = _qt_codes(a) if t in ("keyDown", "keyUp", "keystroke") else []
    mouse = bool(codes) and codes[-1] < QT_KEY_SPACE
    name = ACTION_MOUSE_NAMES[t] if (mouse and t in ACTION_MOUSE_NAMES) else t
    if t in ("keyDown", "keyUp", "keystroke"):
        text = name + "(" + sequence_text(codes)
        if not mouse:
            text += "," + (":".join(a["screens"]) if a["screens"] else "*")
        text += ")"
    elif t == "switchToScreen":
        text = name + "(" + a["screen"] + ")"
    elif t == "toggleScreen":
        text = name
    elif t == "switchInDirection":
        text = name + "(" + a["direction"] + ")"
    else:
        text = name + "(" + a["lockMode"] + ")"
    return (";" if a["activeOnRelease"] else "") + text


def _grid_adjacent(idx: int, dcol: int, drow: int, columns: int, count_total: int) -> int:
    if (dcol > 0 and (idx + 1) % columns == 0) or (dcol < 0 and idx % columns == 0):
        return -1
    pos = idx + dcol + drow * columns
    if pos >= count_total or pos < 0:
        return -1
    return pos


def server_config_text(layout: Dict[str, Any]) -> str:
    """Port of operator<<(QTextStream&, const ServerConfig&)."""
    screens = layout["screens"]
    o = layout["options"]
    columns = layout["grid"]["columns"]
    total = columns * layout["grid"]["rows"]
    lines: List[str] = ["section: screens"]
    for s in screens:
        lines.append("\t%s:" % s["name"])
        for m in MODIFIERS[:5]:
            target = s["modifiers"].get(m, m)
            if target != m:
                lines.append("\t\t%s = %s" % (m, target))
        for f in FIXES:
            lines.append("\t\t%s = %s" % (f, "true" if s["fixes"][f] else "false"))
        corners = "".join("+%s " % CORNERS[i] for i, k in enumerate(CORNER_KEYS) if s["switchCorners"][k])
        lines.append("\t\tswitchCorners = none " + corners)
        lines.append("\t\tswitchCornerSize = %d" % s["switchCornerSize"])
    lines += ["end", "", "section: aliases"]
    for s in screens:
        if s["aliases"]:
            lines.append("\t%s:" % s["name"])
            for a in s["aliases"]:
                lines.append("\t\t%s" % a)
    lines += ["end", "", "section: links"]
    names = {s["name"].lower() for s in screens}
    neighbour = [(1, 0, "right"), (-1, 0, "left"), (0, -1, "up"), (0, 1, "down")]
    for i, s in enumerate(screens):
        lines.append("\t%s:" % s["name"])
        if o["useCustomLinks"]:
            for d in ("right", "left", "up", "down"):
                target = s["links"][d]
                if target and target.lower() in names:
                    lines.append("\t\t%s = %s" % (d, target))
        else:
            for dx, dy, dname in neighbour:
                idx = _grid_adjacent(i, dx, dy, columns, total)
                if idx != -1 and idx < len(screens):
                    lines.append("\t\t%s = %s" % (dname, screens[idx]["name"]))
    lines += ["end", "", "section: options"]
    if o["heartbeat"]["enabled"]:
        lines.append("\theartbeat = %d" % o["heartbeat"]["ms"])
    lines.append("\trelativeMouseMoves = %s" % ("true" if o["relativeMouseMoves"] else "false"))
    lines.append("\tscreenSaverSync = %s" % ("true" if o["screenSaverSync"] else "false"))
    lines.append("\twin32KeepForeground = %s" % ("true" if o["win32KeepForeground"] else "false"))
    lines.append("\tclipboardSharing = %s" % ("true" if o["clipboardSharing"] else "false"))
    if o["switchDelay"]["enabled"]:
        lines.append("\tswitchDelay = %d" % o["switchDelay"]["ms"])
    if o["switchDoubleTap"]["enabled"]:
        lines.append("\tswitchDoubleTap = %d" % o["switchDoubleTap"]["ms"])
    corners = "".join("+%s " % CORNERS[i] for i, k in enumerate(CORNER_KEYS) if o["switchCorners"][k])
    lines.append("\tswitchCorners = none " + corners)
    lines.append("\tswitchCornerSize = %d" % o["switchCornerSize"])
    for h in layout["hotkeys"]:
        if not h["actions"]:
            continue
        codes = _qt_codes(h)
        trigger = ("mousebutton(%s)" if (codes and codes[-1] < QT_KEY_SPACE) else "keystroke(%s)") % sequence_text(codes)
        lines.append("\t%s = %s" % (trigger, ", ".join(_action_text(a) for a in h["actions"])))
    lines += ["end", ""]
    return "\n".join(lines) + "\n"


def layout_json_text(layout: Dict[str, Any]) -> str:
    """Port of ServerConfig::saveLayout (etherwaver-layout.json)."""
    parts = []
    for s in layout["screens"]:
        entry = ("    {\n"
                 "      \"id\": \"%s\",\n"
                 "      \"host\": \"%s\",\n"
                 "      \"name\": \"%s\",\n"
                 "      \"x\": %d,\n"
                 "      \"y\": %d,\n"
                 "      \"width\": %d,\n"
                 "      \"height\": %d") % (s["name"], base_host_name(s["name"]), s["name"],
                                            s["x"], s["y"], s["width"], s["height"])
        if layout["options"]["useCustomLinks"]:
            entry += (",\n      \"links\": {\n"
                      "        \"right\": \"%s\",\n"
                      "        \"left\": \"%s\",\n"
                      "        \"up\": \"%s\",\n"
                      "        \"down\": \"%s\"\n"
                      "      }\n    }") % (s["links"]["right"], s["links"]["left"], s["links"]["up"], s["links"]["down"])
        else:
            entry += "\n    }"
        parts.append(entry)
    return "{\n  \"screens\": [\n" + ",\n".join(parts) + "\n  ]\n}\n"


# ---------------------------------------------------------------------------
# versioning
# ---------------------------------------------------------------------------

def etag_of(obj: Any) -> str:
    blob = json.dumps(obj, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return '"' + hashlib.sha256(blob).hexdigest()[:32] + '"'


def deep_copy(obj: Any) -> Any:
    return copy.deepcopy(obj)

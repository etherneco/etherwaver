"""Reader/writer for Qt ``QSettings`` INI files.

The desktop panel (Qt GUI) keeps its configuration in a QSettings INI file
(``~/.config/Debauchee/EtherWaver.conf`` on Linux).  The web panel edits the
*same* file so both panels share one source of truth.  This module mirrors
Qt 5's encoding rules (``QSettingsPrivate::iniEscapedKey`` /
``iniEscapedString``) and Qt's ``QLockFile`` protocol, so files written here
are read back by Qt exactly as if Qt had written them, and concurrent writes
by the desktop panel are serialised.

Only string scalars are modelled (``QVariant`` booleans and integers are
stored by Qt as plain strings too).  Entries this module does not understand
(``@Variant(...)``, ``@ByteArray(...)``) are preserved verbatim.
"""

from __future__ import annotations

import errno
import os
import socket
import tempfile
import time
from collections import OrderedDict
from typing import Dict, Iterable, List, Optional, Tuple, Union

Value = Union[str, List[str]]

_KEY_SAFE = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.")
_HEX = set("0123456789abcdefABCDEF")


# ---------------------------------------------------------------------------
# key encoding
# ---------------------------------------------------------------------------

def escape_key(key: str) -> str:
    """Qt key -> INI key (``/`` becomes ``\\``; other specials are %-encoded)."""
    out = []
    for ch in key:
        if ch == "/":
            out.append("\\")
        elif ch in _KEY_SAFE:
            out.append(ch)
        elif ord(ch) <= 0xFF:
            out.append("%%%02X" % ord(ch))
        else:
            out.append("%%U%04X" % ord(ch))
    return "".join(out)


def unescape_key(raw: str) -> str:
    out = []
    i = 0
    n = len(raw)
    while i < n:
        ch = raw[i]
        if ch == "\\":
            out.append("/")
            i += 1
        elif ch == "%" and i + 2 < n + 1:
            if raw[i + 1:i + 2] == "U" and i + 6 <= n:
                try:
                    out.append(chr(int(raw[i + 2:i + 6], 16)))
                    i += 6
                    continue
                except ValueError:
                    pass
            try:
                out.append(chr(int(raw[i + 1:i + 3], 16)))
                i += 3
            except ValueError:
                out.append(ch)
                i += 1
        else:
            out.append(ch)
            i += 1
    return "".join(out)


# ---------------------------------------------------------------------------
# value encoding
# ---------------------------------------------------------------------------

def escape_string(value: str) -> str:
    """Mirror of QSettingsPrivate::iniEscapedString (no codec set)."""
    if value.startswith("@"):
        value = "@" + value
    needs_quotes = False
    escape_next_if_digit = False
    out = []
    for i, ch in enumerate(value):
        code = ord(ch)
        if ch in ";,=":
            needs_quotes = True
        if escape_next_if_digit and ch in _HEX:
            out.append("\\x%x" % code)
            continue
        escape_next_if_digit = False
        if ch == "\0":
            out.append("\\0")
            escape_next_if_digit = True
        elif ch == "\a":
            out.append("\\a")
        elif ch == "\b":
            out.append("\\b")
        elif ch == "\f":
            out.append("\\f")
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\r":
            out.append("\\r")
        elif ch == "\t":
            out.append("\\t")
        elif ch == "\v":
            out.append("\\v")
        elif ch == '"' or ch == "\\":
            out.append("\\" + ch)
        elif code < 0x20 or code >= 0x7F:
            out.append("\\x%x" % code)
            escape_next_if_digit = True
        else:
            out.append(ch)
    text = "".join(out)
    if needs_quotes or (value and (value[0] == " " or value[-1] == " ")):
        return '"' + text + '"'
    return text


def escape_value(value: Value) -> str:
    if isinstance(value, list):
        if not value:
            return "@Invalid()"
        return ", ".join(escape_string(v) for v in value)
    return escape_string(value)


def unescape_value(raw: str) -> Value:
    """Mirror of QSettingsPrivate::iniUnescapedStringList."""
    items: List[str] = []
    current: List[str] = []
    in_quotes = False
    i = 0
    n = len(raw)
    # leading whitespace is insignificant
    while i < n and raw[i] in " \t":
        i += 1
    item_started = False
    pending_spaces: List[str] = []
    while i < n:
        ch = raw[i]
        if ch == "\\" and i + 1 < n:
            nxt = raw[i + 1]
            i += 2
            simple = {"a": "\a", "b": "\b", "f": "\f", "n": "\n", "r": "\r",
                      "t": "\t", "v": "\v", '"': '"', "?": "?", "'": "'",
                      "\\": "\\"}
            current.extend(pending_spaces)
            pending_spaces = []
            item_started = True
            if nxt in simple:
                current.append(simple[nxt])
            elif nxt == "x":
                j = i
                while j < n and j - i < 4 and raw[j] in _HEX:
                    j += 1
                if j > i:
                    current.append(chr(int(raw[i:j], 16)))
                i = j
            elif nxt in "01234567":
                j = i - 1
                while j < n and j - (i - 1) < 3 and raw[j] in "01234567":
                    j += 1
                current.append(chr(int(raw[i - 1:j], 8)))
                i = j
            else:
                current.append(nxt)
            continue
        if ch == '"':
            in_quotes = not in_quotes
            item_started = True
            current.extend(pending_spaces)
            pending_spaces = []
            i += 1
            continue
        if ch == "," and not in_quotes:
            items.append("".join(current))
            current = []
            pending_spaces = []
            item_started = False
            i += 1
            while i < n and raw[i] in " \t":
                i += 1
            continue
        if ch in " \t" and not in_quotes:
            if item_started:
                pending_spaces.append(ch)
            i += 1
            continue
        current.extend(pending_spaces)
        pending_spaces = []
        current.append(ch)
        item_started = True
        i += 1
    items.append("".join(current))
    if len(items) == 1:
        value = items[0]
        if value.startswith("@@"):
            return value[1:]
        return value
    return items


# ---------------------------------------------------------------------------
# document
# ---------------------------------------------------------------------------

class QSettingsDocument:
    """In-memory model of an INI file, keyed by Qt keys (``group/sub/key``)."""

    def __init__(self) -> None:
        # qt key -> raw (escaped) value text, insertion ordered
        self._raw: "OrderedDict[str, str]" = OrderedDict()

    # -- parsing -----------------------------------------------------------
    @classmethod
    def parse(cls, text: str) -> "QSettingsDocument":
        doc = cls()
        section = ""
        pending_key: Optional[str] = None
        pending_value: List[str] = []
        lines = text.splitlines()
        idx = 0
        while idx < len(lines):
            line = lines[idx]
            idx += 1
            stripped = line.strip()
            if not stripped or stripped.startswith(";") or stripped.startswith("#"):
                continue
            if stripped.startswith("[") and stripped.endswith("]"):
                name = unescape_key(stripped[1:-1])
                section = "" if name.lower() == "general" else name
                continue
            if "=" not in line:
                continue
            raw_key, raw_value = line.split("=", 1)
            key = unescape_key(raw_key.strip())
            value = raw_value.strip()
            # Qt allows continuation lines ending with a backslash
            while value.endswith("\\") and not value.endswith("\\\\") and idx < len(lines):
                value = value[:-1] + lines[idx].strip()
                idx += 1
            full = key if not section else section + "/" + key
            doc._raw[full] = value
        del pending_key, pending_value
        return doc

    # -- access -------------------------------------------------------------
    def keys(self) -> List[str]:
        return list(self._raw.keys())

    def contains(self, key: str) -> bool:
        return key in self._raw

    def get(self, key: str, default: Optional[Value] = None) -> Optional[Value]:
        raw = self._raw.get(key)
        if raw is None:
            return default
        if raw.startswith("@") and not raw.startswith("@@"):
            return default
        return unescape_value(raw)

    def get_str(self, key: str, default: str = "") -> str:
        value = self.get(key)
        if value is None:
            return default
        if isinstance(value, list):
            return ", ".join(value)
        return value

    def get_int(self, key: str, default: int = 0) -> int:
        value = self.get(key)
        if value is None or isinstance(value, list):
            return default
        try:
            return int(value.strip())
        except ValueError:
            try:
                return int(float(value.strip()))
            except ValueError:
                return default

    def get_bool(self, key: str, default: bool = False) -> bool:
        value = self.get(key)
        if value is None or isinstance(value, list):
            return default
        v = value.strip().lower()
        if v in ("true", "1"):
            return True
        if v in ("false", "0", ""):
            return False
        return default

    def set(self, key: str, value: Union[Value, bool, int]) -> None:
        if isinstance(value, bool):
            text = "true" if value else "false"
        elif isinstance(value, int):
            text = str(value)
        else:
            text = escape_value(value)
        self._raw[key] = text

    def remove_group(self, prefix: str) -> None:
        """Remove ``prefix`` and everything below it (like QSettings::remove)."""
        p = prefix.rstrip("/")
        for key in list(self._raw.keys()):
            if key == p or key.startswith(p + "/"):
                del self._raw[key]

    def raw_items(self) -> List[Tuple[str, str]]:
        return list(self._raw.items())

    # -- arrays (QSettings::beginReadArray semantics) -------------------------
    def array_size(self, prefix: str) -> int:
        return self.get_int(prefix + "/size", 0)

    # -- serialisation ------------------------------------------------------
    def serialize(self) -> str:
        groups: "OrderedDict[str, List[Tuple[str, str]]]" = OrderedDict()
        for key, raw in self._raw.items():
            if "/" in key:
                section, sub = key.split("/", 1)
            else:
                section, sub = "General", key
            groups.setdefault(section, []).append((sub, raw))
        names = sorted(groups.keys(), key=lambda s: (s != "General", s))
        out: List[str] = []
        for name in names:
            if out:
                out.append("")
            out.append("[%s]" % escape_key(name))
            for sub, raw in sorted(groups[name], key=lambda kv: kv[0]):
                out.append("%s=%s" % (escape_key(sub), raw))
        return "\n".join(out) + "\n" if out else ""


# ---------------------------------------------------------------------------
# QLockFile-compatible lock
# ---------------------------------------------------------------------------

def _process_name() -> str:
    try:
        return os.path.basename(os.readlink("/proc/self/exe"))
    except OSError:
        return "waver-web"


def _pid_alive(pid: int) -> bool:
    if pid <= 0:
        return False
    try:
        os.kill(pid, 0)
    except OSError as e:
        return e.errno == errno.EPERM
    return True


class LockTimeout(Exception):
    pass


class QtLockFile:
    """Advisory lock compatible with Qt's QLockFile (used by QSettings::sync).

    Qt creates ``<settings>.lock`` exclusively and writes
    ``pid\\nappname\\nhostname\\n``.  We follow the same protocol so the GUI
    waits while the web backend writes and vice versa.
    """

    STALE_SECONDS = 30.0

    def __init__(self, path: str, timeout: float = 5.0) -> None:
        self.path = path
        self.timeout = timeout
        self._fd: Optional[int] = None

    def _is_stale(self) -> bool:
        try:
            with open(self.path, "r", encoding="utf-8", errors="replace") as fh:
                parts = fh.read().split("\n")
            st = os.stat(self.path)
        except FileNotFoundError:
            return True
        except OSError:
            return False
        pid = int(parts[0]) if parts and parts[0].strip().isdigit() else 0
        host = parts[2] if len(parts) > 2 else ""
        if host == socket.gethostname() and pid and not _pid_alive(pid):
            return True
        return (time.time() - st.st_mtime) > self.STALE_SECONDS

    def acquire(self) -> None:
        os.makedirs(os.path.dirname(os.path.abspath(self.path)) or ".", exist_ok=True)
        deadline = time.monotonic() + self.timeout
        delay = 0.01
        while True:
            try:
                fd = os.open(self.path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)
            except FileExistsError:
                if self._is_stale():
                    try:
                        os.unlink(self.path)
                    except FileNotFoundError:
                        pass
                    continue
                if time.monotonic() >= deadline:
                    raise LockTimeout("settings file is locked by another process: %s" % self.path)
                time.sleep(delay)
                delay = min(delay * 2, 0.2)
                continue
            payload = "%d\n%s\n%s\n" % (os.getpid(), _process_name(), socket.gethostname())
            os.write(fd, payload.encode("utf-8"))
            self._fd = fd
            return

    def release(self) -> None:
        if self._fd is not None:
            try:
                os.close(self._fd)
            finally:
                self._fd = None
                try:
                    os.unlink(self.path)
                except FileNotFoundError:
                    pass

    def __enter__(self) -> "QtLockFile":
        self.acquire()
        return self

    def __exit__(self, *exc) -> None:
        self.release()


def atomic_write(path: str, data: bytes, mode: int = 0o600) -> None:
    directory = os.path.dirname(os.path.abspath(path)) or "."
    os.makedirs(directory, exist_ok=True)
    try:
        mode = os.stat(path).st_mode & 0o777
    except FileNotFoundError:
        pass
    fd, tmp = tempfile.mkstemp(prefix=".%s." % os.path.basename(path), dir=directory)
    try:
        with os.fdopen(fd, "wb") as fh:
            fh.write(data)
            fh.flush()
            os.fsync(fh.fileno())
        os.chmod(tmp, mode)
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except FileNotFoundError:
            pass
        raise


def read_document(path: str) -> QSettingsDocument:
    try:
        with open(path, "r", encoding="utf-8", errors="surrogateescape") as fh:
            return QSettingsDocument.parse(fh.read())
    except FileNotFoundError:
        return QSettingsDocument()


def iter_array(doc: QSettingsDocument, prefix: str) -> Iterable[Tuple[int, str]]:
    """Yields (index, item_prefix) for a Qt settings array, 0-based index."""
    for i in range(doc.array_size(prefix)):
        yield i, "%s/%d" % (prefix, i + 1)


def write_array_size(doc: QSettingsDocument, prefix: str, size: int) -> None:
    doc.set(prefix + "/size", size)


def dict_of(doc: QSettingsDocument) -> Dict[str, Optional[Value]]:
    return {k: doc.get(k) for k in doc.keys()}

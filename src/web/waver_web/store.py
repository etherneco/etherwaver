"""Shared configuration store.

Both panels manage the same files:

* the QSettings file of the desktop panel (general settings and the
  ``internalConfig`` group with screens, links, options and hotkeys);
* ``<profile>/gui-state.ini`` (MainWindow's persisted server/client mode).

Every read returns an ETag per resource computed from the *parsed* values,
so cosmetic rewrites by Qt (key order) never cause false conflicts while any
real change by the desktop panel does.  Writes take the Qt-compatible lock,
re-read the file, compare the ETag supplied by the client (optimistic
concurrency) and only then write atomically.
"""

from __future__ import annotations

import asyncio
import logging
import os
import socket
import threading
from typing import Any, Callable, Dict, Optional, Tuple

from . import model
from .qsettings import QSettingsDocument, QtLockFile, atomic_write, read_document

log = logging.getLogger("waver_web.store")


class ConflictError(Exception):
    def __init__(self, resource: str, current: Dict[str, Any], etag: str):
        super().__init__("%s was modified by another panel" % resource)
        self.resource = resource
        self.current = current
        self.etag = etag


class ReadOnlyError(Exception):
    pass


def default_settings_path() -> str:
    base = os.environ.get("XDG_CONFIG_HOME") or os.path.join(os.path.expanduser("~"), ".config")
    return os.path.join(base, "Debauchee", "EtherWaver.conf")


def default_profile_dir() -> str:
    base = os.environ.get("XDG_DATA_HOME") or os.path.join(os.path.expanduser("~"), ".local", "share")
    return os.path.join(base, "etherwaver")


class SharedConfigStore:
    def __init__(self, settings_path: str, gui_state_path: str, read_only: bool = False) -> None:
        self.settings_path = settings_path
        self.gui_state_path = gui_state_path
        self.read_only = read_only
        self._lock = threading.Lock()
        self._last_signature: Optional[Tuple] = None
        self._last_etags: Dict[str, str] = {}

    # -- reading ------------------------------------------------------------
    def _load(self) -> Tuple[QSettingsDocument, QSettingsDocument]:
        return read_document(self.settings_path), read_document(self.gui_state_path)

    @staticmethod
    def _hostname() -> str:
        return socket.gethostname()

    def _build(self, doc: QSettingsDocument, state: QSettingsDocument) -> Dict[str, Any]:
        general = model.general_from_settings(doc, state, self._hostname())
        server_name = general["screenName"] or self._hostname()
        layout = model.layout_from_settings(doc, server_name)
        return {
            "general": general,
            "layout": layout,
            "etags": {"general": model.etag_of(general), "layout": model.etag_of(layout)},
            "serverScreen": model.ensure_screen_number_suffix(server_name),
        }

    def snapshot(self) -> Dict[str, Any]:
        with self._lock:
            doc, state = self._load()
            snap = self._build(doc, state)
            self._last_etags = dict(snap["etags"])
            return snap

    def file_signature(self) -> Tuple:
        sig = []
        for path in (self.settings_path, self.gui_state_path):
            try:
                st = os.stat(path)
                sig.append((st.st_mtime_ns, st.st_size, st.st_ino))
            except FileNotFoundError:
                sig.append(None)
        return tuple(sig)

    # -- writing ------------------------------------------------------------
    def _write(self, doc: QSettingsDocument, state: QSettingsDocument, state_changed: bool) -> None:
        atomic_write(self.settings_path, doc.serialize().encode("utf-8"), 0o600)
        if state_changed:
            atomic_write(self.gui_state_path, state.serialize().encode("utf-8"), 0o600)

    def update_general(self, values: Dict[str, Any], if_match: Optional[str]) -> Dict[str, Any]:
        if self.read_only:
            raise ReadOnlyError("configuration store is read-only")
        clean = model.validate_general(values)
        with self._lock:
            with QtLockFile(self.settings_path + ".lock"), QtLockFile(self.gui_state_path + ".lock"):
                doc, state = self._load()
                current = self._build(doc, state)
                if if_match is not None and if_match != current["etags"]["general"]:
                    raise ConflictError("general", current["general"], current["etags"]["general"])
                model.apply_general(doc, state, clean)
                state_changed = any(k in clean for k in ("mode", "configSource", "externalConfigFile", "serverHostname"))
                self._write(doc, state, state_changed)
                doc, state = self._load()
                snap = self._build(doc, state)
                self._last_etags = dict(snap["etags"])
                self._last_signature = self.file_signature()
                return snap

    def replace_layout(self, layout: Dict[str, Any], if_match: Optional[str]) -> Dict[str, Any]:
        if self.read_only:
            raise ReadOnlyError("configuration store is read-only")
        with self._lock:
            with QtLockFile(self.settings_path + ".lock"):
                doc, state = self._load()
                current = self._build(doc, state)
                if if_match is not None and if_match != current["etags"]["layout"]:
                    raise ConflictError("layout", current["layout"], current["etags"]["layout"])
                clean = model.validate_layout(layout, current["serverScreen"])
                model.apply_layout(doc, clean)
                self._write(doc, state, False)
                doc, state = self._load()
                snap = self._build(doc, state)
                self._last_etags = dict(snap["etags"])
                self._last_signature = self.file_signature()
                return snap

    def import_all(self, general: Dict[str, Any], layout: Dict[str, Any],
                   general_etag: str, layout_etag: str) -> Dict[str, Any]:
        """Replaces general settings and layout atomically (both or nothing)."""
        if self.read_only:
            raise ReadOnlyError("configuration store is read-only")
        clean_general = model.validate_general(general)
        with self._lock:
            with QtLockFile(self.settings_path + ".lock"), QtLockFile(self.gui_state_path + ".lock"):
                doc, state = self._load()
                current = self._build(doc, state)
                if general_etag != current["etags"]["general"]:
                    raise ConflictError("general", current["general"], current["etags"]["general"])
                if layout_etag != current["etags"]["layout"]:
                    raise ConflictError("layout", current["layout"], current["etags"]["layout"])
                model.apply_general(doc, state, clean_general)
                server_name = model.ensure_screen_number_suffix(
                    model.general_from_settings(doc, state, self._hostname())["screenName"] or self._hostname())
                clean_layout = model.validate_layout(layout, server_name)
                model.apply_layout(doc, clean_layout)
                self._write(doc, state, True)
                doc, state = self._load()
                snap = self._build(doc, state)
                self._last_etags = dict(snap["etags"])
                self._last_signature = self.file_signature()
                return snap

    # -- watching -----------------------------------------------------------
    async def watch(self, on_change: Callable[[Dict[str, Any], Dict[str, bool]], Any],
                    interval: float = 0.75) -> None:
        """Polls the files and reports which resources changed on disk."""
        loop = asyncio.get_running_loop()
        self._last_signature = self.file_signature()
        if not self._last_etags:
            await loop.run_in_executor(None, self.snapshot)
        while True:
            await asyncio.sleep(interval)
            sig = self.file_signature()
            if sig == self._last_signature:
                continue
            self._last_signature = sig
            previous = dict(self._last_etags)
            try:
                snap = await loop.run_in_executor(None, self.snapshot)
            except Exception:  # pragma: no cover - defensive
                log.exception("failed to re-read shared configuration")
                continue
            changed = {k: previous.get(k) != snap["etags"][k] for k in snap["etags"]}
            if any(changed.values()):
                try:
                    result = on_change(snap, changed)
                    if asyncio.iscoroutine(result):
                        await result
                except Exception:  # pragma: no cover
                    log.exception("config change handler failed")

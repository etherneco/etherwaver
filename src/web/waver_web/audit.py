"""Append-only audit log of administrative operations (JSON lines).

Records who, when, on which resource and with what result.  Never records
passwords, tokens, session ids or keystroke contents: callers pass only a
small ``detail`` dict which is filtered through :func:`sanitize`.
"""

from __future__ import annotations

import json
import os
import threading
import time
from typing import Any, Dict, List, Optional

_SECRET_KEYS = {"password", "newpassword", "currentpassword", "token", "csrf", "csrftoken",
                "session", "cookie", "secret", "keys", "authorization"}


def sanitize(detail: Any, depth: int = 0) -> Any:
    if depth > 3:
        return "…"
    if isinstance(detail, dict):
        out = {}
        for k, v in list(detail.items())[:20]:
            if str(k).lower().replace("_", "").replace("-", "") in _SECRET_KEYS:
                out[k] = "[redacted]"
            else:
                out[k] = sanitize(v, depth + 1)
        return out
    if isinstance(detail, list):
        return [sanitize(v, depth + 1) for v in detail[:20]]
    if isinstance(detail, str):
        return detail[:300]
    if isinstance(detail, (int, float, bool)) or detail is None:
        return detail
    return str(detail)[:300]


class AuditLog:
    def __init__(self, path: str, max_bytes: int = 5 * 1024 * 1024, keep: int = 3) -> None:
        self.path = path
        self.max_bytes = max_bytes
        self.keep = keep
        self._lock = threading.Lock()
        os.makedirs(os.path.dirname(path), exist_ok=True)

    def record(self, *, user: Optional[str], role: Optional[str], ip: str, action: str,
               resource: str, result: str, request_id: str, detail: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
        entry = {
            "ts": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "user": user, "role": role, "ip": ip, "action": action,
            "resource": resource, "result": result, "requestId": request_id,
        }
        if detail:
            entry["detail"] = sanitize(detail)
        line = json.dumps(entry, ensure_ascii=False) + "\n"
        with self._lock:
            self._rotate()
            fd = os.open(self.path, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
            try:
                os.write(fd, line.encode("utf-8"))
            finally:
                os.close(fd)
        return entry

    def _rotate(self) -> None:
        try:
            if os.path.getsize(self.path) < self.max_bytes:
                return
        except FileNotFoundError:
            return
        for i in range(self.keep - 1, 0, -1):
            src = "%s.%d" % (self.path, i)
            if os.path.exists(src):
                os.replace(src, "%s.%d" % (self.path, i + 1))
        os.replace(self.path, self.path + ".1")

    def read(self, limit: int = 200, user: Optional[str] = None, action: Optional[str] = None) -> List[Dict[str, Any]]:
        try:
            with open(self.path, "r", encoding="utf-8") as fh:
                lines = fh.readlines()[-5000:]
        except FileNotFoundError:
            return []
        out = []
        for line in reversed(lines):
            try:
                e = json.loads(line)
            except ValueError:
                continue
            if user and e.get("user") != user:
                continue
            if action and not str(e.get("action", "")).startswith(action):
                continue
            out.append(e)
            if len(out) >= limit:
                break
        return out

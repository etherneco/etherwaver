"""Accounts, roles, sessions and CSRF protection for the web panel.

Etherwaver itself has no user accounts (peers are identified by TLS
fingerprints only), so the web panel keeps its own local accounts in
``<profile>/web/users.json`` (mode 0600).  There is no default account and
no built-in password: the first administrator is created on the command
line (``waver-web user add <name> --role admin``).

Roles (each includes the previous one):

* ``viewer``   - read status, devices, sessions and configuration
* ``operator`` - switch the controlled screen, lock the cursor, apply the
                 configuration, start/stop the managed service, read logs
* ``admin``    - change configuration, trust fingerprints, import, manage
                 users, read the audit log
"""

from __future__ import annotations

import base64
import hashlib
import hmac
import json
import os
import re
import secrets
import threading
import time
from typing import Any, Dict, List, Optional

from .qsettings import atomic_write

ROLES = ["viewer", "operator", "admin"]
USERNAME_RE = re.compile(r"^[A-Za-z0-9._-]{1,64}$")

PERMISSIONS: Dict[str, str] = {
    "status:read": "viewer",
    "devices:read": "viewer",
    "config:read": "viewer",
    "jobs:read": "viewer",
    "control:switch": "operator",
    "control:lock": "operator",
    "config:apply": "operator",
    "service:manage": "operator",
    "logs:read": "operator",
    "diagnostics:read": "operator",
    "config:write": "admin",
    "security:read": "viewer",
    "security:write": "admin",
    "users:manage": "admin",
    "audit:read": "admin",
}


def role_allows(role: str, permission: str) -> bool:
    needed = PERMISSIONS.get(permission)
    if needed is None or role not in ROLES:
        return False
    return ROLES.index(role) >= ROLES.index(needed)


def permissions_for(role: str) -> List[str]:
    return sorted(p for p in PERMISSIONS if role_allows(role, p))


# ---------------------------------------------------------------------------
# password hashing (scrypt, falls back to PBKDF2 where scrypt is unavailable)
# ---------------------------------------------------------------------------

def hash_password(password: str) -> str:
    salt = secrets.token_bytes(16)
    if hasattr(hashlib, "scrypt"):
        n, r, p = 2 ** 14, 8, 1
        digest = hashlib.scrypt(password.encode("utf-8"), salt=salt, n=n, r=r, p=p, dklen=32)
        return "scrypt$%d$%d$%d$%s$%s" % (n, r, p, base64.b64encode(salt).decode(), base64.b64encode(digest).decode())
    iterations = 310000
    digest = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), salt, iterations)
    return "pbkdf2$%d$%s$%s" % (iterations, base64.b64encode(salt).decode(), base64.b64encode(digest).decode())


def verify_password(password: str, encoded: str) -> bool:
    try:
        parts = encoded.split("$")
        if parts[0] == "scrypt":
            n, r, p = int(parts[1]), int(parts[2]), int(parts[3])
            salt, expected = base64.b64decode(parts[4]), base64.b64decode(parts[5])
            digest = hashlib.scrypt(password.encode("utf-8"), salt=salt, n=n, r=r, p=p, dklen=len(expected))
        elif parts[0] == "pbkdf2":
            iterations = int(parts[1])
            salt, expected = base64.b64decode(parts[2]), base64.b64decode(parts[3])
            digest = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), salt, iterations)
        else:
            return False
    except (ValueError, IndexError):
        return False
    return hmac.compare_digest(digest, expected)


_DUMMY_HASH = None


def dummy_verify(password: str) -> None:
    """Spends the same time as a real check (avoids user enumeration)."""
    global _DUMMY_HASH
    if _DUMMY_HASH is None:
        _DUMMY_HASH = hash_password("invalid-" + secrets.token_hex(8))
    verify_password(password, _DUMMY_HASH)


def validate_password(password: Any) -> Optional[str]:
    if not isinstance(password, str):
        return "password must be a string"
    if len(password) < 10:
        return "password must have at least 10 characters"
    if len(password) > 1024:
        return "password is too long"
    return None


class UserStore:
    def __init__(self, path: str) -> None:
        self.path = path
        self._lock = threading.Lock()

    def _read(self) -> Dict[str, Any]:
        try:
            with open(self.path, "r", encoding="utf-8") as fh:
                data = json.load(fh)
        except FileNotFoundError:
            return {"version": 1, "users": []}
        if not isinstance(data, dict) or not isinstance(data.get("users"), list):
            raise ValueError("corrupt users file %s" % self.path)
        return data

    def _write(self, data: Dict[str, Any]) -> None:
        atomic_write(self.path, (json.dumps(data, indent=2) + "\n").encode("utf-8"), 0o600)
        os.chmod(self.path, 0o600)

    def list(self) -> List[Dict[str, Any]]:
        with self._lock:
            return [{"username": u["username"], "role": u["role"], "disabled": bool(u.get("disabled")),
                     "created": u.get("created"), "passwordChanged": u.get("passwordChanged")}
                    for u in self._read()["users"]]

    def get(self, username: str) -> Optional[Dict[str, Any]]:
        with self._lock:
            for u in self._read()["users"]:
                if u["username"] == username:
                    return dict(u)
        return None

    def count_admins(self, data: Dict[str, Any]) -> int:
        return sum(1 for u in data["users"] if u["role"] == "admin" and not u.get("disabled"))

    def add(self, username: str, password: str, role: str) -> None:
        if not USERNAME_RE.match(username or ""):
            raise ValueError("username: use 1-64 letters, digits, '.', '_' or '-'")
        if role not in ROLES:
            raise ValueError("role must be one of " + ", ".join(ROLES))
        problem = validate_password(password)
        if problem:
            raise ValueError(problem)
        with self._lock:
            data = self._read()
            if any(u["username"] == username for u in data["users"]):
                raise KeyError("user already exists")
            now = int(time.time())
            data["users"].append({"username": username, "role": role, "password": hash_password(password),
                                  "created": now, "passwordChanged": now, "disabled": False})
            self._write(data)

    def update(self, username: str, role: Optional[str] = None, disabled: Optional[bool] = None,
               password: Optional[str] = None) -> Dict[str, Any]:
        with self._lock:
            data = self._read()
            user = next((u for u in data["users"] if u["username"] == username), None)
            if user is None:
                raise LookupError("no such user")
            if role is not None:
                if role not in ROLES:
                    raise ValueError("role must be one of " + ", ".join(ROLES))
                user["role"] = role
            if disabled is not None:
                user["disabled"] = bool(disabled)
            if password is not None:
                problem = validate_password(password)
                if problem:
                    raise ValueError(problem)
                user["password"] = hash_password(password)
                user["passwordChanged"] = int(time.time())
            if self.count_admins(data) == 0:
                raise PermissionError("at least one enabled administrator must remain")
            self._write(data)
            return {"username": user["username"], "role": user["role"], "disabled": bool(user.get("disabled"))}

    def remove(self, username: str) -> None:
        with self._lock:
            data = self._read()
            before = len(data["users"])
            data["users"] = [u for u in data["users"] if u["username"] != username]
            if len(data["users"]) == before:
                raise LookupError("no such user")
            if self.count_admins(data) == 0:
                raise PermissionError("at least one enabled administrator must remain")
            self._write(data)

    def authenticate(self, username: str, password: str) -> Optional[Dict[str, Any]]:
        user = self.get(username) if isinstance(username, str) else None
        if user is None or user.get("disabled"):
            dummy_verify(password if isinstance(password, str) else "")
            return None
        if not isinstance(password, str) or not verify_password(password, user["password"]):
            return None
        return {"username": user["username"], "role": user["role"]}


class Session:
    __slots__ = ("id", "username", "role", "csrf", "created", "last_seen", "ip")

    def __init__(self, username: str, role: str, ip: str) -> None:
        self.id = secrets.token_urlsafe(32)
        self.username = username
        self.role = role
        self.csrf = secrets.token_urlsafe(32)
        self.created = time.time()
        self.last_seen = self.created
        self.ip = ip


class SessionManager:
    def __init__(self, idle_timeout: float = 1800.0, absolute_timeout: float = 43200.0) -> None:
        self.idle_timeout = idle_timeout
        self.absolute_timeout = absolute_timeout
        self._sessions: Dict[str, Session] = {}
        self._failures: Dict[str, List[float]] = {}

    def create(self, username: str, role: str, ip: str) -> Session:
        self._purge()
        s = Session(username, role, ip)
        self._sessions[s.id] = s
        return s

    def get(self, sid: Optional[str], touch: bool = True) -> Optional[Session]:
        if not sid:
            return None
        s = self._sessions.get(sid)
        if s is None:
            return None
        now = time.time()
        if now - s.last_seen > self.idle_timeout or now - s.created > self.absolute_timeout:
            self._sessions.pop(sid, None)
            return None
        if touch:
            s.last_seen = now
        return s

    def expires_at(self, s: Session) -> float:
        return min(s.last_seen + self.idle_timeout, s.created + self.absolute_timeout)

    def destroy(self, sid: str) -> None:
        self._sessions.pop(sid, None)

    def destroy_user(self, username: str) -> None:
        for sid in [k for k, v in self._sessions.items() if v.username == username]:
            self._sessions.pop(sid, None)

    def update_role(self, username: str, role: str) -> None:
        for s in self._sessions.values():
            if s.username == username:
                s.role = role

    def _purge(self) -> None:
        now = time.time()
        for sid in [k for k, s in self._sessions.items()
                    if now - s.last_seen > self.idle_timeout or now - s.created > self.absolute_timeout]:
            self._sessions.pop(sid, None)

    def count(self) -> int:
        self._purge()
        return len(self._sessions)

    # -- brute force throttling --------------------------------------------
    def throttled(self, key: str, limit: int = 5, window: float = 300.0) -> bool:
        now = time.time()
        attempts = [t for t in self._failures.get(key, []) if now - t < window]
        self._failures[key] = attempts
        return len(attempts) >= limit

    def record_failure(self, key: str) -> None:
        self._failures.setdefault(key, []).append(time.time())

    def clear_failures(self, key: str) -> None:
        self._failures.pop(key, None)

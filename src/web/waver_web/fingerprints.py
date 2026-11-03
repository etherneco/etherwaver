"""SSL fingerprint databases (port of barrier::FingerprintDatabase).

Files live in ``<profile>/SSL/Fingerprints``:

* ``Local.txt``            - this machine's certificate fingerprints
* ``TrustedClients.txt``   - clients trusted by this server
* ``TrustedServers.txt``   - servers trusted by this client

Lines are ``v2:<algorithm>:<lowercase hex>`` (legacy v1 lines are SHA1 in
``AA:BB:..`` form and are preserved).
"""

from __future__ import annotations

import os
import re
import threading
from typing import Dict, List, Optional

from .qsettings import atomic_write

FINGERPRINT_LOG_RE = re.compile(
    r"peer fingerprint \(SHA1\): ([A-F0-9:]+) \(SHA256\): ([A-F0-9:]+)")
HEX_RE = re.compile(r"^[0-9a-fA-F:]+$")


def normalize_hex(value: str) -> str:
    return value.replace(":", "").strip().lower()


def format_fingerprint(hex_value: str) -> str:
    h = normalize_hex(hex_value).upper()
    return ":".join(h[i:i + 2] for i in range(0, len(h), 2))


def parse_line(line: str) -> Optional[Dict[str, str]]:
    line = line.strip()
    if not line:
        return None
    if line.count(":") == 19 and len(line) == 59:
        return {"algorithm": "sha1", "hex": normalize_hex(line), "raw": line}
    parts = line.split(":", 2)
    if len(parts) != 3 or parts[0] != "v2":
        return None
    if not HEX_RE.match(parts[2]):
        return None
    return {"algorithm": parts[1], "hex": normalize_hex(parts[2]), "raw": line}


class FingerprintStore:
    def __init__(self, profile_dir: str) -> None:
        self.dir = os.path.join(profile_dir, "SSL", "Fingerprints")
        self._lock = threading.Lock()

    def _path(self, kind: str) -> str:
        names = {"local": "Local.txt", "clients": "TrustedClients.txt", "servers": "TrustedServers.txt"}
        return os.path.join(self.dir, names[kind])

    def read(self, kind: str) -> List[Dict[str, str]]:
        try:
            with open(self._path(kind), "r", encoding="utf-8") as fh:
                entries = [parse_line(l) for l in fh]
        except FileNotFoundError:
            return []
        out = []
        for e in entries:
            if e:
                e = dict(e)
                e["display"] = format_fingerprint(e["hex"])
                out.append(e)
        return out

    def is_trusted(self, kind: str, sha256_hex: str) -> bool:
        h = normalize_hex(sha256_hex)
        return any(e["algorithm"] == "sha256" and e["hex"] == h for e in self.read(kind))

    def add(self, kind: str, sha256_hex: str) -> bool:
        h = normalize_hex(sha256_hex)
        if len(h) != 64 or not re.fullmatch(r"[0-9a-f]{64}", h):
            raise ValueError("a SHA-256 fingerprint has 32 bytes")
        with self._lock:
            entries = self.read(kind)
            if any(e["algorithm"] == "sha256" and e["hex"] == h for e in entries):
                return False
            lines = [e["raw"] for e in entries] + ["v2:sha256:" + h]
            atomic_write(self._path(kind), ("\n".join(lines) + "\n").encode("utf-8"), 0o600)
            return True

    def remove(self, kind: str, sha256_hex: str) -> bool:
        h = normalize_hex(sha256_hex)
        with self._lock:
            entries = self.read(kind)
            kept = [e for e in entries if not (e["hex"] == h)]
            if len(kept) == len(entries):
                return False
            atomic_write(self._path(kind), ("".join(e["raw"] + "\n" for e in kept)).encode("utf-8"), 0o600)
            return True

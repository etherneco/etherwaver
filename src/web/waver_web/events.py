"""Live update hub feeding Server-Sent Events streams.

Every event gets an id ``<epoch>-<seq>``.  The epoch changes when the web
backend restarts, so a browser that reconnects with an id from a previous
epoch (or older than the replay buffer) is told to ``resync``: it must
reload the full state instead of trusting what it had.
"""

from __future__ import annotations

import asyncio
import json
import secrets
import time
from collections import deque
from typing import Any, Deque, Dict, List, Optional, Set, Tuple


class Subscriber:
    def __init__(self, role: str, maxsize: int) -> None:
        self.queue: "asyncio.Queue[Optional[Tuple[str, str, str]]]" = asyncio.Queue(maxsize=maxsize)
        self.role = role
        self.overflowed = False


class EventHub:
    def __init__(self, replay: int = 500, queue_size: int = 512) -> None:
        self.epoch = secrets.token_hex(4)
        self.seq = 0
        self._buffer: Deque[Tuple[int, str, str, Optional[str]]] = deque(maxlen=replay)
        self._subs: Set[Subscriber] = set()
        self.queue_size = queue_size

    def _event_id(self, seq: int) -> str:
        return "%s-%d" % (self.epoch, seq)

    def publish(self, event: str, data: Dict[str, Any], permission: Optional[str] = None) -> None:
        """``permission`` restricts delivery to roles that hold it."""
        from .auth import role_allows
        self.seq += 1
        payload = json.dumps({"type": event, "ts": time.time(), "data": data}, separators=(",", ":"))
        self._buffer.append((self.seq, event, payload, permission))
        item = (self._event_id(self.seq), event, payload)
        for sub in list(self._subs):
            if permission and not role_allows(sub.role, permission):
                continue
            if sub.overflowed:
                continue
            try:
                sub.queue.put_nowait(item)
            except asyncio.QueueFull:
                # a stalled browser must not hold memory; it will resync
                sub.overflowed = True
                try:
                    while True:
                        sub.queue.get_nowait()
                except asyncio.QueueEmpty:
                    pass
                # one final item; the stream ends after it and the browser
                # reconnects (replay or full resync)
                sub.queue.put_nowait((self._event_id(self.seq), "resync", json.dumps(
                    {"type": "resync", "ts": time.time(), "data": {"reason": "overflow"}})))

    def subscribe(self, role: str, last_event_id: Optional[str]) -> Tuple[Subscriber, List[Tuple[str, str, str]]]:
        """Returns the subscriber and the events to replay first."""
        from .auth import role_allows
        sub = Subscriber(role, self.queue_size)
        self._subs.add(sub)
        replay: List[Tuple[str, str, str]] = []
        resync_reason = "initial"
        if last_event_id:
            epoch, _, seq_text = last_event_id.partition("-")
            try:
                last_seq = int(seq_text)
            except ValueError:
                last_seq = -1
            oldest = self._buffer[0][0] if self._buffer else self.seq + 1
            if epoch == self.epoch and last_seq >= oldest - 1 and last_seq <= self.seq:
                for seq, event, payload, permission in self._buffer:
                    if seq > last_seq and (not permission or role_allows(role, permission)):
                        replay.append((self._event_id(seq), event, payload))
                resync_reason = ""
            else:
                resync_reason = "gap" if epoch == self.epoch else "backend_restarted"
        if resync_reason:
            replay.append((self._event_id(self.seq), "resync", json.dumps(
                {"type": "resync", "ts": time.time(), "data": {"reason": resync_reason}})))
        return sub, replay

    def unsubscribe(self, sub: Subscriber) -> None:
        self._subs.discard(sub)

    def close_all(self) -> None:
        for sub in list(self._subs):
            try:
                sub.queue.put_nowait(None)
            except asyncio.QueueFull:
                pass

    @property
    def subscriber_count(self) -> int:
        return len(self._subs)

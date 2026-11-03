"""Adapter for the Etherwaver Control Protocol (ECP v1).

The browser never talks to the socket protocol.  This module owns a single
long-lived connection from the web backend to ``wavers`` and exposes a small
async API:

* message framing that is independent of TCP segmentation (``LineFramer``);
* request/response correlation by ``id`` - many concurrent HTTP requests
  (several browser tabs) share the connection safely;
* events (``type: event``) are dispatched separately from responses, even
  when they arrive between or inside a burst of responses;
* per-request timeouts and cancellation;
* bounded concurrency (in-flight limit) and a bounded wait queue;
* controlled reconnection with exponential backoff;
* commands that change state are never retried automatically.  If the
  connection drops or the request times out before a response arrives, the
  caller receives :class:`OutcomeUnknown` and must re-read the real state.
"""

from __future__ import annotations

import asyncio
import json
import logging
import random
import time
from typing import Any, Awaitable, Callable, Dict, List, Optional

log = logging.getLogger("waver_web.control")

#: commands that do not modify server state (safe to report as plain failures)
READ_ONLY_COMMANDS = {"status", "layout", "config.get", "ping"}


class ControlError(Exception):
    """The server answered with ``ok: false``."""

    def __init__(self, code: str, message: str):
        super().__init__("%s: %s" % (code, message))
        self.code = code
        self.message = message


class BackendUnavailable(Exception):
    """No connection to the Etherwaver server (request was not sent)."""


class BackendBusy(Exception):
    """Too many queued requests; the request was not sent."""


class RequestTimeout(Exception):
    """A read-only request timed out (no state was changed)."""


class OutcomeUnknown(Exception):
    """A state-changing request was sent but no response was received."""


class ProtocolError(Exception):
    pass


class LineFramer:
    """Splits a byte stream into newline-terminated frames.

    Partial frames are buffered until their terminator arrives; one chunk
    may contain any number of frames.  Frames longer than ``max_line`` raise
    :class:`ProtocolError` (the connection must then be dropped).
    """

    def __init__(self, max_line: int = 2 * 1024 * 1024) -> None:
        self.max_line = max_line
        self._buf = bytearray()

    def feed(self, data: bytes) -> List[bytes]:
        self._buf.extend(data)
        frames: List[bytes] = []
        start = 0
        while True:
            nl = self._buf.find(b"\n", start)
            if nl < 0:
                break
            frame = bytes(self._buf[start:nl]).rstrip(b"\r")
            start = nl + 1
            if len(frame) > self.max_line:
                raise ProtocolError("frame too long")
            if frame.strip():
                frames.append(frame)
        del self._buf[:start]
        if len(self._buf) > self.max_line:
            raise ProtocolError("frame too long")
        return frames

    def reset(self) -> None:
        self._buf.clear()


EventCallback = Callable[[str, Dict[str, Any], int], Any]
StateCallback = Callable[[str, Dict[str, Any]], Any]


class ControlClient:
    def __init__(self, host: str, port: int, token_loader: Callable[[], Optional[str]],
                 on_event: Optional[EventCallback] = None,
                 on_state: Optional[StateCallback] = None,
                 connect_timeout: float = 3.0,
                 request_timeout: float = 5.0,
                 max_inflight: int = 8,
                 max_waiting: int = 64,
                 subscribe_log: bool = True,
                 backoff_initial: float = 0.5,
                 backoff_max: float = 10.0) -> None:
        self.host = host
        self.port = port
        self._token_loader = token_loader
        self._on_event = on_event
        self._on_state = on_state
        self.connect_timeout = connect_timeout
        self.request_timeout = request_timeout
        self.max_inflight = max_inflight
        self.max_waiting = max_waiting
        self.subscribe_log = subscribe_log
        self.backoff_initial = backoff_initial
        self.backoff_max = backoff_max

        self.state = "disconnected"
        self.last_error: Optional[str] = None
        self.hello: Dict[str, Any] = {}
        self.connected_since: Optional[float] = None
        self.reconnects = 0
        self.last_event_seq = 0
        self.stats = {"sent": 0, "responses": 0, "events": 0, "timeouts": 0,
                      "uncertain": 0, "late_responses": 0, "protocol_errors": 0}

        self._reader: Optional[asyncio.StreamReader] = None
        self._writer: Optional[asyncio.StreamWriter] = None
        self._pending: Dict[int, Dict[str, Any]] = {}
        self._next_id = 1
        self._inflight: Optional[asyncio.Semaphore] = None
        self._waiting = 0
        self._task: Optional[asyncio.Task] = None
        self._stopping = False
        self._connected_event: Optional[asyncio.Event] = None
        self._write_lock: Optional[asyncio.Lock] = None
        self._wake: Optional[asyncio.Event] = None
        self._callback_tasks: set = set()
        self._framer = LineFramer()
        self._queued: List[Dict[str, Any]] = []
        self._early: List[Dict[str, Any]] = []

    # ------------------------------------------------------------------ life
    def start(self) -> None:
        self._inflight = asyncio.Semaphore(self.max_inflight)
        self._connected_event = asyncio.Event()
        self._write_lock = asyncio.Lock()
        self._wake = asyncio.Event()
        self._stopping = False
        self._task = asyncio.ensure_future(self._run())

    async def stop(self) -> None:
        self._stopping = True
        if self._wake:
            self._wake.set()
        if self._task:
            self._task.cancel()
            try:
                await self._task
            except (asyncio.CancelledError, Exception):
                pass
        await self._close_transport("stopped")

    def reconnect_now(self) -> None:
        """Skip the current backoff delay (e.g. after the server restarted)."""
        if self._wake:
            self._wake.set()

    @property
    def connected(self) -> bool:
        return self.state == "connected"

    async def wait_connected(self, timeout: float) -> bool:
        if self.connected:
            return True
        assert self._connected_event is not None
        try:
            await asyncio.wait_for(self._connected_event.wait(), timeout)
        except asyncio.TimeoutError:
            return False
        return self.connected

    async def _set_state(self, state: str, error: Optional[str] = None) -> None:
        changed = state != self.state
        self.state = state
        self.last_error = error
        if self._connected_event is not None:
            if state == "connected":
                self._connected_event.set()
            else:
                self._connected_event.clear()
        if changed and self._on_state:
            # run outside the connection task: the callback may issue
            # requests whose responses are read by this very task
            try:
                res = self._on_state(state, {"error": error, "hello": self.hello})
                if asyncio.iscoroutine(res):
                    task = asyncio.ensure_future(res)
                    self._callback_tasks.add(task)
                    task.add_done_callback(self._callback_tasks.discard)
            except Exception:  # pragma: no cover
                log.exception("state callback failed")

    async def _run(self) -> None:
        delay = self.backoff_initial
        while not self._stopping:
            await self._set_state("connecting", self.last_error)
            try:
                await self._connect_once()
                delay = self.backoff_initial
                await self._read_loop()
                error = "connection closed by server"
            except asyncio.CancelledError:
                raise
            except PermissionError as e:
                error = str(e)
                await self._close_transport(error)
                await self._set_state("auth_failed", error)
            except (OSError, asyncio.TimeoutError, ProtocolError, ConnectionError, ValueError) as e:
                error = "%s: %s" % (type(e).__name__, e) if str(e) else type(e).__name__
            await self._close_transport(error)
            if self._stopping:
                break
            if self.state != "auth_failed":
                await self._set_state("disconnected", error)
            self.reconnects += 1
            # jittered exponential backoff; reconnect_now() cuts it short
            assert self._wake is not None
            self._wake.clear()
            try:
                await asyncio.wait_for(self._wake.wait(), delay * (0.8 + random.random() * 0.4))
            except asyncio.TimeoutError:
                pass
            delay = min(delay * 2, self.backoff_max)

    async def _connect_once(self) -> None:
        reader, writer = await asyncio.wait_for(
            asyncio.open_connection(self.host, self.port), self.connect_timeout)
        self._reader, self._writer = reader, writer
        self._framer = LineFramer()
        self._early: List[Dict[str, Any]] = []
        hello = await asyncio.wait_for(self._read_message(), self.connect_timeout)
        if hello.get("type") != "hello" or hello.get("protocol") != "etherwaver-control":
            raise ProtocolError("unexpected greeting")
        if int(hello.get("version", 0)) != 1:
            raise ProtocolError("unsupported protocol version %r" % hello.get("version"))
        self.hello = hello
        token = self._token_loader()
        if not token:
            raise PermissionError("control token not available (is the server running as this user?)")
        resp = await self._handshake({"cmd": "auth", "token": token})
        if not resp.get("ok"):
            raise PermissionError("control authentication failed: %s" % resp.get("error", {}).get("message"))
        resp = await self._handshake({"cmd": "subscribe", "log": self.subscribe_log})
        if not resp.get("ok"):
            raise ProtocolError("subscribe failed")
        self.last_event_seq = int(resp.get("result", {}).get("seq", 0))
        self.connected_since = time.time()
        await self._set_state("connected")

    async def _handshake(self, message: Dict[str, Any]) -> Dict[str, Any]:
        rid = self._alloc_id()
        message = dict(message, id=rid)
        await self._send_raw(message)
        deadline = time.monotonic() + self.connect_timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise asyncio.TimeoutError()
            msg = await asyncio.wait_for(self._read_message(), remaining)
            if msg.get("type") == "response" and msg.get("id") == rid:
                return msg
            # events can precede the response; keep them for after connect
            if msg.get("type") == "event":
                self._early.append(msg)

    async def _read_message(self) -> Dict[str, Any]:
        while True:
            if self._queued:
                return self._queued.pop(0)
            assert self._reader is not None
            chunk = await self._reader.read(65536)
            if not chunk:
                raise ConnectionError("connection closed")
            frames = self._framer.feed(chunk)
            msgs = []
            for frame in frames:
                try:
                    msg = json.loads(frame.decode("utf-8"))
                except (ValueError, UnicodeDecodeError):
                    self.stats["protocol_errors"] += 1
                    raise ProtocolError("malformed frame")
                if not isinstance(msg, dict):
                    raise ProtocolError("frame is not an object")
                msgs.append(msg)
            if msgs:
                self._queued = msgs[1:]
                return msgs[0]

    async def _read_loop(self) -> None:
        for msg in self._early:
            await self._dispatch(msg)
        self._early = []
        while True:
            msg = await self._read_message()
            await self._dispatch(msg)

    async def _dispatch(self, msg: Dict[str, Any]) -> None:
        kind = msg.get("type")
        if kind == "response":
            rid = msg.get("id")
            pending = self._pending.pop(rid, None) if isinstance(rid, int) else None
            if pending is None:
                # late response for a request that already timed out, or an
                # unsolicited error (id null) such as "busy"
                self.stats["late_responses"] += 1
                log.warning("control: response without pending request id=%r", rid)
                return
            self.stats["responses"] += 1
            fut: asyncio.Future = pending["future"]
            if not fut.done():
                fut.set_result(msg)
        elif kind == "event":
            self.stats["events"] += 1
            seq = int(msg.get("seq", 0))
            gap = self.last_event_seq and seq > self.last_event_seq + 1
            self.last_event_seq = max(self.last_event_seq, seq)
            if self._on_event:
                try:
                    if gap:
                        res = self._on_event("control.gap", {"expected": self.last_event_seq + 1, "got": seq}, seq)
                        if asyncio.iscoroutine(res):
                            await res
                    res = self._on_event(str(msg.get("event", "")), msg.get("data") or {}, seq)
                    if asyncio.iscoroutine(res):
                        await res
                except Exception:  # pragma: no cover
                    log.exception("event callback failed")
        elif kind == "hello":
            self.hello = msg
        else:
            self.stats["protocol_errors"] += 1
            log.warning("control: ignoring unknown message type %r", kind)

    async def _close_transport(self, reason: Optional[str]) -> None:
        writer, self._writer, self._reader = self._writer, None, None
        self._queued = []
        if writer is not None:
            try:
                writer.close()
                await asyncio.wait_for(writer.wait_closed(), 1.0)
            except Exception:
                pass
        # fail everything still waiting; never resend
        pending, self._pending = self._pending, {}
        for rid, p in pending.items():
            fut: asyncio.Future = p["future"]
            if fut.done():
                continue
            if p["cmd"] in READ_ONLY_COMMANDS:
                fut.set_exception(BackendUnavailable("connection lost: %s" % (reason or "closed")))
            else:
                self.stats["uncertain"] += 1
                fut.set_exception(OutcomeUnknown(
                    "connection lost before the server confirmed \"%s\"; refresh to see the real state" % p["cmd"]))
        self.connected_since = None

    def _alloc_id(self) -> int:
        rid = self._next_id
        self._next_id += 1
        return rid

    async def _send_raw(self, message: Dict[str, Any]) -> None:
        if self._writer is None:
            raise BackendUnavailable("not connected")
        data = (json.dumps(message, separators=(",", ":")) + "\n").encode("utf-8")
        assert self._write_lock is not None
        async with self._write_lock:
            self._writer.write(data)
            await self._writer.drain()

    # --------------------------------------------------------------- request
    async def request(self, cmd: str, params: Optional[Dict[str, Any]] = None,
                      timeout: Optional[float] = None) -> Dict[str, Any]:
        """Sends one command and waits for its response.

        Raises BackendUnavailable / BackendBusy (not sent), ControlError
        (server refused), RequestTimeout (read-only, nothing changed) or
        OutcomeUnknown (state-changing, outcome not known).
        """
        if not self.connected:
            raise BackendUnavailable(self.last_error or "Etherwaver server is not connected")
        if self._waiting >= self.max_waiting:
            raise BackendBusy("too many requests waiting for the Etherwaver server")
        timeout = self.request_timeout if timeout is None else timeout
        deadline = time.monotonic() + timeout
        read_only = cmd in READ_ONLY_COMMANDS
        assert self._inflight is not None
        self._waiting += 1
        try:
            try:
                await asyncio.wait_for(self._inflight.acquire(), timeout)
            except asyncio.TimeoutError:
                raise BackendBusy("timed out waiting for a free request slot")
        finally:
            self._waiting -= 1
        rid = None
        try:
            if not self.connected:
                raise BackendUnavailable(self.last_error or "Etherwaver server is not connected")
            rid = self._alloc_id()
            loop = asyncio.get_running_loop()
            fut = loop.create_future()
            self._pending[rid] = {"future": fut, "cmd": cmd, "sent": time.monotonic()}
            message = dict(params or {})
            message["id"] = rid
            message["cmd"] = cmd
            try:
                await self._send_raw(message)
            except (OSError, ConnectionError) as e:
                self._pending.pop(rid, None)
                if read_only:
                    raise BackendUnavailable(str(e))
                # write may have reached the server partially or fully
                self.stats["uncertain"] += 1
                raise OutcomeUnknown("connection failed while sending \"%s\"" % cmd)
            self.stats["sent"] += 1
            remaining = max(0.01, deadline - time.monotonic())
            try:
                msg = await asyncio.wait_for(asyncio.shield(fut), remaining)
            except asyncio.TimeoutError:
                self._pending.pop(rid, None)
                self.stats["timeouts"] += 1
                if read_only:
                    raise RequestTimeout("\"%s\" timed out after %.1fs" % (cmd, timeout))
                self.stats["uncertain"] += 1
                raise OutcomeUnknown("no confirmation for \"%s\" within %.1fs; the change may or may not "
                                     "have been applied - refresh to see the real state" % (cmd, timeout))
            except asyncio.CancelledError:
                # caller went away (e.g. HTTP client disconnected); keep the
                # slot accounting correct and drop the late response later
                self._pending.pop(rid, None)
                raise
            if msg.get("ok"):
                return msg.get("result") or {}
            err = msg.get("error") or {}
            raise ControlError(str(err.get("code", "error")), str(err.get("message", "")))
        finally:
            self._inflight.release()

    async def ping(self) -> float:
        start = time.monotonic()
        await self.request("ping", timeout=2.0)
        return (time.monotonic() - start) * 1000.0

    def diagnostics(self) -> Dict[str, Any]:
        return {
            "state": self.state,
            "address": "%s:%d" % (self.host, self.port),
            "lastError": self.last_error,
            "connectedSince": self.connected_since,
            "reconnects": self.reconnects,
            "pending": len(self._pending),
            "waiting": self._waiting,
            "maxInflight": self.max_inflight,
            "lastEventSeq": self.last_event_seq,
            "server": self.hello.get("server"),
            "serverVersion": self.hello.get("serverVersion"),
            "instance": self.hello.get("instance"),
            "stats": dict(self.stats),
        }

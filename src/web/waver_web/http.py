"""A small, dependency-free asyncio HTTP/1.1 server.

Supports what the panel needs and nothing more: keep-alive, Content-Length
request bodies, streaming responses (SSE), strict limits and timeouts, a
per-request id, trusted reverse proxies (X-Forwarded-*), a base path and
graceful shutdown.
"""

from __future__ import annotations

import asyncio
import email.utils
import ipaddress
import json
import logging
import re
import secrets
import time
import urllib.parse
from typing import Any, Awaitable, Callable, Dict, List, Optional, Pattern, Set, Tuple, Union

log = logging.getLogger("waver_web.http")

REASONS = {
    200: "OK", 201: "Created", 202: "Accepted", 204: "No Content", 301: "Moved Permanently",
    302: "Found", 304: "Not Modified", 400: "Bad Request", 401: "Unauthorized", 403: "Forbidden",
    404: "Not Found", 405: "Method Not Allowed", 409: "Conflict", 411: "Length Required",
    412: "Precondition Failed", 413: "Payload Too Large", 415: "Unsupported Media Type",
    422: "Unprocessable Entity", 428: "Precondition Required", 429: "Too Many Requests",
    431: "Request Header Fields Too Large", 500: "Internal Server Error", 502: "Bad Gateway",
    503: "Service Unavailable", 504: "Gateway Timeout",
}

SECURITY_HEADERS = {
    "X-Content-Type-Options": "nosniff",
    "X-Frame-Options": "DENY",
    "Referrer-Policy": "no-referrer",
    "Cross-Origin-Opener-Policy": "same-origin",
    "Cross-Origin-Resource-Policy": "same-origin",
    "Permissions-Policy": "camera=(), microphone=(), geolocation=()",
}


class HTTPError(Exception):
    def __init__(self, status: int, code: str, message: str, details: Any = None,
                 headers: Optional[Dict[str, str]] = None):
        super().__init__(message)
        self.status = status
        self.code = code
        self.message = message
        self.details = details
        self.headers = headers or {}


class Request:
    def __init__(self) -> None:
        self.method = ""
        self.target = ""
        self.path = ""
        self.query: Dict[str, str] = {}
        self.headers: Dict[str, str] = {}
        self.body = b""
        self.peer_ip = ""
        self.client_ip = ""
        self.scheme = "http"
        self.host = ""
        self.request_id = secrets.token_hex(8)
        self.params: Dict[str, str] = {}
        self.state: Dict[str, Any] = {}
        self.cookies: Dict[str, str] = {}

    def json(self) -> Any:
        ctype = self.headers.get("content-type", "")
        if not ctype.split(";")[0].strip().lower() == "application/json":
            raise HTTPError(415, "unsupported_media_type", "expected Content-Type: application/json")
        try:
            return json.loads(self.body.decode("utf-8") or "null")
        except (ValueError, UnicodeDecodeError):
            raise HTTPError(400, "bad_json", "request body is not valid JSON")

    @property
    def origin(self) -> str:
        return "%s://%s" % (self.scheme, self.host)


class Response:
    def __init__(self, status: int = 200, body: Union[bytes, str] = b"", content_type: str = "text/plain; charset=utf-8",
                 headers: Optional[Dict[str, str]] = None) -> None:
        self.status = status
        self.body = body.encode("utf-8") if isinstance(body, str) else body
        self.headers: Dict[str, str] = {"Content-Type": content_type}
        self.cookies: List[str] = []
        if headers:
            self.headers.update(headers)

    @classmethod
    def json(cls, data: Any, status: int = 200, headers: Optional[Dict[str, str]] = None) -> "Response":
        r = cls(status, json.dumps(data, ensure_ascii=False, separators=(",", ":")), "application/json; charset=utf-8", headers)
        return r


StreamWriterFn = Callable[["StreamContext"], Awaitable[None]]


class StreamResponse:
    """A response whose body is produced incrementally (Server-Sent Events)."""

    def __init__(self, producer: StreamWriterFn, headers: Optional[Dict[str, str]] = None) -> None:
        self.producer = producer
        self.headers = {"Content-Type": "text/event-stream; charset=utf-8", "Cache-Control": "no-store",
                        "X-Accel-Buffering": "no"}
        if headers:
            self.headers.update(headers)


class StreamContext:
    def __init__(self, writer: asyncio.StreamWriter, server: "HTTPServer") -> None:
        self._writer = writer
        self._server = server

    @property
    def closing(self) -> bool:
        return self._server.closing or self._writer.is_closing()

    async def write(self, data: str) -> None:
        self._writer.write(data.encode("utf-8"))
        await self._writer.drain()


Handler = Callable[[Request], Awaitable[Union[Response, StreamResponse]]]


class Router:
    def __init__(self) -> None:
        self.routes: List[Tuple[str, Pattern, Handler]] = []

    def add(self, method: str, pattern: str, handler: Handler) -> None:
        regex = "^" + re.sub(r"\{(\w+)\}", r"(?P<\1>[^/]+)", pattern) + "$"
        self.routes.append((method, re.compile(regex), handler))

    def match(self, method: str, path: str) -> Tuple[Optional[Handler], Dict[str, str], List[str]]:
        allowed: List[str] = []
        for m, regex, handler in self.routes:
            match = regex.match(path)
            if not match:
                continue
            if m == method or (method == "HEAD" and m == "GET"):
                return handler, {k: urllib.parse.unquote(v) for k, v in match.groupdict().items()}, []
            allowed.append(m)
        return None, {}, allowed


def parse_cookies(header: str) -> Dict[str, str]:
    cookies = {}
    for part in header.split(";"):
        if "=" in part:
            k, v = part.split("=", 1)
            cookies[k.strip()] = v.strip()
    return cookies


class HTTPServer:
    MAX_HEADER_BYTES = 32 * 1024
    MAX_HEADERS = 100

    def __init__(self, app: Callable[[Request], Awaitable[Union[Response, StreamResponse]]],
                 trusted_proxies: Optional[List[str]] = None, base_path: str = "",
                 max_body: int = 4 * 1024 * 1024, keepalive_timeout: float = 15.0,
                 request_timeout: float = 30.0) -> None:
        self.app = app
        self.trusted: List[Any] = [ipaddress.ip_network(p, strict=False) for p in (trusted_proxies or [])]
        self.base_path = base_path.rstrip("/")
        self.max_body = max_body
        self.keepalive_timeout = keepalive_timeout
        self.request_timeout = request_timeout
        self.closing = False
        self._servers: List[asyncio.AbstractServer] = []
        self._connections: Set[asyncio.Task] = set()
        self._streams = 0

    def _is_trusted(self, ip: str) -> bool:
        try:
            addr = ipaddress.ip_address(ip)
        except ValueError:
            return False
        return any(addr in net for net in self.trusted)

    async def listen(self, host: str, port: int) -> None:
        srv = await asyncio.start_server(self._on_connection, host, port, reuse_address=True,
                                         limit=self.MAX_HEADER_BYTES * 2)
        self._servers.append(srv)

    @property
    def sockets(self):
        return [s for srv in self._servers for s in (srv.sockets or [])]

    async def close(self, grace: float = 5.0) -> None:
        self.closing = True
        for srv in self._servers:
            srv.close()
        for srv in self._servers:
            await srv.wait_closed()
        if self._connections:
            done, pending = await asyncio.wait(list(self._connections), timeout=grace)
            for t in pending:
                t.cancel()
            if pending:
                await asyncio.wait(pending, timeout=1.0)

    async def _on_connection(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        task = asyncio.current_task()
        assert task is not None
        self._connections.add(task)
        peer = writer.get_extra_info("peername")
        peer_ip = peer[0] if isinstance(peer, tuple) else "unix"
        try:
            while not self.closing:
                try:
                    req = await asyncio.wait_for(self._read_request(reader, peer_ip), self.keepalive_timeout)
                except asyncio.TimeoutError:
                    break
                except HTTPError as e:
                    await self._write_response(writer, None, self._error_response(e, None), False)
                    break
                if req is None:
                    break
                keep_alive = req.headers.get("connection", "").lower() != "close" and not self.closing
                started = time.monotonic()
                try:
                    resp = await self.app(req)
                except HTTPError as e:
                    resp = self._error_response(e, req)
                except asyncio.CancelledError:
                    raise
                except Exception:
                    log.exception("unhandled error request_id=%s", req.request_id)
                    resp = self._error_response(HTTPError(500, "internal", "internal server error"), req)
                if isinstance(resp, StreamResponse):
                    await self._write_stream(writer, req, resp)
                    break
                await self._write_response(writer, req, resp, keep_alive)
                log.info("%s %s %d %.1fms id=%s ip=%s", req.method, req.path, resp.status,
                         (time.monotonic() - started) * 1000, req.request_id, req.client_ip)
                if not keep_alive:
                    break
        except (ConnectionError, asyncio.IncompleteReadError):
            pass
        finally:
            self._connections.discard(task)
            try:
                writer.close()
            except Exception:
                pass

    async def _read_request(self, reader: asyncio.StreamReader, peer_ip: str) -> Optional[Request]:
        try:
            head = await reader.readuntil(b"\r\n\r\n")
        except asyncio.IncompleteReadError as e:
            if not e.partial.strip():
                return None
            raise HTTPError(400, "bad_request", "incomplete request")
        except asyncio.LimitOverrunError:
            raise HTTPError(431, "headers_too_large", "request headers too large")
        if len(head) > self.MAX_HEADER_BYTES:
            raise HTTPError(431, "headers_too_large", "request headers too large")
        lines = head.decode("latin-1").split("\r\n")
        parts = lines[0].split(" ")
        if len(parts) != 3 or not parts[2].startswith("HTTP/1."):
            raise HTTPError(400, "bad_request", "malformed request line")
        req = Request()
        req.method, req.target = parts[0].upper(), parts[1]
        headers: Dict[str, str] = {}
        for line in lines[1:]:
            if not line:
                continue
            if ":" not in line:
                raise HTTPError(400, "bad_request", "malformed header")
            k, v = line.split(":", 1)
            k = k.strip().lower()
            headers[k] = (headers[k] + ", " + v.strip()) if k in headers and k != "cookie" else v.strip()
            if len(headers) > self.MAX_HEADERS:
                raise HTTPError(431, "headers_too_large", "too many headers")
        req.headers = headers
        if "transfer-encoding" in headers:
            raise HTTPError(411, "length_required", "chunked request bodies are not supported")
        length_text = headers.get("content-length", "0") or "0"
        if not length_text.isdigit():
            raise HTTPError(400, "bad_request", "invalid Content-Length")
        length = int(length_text)
        if length > self.max_body:
            raise HTTPError(413, "payload_too_large", "request body too large")
        if length:
            req.body = await asyncio.wait_for(reader.readexactly(length), self.request_timeout)

        # addressing, honouring trusted reverse proxies only
        req.peer_ip = peer_ip
        req.client_ip = peer_ip
        req.host = headers.get("host", "")
        if self._is_trusted(peer_ip):
            fwd = headers.get("x-forwarded-for")
            if fwd:
                req.client_ip = fwd.split(",")[0].strip()
            proto = headers.get("x-forwarded-proto")
            if proto in ("http", "https"):
                req.scheme = proto
            fhost = headers.get("x-forwarded-host")
            if fhost:
                req.host = fhost.split(",")[0].strip()
        incoming_id = headers.get("x-request-id", "")
        if re.fullmatch(r"[A-Za-z0-9._-]{8,64}", incoming_id or ""):
            req.request_id = incoming_id

        url = urllib.parse.urlsplit(req.target)
        path = urllib.parse.unquote(url.path)
        if ".." in path.split("/"):
            raise HTTPError(400, "bad_request", "invalid path")
        if self.base_path:
            if path == self.base_path:
                path = "/"
                req.state["redirect_base"] = True
            elif path.startswith(self.base_path + "/"):
                path = path[len(self.base_path):]
            else:
                raise HTTPError(404, "not_found", "not found")
        req.path = path or "/"
        req.query = {k: v[-1] for k, v in urllib.parse.parse_qs(url.query, keep_blank_values=True).items()}
        req.cookies = parse_cookies(headers.get("cookie", ""))
        return req

    def _error_response(self, e: HTTPError, req: Optional[Request]) -> Response:
        body = {"error": {"code": e.code, "message": e.message,
                          "requestId": req.request_id if req else None}}
        if e.details is not None:
            body["error"]["details"] = e.details
        return Response.json(body, e.status, e.headers)

    def _base_headers(self, req: Optional[Request]) -> Dict[str, str]:
        h = dict(SECURITY_HEADERS)
        h["Date"] = email.utils.formatdate(usegmt=True)
        h["Server"] = "waver-web"
        if req is not None:
            h["X-Request-Id"] = req.request_id
            if req.scheme == "https":
                h["Strict-Transport-Security"] = "max-age=31536000"
        return h

    async def _write_response(self, writer: asyncio.StreamWriter, req: Optional[Request], resp: Response,
                              keep_alive: bool) -> None:
        headers = self._base_headers(req)
        headers.update(resp.headers)
        body = resp.body
        if req is not None and req.method == "HEAD":
            headers["Content-Length"] = str(len(body))
            body = b""
        elif resp.status in (204, 304):
            body = b""
        else:
            headers["Content-Length"] = str(len(body))
        headers["Connection"] = "keep-alive" if keep_alive else "close"
        out = ["HTTP/1.1 %d %s" % (resp.status, REASONS.get(resp.status, "Status"))]
        out += ["%s: %s" % (k, v) for k, v in headers.items()]
        out += ["Set-Cookie: %s" % c for c in resp.cookies]
        writer.write(("\r\n".join(out) + "\r\n\r\n").encode("latin-1") + body)
        await writer.drain()

    async def _write_stream(self, writer: asyncio.StreamWriter, req: Request, resp: StreamResponse) -> None:
        headers = self._base_headers(req)
        headers.update(resp.headers)
        headers["Connection"] = "close"
        out = ["HTTP/1.1 200 OK"] + ["%s: %s" % (k, v) for k, v in headers.items()]
        writer.write(("\r\n".join(out) + "\r\n\r\n").encode("latin-1"))
        await writer.drain()
        self._streams += 1
        try:
            await resp.producer(StreamContext(writer, self))
        except (ConnectionError, asyncio.CancelledError):
            pass
        finally:
            self._streams -= 1

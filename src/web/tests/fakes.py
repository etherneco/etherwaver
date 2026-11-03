"""A scriptable Etherwaver Control Protocol server for tests.

It speaks the same wire format as ``wavers`` (see
src/lib/server/ControlServer.h) and lets tests control *how* bytes are
delivered: split into tiny chunks, several messages coalesced into one
write, events injected between responses, delayed or dropped responses,
and abrupt disconnects.
"""

import asyncio
import json


class FakeControlServer:
    def __init__(self, token="t" * 64, server_name="srv-1"):
        self.token = token
        self.server_name = server_name
        self.port = None
        self._server = None
        self.writers = []
        self.received = []          # (cmd, params) of every request
        self.delay = {}             # cmd -> seconds before answering
        self.drop = set()           # cmds that never get an answer
        self.disconnect_on = set()  # cmds that close the connection instead
        self.chunk_size = None      # split outgoing data into chunks of N bytes
        self.coalesce = False       # buffer responses and send them together
        self.events_before_response = []  # events written right before each response
        self.state = {"active": "srv-1", "locked": False,
                      "clients": ["srv-1"], "config": "section: screens\n\tsrv-1:\nend\n"}
        self.seq = 0
        self._pending_out = []
        self.connections = 0

    async def start(self):
        self._server = await asyncio.start_server(self._handle, "127.0.0.1", 0)
        self.port = self._server.sockets[0].getsockname()[1]
        return self

    async def stop(self):
        for w in list(self.writers):
            w.close()
        if self._server:
            self._server.close()
            await self._server.wait_closed()

    async def kick_all(self):
        for w in list(self.writers):
            w.close()
        self.writers.clear()

    async def _write(self, writer, data: bytes):
        if self.chunk_size:
            for i in range(0, len(data), self.chunk_size):
                writer.write(data[i:i + self.chunk_size])
                await writer.drain()
                await asyncio.sleep(0)
        else:
            writer.write(data)
            await writer.drain()

    @staticmethod
    def line(obj):
        return (json.dumps(obj) + "\n").encode()

    def event_line(self, name, data):
        self.seq += 1
        return self.line({"type": "event", "seq": self.seq, "event": name, "data": data})

    async def publish(self, name, data):
        payload = self.event_line(name, data)
        for w in list(self.writers):
            try:
                await self._write(w, payload)
            except ConnectionError:
                pass

    def status(self):
        return {"server": self.server_name, "version": "test", "pid": 4242, "uptimeSeconds": 5,
                "listenAddress": ":24800", "configFile": "/tmp/x.conf", "layoutFile": "/tmp/etherwaver-layout.json",
                "objectLayout": True, "lockedToScreen": self.state["locked"], "clipboardSharing": True,
                "active": {"client": self.state["active"], "screen": self.state["active"], "layoutId": ""},
                "clients": [{"name": c, "primary": c == self.server_name, "active": c == self.state["active"],
                             "shape": {"x": 0, "y": 0, "width": 1920, "height": 1080}} for c in self.state["clients"]],
                "configuredScreens": list(self.state["clients"]), "layout": []}

    async def _handle(self, reader, writer):
        self.connections += 1
        self.writers.append(writer)
        authed = False
        await self._write(writer, self.line({"type": "hello", "protocol": "etherwaver-control", "version": 1,
                                             "server": self.server_name, "serverVersion": "test",
                                             "instance": "fake", "auth": "token"}))
        try:
            while True:
                raw = await reader.readline()
                if not raw:
                    break
                msg = json.loads(raw)
                rid = msg.pop("id", None)
                cmd = msg.pop("cmd", "")
                self.received.append((cmd, msg))

                def ok(result, rid=rid):
                    return self.line({"type": "response", "id": rid, "ok": True, "result": result})

                def err(code, message, rid=rid):
                    return self.line({"type": "response", "id": rid, "ok": False,
                                      "error": {"code": code, "message": message}})

                if cmd == "auth":
                    authed = msg.get("token") == self.token
                    await self._write(writer, ok({"authenticated": True}) if authed else err("unauthorized", "invalid token"))
                    continue
                if not authed:
                    await self._write(writer, err("unauthorized", "authenticate first"))
                    continue
                if cmd == "subscribe":
                    await self._write(writer, ok({"subscribed": True, "seq": self.seq}))
                    continue
                if cmd in self.disconnect_on:
                    writer.close()
                    return
                if cmd in self.drop:
                    continue
                asyncio.ensure_future(self._answer(writer, cmd, msg, ok, err))
        except (ConnectionError, ValueError):
            pass
        finally:
            if writer in self.writers:
                self.writers.remove(writer)

    async def _answer(self, writer, cmd, msg, ok, err):
        if cmd in self.delay:
            await asyncio.sleep(self.delay[cmd])
        if cmd == "status":
            out = ok(self.status())
        elif cmd == "ping":
            out = ok({"pong": True})
        elif cmd == "switch":
            screen = msg.get("screen", "")
            if screen not in self.state["clients"]:
                out = err("not_found", 'unknown screen "%s"' % screen)
            else:
                self.state["active"] = screen
                out = ok({"active": {"client": screen, "screen": screen}})
                await self.publish("screen.active", {"client": screen, "screen": screen})
        elif cmd == "lock":
            self.state["locked"] = {"on": True, "off": False}.get(msg.get("mode"), not self.state["locked"])
            out = ok({"locked": self.state["locked"]})
        elif cmd == "config.apply":
            if "section: screens" not in msg.get("config", ""):
                out = err("invalid_config", "parse error")
            else:
                self.state["config"] = msg["config"]
                out = ok({"applied": True, "persisted": True})
        else:
            out = err("unknown_command", cmd)
        data = b"".join(self.event_line(n, d) for n, d in self.events_before_response) + out
        try:
            if self.coalesce:
                self._pending_out.append(data)
                return
            await self._write(writer, data)
        except ConnectionError:
            pass

    async def flush_coalesced(self):
        data = b"".join(self._pending_out)
        self._pending_out.clear()
        for w in list(self.writers):
            await self._write(w, data)

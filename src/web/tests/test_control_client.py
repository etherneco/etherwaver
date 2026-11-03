import asyncio
import os
import random
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.dirname(__file__))

from waver_web.control_client import (BackendBusy, BackendUnavailable, ControlClient, ControlError,  # noqa: E402
                                      LineFramer, OutcomeUnknown, ProtocolError, RequestTimeout)
from fakes import FakeControlServer  # noqa: E402


class LineFramerTests(unittest.TestCase):
    def test_split_at_every_byte(self):
        data = b'{"a":1}\n{"b":2}\n'
        framer = LineFramer()
        frames = []
        for i in range(len(data)):
            frames += framer.feed(data[i:i + 1])
        self.assertEqual(frames, [b'{"a":1}', b'{"b":2}'])

    def test_random_chunking(self):
        msgs = [('{"n":%d,"pad":"%s"}' % (i, "x" * random.randint(0, 300))).encode() for i in range(200)]
        stream = b"\n".join(msgs) + b"\n"
        framer = LineFramer()
        out = []
        pos = 0
        while pos < len(stream):
            step = random.randint(1, 97)
            out += framer.feed(stream[pos:pos + step])
            pos += step
        self.assertEqual(out, msgs)

    def test_many_messages_in_one_chunk_and_crlf(self):
        framer = LineFramer()
        self.assertEqual(framer.feed(b'{"a":1}\r\n\n{"b":2}\n{"c"'), [b'{"a":1}', b'{"b":2}'])
        self.assertEqual(framer.feed(b':3}\n'), [b'{"c":3}'])

    def test_oversized_frame_is_rejected(self):
        framer = LineFramer(max_line=16)
        with self.assertRaises(ProtocolError):
            framer.feed(b"x" * 40)


class ControlClientTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.server = await FakeControlServer().start()
        self.events = []
        self.states = []
        self.client = ControlClient("127.0.0.1", self.server.port, lambda: self.server.token,
                                    on_event=lambda e, d, s: self.events.append((e, d)),
                                    on_state=lambda s, i: self.states.append(s),
                                    request_timeout=1.0, backoff_initial=0.05, backoff_max=0.2)
        self.client.start()
        self.assertTrue(await self.client.wait_connected(3))

    async def asyncTearDown(self):
        await self.client.stop()
        await self.server.stop()

    async def test_request_response(self):
        status = await self.client.request("status")
        self.assertEqual(status["server"], "srv-1")

    async def test_response_split_into_single_bytes(self):
        self.server.chunk_size = 1
        status = await self.client.request("status")
        self.assertEqual(status["active"]["client"], "srv-1")

    async def test_several_messages_received_together(self):
        self.server.coalesce = True
        tasks = [asyncio.ensure_future(self.client.request("status")) for _ in range(5)]
        tasks.append(asyncio.ensure_future(self.client.request("ping")))
        await asyncio.sleep(0.2)
        await self.server.flush_coalesced()
        results = await asyncio.gather(*tasks)
        self.assertEqual(len(results), 6)
        self.assertEqual(results[-1], {"pong": True})
        self.assertTrue(all(r.get("server") == "srv-1" for r in results[:5]))

    async def test_events_between_responses_are_dispatched_separately(self):
        self.server.events_before_response = [("client.connected", {"name": "a-1"}),
                                              ("client.disconnected", {"name": "b-1"})]
        self.server.chunk_size = 7
        r = await self.client.request("ping")
        self.assertEqual(r, {"pong": True})
        await asyncio.sleep(0.05)
        names = [e for e, _ in self.events]
        self.assertIn("client.connected", names)
        self.assertIn("client.disconnected", names)

    async def test_concurrent_requests_are_correlated(self):
        self.server.state["clients"] = ["srv-1", "a-1", "b-1"]
        self.server.delay = {"status": 0.2}
        ops = [self.client.request("status"), self.client.request("switch", {"screen": "a-1"}),
               self.client.request("ping"), self.client.request("switch", {"screen": "nope"})]
        results = await asyncio.gather(*ops, return_exceptions=True)
        self.assertEqual(results[0]["server"], "srv-1")
        self.assertEqual(results[1]["active"]["client"], "a-1")
        self.assertEqual(results[2], {"pong": True})
        self.assertIsInstance(results[3], ControlError)
        self.assertEqual(results[3].code, "not_found")

    async def test_read_timeout_is_plain_timeout(self):
        self.server.drop = {"status"}
        with self.assertRaises(RequestTimeout):
            await self.client.request("status", timeout=0.3)

    async def test_mutating_timeout_is_uncertain_not_success(self):
        self.server.drop = {"switch"}
        with self.assertRaises(OutcomeUnknown):
            await self.client.request("switch", {"screen": "srv-1"}, timeout=0.3)
        self.assertEqual(self.client.stats["uncertain"], 1)

    async def test_late_response_after_timeout_is_ignored(self):
        self.server.delay = {"status": 0.5}
        with self.assertRaises(RequestTimeout):
            await self.client.request("status", timeout=0.1)
        await asyncio.sleep(0.6)
        self.assertEqual(self.client.stats["late_responses"], 1)
        self.assertEqual((await self.client.request("ping"))["pong"], True)

    async def test_disconnect_during_mutation_reports_unknown_and_is_not_resent(self):
        self.server.disconnect_on = {"lock"}
        with self.assertRaises(OutcomeUnknown):
            await self.client.request("lock", {"mode": "on"})
        self.assertTrue(await self.client.wait_connected(3))
        self.server.disconnect_on = set()
        await asyncio.sleep(0.1)
        sent_locks = [c for c, _ in self.server.received if c == "lock"]
        self.assertEqual(len(sent_locks), 1, "a state-changing command must not be repeated automatically")

    async def test_disconnect_during_read_reports_unavailable(self):
        self.server.disconnect_on = {"status"}
        with self.assertRaises(BackendUnavailable):
            await self.client.request("status")

    async def test_reconnects_after_server_restart(self):
        await self.server.kick_all()
        await asyncio.sleep(0.05)
        self.assertTrue(await self.client.wait_connected(3))
        self.assertGreaterEqual(self.server.connections, 2)
        self.assertIn("disconnected", self.states)
        self.assertEqual((await self.client.request("ping"))["pong"], True)

    async def test_unavailable_when_not_connected(self):
        await self.server.stop()
        await self.server.kick_all()
        await asyncio.sleep(0.1)
        with self.assertRaises(BackendUnavailable):
            await self.client.request("status")

    async def test_bounded_queue(self):
        self.server.delay = {"status": 0.5}
        self.client.max_waiting = 3
        tasks = [asyncio.ensure_future(self.client.request("status", timeout=2)) for _ in range(20)]
        results = await asyncio.gather(*tasks, return_exceptions=True)
        self.assertTrue(any(isinstance(r, BackendBusy) for r in results))
        self.assertTrue(any(isinstance(r, dict) for r in results))

    async def test_cancelled_request_frees_its_slot(self):
        self.server.delay = {"status": 1.0}
        t = asyncio.ensure_future(self.client.request("status", timeout=5))
        await asyncio.sleep(0.05)
        t.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await t
        self.assertEqual(self.client._inflight._value, self.client.max_inflight)


class AuthFailureTests(unittest.IsolatedAsyncioTestCase):
    async def test_wrong_token(self):
        server = await FakeControlServer().start()
        client = ControlClient("127.0.0.1", server.port, lambda: "wrong", backoff_initial=0.05)
        client.start()
        for _ in range(50):
            if client.state == "auth_failed":
                break
            await asyncio.sleep(0.02)
        self.assertEqual(client.state, "auth_failed")
        await client.stop()
        await server.stop()


if __name__ == "__main__":
    unittest.main()

import json
from pathlib import Path
import sys
import threading
import time
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "pc"))
from device_links import DeviceSession, LineDecoder, LocalAPI, encode_frame, proof
import receiver

TOKEN = "test-three-links-token-0123456789abcdef"
SESSION = "0123456789abcdef"


def hello(mode="usb", generation=1):
    return {"v": 1, "type": "hello", "session": SESSION, "link": generation, "mode": mode,
            "proof": proof(TOKEN, "device", SESSION, generation, mode)}


def event(mode="usb", generation=1, sequence=1, action="copy"):
    return {"v": 1, "type": "event", "session": SESSION, "link": generation, "mode": mode,
            "id": f"{SESSION}-{sequence}", "action": action, "created_ms": int(time.time() * 1000)}


class FramingTests(unittest.TestCase):
    def test_fragmentation_at_every_byte_and_boot_logs(self):
        decoder = LineDecoder()
        expected = hello()
        frames = []
        for value in b"[BOOT] ready\n" + encode_frame(expected):
            frames.extend(decoder.feed(bytes([value])))
        self.assertEqual(frames, [expected])

    def test_multiple_frames_and_invalid_utf8_are_recoverable(self):
        decoder = LineDecoder()
        self.assertEqual(decoder.feed(b"\xff\n[]\n" + encode_frame(hello()) + encode_frame(event())),
                         [hello(), event()])

    def test_oversized_line_discards_suffix_until_newline(self):
        decoder = LineDecoder(8)
        self.assertEqual(decoder.feed(b"x" * 9 + b'{}\n{"a":1}\n'), [{"a": 1}])
        self.assertLessEqual(len(decoder.buffer), 8)

    def test_new_frame_resynchronizes_after_partial_disconnection(self):
        decoder = LineDecoder()
        decoder.feed(b'{"truncated":')
        self.assertEqual(decoder.feed(encode_frame(hello())), [hello()])


class LinkAuthenticationTests(unittest.TestCase):
    def setUp(self):
        self.api = Mock()
        self.api.request.return_value = (200, {"ok": True, "epoch": 1800000000,
                                               "stats": {"cpu": 20}, "audio": {"volume": 15}})
        self.session = DeviceSession("usb", TOKEN, self.api)

    def test_verified_hello_returns_status_without_disclosing_token(self):
        message = self.session.handle(hello())
        self.assertEqual(message["type"], "status")
        self.assertEqual(message["stats"]["cpu"], 20)
        self.assertEqual(message["proof"], proof(TOKEN, "pc", SESSION, 1, "usb"))
        self.assertNotIn(TOKEN, json.dumps(message))

    def test_wrong_token_role_and_channel_cannot_authenticate(self):
        for value in ("invalid", "蓝牙" * 32, proof(TOKEN, "pc", SESSION, 1, "usb"),
                      proof(TOKEN, "device", SESSION, 1, "ble")):
            message = hello()
            message["proof"] = value
            self.assertIsNone(self.session.handle(message))
        self.api.request.assert_not_called()

    def test_event_before_handshake_cannot_execute(self):
        self.assertIsNone(self.session.handle(event()))
        self.api.request.assert_not_called()

    def test_status_stops_when_board_switches_away(self):
        self.session.handle(hello())
        self.api.reset_mock()
        with patch("device_links.time.monotonic", return_value=self.session.last_hello + 4):
            self.assertIsNone(self.session.status())
        self.assertIsNone(self.session.session)
        self.api.request.assert_not_called()

    def test_old_connection_and_other_channel_events_are_ignored(self):
        self.session.handle(hello(generation=2))
        self.api.reset_mock()
        self.assertIsNone(self.session.handle(event(generation=1)))
        self.assertIsNone(self.session.handle(event(mode="ble", generation=2)))
        self.api.request.assert_not_called()

    def test_stale_or_missing_timestamp_never_sends_a_delayed_shortcut(self):
        self.session.handle(hello())
        self.api.reset_mock()
        for created in (None, 0, int(time.time() * 1000) - 10000, int(time.time() * 1000) + 10000):
            message = event()
            message["created_ms"] = created
            self.assertEqual(self.session.handle(message)["code"], 408)
        self.api.request.assert_not_called()

    def test_event_id_must_belong_to_current_device_session(self):
        self.session.handle(hello())
        self.api.reset_mock()
        message = event()
        message["id"] = "ffffffffffffffff-1"
        self.assertIsNone(self.session.handle(message))
        self.api.request.assert_not_called()


class SharedExecutorTests(unittest.TestCase):
    def setUp(self):
        self.calls = []
        self.server = receiver.Receiver(("127.0.0.1", 0), TOKEN, self.calls.append)
        self.thread = threading.Thread(target=self.server.serve_forever,
                                       kwargs={"poll_interval": 0.01}, daemon=True)
        self.thread.start()
        self.api = LocalAPI(self.server.server_port, TOKEN)

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)

    def test_usb_and_ble_support_all_actions_without_duplicate_execution(self):
        for mode in ("usb", "ble"):
            session = DeviceSession(mode, TOKEN, self.api)
            self.assertEqual(session.handle(hello(mode))["type"], "status")
            for index, action in enumerate(sorted(receiver.ACTIONS)):
                message = event(mode=mode, sequence=index, action=action)
                self.assertEqual(session.handle(message)["code"], 200)
                self.assertEqual(session.handle(message)["code"], 200)
        # Both transports share the same event-ID cache; crossing channels does
        # not re-execute a previously completed action.
        self.assertEqual(self.calls, sorted(receiver.ACTIONS))

    def test_invalid_actions_are_rejected_by_the_shared_allowlist(self):
        session = DeviceSession("usb", TOKEN, self.api)
        session.handle(hello())
        self.assertEqual(session.handle(event(action="shutdown"))["code"], 400)
        self.assertEqual(self.calls, [])

    def test_dry_run_is_not_a_successful_execution_ack(self):
        self.server.dry_run = True
        session = DeviceSession("usb", TOKEN, self.api)
        session.handle(hello())
        self.assertEqual(session.handle(event())["code"], 202)
        self.assertEqual(self.calls, [])


if __name__ == "__main__":
    unittest.main()

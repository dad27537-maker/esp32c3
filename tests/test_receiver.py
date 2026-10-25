import ctypes
from contextlib import closing
import http.client
import json
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest.mock import Mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "pc"))
sys.path.insert(0, str(ROOT / "tools"))
import receiver
import configure

TOKEN = "0123456789abcdef" * 3
ID = "0123456789abcdef-1"


class ReceiverTests(unittest.TestCase):
    def setUp(self):
        self.calls = []
        self.server = receiver.Receiver(("127.0.0.1", 0), TOKEN, self.calls.append)
        self.thread = threading.Thread(target=self.server.serve_forever,
                                       kwargs={"poll_interval": 0.01}, daemon=True)
        self.thread.start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)

    def request(self, path="/event", payload=None, token=TOKEN, method="POST", raw=None):
        headers = {"Content-Type": "application/json"}
        if token is not None:
            headers["Authorization"] = "Bearer " + token
        body = raw if raw is not None else json.dumps(
            payload if payload is not None else {"id": ID, "action": "copy"})
        with closing(http.client.HTTPConnection("127.0.0.1", self.server.server_port, timeout=2)) as conn:
            conn.request(method, path, body=body, headers=headers)
            reply = conn.getresponse()
            return reply.status, reply.read()

    def test_health_never_sends_keys(self):
        self.assertEqual(self.request("/health", method="GET"), (200, receiver.ACK))
        self.assertEqual(self.calls, [])

    def test_copy_and_paste(self):
        self.assertEqual(self.request()[0], 200)
        self.assertEqual(self.request(payload={"id": ID[:-1] + "2", "action": "paste"})[0], 200)
        self.assertEqual(self.calls, ["copy", "paste"])

    def test_duplicate_does_not_repeat_paste(self):
        payload = {"id": ID, "action": "paste"}
        self.assertEqual(self.request(payload=payload), (200, receiver.ACK))
        self.assertEqual(self.request(payload=payload), (200, receiver.ACK))
        self.assertEqual(self.calls, ["paste"])

    def test_same_id_with_changed_action_is_rejected(self):
        self.request()
        self.assertEqual(self.request(payload={"id": ID, "action": "paste"})[0], 409)
        self.assertEqual(self.calls, ["copy"])

    def test_missing_and_wrong_tokens(self):
        for token in (None, "wrong"):
            self.assertEqual(self.request(token=token)[0], 401)
        self.assertEqual(self.calls, [])

    def test_unsupported_actions_are_rejected(self):
        for action in ("shutdown", "ctrl+x", [], None):
            self.assertEqual(self.request(payload={"id": ID, "action": action})[0], 400)
        self.assertEqual(self.calls, [])

    def test_invalid_json_and_wrong_shapes(self):
        for raw in ("{", "[]", "null", '"copy"', "{}"):
            self.assertEqual(self.request(raw=raw)[0], 400)
        self.assertEqual(self.calls, [])

    def test_invalid_event_ids(self):
        for event_id in (None, 1, "", "0" * 200):
            self.assertEqual(self.request(payload={"id": event_id, "action": "copy"})[0], 400)
        self.assertEqual(self.calls, [])

    def test_oversized_body(self):
        self.assertEqual(self.request(raw=" " * 513)[0], 400)
        self.assertEqual(self.calls, [])

    def test_unknown_path(self):
        self.assertEqual(self.request("/other")[0], 404)
        self.assertEqual(self.calls, [])

    def test_send_failure_is_not_acknowledged_or_replayed(self):
        execute = Mock(side_effect=RuntimeError("simulated Windows input failure"))
        self.server.execute = execute
        self.assertEqual(self.request()[0], 503)
        self.assertEqual(self.request()[0], 503)
        execute.assert_called_once_with("copy")

    def test_dry_run_never_injects_or_returns_success_ack(self):
        self.server.dry_run = True
        self.assertEqual(self.request()[0], 202)
        self.assertEqual(self.calls, [])


class KeyboardTests(unittest.TestCase):
    def test_media_keys_press_and_release_without_ctrl(self):
        for action, vk in (("media_play_pause", 0xB3), ("media_next", 0xB0), ("media_previous", 0xB1)):
            with self.subTest(action=action):
                report = receiver.shortcut_inputs(action)
                self.assertEqual([(i.data.ki.wVk, i.data.ki.dwFlags) for i in report],
                                 [(vk, 0), (vk, 2)])
                keyboard = receiver.WindowsKeyboard.__new__(receiver.WindowsKeyboard)
                keyboard.user32 = Mock()
                keyboard.user32.GetAsyncKeyState.return_value = 0
                keyboard.user32.SendInput.return_value = 2
                keyboard(action)
                self.assertEqual(keyboard.user32.SendInput.call_args.args[0], 2)

    def test_partial_media_injection_releases_only_media_key(self):
        keyboard = receiver.WindowsKeyboard.__new__(receiver.WindowsKeyboard)
        keyboard.user32 = Mock()
        keyboard.user32.GetAsyncKeyState.return_value = 0
        keyboard.user32.SendInput.side_effect = [1, 1]
        with self.assertRaises(RuntimeError):
            keyboard("media_play_pause")
        releases = keyboard.user32.SendInput.call_args.args[1]
        self.assertEqual([(i.data.ki.wVk, i.data.ki.dwFlags) for i in releases], [(0xB3, 2)])

    def test_win32_structure_layout(self):
        self.assertEqual(ctypes.sizeof(receiver.INPUT), 40 if ctypes.sizeof(ctypes.c_void_p) == 8 else 28)

    def test_paste_report_includes_releases(self):
        report = receiver.shortcut_inputs("paste")
        self.assertEqual([(item.data.ki.wVk, item.data.ki.dwFlags) for item in report],
                         [(0x11, 0), (ord("V"), 0), (ord("V"), 2), (0x11, 2)])

    def test_partial_injection_releases_ctrl_and_key(self):
        keyboard = receiver.WindowsKeyboard.__new__(receiver.WindowsKeyboard)
        keyboard.user32 = Mock()
        keyboard.user32.GetAsyncKeyState.return_value = 0
        keyboard.user32.SendInput.side_effect = [2, 2]
        with self.assertRaises(RuntimeError):
            keyboard("paste")
        self.assertEqual(keyboard.user32.SendInput.call_count, 2)
        releases = keyboard.user32.SendInput.call_args.args[1]
        self.assertEqual([(x.data.ki.wVk, x.data.ki.dwFlags) for x in releases],
                         [(ord("V"), 2), (0x11, 2)])

    def test_physical_modifier_blocks_injection(self):
        keyboard = receiver.WindowsKeyboard.__new__(receiver.WindowsKeyboard)
        keyboard.user32 = Mock()
        keyboard.user32.GetAsyncKeyState.return_value = 0x8000
        with self.assertRaises(RuntimeError):
            keyboard("copy")
        keyboard.user32.SendInput.assert_not_called()


class ConfigurationTests(unittest.TestCase):
    def test_usb_only_configuration_needs_no_wifi_credentials(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            configure.write_configuration(root, "", "", "127.0.0.1", 8765, wifi_optional=True)
            config = json.loads((root / "pc" / "receiver_config.json").read_text())
            header = (root / "include" / "secrets.h").read_text()
            self.assertIn(configure.cpp_string(config["token"]), header)
            self.assertIn('#define WIFI_SSID ""', header)

    def test_generated_board_and_pc_share_token(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            configure.write_configuration(root, "中文WiFi", 'a"b\\c', "192.168.1.10", 8765)
            config = json.loads((root / "pc" / "receiver_config.json").read_text())
            header = (root / "include" / "secrets.h").read_text()
            self.assertIn(configure.cpp_string(config["token"]), header)
            self.assertIn(configure.cpp_string("中文WiFi"), header)
            self.assertEqual(len(config["token"]), 48)
            self.assertEqual(config["port"], 8765)

    def test_invalid_host_is_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaises(ValueError):
                configure.write_configuration(Path(folder), "WiFi", "pass", "127.1.bad", 8765)


if __name__ == "__main__":
    unittest.main()

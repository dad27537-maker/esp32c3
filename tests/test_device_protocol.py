from contextlib import closing
import ctypes
import http.client
import json
from pathlib import Path
import sys
import threading
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "pc"))
import receiver
from system_status import SystemMonitor


class DeviceProtocolTests(unittest.TestCase):
    def setUp(self):
        self.calls = []
        self.token = "ui-protocol-test-token-0123456789abcdef"
        self.monitor = SimpleNamespace(snapshot=lambda: {
            "available": True, "cpu": 12.5, "ram": 65, "gpu": 22,
            "cpu_temp": None, "gpu_temp": 47, "disk": 50, "vram": 10, "age_ms": 100})
        self.audio = SimpleNamespace(snapshot=lambda: {"available": True, "volume": 15, "muted": False})
        self.server = receiver.Receiver(("127.0.0.1", 0), self.token, self.calls.append,
                                        status_provider=self.monitor, audio=self.audio)
        self.thread = threading.Thread(target=self.server.serve_forever,
                                       kwargs={"poll_interval": 0.01}, daemon=True)
        self.thread.start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)

    def request(self, method, path, body=None, token=True):
        headers = {"Content-Type": "application/json"}
        if token:
            headers["Authorization"] = "Bearer " + self.token
        with closing(http.client.HTTPConnection("127.0.0.1", self.server.server_port, timeout=2)) as client:
            client.request(method, path, json.dumps(body) if body else None, headers)
            result = client.getresponse()
            return result.status, json.loads(result.read())

    def test_status_reads_real_provider_without_sending_keys(self):
        code, data = self.request("GET", "/status")
        self.assertEqual(code, 200)
        self.assertTrue(data["ok"])
        self.assertGreater(data["epoch"], 1700000000)
        self.assertEqual(data["stats"]["cpu"], 12.5)
        self.assertIsNone(data["stats"]["cpu_temp"])
        self.assertEqual(data["audio"]["volume"], 15)
        self.assertEqual(self.calls, [])

    def test_status_requires_token(self):
        self.assertEqual(self.request("GET", "/status", token=False)[0], 401)

    def test_missing_hardware_and_audio_are_unavailable_not_fabricated(self):
        self.server.status_provider = None
        self.server.audio = None
        _, data = self.request("GET", "/status")
        self.assertFalse(data["stats"]["available"])
        self.assertFalse(data["audio"]["available"])
        self.assertIsNone(data["audio"]["volume"])

    def test_all_volume_actions_are_supported(self):
        for index, action in enumerate(("volume_up", "volume_down", "volume_mute")):
            code, data = self.request("POST", "/event", {"id": f"0123456789abcdef-{index}", "action": action})
            self.assertEqual((code, data), (200, {"ok": True}))
        self.assertEqual(self.calls, ["volume_up", "volume_down", "volume_mute"])

    def test_media_actions_are_supported_and_duplicates_do_not_toggle_twice(self):
        actions = ("media_play_pause", "media_next", "media_previous")
        for index, action in enumerate(actions):
            body = {"id": f"0123456789abcdef-{index}", "action": action}
            for _ in range(2):
                self.assertEqual(self.request("POST", "/event", body), (200, {"ok": True}))
        self.assertEqual(self.calls, list(actions))

    def test_duplicate_volume_event_does_not_adjust_twice(self):
        body = {"id": "0123456789abcdef-1", "action": "volume_up"}
        self.assertEqual(self.request("POST", "/event", body)[0], 200)
        self.assertEqual(self.request("POST", "/event", body)[0], 200)
        self.assertEqual(self.calls, ["volume_up"])

    def test_permission_failure_is_distinct_from_network_failure(self):
        self.server.execute = Mock(side_effect=receiver.InputPermissionError("denied"))
        body = {"id": "0123456789abcdef-1", "action": "copy"}
        self.assertEqual(self.request("POST", "/event", body)[0], 403)
        self.assertEqual(self.request("POST", "/event", body)[0], 403)
        self.server.execute.assert_called_once_with("copy")


class HardwareAndRoutingTests(unittest.TestCase):
    def test_media_actions_use_keyboard_without_changing_volume(self):
        keyboard, audio = Mock(), Mock()
        controller = receiver.DesktopController(keyboard, audio)
        for action in ("media_play_pause", "media_next", "media_previous"):
            controller(action)
        self.assertEqual([call.args[0] for call in keyboard.call_args_list],
                         ["media_play_pause", "media_next", "media_previous"])
        audio.execute.assert_not_called()

    def test_volume_actions_never_send_keyboard_shortcuts(self):
        keyboard, audio = Mock(), Mock()
        controller = receiver.DesktopController(keyboard, audio)
        controller("volume_up")
        keyboard.assert_not_called()
        audio.execute.assert_called_once_with("volume_up")

    def test_copy_and_paste_never_change_volume(self):
        keyboard, audio = Mock(), Mock()
        controller = receiver.DesktopController(keyboard, audio)
        controller("copy")
        controller("paste")
        self.assertEqual([call.args[0] for call in keyboard.call_args_list], ["copy", "paste"])
        audio.execute.assert_not_called()

    def test_cpu_sample_uses_delta_and_kernel_includes_idle(self):
        monitor = SystemMonitor.__new__(SystemMonitor)
        monitor._previous_cpu = None
        readings = iter(((100, 200, 100), (110, 220, 120)))

        def sample(*pointers):
            for pointer, value in zip(pointers, next(readings)):
                pointer._obj.low = value
                pointer._obj.high = 0
            return 1

        monitor.kernel32 = SimpleNamespace(GetSystemTimes=sample)
        self.assertIsNone(monitor._cpu())
        self.assertEqual(monitor._cpu(), 75.0)

    def test_unsupported_gpu_sensor_stays_unknown(self):
        monitor = SystemMonitor.__new__(SystemMonitor)
        monitor.nvidia_smi = "nvidia-smi"
        result = SimpleNamespace(returncode=0, stdout="25, N/A, 512, 8192\n")
        with patch("system_status.subprocess.run", return_value=result):
            data = monitor._nvidia()
        self.assertEqual(data["gpu"], 25)
        self.assertIsNone(data["gpu_temp"])
        self.assertEqual(data["vram"], 6.2)

    def test_invalid_gpu_numbers_do_not_enter_status_json(self):
        monitor = SystemMonitor.__new__(SystemMonitor)
        monitor.nvidia_smi = "nvidia-smi"
        result = SimpleNamespace(returncode=0, stdout="nan, inf, N/A, 8192\n")
        with patch("system_status.subprocess.run", return_value=result):
            data = monitor._nvidia()
        self.assertIsNone(data["gpu"])
        self.assertIsNone(data["gpu_temp"])
        self.assertIsNone(data["vram"])


if __name__ == "__main__":
    unittest.main()

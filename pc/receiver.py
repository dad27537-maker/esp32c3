"""Windows WiFi macropad receiver. Python 3.10+, standard library only."""
from __future__ import annotations

import argparse
from collections import OrderedDict
import ctypes
import hmac
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
import logging
from pathlib import Path
import re
import sys
import time
from typing import Callable

LOG = logging.getLogger("macropad")
ACK = b'{"ok":true}'
EVENT_ID = re.compile(r"[0-9a-f]{16}-[0-9]{1,10}\Z")
MEDIA_KEYS = {"media_play_pause": 0xB3, "media_next": 0xB0, "media_previous": 0xB1}
ACTIONS = frozenset(("copy", "paste", "volume_up", "volume_down", "volume_mute", *MEDIA_KEYS))


class InputPermissionError(RuntimeError):
    pass


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", ctypes.c_uint16), ("wScan", ctypes.c_uint16),
                ("dwFlags", ctypes.c_uint32), ("time", ctypes.c_uint32),
                ("dwExtraInfo", ctypes.c_size_t)]


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", ctypes.c_int32), ("dy", ctypes.c_int32),
                ("mouseData", ctypes.c_uint32), ("dwFlags", ctypes.c_uint32),
                ("time", ctypes.c_uint32), ("dwExtraInfo", ctypes.c_size_t)]


class HARDWAREINPUT(ctypes.Structure):
    _fields_ = [("uMsg", ctypes.c_uint32), ("wParamL", ctypes.c_uint16),
                ("wParamH", ctypes.c_uint16)]


class INPUTUNION(ctypes.Union):
    _fields_ = [("ki", KEYBDINPUT), ("mi", MOUSEINPUT), ("hi", HARDWAREINPUT)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", ctypes.c_uint32), ("data", INPUTUNION)]


def keyboard_input(vk: int, release: bool = False) -> INPUT:
    event = INPUT()
    event.type = 1  # INPUT_KEYBOARD
    event.data.ki.wVk = vk
    event.data.ki.dwFlags = 2 if release else 0  # KEYEVENTF_KEYUP
    return event


def shortcut_inputs(action: str):
    if action in MEDIA_KEYS:
        vk = MEDIA_KEYS[action]
        return (INPUT * 2)(keyboard_input(vk), keyboard_input(vk, True))
    vk = {"copy": ord("C"), "paste": ord("V")}[action]
    return (INPUT * 4)(keyboard_input(0x11), keyboard_input(vk),
                       keyboard_input(vk, True), keyboard_input(0x11, True))


class WindowsKeyboard:
    def __init__(self):
        if sys.platform != "win32":
            raise RuntimeError("Actual keyboard output requires Windows.")
        self.user32 = ctypes.WinDLL("user32", use_last_error=True)
        self.user32.SendInput.argtypes = (ctypes.c_uint, ctypes.POINTER(INPUT), ctypes.c_int)
        self.user32.SendInput.restype = ctypes.c_uint
        self.user32.GetAsyncKeyState.argtypes = (ctypes.c_int,)
        self.user32.GetAsyncKeyState.restype = ctypes.c_short

    def __call__(self, action: str) -> None:
        # Do not turn a held Shift/Alt/Win/Ctrl into an unintended combination.
        if any(self.user32.GetAsyncKeyState(vk) & 0x8000
               for vk in (0x10, 0x11, 0x12, 0x5B, 0x5C)):
            raise RuntimeError("Release physical Ctrl/Shift/Alt/Win keys and press again.")
        inputs = shortcut_inputs(action)
        count = len(inputs)
        sent = self.user32.SendInput(count, inputs, ctypes.sizeof(INPUT))
        if sent != count:
            error = ctypes.get_last_error()
            if sent:
                # Release only keys belonging to this action, in reverse order.
                down = [item.data.ki.wVk for item in inputs if not item.data.ki.dwFlags & 2]
                releases = (INPUT * len(down))(*(keyboard_input(vk, True) for vk in reversed(down)))
                self.user32.SendInput(len(releases), releases, ctypes.sizeof(INPUT))
            if sent == 0 and error == 5:
                raise InputPermissionError(
                    "Windows denied keyboard input (error 5). Start pc/start_receiver.cmd "
                    "from your Windows desktop or an ordinary VS Code terminal. "
                    "An elevated target app requires a receiver with matching privileges."
                )
            raise RuntimeError(f"SendInput inserted {sent}/{count} events; Windows error {error}.")


class DesktopController:
    def __init__(self, keyboard, audio):
        self.keyboard = keyboard
        self.audio = audio

    def __call__(self, action):
        if action in ("copy", "paste") or action in MEDIA_KEYS:
            self.keyboard(action)
        elif action in ("volume_up", "volume_down", "volume_mute"):
            self.audio.execute(action)
        else:
            raise ValueError("Unsupported action")


class Receiver(HTTPServer):
    allow_reuse_address = True

    def __init__(self, address, token: str, execute: Callable[[str], None], *, dry_run=False,
                 status_provider=None, audio=None):
        if len(token) < 32:
            raise ValueError("Token must contain at least 32 characters.")
        self.token = token
        self.execute = execute
        self.dry_run = dry_run
        self.status_provider = status_provider
        self.audio = audio
        # Serial HTTPServer ensures duplicate checks and key injection stay ordered.
        self.completed: OrderedDict[str, tuple[str, int]] = OrderedDict()
        super().__init__(address, Handler)


class Handler(BaseHTTPRequestHandler):
    server: Receiver
    server_version = "Macropad/1"

    def setup(self):
        super().setup()
        self.connection.settimeout(2)

    def log_message(self, _format, *args):
        # Never log Authorization headers, request bodies, or WiFi credentials.
        pass

    def reply(self, code: int, body: bytes = b'{"ok":false}'):
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass  # Action stays cached if the acknowledgement is lost.

    def authorized(self) -> bool:
        value = self.headers.get("Authorization", "").encode("utf-8")
        expected = ("Bearer " + self.server.token).encode("utf-8")
        if not hmac.compare_digest(value, expected):
            # Drain a bounded ordinary body before closing. Otherwise Windows
            # can reset the connection and hide the 401 behind WinError 10053.
            try:
                size = int(self.headers.get("Content-Length", "0"))
                if 0 < size <= 512 and not self.headers.get("Transfer-Encoding"):
                    self.rfile.read(size)
            except (ValueError, OSError):
                pass
            self.reply(401)
            return False
        return True

    def do_GET(self):
        if not self.authorized():
            return
        if self.path == "/health":
            self.reply(200, ACK)
        elif self.path == "/status":
            payload = {"ok": True, "epoch": int(time.time()),
                       "stats": self.server.status_provider.snapshot()
                       if self.server.status_provider else {"available": False},
                       "audio": self.server.audio.snapshot()
                       if self.server.audio else {"available": False, "volume": None, "muted": False}}
            self.reply(200, json.dumps(payload, separators=(",", ":"), allow_nan=False).encode("utf-8"))
        else:
            self.reply(404)

    def do_POST(self):
        if not self.authorized():
            return
        if self.path != "/event":
            self.reply(404)
            return
        if self.headers.get_content_type() != "application/json":
            self.reply(415)
            return
        try:
            size = int(self.headers.get("Content-Length", "0"))
            if not 0 < size <= 512 or self.headers.get("Transfer-Encoding"):
                raise ValueError("Invalid body size")
            payload = json.loads(self.rfile.read(size))
            if not isinstance(payload, dict):
                raise ValueError("Expected an object")
            event_id, action = payload.get("id"), payload.get("action")
            if not isinstance(event_id, str) or not EVENT_ID.fullmatch(event_id):
                raise ValueError("Invalid event id")
            if not isinstance(action, str) or action not in ACTIONS:
                raise ValueError("Invalid action")
        except (ValueError, UnicodeError, OSError):
            self.reply(400)
            return
        cached = self.server.completed.get(event_id)
        if cached is not None:
            old_action, code = cached
            if old_action != action:
                self.reply(409)
            else:
                self.reply(code, ACK if code == 200 else b'{"ok":false}')
            return

        if self.server.dry_run:
            LOG.info("DRY RUN: %s (no keyboard output)", action)
            code = 202  # Never acknowledge a simulated key as really sent.
        else:
            try:
                self.server.execute(action)
                code = 200
                if action in ("copy", "paste"):
                    LOG.info("Sent Ctrl+%s", "C" if action == "copy" else "V")
                elif action in MEDIA_KEYS:
                    LOG.info("Media action: %s", action)
                else:
                    LOG.info("Audio action: %s", action)
            except InputPermissionError as exc:
                code = 403
                LOG.error("Keyboard permission denied (%s): %s", action, exc)
            except Exception as exc:
                code = 503
                LOG.error("Keyboard output failed (%s): %s", action, exc)
        self.server.completed[event_id] = (action, code)
        if len(self.server.completed) > 512:
            self.server.completed.popitem(last=False)
        self.reply(code, ACK if code == 200 else b'{"ok":false}')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path,
                        default=Path(__file__).with_name("receiver_config.json"))
    parser.add_argument("--dry-run", action="store_true", help="Receive/log only; inject no keys")
    parser.add_argument("--links", default="wifi,usb,ble",
                        help="Enabled links: wifi,usb,ble; use wifi alone for serial diagnostics/upload")
    parser.add_argument("--usb-port", help="Force a USB COM port; default: discover CH343 automatically")
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s", datefmt="%H:%M:%S")
    monitor = None
    workers = None
    try:
        config = json.loads(args.config.read_text(encoding="utf-8"))
        audio = None
        if args.dry_run:
            execute = lambda _action: None
        else:
            from system_status import SystemMonitor, WindowsAudio
            audio = WindowsAudio()
            monitor = SystemMonitor()
            execute = DesktopController(WindowsKeyboard(), audio)
        links = set(args.links.split(","))
        if not links or not links <= {"wifi", "usb", "ble"}:
            raise ValueError("--links must contain wifi, usb and/or ble")
        server = Receiver(("0.0.0.0" if "wifi" in links else "127.0.0.1", int(config.get("port", 8765))),
                          config["token"], execute, dry_run=args.dry_run,
                          status_provider=monitor, audio=audio)
    except FileNotFoundError:
        parser.error("Configuration missing. Run: python tools/configure.py")
    except (ValueError, KeyError, OSError, RuntimeError) as exc:
        parser.error(str(exc))
    LOG.info("Listening on port %s. %s", server.server_port,
             "DRY RUN" if args.dry_run else "WiFi keyboard output enabled")
    LOG.info("Keep this program running; focus the application to copy/paste. Ctrl+C here stops it.")
    LOG.info("Hardware/time status: /status; master volume and media control enabled%s",
             " (dry-run: no output)" if args.dry_run else "")
    from device_links import LinkWorkers
    workers = LinkWorkers(config, links, args.usb_port)
    workers.start()
    LOG.info("Enabled connections: %s", ", ".join(sorted(links)))
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        LOG.info("Stopped")
    finally:
        if workers:
            workers.close()
        server.server_close()
        if monitor:
            monitor.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

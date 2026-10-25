"""Check the real board's USB/BLE command and telemetry paths safely.

Uses the running receiver's real hardware/time/audio snapshots. Desktop actions
are intercepted by the test backend: no clipboard, volume or player changes.
Close the receiver's USB/BLE workers first (start it with --links wifi).
"""
from __future__ import annotations

import argparse
from collections import deque
import json
import logging
from pathlib import Path
import re
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "pc"))
import serial
from device_links import DeviceSession, LinkWorkers, LocalAPI, encode_frame
from receiver import ACTIONS


class TestAPI:
    def __init__(self, config):
        self.real = LocalAPI(config["port"], config["token"])
        self.actions = []
        self.ids = set()

    def request(self, path, payload=None):
        if path != "/event":
            return self.real.request(path, payload)
        if payload.get("action") not in ACTIONS:
            return 400, {"ok": False}
        if payload["id"] not in self.ids:
            self.ids.add(payload["id"])
            self.actions.append(payload["action"])
        return 200, {"ok": True}


class TestBLE(LinkWorkers):
    def __init__(self, config, api):
        super().__init__(config, {"ble"})
        self.api = api

    def new_session(self, mode):
        return DeviceSession(mode, self.config["token"], self.api)


class Board:
    def __init__(self, port, config, api):
        self.serial = serial.Serial(port=None, baudrate=115200, timeout=0.1, write_timeout=1)
        self.serial.dtr = False
        self.serial.rts = False
        self.serial.port = port
        self.serial.open()
        self.serial.reset_input_buffer()
        self.session = DeviceSession("usb", config["token"], api)
        self.lines = deque(maxlen=300)
        self.usb = False

    def pump(self):
        raw = self.serial.readline().strip()
        if raw.startswith(b"{"):
            if self.usb:
                try:
                    result = self.session.handle(json.loads(raw))
                    if result:
                        self.serial.write(encode_frame(result))
                except (ValueError, UnicodeError):
                    pass
        elif raw:
            self.lines.append(raw.decode("utf-8", errors="replace"))
        if self.usb and self.session.session and time.monotonic() - self.session.last_status >= 1:
            status = self.session.status()
            if status:
                self.serial.write(encode_frame(status))

    def wait(self, prefix, timeout=5):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            for line in list(self.lines):
                if line.startswith(prefix):
                    self.lines.remove(line)
                    return line
            self.pump()
        raise TimeoutError("Missing board response: " + prefix)

    def command(self, command, prefix):
        self.serial.write((command + "\n").encode("ascii"))
        return self.wait(prefix)

    def state(self):
        return dict(re.findall(r"(\w+)=([^ ]+)", self.command("UI STATE", "UI_STATE")))

    def key(self, name):
        self.command("UI KEY " + name, "KEY ")
        return self.state()

    def mode(self, name):
        self.command("UI PAGE 8", "UI PAGE OK")
        target = {"wifi": 0, "usb": 1, "ble": 2}[name]
        state = self.state()
        while int(state["conn_selected"]) != target:
            state = self.key("DOWN")
        state = self.key("CENTER")
        if state["mode"] != name:
            raise AssertionError("Mode selection failed")

    def online(self, mode, timeout=40):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            state = self.state()
            if state["pc"] == "1" and state["mode"] == mode:
                return state
            self.pump()
        raise TimeoutError(mode + " did not receive authenticated PC telemetry")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM4")
    parser.add_argument("--mode", choices=("usb", "ble"), required=True)
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(message)s")
    config = json.loads((ROOT / "pc" / "receiver_config.json").read_text(encoding="utf-8"))
    api = TestAPI(config)
    board = Board(args.port, config, api)
    original = board.state()["mode"]
    worker = TestBLE(config, api) if args.mode == "ble" else None
    try:
        board.usb = args.mode == "usb"
        if worker:
            worker.start()
        board.mode(args.mode)
        state = board.online(args.mode)
        print("TELEMETRY:", {key: state[key] for key in ("mode", "pc", "cpu", "ram", "gpu", "volume")}, flush=True)
        for page, key, action in ((1, "UP", "copy"), (1, "CENTER", "paste"),
                                  (5, "UP", "volume_up"), (5, "DOWN", "volume_down"), (5, "CENTER", "volume_mute"),
                                  (6, "CENTER", "media_play_pause"), (6, "DOWN", "media_next"), (6, "UP", "media_previous")):
            board.command(f"UI PAGE {page}", "UI PAGE OK")
            board.key(key)
            reply = board.wait("EVENT ")
            if f"action={action} result=0" not in reply:
                raise AssertionError(reply)
            print("ACTION PASS:", action, flush=True)
        if set(api.actions) != ACTIONS:
            raise AssertionError("Missing desktop actions")
        print(args.mode.upper() + "_DEVICE_LINK_TEST: PASS (real telemetry, 8 intercepted actions, acknowledgements)", flush=True)
    finally:
        board.mode(original)
        board.command("UI PAGE 1", "UI PAGE OK")
        board.command("UI PAGE 0", "UI PAGE OK")
        if worker:
            worker.close()
        board.serial.close()


if __name__ == "__main__":
    main()

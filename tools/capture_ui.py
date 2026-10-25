"""Capture the actual device framebuffer as PNG; optionally check UI navigation.

Uses pyserial (already bundled with PlatformIO) and Python's standard library.
Control checks navigate menus/timers/coin only; they never send PC shortcuts/audio.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import struct
import time
import zlib
import serial

ROOT = Path(__file__).resolve().parents[1]


def png(path, width, height, rgb):
    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))
    raw = b"".join(b"\0" + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))
    path.write_bytes(b"\x89PNG\r\n\x1a\n" +
                     chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


class Device:
    def __init__(self, port):
        self.serial = serial.Serial(port=None, baudrate=115200, timeout=0.2, write_timeout=2)
        self.serial.dtr = False
        self.serial.rts = False
        self.serial.port = port
        self.serial.open()
        self.serial.reset_input_buffer()

    def command(self, value, expected, timeout=3):
        self.serial.write((value + "\n").encode("ascii"))
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            line = self.serial.readline().decode("utf-8", errors="replace").strip()
            if line.startswith(expected):
                return line
        raise TimeoutError(f"Device did not reply to {value}")

    def state(self):
        line = self.command("UI STATE", "UI_STATE")
        return dict(re.findall(r"(\w+)=([^ ]+)", line))

    def key(self, value):
        self.command("UI KEY " + value, "KEY ")
        return self.state()

    def frame(self, page):
        if page == 0:
            self.command("UI PAGE 1", "UI PAGE OK")
        self.command(f"UI PAGE {page}", "UI PAGE OK")
        time.sleep(0.1)
        line = self.command("UI FRAME", "FRAME ")
        _, width, height, length = line.split()
        width, height, length = int(width), int(height), int(length)
        if width != 160 or height != 80 or length != width * height * 2:
            raise ValueError("Unexpected framebuffer dimensions")
        raw = bytearray()
        deadline = time.monotonic() + 5
        while len(raw) < length and time.monotonic() < deadline:
            raw.extend(self.serial.read(length - len(raw)))
        if len(raw) != length:
            raise TimeoutError("Incomplete framebuffer")
        if self.serial.readline().strip() or self.serial.readline().strip() != b"END_FRAME":
            raise ValueError("Framebuffer trailer missing")
        rgb = bytearray()
        for position in range(0, length, 2):
            color = (raw[position] << 8) | raw[position + 1]
            rgb.extend((((color >> 11) & 31) * 255 // 31,
                        ((color >> 5) & 63) * 255 // 63, (color & 31) * 255 // 31))
        return bytes(rgb)

    def check_controls(self):
        def require(state, **expected):
            for key, value in expected.items():
                if state.get(key) != str(value):
                    raise AssertionError(f"Expected {key}={value}, actual state {state}")

        self.command("UI PAGE 1", "UI PAGE OK")
        before_coin = self.state()
        next_toss = int(before_coin["tosses"]) + 1
        require(self.key("LEFT"), page=0, selected=0)
        require(self.key("UP"), page=0, selected=7)
        settings = self.key("CENTER")
        require(settings, page=8)
        original_selection = int(settings["conn_selected"])
        require(self.key("DOWN"), page=8, conn_selected=(original_selection + 1) % 3,
                mode=settings["mode"])
        require(self.key("UP"), page=8, conn_selected=original_selection, mode=settings["mode"])
        require(self.key("LEFT"), page=0, selected=7)
        require(self.key("UP"), page=0, selected=6)
        require(self.key("CENTER"), page=7, coin=before_coin["coin"], tosses=before_coin["tosses"])
        tossed = self.key("CENTER")
        require(tossed, page=7, tosses=next_toss, coin_anim=1)
        require(self.key("CENTER"), page=7, tosses=next_toss, coin=tossed["coin"])
        if tossed["coin"] not in ("1", "2"):
            raise AssertionError("Coin result missing")
        for key in ("UP", "DOWN", "RIGHT"):
            require(self.key(key), page=7, tosses=next_toss, coin=tossed["coin"])
        require(self.key("LEFT"), page=0, selected=6)
        require(self.key("CENTER"), page=7, tosses=next_toss, coin=tossed["coin"])
        deadline = time.monotonic() + 3
        settled = self.state()
        while settled["coin_anim"] == "1" and time.monotonic() < deadline:
            time.sleep(0.05)
            settled = self.state()
        require(settled, coin_anim=0, tosses=next_toss, coin=tossed["coin"])
        require(self.key("LEFT"), page=0)
        require(self.key("UP"), page=0, selected=5)
        require(self.key("CENTER"), page=6)
        require(self.key("LEFT"), page=0, selected=5)
        require(self.key("DOWN"), page=0, selected=6)
        require(self.key("DOWN"), page=0, selected=7)
        require(self.key("DOWN"), page=0, selected=0)
        require(self.key("CENTER"), page=1)
        require(self.key("LEFT"), page=0)
        require(self.key("DOWN"), selected=1)
        require(self.key("CENTER"), page=2)
        require(self.key("RIGHT"), running=0, pomo_seconds=1500)
        require(self.key("CENTER"), running=1)
        require(self.key("LEFT"), page=0, running=1)
        time.sleep(1.2)
        ticking = self.state()
        if int(ticking["pomo_seconds"]) >= 1500:
            raise AssertionError("Pomodoro did not continue while in menu")
        require(self.key("CENTER"), page=2)
        paused = self.key("CENTER")
        require(paused, running=0)
        time.sleep(1.1)
        require(self.state(), pomo_seconds=paused["pomo_seconds"])
        require(self.key("RIGHT"), running=0, pomo_seconds=1500)
        require(self.key("LEFT"), page=0)
        timer = self.command("UI TEST TIMER", "TIMER_TEST")
        if "TIMER_TEST PASS" not in timer:
            raise AssertionError(timer)
        print("UI_CONTROL_CHECK: PASS (menu, enter/back, background timer, pause/reset)")
        print(timer)
        media = self.command("UI TEST MEDIA", "MEDIA_TEST")
        if "MEDIA_TEST PASS" not in media:
            raise AssertionError(media)
        print(media)
        coin = self.command("UI TEST COIN", "COIN_TEST")
        if "COIN_TEST PASS" not in coin:
            raise AssertionError(coin)
        print(coin)
        settings = self.command("UI TEST SETTINGS", "SETTINGS_TEST")
        if "SETTINGS_TEST PASS" not in settings:
            raise AssertionError(settings)
        print(settings)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM4")
    parser.add_argument("--check-controls", action="store_true")
    parser.add_argument("--output", type=Path, default=ROOT / "docs" / "previews")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    device = Device(args.port)
    try:
        if args.check_controls:
            device.check_controls()
        names = ("menu", "clipboard", "pomodoro", "hardware", "clock", "volume", "media", "coin", "settings")
        frames = []
        for page, name in enumerate(names):
            frame = device.frame(page)
            frames.append(frame)
            png(args.output / (name + ".png"), 160, 80, frame)
            print("Captured:", name)
        device.command("UI PAGE 1", "UI PAGE OK")
        device.command("UI PAGE 0", "UI PAGE OK")
        print(device.command("UI STATE", "UI_STATE"))
    finally:
        device.serial.close()
    # Actual device frames, displayed with exact integer pixel scaling.
    width, height, scale = 326, ((len(frames) + 1) // 2) * 86 - 6, 3
    sheet = bytearray(bytes((8, 13, 23)) * width * height)
    for index, frame in enumerate(frames):
        x, y = index % 2 * 166, index // 2 * 86
        for row in range(80):
            start = ((y + row) * width + x) * 3
            sheet[start:start + 480] = frame[row * 480:(row + 1) * 480]
    enlarged = bytearray()
    for row in range(height):
        line = bytearray()
        for column in range(width):
            start = (row * width + column) * 3
            line.extend(sheet[start:start + 3] * scale)
        enlarged.extend(line * scale)
    png(args.output / "ui-overview.png", width * scale, height * scale, enlarged)
    print("Overview:", args.output / "ui-overview.png")


if __name__ == "__main__":
    main()

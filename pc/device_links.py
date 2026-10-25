"""USB serial and encrypted BLE links, sharing the existing local HTTP executor.

Only the HTTP server executes desktop actions, so all connections share its
ordering, authentication and event-ID deduplication. No automatic action retry.
"""
from __future__ import annotations

import asyncio
import hashlib
import hmac
import http.client
import json
import logging
import re
import threading
import time

LOG = logging.getLogger("macropad.links")
SERVICE_UUID = "fb7b0001-2c92-4cc5-aaca-5c65d5e8a301"
RX_UUID = "fb7b0002-2c92-4cc5-aaca-5c65d5e8a301"  # PC -> board
TX_UUID = "fb7b0003-2c92-4cc5-aaca-5c65d5e8a301"  # board -> PC
DEVICE_NAME = "ESP32C3-Macropad"
SESSION = re.compile(r"[0-9a-f]{16}\Z")
EVENT = re.compile(r"[0-9a-f]{16}-[0-9]{1,10}\Z")


def proof(token, role, session, generation, mode):
    value = f"macropad-v1|{role}|{session}|{generation}|{mode}"
    return hmac.new(token.encode(), value.encode("ascii"), hashlib.sha256).hexdigest()


def encode_frame(message):
    # Leading newline also resynchronizes after a disconnected partial frame.
    return ("\n" + json.dumps(message, separators=(",", ":"), allow_nan=False) + "\n").encode("ascii")


class LineDecoder:
    def __init__(self, limit=2048):
        self.limit = limit
        self.buffer = bytearray()
        self.discard = False

    def feed(self, data):
        frames = []
        for value in data:
            if value == 10:
                if self.buffer and not self.discard:
                    try:
                        frame = json.loads(self.buffer)
                        if isinstance(frame, dict):
                            frames.append(frame)
                    except (ValueError, UnicodeError):
                        pass  # Serial boot/debug output is not a protocol frame.
                self.buffer.clear()
                self.discard = False
            elif not self.discard:
                if len(self.buffer) < self.limit:
                    self.buffer.append(value)
                else:
                    self.buffer.clear()
                    self.discard = True
        return frames


class LocalAPI:
    def __init__(self, port, token):
        self.port, self.token = port, token

    def request(self, path, payload=None):
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=1)
        try:
            connection.request("POST" if payload is not None else "GET", path,
                               json.dumps(payload) if payload is not None else None,
                               {"Authorization": "Bearer " + self.token,
                                "Content-Type": "application/json"})
            response = connection.getresponse()
            return response.status, json.loads(response.read())
        finally:
            connection.close()


class DeviceSession:
    def __init__(self, mode, token, api):
        self.mode, self.token, self.api = mode, token, api
        self.session = None
        self.generation = None
        self.last_status = 0.0
        self.last_hello = 0.0

    def envelope(self, kind):
        return {"v": 1, "type": kind, "session": self.session, "link": self.generation,
                "mode": self.mode,
                "proof": proof(self.token, "pc", self.session, self.generation, self.mode)}

    def status(self):
        if self.session is None:
            return None
        if time.monotonic() - self.last_hello > 3:
            self.session, self.generation = None, None
            return None  # Board switched away or disconnected; stop pushing.
        code, payload = self.api.request("/status")
        if code != 200 or payload.get("ok") is not True:
            return None
        result = self.envelope("status")
        result.update({key: payload[key] for key in ("epoch", "stats", "audio")})
        result["epoch_ms"] = int(time.time() * 1000)
        self.last_status = time.monotonic()
        return result

    def handle(self, message):
        if message.get("v") != 1 or message.get("mode") != self.mode:
            return None
        if message.get("type") == "hello":
            session, generation = message.get("session"), message.get("link")
            if not isinstance(session, str) or not SESSION.fullmatch(session):
                return None
            if type(generation) is not int or not 0 < generation <= 0xffffffff:
                return None
            supplied = message.get("proof")
            if not isinstance(supplied, str) or not re.fullmatch(r"[0-9a-f]{64}", supplied) or not hmac.compare_digest(
                    supplied, proof(self.token, "device", session, generation, self.mode)):
                return None
            self.session, self.generation = session, generation
            self.last_hello = time.monotonic()
            return self.status()
        if (self.session is None or message.get("session") != self.session or
                message.get("link") != self.generation or message.get("type") != "event"):
            return None
        event_id = message.get("id")
        if (not isinstance(event_id, str) or not EVENT.fullmatch(event_id) or
                not event_id.startswith(self.session + "-")):
            return None
        response = self.envelope("ack")
        response["id"] = event_id
        created = message.get("created_ms")
        age = int(time.time() * 1000) - created if type(created) is int else 999999
        if not -2000 <= age <= 1500:
            response["code"] = 408
            return response
        code, _ = self.api.request("/event", {"id": event_id, "action": message.get("action")})
        response["code"] = code
        return response


class LinkWorkers:
    def __init__(self, config, links, usb_port=None):
        self.config, self.links, self.usb_port = config, links, usb_port
        self.stop = threading.Event()
        self.threads = []

    def start(self):
        for name, target in (("usb", self.usb), ("ble", self.ble)):
            if name in self.links:
                thread = threading.Thread(target=target, name="macropad-" + name, daemon=True)
                self.threads.append(thread)
                thread.start()

    def close(self):
        self.stop.set()
        for thread in self.threads:
            thread.join(timeout=3)

    def new_session(self, mode):
        return DeviceSession(mode, self.config["token"],
                             LocalAPI(int(self.config.get("port", 8765)), self.config["token"]))

    def usb(self):
        try:
            import serial
            from serial.tools import list_ports
        except ImportError:
            LOG.warning("USB needs pyserial: python -m pip install -r pc/requirements.txt")
            return
        last_error = None
        while not self.stop.is_set():
            ports = [self.usb_port] if self.usb_port else [
                p.device for p in list_ports.comports() if (p.vid, p.pid) == (0x1a86, 0x55d3)]
            if not ports:
                self.stop.wait(2)
                continue
            for port in ports:
                if self.stop.is_set():
                    break
                device = serial.Serial(port=None, baudrate=115200, timeout=0.05, write_timeout=1)
                device.dtr = False
                device.rts = False  # Do not reset ESP32 when opening CH343.
                device.port = port
                try:
                    device.open()
                    last_error = None
                    LOG.info("USB listening: %s (close receiver before uploading)", port)
                    session, decoder = self.new_session("usb"), LineDecoder()
                    while not self.stop.is_set():
                        for message in decoder.feed(device.read(min(device.in_waiting or 1, 2048))):
                            was_ready = session.session is not None
                            result = session.handle(message)
                            if not was_ready and session.session is not None:
                                LOG.info("USB ready: %s; desktop controls and hardware telemetry enabled", port)
                            if result is not None:
                                device.write(encode_frame(result))
                        if session.session and time.monotonic() - session.last_status >= 1:
                            status = session.status()
                            if status is not None:
                                device.write(encode_frame(status))
                except (OSError, ValueError) as exc:
                    error = f"USB {port}: {type(exc).__name__}"
                    if error != last_error:
                        LOG.info("%s; retrying", error)
                        last_error = error
                finally:
                    device.close()
            self.stop.wait(2)

    def ble(self):
        try:
            import bleak  # Optional dependency, WiFi remains usable without it.
        except ImportError:
            LOG.warning("Bluetooth needs bleak: python -m pip install -r pc/requirements.txt")
            return
        asyncio.run(self.ble_loop(bleak))

    async def ble_loop(self, bleak):
        last_error = None
        while not self.stop.is_set():
            try:
                device = await bleak.BleakScanner.find_device_by_filter(
                    lambda _device, advertisement: SERVICE_UUID in advertisement.service_uuids,
                    timeout=5)
                if device is None or self.stop.is_set():
                    continue
                async with bleak.BleakClient(device, pair=True, timeout=30) as client:
                    LOG.info("BLE connected: %s", DEVICE_NAME)
                    last_error = None
                    session, decoder = self.new_session("ble"), LineDecoder()
                    messages = asyncio.Queue(maxsize=32)

                    def received(_characteristic, data):
                        for message in decoder.feed(data):
                            try:
                                messages.put_nowait(message)
                            except asyncio.QueueFull:
                                pass

                    async def write(message):
                        data = encode_frame(message)
                        size = max(20, min(180, client.mtu_size - 3))
                        for start in range(0, len(data), size):
                            await client.write_gatt_char(RX_UUID, data[start:start + size], response=True)

                    await client.start_notify(TX_UUID, received)
                    while client.is_connected and not self.stop.is_set():
                        try:
                            message = await asyncio.wait_for(messages.get(), timeout=0.2)
                            was_ready = session.session is not None
                            result = await asyncio.to_thread(session.handle, message)
                            if not was_ready and session.session is not None:
                                LOG.info("BLE ready; desktop controls and hardware telemetry enabled")
                            if result is not None:
                                await write(result)
                        except asyncio.TimeoutError:
                            pass
                        if session.session and time.monotonic() - session.last_status >= 1:
                            result = await asyncio.to_thread(session.status)
                            if result is not None:
                                await write(result)
            except Exception as exc:
                if self.stop.is_set():
                    break
                error = f"BLE: {type(exc).__name__}"
                if error != last_error:
                    LOG.info("%s; check Windows Bluetooth and retry", error)
                    last_error = error
                await asyncio.sleep(2)

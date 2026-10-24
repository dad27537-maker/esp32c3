"""Verify real Windows media-key injection through the running receiver.

A temporary keyboard hook consumes only injected media keys during this check,
so the three test commands do not reach a music player. Other keys are passed
through immediately and never recorded. Requires an interactive Windows desktop.
"""
from __future__ import annotations

import ctypes
from ctypes import wintypes
import http.client
import json
from pathlib import Path
import secrets
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
ACTIONS = (("media_play_pause", 0xB3), ("media_next", 0xB0), ("media_previous", 0xB1))


class KeyboardEvent(ctypes.Structure):
    _fields_ = [("vk", wintypes.DWORD), ("scan", wintypes.DWORD),
                ("flags", wintypes.DWORD), ("time", wintypes.DWORD),
                ("extra", ctypes.c_size_t)]


def main():
    if sys.platform != "win32":
        raise SystemExit("This check requires Windows.")
    config = json.loads((ROOT / "pc" / "receiver_config.json").read_text(encoding="utf-8"))
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    callback_type = ctypes.WINFUNCTYPE(ctypes.c_ssize_t, ctypes.c_int, ctypes.c_size_t, ctypes.c_ssize_t)
    user32.SetWindowsHookExW.argtypes = (ctypes.c_int, callback_type, ctypes.c_void_p, wintypes.DWORD)
    user32.SetWindowsHookExW.restype = ctypes.c_void_p
    user32.CallNextHookEx.argtypes = (ctypes.c_void_p, ctypes.c_int, ctypes.c_size_t, ctypes.c_ssize_t)
    user32.CallNextHookEx.restype = ctypes.c_ssize_t
    user32.UnhookWindowsHookEx.argtypes = (ctypes.c_void_p,)
    user32.PeekMessageW.argtypes = (ctypes.POINTER(wintypes.MSG), ctypes.c_void_p,
                                   wintypes.UINT, wintypes.UINT, wintypes.UINT)
    user32.TranslateMessage.argtypes = (ctypes.POINTER(wintypes.MSG),)
    user32.DispatchMessageW.argtypes = (ctypes.POINTER(wintypes.MSG),)
    user32.DispatchMessageW.restype = ctypes.c_ssize_t
    kernel32.GetModuleHandleW.argtypes = (wintypes.LPCWSTR,)
    kernel32.GetModuleHandleW.restype = ctypes.c_void_p
    received, acknowledgements, errors = [], [], []
    media_keys = {vk for _, vk in ACTIONS}

    @callback_type
    def intercept(code, message, pointer):
        if code >= 0:
            event = ctypes.cast(pointer, ctypes.POINTER(KeyboardEvent)).contents
            if event.vk in media_keys and event.flags & 0x10:  # LLKHF_INJECTED
                received.append((event.vk, bool(event.flags & 0x80)))
                return 1  # Consume both press and release; protect the player.
        return user32.CallNextHookEx(None, code, message, pointer)

    hook = user32.SetWindowsHookExW(13, intercept, kernel32.GetModuleHandleW(None), 0)
    if not hook:
        raise ctypes.WinError(ctypes.get_last_error())

    def send():
        session = secrets.token_hex(8)
        try:
            for index, (action, _) in enumerate(ACTIONS):
                connection = http.client.HTTPConnection("127.0.0.1", config["port"], timeout=2)
                try:
                    connection.request("POST", "/event", json.dumps({
                        "id": f"{session}-{index}", "action": action}), headers={
                            "Authorization": "Bearer " + config["token"],
                            "Content-Type": "application/json"})
                    response = connection.getresponse()
                    acknowledgements.append(response.status)
                    response.read()
                finally:
                    connection.close()
        except OSError as exc:
            errors.append(str(exc))

    worker = threading.Thread(target=send, daemon=True)
    message = wintypes.MSG()
    deadline = time.monotonic() + 8
    try:
        worker.start()
        while worker.is_alive() and time.monotonic() < deadline:
            # Hook callbacks run on this thread; keep pumping while HTTP runs.
            while user32.PeekMessageW(ctypes.byref(message), None, 0, 0, 1):
                user32.TranslateMessage(ctypes.byref(message))
                user32.DispatchMessageW(ctypes.byref(message))
            time.sleep(0.005)
        if worker.is_alive():
            errors.append("HTTP worker timed out")
    finally:
        user32.UnhookWindowsHookEx(hook)
    expected = [(vk, release) for _, vk in ACTIONS for release in (False, True)]
    passed = received == expected and acknowledgements == [200] * 3 and not errors
    print("MEDIA_INPUT_SELF_TEST:", "PASS" if passed else "FAIL")
    print("HTTP acknowledgements:", acknowledgements)
    print("Injected media press/release sequence:", received)
    print("Media events consumed; no player commands delivered.")
    if errors:
        print("Details:", errors)
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())

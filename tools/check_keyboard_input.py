"""Verify the receiver's real Ctrl+C input in a temporary Windows test window.

The test catches Ctrl+C in its own window, so it does not copy, paste, read,
or replace the user's clipboard. No keys are sent unless this process owns
the foreground window. Run from a normal interactive Windows terminal.
"""
from __future__ import annotations

import ctypes
import http.client
import json
from pathlib import Path
import secrets
import sys
import tkinter as tk

ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    if sys.platform != "win32":
        raise SystemExit("This input check requires an interactive Windows desktop.")
    config = json.loads((ROOT / "pc" / "receiver_config.json").read_text(encoding="utf-8"))
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    user32.GetForegroundWindow.restype = ctypes.c_void_p
    user32.GetWindowThreadProcessId.argtypes = (ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32))
    user32.SetForegroundWindow.argtypes = (ctypes.c_void_p,)
    user32.GetAncestor.argtypes = (ctypes.c_void_p, ctypes.c_uint32)
    user32.GetAncestor.restype = ctypes.c_void_p
    user32.BringWindowToTop.argtypes = (ctypes.c_void_p,)
    user32.AttachThreadInput.argtypes = (ctypes.c_uint32, ctypes.c_uint32, ctypes.c_int)
    user32.AttachThreadInput.restype = ctypes.c_int
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.GetCurrentThreadId.restype = ctypes.c_uint32
    previous_window = user32.GetForegroundWindow()

    root = tk.Tk()
    root.title("ESP32-C3 Ctrl+C input check")
    root.geometry("440x130")
    status = tk.Label(root, text="Checking Ctrl+C in this test window only...", pady=20)
    status.pack()
    result = {"received": False, "http": None, "error": None}

    def copied(_event):
        result["received"] = True
        status.configure(text="Ctrl+C received successfully", fg="green")
        return "break"  # Consume input; never invoke the clipboard operation.

    root.bind_all("<Control-KeyPress-c>", copied)
    root.bind_all("<Control-KeyPress-C>", copied)

    def finish():
        success = result["received"] and result["http"] == 200
        print("CTRL_C_SELF_TEST:", "PASS" if success else "FAIL", flush=True)
        print("HTTP acknowledgement:", result["http"], flush=True)
        print("Ctrl+C received by test window:", result["received"], flush=True)
        if result["error"]:
            print("Details:", result["error"], flush=True)
        root.destroy()
        if previous_window:
            user32.SetForegroundWindow(previous_window)

    def send():
        import os
        owner = ctypes.c_uint32()
        user32.GetWindowThreadProcessId(user32.GetForegroundWindow(), ctypes.byref(owner))
        if owner.value != os.getpid():
            result["error"] = "Test window did not get focus; no keystrokes were sent."
            finish()
            return
        connection = http.client.HTTPConnection("127.0.0.1", config["port"], timeout=3)
        try:
            payload = json.dumps({"id": secrets.token_hex(8) + "-1", "action": "copy"})
            connection.request("POST", "/event", body=payload, headers={
                "Authorization": "Bearer " + config["token"],
                "Content-Type": "application/json",
            })
            response = connection.getresponse()
            result["http"] = response.status
            response.read()
        except OSError as exc:
            result["error"] = str(exc)
        finally:
            connection.close()
        root.after(500, finish)

    def focus():
        root.lift()
        root.update_idletasks()
        # Tk's inner HWND is not always its actual Windows top-level window.
        target = user32.GetAncestor(root.winfo_id(), 2)
        foreground_thread = user32.GetWindowThreadProcessId(user32.GetForegroundWindow(), None)
        own_thread = kernel32.GetCurrentThreadId()
        attached = False
        try:
            if foreground_thread and foreground_thread != own_thread:
                attached = bool(user32.AttachThreadInput(own_thread, foreground_thread, True))
            user32.BringWindowToTop(target)
            user32.SetForegroundWindow(target)
            root.focus_force()
        finally:
            if attached:
                user32.AttachThreadInput(own_thread, foreground_thread, False)
        root.after(250, send)

    root.after(250, focus)
    root.mainloop()
    return 0 if result["received"] and result["http"] == 200 else 1


if __name__ == "__main__":
    raise SystemExit(main())

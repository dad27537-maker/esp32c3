"""Windows hardware sampling and master audio control, using the standard library.

GPU readings use NVIDIA's installed nvidia-smi when available. An optional
LibreHardwareMonitor local web server supplies CPU temperatures/other GPUs.
Unsupported sensors are None, never invented values.
"""
from __future__ import annotations

from contextlib import contextmanager
import ctypes
import http.client
import json
import math
import os
import re
import shutil
import subprocess
import threading
import time
import uuid


class GUID(ctypes.Structure):
    _fields_ = [("data1", ctypes.c_uint32), ("data2", ctypes.c_uint16),
                ("data3", ctypes.c_uint16), ("data4", ctypes.c_ubyte * 8)]

    @classmethod
    def parse(cls, value: str):
        return cls.from_buffer_copy(uuid.UUID(value).bytes_le)


def _check(result: int):
    if result < 0:
        raise OSError(f"Windows audio HRESULT 0x{result & 0xffffffff:08x}")


def _method(pointer, index, result_type, *arguments):
    table = ctypes.cast(pointer, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
    return ctypes.WINFUNCTYPE(result_type, ctypes.c_void_p, *arguments)(table[index])


class WindowsAudio:
    """Resolve the current default playback device each time (supports device changes)."""
    def __init__(self):
        self.ole32 = ctypes.WinDLL("ole32", use_last_error=True)
        self.ole32.CoInitializeEx.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
        self.ole32.CoInitializeEx.restype = ctypes.c_int32
        self.ole32.CoCreateInstance.argtypes = [ctypes.POINTER(GUID), ctypes.c_void_p,
                                               ctypes.c_uint32, ctypes.POINTER(GUID),
                                               ctypes.POINTER(ctypes.c_void_p)]
        self.ole32.CoCreateInstance.restype = ctypes.c_int32
        self.ole32.CoUninitialize.argtypes = []

    @contextmanager
    def _endpoint(self):
        initialized = self.ole32.CoInitializeEx(None, 0)
        if initialized not in (0, 1, -2147417850):  # RPC_E_CHANGED_MODE: use existing apartment.
            _check(initialized)
        enumerator, device, endpoint = (ctypes.c_void_p() for _ in range(3))
        try:
            clsid = GUID.parse("bcde0395-e52f-467c-8e3d-c4579291692e")
            iid = GUID.parse("a95664d2-9614-4f35-a746-de8db63617e6")
            endpoint_iid = GUID.parse("5cdf2c82-841e-4546-9722-0cf74078229a")
            _check(self.ole32.CoCreateInstance(ctypes.byref(clsid), None, 0x17,
                                              ctypes.byref(iid), ctypes.byref(enumerator)))
            _check(_method(enumerator, 4, ctypes.c_int32, ctypes.c_int, ctypes.c_int,
                           ctypes.POINTER(ctypes.c_void_p))(enumerator, 0, 1, ctypes.byref(device)))
            _check(_method(device, 3, ctypes.c_int32, ctypes.POINTER(GUID), ctypes.c_uint32,
                           ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p))(
                               device, ctypes.byref(endpoint_iid), 0x17, None, ctypes.byref(endpoint)))
            yield endpoint
        finally:
            for pointer in (endpoint, device, enumerator):
                if pointer:
                    _method(pointer, 2, ctypes.c_uint32)(pointer)
            if initialized in (0, 1):
                self.ole32.CoUninitialize()

    @staticmethod
    def _read(endpoint):
        scalar = ctypes.c_float()
        muted = ctypes.c_int()
        _check(_method(endpoint, 9, ctypes.c_int32, ctypes.POINTER(ctypes.c_float))(
            endpoint, ctypes.byref(scalar)))
        _check(_method(endpoint, 15, ctypes.c_int32, ctypes.POINTER(ctypes.c_int))(
            endpoint, ctypes.byref(muted)))
        return scalar.value, bool(muted.value)

    def snapshot(self):
        try:
            with self._endpoint() as endpoint:
                scalar, muted = self._read(endpoint)
            return {"available": True, "volume": round(scalar * 100), "muted": muted}
        except OSError:
            return {"available": False, "volume": None, "muted": False}

    def execute(self, action: str):
        with self._endpoint() as endpoint:
            scalar, muted = self._read(endpoint)
            set_mute = _method(endpoint, 14, ctypes.c_int32, ctypes.c_int, ctypes.c_void_p)
            if action == "volume_mute":
                _check(set_mute(endpoint, not muted, None))
            elif action in ("volume_up", "volume_down"):
                scalar = max(0.0, min(1.0, scalar + (0.02 if action == "volume_up" else -0.02)))
                _check(_method(endpoint, 7, ctypes.c_int32, ctypes.c_float, ctypes.c_void_p)(
                    endpoint, scalar, None))
                _check(set_mute(endpoint, False, None))
            else:
                raise ValueError("Unsupported audio action")


class FILETIME(ctypes.Structure):
    _fields_ = [("low", ctypes.c_uint32), ("high", ctypes.c_uint32)]

    def number(self):
        return (self.high << 32) | self.low


class MEMORYSTATUSEX(ctypes.Structure):
    _fields_ = [("length", ctypes.c_uint32), ("load", ctypes.c_uint32),
                ("total_phys", ctypes.c_uint64), ("avail_phys", ctypes.c_uint64),
                ("total_page", ctypes.c_uint64), ("avail_page", ctypes.c_uint64),
                ("total_virtual", ctypes.c_uint64), ("avail_virtual", ctypes.c_uint64),
                ("avail_extended_virtual", ctypes.c_uint64)]


def _number(value):
    try:
        number = float(value)
        return number if math.isfinite(number) else None
    except (ValueError, TypeError):
        return None


class SystemMonitor:
    def __init__(self):
        self.kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        self.kernel32.GetSystemTimes.argtypes = [ctypes.POINTER(FILETIME)] * 3
        self.kernel32.GetSystemTimes.restype = ctypes.c_int
        self.kernel32.GlobalMemoryStatusEx.argtypes = [ctypes.POINTER(MEMORYSTATUSEX)]
        self.kernel32.GlobalMemoryStatusEx.restype = ctypes.c_int
        self._previous_cpu = None
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._data = {"available": False}
        self._sampled = time.monotonic()
        self.nvidia_smi = shutil.which("nvidia-smi")
        self._collect()
        self._thread = threading.Thread(target=self._run, name="hardware-sampler", daemon=True)
        self._thread.start()

    def _cpu(self):
        idle, kernel, user = FILETIME(), FILETIME(), FILETIME()
        if not self.kernel32.GetSystemTimes(ctypes.byref(idle), ctypes.byref(kernel), ctypes.byref(user)):
            return None
        values = idle.number(), kernel.number(), user.number()
        previous, self._previous_cpu = self._previous_cpu, values
        if previous is None:
            return None
        idle_delta = values[0] - previous[0]
        total_delta = values[1] - previous[1] + values[2] - previous[2]
        if total_delta <= 0:
            return None
        return round(max(0.0, min(100.0, 100 * (total_delta - idle_delta) / total_delta)), 1)

    def _nvidia(self):
        if not self.nvidia_smi:
            return {}
        try:
            result = subprocess.run(
                [self.nvidia_smi,
                 "--query-gpu=utilization.gpu,temperature.gpu,memory.used,memory.total",
                 "--format=csv,noheader,nounits"],
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
                timeout=1.5, creationflags=0x08000000, check=False)
            if result.returncode or not result.stdout.strip():
                return {}
            values = [_number(x.strip()) for x in result.stdout.splitlines()[0].split(",")]
            if len(values) != 4:
                return {}
            gpu, temperature, used, total = values
            return {"gpu": gpu, "gpu_temp": temperature,
                    "vram": round(used / total * 100, 1) if used is not None and total else None}
        except (OSError, subprocess.TimeoutExpired):
            return {}

    @staticmethod
    def _optional_sensors():
        # LibreHardwareMonitor: Options -> Remote Web Server -> Run (port 8085).
        connection = http.client.HTTPConnection("127.0.0.1", 8085, timeout=0.15)
        try:
            connection.request("GET", "/data.json")
            response = connection.getresponse()
            if response.status != 200:
                return {}
            tree = json.loads(response.read(262144))
            cpu_temperatures, gpu_temperatures, gpu_loads = [], [], []

            def visit(node, cpu=False, gpu=False):
                if not isinstance(node, dict):
                    return
                text = str(node.get("Text", "")).lower()
                image = str(node.get("ImageURL", "")).lower()
                cpu = cpu or "cpu" in image or text.startswith("cpu")
                gpu = gpu or "gpu" in image or text.startswith("gpu")
                value = str(node.get("Value", ""))
                match = re.search(r"-?\d+(?:[.,]\d+)?", value)
                if match:
                    number = float(match.group().replace(",", "."))
                    if "°c" in value.lower():
                        if cpu:
                            cpu_temperatures.append(number)
                        if gpu:
                            gpu_temperatures.append(number)
                    elif gpu and "%" in value and "core" in text:
                        gpu_loads.append(number)
                for child in node.get("Children", []):
                    visit(child, cpu, gpu)

            visit(tree)
            result = {}
            if cpu_temperatures:
                result["cpu_temp"] = max(cpu_temperatures)
            if gpu_temperatures:
                result["gpu_temp"] = max(gpu_temperatures)
            if gpu_loads:
                result["gpu"] = max(gpu_loads)
            return result
        except (OSError, ValueError, http.client.HTTPException):
            return {}
        finally:
            connection.close()

    def _collect(self):
        memory = MEMORYSTATUSEX()
        memory.length = ctypes.sizeof(memory)
        memory_ok = bool(self.kernel32.GlobalMemoryStatusEx(ctypes.byref(memory)))
        data = {"available": memory_ok, "cpu": self._cpu(),
                "ram": memory.load if memory_ok else None, "disk": None,
                "gpu": None, "cpu_temp": None, "gpu_temp": None, "vram": None}
        try:
            disk = shutil.disk_usage(os.environ.get("SystemDrive", "C:") + "\\")
            data["disk"] = round(disk.used / disk.total * 100, 1)
        except OSError:
            pass
        data.update(self._optional_sensors())
        data.update(self._nvidia())
        with self._lock:
            self._data = data
            self._sampled = time.monotonic()

    def _run(self):
        while not self._stop.wait(1):
            try:
                self._collect()
            except OSError:
                # Keep the last sample with its age; never present a failed read as fresh.
                pass

    def snapshot(self):
        with self._lock:
            return {**self._data, "age_ms": round((time.monotonic() - self._sampled) * 1000)}

    def close(self):
        self._stop.set()
        self._thread.join(timeout=2)

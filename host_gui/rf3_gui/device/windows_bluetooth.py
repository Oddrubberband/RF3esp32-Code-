"""Windows Classic Bluetooth discovery/pairing and address-bound SPP selection.

Protocol basis: Bluetooth SIG Classic GAP/SSP and Serial Port Profile (SPP),
using Microsoft's BluetoothFindFirstDevice / BluetoothAuthenticateDeviceEx /
BluetoothSetServiceState APIs. This is not a BLE/GATT transport. Native calls
are lazy and run only in response to an explicit application action.
"""
from __future__ import annotations

import ctypes as ct
import os
import re
import threading
import time
import uuid
from contextlib import contextmanager
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone


SPP_SERVICE_UUID = "00001101-0000-1000-8000-00805f9b34fb"
ERROR_NO_MORE_ITEMS = 259
ERROR_CANCELLED = 1223
E_INVALIDARG = 0x80070057


class BluetoothError(RuntimeError):
    pass


class BluetoothCancelled(BluetoothError):
    pass


def compact_address(address: str) -> str:
    value = address.replace(":", "").replace("-", "").upper()
    if not re.fullmatch(r"[0-9A-F]{12}", value) or value == "000000000000":
        raise BluetoothError("Invalid Bluetooth address")
    return value


def format_address(address: int) -> str:
    value = f"{address:012X}"
    return ":".join(value[index:index + 2] for index in range(0, 12, 2))


@dataclass(frozen=True)
class BluetoothPeer:
    name: str
    address: str
    paired: bool
    seen_in_scan: bool


class _SystemTime(ct.Structure):
    _fields_ = [(name, ct.c_uint16) for name in (
        "year", "month", "day_of_week", "day", "hour", "minute", "second", "milliseconds"
    )]

    def utc(self) -> datetime | None:
        try:
            return datetime(self.year, self.month, self.day, self.hour, self.minute,
                            self.second, self.milliseconds * 1000, tzinfo=timezone.utc)
        except ValueError:
            return None


class _RadioParams(ct.Structure):
    _fields_ = [("size", ct.c_uint32)]


class _SearchParams(ct.Structure):
    _fields_ = [("size", ct.c_uint32), ("authenticated", ct.c_int32),
                ("remembered", ct.c_int32), ("unknown", ct.c_int32),
                ("connected", ct.c_int32), ("inquiry", ct.c_int32),
                ("timeout", ct.c_ubyte), ("radio", ct.c_void_p)]


class _DeviceInfo(ct.Structure):
    _fields_ = [("size", ct.c_uint32), ("address", ct.c_uint64),
                ("device_class", ct.c_uint32), ("connected", ct.c_int32),
                ("remembered", ct.c_int32), ("authenticated", ct.c_int32),
                ("last_seen", _SystemTime), ("last_used", _SystemTime),
                ("name", ct.c_wchar * 248)]


class _Guid(ct.Structure):
    _fields_ = [("data1", ct.c_uint32), ("data2", ct.c_uint16),
                ("data3", ct.c_uint16), ("data4", ct.c_ubyte * 8)]


class _NativeApi:
    def __init__(self):
        if os.name != "nt":
            raise BluetoothError("In-app Classic Bluetooth discovery requires Windows")
        self.bt = ct.WinDLL("bthprops.cpl", use_last_error=True)
        self.kernel = ct.WinDLL("kernel32", use_last_error=True)
        signatures = {
            "BluetoothFindFirstRadio": ([ct.POINTER(_RadioParams), ct.POINTER(ct.c_void_p)], ct.c_void_p),
            "BluetoothFindRadioClose": ([ct.c_void_p], ct.c_int32),
            "BluetoothFindFirstDevice": ([ct.POINTER(_SearchParams), ct.POINTER(_DeviceInfo)], ct.c_void_p),
            "BluetoothFindNextDevice": ([ct.c_void_p, ct.POINTER(_DeviceInfo)], ct.c_int32),
            "BluetoothFindDeviceClose": ([ct.c_void_p], ct.c_int32),
            "BluetoothGetDeviceInfo": ([ct.c_void_p, ct.POINTER(_DeviceInfo)], ct.c_uint32),
            "BluetoothAuthenticateDeviceEx": ([ct.c_void_p, ct.c_void_p, ct.POINTER(_DeviceInfo), ct.c_void_p, ct.c_int], ct.c_uint32),
            "BluetoothSetServiceState": ([ct.c_void_p, ct.POINTER(_DeviceInfo), ct.POINTER(_Guid), ct.c_uint32], ct.c_uint32),
        }
        for name, (arguments, result) in signatures.items():
            function = getattr(self.bt, name)
            function.argtypes, function.restype = arguments, result
        self.kernel.CloseHandle.argtypes = [ct.c_void_p]
        self.kernel.CloseHandle.restype = ct.c_int32

    @contextmanager
    def radio(self):
        radio = ct.c_void_p()
        parameters = _RadioParams(ct.sizeof(_RadioParams))
        search = self.bt.BluetoothFindFirstRadio(ct.byref(parameters), ct.byref(radio))
        if not search:
            raise BluetoothError("No enabled Windows Bluetooth radio is available; check the adapter")
        self.bt.BluetoothFindRadioClose(search)
        try:
            yield radio
        finally:
            self.kernel.CloseHandle(radio)

    def inquire(self, radio):
        # 8 * 1.28 seconds; include cached records, but never label them live
        # without a LastSeen timestamp from this actual inquiry (GAP basis).
        parameters = _SearchParams(ct.sizeof(_SearchParams), 1, 1, 1, 1, 1, 8, radio.value)
        device = _DeviceInfo()
        device.size = ct.sizeof(_DeviceInfo)
        search = self.bt.BluetoothFindFirstDevice(ct.byref(parameters), ct.byref(device))
        if not search:
            error = ct.get_last_error()
            if error == ERROR_NO_MORE_ITEMS:
                return
            raise BluetoothError(f"Bluetooth inquiry failed (Windows error {error})")
        try:
            while True:
                yield _DeviceInfo.from_buffer_copy(device)
                device.size = ct.sizeof(_DeviceInfo)
                if not self.bt.BluetoothFindNextDevice(search, ct.byref(device)):
                    error = ct.get_last_error()
                    if error != ERROR_NO_MORE_ITEMS:
                        raise BluetoothError(f"Bluetooth enumeration failed (Windows error {error})")
                    break
        finally:
            self.bt.BluetoothFindDeviceClose(search)

    def info(self, radio, address):
        device = _DeviceInfo()
        device.size = ct.sizeof(_DeviceInfo)
        device.address = int(compact_address(address), 16)
        error = self.bt.BluetoothGetDeviceInfo(radio, ct.byref(device))
        if error:
            raise BluetoothError(f"Device information unavailable (Windows error {error}); scan again")
        return device

    def pair(self, radio, device, parent_window):
        # General bonding without MITM matches ESP SSP IO_CAP_NONE. Windows
        # owns user consent/PIN UI; do not silently accept authentication.
        return self.bt.BluetoothAuthenticateDeviceEx(parent_window or None, radio,
                                                    ct.byref(device), None, 4)

    def enable_spp(self, radio, device):
        service = _Guid.from_buffer_copy(uuid.UUID(SPP_SERVICE_UUID).bytes_le)
        return self.bt.BluetoothSetServiceState(radio, ct.byref(device), ct.byref(service), 1)


def _serial_ports():
    from serial.tools import list_ports
    return list(list_ports.comports())


def _address_in_instance(address: str, instance: str) -> bool:
    return re.search(r"(?<![0-9A-F])" + compact_address(address) + r"(?![0-9A-F])",
                     instance.upper()) is not None


def _registry_spp_ports(address: str) -> list[str]:
    """Read only the selected remote device's SPP driver mapping; no registry writes."""
    import winreg
    result = []
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                            r"SYSTEM\CurrentControlSet\Enum\BTHENUM") as root:
            for index in range(winreg.QueryInfoKey(root)[0]):
                service_name = winreg.EnumKey(root, index)
                if SPP_SERVICE_UUID.upper() not in service_name.upper():
                    continue
                with winreg.OpenKey(root, service_name) as service:
                    for child in range(winreg.QueryInfoKey(service)[0]):
                        instance = winreg.EnumKey(service, child)
                        if not _address_in_instance(address, instance):
                            continue
                        try:
                            with winreg.OpenKey(service, instance + r"\Device Parameters") as parameters:
                                port, _ = winreg.QueryValueEx(parameters, "PortName")
                                if re.fullmatch(r"COM[0-9]+", str(port), re.IGNORECASE):
                                    result.append(str(port))
                        except OSError:
                            continue
    except OSError:
        pass
    return result


def matching_spp_ports(address: str, ports, registry_ports=()) -> list[str]:
    matches = set()
    mapped = {str(port).upper() for port in registry_ports}
    for port in ports:
        hardware = str(getattr(port, "hwid", ""))
        label = str(getattr(port, "description", ""))
        combined = (hardware + " " + label).upper()
        if "INCOMING" in combined or "000000000000" in combined:
            continue
        if ("BTHENUM" in combined and _address_in_instance(address, hardware)) or (
            str(port.device).upper() in mapped and ("BLUETOOTH" in combined or "BTHENUM" in combined)
        ):
            matches.add(str(port.device))
    return sorted(matches)


class WindowsBluetooth:
    def __init__(self, *, api_factory=None, port_provider=None, registry_lookup=None,
                 port_timeout=30.0):
        self._api_factory = api_factory or _NativeApi
        self._port_provider = port_provider or _serial_ports
        self._registry_lookup = registry_lookup or _registry_spp_ports
        self._port_timeout = port_timeout
        self._lock = threading.Lock()

    @staticmethod
    def _check_cancel(cancel):
        if cancel.is_set():
            raise BluetoothCancelled("Bluetooth operation cancelled")

    @contextmanager
    def _operation(self, cancel):
        while not self._lock.acquire(timeout=0.1):
            self._check_cancel(cancel)
        try:
            self._check_cancel(cancel)
            yield self._api_factory()
        finally:
            self._lock.release()

    def scan(self, cancel: threading.Event) -> list[BluetoothPeer]:
        with self._operation(cancel) as api, api.radio() as radio:
            started = datetime.now(timezone.utc)
            records = list(api.inquire(radio))
            finished = datetime.now(timezone.utc)
            self._check_cancel(cancel)
            peers = {}
            for device in records:
                if not device.address:
                    continue
                seen = device.last_seen.utc()
                fresh = seen is not None and started - timedelta(seconds=2) <= seen <= finished + timedelta(seconds=2)
                address = format_address(device.address)
                peers[address] = BluetoothPeer(device.name or "Unnamed Classic Bluetooth device",
                                               address, bool(device.authenticated), fresh)
            return sorted(peers.values(), key=lambda peer: (
                not peer.seen_in_scan, not peer.name.upper().startswith(("RF3", "ESP")), peer.name.casefold()
            ))

    def prepare_serial(self, address: str, cancel: threading.Event, *, parent_window=0,
                       progress=lambda message: None) -> str:
        compact_address(address)
        with self._operation(cancel) as api, api.radio() as radio:
            device = api.info(radio, address)
            if not device.authenticated:
                self._check_cancel(cancel)
                progress("Pairing selected device; complete the Windows confirmation/PIN dialog")
                error = api.pair(radio, device, parent_window)
                self._check_cancel(cancel)
                if error == ERROR_CANCELLED:
                    raise BluetoothCancelled("Windows pairing was cancelled")
                if error not in (0, ERROR_NO_MORE_ITEMS):
                    raise BluetoothError(f"Windows pairing failed (error {error}); the ESP must be powered and discoverable")
                device = api.info(radio, address)
                if not device.authenticated:
                    raise BluetoothError("Windows did not confirm pairing; no serial connection was opened")
            self._check_cancel(cancel)
            progress("Enabling the selected device's Serial Port Profile service")
            error = api.enable_spp(radio, device)
            if error not in (0, E_INVALIDARG):
                raise BluetoothError(f"Serial Port Profile unavailable (Windows error {error}). Use Classic SPP firmware; BLE-only ESP chips need a BLE transport")
            deadline = time.monotonic() + self._port_timeout
            progress("Waiting for the selected device's outgoing Bluetooth COM port")
            while True:
                self._check_cancel(cancel)
                matches = matching_spp_ports(address, self._port_provider(), self._registry_lookup(address))
                if len(matches) == 1:
                    return matches[0]
                if len(matches) > 1:
                    raise BluetoothError("Multiple outgoing SPP ports match this device; choose its COM port using Refresh Ports")
                if time.monotonic() >= deadline:
                    raise BluetoothError("Paired, but no outgoing SPP COM port appeared for this device. Check SPP firmware/service and retry; no other device's port was selected")
                cancel.wait(0.25)

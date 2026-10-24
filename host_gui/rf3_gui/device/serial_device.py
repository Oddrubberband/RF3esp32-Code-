from __future__ import annotations

import binascii
import hashlib
import io
import json
import logging
import os
import queue
import re
import threading
import tempfile
import time
from collections import deque
from dataclasses import dataclass
from pathlib import Path
from typing import Any, BinaryIO

from PySide6.QtCore import QTimer, Signal

from rf3_gui.app.state_model import (
    ConnectionState,
    DEFAULT_CHANNEL,
    DeviceInfo,
    DeviceSnapshot,
    PROTOCOL_V2_MAX_FILE_SIZE,
    RadioCapabilities,
    RadioState,
    Telemetry,
    TransferProgress,
    TransferState,
    TEXT_MESSAGE_MAX_BYTES,
    StoredFile,
    StorageListing,
)
from rf3_gui.device.base import DeviceBackend
from rf3_gui.device.windows_bluetooth import WindowsBluetooth
from rf3_gui.protocol import console_protocol
from rf3_gui.protocol.status_parser import parse_fraction, parse_status_line


SERIAL_BAUD_RATE = 115200
SERIAL_READ_TIMEOUT_SECONDS = 0.2
SERIAL_WRITE_TIMEOUT_SECONDS = 2.0
BLUETOOTH_WRITE_TIMEOUT_SECONDS = 8.0
HANDSHAKE_TIMEOUT_MS = 25000
BLUETOOTH_SETUP_TIMEOUT_MS = 120000
SERIAL_SETUP_TIMEOUT_MS = 30000
HANDSHAKE_STATUS_RETRY_MS = 3000
STATUS_POLL_MS = 3000
STATUS_RESPONSE_TIMEOUT_MS = 15000
COMMAND_RESPONSE_TIMEOUT_MS = 15000
RF_PROGRESS_TIMEOUT_MS = 30000
RX_CANCEL_TIMEOUT_MS = 15000
TEST_TX_TIMEOUT_MS = 10000
TEST_MODE_TIMEOUT_MS = 10000
TEXT_ECHO_TIMEOUT_MS = 10000
HOST_TRANSFER_TIMEOUT_MS = 30000
HOST_TRANSFER_CHUNK_BYTES = 64
HOST_TRANSFER_FAST_CHUNK_BYTES = 256
SERIAL_READ_CHUNK_BYTES = 4096
HOST_FILENAME_MAX_BYTES = 30
SERIAL_MAX_RESPONSE_BYTES = 4096
HOST_TRANSFER_MAX_RETRIES = 3
RX_ACTIVE_STATES = frozenset({"Receiving", "WaitingForEnd", "Verifying", "Publishing"})
RX_TERMINAL_STATES = frozenset({"Completed", "Cancelled", "Failed"})


class _LineBuffer:
    """Retain incomplete lines across read timeouts; bound corrupt input."""

    def __init__(self) -> None:
        self.pending = bytearray()
        self.dropping = False

    def feed(self, data: bytes) -> list[str]:
        lines = []
        for byte in data:
            if byte == 10:
                if not self.dropping:
                    line = self.pending.decode("utf-8", errors="replace").strip()
                    if line:
                        lines.append(line)
                self.pending.clear()
                self.dropping = False
            elif not self.dropping:
                self.pending.append(byte)
                if len(self.pending) > SERIAL_MAX_RESPONSE_BYTES:
                    self.pending.clear()
                    self.dropping = True
        return lines


@dataclass
class _UploadSession:
    source_path: Path
    remote_name: str
    data: bytes
    crc32: int
    verify_only: bool = False
    store_only: bool = False
    chunk_bytes: int = HOST_TRANSFER_CHUNK_BYTES
    sequence: int = 0
    acknowledged_bytes: int = 0
    current_chunk: bytes = b""
    ready: bool = False
    acknowledged_sequence: int | None = None
    acknowledged_total: int | None = None
    complete: bool = False


@dataclass
class _DownloadSession:
    source_name: str
    destination_path: Path | None
    partial_path: Path | None
    verification_source: _UploadSession | None = None
    file: BinaryIO | None = None
    expected_size: int | None = None
    expected_crc32: int | None = None
    sequence: int = 0
    received_bytes: int = 0
    running_crc32: int = 0
    ready: bool = False
    data_received: bool = False
    complete: bool = False
    encoded_name: bool = False
    chunk_bytes: int = HOST_TRANSFER_CHUNK_BYTES


class SerialDeviceBackend(DeviceBackend):
    """USB serial backend for the firmware's existing console command path."""

    _worker_event = Signal(int, str, object)
    _discovery_result = Signal(int, object, str)

    def __init__(self, preferred_port: str | None = None, *, bluetooth_adapter=None) -> None:
        super().__init__()
        self._preferred_port = preferred_port
        self._bluetooth = bluetooth_adapter if bluetooth_adapter is not None else WindowsBluetooth()
        self._pairing_window = 0
        self._setup_cancel = threading.Event()
        self._discovery_cancel = threading.Event()
        self._discovery_generation = 0
        self._discovery_running = False
        self._connection_generation = 0
        self._serial = None
        self._reader_thread: threading.Thread | None = None
        self._writer_thread: threading.Thread | None = None
        self._active_device: DeviceInfo | None = None
        self._stop_event = threading.Event()
        self._write_queue: queue.Queue[str | None] = queue.Queue()
        self._pending_commands: deque[str] = deque()
        self._command_deadlines: dict[str, float] = {}
        self._last_tx_id = ""
        self._rf_tx_name: str | None = None
        self._rf_tx_baseline_id = ""
        self._rf_tx_id = ""
        self._rf_progress_marker: tuple | None = None
        self._pending_selection_name: str | None = None
        self._observed_rf_id = ""
        self._rx_cancel_requested = False
        self._handshake_ok = False
        self._identity_ok = False
        self._hello_sent = False
        self._handshake_write_pending = False
        self._handshake_write_warning_shown = False
        self._last_test_tx_count = 0
        self._awaiting_test_tx_count: int | None = None
        self._awaiting_test_mode: bool | None = None
        self._awaiting_echo: str | None = None
        self._upload_session: _UploadSession | None = None
        self._download_session: _DownloadSession | None = None
        self._last_host_command: str | None = None
        self._host_retry_count = 0
        self._storage = StorageListing()
        self._storage_kind: str | None = None
        self._storage_files: list[StoredFile] = []
        self._storage_offset = 0
        self._storage_total: int | None = None
        self._storage_delete_name: str | None = None
        self._snapshot = DeviceSnapshot(
            connection_state=ConnectionState.DISCONNECTED,
            requested_radio_state=RadioState.UNKNOWN,
            confirmed_radio_state=RadioState.UNKNOWN,
            test_mode=False,
            test_tx_count=0,
            test_tx_bytes=0,
            channel=DEFAULT_CHANNEL,
            power_level=None,
            selected_file="",
            profile="",
            active_tx_file="",
            telemetry=Telemetry(),
            capabilities=RadioCapabilities(supports_battery_percent=False),
        )

        self._status_poll_timer = QTimer(self)
        self._status_poll_timer.setInterval(STATUS_POLL_MS)
        self._status_poll_timer.timeout.connect(self.request_status)

        self._status_response_timer = QTimer(self)
        self._status_response_timer.setSingleShot(True)
        self._status_response_timer.timeout.connect(
            lambda: self._on_read_failed("ESP stopped responding to STATUS; reconnect the device")
        )

        self._handshake_timer = QTimer(self)
        self._handshake_timer.setSingleShot(True)
        self._handshake_timer.timeout.connect(self._check_handshake)

        self._setup_timer = QTimer(self)
        self._setup_timer.setSingleShot(True)
        self._setup_timer.timeout.connect(self._check_handshake)

        self._handshake_status_timer = QTimer(self)
        self._handshake_status_timer.setInterval(HANDSHAKE_STATUS_RETRY_MS)
        self._handshake_status_timer.timeout.connect(self._send_handshake_status)

        self._test_tx_timer = QTimer(self)
        self._test_tx_timer.setSingleShot(True)
        self._test_tx_timer.timeout.connect(self._on_test_tx_timeout)

        self._test_mode_timer = QTimer(self)
        self._test_mode_timer.setSingleShot(True)
        self._test_mode_timer.timeout.connect(self._on_test_mode_timeout)

        self._text_echo_timer = QTimer(self)
        self._text_echo_timer.setSingleShot(True)
        self._text_echo_timer.timeout.connect(lambda: self._on_read_failed(
            "ESP did not return the text echo within 10 seconds; reconnect before retrying"
        ))

        self._host_transfer_timer = QTimer(self)
        self._host_transfer_timer.setSingleShot(True)
        self._host_transfer_timer.timeout.connect(self._on_host_transfer_timeout)

        self._storage_timer = QTimer(self)
        self._storage_timer.setSingleShot(True)
        self._storage_timer.timeout.connect(self._on_storage_timeout)

        self._command_timer = QTimer(self)
        self._command_timer.setInterval(250)
        self._command_timer.timeout.connect(self._check_command_deadlines)
        self._rf_progress_timer = QTimer(self)
        self._rf_progress_timer.setSingleShot(True)
        self._rf_progress_timer.timeout.connect(self._on_rf_progress_timeout)
        self._rx_cancel_timer = QTimer(self)
        self._rx_cancel_timer.setSingleShot(True)
        self._rx_cancel_timer.timeout.connect(self._on_rx_cancel_timeout)

        self._worker_event.connect(self._on_worker_event)
        self._discovery_result.connect(self._on_discovery_result)
        self.log_event.connect(self._record_log)

    @staticmethod
    def _record_log(level: str, message: str) -> None:
        # Keep connection diagnostics without persisting uploaded/downloaded
        # file bytes or live message contents from the UI's debug transcript.
        if level.lower() == "debug" and not (
                message.startswith(("TX -> STATUS", "TX -> HELLO", "RX <- {",
                                    "RX <- OK STATUS", "RX <- ERR STATUS", "RX <- RF3"))):
            return
        logging.getLogger(__name__).log(getattr(logging, level.upper(), logging.INFO), message)

    def _connection_stage(self, message: str) -> None:
        self._snapshot = self._replace_snapshot(connection_detail=message)
        self.status_changed.emit(self._snapshot)
        self.log_event.emit("info", message)

    def discover(self) -> None:
        self._begin_discovery(False)

    def scan_bluetooth(self) -> None:
        self._begin_discovery(True)

    def set_pairing_window(self, handle: int) -> None:
        self._pairing_window = handle

    def _begin_discovery(self, bluetooth: bool) -> None:
        if self._serial is not None or self._snapshot.connection_state == ConnectionState.CONNECTING or self._discovery_running:
            self.command_finished.emit("DISCOVER", False, "Disconnect or cancel the current operation before scanning")
            return
        self._discovery_generation += 1
        self._discovery_cancel = threading.Event()
        self._discovery_running = True
        self._snapshot = self._replace_snapshot(connection_state=ConnectionState.SEARCHING)
        self.status_changed.emit(self._snapshot)
        self.log_event.emit("info", "Scanning Classic Bluetooth (about 10 seconds)" if bluetooth else "Refreshing serial ports")
        thread = threading.Thread(target=self._discover_worker,
                                  args=(bluetooth, self._discovery_generation, self._discovery_cancel), daemon=True)
        thread.start()

    def _on_discovery_result(self, generation: int, devices: list, error: str) -> None:
        if generation != self._discovery_generation:
            return
        self._discovery_running = False
        self._snapshot = self._replace_snapshot(
            connection_state=ConnectionState.ERROR if error else ConnectionState.DISCONNECTED
        )
        self.devices_discovered.emit(devices)
        self.status_changed.emit(self._snapshot)
        self.command_finished.emit("DISCOVER", not bool(error), error or f"Found {len(devices)} device/port entries")

    def connect_to(self, device: DeviceInfo) -> None:
        if device.transport not in {"serial", "bluetooth"}:
            self.command_finished.emit("CONNECT", False, "Selected device has no supported transport")
            return
        if self._serial is not None or self._snapshot.connection_state == ConnectionState.CONNECTING or self._discovery_running:
            self.command_finished.emit(
                "CONNECT",
                False,
                "A serial connection is already open or still connecting. Disconnect it first.",
            )
            return

        self._active_device = device
        self._storage = StorageListing()
        self.storage_changed.emit(self._storage)
        self._setup_cancel = threading.Event()
        self._connection_generation += 1
        generation = self._connection_generation
        self._snapshot = self._replace_snapshot(
            connection_state=ConnectionState.CONNECTING,
            connection_detail="Preparing Bluetooth connection" if _is_bluetooth_serial(device) else f"Opening {device.address}",
            requested_radio_state=RadioState.UNKNOWN,
            confirmed_radio_state=RadioState.UNKNOWN,
            test_mode=False,
            test_tx_count=0,
            test_tx_bytes=0,
            channel=DEFAULT_CHANNEL,
            power_level=None,
            selected_file="",
            profile="",
            active_tx_file="",
            telemetry=Telemetry(),
            capabilities=RadioCapabilities(supports_battery_percent=False),
        )
        self.status_changed.emit(self._snapshot)
        self._setup_timer.start(BLUETOOTH_SETUP_TIMEOUT_MS if _is_bluetooth_serial(device) else SERIAL_SETUP_TIMEOUT_MS)
        thread = threading.Thread(target=self._connect_worker,
                                  args=(device, generation, self._setup_cancel), daemon=True)
        thread.start()

    def disconnect_from_device(self) -> None:
        self._discovery_cancel.set()
        self._discovery_generation += 1
        self._discovery_running = False
        if self._snapshot.connection_state == ConnectionState.CONNECTING and self._active_device and self._active_device.transport == "bluetooth":
            self.log_event.emit("info", "Local connection cancelled. Dismiss any Windows pairing dialog; Windows may retain pairing if it already completed.")
        self._handshake_timer.stop()
        self._handshake_status_timer.stop()
        self._status_poll_timer.stop()
        self._test_tx_timer.stop()
        self._test_mode_timer.stop()
        self._host_transfer_timer.stop()
        self._close_serial_port()
        self._active_device = None
        self._awaiting_test_tx_count = None
        self._awaiting_test_mode = None
        self._snapshot = DeviceSnapshot(connection_state=ConnectionState.DISCONNECTED,
                                        connection_detail="Disconnected")
        self.status_changed.emit(self._snapshot)
        self.log_event.emit("info", "Serial device disconnected")

    def request_status(self) -> None:
        self._send_command(console_protocol.status_command())

    def _storage_allowed(self, command: str) -> bool:
        if not self._operation_allowed(command):
            return False
        if (self._serial is None or not self._handshake_ok
                or not self._snapshot.capabilities.supports_host_storage):
            self.command_finished.emit(command, False, "Connect to firmware with SPIFFS file management support")
            return False
        if (self._snapshot.connection_state != ConnectionState.CONNECTED
                or self._storage_kind is not None or self._upload_session is not None
                or self._download_session is not None or self._awaiting_echo is not None
                or self._awaiting_test_mode is not None):
            self.command_finished.emit(command, False, "Wait for the current device operation to finish")
            return False
        return True

    def refresh_storage(self) -> None:
        if not self._storage_allowed("STORAGE LIST"):
            return
        self._storage_kind = "list"
        self._storage_files = []
        self._storage_offset = 0
        self._storage_total = None
        self._storage = StorageListing(self._storage.files, self._storage.used, self._storage.capacity, False)
        self.storage_changed.emit(self._storage)
        self._begin_storage(console_protocol.storage_list_command())

    def delete_file(self, remote_name: str) -> None:
        if not self._storage_allowed("STORAGE DELETE"):
            return
        if not console_protocol.valid_storage_name(remote_name):
            self.command_finished.emit("STORAGE DELETE", False, "Invalid or protected SPIFFS file name")
            return
        self._storage_kind = "delete"
        self._storage_delete_name = remote_name
        self._begin_storage(console_protocol.storage_delete_command(remote_name))

    def _begin_storage(self, command: str) -> None:
        self._pause_status_polling()
        self._snapshot = self._replace_snapshot(connection_state=ConnectionState.BUSY)
        self.status_changed.emit(self._snapshot)
        self._send_command(command)
        self._storage_timer.start(HOST_TRANSFER_TIMEOUT_MS)

    def _finish_storage(self, accepted: bool, message: str) -> None:
        kind = self._storage_kind
        if kind is None:
            return
        self._storage_timer.stop()
        self._storage_kind = None
        self._storage_delete_name = None
        if not accepted:
            self._storage = StorageListing(self._storage.files, self._storage.used, self._storage.capacity, False)
            self.storage_changed.emit(self._storage)
        self._pending_commands = deque(command for command in self._pending_commands
                                       if not command.startswith("STORAGE "))
        self._snapshot = self._replace_snapshot(connection_state=ConnectionState.CONNECTED)
        self.status_changed.emit(self._snapshot)
        if self._handshake_ok:
            self._status_poll_timer.start()
        self.command_finished.emit("STORAGE " + kind.upper(), accepted, message)
        if accepted and kind == "delete":
            self.refresh_storage()

    def _on_storage_timeout(self) -> None:
        kind = self._storage_kind
        if kind is None:
            return
        message = "SPIFFS operation timed out; reconnect before retrying"
        # Closing rejects late replies by connection generation. A delayed
        # DELETE acknowledgement must never complete a later delete request.
        self._on_read_failed(message)
        self.command_finished.emit("STORAGE " + kind.upper(), False, message)

    def _handle_storage_response(self, values: object) -> None:
        if self._storage_kind is None:
            return  # Unsolicited/late replies cannot perform another operation.
        try:
            if not isinstance(values, dict):
                raise ValueError("Malformed SPIFFS response")
            if values.get("op") == "error":
                self._finish_storage(False, "SPIFFS: " + str(values.get("message", "operation failed")))
                return
            if self._storage_kind == "delete":
                if values.get("op") != "delete" or values.get("name") != self._storage_delete_name:
                    raise ValueError("SPIFFS delete response does not match the selected file")
                self._finish_storage(True, "Deleted " + self._storage_delete_name + " from SPIFFS")
                return
            offset, total = values.get("offset"), values.get("total")
            used, capacity = values.get("used"), values.get("capacity")
            files, next_offset = values.get("files"), values.get("next")
            if (values.get("op") != "list" or type(offset) is not int or offset != self._storage_offset
                    or type(total) is not int or not 0 <= total <= 4096
                    or type(used) is not int or type(capacity) is not int or not 0 <= used <= capacity
                    or not isinstance(files, list) or len(files) > 4
                    or (self._storage_total is not None and total != self._storage_total)):
                raise ValueError("SPIFFS listing changed or contains invalid metadata; refresh again")
            entries = []
            seen = {entry.name for entry in self._storage_files}
            for item in files:
                if (not isinstance(item, dict) or not console_protocol.valid_storage_name(item.get("name"))
                        or type(item.get("size")) is not int or item["size"] < 0
                        or item["name"] in seen):
                    raise ValueError("Invalid or duplicate SPIFFS file entry")
                seen.add(item["name"])
                entries.append(StoredFile(item["name"], item["size"]))
            end = offset + len(entries)
            if end > total or (next_offset is None and end != total) or (
                    next_offset is not None and (type(next_offset) is not int or not entries
                                                  or next_offset != end or end >= total)):
                raise ValueError("Invalid SPIFFS listing cursor")
            self._storage_total = total
            self._storage_files.extend(entries)
            self._pending_commands = deque(command for command in self._pending_commands
                                           if not command.startswith("STORAGE LIST"))
            if next_offset is not None:
                self._storage_offset = next_offset
                self._send_command(console_protocol.storage_list_command(next_offset))
                self._storage_timer.start(HOST_TRANSFER_TIMEOUT_MS)
                return
            self._storage = StorageListing(tuple(self._storage_files), used, capacity, True)
            self.storage_changed.emit(self._storage)
            self._finish_storage(True, f"Listed {total} SPIFFS file(s)")
        except (ValueError, TypeError, UnicodeError) as exc:
            self._finish_storage(False, str(exc))

    def _operation_allowed(self, command: str, *, radio: bool = False) -> bool:
        if (self._serial is None or not self._handshake_ok
                or self._snapshot.connection_state not in {ConnectionState.CONNECTED, ConnectionState.BUSY}):
            self.command_finished.emit(command, False, "Connect to RF3 firmware first")
            return False
        if (self._upload_session is not None or self._download_session is not None
                or self._storage_kind is not None or self._awaiting_echo is not None
                or self._awaiting_test_mode is not None or self._rf_tx_name is not None
                or self._incoming_rx_active()
                or self._snapshot.connection_state == ConnectionState.BUSY):
            self.command_finished.emit(command, False, "Wait for the current device operation to finish")
            return False
        if any(not item.startswith("STATUS") for item in self._pending_commands):
            self.command_finished.emit(command, False, "Wait for the previous command acknowledgement")
            return False
        if radio and self._snapshot.confirmed_radio_state not in {RadioState.STANDBY, RadioState.RX_LISTENING}:
            self.command_finished.emit(command, False,
                                       "Radio is not ready; check power/wiring and reinitialize the channel")
            return False
        return True

    def _radio_command(self, command: str, requested: RadioState | None = None,
                       *, ready: bool = False) -> None:
        if not self._operation_allowed(command, radio=ready):
            return
        if requested is not None:
            self._snapshot = self._replace_snapshot(requested_radio_state=requested)
            self.status_changed.emit(self._snapshot)
        self._send_command(command)

    def set_test_mode(self, enabled: bool) -> None:
        if not self._operation_allowed("TESTMODE"):
            return
        if not self._snapshot.capabilities.supports_test_mode:
            self.command_finished.emit("TESTMODE", False, "Test mode is available only on supported devboard firmware")
            return
        self._awaiting_test_mode = enabled
        self._send_command(console_protocol.test_mode_command(enabled))
        self._test_mode_timer.start(TEST_MODE_TIMEOUT_MS)

    def set_channel(self, channel: int) -> None:
        if type(channel) is not int or not 0 <= channel <= 125:
            self.command_finished.emit("CHANNEL", False, "Channel must be 0-125")
            return
        self._radio_command(console_protocol.channel_command(channel), RadioState.STANDBY)

    def set_power(self, level: int) -> None:
        if type(level) is not int or level not in (0, 1, 2, 3):
            self.command_finished.emit("POWER", False, "Power must be 0-3")
            return
        self._radio_command(console_protocol.power_command(level), ready=True)

    def enter_rx(self) -> None:
        # Receiving is already armed. Repeating this action must never reset an
        # in-progress file or its CRC/counters, even if a stale UI still offers it.
        other_operation = (self._upload_session is not None or self._download_session is not None
                           or self._storage_kind is not None or self._awaiting_echo is not None
                           or self._awaiting_test_mode is not None or self._rf_tx_name is not None
                           or any(command != "STATUS" for command in self._pending_commands))
        if (self._serial is not None and self._handshake_ok
                and self._snapshot.confirmed_radio_state == RadioState.RX_LISTENING
                and self._snapshot.connection_state in {ConnectionState.CONNECTED, ConnectionState.BUSY}
                and not other_operation):
            return
        self._radio_command(console_protocol.rx_command(), RadioState.RX_LISTENING, ready=True)

    def enter_standby(self) -> None:
        self._radio_command(console_protocol.standby_command(), RadioState.STANDBY)

    def enter_sleep(self) -> None:
        self._radio_command(console_protocol.sleep_command(), RadioState.SLEEP, ready=True)

    def wake(self) -> None:
        self._radio_command(console_protocol.wake_command(), RadioState.STANDBY)

    def power_down(self) -> None:
        self._radio_command(console_protocol.power_down_command(), RadioState.POWER_DOWN)

    def stop(self) -> None:
        # STOP is allowed to cancel active RF, but never interleaves a host-file
        # or storage transaction (disconnect cancels those safely).
        if self._serial is None or not self._handshake_ok:
            self.command_finished.emit("STOP", False, "Not connected")
            return
        if self._upload_session or self._download_session or self._storage_kind or self._awaiting_echo:
            self.command_finished.emit("STOP", False, "Finish the host operation or disconnect to cancel it")
            return
        if self._incoming_rx_active() and not self._rx_cancel_requested:
            self._rx_cancel_requested = True
            self._rx_cancel_timer.start(RX_CANCEL_TIMEOUT_MS)
        self._snapshot = self._replace_snapshot(requested_radio_state=RadioState.STANDBY)
        self.status_changed.emit(self._snapshot)
        self._send_command(console_protocol.stop_command())

    def select_stored_file(self, name: str) -> None:
        if not self._stored_action_allowed("SELECTHEX", name, radio=False):
            return
        self._pending_selection_name = name
        self._send_command(console_protocol.select_stored_command(name))

    def start_stored_tx(self, name: str) -> None:
        if not self._stored_action_allowed("TXHEX", name, radio=True):
            return
        if self._snapshot.test_mode:
            self.command_finished.emit("TXHEX", False, "Disable test mode before RF transmission")
            return
        self._begin_rf_transfer(name)
        self._send_command(console_protocol.stored_tx_command(name))

    def _stored_action_allowed(self, command: str, name: str, *, radio: bool) -> bool:
        if not console_protocol.valid_storage_name(name):
            self.command_finished.emit(command, False, "Invalid or protected SPIFFS file name")
            return False
        if radio:
            try:
                console_protocol.transfer_extension(name)
            except ValueError as exc:
                self.command_finished.emit(command, False, str(exc))
                return False
        if not self._operation_allowed(command, radio=radio):
            return False
        if not self._snapshot.capabilities.supports_stored_file_tx:
            self.command_finished.emit(command, False, "Flash firmware with stored-file selection/transmit support")
            return False
        return True

    def _begin_rf_transfer(self, name: str, *, source_path: Path | None = None,
                           total_bytes: int = 0) -> None:
        self._rf_tx_name = name
        self._rf_tx_baseline_id = self._last_tx_id
        self._rf_tx_id = ""
        self._rf_progress_marker = None
        self._rf_progress_timer.start(RF_PROGRESS_TIMEOUT_MS)
        self._snapshot = self._replace_snapshot(connection_state=ConnectionState.BUSY,
                                                requested_radio_state=RadioState.TX_BUSY)
        self.status_changed.emit(self._snapshot)
        self.transfer_changed.emit(TransferProgress(
            state=TransferState.HANDSHAKING, total_bytes=total_bytes,
            message=f"Starting RF transfer of SPIFFS file {name}; awaiting peer handshake",
            source_path=source_path))
        self._status_poll_timer.start()

    def _finish_rf_transfer(self, state: TransferState, message: str, sent: int = 0, total: int = 0) -> None:
        self._rf_progress_timer.stop()
        self._rf_tx_name = None
        self._rf_tx_id = ""
        self._observed_rf_id = ""
        self._rf_progress_marker = None
        self._snapshot = self._replace_snapshot(
            connection_state=(ConnectionState.DISCONNECTED if self._serial is None else
                              ConnectionState.BUSY if self._snapshot.confirmed_radio_state in
                              {RadioState.TX_BUSY, RadioState.CW_TEST} else ConnectionState.CONNECTED),
            requested_radio_state=self._snapshot.confirmed_radio_state)
        self.status_changed.emit(self._snapshot)
        self.transfer_changed.emit(TransferProgress(state=state, sent_bytes=sent, total_bytes=total, message=message))

    def _on_rf_progress_timeout(self) -> None:
        if self._rf_tx_name is not None:
            self._finish_rf_transfer(TransferState.FAILED,
                                     "RF progress timed out; delivery is unconfirmed. Stop/reinitialize the radio before retrying")

    def start_tx(self, source_path: Path) -> None:
        self._start_upload(source_path)

    def import_file(self, source_path: Path) -> None:
        if self._storage_allowed("UPLOAD"):
            self._start_upload(source_path, store_only=True)

    def _start_upload(self, source_path: Path, *, store_only: bool = False) -> None:
        if not self._operation_allowed("UPLOAD", radio=not (store_only or self._snapshot.test_mode)):
            return
        if self._awaiting_test_mode is not None or self._awaiting_echo is not None:
            self.command_finished.emit("UPLOAD", False, "Wait for the ESP to confirm test mode")
            return
        if self._upload_session is not None or self._download_session is not None or self._storage_kind is not None:
            self.command_finished.emit("UPLOAD", False, "Another host file transfer is active")
            return
        if not self._snapshot.capabilities.supports_host_file_upload:
            self.command_finished.emit(
                "UPLOAD",
                False,
                "Connected firmware does not advertise PC-to-ESP file upload support",
            )
            return
        if (self._snapshot.test_mode or store_only) and not self._snapshot.capabilities.supports_host_file_download:
            self.command_finished.emit("UPLOAD", False, "Test-mode verification requires firmware with file download support")
            return
        try:
            # Validate the original suffix before normalization or reading any
            # file bytes: a published/received file must retain its actual type.
            if store_only or self._snapshot.test_mode:
                console_protocol.upload_extension(source_path.name)
            else:
                console_protocol.transfer_extension(source_path.name)
            remote_name = _safe_remote_filename(source_path.name)
        except ValueError as exc:
            self.command_finished.emit("UPLOAD", False, str(exc))
            return
        if not source_path.is_file():
            self.command_finished.emit("UPLOAD", False, "Selected file no longer exists")
            return
        try:
            # Bound the read even if the file grew after GUI selection.
            with source_path.open("rb") as source:
                data = source.read(PROTOCOL_V2_MAX_FILE_SIZE + 1)
        except OSError as exc:
            self.command_finished.emit("UPLOAD", False, f"Could not read selected file: {exc}")
            return
        if len(data) > PROTOCOL_V2_MAX_FILE_SIZE:
            self.command_finished.emit("UPLOAD", False, "File exceeds Protocol v2 size limit")
            return

        self._upload_session = _UploadSession(
            source_path=source_path,
            remote_name=remote_name,
            data=data,
            crc32=binascii.crc32(data) & 0xFFFFFFFF,
            verify_only=self._snapshot.test_mode or store_only,
            store_only=store_only,
            chunk_bytes=_negotiated_host_chunk(self._snapshot.capabilities.host_chunk_max),
        )
        self._pause_status_polling()
        self._snapshot = self._replace_snapshot(
            connection_state=ConnectionState.BUSY,
            requested_radio_state=(self._snapshot.confirmed_radio_state
                                   if self._upload_session.verify_only else RadioState.TX_BUSY),
        )
        self.status_changed.emit(self._snapshot)
        self.log_event.emit("info", f"Upload host chunk size: {self._upload_session.chunk_bytes} bytes; CRC32 enabled")
        self.transfer_changed.emit(
            TransferProgress(
                state=TransferState.PREPARING,
                sent_bytes=0,
                total_bytes=len(data),
                message=f"Uploading {remote_name} to ESP storage"
                        + (" for readback verification; no nRF24 TX" if self._upload_session.verify_only else ""),
                source_path=source_path,
            )
        )
        self._send_host_command(
            console_protocol.upload_begin_command(remote_name, len(data), self._upload_session.crc32,
                                                  self._upload_session.chunk_bytes)
        )

    def export_file(self, remote_name: str, destination_path: Path) -> None:
        if not self._storage_allowed("DOWNLOAD"):
            return
        if not console_protocol.valid_storage_name(remote_name):
            self.command_finished.emit("DOWNLOAD", False, "Invalid or protected SPIFFS file name")
            return
        self._start_download(remote_name, destination_path, encoded_name=True)

    def save_received_file(self, destination_path: Path) -> None:
        source_name = self._snapshot.telemetry.last_rx_file
        if not source_name:
            self.command_finished.emit("DOWNLOAD", False, "No received file is available")
            return
        self._start_download(source_name, destination_path)

    def _start_download(self, source_name: str, destination_path: Path, *, encoded_name: bool = False) -> None:
        if not self._operation_allowed("DOWNLOAD"):
            return
        if not console_protocol.valid_storage_name(source_name):
            self.command_finished.emit("DOWNLOAD", False, "Invalid or protected SPIFFS file name")
            return
        if self._upload_session is not None or self._download_session is not None or self._awaiting_echo is not None or self._storage_kind is not None:
            self.command_finished.emit("DOWNLOAD", False, "Another host file transfer is active")
            return
        if not self._snapshot.capabilities.supports_host_file_download:
            self.command_finished.emit(
                "DOWNLOAD",
                False,
                "Connected firmware does not advertise ESP-to-PC file download support",
            )
            return
        try:
            temporary_file = tempfile.NamedTemporaryFile(mode="wb", prefix=".rf3-", suffix=".part",
                                                         dir=destination_path.parent, delete=False)
            partial_path = Path(temporary_file.name)
        except OSError as exc:
            self.command_finished.emit("DOWNLOAD", False, f"Could not prepare destination: {exc}")
            return

        self._download_session = _DownloadSession(
            source_name=source_name,
            destination_path=destination_path,
            partial_path=partial_path,
            encoded_name=encoded_name,
            file=temporary_file,
            chunk_bytes=_negotiated_host_chunk(self._snapshot.capabilities.host_chunk_max),
        )
        self._pause_status_polling()
        self._snapshot = self._replace_snapshot(connection_state=ConnectionState.BUSY)
        self.status_changed.emit(self._snapshot)
        self.transfer_changed.emit(
            TransferProgress(
                state=TransferState.HANDSHAKING,
                message=f"Opening {source_name} on ESP storage",
                source_path=destination_path,
            )
        )
        chunk = self._download_session.chunk_bytes
        self.log_event.emit("info", f"Download host chunk size: {chunk} bytes; CRC32 enabled")
        self._send_host_command(console_protocol.download_begin_hex_command(source_name, chunk) if encoded_name
                                else console_protocol.download_begin_command(source_name, chunk))

    def send_morse_text(self, text: str) -> None:
        self._radio_command(console_protocol.morse_command(text), RadioState.TX_BUSY, ready=True)

    def send_remote_command(self, command: str) -> None:
        self._radio_command(console_protocol.remote_command(command), RadioState.TX_BUSY, ready=True)

    def send_text_message(self, text: str) -> None:
        if not self._operation_allowed("TEXT", radio=not self._snapshot.test_mode):
            return
        if self._serial is None or not self._handshake_ok:
            self.command_finished.emit("TEXT", False, "Connect to RF3 firmware before sending text")
            return
        if (self._awaiting_test_mode is not None or self._awaiting_echo is not None
                or self._upload_session is not None or self._download_session is not None):
            self.command_finished.emit("TEXT", False, "Wait for the current ESP operation to finish")
            return
        if not text or len(text) > TEXT_MESSAGE_MAX_BYTES or any(not 0x20 <= ord(ch) <= 0x7E for ch in text):
            self.command_finished.emit("TEXT", False, "Use 1-154 printable ASCII characters")
            return
        if self._snapshot.test_mode:
            if not self._snapshot.capabilities.supports_host_echo:
                self.command_finished.emit("ECHO", False, "Flash the updated devboard firmware to enable ESP text echo")
                return
            # RF3 ASCII host protocol over UART/Bluetooth SIG SPP. ECHO is
            # distinct from RF TEXT and succeeds only on matching ESP content.
            self._awaiting_echo = text
            self._pause_status_polling()
            self._snapshot = self._replace_snapshot(connection_state=ConnectionState.BUSY)
            self.status_changed.emit(self._snapshot)
            self._send_command(console_protocol.echo_command(text))
            self._text_echo_timer.start(TEXT_ECHO_TIMEOUT_MS)
        else:
            if not self._snapshot.capabilities.supports_text_messages:
                self.command_finished.emit("TEXT", False, "Firmware does not advertise radio text support")
                return
            self._send_command(console_protocol.text_command(text))

    def _finish_text_echo(self, accepted: bool, message: str) -> None:
        text = self._awaiting_echo
        if text is None:
            return
        self._awaiting_echo = None
        self._text_echo_timer.stop()
        self._snapshot = self._replace_snapshot(connection_state=ConnectionState.CONNECTED)
        self.status_changed.emit(self._snapshot)
        if accepted:
            self.text_message_received.emit("test_tx", text)
            self.text_message_received.emit("echo", text)
        self._finish_pending(accepted, message, "ECHO")
        self._status_poll_timer.start()

    def start_cw(self, channel: int, power_level: int) -> None:
        self._radio_command(console_protocol.cw_start_command(channel, power_level), RadioState.CW_TEST, ready=True)

    def start_cw_loop(self, on_ms: int, off_ms: int, channel: int, power_level: int) -> None:
        self._radio_command(console_protocol.cw_loop_command(on_ms, off_ms, channel, power_level),
                            RadioState.CW_TEST, ready=True)

    def _discover_worker(self, bluetooth=False, generation=None, cancel=None) -> None:
        generation = self._discovery_generation if generation is None else generation
        cancel = self._discovery_cancel if cancel is None else cancel
        try:
            from serial.tools import list_ports
        except ImportError:
            self._discovery_result.emit(generation, [], "pyserial is not installed. Install host_gui requirements first.")
            return

        devices: list[DeviceInfo] = []
        error = ""
        if bluetooth:
            try:
                devices.extend(DeviceInfo(
                    name=peer.name, address=peer.address, transport="bluetooth",
                    protocol_version="SPP candidate", bluetooth_address=peer.address,
                    paired=peer.paired, seen_in_scan=peer.seen_in_scan,
                ) for peer in self._bluetooth.scan(cancel))
            except Exception as exc:
                error = str(exc)
        if cancel.is_set():
            return
        skipped_incoming_bluetooth_ports = 0
        try:
            ports = list(list_ports.comports())
        except Exception as exc:
            self._discovery_result.emit(generation, devices, f"Serial port enumeration failed: {exc}")
            return
        for port in ports:
            label = port.description or "Serial port"
            hwid = str(getattr(port, "hwid", ""))
            if _is_windows_incoming_bluetooth_port(label, hwid):
                skipped_incoming_bluetooth_ports += 1
                continue
            if _is_bluetooth_port(label, hwid):
                label = f"{label} (Bluetooth SPP)"
            devices.append(
                DeviceInfo(
                    name=label,
                    address=port.device,
                    transport="serial",
                    protocol_version="console",
                )
            )

        if self._preferred_port and all(d.address != self._preferred_port for d in devices):
            devices.insert(
                0,
                DeviceInfo(
                    name=f"Preferred serial port {self._preferred_port}",
                    address=self._preferred_port,
                    transport="serial",
                    protocol_version="console",
                ),
            )

        if self._preferred_port:
            devices.sort(key=lambda d: 0 if d.address == self._preferred_port else 1)

        if skipped_incoming_bluetooth_ports:
            self.log_event.emit(
                "info",
                (
                    f"Ignored {skipped_incoming_bluetooth_ports} incoming Bluetooth COM "
                    "port(s); use the outgoing RF3/ESP SPP port instead."
                ),
            )

        self._discovery_result.emit(generation, devices, error)

    def _connect_worker(self, device: DeviceInfo, generation: int, setup_cancel=None) -> None:
        if generation != self._connection_generation:
            return
        cancel = self._setup_cancel if setup_cancel is None else setup_cancel
        if device.transport == "bluetooth":
            try:
                port_name = self._bluetooth.prepare_serial(
                    device.address, cancel, parent_window=self._pairing_window,
                    progress=lambda message: self._worker_event.emit(generation, "setup_progress", message),
                )
                if cancel.is_set() or generation != self._connection_generation:
                    return
                device = DeviceInfo(name=f"{device.name} (Bluetooth SPP)", address=port_name,
                                    transport="serial", protocol_version="console",
                                    bluetooth_address=device.address, paired=True)
                self._worker_event.emit(generation, "resolved", device)
            except Exception as exc:
                self._worker_event.emit(generation, "open_error", str(exc))
                return
        try:
            import serial
        except ImportError:
            self._worker_event.emit(
                generation, "open_error", "pyserial is not installed. Install host_gui requirements first."
            )
            return

        serial_port = None
        try:
            if generation != self._connection_generation or (device.bluetooth_address and cancel.is_set()):
                return
            serial_port = serial.Serial(
                port=None,
                baudrate=SERIAL_BAUD_RATE,
                timeout=SERIAL_READ_TIMEOUT_SECONDS,
                write_timeout=_write_timeout_for(device),
                rtscts=False,
                dsrdtr=False,
            )
            serial_port.port = device.address
            serial_port.dtr = False
            serial_port.rts = False
            self._worker_event.emit(generation, "setup_progress", f"Opening {device.address}")
            serial_port.open()
        except Exception as exc:
            if serial_port is not None:
                serial_port.close()
            self._worker_event.emit(generation, "open_error", _format_open_error(device, exc))
            return

        try:
            serial_port.dtr = False
            serial_port.rts = False
            serial_port.reset_input_buffer()
            serial_port.reset_output_buffer()
        except Exception as exc:
            self.log_event.emit(
                "warning",
                f"{device.address} opened, but DTR/RTS setup was ignored: {exc}",
            )

        self._worker_event.emit(generation, "opened", (device, serial_port))

    def _on_worker_event(self, generation: int, kind: str, payload: Any) -> None:
        if generation != self._connection_generation:
            if kind == "opened":
                payload[1].close()
            return
        if kind == "opened":
            device, serial_port = payload
            self._start_serial_io(device, serial_port, generation)
        elif kind == "setup_progress":
            self._connection_stage(str(payload))
        elif kind == "resolved":
            self._active_device = payload
            self.log_event.emit("info", f"Selected Bluetooth {payload.bluetooth_address} maps to {payload.address}")
        elif kind == "open_error":
            self._on_connect_failed(str(payload))
        elif self._serial is not None:
            if kind == "line":
                self._handle_line(str(payload))
            elif kind == "read_error":
                self._on_read_failed(str(payload))
            elif kind == "write_error":
                if payload[0] == "STATUS":
                    self._handshake_write_pending = False
                self._on_write_failed(*payload)
            elif kind == "written":
                if payload == "STATUS":
                    self._handshake_write_pending = False

    def _start_serial_io(self, device: DeviceInfo, serial_port: Any, generation: int) -> None:
        self._active_device = device
        self._serial = serial_port
        # Each connection gets a fresh stop event and write queue. Reusing either
        # can let a late thread from the previous connection consume new commands
        # (or let a stale sentinel immediately stop the new writer).
        self._stop_event = threading.Event()
        self._write_queue = queue.Queue()
        self._handshake_ok = False
        self._identity_ok = False
        self._hello_sent = False
        self._handshake_write_pending = False
        self._handshake_write_warning_shown = False
        self._last_test_tx_count = 0
        self._awaiting_test_tx_count = None
        self._awaiting_test_mode = None
        self._pending_commands.clear()
        self._reader_thread = threading.Thread(
            target=self._read_loop, args=(serial_port, self._stop_event, generation), daemon=True
        )
        self._writer_thread = threading.Thread(
            target=self._write_loop,
            args=(serial_port, self._stop_event, self._write_queue, generation), daemon=True
        )
        self._reader_thread.start()
        self._writer_thread.start()
        self._on_serial_opened(device.address)

    def _on_serial_opened(self, address: str) -> None:
        self._setup_timer.stop()
        self._handshake_timer.start(HANDSHAKE_TIMEOUT_MS)
        self.log_event.emit("info", f"Opened {address} at {SERIAL_BAUD_RATE} baud")
        self._connection_stage(f"Waiting for RF3 identity/status on {address}")
        self._handshake_status_timer.start()
        generation = self._connection_generation
        QTimer.singleShot(750, lambda: self._send_handshake_status()
                          if generation == self._connection_generation else None)

    def _on_connect_failed(self, message: str) -> None:
        self._handshake_timer.stop()
        self._handshake_status_timer.stop()
        self._close_serial_port()
        self.log_event.emit("error", message)
        self.command_finished.emit("CONNECT", False, message)
        self._snapshot = DeviceSnapshot(connection_state=ConnectionState.ERROR, connection_detail=message)
        self.status_changed.emit(self._snapshot)

    def _check_handshake(self) -> None:
        if self._handshake_ok or self._snapshot.connection_state != ConnectionState.CONNECTING:
            return
        self._handshake_status_timer.stop()
        was_open = self._serial is not None
        self._close_serial_port()
        message = (
            "Serial port opened, but RF3 HELLO/STATUS was not detected. "
            "Check that this is the RF3 firmware, the board finished booting, "
            "and no serial monitor is already using the port."
        )
        if not was_open:
            message = "Serial/Bluetooth port did not finish opening before the connection timeout."
        if self._active_device and _is_bluetooth_serial(self._active_device):
            message += (
                " Select this device's outgoing Bluetooth SPP port. "
                "Enable Show debug traffic to inspect the transmitted probes and received bytes."
            )
        self._snapshot = DeviceSnapshot(connection_state=ConnectionState.ERROR, connection_detail=message)
        self.status_changed.emit(self._snapshot)
        self.log_event.emit("error", message)
        self.command_finished.emit("CONNECT", False, message)

    def _send_handshake_status(self) -> None:
        if self._serial is None or self._handshake_ok or self._handshake_write_pending:
            return
        if not self._hello_sent:
            self._hello_sent = True
            self.log_event.emit("info", "Sending RF3 HELLO and STATUS probes")
            self._write_queue.put("HELLO")
        command = console_protocol.status_command()
        self.log_event.emit("debug", f"TX -> {command} (handshake)")
        self._handshake_write_pending = True
        self._write_queue.put(command)

    def _send_command(self, command: str) -> None:
        if self._serial is None:
            self.command_finished.emit(command, False, "Not connected")
            return
        if command == "STATUS":
            if self._upload_session is not None or self._download_session is not None or self._storage_kind is not None:
                return
            if "STATUS" in self._pending_commands:
                return
            if not self._status_response_timer.isActive():
                self._status_response_timer.start(STATUS_RESPONSE_TIMEOUT_MS)
        elif not command.startswith(("UPLOAD ", "DOWNLOAD ", "STORAGE ", "ECHO ", "TESTMODE", "TESTTX")):
            if command in self._pending_commands:
                self.command_finished.emit(command, False, "Command is already awaiting acknowledgement")
                return
            self._command_deadlines[command] = time.monotonic() + COMMAND_RESPONSE_TIMEOUT_MS / 1000
            self._command_timer.start()
        self._pending_commands.append(command)
        self.log_event.emit("debug", f"TX -> {command}")
        self._write_queue.put(command)

    def _check_command_deadlines(self) -> None:
        active = set(self._pending_commands)
        self._command_deadlines = {command: deadline for command, deadline in self._command_deadlines.items()
                                   if command in active}
        if not self._command_deadlines:
            self._command_timer.stop()
            return
        if any(deadline <= time.monotonic() for deadline in self._command_deadlines.values()):
            self._on_read_failed("Command acknowledgement timed out; reconnect before retrying")

    def _send_host_command(self, command: str) -> None:
        self._last_host_command = command
        self._host_retry_count = 0
        self._send_command(command)
        if self._serial is not None:
            self._host_transfer_timer.start(HOST_TRANSFER_TIMEOUT_MS)

    def _pause_status_polling(self) -> None:
        self._status_poll_timer.stop()
        self._status_response_timer.stop()
        self._pending_commands = deque(
            command for command in self._pending_commands if command != "STATUS"
        )

    def _on_host_transfer_timeout(self) -> None:
        command = self._last_host_command
        if (self._serial is not None and command is not None
                and self._snapshot.capabilities.supports_host_chunk_retry
                and command.startswith(("UPLOAD DATA ", "DOWNLOAD DATA "))
                and self._host_retry_count < HOST_TRANSFER_MAX_RETRIES):
            self._host_retry_count += 1
            self.log_event.emit("warning", f"Retrying {command.split()[0]} chunk after missing response "
                                f"({self._host_retry_count}/{HOST_TRANSFER_MAX_RETRIES})")
            self._write_queue.put(command)
            self._host_transfer_timer.start(HOST_TRANSFER_TIMEOUT_MS)
            return
        if self._upload_session is not None:
            self._fail_host_transfer("Timed out waiting for an upload acknowledgement", "UPLOAD")
        elif self._download_session is not None:
            self._fail_host_transfer("Timed out waiting for a download response", "DOWNLOAD")

    def _discard_host_transfer(self) -> None:
        download = self._download_session
        if download and download.file:
            try:
                download.file.close()
            except OSError:
                pass
        if download and download.partial_path is not None:
            try:
                download.partial_path.unlink(missing_ok=True)
            except OSError:
                pass
        self._upload_session = None
        self._download_session = None
        self._last_host_command = None
        self._host_retry_count = 0

    def _fail_host_transfer(self, message: str, kind: str) -> None:
        self._host_transfer_timer.stop()
        prefix = kind + " "
        self._pending_commands = deque(
            command for command in self._pending_commands if not command.startswith(prefix)
        )
        if self._serial is not None:
            cancel = (
                console_protocol.upload_cancel_command()
                if kind == "UPLOAD"
                else console_protocol.download_cancel_command()
            )
            self._write_queue.put(cancel)
        path = (
            self._upload_session.source_path
            if self._upload_session is not None
            else self._download_session.verification_source.source_path
            if self._download_session is not None and self._download_session.verification_source is not None
            else self._download_session.destination_path
            if self._download_session is not None
            else None
        )
        self._discard_host_transfer()
        self._snapshot = self._replace_snapshot(
            connection_state=ConnectionState.CONNECTED,
            requested_radio_state=self._snapshot.confirmed_radio_state,
        )
        self.status_changed.emit(self._snapshot)
        if self._handshake_ok:
            self._status_poll_timer.start()
        self.transfer_changed.emit(
            TransferProgress(
                state=TransferState.FAILED,
                message=message,
                source_path=path,
            )
        )

    def _host_command_finished(self, command: str, accepted: bool, message: str) -> None:
        if command.startswith("UPLOAD ") and self._upload_session is not None:
            self._advance_upload(command, accepted, message)
        elif command.startswith("DOWNLOAD ") and self._download_session is not None:
            self._advance_download(command, accepted, message)

    def _advance_upload(self, command: str, accepted: bool, message: str) -> None:
        session = self._upload_session
        if session is None:
            return
        if not accepted:
            self._fail_host_transfer(message, "UPLOAD")
            return

        if command.startswith("UPLOAD BEGIN"):
            if not session.ready:
                self._fail_host_transfer("ESP did not confirm upload metadata", "UPLOAD")
                return
            self._send_next_upload_chunk()
            return

        if command.startswith("UPLOAD DATA"):
            if (
                session.acknowledged_sequence != session.sequence
                or session.acknowledged_total
                != session.acknowledged_bytes + len(session.current_chunk)
            ):
                self._fail_host_transfer("ESP returned an invalid upload acknowledgement", "UPLOAD")
                return
            session.acknowledged_bytes += len(session.current_chunk)
            session.sequence += 1
            session.current_chunk = b""
            session.acknowledged_sequence = None
            session.acknowledged_total = None
            self.transfer_changed.emit(
                TransferProgress(
                    state=TransferState.TRANSFERRING,
                    sent_bytes=session.acknowledged_bytes,
                    total_bytes=len(session.data),
                    message="Uploading file to ESP storage",
                    source_path=session.source_path,
                )
            )
            self._send_next_upload_chunk()
            return

        if command == console_protocol.upload_end_command():
            if not session.complete:
                self._fail_host_transfer("ESP did not verify the staged upload", "UPLOAD")
                return
            self._host_transfer_timer.stop()
            if session.verify_only:
                # RF3 host protocol uses IEEE 802.3 CRC32 over USB/Bluetooth
                # SIG SPP. Read back published bytes as well as checking CRC.
                # Latch this choice in the session: even a later STATUS change
                # must never turn a validation upload into an RF TX command.
                self._upload_session = None
                self._download_session = _DownloadSession(
                    source_name=session.remote_name, destination_path=None,
                    partial_path=None, verification_source=session,
                    chunk_bytes=session.chunk_bytes,
                )
                self.transfer_changed.emit(TransferProgress(
                    state=TransferState.VERIFYING, total_bytes=len(session.data),
                    message="ESP verified upload CRC32; reading file back from SPIFFS; no nRF24 TX",
                    source_path=session.source_path,
                ))
                self._send_host_command(console_protocol.download_begin_command(session.remote_name, session.chunk_bytes))
                return
            remote_name = session.remote_name
            source_path = session.source_path
            total = len(session.data)
            self._upload_session = None
            if self._snapshot.capabilities.supports_stored_file_tx:
                command = console_protocol.stored_tx_command(remote_name)
            elif remote_name.upper() in {"LOOP", "STOP"}:
                self._snapshot = self._replace_snapshot(connection_state=ConnectionState.CONNECTED,
                                                        requested_radio_state=self._snapshot.confirmed_radio_state)
                self.status_changed.emit(self._snapshot)
                self.transfer_changed.emit(TransferProgress(
                    state=TransferState.FAILED, message="File stored, but this name requires updated encoded-TX firmware; no RF command sent",
                    source_path=source_path))
                self._status_poll_timer.start()
                return
            else:
                command = console_protocol.tx_command(remote_name)
            self._begin_rf_transfer(remote_name, source_path=source_path, total_bytes=total)
            self._send_command(command)
            self._status_poll_timer.start()

    def _send_next_upload_chunk(self) -> None:
        session = self._upload_session
        if session is None:
            return
        if session.acknowledged_bytes >= len(session.data):
            self._send_host_command(console_protocol.upload_end_command())
            return
        session.current_chunk = session.data[
            session.acknowledged_bytes:
            session.acknowledged_bytes + session.chunk_bytes
        ]
        self._send_host_command(
            console_protocol.upload_data_command(session.sequence, session.current_chunk)
        )

    def _advance_download(self, command: str, accepted: bool, message: str) -> None:
        session = self._download_session
        if session is None:
            return
        if not accepted:
            self._fail_host_transfer(message, "DOWNLOAD")
            return

        if command.startswith("DOWNLOAD BEGIN"):
            if not session.ready or session.expected_size is None or session.expected_crc32 is None:
                self._fail_host_transfer("ESP did not provide valid download metadata", "DOWNLOAD")
                return
            try:
                if session.verification_source is not None:
                    source = session.verification_source
                    if session.expected_size != len(source.data) or session.expected_crc32 != source.crc32:
                        self._fail_host_transfer("ESP readback metadata differs from the uploaded file", "DOWNLOAD")
                        return
                    session.file = io.BytesIO()
                else:
                    if session.file is None:
                        raise OSError("Export temporary file is not open")
            except OSError as exc:
                self._fail_host_transfer(f"Could not open destination: {exc}", "DOWNLOAD")
                return
            self._send_next_download_chunk()
            return

        if command.startswith("DOWNLOAD DATA"):
            if not session.data_received:
                self._fail_host_transfer("ESP acknowledged download data without a payload", "DOWNLOAD")
                return
            session.sequence += 1
            session.data_received = False
            self.transfer_changed.emit(
                TransferProgress(
                    state=TransferState.VERIFYING if session.verification_source else TransferState.TRANSFERRING,
                    sent_bytes=session.received_bytes,
                    total_bytes=session.expected_size or 0,
                    message="Reading uploaded file back for verification; no nRF24 TX"
                            if session.verification_source else "Downloading received file from ESP storage",
                    source_path=session.verification_source.source_path
                                if session.verification_source else session.destination_path,
                )
            )
            self._send_next_download_chunk()
            return

        if command == console_protocol.download_end_command():
            if not session.complete:
                self._fail_host_transfer("ESP did not complete the download session", "DOWNLOAD")
                return
            if (
                session.expected_size != session.received_bytes
                or session.expected_crc32 != (session.running_crc32 & 0xFFFFFFFF)
            ):
                self._fail_host_transfer("Downloaded file failed size or CRC32 verification", "DOWNLOAD")
                return
            if session.verification_source is not None:
                source = session.verification_source
                returned = session.file.getvalue()
                # Exact byte comparison proves binary preservation; SHA-256 is
                # a reproducible fingerprint for the operator, not an RF ACK.
                if returned != source.data:
                    self._fail_host_transfer("ESP readback differs from the original file bytes", "DOWNLOAD")
                    return
                digest = hashlib.sha256(returned).hexdigest()
                message = ((f"Imported {source.remote_name}: " if source.store_only else "")
                           + f"Verified {len(returned)} bytes uploaded and read back from ESP SPIFFS; "
                           f"byte-for-byte match; CRC32={source.crc32:08X}; SHA256={digest}; no nRF24 TX")
                self._host_transfer_timer.stop()
                self._discard_host_transfer()
                self._snapshot = self._replace_snapshot(
                    connection_state=ConnectionState.CONNECTED,
                    requested_radio_state=self._snapshot.confirmed_radio_state,
                )
                self.status_changed.emit(self._snapshot)
                self._status_poll_timer.start()
                self.log_event.emit("info", message)
                self.transfer_changed.emit(TransferProgress(
                    state=TransferState.COMPLETED, sent_bytes=len(returned), total_bytes=len(returned),
                    message=message, source_path=source.source_path,
                ))
                if source.store_only:
                    self.refresh_storage()
                return
            try:
                if session.file:
                    session.file.flush()
                    os.fsync(session.file.fileno())
                    session.file.close()
                    session.file = None
                os.replace(session.partial_path, session.destination_path)
            except OSError as exc:
                self._fail_host_transfer(f"Could not publish downloaded file: {exc}", "DOWNLOAD")
                return

            self._host_transfer_timer.stop()
            destination = session.destination_path
            total = session.received_bytes
            self._download_session = None
            self._snapshot = self._replace_snapshot(connection_state=ConnectionState.CONNECTED)
            self.status_changed.emit(self._snapshot)
            self._status_poll_timer.start()
            self.transfer_changed.emit(
                TransferProgress(
                    state=TransferState.COMPLETED,
                    sent_bytes=total,
                    total_bytes=total,
                    message=f"Saved received file to {destination}",
                    source_path=destination,
                )
            )

    def _send_next_download_chunk(self) -> None:
        session = self._download_session
        if session is None or session.expected_size is None:
            return
        if session.received_bytes >= session.expected_size:
            self._send_host_command(console_protocol.download_end_command())
            return
        self._send_host_command(console_protocol.download_data_command(session.sequence))

    def _write_loop(self, serial_port: Any, stop_event: threading.Event,
                    write_queue: queue.Queue, generation: int) -> None:
        while not stop_event.is_set():
            try:
                command = write_queue.get(timeout=SERIAL_READ_TIMEOUT_SECONDS)
            except queue.Empty:
                continue
            if command is None:
                return
            if stop_event.is_set():
                return
            try:
                payload = (command + "\r\n").encode("ascii")
                written = serial_port.write(payload)
                if written != len(payload):
                    raise IOError(f"Incomplete serial write ({written}/{len(payload)} bytes)")
                # write_timeout bounds write(); Win32 flush() has no deadline
                # and can indefinitely stall this worker on an SPP driver.
                self._worker_event.emit(generation, "written", command)
            except Exception as exc:
                if stop_event.is_set():
                    return
                self._worker_event.emit(generation, "write_error", (command, f"Serial write failed: {exc}"))
                if command not in {"HELLO", "STATUS"}:
                    return

    def _read_loop(self, serial_port: Any, stop_event: threading.Event, generation: int) -> None:
        buffer = _LineBuffer()
        while not stop_event.is_set():
            try:
                # Block for one byte when idle, then drain bytes already queued
                # by the driver. Do not wait for a full 4 KiB buffer or perform
                # a Win32 read per character through IOBase.readline().
                waiting = serial_port.in_waiting
                raw = serial_port.read(max(1, min(waiting, SERIAL_READ_CHUNK_BYTES)))
            except Exception as exc:
                if stop_event.is_set():
                    return
                self._worker_event.emit(generation, "read_error", f"Serial read failed: {exc}")
                return
            if not raw:
                continue
            for line in buffer.feed(raw):
                if stop_event.is_set():
                    return
                self._worker_event.emit(generation, "line", line)

    def _on_read_failed(self, message: str) -> None:
        if self._stop_event.is_set():
            return
        was_connecting = self._snapshot.connection_state == ConnectionState.CONNECTING
        self._handshake_timer.stop()
        self._handshake_status_timer.stop()
        self._status_poll_timer.stop()
        self._close_serial_port()
        self._snapshot = DeviceSnapshot(connection_state=ConnectionState.ERROR, connection_detail=message)
        self.status_changed.emit(self._snapshot)
        self.log_event.emit("error", message)
        if was_connecting:
            self.command_finished.emit("CONNECT", False, message)

    def _on_write_failed(self, command: str, message: str) -> None:
        if self._stop_event.is_set():
            return
        if (
            command in {"HELLO", console_protocol.status_command()}
            and not self._handshake_ok
            and self._snapshot.connection_state == ConnectionState.CONNECTING
        ):
            if command == "HELLO":
                self._hello_sent = False
            if not self._handshake_write_warning_shown:
                self.log_event.emit(
                    "warning",
                    "Handshake write timed out. Retrying until the Bluetooth/serial handshake timeout.",
                )
                self._handshake_write_warning_shown = True
            return
        self._close_serial_port()
        self._snapshot = DeviceSnapshot(connection_state=ConnectionState.ERROR, connection_detail=message)
        self.status_changed.emit(self._snapshot)
        self.command_finished.emit(command, False, message)

    def _close_serial_port(self) -> None:
        # Signal handlers may open modal dialogs and run a nested Qt event loop.
        # Invalidate the entire connection/operation state before any signal can
        # re-enter a backend method or an old worker event can arrive.
        serial_port = self._serial
        stop_event = self._stop_event
        write_queue = self._write_queue
        unresolved = list(self._pending_commands)
        had_echo = self._awaiting_echo is not None
        had_test_tx = self._awaiting_test_tx_count is not None
        had_rf = self._rf_tx_name is not None
        had_host = self._upload_session is not None or self._download_session is not None
        self._setup_cancel.set()
        self._connection_generation += 1
        self._serial = None
        self._handshake_ok = False
        self._identity_ok = False
        self._handshake_write_pending = False
        stop_event.set()
        for timer in (self._handshake_timer, self._setup_timer, self._handshake_status_timer,
                      self._status_poll_timer, self._status_response_timer, self._test_tx_timer,
                      self._test_mode_timer, self._text_echo_timer, self._host_transfer_timer,
                      self._storage_timer, self._command_timer, self._rf_progress_timer,
                      self._rx_cancel_timer):
            timer.stop()
        self._command_deadlines.clear()
        self._pending_commands.clear()
        self._awaiting_echo = None
        self._awaiting_test_tx_count = None
        self._awaiting_test_mode = None
        self._rf_tx_name = None
        self._rf_tx_id = ""
        self._rf_progress_marker = None
        self._last_tx_id = ""
        self._observed_rf_id = ""
        self._rx_cancel_requested = False
        self._pending_selection_name = None
        self._storage_kind = None
        self._storage_delete_name = None
        self._storage_files = []
        self._storage_offset = 0
        self._storage_total = None
        self._storage = StorageListing()
        self._discard_host_transfer()
        self._snapshot = DeviceSnapshot(connection_state=ConnectionState.DISCONNECTED,
                                        connection_detail="Connection closed")
        self._reader_thread = None
        self._writer_thread = None
        write_queue.put(None)
        close_error = ""
        if serial_port is not None:
            try:
                for operation in ("cancel_read", "cancel_write"):
                    cancel = getattr(serial_port, operation, None)
                    if cancel is not None:
                        try:
                            cancel()
                        except Exception:
                            pass
                serial_port.close()
            except Exception as exc:
                close_error = f"Serial close failed: {exc}"

        # All callbacks below now see a fully detached, cleaned-up connection.
        self.storage_changed.emit(self._storage)
        if had_rf:
            self.transfer_changed.emit(TransferProgress(state=TransferState.FAILED,
                                                        message="Connection closed; RF delivery is unconfirmed"))
        if had_host:
            self.transfer_changed.emit(TransferProgress(state=TransferState.FAILED,
                                                        message="Connection closed during file transfer"))
        if had_test_tx:
            self.transfer_changed.emit(TransferProgress(state=TransferState.FAILED,
                                                        message="Connection closed before simulated TX was confirmed"))
        for command in unresolved:
            if command != "STATUS":
                message = ("Connection closed before ESP text echo was verified" if command.startswith("ECHO ")
                           else "Connection closed before command completion")
                self.command_finished.emit(command, False, message)
        if had_echo and not any(command.startswith("ECHO ") for command in unresolved):
            self.command_finished.emit("ECHO", False, "Connection closed before ESP text echo was verified")
        if close_error:
            self.log_event.emit("error", close_error)

    def _handle_line(self, line: str) -> None:
        if self._serial is None:
            return
        line = self._clean_console_line(line)
        if not line:
            return

        if self._pending_commands and line == self._pending_commands[0]:
            return

        self.log_event.emit("debug", f"RX <- {line}")

        if self._handle_host_transfer_line(line):
            return

        if line.startswith("{"):
            try:
                values = json.loads(line)
            except json.JSONDecodeError as exc:
                self.log_event.emit("warning", f"Ignored malformed JSON status: {exc}")
                return
            if isinstance(values, dict):
                if "storage" in values:
                    self._handle_storage_response(values["storage"])
                    return
                if "rf3_protocol" in values:
                    self._apply_identity(values)
                    return
                status_values = values.get("status", values)
                if isinstance(status_values, dict):
                    try:
                        self._apply_status(status_values)
                    except (ValueError, TypeError, OverflowError):
                        self.log_event.emit("warning", "Ignored invalid numeric fields in RF3 status")
                if isinstance(values.get("accepted"), bool) and isinstance(values.get("command"), str):
                    self._finish_pending(bool(values["accepted"]), line, values["command"])
            return

        status_start = line.find("profile=")
        if status_start >= 0:
            try:
                self._apply_status(parse_status_line(line[status_start:]))
            except (ValueError, TypeError, OverflowError):
                self.log_event.emit("warning", "Ignored invalid numeric fields in RF3 status")
                return
            return

        if self._apply_known_response(line):
            return

        if _looks_like_error(line):
            if line.startswith("TESTTX"):
                self._fail_test_tx(line)
            expected = "TESTMODE" if "TESTMODE" in line else "TESTTX" if "TESTTX" in line else None
            if expected is not None:
                self._finish_pending(False, line, expected)
            else:
                self._on_read_failed("Unscoped firmware error: " + line)

    def _handle_host_transfer_line(self, line: str) -> bool:
        if line.startswith("UPLOAD ERROR"):
            self._finish_pending(False, line, "UPLOAD")
            return True
        if line.startswith("DOWNLOAD ERROR"):
            self._finish_pending(False, line, "DOWNLOAD")
            return True

        upload = self._upload_session
        if upload is not None and line.startswith("UPLOAD READY "):
            parts = line.split()
            try:
                valid = (
                    len(parts) == (6 if upload.chunk_bytes != HOST_TRANSFER_CHUNK_BYTES else 5)
                    and parts[2] == upload.remote_name
                    and int(parts[3], 10) == len(upload.data)
                    and int(parts[4], 16) == upload.crc32
                    and (upload.chunk_bytes == HOST_TRANSFER_CHUNK_BYTES or int(parts[5], 10) == upload.chunk_bytes)
                )
            except ValueError:
                valid = False
            if not valid:
                self._finish_pending(False, "Invalid UPLOAD READY response", "UPLOAD BEGIN")
                return True
            upload.ready = True
            self._finish_pending(True, line, "UPLOAD BEGIN")
            return True

        if upload is not None and line.startswith("UPLOAD ACK "):
            parts = line.split()
            try:
                sequence = int(parts[2], 10)
                total = int(parts[3], 10)
            except (IndexError, ValueError):
                self._finish_pending(False, "Invalid UPLOAD ACK response", "UPLOAD DATA")
                return True
            if sequence < upload.sequence:
                return True  # Late acknowledgement of a chunk already advanced.
            upload.acknowledged_sequence = sequence
            upload.acknowledged_total = total
            self._finish_pending(True, line, f"UPLOAD DATA {sequence}")
            return True

        if upload is not None and line.startswith("UPLOAD COMPLETE "):
            parts = line.split()
            try:
                valid = (
                    len(parts) == 5
                    and parts[2] == upload.remote_name
                    and int(parts[3], 10) == len(upload.data)
                    and int(parts[4], 16) == upload.crc32
                )
            except ValueError:
                valid = False
            if not valid:
                self._finish_pending(False, "Invalid UPLOAD COMPLETE response", "UPLOAD END")
                return True
            upload.complete = True
            self._finish_pending(True, line, "UPLOAD END")
            return True

        download = self._download_session
        if download is not None and line.startswith(("DOWNLOAD READY ", "DOWNLOAD READYHEX ")):
            parts = line.split()
            try:
                name = bytes.fromhex(parts[2]).decode("utf-8") if parts[1] == "READYHEX" else parts[2]
                size = int(parts[3], 10)
                crc32 = int(parts[4], 16)
                valid = (
                    len(parts) == (6 if download.chunk_bytes != HOST_TRANSFER_CHUNK_BYTES else 5)
                    and name == download.source_name
                    and 0 <= size <= PROTOCOL_V2_MAX_FILE_SIZE
                    and 0 <= crc32 <= 0xFFFFFFFF
                    and (download.chunk_bytes == HOST_TRANSFER_CHUNK_BYTES or int(parts[5], 10) == download.chunk_bytes)
                )
            except (IndexError, ValueError, UnicodeError):
                valid = False
                size = 0
                crc32 = 0
            if not valid:
                self._finish_pending(False, "Invalid DOWNLOAD READY response", "DOWNLOAD BEGINHEX" if download.encoded_name else "DOWNLOAD BEGIN")
                return True
            download.expected_size = size
            download.expected_crc32 = crc32
            download.ready = True
            self._finish_pending(True, line, "DOWNLOAD BEGINHEX" if download.encoded_name else "DOWNLOAD BEGIN")
            return True

        if download is not None and line.startswith("DOWNLOAD DATA "):
            parts = line.split()
            try:
                sequence = int(parts[2], 10)
                payload = bytes.fromhex(parts[3])
            except (IndexError, ValueError):
                self._finish_pending(False, "Invalid DOWNLOAD DATA response", "DOWNLOAD DATA")
                return True
            if sequence < download.sequence:
                return True  # Original response may arrive after its retry.
            expected_size = download.expected_size
            expected_chunk = (
                min(download.chunk_bytes, expected_size - download.received_bytes)
                if expected_size is not None
                else -1
            )
            if (
                sequence != download.sequence
                or download.data_received
                or len(payload) != expected_chunk
                or download.file is None
            ):
                self._finish_pending(False, "Unexpected DOWNLOAD DATA response", "DOWNLOAD DATA")
                return True
            try:
                download.file.write(payload)
            except OSError as exc:
                self._finish_pending(False, f"Could not write download: {exc}", "DOWNLOAD DATA")
                return True
            download.running_crc32 = binascii.crc32(payload, download.running_crc32)
            download.received_bytes += len(payload)
            download.data_received = True
            self._finish_pending(True, line, f"DOWNLOAD DATA {sequence}")
            return True

        if download is not None and line.startswith(("DOWNLOAD COMPLETE ", "DOWNLOAD COMPLETEHEX ")):
            parts = line.split()
            try:
                name = bytes.fromhex(parts[2]).decode("utf-8") if parts[1] == "COMPLETEHEX" else parts[2]
                valid = (
                    len(parts) == 5
                    and name == download.source_name
                    and int(parts[3], 10) == download.expected_size
                    and int(parts[4], 16) == download.expected_crc32
                )
            except (IndexError, ValueError, TypeError, UnicodeError):
                valid = False
            if not valid:
                self._finish_pending(False, "Invalid DOWNLOAD COMPLETE response", "DOWNLOAD END")
                return True
            download.complete = True
            self._finish_pending(True, line, "DOWNLOAD END")
            return True

        return line.startswith(("UPLOAD ", "DOWNLOAD "))

    def _apply_status(self, values: dict[str, Any]) -> None:
        state = _radio_state_from_text(str(values.get("State") or values.get("state") or "Unknown"))
        if self._serial is not None and self._identity_ok and values.get("state") == "Unavailable":
            # A verified RF3 host response can remain usable while another
            # operation holds the radio mutex. Never present a stale radio state.
            rx_fields = self._rx_status_fields(values)
            self._status_response_timer.stop()
            self._finish_pending(True, "RF3 connected; radio status unavailable", "STATUS")
            self._snapshot = self._replace_snapshot(
                confirmed_radio_state=RadioState.UNKNOWN,
                power_level=None,
                connection_state=(ConnectionState.BUSY if self._local_operation_active(include_rx=False)
                                  or rx_fields["rx_transfer_state"] in RX_ACTIVE_STATES else ConnectionState.CONNECTED),
                connection_detail="RF3 connected; radio status temporarily unavailable. Other readings are last reported.",
                **rx_fields,
            )
            self.status_changed.emit(self._snapshot)
            return
        if self._serial is None or state == RadioState.UNKNOWN:
            self.log_event.emit("warning", "Ignored status without a recognized RF3 radio state")
            return
        for key in ("selected", "tx_file", "rx_last", "rx_last_path"):
            if key in values and not isinstance(values[key], str):
                raise ValueError(f"Invalid textual {key}")
        for key in ("fault", "last_fault", "rx_packets", "rx_saved", "rx_saved_bytes",
                    "test_tx_count", "test_tx_bytes"):
            if key in values:
                raw = values[key]
                if isinstance(raw, bool) or not isinstance(raw, (int, str)):
                    raise ValueError(f"Invalid {key}")
                int(raw)
        rx_fields = self._rx_status_fields(values)
        confirmed_test_request = (self._awaiting_test_mode is not None and "test_mode" in values
                                  and self._awaiting_test_mode == _status_enabled(values["test_mode"]))
        if confirmed_test_request:
            self._awaiting_test_mode = None
            self._test_mode_timer.stop()
        tx_state_text = str(values.get("tx_state") or "")
        loop_text = str(values.get("loop") or "")
        connection_state = (
            ConnectionState.BUSY
            if state == RadioState.TX_BUSY
            or state == RadioState.CW_TEST
            or tx_state_text in {"Preparing", "WaitingForReady", "SendingData", "WaitingForDataAck", "WaitingForComplete", "Cancelling"}
            or rx_fields["rx_transfer_state"] in RX_ACTIVE_STATES
            or loop_text not in {"", "None", "Idle"}
            else ConnectionState.CONNECTED
        )
        if self._local_operation_active(include_rx=False):
            connection_state = ConnectionState.BUSY
        telemetry = Telemetry(
            battery_percent=None,
            last_fault=int(values.get("fault", values.get("last_fault", 0)) or 0),
            last_tx_ok=_status_enabled(values.get("tx_ok")),
            tx_timeout=_status_enabled(values.get("tx_timeout")),
            rx_packets=int(values.get("rx_packets", 0) or 0),
            rx_saved=int(values.get("rx_saved", self._snapshot.telemetry.rx_saved) or 0),
            rx_saved_bytes=int(values.get("rx_saved_bytes", self._snapshot.telemetry.rx_saved_bytes) or 0),
            last_rx_file=values.get("rx_last", self._snapshot.telemetry.last_rx_file),
            last_rx_path=values.get("rx_last_path", self._snapshot.telemetry.last_rx_path),
            rpd=_status_enabled(values.get("rpd")),
        )
        capabilities = RadioCapabilities(
            host_chunk_max=_negotiated_host_chunk(values.get("host_chunk_max")),
            supports_host_storage=_status_enabled(values.get("host_storage")),
            supports_battery_percent=False,
            supports_host_file_upload=_status_enabled(values.get("host_upload")),
            supports_host_file_download=_status_enabled(values.get("host_download")),
            supports_host_chunk_retry=_status_enabled(values.get("host_retry")),
            supports_remote_commands=_status_enabled(values.get("remote")),
            supports_text_messages=_status_enabled(values.get("text")),
            supports_host_echo=_status_enabled(values.get("host_echo")),
            supports_test_mode=_status_enabled(values.get("test_mode_supported")),
            supports_stored_file_tx=_status_enabled(values.get("host_stored_tx")),
        )
        power = values.get("power")
        power_level = power if type(power) is int and power in (0, 1, 2, 3) else None
        channel = values.get("channel")
        self._handshake_ok = True
        self._status_response_timer.stop()
        self._finish_pending(True, "Valid RF3 telemetry received", "STATUS")
        completing_handshake = self._snapshot.connection_state == ConnectionState.CONNECTING
        if completing_handshake:
            self._handshake_timer.stop()
            self._handshake_status_timer.stop()
            self._status_poll_timer.start()

        self._snapshot = self._replace_snapshot(
            connection_state=connection_state,
            connection_detail="RF3 connected; radio status received",
            test_mode=str(values.get("test_mode", "off")).lower() in {"on", "true"},
            test_tx_count=int(values.get("test_tx_count", 0) or 0),
            test_tx_bytes=int(values.get("test_tx_bytes", 0) or 0),
            confirmed_radio_state=state,
            channel=channel if type(channel) is int and 0 <= channel <= 125 else self._snapshot.channel,
            power_level=power_level,
            selected_file=values.get("selected", self._snapshot.selected_file),
            profile=_status_text(values.get("profile")) or self._snapshot.profile,
            active_tx_file=values.get("tx_file", self._snapshot.active_tx_file),
            telemetry=telemetry,
            capabilities=capabilities,
            **rx_fields,
        )
        self.status_changed.emit(self._snapshot)
        if confirmed_test_request:
            self._finish_pending(True, "ESP STATUS confirmed requested test mode", "TESTMODE")
        self._apply_transfer_status(values)

        test_tx_count = int(values.get("test_tx_count", 0) or 0)
        if (self._awaiting_test_tx_count is not None
                and test_tx_count >= self._awaiting_test_tx_count
                and self._snapshot.test_mode
                and values.get("test_tx_state") == "Completed"):
            size = int(values.get("test_tx_bytes", 0) or 0)
            self._awaiting_test_tx_count = None
            self._test_tx_timer.stop()
            self.transfer_changed.emit(
                TransferProgress(
                    state=TransferState.COMPLETED,
                    sent_bytes=size,
                    total_bytes=size,
                    message="ESP acknowledged simulated TX; no nRF24 TX or peer handshake occurred",
                )
            )
        self._last_test_tx_count = test_tx_count
        if completing_handshake:
            self.command_finished.emit("CONNECT", True, "RF3 STATUS handshake received")

    def _apply_identity(self, values: dict[str, Any]) -> None:
        if (self._serial is None or type(values.get("rf3_protocol")) is not int
                or values["rf3_protocol"] != 1 or type(values.get("protocol")) is not int
                or values["protocol"] != 2
                or values.get("profile") not in ("custom-pcb", "esp32-devboard")
                or not isinstance(values.get("node_name"), str) or not values["node_name"]):
            self.log_event.emit("warning", "Ignored unsupported or incomplete RF3 identity")
            return
        self._identity_ok = True
        if self._handshake_ok:
            return
        self._handshake_ok = True
        self._setup_timer.stop()
        self._handshake_timer.stop()
        self._handshake_status_timer.stop()
        self._status_poll_timer.start()
        self._snapshot = self._replace_snapshot(
            connection_state=ConnectionState.CONNECTED,
            connection_detail=f"RF3 {values['node_name']} connected; waiting for radio status",
            profile=values["profile"],
        )
        self.status_changed.emit(self._snapshot)
        self.command_finished.emit("CONNECT", True, "RF3 HELLO identity received")

    def _on_test_tx_timeout(self) -> None:
        if self._awaiting_test_tx_count is not None:
            self._fail_test_tx("ESP did not confirm simulated TX in STATUS")

    def _on_test_mode_timeout(self) -> None:
        if self._awaiting_test_mode is not None:
            self._awaiting_test_mode = None
            self.command_finished.emit("TESTMODE", False, "ESP did not confirm test mode in STATUS")

    def _fail_test_tx(self, message: str) -> None:
        if self._awaiting_test_tx_count is None:
            return
        self._awaiting_test_tx_count = None
        self._test_tx_timer.stop()
        self.transfer_changed.emit(TransferProgress(state=TransferState.FAILED, message=message))

    def _on_rx_cancel_timeout(self) -> None:
        if self._rx_cancel_requested:
            self._on_read_failed("ESP did not confirm receive cancellation; reconnect before changing settings")

    def _incoming_rx_active(self) -> bool:
        return self._snapshot.rx_transfer_state in RX_ACTIVE_STATES

    def _local_operation_active(self, *, include_rx: bool = True) -> bool:
        return bool(self._upload_session is not None or self._download_session is not None
                    or self._storage_kind is not None or self._awaiting_echo is not None
                    or self._awaiting_test_mode is not None or self._rf_tx_name is not None
                    or (include_rx and self._incoming_rx_active()))

    def _rx_status_fields(self, values: dict[str, Any]) -> dict[str, Any]:
        # Incoming progress has its own snapshot fields: it must never complete,
        # reset or replace the outgoing RF/PC transfer progress stream.
        previous = {
            "rx_transfer_state": self._snapshot.rx_transfer_state,
            "rx_transfer_id": self._snapshot.rx_transfer_id,
            "rx_received_bytes": self._snapshot.rx_received_bytes,
            "rx_total_bytes": self._snapshot.rx_total_bytes,
            "rx_error": self._snapshot.rx_error,
        }
        if "rx_state" not in values:
            if (self._rx_cancel_requested and previous["rx_transfer_state"] in RX_ACTIVE_STATES
                    and values.get("State", values.get("state")) == "Standby"):
                self._rx_cancel_requested = False
                self._rx_cancel_timer.stop()
                previous["rx_transfer_state"] = "Cancelled"
                previous["rx_error"] = "Receive stopped; no verified receive-completion metadata returned"
            return previous
        state = values["rx_state"]
        if not isinstance(state, str) or state not in RX_ACTIVE_STATES | RX_TERMINAL_STATES | {"Idle"}:
            raise ValueError("Invalid receiver state")
        if state == "Idle":
            idle_id = values.get("rx_id", "00000000")
            idle_bytes = parse_fraction(values.get("rx_bytes", "0/0"))
            if (not isinstance(idle_id, str) or re.fullmatch(r"[0-9A-Fa-f]{8}", idle_id) is None
                    or idle_bytes is None or not 0 <= idle_bytes[0] <= idle_bytes[1] <= PROTOCOL_V2_MAX_FILE_SIZE
                    or not isinstance(values.get("rx_error", "none"), str)):
                raise ValueError("Invalid idle receiver metadata")
            # Preserve terminal results until the next manifest. If an active
            # receiver returns to Idle without a result, expose interruption.
            if previous["rx_transfer_state"] in RX_TERMINAL_STATES:
                return previous
            if previous["rx_transfer_state"] in RX_ACTIVE_STATES:
                previous["rx_transfer_state"] = "Cancelled" if (
                    self._rx_cancel_requested or "STOP" in self._pending_commands) else "Failed"
                self._rx_cancel_requested = False
                self._rx_cancel_timer.stop()
                previous["rx_error"] = "Receiver returned to idle before file completion"
                return previous
            return dict(rx_transfer_state="Idle", rx_transfer_id="", rx_received_bytes=0,
                        rx_total_bytes=0, rx_error="")
        identifier = values.get("rx_id")
        parsed = parse_fraction(values.get("rx_bytes"))
        error = values.get("rx_error", "none")
        if (not isinstance(identifier, str) or re.fullmatch(r"[0-9A-Fa-f]{8}", identifier) is None
                or int(identifier, 16) == 0 or parsed is None
                or not 0 <= parsed[0] <= parsed[1] <= PROTOCOL_V2_MAX_FILE_SIZE
                or not isinstance(error, str)):
            raise ValueError("Invalid receiver progress metadata")
        identifier = identifier.upper()
        received, total = parsed
        inconsistent_complete = state == "Completed" and (
            received != total or error.lower() not in {"", "none", "0"})
        if identifier == previous["rx_transfer_id"]:
            if previous["rx_transfer_state"] in RX_TERMINAL_STATES:
                return previous  # Duplicate/late active snapshots cannot revive it.
            if total != previous["rx_total_bytes"]:
                raise ValueError("Receiver size changed within one transfer")
            if state in RX_ACTIVE_STATES:
                received = max(received, previous["rx_received_bytes"])
                phases = ("Receiving", "WaitingForEnd", "Verifying", "Publishing")
                if (previous["rx_transfer_state"] in RX_ACTIVE_STATES
                        and phases.index(previous["rx_transfer_state"]) > phases.index(state)):
                    state = previous["rx_transfer_state"]
        if inconsistent_complete:
            if self._rx_cancel_requested:
                raise ValueError("Inconsistent receive completion cannot confirm cancellation")
            state = "Failed"
            error = "Inconsistent receive completion metadata; file verification is unconfirmed"
        if state in RX_TERMINAL_STATES:
            if self._rx_cancel_requested and identifier != previous["rx_transfer_id"]:
                raise ValueError("Unrelated receive result cannot confirm cancellation")
            # Retire the bounded cancellation only after all metadata and
            # same-session invariants passed. Malformed replies must leave the
            # existing guard/deadline intact.
            self._rx_cancel_requested = False
            self._rx_cancel_timer.stop()
        return dict(rx_transfer_state=state, rx_transfer_id=identifier,
                    rx_received_bytes=received, rx_total_bytes=total, rx_error=_status_text(error))

    def _apply_transfer_status(self, values: dict[str, Any]) -> None:
        if self._snapshot.test_mode or self._upload_session is not None or self._download_session is not None:
            return  # RF transfer telemetry cannot complete a host-file check.
        tx_state = str(values.get("tx_state") or "")
        tx_id = str(values.get("tx_id") or "")
        tx_file = values.get("tx_file", "")
        if tx_id:
            self._last_tx_id = tx_id
        if self._rf_tx_name is not None:
            if not tx_state or not tx_id or "peer_complete" not in values:
                self._finish_rf_transfer(TransferState.FAILED,
                    "Firmware does not report complete RF progress; delivery is unconfirmed. Flash matching firmware")
                return
            if not self._rf_tx_id:
                # STATUS queued before TX may describe the previous transfer.
                if tx_id == self._rf_tx_baseline_id or tx_state == "Idle":
                    return
                if tx_file != self._rf_tx_name and tx_state in {"Completed", "Failed", "Cancelled"}:
                    return  # Delayed previous-transfer terminal snapshot.
                if tx_file != self._rf_tx_name:
                    self._finish_rf_transfer(TransferState.FAILED,
                                            "ESP active TX file differs from the requested SPIFFS file")
                    return
                self._rf_tx_id = tx_id
            elif tx_id != self._rf_tx_id or tx_file != self._rf_tx_name:
                self._finish_rf_transfer(TransferState.FAILED,
                                        "ESP transfer identity changed; delivery of the requested file is unconfirmed")
                return
        elif not tx_state or tx_state == "Idle":
            return
        parsed = parse_fraction(values.get("tx_bytes"))
        if parsed is None or not 0 <= parsed[0] <= parsed[1] <= PROTOCOL_V2_MAX_FILE_SIZE:
            if self._rf_tx_name is not None:
                self._finish_rf_transfer(TransferState.FAILED, "Invalid RF progress metadata; delivery is unconfirmed")
            return
        sent, total = parsed
        transfer_state = {
            "Preparing": TransferState.PREPARING,
            "WaitingForReady": TransferState.HANDSHAKING,
            "SendingData": TransferState.TRANSFERRING,
            "WaitingForDataAck": TransferState.TRANSFERRING,
            "WaitingForComplete": TransferState.VERIFYING,
            "Completed": TransferState.COMPLETED,
            "Cancelling": TransferState.CANCELLING,
            "Cancelled": TransferState.CANCELLED,
            "Failed": TransferState.FAILED,
        }.get(tx_state)
        if transfer_state is None:
            if self._rf_tx_name is not None:
                self._finish_rf_transfer(TransferState.FAILED, "Unknown RF state; delivery is unconfirmed")
            return
        error = _status_text(values.get("tx_error"))
        if tx_state == "Completed":
            if values.get("peer_complete") is not True:
                transfer_state = TransferState.FAILED
                error = "ESP completed locally but peer delivery was not confirmed"
            elif sent != total or (error and error.lower() not in {"none", "0"}):
                transfer_state = TransferState.FAILED
                error = "Inconsistent RF completion metadata; delivery is unconfirmed" + (": " + error if error else "")
        message = f"RF {tx_id or 'legacy'} | {tx_file or self._snapshot.active_tx_file} | {tx_state}"
        if error and error.lower() not in {"none", "0"}:
            message += ": " + error
        if transfer_state == TransferState.COMPLETED:
            message += "; peer confirmed CRC/complete"
        if transfer_state in {TransferState.COMPLETED, TransferState.FAILED, TransferState.CANCELLED}:
            if self._rf_tx_name is not None:
                self._finish_rf_transfer(transfer_state, message, sent, total)
            elif tx_id and tx_id == self._observed_rf_id:
                self._observed_rf_id = ""
                self.transfer_changed.emit(TransferProgress(state=transfer_state, sent_bytes=sent,
                                                            total_bytes=total, message=message))
            return
        self._observed_rf_id = tx_id
        marker = (tx_id, tx_state, sent, total, values.get("tx_seq"), values.get("tx_retries"))
        if self._rf_tx_name is not None and marker != self._rf_progress_marker:
            self._rf_progress_marker = marker
            self._rf_progress_timer.start(RF_PROGRESS_TIMEOUT_MS)
        self.transfer_changed.emit(TransferProgress(state=transfer_state, sent_bytes=sent,
                                                    total_bytes=total, message=message))

    def _apply_known_response(self, line: str) -> bool:
        if line.startswith("ECHO TEXT: "):
            if self._awaiting_echo is not None:
                if line[len("ECHO TEXT: "):] != self._awaiting_echo:
                    self._on_read_failed("ESP text echo did not match the sent text; reconnect")
                else:
                    self._finish_text_echo(True, "ESP echoed the exact text over the host connection; no nRF24 TX")
            return True
        if line.startswith("ECHO ERROR") or line.startswith("ERR ECHO"):
            self._finish_text_echo(False, line)
            return True
        if line.startswith("OK ECHO"):
            return True  # Generic acceptance is not proof of returned content.
        state: RadioState | None = None
        expected: str | None = None
        if line.startswith("Test mode enabled;") or line.startswith("Test mode disabled;"):
            self._finish_pending(True, line, "TESTMODE")
            self.request_status()
            return True
        if line.startswith("TESTTX accepted bytes="):
            self._finish_pending(True, line, "TESTTX")
            self.request_status()
            return True
        if line == "OK STATUS":
            return True  # Acceptance is not usable telemetry; keep the original deadline.
        if line.startswith("OK "):
            command = line[3:]
            if command.startswith("TEXT "):
                self.text_message_received.emit("tx", command[len("TEXT "):])
            self._finish_pending(True, line, command)
            if command.startswith(("TESTMODE", "TESTTX", "TEXT ")):
                self.request_status()
            return True
        if line.startswith("ERR "):
            if line[4:].startswith("TESTTX"):
                self._fail_test_tx(line)
            token = line[4:].split(" ", 1)[0]
            known = {item.split(" ", 1)[0] for item in self._pending_commands}
            if token in known:
                self._finish_pending(False, line, token)
            else:
                self._on_read_failed("Unscoped firmware error: " + line)
            return True
        if line.startswith("TEXT RX: "):
            self.text_message_received.emit("rx", line[len("TEXT RX: "):])
            return True
        if line.startswith("TEXT TX: "):
            self.text_message_received.emit("tx", line[len("TEXT TX: "):])
            self._finish_pending(True, line, "TEXT")
            self.request_status()
            return True
        if match := re.search(r"RX listening on channel (\d+)", line):
            expected = "RX"
            state = RadioState.RX_LISTENING
            self._snapshot = self._replace_snapshot(
                connection_state=ConnectionState.CONNECTED,
                confirmed_radio_state=state,
                channel=int(match.group(1)),
            )
        elif match := re.search(r"Radio is in standby on channel (\d+)", line):
            expected = "STANDBY"
            state = RadioState.STANDBY
            self._snapshot = self._replace_snapshot(
                connection_state=ConnectionState.CONNECTED,
                confirmed_radio_state=state,
                channel=int(match.group(1)),
            )
        elif "Stop complete. Radio is in standby." in line or "Radio is already in standby." in line:
            expected = "STOP"
            state = RadioState.STANDBY
            self._snapshot = self._replace_snapshot(
                connection_state=ConnectionState.CONNECTED,
                confirmed_radio_state=state,
            )
        elif "Radio is sleeping." in line:
            expected = "SLEEP"
            state = RadioState.SLEEP
            self._snapshot = self._replace_snapshot(
                connection_state=ConnectionState.CONNECTED,
                confirmed_radio_state=state,
            )
        elif "Radio is awake in standby." in line:
            expected = "WAKE"
            state = RadioState.STANDBY
            self._snapshot = self._replace_snapshot(
                connection_state=ConnectionState.CONNECTED,
                confirmed_radio_state=state,
            )
        elif "Radio is powered down." in line:
            expected = "POWERDOWN"
            state = RadioState.POWER_DOWN
            self._snapshot = self._replace_snapshot(
                connection_state=ConnectionState.CONNECTED,
                confirmed_radio_state=state,
            )
        elif match := re.search(r"Radio reinitialized on channel (\d+)", line):
            expected = "CHANNEL"
            state = RadioState.STANDBY
            self._snapshot = self._replace_snapshot(
                connection_state=ConnectionState.CONNECTED,
                confirmed_radio_state=state,
                channel=int(match.group(1)),
            )
        elif match := re.search(r"Packet TX power set to (\d+) on channel (\d+)", line):
            expected = "POWER"
            state = self._snapshot.confirmed_radio_state
            self._snapshot = self._replace_snapshot(
                connection_state=ConnectionState.CONNECTED,
                power_level=int(match.group(1)),
                channel=int(match.group(2)),
            )
        elif line.startswith("TX started for "):
            expected = "TX"
            state = RadioState.TX_BUSY
            self._snapshot = self._replace_snapshot(
                connection_state=ConnectionState.BUSY,
                confirmed_radio_state=state,
            )
        elif match := re.search(r"Morse started on channel (\d+)", line):
            expected = "MORSE"
            state = RadioState.TX_BUSY
            self._snapshot = self._replace_snapshot(
                connection_state=ConnectionState.BUSY,
                confirmed_radio_state=state,
                channel=int(match.group(1)),
            )
        elif match := re.search(r"CW mode active on channel (\d+) at power level (\d+)", line):
            expected = "CW"
            state = RadioState.CW_TEST
            self._snapshot = self._replace_snapshot(
                connection_state=ConnectionState.BUSY,
                confirmed_radio_state=state,
                channel=int(match.group(1)),
                power_level=int(match.group(2)),
            )
        elif line.startswith("Sent remote command:"):
            self._finish_pending(True, line, "REMOTE")
            return True
        else:
            return False

        self.status_changed.emit(self._snapshot)
        self._finish_pending(True, line, expected)
        return True

    def _finish_pending(self, accepted: bool, message: str, expected_prefix: str | None = None) -> None:
        if not self._pending_commands:
            return
        if expected_prefix is None:
            return  # Unscoped/unsolicited replies cannot acknowledge unrelated actions.
        command = next((item for item in self._pending_commands
                        if item == expected_prefix or item.startswith(expected_prefix + " ")), None)
        if command is None:
            return
        self._pending_commands.remove(command)
        self._command_deadlines.pop(command, None)
        if command.startswith("SELECTHEX"):
            name = self._pending_selection_name
            self._pending_selection_name = None
            if accepted and name is not None:
                self._snapshot = self._replace_snapshot(selected_file=name)
                self.status_changed.emit(self._snapshot)
        if command.startswith(("TX ", "TXHEX ")) and not accepted and self._rf_tx_name is not None:
            self._finish_rf_transfer(TransferState.FAILED, message)
        is_host_command = command.startswith(("UPLOAD ", "DOWNLOAD "))
        if is_host_command:
            self._host_transfer_timer.stop()
        self._host_command_finished(command, accepted, message)
        if not is_host_command:
            self.command_finished.emit(command, accepted, message)
        if command.startswith("TESTMODE") and not accepted:
            self._awaiting_test_mode = None
            self._test_mode_timer.stop()

    @staticmethod
    def _clean_console_line(line: str) -> str:
        line = line.strip()
        while line.startswith("rf24>"):
            line = line[len("rf24>"):].lstrip()
        return line  # Preserve prompt-like text inside message/echo contents.

    def _replace_snapshot(self, **changes) -> DeviceSnapshot:
        values = {
            "connection_state": self._snapshot.connection_state,
            "connection_detail": self._snapshot.connection_detail,
            "test_mode": self._snapshot.test_mode,
            "test_tx_count": self._snapshot.test_tx_count,
            "test_tx_bytes": self._snapshot.test_tx_bytes,
            "requested_radio_state": self._snapshot.requested_radio_state,
            "confirmed_radio_state": self._snapshot.confirmed_radio_state,
            "channel": self._snapshot.channel,
            "power_level": self._snapshot.power_level,
            "selected_file": self._snapshot.selected_file,
            "profile": self._snapshot.profile,
            "active_tx_file": self._snapshot.active_tx_file,
            "rx_transfer_state": self._snapshot.rx_transfer_state,
            "rx_transfer_id": self._snapshot.rx_transfer_id,
            "rx_received_bytes": self._snapshot.rx_received_bytes,
            "rx_total_bytes": self._snapshot.rx_total_bytes,
            "rx_error": self._snapshot.rx_error,
            "telemetry": self._snapshot.telemetry,
            "capabilities": self._snapshot.capabilities,
        }
        values.update(changes)
        # Known console ACKs and unrelated host-operation completions must not
        # clear the incoming-file guard before terminal RX telemetry arrives.
        if (self._serial is not None and values["connection_state"] == ConnectionState.CONNECTED
                and values["rx_transfer_state"] in RX_ACTIVE_STATES):
            values["connection_state"] = ConnectionState.BUSY
        return DeviceSnapshot(**values)


def _radio_state_from_text(text: str) -> RadioState:
    for state in RadioState:
        if state.value == text:
            return state
    return RadioState.UNKNOWN


def _is_bluetooth_serial(device: DeviceInfo) -> bool:
    return bool(device.bluetooth_address) or device.transport == "bluetooth" or _is_bluetooth_port(device.name, device.address)


def _is_bluetooth_port(description: str, hwid: str) -> bool:
    text = f"{description} {hwid}".lower()
    return "bluetooth" in text or "bthenum" in text


def _is_windows_incoming_bluetooth_port(description: str, hwid: str) -> bool:
    text = f"{description} {hwid}".lower()
    return _is_bluetooth_port(description, hwid) and (
        "incoming" in text or "000000000000" in text
    )


def _write_timeout_for(device: DeviceInfo) -> float:
    if _is_bluetooth_serial(device):
        return BLUETOOTH_WRITE_TIMEOUT_SECONDS
    return SERIAL_WRITE_TIMEOUT_SECONDS


def _format_open_error(device: DeviceInfo, exc: Exception) -> str:
    message = f"Could not open {device.address}: {exc}"
    if _is_bluetooth_serial(device):
        message += (
            ". Windows often creates two Bluetooth serial ports; one may be an incoming "
            "listener or a stale link. Try the other Bluetooth COM port, and confirm the "
            "ESP is paired and powered on."
        )
    return message


def _looks_like_error(line: str) -> bool:
    return line.startswith(
        (
            "Could not",
            "Usage:",
            "Unknown command",
            "Radio is busy",
            "File ",
            "Channel must",
            "TX power level must",
            "CW channel must",
            "CW power level must",
            "CW loop timings must",
            "Message had no Morse-supported",
            "Text message",
            "Remote command",
            "ERR ",
            "Test mode blocks",
            "TESTTX requires",
        )
    )


def _status_text(raw: Any) -> str:
    if raw is None:
        return ""
    text = str(raw)
    return "" if text.lower() in {"", "none", "unknown", "unavailable"} else text


def _status_enabled(raw: Any) -> bool:
    if raw is True:
        return True
    if raw is None:
        return False
    return str(raw).lower() in {"enabled", "true", "on", "1", "yes"}


def _safe_remote_filename(name: str) -> str:
    return console_protocol.import_filename(name)


def _negotiated_host_chunk(value: object) -> int:
    # STATUS advertises the maximum; absence/malformed values use legacy64.
    # Latch this value into each session so later STATUS cannot alter framing.
    if type(value) is int and value >= HOST_TRANSFER_CHUNK_BYTES:
        return min(value, HOST_TRANSFER_FAST_CHUNK_BYTES)
    return HOST_TRANSFER_CHUNK_BYTES

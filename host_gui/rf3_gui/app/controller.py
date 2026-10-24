from __future__ import annotations

from dataclasses import replace
from pathlib import Path

from PySide6.QtCore import QObject, Signal

from rf3_gui.app.state_model import (
    CONSOLE_COMMAND_MAX_CHARS,
    ConnectionState,
    DeviceInfo,
    DeviceSnapshot,
    PROTOCOL_V2_MAX_FILE_SIZE,
    REMOTE_COMMAND_MAX_BYTES,
    TEXT_MESSAGE_MAX_BYTES,
    TransferProgress,
    TransferState,
    StorageListing,
)
from rf3_gui.device.base import DeviceBackend
from rf3_gui.protocol import console_protocol


class AppController(QObject):
    devices_changed = Signal(list)
    status_changed = Signal(object)
    transfer_changed = Signal(object)
    log_event = Signal(str, str)
    command_finished = Signal(str, bool, str)
    text_message_received = Signal(str, str)
    storage_changed = Signal(object)

    def __init__(self, backend: DeviceBackend) -> None:
        super().__init__()
        self._backend = backend
        self._devices: list[DeviceInfo] = []
        self._selected_device: DeviceInfo | None = None
        self._snapshot = DeviceSnapshot()
        self._selected_file: Path | None = None
        self._storage = StorageListing()

        backend.devices_discovered.connect(self._on_devices_discovered)
        backend.status_changed.connect(self._on_status_changed)
        backend.transfer_changed.connect(self._on_transfer_changed)
        backend.command_finished.connect(self._on_command_finished)
        backend.log_event.connect(self.log_event)
        backend.text_message_received.connect(self.text_message_received)
        backend.storage_changed.connect(self._on_storage_changed)

    @property
    def storage(self) -> StorageListing:
        return self._storage

    def _on_storage_changed(self, listing: StorageListing) -> None:
        self._storage = listing
        self.storage_changed.emit(listing)

    def refresh_storage(self) -> None:
        self._backend.refresh_storage()

    def import_file(self, source_path: Path) -> None:
        self._backend.import_file(source_path)

    def export_file(self, remote_name: str, destination_path: Path) -> None:
        self._backend.export_file(remote_name, destination_path)

    def delete_file(self, remote_name: str) -> None:
        self._backend.delete_file(remote_name)

    def select_stored_file(self, remote_name: str) -> None:
        self._backend.select_stored_file(remote_name)

    def start_stored_tx(self, remote_name: str) -> None:
        self._backend.start_stored_tx(remote_name)

    @property
    def devices(self) -> list[DeviceInfo]:
        return list(self._devices)

    @property
    def selected_file(self) -> Path | None:
        return self._selected_file

    @property
    def selected_device(self) -> DeviceInfo | None:
        return self._selected_device

    @property
    def snapshot(self) -> DeviceSnapshot:
        return self._snapshot

    def discover_devices(self) -> None:
        self._discover(False)

    def scan_bluetooth_devices(self) -> None:
        self._discover(True)

    def _discover(self, bluetooth: bool) -> None:
        if self._snapshot.connection_state in {ConnectionState.CONNECTING, ConnectionState.CONNECTED,
                                               ConnectionState.BUSY, ConnectionState.SEARCHING}:
            self.command_finished.emit("DISCOVER", False, "Disconnect or cancel before scanning")
            return
        self._snapshot = replace(
            self._snapshot,
            connection_state=ConnectionState.SEARCHING,
        )
        self.status_changed.emit(self._snapshot)
        self.log_event.emit("info", "Device discovery requested")
        if bluetooth:
            self._backend.scan_bluetooth()
        else:
            self._backend.discover()

    def select_device(self, index: int) -> None:
        if 0 <= index < len(self._devices):
            self._selected_device = self._devices[index]
            self.log_event.emit("info", f"Selected device {self._selected_device.name}")
            self.status_changed.emit(self._snapshot)

    def connect_selected_device(self, parent_window: int = 0) -> None:
        if self._selected_device is None:
            self.command_finished.emit("CONNECT", False, "No device selected")
            return
        self._backend.set_pairing_window(parent_window)
        self._backend.connect_to(self._selected_device)

    def disconnect(self) -> None:
        self._backend.disconnect_from_device()

    def refresh_status(self) -> None:
        self._backend.request_status()

    def set_test_mode(self, enabled: bool) -> None:
        self._backend.set_test_mode(enabled)

    def set_selected_file(self, path: Path) -> bool:
        if not path.exists() or not path.is_file():
            self.command_finished.emit("FILE", False, "Selected path is not a file")
            return False
        try:
            console_protocol.upload_extension(path.name)
            console_protocol.import_filename(path.name)
        except ValueError as exc:
            self.command_finished.emit("FILE", False, str(exc))
            return False
        if path.stat().st_size > PROTOCOL_V2_MAX_FILE_SIZE:
            self.command_finished.emit(
                "FILE",
                False,
                f"File exceeds Protocol v2 limit of {PROTOCOL_V2_MAX_FILE_SIZE} bytes",
            )
            return False
        self._selected_file = path
        self.log_event.emit("info", f"Selected file {path.name}")
        return True

    def set_channel(self, channel: int) -> None:
        self._backend.set_channel(channel)

    def set_power(self, level: int) -> None:
        self._backend.set_power(level)

    def enter_rx(self) -> None:
        self._backend.enter_rx()

    def enter_standby(self) -> None:
        self._backend.enter_standby()

    def enter_sleep(self) -> None:
        self._backend.enter_sleep()

    def wake(self) -> None:
        self._backend.wake()

    def power_down(self) -> None:
        self._backend.power_down()

    def stop(self) -> None:
        self._backend.stop()

    def start_tx(self) -> None:
        if self._selected_file is None:
            self.command_finished.emit("TX", False, "No file selected")
            return
        self._backend.start_tx(self._selected_file)

    def save_received_file(self, destination_path: Path) -> None:
        self._backend.save_received_file(destination_path)

    def send_morse_text(self, text: str) -> None:
        text = text.strip()
        max_text = CONSOLE_COMMAND_MAX_CHARS - len("MORSE ")
        if not _is_ascii_console_text(text):
            self.command_finished.emit("MORSE", False, "Morse text must be printable ASCII")
            return
        if not text:
            self.command_finished.emit("MORSE", False, "Enter text to send as Morse")
            return
        if len(text) > max_text:
            self.command_finished.emit(
                "MORSE",
                False,
                f"Morse text must be {max_text} characters or fewer",
            )
            return
        self._backend.send_morse_text(text)

    def send_remote_command(self, command: str) -> None:
        command = command.strip()
        if not _is_ascii_console_text(command):
            self.command_finished.emit("REMOTE", False, "Remote command must be printable ASCII")
            return
        if not command:
            self.command_finished.emit("REMOTE", False, "Enter a remote command")
            return
        if len(command) > REMOTE_COMMAND_MAX_BYTES:
            self.command_finished.emit(
                "REMOTE",
                False,
                f"Remote command must be {REMOTE_COMMAND_MAX_BYTES} characters or fewer",
            )
            return
        self._backend.send_remote_command(command)

    def send_text_message(self, text: str) -> None:
        text = text.strip()
        if not _is_ascii_console_text(text):
            self.command_finished.emit("TEXT", False, "Text messages must be printable ASCII")
            return
        if not text:
            self.command_finished.emit("TEXT", False, "Enter a message to send")
            return
        if len(text) > TEXT_MESSAGE_MAX_BYTES:
            self.command_finished.emit(
                "TEXT",
                False,
                f"Text messages must be {TEXT_MESSAGE_MAX_BYTES} characters or fewer",
            )
            return
        self._backend.send_text_message(text)

    def start_cw(self, channel: int, power_level: int) -> None:
        self._backend.start_cw(channel, power_level)

    def start_cw_loop(self, on_ms: int, off_ms: int, channel: int, power_level: int) -> None:
        self._backend.start_cw_loop(on_ms, off_ms, channel, power_level)

    def _on_devices_discovered(self, devices: list[DeviceInfo]) -> None:
        if self._snapshot.connection_state in {ConnectionState.CONNECTING, ConnectionState.CONNECTED, ConnectionState.BUSY}:
            return
        previous = self._selected_device
        self._devices = devices
        self._selected_device = _matching_device(previous, devices) or (devices[0] if devices else None)
        self._snapshot = replace(
            self._snapshot,
            connection_state=ConnectionState.DISCONNECTED,
        )
        self.devices_changed.emit(devices)
        self.status_changed.emit(self._snapshot)
        self.log_event.emit("info", f"Listed {len(devices)} device/port candidates; compatibility is confirmed only after STATUS")

    def _on_status_changed(self, snapshot: DeviceSnapshot) -> None:
        self._snapshot = snapshot
        self.status_changed.emit(snapshot)

    def _on_transfer_changed(self, progress: TransferProgress) -> None:
        self.transfer_changed.emit(progress)
        if progress.state in {TransferState.COMPLETED, TransferState.CANCELLED, TransferState.FAILED}:
            self.log_event.emit("info", progress.message)

    def _on_command_finished(self, command: str, accepted: bool, message: str) -> None:
        if command == "STATUS" and accepted:
            return
        level = "info" if accepted else "error"
        self.log_event.emit(level, f"{command}: {message}")
        self.command_finished.emit(command, accepted, message)


def _matching_device(
    previous: DeviceInfo | None,
    devices: list[DeviceInfo],
) -> DeviceInfo | None:
    if previous is None:
        return None
    for device in devices:
        if device.transport == previous.transport and device.address == previous.address:
            return device
    return None


def _is_ascii_console_text(text: str) -> bool:
    if any(ch in "\r\n" for ch in text):
        return False
    try:
        text.encode("ascii")
    except UnicodeEncodeError:
        return False
    return all(0x20 <= ord(ch) <= 0x7E for ch in text)

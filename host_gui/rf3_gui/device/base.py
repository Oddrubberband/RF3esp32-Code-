from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import QObject, Signal

from rf3_gui.app.state_model import DeviceInfo, DeviceSnapshot, TransferProgress


class DeviceBackend(QObject):
    devices_discovered = Signal(list)
    connection_changed = Signal(object)
    status_changed = Signal(object)
    transfer_changed = Signal(object)
    command_finished = Signal(str, bool, str)
    log_event = Signal(str, str)
    text_message_received = Signal(str, str)
    storage_changed = Signal(object)

    def discover(self) -> None:
        raise NotImplementedError

    def scan_bluetooth(self) -> None:
        self.discover()  # Mock backends remain hardware-free.

    def set_pairing_window(self, handle: int) -> None:
        pass

    def connect_to(self, device: DeviceInfo) -> None:
        raise NotImplementedError

    def disconnect_from_device(self) -> None:
        raise NotImplementedError

    def request_status(self) -> None:
        raise NotImplementedError

    def refresh_storage(self) -> None:
        raise NotImplementedError

    def import_file(self, source_path: Path) -> None:
        raise NotImplementedError

    def export_file(self, remote_name: str, destination_path: Path) -> None:
        raise NotImplementedError

    def delete_file(self, remote_name: str) -> None:
        raise NotImplementedError

    def select_stored_file(self, remote_name: str) -> None:
        raise NotImplementedError

    def start_stored_tx(self, remote_name: str) -> None:
        raise NotImplementedError

    def set_test_mode(self, enabled: bool) -> None:
        raise NotImplementedError

    def set_channel(self, channel: int) -> None:
        raise NotImplementedError

    def set_power(self, level: int) -> None:
        raise NotImplementedError

    def enter_rx(self) -> None:
        raise NotImplementedError

    def enter_standby(self) -> None:
        raise NotImplementedError

    def enter_sleep(self) -> None:
        raise NotImplementedError

    def wake(self) -> None:
        raise NotImplementedError

    def power_down(self) -> None:
        raise NotImplementedError

    def stop(self) -> None:
        raise NotImplementedError

    def start_tx(self, source_path: Path) -> None:
        raise NotImplementedError

    def save_received_file(self, destination_path: Path) -> None:
        raise NotImplementedError

    def send_morse_text(self, text: str) -> None:
        raise NotImplementedError

    def send_remote_command(self, command: str) -> None:
        raise NotImplementedError

    def send_text_message(self, text: str) -> None:
        raise NotImplementedError

    def start_cw(self, channel: int, power_level: int) -> None:
        raise NotImplementedError

    def start_cw_loop(self, on_ms: int, off_ms: int, channel: int, power_level: int) -> None:
        raise NotImplementedError

from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import QTimer

from rf3_gui.app.state_model import (
    ConnectionState,
    DEFAULT_CHANNEL,
    DeviceInfo,
    DeviceSnapshot,
    MAX_CHANNEL,
    MAX_POWER_LEVEL,
    MIN_CHANNEL,
    MIN_POWER_LEVEL,
    PROTOCOL_V2_MAX_FILE_SIZE,
    RadioCapabilities,
    RadioState,
    Telemetry,
    TransferProgress,
    TransferState,
    StoredFile,
    StorageListing,
)
from rf3_gui.device.base import DeviceBackend
from rf3_gui.protocol import console_protocol


class MockDeviceBackend(DeviceBackend):
    def __init__(self) -> None:
        super().__init__()
        self._device = DeviceInfo(
            name="RF3 Mock ESP32",
            address="MOCK-001",
            transport="mock",
            firmware_version="mock-dev",
            protocol_version="2",
        )
        self._capabilities = RadioCapabilities(
            supports_stored_file_tx=True,
            supports_host_storage=True,
            supports_host_file_upload=True,
            supports_host_file_download=True,
            supports_remote_commands=True,
            supports_text_messages=True,
        )
        self._snapshot = DeviceSnapshot(
            connection_state=ConnectionState.DISCONNECTED,
            requested_radio_state=RadioState.UNKNOWN,
            confirmed_radio_state=RadioState.UNKNOWN,
            channel=DEFAULT_CHANNEL,
            power_level=3,
            capabilities=self._capabilities,
        )
        self._connected = False
        self._stored_files = {"README.md": b"RF3 mock SPIFFS storage\n"}
        self._battery_percent = 87
        self._transfer_timer = QTimer(self)
        self._transfer_timer.timeout.connect(self._advance_transfer)
        self._transfer_path: Path | None = None
        self._transfer_total = 0
        self._transfer_sent = 0

    def discover(self) -> None:
        self.log_event.emit("info", "Mock discovery started")
        QTimer.singleShot(450, lambda: self.devices_discovered.emit([self._device]))

    def connect_to(self, device: DeviceInfo) -> None:
        self._snapshot = DeviceSnapshot(
            connection_state=ConnectionState.CONNECTING,
            requested_radio_state=RadioState.UNKNOWN,
            confirmed_radio_state=RadioState.UNKNOWN,
            channel=self._snapshot.channel,
            power_level=self._snapshot.power_level,
            capabilities=self._capabilities,
        )
        self.status_changed.emit(self._snapshot)
        self.log_event.emit("info", f"Connecting to {device.name}")
        QTimer.singleShot(500, self._finish_connect)

    def disconnect_from_device(self) -> None:
        self._connected = False
        self._transfer_timer.stop()
        self._snapshot = DeviceSnapshot(
            connection_state=ConnectionState.DISCONNECTED,
            requested_radio_state=RadioState.UNKNOWN,
            confirmed_radio_state=RadioState.UNKNOWN,
            channel=self._snapshot.channel,
            power_level=self._snapshot.power_level,
            capabilities=self._capabilities,
        )
        self.connection_changed.emit(self._device)
        self.status_changed.emit(self._snapshot)
        self.log_event.emit("info", "Mock device disconnected")

    def request_status(self) -> None:
        if not self._connected:
            self.command_finished.emit(console_protocol.status_command(), False, "Not connected")
            return
        self.command_finished.emit(console_protocol.status_command(), True, "Status refreshed")
        self.status_changed.emit(self._snapshot)

    def refresh_storage(self) -> None:
        if self._require_connected("STORAGE LIST"):
            self.storage_changed.emit(StorageListing(
                tuple(StoredFile(name, len(data)) for name, data in sorted(self._stored_files.items())),
                sum(map(len, self._stored_files.values())), 1_048_576, True))

    def import_file(self, source_path: Path) -> None:
        if not self._require_connected("UPLOAD"):
            return
        try:
            console_protocol.upload_extension(source_path.name)
            with source_path.open("rb") as file:
                data = file.read(PROTOCOL_V2_MAX_FILE_SIZE + 1)
            if len(data) > PROTOCOL_V2_MAX_FILE_SIZE:
                raise ValueError("File exceeds the supported transfer size")
            name = console_protocol.import_filename(source_path.name)
            self._stored_files[name] = data
            self.transfer_changed.emit(TransferProgress(state=TransferState.COMPLETED,
                sent_bytes=len(data), total_bytes=len(data),
                message=f"Mock import of {name}; no nRF24 TX", source_path=source_path))
            self.refresh_storage()
        except (OSError, ValueError) as exc:
            self.command_finished.emit("UPLOAD", False, str(exc))

    def export_file(self, remote_name: str, destination_path: Path) -> None:
        if not self._require_connected("DOWNLOAD"):
            return
        try:
            data = self._stored_files[remote_name]
            destination_path.write_bytes(data)
            self.transfer_changed.emit(TransferProgress(state=TransferState.COMPLETED,
                sent_bytes=len(data), total_bytes=len(data), message="Mock SPIFFS export complete",
                source_path=destination_path))
        except (OSError, KeyError) as exc:
            self.command_finished.emit("DOWNLOAD", False, str(exc))

    def delete_file(self, remote_name: str) -> None:
        if not self._require_connected("STORAGE DELETE"):
            return
        if remote_name not in self._stored_files or not console_protocol.valid_storage_name(remote_name):
            self.command_finished.emit("STORAGE DELETE", False, "Invalid SPIFFS file")
            return
        del self._stored_files[remote_name]
        self.command_finished.emit("STORAGE DELETE", True, f"Deleted {remote_name} from mock SPIFFS")
        self.refresh_storage()

    def set_test_mode(self, enabled: bool) -> None:
        self.command_finished.emit("TESTMODE", False, "Use a serial ESP32 devboard for test mode")

    def select_stored_file(self, remote_name: str) -> None:
        if not self._require_connected("SELECTHEX"):
            return
        if remote_name not in self._stored_files or not console_protocol.valid_storage_name(remote_name):
            self.command_finished.emit("SELECTHEX", False, "Invalid SPIFFS file")
            return
        self._snapshot = self._replace_snapshot(selected_file=remote_name)
        self.status_changed.emit(self._snapshot)
        self.command_finished.emit("SELECTHEX", True, f"Selected {remote_name}")

    def start_stored_tx(self, remote_name: str) -> None:
        if not self._require_connected("TXHEX"):
            return
        if remote_name not in self._stored_files or not console_protocol.valid_storage_name(remote_name):
            self.command_finished.emit("TXHEX", False, "Invalid SPIFFS file")
            return
        self._snapshot = self._replace_snapshot(selected_file=remote_name, active_tx_file=remote_name)
        self.status_changed.emit(self._snapshot)
        self.transfer_changed.emit(TransferProgress(
            state=TransferState.COMPLETED, sent_bytes=len(self._stored_files[remote_name]),
            total_bytes=len(self._stored_files[remote_name]),
            message=f"Mock stored-file TX: {remote_name}; no hardware transmission"))

    def set_channel(self, channel: int) -> None:
        command = console_protocol.channel_command(channel)
        if not self._require_connected(command):
            return
        if channel < MIN_CHANNEL or channel > MAX_CHANNEL:
            self.command_finished.emit(command, False, "Channel must be 0-125")
            return
        self._snapshot = self._replace_snapshot(
            requested_radio_state=RadioState.STANDBY,
            confirmed_radio_state=RadioState.STANDBY,
            channel=channel,
        )
        self.command_finished.emit(command, True, f"Radio reinitialized on channel {channel}")
        self.status_changed.emit(self._snapshot)

    def set_power(self, level: int) -> None:
        command = console_protocol.power_command(level)
        if not self._require_connected(command):
            return
        if level < MIN_POWER_LEVEL or level > MAX_POWER_LEVEL:
            self.command_finished.emit(command, False, "Power level must be 0-3")
            return
        self._snapshot = self._replace_snapshot(power_level=level)
        self.command_finished.emit(command, True, f"Packet TX power set to {level}")
        self.status_changed.emit(self._snapshot)

    def enter_rx(self) -> None:
        self._set_radio_state(console_protocol.rx_command(), RadioState.RX_LISTENING)

    def enter_standby(self) -> None:
        self._set_radio_state(console_protocol.standby_command(), RadioState.STANDBY)

    def enter_sleep(self) -> None:
        self._set_radio_state(console_protocol.sleep_command(), RadioState.SLEEP)

    def wake(self) -> None:
        self._set_radio_state(console_protocol.wake_command(), RadioState.STANDBY)

    def power_down(self) -> None:
        self._set_radio_state(console_protocol.power_down_command(), RadioState.POWER_DOWN)

    def stop(self) -> None:
        command = console_protocol.stop_command()
        if not self._require_connected(command):
            return
        was_transferring = self._transfer_timer.isActive()
        self._transfer_timer.stop()
        self._snapshot = self._replace_snapshot(
            connection_state=ConnectionState.CONNECTED,
            requested_radio_state=RadioState.STANDBY,
            confirmed_radio_state=RadioState.STANDBY,
        )
        if was_transferring:
            self.transfer_changed.emit(
                TransferProgress(
                    state=TransferState.CANCELLED,
                    sent_bytes=self._transfer_sent,
                    total_bytes=self._transfer_total,
                    message="Transfer cancelled",
                    source_path=self._transfer_path,
                )
            )
        self.command_finished.emit(command, True, "Stop complete. Radio is in standby.")
        self.status_changed.emit(self._snapshot)

    def start_tx(self, source_path: Path) -> None:
        command = console_protocol.tx_command(source_path.name)
        if not self._require_connected(command):
            return
        if not source_path.exists() or not source_path.is_file():
            self.command_finished.emit(command, False, "Source file does not exist")
            return
        size = source_path.stat().st_size
        if size > PROTOCOL_V2_MAX_FILE_SIZE:
            self.command_finished.emit(
                command,
                False,
                f"File exceeds Protocol v2 limit of {PROTOCOL_V2_MAX_FILE_SIZE} bytes",
            )
            return

        self._transfer_path = source_path
        self._transfer_total = size
        self._transfer_sent = 0
        self._snapshot = self._replace_snapshot(
            connection_state=ConnectionState.BUSY,
            requested_radio_state=RadioState.TX_BUSY,
            confirmed_radio_state=RadioState.TX_BUSY,
            selected_file=source_path.name,
        )
        self.command_finished.emit(command, True, f"TX started for {source_path.name}")
        self.status_changed.emit(self._snapshot)
        self.transfer_changed.emit(
            TransferProgress(
                state=TransferState.PREPARING,
                sent_bytes=0,
                total_bytes=size,
                message="Preparing transfer",
                source_path=source_path,
            )
        )
        self._transfer_timer.start(120)

    def save_received_file(self, destination_path: Path) -> None:
        command = "DOWNLOAD"
        if not self._require_connected(command):
            return
        if not self._snapshot.telemetry.last_rx_file:
            self.command_finished.emit(command, False, "No received file is available")
            return
        try:
            destination_path.write_bytes(b"RF3 mock received file\n")
        except OSError as exc:
            self.command_finished.emit(command, False, f"Could not save file: {exc}")
            return
        self.transfer_changed.emit(
            TransferProgress(
                state=TransferState.COMPLETED,
                sent_bytes=23,
                total_bytes=23,
                message=f"Saved received file to {destination_path}",
                source_path=destination_path,
            )
        )
        self.command_finished.emit(command, True, "Mock received file saved")

    def send_morse_text(self, text: str) -> None:
        command = console_protocol.morse_command(text)
        if not self._require_connected(command):
            return
        self._transfer_timer.stop()
        self._snapshot = self._replace_snapshot(
            connection_state=ConnectionState.BUSY,
            requested_radio_state=RadioState.TX_BUSY,
            confirmed_radio_state=RadioState.TX_BUSY,
        )
        self.command_finished.emit(command, True, "Morse started on channel 76 at dot=120 ms. Use STOP to abort.")
        self.status_changed.emit(self._snapshot)
        QTimer.singleShot(600, self._finish_morse)

    def send_remote_command(self, command_text: str) -> None:
        command = console_protocol.remote_command(command_text)
        if not self._require_connected(command):
            return
        telemetry = Telemetry(
            battery_percent=self._battery_percent,
            last_tx_ok=True,
            rx_packets=self._snapshot.telemetry.rx_packets,
            rx_saved=self._snapshot.telemetry.rx_saved,
            rx_saved_bytes=self._snapshot.telemetry.rx_saved_bytes,
            last_rx_file=self._snapshot.telemetry.last_rx_file,
            last_rx_path=self._snapshot.telemetry.last_rx_path,
        )
        self._snapshot = self._replace_snapshot(telemetry=telemetry)
        self.command_finished.emit(command, True, f"Sent remote command: {command_text}")
        self.status_changed.emit(self._snapshot)

    def send_text_message(self, text: str) -> None:
        command = console_protocol.text_command(text)
        if not self._require_connected(command):
            return
        self._snapshot = self._replace_snapshot(
            requested_radio_state=RadioState.RX_LISTENING,
            confirmed_radio_state=RadioState.RX_LISTENING,
        )
        self.text_message_received.emit("tx", text)
        self.command_finished.emit(command, True, f"TEXT TX: {text}")
        self.status_changed.emit(self._snapshot)

    def start_cw(self, channel: int, power_level: int) -> None:
        command = console_protocol.cw_start_command(channel, power_level)
        if not self._require_connected(command):
            return
        self._transfer_timer.stop()
        self._snapshot = self._replace_snapshot(
            connection_state=ConnectionState.BUSY,
            requested_radio_state=RadioState.CW_TEST,
            confirmed_radio_state=RadioState.CW_TEST,
            channel=channel,
            power_level=power_level,
        )
        self.command_finished.emit(
            command,
            True,
            f"CW mode active on channel {channel} at power level {power_level}. Use STOP to abort.",
        )
        self.status_changed.emit(self._snapshot)

    def start_cw_loop(self, on_ms: int, off_ms: int, channel: int, power_level: int) -> None:
        command = console_protocol.cw_loop_command(on_ms, off_ms, channel, power_level)
        if not self._require_connected(command):
            return
        self._transfer_timer.stop()
        self._snapshot = self._replace_snapshot(
            connection_state=ConnectionState.BUSY,
            requested_radio_state=RadioState.CW_TEST,
            confirmed_radio_state=RadioState.CW_TEST,
            channel=channel,
            power_level=power_level,
        )
        self.command_finished.emit(
            command,
            True,
            (
                f"CW mode active on channel {channel} at power level {power_level} "
                f"({on_ms} ms on, {off_ms} ms off). Use STOP to abort."
            ),
        )
        self.status_changed.emit(self._snapshot)

    def _finish_connect(self) -> None:
        self._connected = True
        self.connection_changed.emit(self._device)
        self._snapshot = DeviceSnapshot(
            connection_state=ConnectionState.CONNECTED,
            requested_radio_state=RadioState.STANDBY,
            confirmed_radio_state=RadioState.STANDBY,
            channel=self._snapshot.channel,
            power_level=self._snapshot.power_level,
            telemetry=Telemetry(battery_percent=self._battery_percent),
            capabilities=self._capabilities,
        )
        self.log_event.emit("info", "Application handshake accepted by mock device")
        self.status_changed.emit(self._snapshot)

    def _set_radio_state(self, command: str, state: RadioState) -> None:
        if not self._require_connected(command):
            return
        self._transfer_timer.stop()
        self._snapshot = self._replace_snapshot(
            connection_state=ConnectionState.CONNECTED,
            requested_radio_state=state,
            confirmed_radio_state=state,
        )
        self.command_finished.emit(command, True, f"Confirmed state {state.value}")
        self.status_changed.emit(self._snapshot)

    def _advance_transfer(self) -> None:
        if self._transfer_path is None:
            self._transfer_timer.stop()
            return

        step = max(1, self._transfer_total // 24) if self._transfer_total else 1
        self._transfer_sent = min(self._transfer_total, self._transfer_sent + step)
        self.transfer_changed.emit(
            TransferProgress(
                state=TransferState.TRANSFERRING,
                sent_bytes=self._transfer_sent,
                total_bytes=self._transfer_total,
                message="Transfer in progress",
                source_path=self._transfer_path,
            )
        )

        if self._transfer_sent >= self._transfer_total:
            self._transfer_timer.stop()
            telemetry = Telemetry(
                battery_percent=self._battery_percent,
                last_tx_ok=True,
                rx_packets=self._snapshot.telemetry.rx_packets,
                rx_saved=self._snapshot.telemetry.rx_saved,
                rx_saved_bytes=self._snapshot.telemetry.rx_saved_bytes,
                last_rx_file=self._snapshot.telemetry.last_rx_file,
                last_rx_path=self._snapshot.telemetry.last_rx_path,
            )
            self._snapshot = self._replace_snapshot(
                connection_state=ConnectionState.CONNECTED,
                requested_radio_state=RadioState.STANDBY,
                confirmed_radio_state=RadioState.STANDBY,
                telemetry=telemetry,
            )
            self.transfer_changed.emit(
                TransferProgress(
                    state=TransferState.COMPLETED,
                    sent_bytes=self._transfer_total,
                    total_bytes=self._transfer_total,
                    message="Mock transfer complete",
                    source_path=self._transfer_path,
                )
            )
            self.status_changed.emit(self._snapshot)

    def _finish_morse(self) -> None:
        if not self._connected:
            return
        if self._snapshot.confirmed_radio_state != RadioState.TX_BUSY:
            return
        self._snapshot = self._replace_snapshot(
            connection_state=ConnectionState.CONNECTED,
            requested_radio_state=RadioState.STANDBY,
            confirmed_radio_state=RadioState.STANDBY,
        )
        self.status_changed.emit(self._snapshot)

    def _require_connected(self, command: str) -> bool:
        if self._connected:
            return True
        self.command_finished.emit(command, False, "Not connected")
        return False

    def _replace_snapshot(self, **changes) -> DeviceSnapshot:
        values = {
            "rx_transfer_state": self._snapshot.rx_transfer_state,
            "rx_transfer_id": self._snapshot.rx_transfer_id,
            "rx_received_bytes": self._snapshot.rx_received_bytes,
            "rx_total_bytes": self._snapshot.rx_total_bytes,
            "rx_error": self._snapshot.rx_error,
            "profile": self._snapshot.profile,
            "active_tx_file": self._snapshot.active_tx_file,
            "connection_state": self._snapshot.connection_state,
            "test_mode": self._snapshot.test_mode,
            "test_tx_count": self._snapshot.test_tx_count,
            "test_tx_bytes": self._snapshot.test_tx_bytes,
            "requested_radio_state": self._snapshot.requested_radio_state,
            "confirmed_radio_state": self._snapshot.confirmed_radio_state,
            "channel": self._snapshot.channel,
            "power_level": self._snapshot.power_level,
            "selected_file": self._snapshot.selected_file,
            "telemetry": self._snapshot.telemetry,
            "capabilities": self._snapshot.capabilities,
        }
        values.update(changes)
        if "telemetry" not in changes and self._connected:
            values["telemetry"] = Telemetry(
                battery_percent=self._battery_percent,
                last_fault=self._snapshot.telemetry.last_fault,
                last_tx_ok=self._snapshot.telemetry.last_tx_ok,
                tx_timeout=self._snapshot.telemetry.tx_timeout,
                rx_packets=self._snapshot.telemetry.rx_packets,
                rx_saved=self._snapshot.telemetry.rx_saved,
                rx_saved_bytes=self._snapshot.telemetry.rx_saved_bytes,
                last_rx_file=self._snapshot.telemetry.last_rx_file,
                last_rx_path=self._snapshot.telemetry.last_rx_path,
                rpd=self._snapshot.telemetry.rpd,
            )
        return DeviceSnapshot(**values)

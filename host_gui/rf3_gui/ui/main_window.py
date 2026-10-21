from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QApplication,
    QCheckBox,
    QFileDialog,
    QFormLayout,
    QGridLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMainWindow,
    QMessageBox,
    QPlainTextEdit,
    QProgressBar,
    QScrollArea,
    QPushButton,
    QComboBox,
    QSizePolicy,
    QSpinBox,
    QTabWidget,
    QVBoxLayout,
    QWidget,
)

from rf3_gui.app.controller import AppController
from rf3_gui.app.state_model import (
    CONSOLE_COMMAND_MAX_CHARS,
    ConnectionState,
    DeviceInfo,
    DeviceSnapshot,
    MAX_CHANNEL,
    MIN_CHANNEL,
    REMOTE_COMMAND_MAX_BYTES,
    TEXT_MESSAGE_MAX_BYTES,
    TransferProgress,
    TransferState,
)


class MainWindow(QMainWindow):
    def __init__(self, controller: AppController) -> None:
        super().__init__()
        self._controller = controller
        self._devices: list[DeviceInfo] = []
        self._pending_test_mode: bool | None = None

        self.setWindowTitle("RF3 Desktop")
        self._build_ui()
        self._connect_signals()
        self._apply_snapshot(controller.snapshot)

    def _build_ui(self) -> None:
        central = QWidget(self)
        root = QVBoxLayout(central)
        root.setContentsMargins(14, 14, 14, 14)
        root.setSpacing(8)

        title = QLabel("RF3 Desktop")
        title.setObjectName("titleLabel")
        title.setStyleSheet("font-size: 22px; font-weight: 600;")
        root.addWidget(title)

        root.addWidget(self._build_connection_group())

        tabs = QTabWidget()
        radio_page = QWidget()
        radio_layout = QVBoxLayout(radio_page)
        radio_layout.setContentsMargins(0, 8, 0, 0)
        radio_layout.setSpacing(8)

        middle = QHBoxLayout()
        middle.setSpacing(10)
        middle.addWidget(self._build_status_group(), 1)
        middle.addWidget(self._build_controls_group(), 1)
        radio_layout.addLayout(middle)

        transfer_row = QHBoxLayout()
        transfer_row.setSpacing(10)
        transfer_row.addWidget(self._build_transfer_group(), 3)
        transfer_row.addWidget(self._build_received_group(), 2)
        radio_layout.addLayout(transfer_row)
        radio_layout.addWidget(self._build_log_group(), 1)

        text_page = QWidget()
        text_layout = QVBoxLayout(text_page)
        text_layout.setContentsMargins(0, 8, 0, 0)
        text_layout.addWidget(self._build_over_air_group(), 1)

        for page, label in ((radio_page, "Radio && Files"), (text_page, "Live Text && CW")):
            scroll = QScrollArea()
            scroll.setWidgetResizable(True)
            scroll.setWidget(page)
            tabs.addTab(scroll, label)
        root.addWidget(tabs, 1)

        self.setCentralWidget(central)
        self.statusBar().showMessage("Ready")

    def _build_connection_group(self) -> QGroupBox:
        group = QGroupBox("Connection")
        layout = QGridLayout(group)

        self.device_combo = QComboBox()
        self.device_combo.addItem("No devices discovered")
        self.device_combo.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        self.device_combo.setMinimumContentsLength(24)
        self.device_combo.setSizeAdjustPolicy(QComboBox.SizeAdjustPolicy.AdjustToMinimumContentsLengthWithIcon)

        self.discover_button = QPushButton("Refresh Ports")
        self.bluetooth_scan_button = QPushButton("Scan Bluetooth")
        self.connect_button = QPushButton("Connect")
        self.disconnect_button = QPushButton("Disconnect")
        self.refresh_button = QPushButton("Refresh Status")
        self.test_mode_checkbox = QCheckBox("Test/devboard mode (no nRF24 TX)")
        self.test_mode_checkbox.setToolTip("Verify files and text with the connected ESP over USB/Bluetooth; nRF24 commands stay blocked")

        self.connection_state_label = QLabel("Disconnected")
        self.connection_state_label.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)

        layout.addWidget(QLabel("Device"), 0, 0)
        layout.addWidget(self.device_combo, 0, 1, 1, 3)
        layout.addWidget(self.connection_state_label, 0, 4)
        layout.addWidget(self.discover_button, 1, 1)
        layout.addWidget(self.bluetooth_scan_button, 1, 2)
        layout.addWidget(self.connect_button, 1, 3)
        layout.addWidget(self.disconnect_button, 1, 4)
        layout.addWidget(self.refresh_button, 2, 4)
        layout.addWidget(self.test_mode_checkbox, 2, 1, 1, 3)
        guidance = QLabel("Classic SPP: scan, select your ESP, then Pair & Connect. Windows may request confirmation or a PIN.")
        guidance.setToolTip("BLE-only devices require a separate transport and compatible firmware.")
        guidance.setWordWrap(True)
        layout.addWidget(guidance, 3, 1, 1, 4)
        layout.setColumnStretch(1, 1)

        return group

    def _build_status_group(self) -> QGroupBox:
        group = QGroupBox("Confirmed Device Status")
        layout = QFormLayout(group)

        self.requested_state_value = QLabel("Unknown")
        self.confirmed_state_value = QLabel("Unknown")
        self.channel_value = QLabel("76")
        self.power_value = QLabel("unknown")
        self.battery_value = QLabel("not reported")
        self.selected_file_value = QLabel("")
        self.last_tx_value = QLabel("false")
        self.test_tx_value = QLabel("0 acknowledged")
        self.rx_value = QLabel("0 packets, 0 saved")

        layout.addRow("Requested state", self.requested_state_value)
        layout.addRow("Radio state", self.confirmed_state_value)
        layout.addRow("Channel", self.channel_value)
        layout.addRow("TX power", self.power_value)
        layout.addRow("Battery", self.battery_value)
        layout.addRow("Selected file", self.selected_file_value)
        layout.addRow("Last RF TX ok", self.last_tx_value)
        layout.addRow("Legacy TESTTX counter", self.test_tx_value)
        layout.addRow("RX summary", self.rx_value)

        return group

    def _build_controls_group(self) -> QGroupBox:
        group = QGroupBox("Radio Controls")
        layout = QVBoxLayout(group)

        state_row = QGridLayout()
        self.stop_button = QPushButton("Stop")
        self.standby_button = QPushButton("Standby")
        self.rx_button = QPushButton("Receive")
        self.sleep_button = QPushButton("Sleep")
        self.wake_button = QPushButton("Wake")
        self.power_down_button = QPushButton("Power Down")
        state_row.addWidget(self.stop_button, 0, 0)
        state_row.addWidget(self.standby_button, 0, 1)
        state_row.addWidget(self.rx_button, 0, 2)
        state_row.addWidget(self.sleep_button, 1, 0)
        state_row.addWidget(self.wake_button, 1, 1)
        state_row.addWidget(self.power_down_button, 1, 2)
        layout.addLayout(state_row)

        channel_row = QHBoxLayout()
        self.channel_spin = QSpinBox()
        self.channel_spin.setRange(MIN_CHANNEL, MAX_CHANNEL)
        self.channel_spin.setValue(76)
        self.apply_channel_button = QPushButton("Set Channel")
        channel_row.addWidget(QLabel("Channel"))
        channel_row.addWidget(self.channel_spin)
        channel_row.addWidget(self.apply_channel_button)
        layout.addLayout(channel_row)

        power_row = QHBoxLayout()
        self.power_combo = QComboBox()
        for level in range(4):
            self.power_combo.addItem(str(level), level)
        self.power_combo.setCurrentIndex(3)
        self.apply_power_button = QPushButton("Set Power")
        power_row.addWidget(QLabel("TX Power"))
        power_row.addWidget(self.power_combo)
        power_row.addWidget(self.apply_power_button)
        layout.addLayout(power_row)

        mode_row = QHBoxLayout()
        self.modulation_combo = QComboBox()
        self.modulation_combo.addItem("nRF24 packet mode (fixed)")
        self.modulation_combo.setEnabled(False)
        mode_row.addWidget(QLabel("RF mode"))
        mode_row.addWidget(self.modulation_combo)
        layout.addLayout(mode_row)

        return group

    def _build_transfer_group(self) -> QGroupBox:
        group = QGroupBox("Transmit File")
        layout = QGridLayout(group)

        self.file_path_edit = QLineEdit()
        self.file_path_edit.setReadOnly(True)
        self.file_path_edit.setPlaceholderText("No file selected")
        self.browse_button = QPushButton("Choose File")
        self.transmit_button = QPushButton("Transmit")
        self.progress_bar = QProgressBar()
        self.progress_bar.setRange(0, 1000)
        self.progress_bar.setValue(0)
        self.transfer_state_label = QLabel("Idle")
        self.transfer_detail_label = QLabel("Choose a file to transfer or verify.")
        self.transfer_detail_label.setWordWrap(True)
        self.transfer_detail_label.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)

        layout.addWidget(QLabel("Source"), 0, 0)
        layout.addWidget(self.file_path_edit, 0, 1)
        layout.addWidget(self.browse_button, 0, 2)
        layout.addWidget(self.transmit_button, 0, 3)
        layout.addWidget(self.progress_bar, 1, 1, 1, 2)
        layout.addWidget(self.transfer_state_label, 1, 3)
        layout.addWidget(self.transfer_detail_label, 2, 1, 1, 3)
        layout.setColumnStretch(1, 1)

        return group

    def _build_received_group(self) -> QGroupBox:
        group = QGroupBox("Received Files")
        layout = QGridLayout(group)

        self.rx_storage_value = QLabel("ESP SPIFFS (/spiffs)")
        self.rx_last_file_value = QLabel("No received file reported")
        self.rx_last_file_value.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        self.rx_saved_detail_value = QLabel("0 files, 0 bytes")
        self.copy_rx_path_button = QPushButton("Copy ESP Path")
        self.save_received_button = QPushButton("Save to PC")
        self.save_received_button.setToolTip("Download the received file from ESP storage")

        layout.addWidget(QLabel("Storage"), 0, 0)
        layout.addWidget(self.rx_storage_value, 0, 1, 1, 2)
        layout.addWidget(QLabel("Last file"), 1, 0)
        layout.addWidget(self.rx_last_file_value, 1, 1, 1, 2)
        layout.addWidget(QLabel("Saved"), 2, 0)
        layout.addWidget(self.rx_saved_detail_value, 2, 1, 1, 2)
        layout.addWidget(self.copy_rx_path_button, 3, 1)
        layout.addWidget(self.save_received_button, 3, 2)
        layout.setColumnStretch(1, 1)

        return group

    def _build_over_air_group(self) -> QGroupBox:
        group = QGroupBox("Over-the-Air Text and CW")
        layout = QGridLayout(group)

        self.live_text_output = QPlainTextEdit()
        self.live_text_output.setReadOnly(True)
        self.live_text_output.setMaximumBlockCount(500)
        self.live_text_output.setMinimumHeight(100)
        self.live_text_edit = QLineEdit()
        self.live_text_edit.setMaxLength(TEXT_MESSAGE_MAX_BYTES)
        self.live_text_edit.setPlaceholderText("Message")
        self.send_text_button = QPushButton("Send")

        self.morse_text_edit = QLineEdit()
        self.morse_text_edit.setMaxLength(CONSOLE_COMMAND_MAX_CHARS - len("MORSE "))
        self.morse_text_edit.setPlaceholderText("Text to key as Morse")
        self.send_morse_button = QPushButton("Send Morse")

        self.remote_command_edit = QLineEdit()
        self.remote_command_edit.setMaxLength(REMOTE_COMMAND_MAX_BYTES)
        self.remote_command_edit.setPlaceholderText("Remote command")
        self.send_remote_button = QPushButton("Send Remote")
        self.send_remote_button.setToolTip(
            f"Remote control frames are limited to {REMOTE_COMMAND_MAX_BYTES} printable ASCII characters."
        )

        self.cw_channel_spin = QSpinBox()
        self.cw_channel_spin.setRange(MIN_CHANNEL, MAX_CHANNEL)
        self.cw_channel_spin.setValue(76)
        self.cw_power_combo = QComboBox()
        for level in range(4):
            self.cw_power_combo.addItem(str(level), level)
        self.cw_power_combo.setCurrentIndex(3)
        self.cw_start_button = QPushButton("Start CW")
        self.cw_stop_button = QPushButton("Stop")

        self.cw_on_spin = QSpinBox()
        self.cw_on_spin.setRange(1, 60000)
        self.cw_on_spin.setValue(500)
        self.cw_on_spin.setSuffix(" ms")
        self.cw_off_spin = QSpinBox()
        self.cw_off_spin.setRange(1, 60000)
        self.cw_off_spin.setValue(500)
        self.cw_off_spin.setSuffix(" ms")
        self.cw_loop_button = QPushButton("Loop CW")

        self.live_text_help = QLabel()
        self.live_text_help.setWordWrap(True)
        layout.addWidget(self.live_text_help, 0, 0, 1, 5)
        layout.addWidget(self.live_text_output, 1, 0, 1, 5)
        layout.addWidget(self.live_text_edit, 2, 0, 1, 4)
        layout.addWidget(self.send_text_button, 2, 4)
        layout.addWidget(QLabel("Morse"), 3, 0)
        layout.addWidget(self.morse_text_edit, 3, 1, 1, 3)
        layout.addWidget(self.send_morse_button, 3, 4)
        layout.addWidget(QLabel("Remote"), 4, 0)
        layout.addWidget(self.remote_command_edit, 4, 1, 1, 3)
        layout.addWidget(self.send_remote_button, 4, 4)
        layout.addWidget(QLabel("CW channel"), 5, 0)
        layout.addWidget(self.cw_channel_spin, 5, 1)
        layout.addWidget(QLabel("Power"), 5, 2)
        layout.addWidget(self.cw_power_combo, 5, 3)
        layout.addWidget(self.cw_start_button, 5, 4)
        layout.addWidget(QLabel("CW loop"), 6, 0)
        layout.addWidget(self.cw_on_spin, 6, 1)
        layout.addWidget(self.cw_off_spin, 6, 2)
        layout.addWidget(self.cw_loop_button, 6, 3)
        layout.addWidget(self.cw_stop_button, 6, 4)
        layout.setColumnStretch(1, 1)
        layout.setColumnStretch(3, 1)

        return group

    def _build_log_group(self) -> QGroupBox:
        group = QGroupBox("Integration Log")
        layout = QVBoxLayout(group)
        self.show_debug_checkbox = QCheckBox("Show debug traffic")
        self.show_debug_checkbox.setChecked(False)
        self.log_output = QPlainTextEdit()
        self.log_output.setReadOnly(True)
        self.log_output.setMaximumBlockCount(1000)
        layout.addWidget(self.show_debug_checkbox)
        layout.addWidget(self.log_output)
        return group

    def _connect_signals(self) -> None:
        self.discover_button.clicked.connect(self._controller.discover_devices)
        self.bluetooth_scan_button.clicked.connect(self._controller.scan_bluetooth_devices)
        self.connect_button.clicked.connect(lambda: self._controller.connect_selected_device(int(self.winId())))
        self.disconnect_button.clicked.connect(self._controller.disconnect)
        self.refresh_button.clicked.connect(self._controller.refresh_status)
        self.test_mode_checkbox.toggled.connect(self._set_test_mode)
        self.device_combo.currentIndexChanged.connect(self._controller.select_device)

        self.stop_button.clicked.connect(self._controller.stop)
        self.standby_button.clicked.connect(self._controller.enter_standby)
        self.rx_button.clicked.connect(self._controller.enter_rx)
        self.sleep_button.clicked.connect(self._controller.enter_sleep)
        self.wake_button.clicked.connect(self._controller.wake)
        self.power_down_button.clicked.connect(self._controller.power_down)
        self.apply_channel_button.clicked.connect(self._set_channel)
        self.apply_power_button.clicked.connect(self._set_power)

        self.browse_button.clicked.connect(self._choose_file)
        self.transmit_button.clicked.connect(self._controller.start_tx)
        self.copy_rx_path_button.clicked.connect(self._copy_received_path)
        self.save_received_button.clicked.connect(self._save_received_file)
        self.send_morse_button.clicked.connect(self._send_morse_text)
        self.morse_text_edit.returnPressed.connect(self._send_morse_text)
        self.send_remote_button.clicked.connect(self._send_remote_command)
        self.remote_command_edit.returnPressed.connect(self._send_remote_command)
        self.send_text_button.clicked.connect(self._send_text_message)
        self.live_text_edit.returnPressed.connect(self._send_text_message)
        self.cw_start_button.clicked.connect(self._start_cw)
        self.cw_loop_button.clicked.connect(self._start_cw_loop)
        self.cw_stop_button.clicked.connect(self._controller.stop)

        self._controller.devices_changed.connect(self._on_devices_changed)
        self._controller.status_changed.connect(self._apply_snapshot)
        self._controller.transfer_changed.connect(self._apply_transfer_progress)
        self._controller.log_event.connect(self._append_log)
        self._controller.command_finished.connect(self._on_command_finished)
        self._controller.text_message_received.connect(self._append_text_message)

    def _on_devices_changed(self, devices: list[DeviceInfo]) -> None:
        self._devices = devices
        self.device_combo.blockSignals(True)
        self.device_combo.clear()
        if not devices:
            self.device_combo.addItem("No compatible devices found")
        else:
            for device in devices:
                detail = ""
                if device.transport == "bluetooth":
                    detail = f" | {'paired' if device.paired else 'unpaired'} | {'seen in this scan' if device.seen_in_scan else 'cached, presence unconfirmed'}"
                self.device_combo.addItem(f"{device.name} ({device.transport}: {device.address}){detail}")
                self.device_combo.setItemData(self.device_combo.count() - 1,
                                             self.device_combo.itemText(self.device_combo.count() - 1),
                                             Qt.ItemDataRole.ToolTipRole)
        self.device_combo.blockSignals(False)
        if devices:
            selected = self._controller.selected_device
            selected_index = 0
            if selected is not None:
                for index, device in enumerate(devices):
                    if device.transport == selected.transport and device.address == selected.address:
                        selected_index = index
                        break
            self.device_combo.setCurrentIndex(selected_index)
            self._controller.select_device(selected_index)

    def _apply_snapshot(self, snapshot: DeviceSnapshot) -> None:
        connected = snapshot.connection_state in {
            ConnectionState.CONNECTED,
            ConnectionState.BUSY,
        }
        busy = snapshot.connection_state == ConnectionState.BUSY
        if not connected:
            self._pending_test_mode = None
        elif self._pending_test_mode == snapshot.test_mode:
            self._pending_test_mode = None

        if self._pending_test_mode is None:
            self.test_mode_checkbox.blockSignals(True)
            self.test_mode_checkbox.setChecked(snapshot.test_mode)
            self.test_mode_checkbox.blockSignals(False)
        self.test_mode_checkbox.setEnabled(
            connected and not busy and self._pending_test_mode is None
            and self._controller.selected_device is not None
            and self._controller.selected_device.transport in {"serial", "bluetooth"}
        )
        controls_enabled = connected and not busy and not snapshot.test_mode and self._pending_test_mode is None

        self.connection_state_label.setText(snapshot.connection_state.value)
        self.requested_state_value.setText(snapshot.requested_radio_state.value)
        self.confirmed_state_value.setText(snapshot.confirmed_radio_state.value)
        self.channel_value.setText(str(snapshot.channel))
        self.channel_spin.setValue(snapshot.channel)
        self.cw_channel_spin.setValue(snapshot.channel)
        self.power_value.setText("unknown" if snapshot.power_level is None else str(snapshot.power_level))
        if snapshot.power_level is not None:
            index = self.power_combo.findData(snapshot.power_level)
            if index >= 0:
                self.power_combo.setCurrentIndex(index)
            cw_index = self.cw_power_combo.findData(snapshot.power_level)
            if cw_index >= 0:
                self.cw_power_combo.setCurrentIndex(cw_index)

        battery = snapshot.telemetry.battery_percent
        self.battery_value.setText("not reported" if battery is None else f"{battery}%")
        self.selected_file_value.setText(snapshot.selected_file or "")
        self.last_tx_value.setText("true" if snapshot.telemetry.last_tx_ok else "false")
        self.test_tx_value.setText(f"{snapshot.test_tx_count} acknowledged ({snapshot.test_tx_bytes} bytes last)")
        self.rx_value.setText(
            f"{snapshot.telemetry.rx_packets} packets, "
            f"{snapshot.telemetry.rx_saved} saved "
            f"({snapshot.telemetry.rx_saved_bytes} bytes)"
        )
        rx_path = snapshot.telemetry.last_rx_path
        rx_file = snapshot.telemetry.last_rx_file
        if rx_path:
            self.rx_last_file_value.setText(rx_path)
        elif rx_file:
            self.rx_last_file_value.setText(f"/spiffs/{rx_file}")
        elif snapshot.telemetry.rx_saved:
            self.rx_last_file_value.setText("Saved on ESP; path not reported by this firmware")
        else:
            self.rx_last_file_value.setText("No received file reported")
        self.rx_saved_detail_value.setText(
            f"{snapshot.telemetry.rx_saved} files, {snapshot.telemetry.rx_saved_bytes} bytes"
        )

        connecting = snapshot.connection_state == ConnectionState.CONNECTING
        searching = snapshot.connection_state == ConnectionState.SEARCHING
        selected = self._controller.selected_device
        self.connect_button.setText("Pair && Connect" if selected and selected.transport == "bluetooth" and not selected.paired else "Connect")
        self.connect_button.setEnabled(bool(self._devices) and not connected and not busy and not connecting and not searching)
        self.disconnect_button.setText("Cancel" if searching or connecting else "Disconnect")
        self.disconnect_button.setEnabled(connected or busy or connecting or searching)
        self.discover_button.setEnabled(not (connected or busy or connecting or searching))
        self.bluetooth_scan_button.setEnabled(not (connected or busy or connecting or searching))
        self.device_combo.setEnabled(not (connected or busy or connecting or searching))
        self.refresh_button.setEnabled(connected and not busy)
        self.stop_button.setEnabled((connected or busy) and not snapshot.test_mode and self._pending_test_mode is None)
        self.standby_button.setEnabled(controls_enabled)
        self.rx_button.setEnabled(controls_enabled)
        self.sleep_button.setEnabled(controls_enabled)
        self.wake_button.setEnabled(controls_enabled)
        self.power_down_button.setEnabled(controls_enabled)
        self.apply_channel_button.setEnabled(controls_enabled)
        self.apply_power_button.setEnabled(controls_enabled)
        self.transmit_button.setText("Upload && Verify" if snapshot.test_mode else "Transmit")
        self.transmit_button.setToolTip(
            "Upload the actual file, read it back from ESP SPIFFS, and compare every byte. No nRF24 TX."
            if snapshot.test_mode else "Upload the file to ESP storage, then transmit it over nRF24."
        )
        self.transmit_button.setEnabled(
            connected and self._controller.selected_file is not None and not busy
            and self._pending_test_mode is None
            and snapshot.capabilities.supports_host_file_upload
            and (not snapshot.test_mode or snapshot.capabilities.supports_host_file_download)
        )
        self.copy_rx_path_button.setEnabled(bool(rx_path or rx_file))
        self.save_received_button.setEnabled(
            connected
            and snapshot.telemetry.rx_saved > 0
            and snapshot.capabilities.supports_host_file_download
            and not snapshot.test_mode
            and self._pending_test_mode is None
        )
        self.morse_text_edit.setEnabled(controls_enabled)
        self.send_morse_button.setEnabled(controls_enabled)
        remote_enabled = controls_enabled and snapshot.capabilities.supports_remote_commands
        self.remote_command_edit.setEnabled(remote_enabled)
        self.send_remote_button.setEnabled(remote_enabled)
        text_enabled = connected and not busy and self._pending_test_mode is None and (
            snapshot.capabilities.supports_host_echo if snapshot.test_mode
            else snapshot.capabilities.supports_text_messages
        )
        self.send_text_button.setText("Send to ESP" if snapshot.test_mode else "Send to Peer")
        if snapshot.test_mode:
            description = (
                "ESP echo test: laptop → connected ESP → laptop over USB/Bluetooth. "
                "The ESP returns your text; no nRF24 TX or remote peer is involved."
            )
            if not snapshot.capabilities.supports_host_echo:
                description += " Flash the updated devboard firmware to enable this test."
        else:
            description = (
                "Live conversation: laptop → local ESP → nRF24 → peer ESP → peer laptop. "
                "Both radios must use matching RF settings; put the peer in Receive. "
                "YOU means your radio sent the text, not confirmed peer delivery."
            )
        self.live_text_help.setText(description + " Up to 154 printable ASCII characters per message.")
        self.send_text_button.setToolTip(description)
        self.live_text_edit.setEnabled(text_enabled)
        self.send_text_button.setEnabled(text_enabled)
        self.cw_channel_spin.setEnabled(controls_enabled)
        self.cw_power_combo.setEnabled(controls_enabled)
        self.cw_on_spin.setEnabled(controls_enabled)
        self.cw_off_spin.setEnabled(controls_enabled)
        self.cw_start_button.setEnabled(controls_enabled)
        self.cw_loop_button.setEnabled(controls_enabled)
        self.cw_stop_button.setEnabled((connected or busy) and not snapshot.test_mode and self._pending_test_mode is None)

        self.statusBar().showMessage(
            f"{snapshot.connection_state.value} | radio {snapshot.confirmed_radio_state.value}"
            + (" | TEST MODE: no nRF24 TX" if snapshot.test_mode else "")
        )

    def _apply_transfer_progress(self, progress: TransferProgress) -> None:
        self.progress_bar.setValue(int(progress.ratio * 1000))
        label = progress.state.value
        if progress.total_bytes:
            label = f"{label}: {progress.sent_bytes}/{progress.total_bytes} bytes"
        self.transfer_state_label.setText(label)
        self.transfer_detail_label.setText(progress.message)
        self.transfer_detail_label.setToolTip(progress.message)

        if progress.state == TransferState.FAILED:
            QMessageBox.warning(self, "Transfer failed", progress.message)

    def _append_log(self, level: str, message: str) -> None:
        if level.lower() == "debug" and not self.show_debug_checkbox.isChecked():
            return
        self.log_output.appendPlainText(f"{level.upper():5} {message}")

    def _on_command_finished(self, _command: str, accepted: bool, message: str) -> None:
        if _command.startswith("TESTMODE") and not accepted:
            self._pending_test_mode = None
            self._apply_snapshot(self._controller.snapshot)
        if not accepted:
            if "state=Fault" in message:
                message = (
                    f"{message}\n\n"
                    "The PC-to-ESP connection is working, but the RF radio chip is reporting "
                    "a fault. Check nRF24 power, ground, SPI wiring, CSN/CE pins, and that the "
                    "radio module is installed before trying RF sleep/wake/RX/TX commands."
                )
            QMessageBox.warning(self, "Command rejected", message)
        elif _command.startswith(("TEXT ", "ECHO ")):
            self.live_text_edit.clear()

    def _choose_file(self) -> None:
        path_text, _ = QFileDialog.getOpenFileName(self, "Choose RF3 source file")
        if not path_text:
            return
        path = Path(path_text)
        if self._controller.set_selected_file(path):
            self.file_path_edit.setText(str(path))
            self.progress_bar.setValue(0)
            self.transfer_state_label.setText("Idle")
            self.transfer_detail_label.setText("File selected. Test mode uploads and verifies without nRF24 TX.")
            self._apply_snapshot(self._controller.snapshot)

    def _set_channel(self) -> None:
        self._controller.set_channel(self.channel_spin.value())

    def _set_power(self) -> None:
        self._controller.set_power(int(self.power_combo.currentData()))

    def _set_test_mode(self, enabled: bool) -> None:
        self._pending_test_mode = enabled
        self._apply_snapshot(self._controller.snapshot)
        self._controller.set_test_mode(enabled)

    def _copy_received_path(self) -> None:
        text = self.rx_last_file_value.text().strip()
        if text and text.startswith("/spiffs/"):
            QApplication.clipboard().setText(text)
            self.statusBar().showMessage(f"Copied {text}")

    def _save_received_file(self) -> None:
        source_name = self._controller.snapshot.telemetry.last_rx_file
        if not source_name:
            QMessageBox.warning(self, "No received file", "The ESP has not reported a received file.")
            return
        path_text, _ = QFileDialog.getSaveFileName(self, "Save received RF3 file", source_name)
        if path_text:
            self._controller.save_received_file(Path(path_text))

    def _send_morse_text(self) -> None:
        self._controller.send_morse_text(self.morse_text_edit.text())

    def _send_remote_command(self) -> None:
        self._controller.send_remote_command(self.remote_command_edit.text())

    def _send_text_message(self) -> None:
        self._controller.send_text_message(self.live_text_edit.text())

    def _append_text_message(self, direction: str, text: str) -> None:
        label = {"tx": "YOU", "rx": "PEER", "test_tx": "YOU (TEST)", "echo": "ESP ECHO"}.get(direction.lower(), direction)
        self.live_text_output.appendPlainText(f"{label:4}  {text}")

    def _start_cw(self) -> None:
        self._controller.start_cw(
            self.cw_channel_spin.value(),
            int(self.cw_power_combo.currentData()),
        )

    def _start_cw_loop(self) -> None:
        self._controller.start_cw_loop(
            self.cw_on_spin.value(),
            self.cw_off_spin.value(),
            self.cw_channel_spin.value(),
            int(self.cw_power_combo.currentData()),
        )

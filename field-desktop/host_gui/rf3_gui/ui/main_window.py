from __future__ import annotations

from pathlib import Path
import sys

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
    QPushButton,
    QSizePolicy,
    QSpinBox,
    QTableWidget,
    QTableWidgetItem,
    QAbstractItemView,
    QHeaderView,
    QVBoxLayout,
    QWidget,
)

from rf3_gui.app.controller import AppController
from rf3_gui.app.receive_rate import ReceiveRateTracker
from rf3_gui.build_info import BUILD_ID, DISPLAY_NAME
from rf3_gui.ui import workbench
from rf3_gui.ui.field_art import FieldComboBox
from rf3_gui.protocol import console_protocol
from rf3_gui.app.state_model import (
    BATTERY_CUTOFF_MV,
    BATTERY_RESET_MV,
    CONSOLE_COMMAND_MAX_CHARS,
    ConnectionState,
    DeviceInfo,
    DeviceSnapshot,
    MAX_CHANNEL,
    MIN_CHANNEL,
    REMOTE_COMMAND_MAX_BYTES,
    RadioState,
    StorageListing,
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
        self._draft_dirty: set[str] = set()
        self._syncing_drafts = False
        self._drafts_initialized = False
        self._connection_epoch = 0
        self._was_connected = False
        self._closing = False
        self._rx_rate_tracker = ReceiveRateTracker()
        self._rx_rate_snapshot = None

        self.setWindowTitle(DISPLAY_NAME)
        self._build_ui()
        self._connect_signals()
        self._apply_snapshot(controller.snapshot)

    def closeEvent(self, event) -> None:
        self._closing = True
        self._controller.disconnect()
        super().closeEvent(event)

    def _build_ui(self) -> None:
        workbench.build(self)

    def _build_storage_page(self) -> QWidget:
        page = QWidget()
        layout = QVBoxLayout(page)
        help_text = QLabel("Files on the connected ESP32. Import copies a PC file into SPIFFS and verifies it; export saves a selected file to the PC.")
        help_text.setWordWrap(True)
        layout.addWidget(help_text)
        self.storage_summary = QLabel("Connect and refresh to view SPIFFS files.")
        self.storage_summary.setWordWrap(True)
        layout.addWidget(self.storage_summary)
        self.capacity_bar = QProgressBar()
        self.capacity_bar.setRange(0, 1000)
        self.capacity_bar.setAccessibleName("Storage used at last file refresh")
        self.capacity_bar.hide()
        layout.addWidget(self.capacity_bar)
        self.storage_table = QTableWidget(0, 2)
        self.storage_table.setHorizontalHeaderLabels(["File name", "Size (bytes)"])
        self.storage_table.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
        self.storage_table.setSelectionMode(QAbstractItemView.SelectionMode.SingleSelection)
        self.storage_table.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
        self.storage_table.horizontalHeader().setSectionResizeMode(0, QHeaderView.ResizeMode.Stretch)
        self.storage_table.horizontalHeader().setSectionResizeMode(1, QHeaderView.ResizeMode.ResizeToContents)
        layout.addWidget(self.storage_table, 1)
        buttons = QHBoxLayout()
        self.storage_refresh_button = QPushButton("Refresh Files")
        self.storage_import_button = QPushButton("Import from PC")
        self.storage_export_button = QPushButton("Export to PC")
        self.storage_delete_button = QPushButton("Delete from SPIFFS")
        self.storage_delete_button.setToolTip("Delete the selected file after confirmation; stop radio activity first")
        for button in (self.storage_refresh_button, self.storage_import_button,
                       self.storage_export_button, self.storage_delete_button):
            buttons.addWidget(button)
        layout.addLayout(buttons)
        tx_buttons = QHBoxLayout()
        self.storage_select_button = QPushButton("Select on ESP")
        self.storage_transmit_button = QPushButton("Transmit Stored File")
        self.storage_select_button.setToolTip("Set the ESP's selected file to this row; no upload or transmission")
        self.storage_transmit_button.setToolTip("Transmit this stored file over nRF24 without uploading it again")
        tx_buttons.addWidget(self.storage_select_button)
        tx_buttons.addWidget(self.storage_transmit_button)
        layout.addLayout(tx_buttons)
        self.storage_progress = QProgressBar()
        self.storage_progress.setRange(0, 1000)
        self.storage_progress.hide()
        self.storage_operation = QLabel("Import and export use the host connection; import does not transmit over nRF24.")
        self.storage_operation.setWordWrap(True)
        layout.addWidget(QLabel("Latest file operation"))
        layout.addWidget(self.storage_progress)
        layout.addWidget(self.storage_operation)
        return page

    def _selected_storage_file(self):
        row = self.storage_table.currentRow()
        files = self._controller.storage.files
        return files[row] if 0 <= row < len(files) else None

    def _apply_storage_listing(self, listing: StorageListing) -> None:
        old_item = self.storage_table.item(self.storage_table.currentRow(), 0)
        selected_name = old_item.text() if old_item else None
        self.storage_table.setRowCount(len(listing.files))
        selected_row = -1
        for row, entry in enumerate(listing.files):
            self.storage_table.setItem(row, 0, QTableWidgetItem(entry.name))
            self.storage_table.setItem(row, 1, QTableWidgetItem(str(entry.size)))
            if selected_name == entry.name:
                selected_row = row
        if selected_row >= 0:
            self.storage_table.selectRow(selected_row)
        else:
            self.storage_table.clearSelection()
            self.storage_table.setCurrentCell(-1, -1)
        online = self._controller.snapshot.connection_state in {ConnectionState.CONNECTED, ConnectionState.BUSY}
        valid_capacity = online and listing.available and 0 <= listing.used <= listing.capacity and listing.capacity > 0
        self.capacity_bar.setVisible(valid_capacity)
        if valid_capacity:
            per_mille = listing.used * 1000 // listing.capacity
            percent = per_mille // 10
            display = "<1%" if listing.used and percent == 0 else f"{percent}%"
            self.capacity_bar.setFormat(f"{display} used")
            self.capacity_bar.setValue(per_mille)
            self.storage_summary.setText(f"Last file listing · {len(listing.files)} file(s) · {listing.capacity - listing.used:,} bytes free · {listing.used:,} of {listing.capacity:,} bytes used")
        elif online and listing.available:
            self.storage_summary.setText(f"{len(listing.files)} file(s) · Storage capacity unavailable")
        else:
            self.storage_summary.setText("Refresh Files to read this board's SPIFFS contents. Updated firmware is required.")
        self._apply_storage_controls()

    def _apply_storage_controls(self) -> None:
        snapshot = self._controller.snapshot
        ready = (snapshot.connection_state == ConnectionState.CONNECTED
                 and snapshot.capabilities.supports_host_storage and self._pending_test_mode is None)
        entry = self._selected_storage_file()
        listed = self._controller.storage.available
        self.storage_refresh_button.setEnabled(ready)
        self.storage_import_button.setEnabled(ready and listed and snapshot.capabilities.supports_host_file_upload
                                              and snapshot.capabilities.supports_host_file_download)
        self.storage_export_button.setEnabled(ready and listed and entry is not None
                                              and snapshot.capabilities.supports_host_file_download)
        self.storage_delete_button.setEnabled(ready and listed and entry is not None
                                              and snapshot.confirmed_radio_state not in
                                              {RadioState.RX_LISTENING, RadioState.TX_BUSY, RadioState.CW_TEST})
        can_select = (ready and listed and entry is not None
                      and snapshot.capabilities.supports_stored_file_tx
                      and snapshot.confirmed_radio_state not in {RadioState.TX_BUSY, RadioState.CW_TEST})
        self.storage_select_button.setEnabled(can_select)
        self.storage_transmit_button.setEnabled(
            can_select and not snapshot.test_mode
            and snapshot.confirmed_radio_state in {RadioState.STANDBY, RadioState.RX_LISTENING})

    def _select_storage_file(self) -> None:
        entry = self._selected_storage_file()
        if entry is not None:
            self._controller.select_stored_file(entry.name)

    def _transmit_storage_file(self) -> None:
        entry = self._selected_storage_file()
        if entry is not None:
            self._controller.start_stored_tx(entry.name)

    def _dialog_connection_valid(self, epoch: int) -> bool:
        return (epoch == self._connection_epoch
                and self._controller.snapshot.connection_state == ConnectionState.CONNECTED)

    def _import_storage_file(self) -> None:
        epoch = self._connection_epoch
        path_text, _ = QFileDialog.getOpenFileName(self, "Import file into ESP SPIFFS")
        if not path_text:
            return
        if not self._dialog_connection_valid(epoch):
            return
        path = Path(path_text)
        try:
            console_protocol.upload_extension(path.name)
            remote_name = console_protocol.import_filename(path.name)
        except ValueError as exc:
            QMessageBox.warning(self, "Unsupported file type", str(exc))
            return
        existing = {entry.name for entry in self._controller.storage.files}
        if remote_name in existing:
            answer = QMessageBox.question(self, "Replace SPIFFS file?",
                                          f"Replace {remote_name} on the connected ESP32?",
                                          QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
                                          QMessageBox.StandardButton.No)
            if answer != QMessageBox.StandardButton.Yes:
                return
        if not self._dialog_connection_valid(epoch):
            return
        self.storage_operation.setText(f"Importing {path.name} as {remote_name}; no nRF24 transmission")
        self._controller.import_file(path)

    def _export_storage_file(self) -> None:
        epoch = self._connection_epoch
        entry = self._selected_storage_file()
        if entry is None:
            return
        path_text, _ = QFileDialog.getSaveFileName(self, "Export SPIFFS file to PC", entry.name)
        if path_text and self._dialog_connection_valid(epoch):
            self._controller.export_file(entry.name, Path(path_text))

    def _delete_storage_file(self) -> None:
        epoch = self._connection_epoch
        entry = self._selected_storage_file()
        if entry is None:
            return
        answer = QMessageBox.question(self, "Delete SPIFFS file?",
                                      f"Delete {entry.name} ({entry.size:,} bytes) from the connected ESP32?",
                                      QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
                                      QMessageBox.StandardButton.No)
        if answer == QMessageBox.StandardButton.Yes and self._dialog_connection_valid(epoch):
            self._controller.delete_file(entry.name)

    def _build_connection_group(self) -> QGroupBox:
        group = QGroupBox("Choose a board")
        layout = QGridLayout(group)

        self.device_combo = FieldComboBox()
        self.device_combo.addItem("No devices discovered")
        self.device_combo.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        self.device_combo.setMinimumContentsLength(24)
        self.device_combo.setSizeAdjustPolicy(FieldComboBox.SizeAdjustPolicy.AdjustToMinimumContentsLengthWithIcon)

        self.discover_button = QPushButton("Refresh Ports")
        self.bluetooth_scan_button = QPushButton("Scan Bluetooth")
        self.connect_button = QPushButton("Connect")
        self.disconnect_button = QPushButton("Disconnect")
        self.refresh_button = QPushButton("Refresh Status")
        self.test_mode_checkbox = QCheckBox("ESP link test only (RF disabled)")
        self.test_mode_checkbox.setToolTip("Clear this for a physical radio test. Select it to test the PC-to-ESP link only.")
        self.mode_status_label = QLabel()

        self.connection_state_label = QLabel("Disconnected")
        self.connection_state_label.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
        self.connection_detail_label = QLabel()
        self.connection_detail_label.setWordWrap(True)

        layout.addWidget(self.device_combo, 0, 0, 1, 2)
        layout.addWidget(self.connect_button, 0, 2)
        discover = QHBoxLayout()
        discover.addWidget(self.discover_button)
        discover.addWidget(self.bluetooth_scan_button)
        discover.addStretch()
        layout.addLayout(discover, 1, 0, 1, 2)
        layout.addWidget(self.disconnect_button, 1, 2)
        self.connection_help = QLabel("For USB, refresh the ports and select the board. For Classic Bluetooth, scan and select the ESP. Windows may ask for a pairing PIN.")
        self.connection_help.setWordWrap(True)
        self.connection_help.setToolTip("Bluetooth requires Classic SPP on the selected device.")
        layout.addWidget(self.connection_help, 2, 0, 1, 3)
        layout.setRowMinimumHeight(3, 10)
        layout.addWidget(self.mode_status_label, 3, 0, 1, 3)
        layout.addWidget(self.test_mode_checkbox, 4, 0, 1, 2)
        layout.addWidget(self.refresh_button, 4, 2)
        layout.addWidget(self.connection_detail_label, 5, 0, 1, 2)
        layout.addWidget(self.connection_state_label, 5, 2)
        layout.setColumnStretch(0, 1)
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
        self.battery_value.setWordWrap(True)
        self.battery_voltage_value=QLabel("not reported")
        self.battery_status_value=QLabel("not reported")
        self.battery_status_value.setWordWrap(True)
        self.sense_voltage_value = QLabel("not reported")
        self.sense_voltage_value.setWordWrap(True)
        self.sense_voltage_value.setToolTip(
            "GPIO34 voltage from the most recent quiet reading. Firmware converts it to pack voltage using "
            "the measured calibration table. The reading is held during RF activity.")
        self.power_good_value = QLabel("not reported")
        self.power_good_value.setToolTip("GPIO15 battery-control output: HIGH unless sustained pack undervoltage latches it LOW. Reset only after a fresh quiet reading >=21 V.")
        self.nrf_rail_value = QLabel("not reported")
        self.selected_file_value = QLabel("")
        self.active_tx_file_value = QLabel("")
        self.fault_value = QLabel("None")
        self.fault_value.setWordWrap(True)
        self.last_tx_value = QLabel("false")
        self.test_tx_value = QLabel("0 acknowledged")
        self.rx_value = QLabel("0 packets, 0 saved")

        layout.addRow("Requested state", self.requested_state_value)
        layout.addRow("Radio state", self.confirmed_state_value)
        layout.addRow("Channel", self.channel_value)
        layout.addRow("TX power", self.power_value)
        layout.addRow("Battery", self.battery_value)
        layout.addRow("Battery pack (6S)",self.battery_voltage_value)
        layout.addRow("Battery protection",self.battery_status_value)
        layout.addRow("Sense input (GPIO34)", self.sense_voltage_value)
        layout.addRow("Power good (GPIO15)", self.power_good_value)
        layout.addRow("nRF rail request (GPIO21)", self.nrf_rail_value)
        layout.addRow("Selected SPIFFS file (ESP)", self.selected_file_value)
        layout.addRow("RF file (current/last)", self.active_tx_file_value)
        layout.addRow("Radio fault", self.fault_value)
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
        state_row.addWidget(self.standby_button, 0, 0)
        state_row.addWidget(self.sleep_button, 0, 1)
        state_row.addWidget(self.wake_button, 0, 2)
        state_row.addWidget(self.power_down_button, 0, 3)
        layout.addLayout(state_row)

        rail_row = QHBoxLayout()
        self.nrf_rail_on_button = QPushButton("nRF Rail On")
        self.nrf_rail_off_button = QPushButton("nRF Rail Off")
        self.nrf_rail_off_button.setToolTip("Deassert GPIO21 after stopping radio activity. Requires an external rail switch.")
        rail_row.addWidget(self.nrf_rail_on_button)
        rail_row.addWidget(self.nrf_rail_off_button)
        layout.addLayout(rail_row)
        self.battery_reset_button=QPushButton("Reset Battery Cutoff")
        self.battery_reset_button.setToolTip(
            "After charging or replacing the battery, use USB power and wait for a fresh quiet reading of at least 21 V. "
            "Devboards require a separate nRF rail enable after reset. The old custom PCB restores radio access during reset.")
        layout.addWidget(self.battery_reset_button)

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
        self.power_combo = FieldComboBox()
        for level in range(4):
            self.power_combo.addItem(str(level), level)
        self.power_combo.setCurrentIndex(3)
        self.apply_power_button = QPushButton("Set Power")
        power_row.addWidget(QLabel("TX Power"))
        power_row.addWidget(self.power_combo)
        power_row.addWidget(self.apply_power_button)
        layout.addLayout(power_row)

        self.fem_antenna_row = QWidget()
        antenna_row = QHBoxLayout(self.fem_antenna_row)
        antenna_row.setContentsMargins(0, 0, 0, 0)
        self.fem_antenna_label = QLabel()
        self.fem_antenna_button = QPushButton()
        antenna_row.addWidget(self.fem_antenna_label)
        antenna_row.addWidget(self.fem_antenna_button)
        layout.addWidget(self.fem_antenna_row)

        self.fem_lna_row = QWidget()
        lna_row = QHBoxLayout(self.fem_lna_row)
        lna_row.setContentsMargins(0, 0, 0, 0)
        self.fem_lna_label = QLabel()
        self.fem_lna_button = QPushButton()
        lna_row.addWidget(self.fem_lna_label)
        lna_row.addWidget(self.fem_lna_button)
        layout.addWidget(self.fem_lna_row)



        mode_row = QHBoxLayout()
        self.modulation_combo = FieldComboBox()
        self.modulation_combo.addItem("nRF24 packet mode (fixed)")
        self.modulation_combo.setEnabled(False)
        mode_row.addWidget(QLabel("RF mode"))
        mode_row.addWidget(self.modulation_combo)
        layout.addLayout(mode_row)

        return group

    def _build_transfer_options(self) -> QGroupBox:
        group = QGroupBox("Transfer settings")
        layout = QGridLayout(group)
        self.file_mode_combo = FieldComboBox()
        self.file_mode_combo.addItem("Reliable (ACK)", False)
        self.file_mode_combo.addItem("No ACK (one-way)", True)
        self.file_mode_combo.setAccessibleName("File transfer mode")
        self.file_mode_combo.setToolTip("Set both boards to the same mode, then press Receive on the destination.")
        self.file_mode_combo.currentIndexChanged.connect(self._set_file_mode)
        self.file_mode_hint = QLabel()
        self.file_mode_hint.setWordWrap(True)
        layout.addWidget(QLabel("File mode"), 0, 0)
        layout.addWidget(self.file_mode_combo, 0, 1)
        self.transfer_channel = QLabel()
        layout.addWidget(self.transfer_channel, 0, 2, Qt.AlignmentFlag.AlignRight)
        layout.addWidget(self.file_mode_hint, 1, 0, 1, 3)
        guidance = QLabel("Match the channel and mode on both boards. Start Receive on the destination first.")
        guidance.setWordWrap(True)
        layout.addWidget(guidance, 2, 0, 1, 3)
        layout.setColumnStretch(2, 1)
        return group

    def _build_transfer_group(self) -> QGroupBox:
        group = QGroupBox("Send a file")
        group.setObjectName("sendCard")
        layout = QGridLayout(group)

        self.file_path_edit = QLineEdit()
        self.file_path_edit.setReadOnly(True)
        self.file_path_edit.setPlaceholderText("No file selected")
        self.browse_button = QPushButton("Choose File")
        self.transmit_button = QPushButton("Transmit")
        self.progress_bar = QProgressBar()
        self.progress_bar.setRange(0, 1000)
        self.progress_bar.setValue(0)
        self.progress_bar.hide()
        self.transfer_state_label = QLabel("Idle")
        self.transfer_state_label.setWordWrap(True)
        self.transfer_detail_label = QLabel("Choose a file to transfer or verify.")
        self.transfer_detail_label.setWordWrap(True)
        self.transfer_detail_label.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)

        layout.addWidget(QLabel("PC source"), 0, 0, 1, 2)
        layout.addWidget(self.file_path_edit, 1, 0)
        layout.addWidget(self.browse_button, 1, 1)
        self.upload_name_value = QLabel("SPIFFS upload name: no PC file chosen")
        self.upload_name_value.setWordWrap(True)
        layout.addWidget(self.upload_name_value, 2, 0, 1, 2)
        layout.addWidget(QLabel("Latest file operation"), 3, 0, 1, 2)
        layout.addWidget(self.transfer_state_label, 4, 0, 1, 2)
        layout.addWidget(self.progress_bar, 5, 0, 1, 2)
        layout.addWidget(self.transfer_detail_label, 6, 0, 1, 2)
        layout.setRowStretch(7, 1)
        layout.addWidget(self.transmit_button, 8, 0, 1, 2)
        layout.setColumnStretch(0, 1)

        return group

    def _set_file_mode(self, index: int) -> None:
        enabled = bool(self.file_mode_combo.itemData(index))
        if not self._controller.set_no_ack_mode(enabled):
            self.file_mode_combo.blockSignals(True)
            self.file_mode_combo.setCurrentIndex(0 if enabled else 1)
            self.file_mode_combo.blockSignals(False)
        self._apply_snapshot(self._controller.snapshot)

    def _build_received_group(self) -> QGroupBox:
        group = QGroupBox("Receive a file")
        layout = QGridLayout(group)

        self.rx_storage_value = QLabel("ESP SPIFFS (/spiffs)")
        self.rx_last_file_value = QLabel("No received file reported")
        self.rx_last_file_value.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        self.rx_saved_detail_value = QLabel("0 files, 0 bytes")
        self.rx_transfer_value = QLabel("Idle")
        self.rx_transfer_value.setWordWrap(True)
        self.rx_progress = QProgressBar()
        self.rx_progress.setRange(0, 1000)
        self.rx_rate_value = QLabel("Not receiving")
        self.rx_rate_value.setToolTip("Received payload bytes per second, averaged over about 10 seconds of status samples. Time starts when this window first observes the transfer; protocol overhead is excluded.")
        self.copy_rx_path_button = QPushButton("Copy ESP Path")
        self.save_received_button = QPushButton("Save to PC")
        self.save_received_button.setToolTip("Download the received file from ESP storage")

        layout.addWidget(QLabel("Incoming RF"), 0, 0)
        layout.addWidget(self.rx_transfer_value, 0, 1, 1, 2)
        self.rx_stages = workbench.ReceiveStages()
        progress_layout = QVBoxLayout()
        progress_layout.setContentsMargins(0, 0, 0, 0)
        progress_layout.addWidget(self.rx_stages)
        progress_layout.addWidget(self.rx_progress)
        layout.addLayout(progress_layout, 1, 1, 1, 2)
        layout.addWidget(QLabel("Payload rate"), 2, 0)
        layout.addWidget(self.rx_rate_value, 2, 1, 1, 2)
        layout.addWidget(QLabel("Last file"), 3, 0)
        layout.addWidget(self.rx_last_file_value, 3, 1, 1, 2)
        layout.addWidget(QLabel("Saved"), 4, 0)
        layout.addWidget(self.rx_saved_detail_value, 4, 1, 1, 2)
        layout.addWidget(QLabel("Storage"), 5, 0)
        layout.addWidget(self.rx_storage_value, 5, 1, 1, 2)
        actions = QHBoxLayout()
        actions.addWidget(self.rx_button)
        actions.addStretch()
        actions.addWidget(self.copy_rx_path_button)
        actions.addWidget(self.save_received_button)
        layout.addLayout(actions, 6, 0, 1, 3)
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
        self.cw_power_combo = FieldComboBox()
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

    def _show_diagnostics(self, values: dict) -> None:
        self.firmware_build_label.setText(
            f"Firmware: {values['build']} | {values['profile']} | {values['tick_hz']} Hz scheduler")

    def _build_log_group(self) -> QGroupBox:
        group = QGroupBox("Integration Log")
        layout = QVBoxLayout(group)
        self.show_debug_checkbox = QCheckBox("Show debug traffic")
        self.show_debug_checkbox.setChecked(False)
        self.show_debug_checkbox.setToolTip("TX/RX arrows show PC connection traffic. OK STATUS is an acknowledgement, not RF delivery.")
        self.log_output = QPlainTextEdit()
        self.log_output.setReadOnly(True)
        self.log_output.setMaximumBlockCount(1000)
        self.log_location_label = QLabel("Diagnostic log: not configured")
        self.log_location_label.setWordWrap(True)
        self.log_location_label.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        self.diagnostics_button = QPushButton("Read RF Diagnostics")
        self.diagnostics_button.setToolTip("Read a one-time firmware diagnostic snapshot; does not transmit RF or reset counters")
        self.firmware_build_label = QLabel("Firmware build: read diagnostics to identify")
        self.firmware_build_label.setTextFormat(Qt.TextFormat.PlainText)
        self.firmware_build_label.setWordWrap(True)
        desktop_build = QLabel(f"Desktop: {BUILD_ID}")
        desktop_build.setToolTip(sys.executable)
        layout.addWidget(desktop_build)
        layout.addWidget(self.diagnostics_button)
        layout.addWidget(self.firmware_build_label)
        layout.addWidget(self.show_debug_checkbox)
        layout.addWidget(self.log_location_label)
        layout.addWidget(self.log_output)
        return group

    def _connect_signals(self) -> None:
        self.discover_button.clicked.connect(self._controller.discover_devices)
        self.bluetooth_scan_button.clicked.connect(self._controller.scan_bluetooth_devices)
        self.connect_button.clicked.connect(lambda: self._controller.connect_selected_device(int(self.winId())))
        self.disconnect_button.clicked.connect(self._controller.disconnect)
        self.refresh_button.clicked.connect(self._controller.refresh_status)
        self.diagnostics_button.clicked.connect(self._controller.request_diagnostics)
        self._controller.diagnostics_received.connect(self._show_diagnostics)
        self.test_mode_checkbox.toggled.connect(self._set_test_mode)
        self.device_combo.currentIndexChanged.connect(self._controller.select_device)

        self.stop_button.clicked.connect(self._controller.stop)
        self.standby_button.clicked.connect(self._controller.enter_standby)
        self.rx_button.clicked.connect(self._controller.enter_rx)
        self.sleep_button.clicked.connect(self._controller.enter_sleep)
        self.wake_button.clicked.connect(self._controller.wake)
        self.power_down_button.clicked.connect(self._controller.power_down)
        self.battery_reset_button.clicked.connect(self._controller.reset_battery_cutoff)
        self.nrf_rail_on_button.clicked.connect(lambda: self._controller.set_nrf_rail(True))
        self.nrf_rail_off_button.clicked.connect(lambda: self._controller.set_nrf_rail(False))
        self.apply_channel_button.clicked.connect(self._set_channel)
        self.apply_power_button.clicked.connect(self._set_power)
        self.fem_antenna_button.clicked.connect(
            lambda: self._controller.set_fem_antenna(self.fem_antenna_button.property("antenna")))
        self.fem_lna_button.clicked.connect(
            lambda: self._controller.set_fem_lna(self.fem_lna_button.property("mode")))
        self.channel_spin.valueChanged.connect(lambda: self._mark_draft("channel"))
        self.cw_channel_spin.valueChanged.connect(lambda: self._mark_draft("cw_channel"))
        self.power_combo.currentIndexChanged.connect(lambda: self._mark_draft("power"))
        self.cw_power_combo.currentIndexChanged.connect(lambda: self._mark_draft("cw_power"))

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
        self._controller.storage_changed.connect(self._apply_storage_listing)
        self.storage_table.itemSelectionChanged.connect(self._apply_storage_controls)
        self.storage_refresh_button.clicked.connect(self._controller.refresh_storage)
        self.storage_import_button.clicked.connect(self._import_storage_file)
        self.storage_export_button.clicked.connect(self._export_storage_file)
        self.storage_delete_button.clicked.connect(self._delete_storage_file)
        self.storage_select_button.clicked.connect(self._select_storage_file)
        self.storage_transmit_button.clicked.connect(self._transmit_storage_file)

    def _mark_draft(self, name: str) -> None:
        if not self._syncing_drafts:
            self._draft_dirty.add(name)

    def _sync_drafts(self, snapshot: DeviceSnapshot, connected: bool) -> None:
        if not connected:
            self._drafts_initialized = False
            return
        if snapshot.confirmed_radio_state == RadioState.UNKNOWN:
            return
        self._syncing_drafts = True
        try:
            for name, widget, value in (("channel", self.channel_spin, snapshot.channel),
                                        ("cw_channel", self.cw_channel_spin, snapshot.channel),
                                        ("power", self.power_combo, snapshot.power_level),
                                        ("cw_power", self.cw_power_combo, snapshot.power_level)):
                if value is None or ("power" in name and value not in range(4)):
                    continue
                current = widget.value() if isinstance(widget, QSpinBox) else widget.currentData()
                if not self._drafts_initialized or name not in self._draft_dirty:
                    if isinstance(widget, QSpinBox):
                        widget.setValue(value)
                    else:
                        widget.setCurrentIndex(widget.findData(value))
                    self._draft_dirty.discard(name)
                elif current == value:
                    # A confirmed value has caught up with this draft. An ACK
                    # alone never clears a draft before telemetry confirms it.
                    self._draft_dirty.discard(name)
            self._drafts_initialized = True
        finally:
            self._syncing_drafts = False

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
        self.channel_spin.setMaximum(snapshot.capabilities.max_channel)
        self.cw_channel_spin.setMaximum(snapshot.capabilities.max_channel)
        if connected != self._was_connected:
            self._connection_epoch += 1
            self._was_connected = connected
            self.capacity_bar.hide()
            self.storage_summary.setText("Refresh Files to read this board’s storage." if connected else "Connect and refresh to view board storage.")
            if connected and self._connection_epoch > 1:
                self.progress_bar.hide()
                self.storage_progress.hide()
                self.transfer_state_label.setText("Idle")
                self.transfer_detail_label.setText("No file operation reported in this connection.")
                self.storage_operation.setText(self.transfer_detail_label.text())
        self._sync_drafts(snapshot, connected)
        if not connected:
            self._pending_test_mode = None
        elif self._pending_test_mode == snapshot.test_mode:
            self._pending_test_mode = None

        if self._pending_test_mode is None:
            self.test_mode_checkbox.blockSignals(True)
            self.test_mode_checkbox.setChecked(snapshot.test_mode)
            self.test_mode_checkbox.blockSignals(False)
        have_report = connected and snapshot.confirmed_radio_state != RadioState.UNKNOWN
        mode_text = "Waiting for board status"
        if have_report:
            mode_text = "ESP link test only" if snapshot.test_mode else "Physical radio"
            device = self._controller.selected_device
            if device is not None and device.transport == "mock":
                mode_text = "Practice (simulated radio)"
        self.mode_status_label.setText("Reported operating mode: " + mode_text)
        self.test_mode_checkbox.setVisible(connected and snapshot.capabilities.supports_test_mode)
        self.test_mode_checkbox.setEnabled(
            connected and not busy and self._pending_test_mode is None
            and snapshot.confirmed_radio_state != RadioState.UNKNOWN
            and snapshot.capabilities.supports_test_mode
        )
        rail_off = snapshot.telemetry.nrf_rail is False or snapshot.telemetry.battery_cutoff
        controls_enabled = (connected and not busy and not snapshot.test_mode and not rail_off
                            and snapshot.confirmed_radio_state != RadioState.UNKNOWN
                            and self._pending_test_mode is None)
        bt=snapshot.telemetry
        rf_ready = controls_enabled and snapshot.confirmed_radio_state in {RadioState.STANDBY, RadioState.RX_LISTENING}
        antenna = snapshot.fem_antenna
        self.fem_antenna_row.setVisible(connected and antenna in (1, 2))
        if antenna in (1, 2):
            other = 3 - antenna
            ports = {1: "J1", 2: "J5"}
            self.fem_antenna_label.setText(f"FEM antenna: ANT{antenna} ({ports[antenna]})")
            self.fem_antenna_button.setText(f"Select ANT{other} ({ports[other]})")
            self.fem_antenna_button.setProperty("antenna", other)
            self.fem_antenna_button.setEnabled(controls_enabled and snapshot.confirmed_radio_state == RadioState.STANDBY)

        lna = snapshot.fem_lna
        if connected and lna in ("gain", "bypass"):
            other = "bypass" if lna == "gain" else "gain"
            self.fem_lna_label.setText("Receive LNA: " + ("Amplified" if lna == "gain" else "Bypassed"))
            self.fem_lna_button.setText("Bypass LNA" if other == "bypass" else "Enable LNA")
            self.fem_lna_button.setProperty("mode", other)
            self.fem_lna_button.setToolTip(self.fem_lna_label.text() + ". Change the receive path in standby.")
            self.fem_lna_button.setEnabled(controls_enabled and not bt.battery_cutoff
                                          and snapshot.confirmed_radio_state == RadioState.STANDBY)
        else:
            self.fem_lna_label.setText("Receive LNA: " + ("Unavailable" if connected else "No board connected"))
            self.fem_lna_button.setText("Bypass LNA")
            self.fem_lna_button.setProperty("mode", None)
            self.fem_lna_button.setEnabled(False)
            self.fem_lna_button.setToolTip("Available on boards that report receive LNA control.")

        board_power_controls = (connected and not busy and self._pending_test_mode is None
                                and snapshot.capabilities.supports_board_power
                                and snapshot.confirmed_radio_state != RadioState.RX_LISTENING)
        self.nrf_rail_on_button.setEnabled(board_power_controls and bt.nrf_rail is False and not bt.battery_cutoff)
        self.nrf_rail_off_button.setEnabled(board_power_controls and bt.nrf_rail is True)

        self.connection_state_label.setText(snapshot.connection_state.value)
        self.connection_detail_label.setText(snapshot.connection_detail)
        selected_device = self._controller.selected_device
        identity = (f"{selected_device.name} [{selected_device.address}]" if selected_device else snapshot.profile)
        self.setWindowTitle(DISPLAY_NAME + (f" — {identity}" if identity else ""))
        self.station.setText(identity if connected and identity else "No board connected")
        self.station.setToolTip(self.station.text())
        radio = snapshot.confirmed_radio_state
        radio_label = {
            RadioState.TX_BUSY: "Transmitting", RadioState.RX_LISTENING: "Listening",
            RadioState.CW_TEST: "CW active", RadioState.POWER_DOWN: "Powered down",
            RadioState.UNKNOWN: "Radio state unknown",
        }.get(radio, radio.value)
        self.host_state.setText(snapshot.connection_state.value)
        workbench.tone(self.host_state, "active" if connected else
                       "fault" if snapshot.connection_state == ConnectionState.ERROR else "neutral")
        self.link_state.setText(radio_label if connected else "Unavailable")
        radio_tone = {RadioState.RX_LISTENING: "active", RadioState.TX_BUSY: "outbound",
                      RadioState.CW_TEST: "outbound", RadioState.FAULT: "fault"}.get(radio, "neutral")
        workbench.tone(self.link_state, radio_tone if connected else "neutral")
        self.host_state.setAccessibleName(f"PC link: {self.host_state.text()}")
        self.link_state.setAccessibleName(f"Last radio report: {self.link_state.text()}")
        self.diagnostics_button.setEnabled(connected)
        if not connected:
            self.firmware_build_label.setText("Firmware build: read diagnostics to identify")
        self.requested_state_value.setText(snapshot.requested_radio_state.value)
        self.confirmed_state_value.setText(snapshot.confirmed_radio_state.value)
        self.channel_value.setText(str(snapshot.channel))
        self.power_value.setText(str(snapshot.power_level) if snapshot.power_level in range(4) else "Unknown")
        faults = {0: "None", 1: "SPI probe / RX entry failed", 2: "Register initialization / RX exit failed",
                  3: "Transmit failed or timed out", 4: "RX payload read failed", 5: "Sleep transition failed",
                  6: "Power-down failed", 7: "Wake failed", 8: "CW start failed", 9: "Power change failed"}
        fault = snapshot.telemetry.last_fault
        self.fault_value.setText(f"{fault}: {faults.get(fault, 'Radio error')}" if fault else "None")

        battery = snapshot.telemetry.battery_percent
        t=snapshot.telemetry
        if battery is None:
            self.battery_value.setText("waiting for quiet reading" if t.battery_state else "not reported")
        elif t.battery_state:
            self.battery_value.setText(f"~{battery}% (~{t.battery_remaining_mah or 0} mAh; voltage estimate)")
        else:
            self.battery_value.setText(f"{battery}%")
        display_mv = t.battery_estimated_mv if t.battery_estimated_mv is not None else t.battery_mv
        self.battery_voltage_value.setText("unavailable" if display_mv is None else f"{display_mv/1000:.2f} V (estimate)")
        sensed = "unavailable" if t.battery_mv is None else f"{t.battery_mv/1000:.3f} V"
        self.battery_voltage_value.setToolTip(
            f"Sensed protection voltage: {sensed}. Cutoff: below {BATTERY_CUTOFF_MV/1000:.2f} V; "
            f"explicit recovery: at least {BATTERY_RESET_MV/1000:.2f} V sensed. "
            "The corrected display and charge gauge use the firmware estimate when provided.")
        states={"unavailable":"Waiting for a valid sample", "settling":"Waiting for 2 s of radio quiet",
                "startup":"Waiting through 10 s startup exclusion",
                "averaging":"Collecting a 4 s quiet mean",
                "fresh":"Quiet mean; monitoring 20.4 V sensed cutoff", "held":"Held during radio activity / settling",
                "stale":"Stale reading; wait in Standby", "low_pending":"Below 20.4 V sensed; confirming five complete low means",
                "sensor_error":"ADC unavailable/noisy; last pack reading shown", "cutoff":"CUTOFF LATCHED: shutdown requested"}
        detail=states.get(t.battery_state,"not reported")
        if t.battery_windows is not None:
            n = t.battery_windows
            if n < 4:
                self.battery_value.setText(f"Collecting voltage readings ({n}/4)")
            latest = "pending" if t.battery_interval_mv is None else f"{t.battery_interval_mv/1000:.3f} V"
            rolling = "pending" if t.battery_mv is None else f"{t.battery_mv/1000:.3f} V"
            self.battery_voltage_value.setText(f"{latest} (1 s)  |  {rolling} (4-interval mean)")
            self.battery_voltage_value.setToolTip("Measured pack voltage. GPIO15 stays enabled. Firmware cutoff is disabled.")
            detail = {"startup":"Waiting for first reading", "averaging":"Sampling for 1 s",
                      "fresh":"Interval complete", "held":"Sampling deferred by radio activity or timing",
                      "sensor_error":"ADC reading unavailable", "stale":"Waiting for a new interval"}.get(t.battery_state, "Waiting")
        if t.battery_age_ms is not None:
            detail+=f"; age {t.battery_age_ms/1000:.1f} s"
        if t.battery_shutdown_pending:
            detail+="; shutdown in progress, awaiting output confirmation"
        if t.battery_cutoff and not t.battery_shutdown_pending and not t.battery_persisted:
            detail+="; persistence write failed"
        if t.battery_cutoff and not t.battery_shutdown_pending and t.power_good:
            detail+="; ENABLE OUTPUT ERROR"
        self.battery_status_value.setText(detail)
        self.battery_reset_button.setEnabled(board_power_controls and t.battery_cutoff and
            t.battery_mv is not None and t.battery_mv>=BATTERY_RESET_MV and
            t.battery_age_ms is not None and t.battery_age_ms<=10000)
        telemetry = snapshot.telemetry
        if telemetry.sense_mv is None:
            self.sense_voltage_value.setText("unavailable" if snapshot.capabilities.supports_board_power else "not reported")
        else:
            quality = "calibrated" if telemetry.sense_calibrated else "estimated"
            if telemetry.sense_noisy:
                quality+=", noisy sample rejected"
            if telemetry.sense_saturated:
                quality += ", ADC saturated"
            elif telemetry.sense_mv > 2450 or telemetry.sense_mv < 150:
                quality += ", outside characterized range"
            self.sense_voltage_value.setText(f"{telemetry.sense_mv / 1000:.3f} V ({quality})")
        self.power_good_value.setText("not reported" if telemetry.power_good is None else "HIGH" if telemetry.power_good else "LOW")
        if telemetry.nrf_rail is None:
            self.nrf_rail_value.setText("not available" if snapshot.capabilities.supports_board_power else "not reported")
        else:
            self.nrf_rail_value.setText("ON (HIGH)" if telemetry.nrf_rail else "OFF (LOW)")
        self.selected_file_value.setText(snapshot.selected_file or "")
        self.active_tx_file_value.setText(snapshot.active_tx_file or "None reported")
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
        rx_state = snapshot.rx_transfer_state or "Idle"
        detail = (f"{rx_state} | {snapshot.rx_transfer_id} | "
                  f"{snapshot.rx_received_bytes:,}/{snapshot.rx_total_bytes:,} bytes"
                  if snapshot.rx_transfer_id else rx_state)
        if snapshot.rx_error and snapshot.rx_error != "none":
            detail += f" | {snapshot.rx_error}"
        self.rx_transfer_value.setText(detail)
        ratio = (snapshot.rx_received_bytes / snapshot.rx_total_bytes if snapshot.rx_total_bytes
                 else 1.0 if rx_state == "Completed" else 0.0)
        self.rx_progress.setValue(int(min(1.0, max(0.0, ratio)) * 1000))
        if snapshot is not self._rx_rate_snapshot:
            self._rx_rate_snapshot = snapshot
            estimate = self._rx_rate_tracker.observe(snapshot.rx_transfer_id,
                snapshot.rx_transfer_state, snapshot.rx_received_bytes)
            if estimate is None:
                self.rx_rate_value.setText("Waiting for samples" if snapshot.rx_transfer_id else "Not receiving")
            else:
                rate_text = (f"{estimate.bytes_per_second / 1024:.2f} KiB/s"
                             if estimate.bytes_per_second is not None else "Waiting for samples")
                self.rx_rate_value.setText(f"{rate_text} | {estimate.elapsed_seconds:.1f} s observed")
        telemetry_available = connected and snapshot.confirmed_radio_state != RadioState.UNKNOWN
        if not telemetry_available:
            for label in (self.channel_value, self.power_value, self.fault_value,
                          self.last_tx_value, self.test_tx_value, self.rx_value,
                          self.rx_saved_detail_value, self.rx_last_file_value,
                          self.selected_file_value, self.active_tx_file_value):
                label.setText("Unavailable")
        # Keep valid transfer metadata even while the radio register lock is busy.
        self.transfer_channel.setText(f"Radio channel   {self.channel_value.text()}")
        # Unknown radio state continues to guard all disruptive RF controls.
        incoming_available = connected and (telemetry_available or bool(snapshot.rx_transfer_id))
        received_report = incoming_available and bool(snapshot.rx_transfer_id)
        self.rx_stages.show_report(rx_state if received_report else "")
        self.rx_progress.setVisible(received_report and snapshot.rx_total_bytes > 0)
        self.rx_progress.setFormat("%p% of bytes")
        result_tone = {"Failed": "fault", "Cancelled": "attention", "Completed": "active"}
        workbench.tone(self.rx_transfer_value, result_tone.get(rx_state, "neutral") if received_report else "neutral")
        if not incoming_available:
            self.rx_transfer_value.setText("Unavailable")
            self.rx_progress.setValue(0)
            self.rx_rate_value.setText("Unavailable")
            self._rx_rate_tracker.reset()

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
        self.file_mode_combo.setEnabled(connected and not busy and not snapshot.test_mode and snapshot.rx_transfer_state not in {"Receiving", "WaitingForEnd", "Verifying", "Publishing"})
        no_ack = bool(self.file_mode_combo.currentData())
        self.file_mode_hint.setText(
            "Select No ACK on both boards. The receiver sends no replies; delivery is unconfirmed."
            if no_ack else "ACK confirms delivery and retries missing packets."
        )
        self.refresh_button.setEnabled(connected and not busy)
        self.stop_button.setEnabled((connected or busy) and not snapshot.test_mode and self._pending_test_mode is None)
        receiving = snapshot.rx_transfer_state in {"Receiving", "WaitingForEnd", "Verifying", "Publishing"}
        self.stop_button.setText("Cancel Receive" if receiving else "Stop")
        self.stop_button.setToolTip("Cancel the incoming RF file" if receiving else "Stop the current RF operation")
        self.standby_button.setEnabled(controls_enabled)
        self.rx_button.setEnabled(rf_ready)
        self.rx_button.setText("Receive (No ACK)" if no_ack else "Receive")
        self.sleep_button.setEnabled(rf_ready)
        self.wake_button.setEnabled(controls_enabled)
        self.power_down_button.setEnabled(controls_enabled)
        self.apply_channel_button.setEnabled(controls_enabled)
        self.apply_power_button.setEnabled(rf_ready)
        self.transmit_button.setText("Upload && Verify" if snapshot.test_mode else "Transmit (No ACK)" if no_ack else "Transmit")
        self.storage_transmit_button.setText("Transmit Stored File (No ACK)" if no_ack else "Transmit Stored File")
        self.transmit_button.setToolTip(
            "Upload the actual file, read it back from ESP SPIFFS, and compare every byte. No nRF24 TX."
            if snapshot.test_mode else "Transmit the file over nRF24. A matching board copy saves the PC upload."
        )
        self.transmit_button.setEnabled(
            connected and self._controller.selected_file is not None and not busy
            and (snapshot.test_mode or snapshot.confirmed_radio_state in
                 {RadioState.STANDBY, RadioState.RX_LISTENING})
            and self._pending_test_mode is None
            and snapshot.capabilities.supports_host_file_upload
            and (not snapshot.test_mode or snapshot.capabilities.supports_host_file_download)
            and (snapshot.test_mode or not rail_off)
        )
        self.copy_rx_path_button.setEnabled(telemetry_available and bool(rx_path or rx_file))
        self.save_received_button.setEnabled(
            connected
            and not busy
            and snapshot.telemetry.rx_saved > 0
            and snapshot.capabilities.supports_host_file_download
            and not snapshot.test_mode
            and self._pending_test_mode is None
        )
        self.morse_text_edit.setEnabled(controls_enabled)
        self.send_morse_button.setEnabled(rf_ready)
        remote_enabled = rf_ready and snapshot.capabilities.supports_remote_commands
        self.remote_command_edit.setEnabled(remote_enabled)
        self.send_remote_button.setEnabled(remote_enabled)
        text_enabled = connected and not busy and self._pending_test_mode is None and (
            snapshot.capabilities.supports_host_echo if snapshot.test_mode
            else snapshot.capabilities.supports_text_messages
        ) and (snapshot.test_mode or (not rail_off and snapshot.confirmed_radio_state in {RadioState.STANDBY, RadioState.RX_LISTENING}))
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
        self.cw_start_button.setEnabled(rf_ready)
        self.cw_loop_button.setEnabled(rf_ready)
        self.cw_stop_button.setEnabled((connected or busy) and not snapshot.test_mode and self._pending_test_mode is None)
        self.cw_stop_button.setText("Cancel Receive" if receiving else "Stop")

        self.statusBar().showMessage(
            f"{snapshot.connection_state.value} | radio {snapshot.confirmed_radio_state.value}"
            + (" | TEST MODE: no nRF24 TX" if snapshot.test_mode else "")
            + (f" | {rx_state} {snapshot.rx_received_bytes:,}/{snapshot.rx_total_bytes:,} bytes"
               if receiving else "")
        )
        self._apply_storage_controls()

    def _apply_transfer_progress(self, progress: TransferProgress) -> None:
        self.progress_bar.setVisible(progress.total_bytes > 0)
        self.storage_progress.setVisible(progress.total_bytes > 0)
        self.storage_progress.setValue(int(progress.ratio * 1000))
        self.storage_operation.setText(progress.message)
        self.progress_bar.setValue(int(progress.ratio * 1000))
        label = progress.state.value
        if progress.total_bytes:
            label = f"{label}: {progress.sent_bytes}/{progress.total_bytes} bytes"
        self.transfer_state_label.setText(label)
        self.transfer_detail_label.setText(progress.message)
        self.transfer_detail_label.setToolTip(progress.message)

        if progress.state == TransferState.FAILED and not self._closing:
            QMessageBox.warning(self, "Transfer failed", progress.message)

    def _append_log(self, level: str, message: str) -> None:
        if level.lower() == "debug" and not self.show_debug_checkbox.isChecked():
            return
        self.log_output.appendPlainText(f"{level.upper():5} {message}")

    def _on_command_finished(self, _command: str, accepted: bool, message: str) -> None:
        if self._closing or message == "Connection closed before command completion":
            return  # Closing/cancelling a link already has a visible connection result.
        if _command.startswith("STORAGE"):
            self.storage_operation.setText(message)
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
            self.progress_bar.hide()
            self.transfer_state_label.setText("Idle")
            self.upload_name_value.setText(f"SPIFFS upload name: {console_protocol.import_filename(path.name)}")
            self.transfer_detail_label.setText("File Transmit reuses a board copy when its name, size and CRC32 match.")
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
        epoch = self._connection_epoch
        source_name = self._controller.snapshot.telemetry.last_rx_file
        if not source_name:
            QMessageBox.warning(self, "No received file", "The ESP has not reported a received file.")
            return
        path_text, _ = QFileDialog.getSaveFileName(self, "Save received RF3 file", source_name)
        if (path_text and self._dialog_connection_valid(epoch)
                and self._controller.snapshot.telemetry.last_rx_file == source_name):
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

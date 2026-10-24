import json
import os
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
from PySide6.QtWidgets import QApplication, QMessageBox

from rf3_gui.app.controller import AppController
from rf3_gui.app.state_model import (ConnectionState, DeviceInfo, DeviceSnapshot,
    RadioCapabilities, RadioState, StoredFile, StorageListing, Telemetry)
from rf3_gui.device.mock_device import MockDeviceBackend
from rf3_gui.device.serial_device import SerialDeviceBackend
from rf3_gui.ui.main_window import MainWindow


class AuditUIRepairs(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def setUp(self):
        self.backend = MockDeviceBackend()
        self.backend._connected = True
        self.controller = AppController(self.backend)
        self.controller._selected_device = DeviceInfo("RF3 PCB", "board-one", "bluetooth")
        self.window = MainWindow(self.controller)
        self.addCleanup(self.window.close)
        self.snapshot = DeviceSnapshot(connection_state=ConnectionState.CONNECTED,
            profile="custom-pcb", confirmed_radio_state=RadioState.STANDBY,
            channel=76, power_level=3, capabilities=RadioCapabilities(
                supports_host_storage=True, supports_stored_file_tx=True,
                supports_host_file_upload=True, supports_host_file_download=True))
        self.status(self.snapshot)

    def status(self, snapshot):
        self.backend._snapshot = snapshot
        self.backend.status_changed.emit(snapshot)

    def test_manual_and_periodic_refresh_preserve_four_dirty_drafts(self):
        for drafts in ((83, 84, 2, 1), (88, 89, 0, 2)):
            self.window.channel_spin.setValue(drafts[0])
            self.window.cw_channel_spin.setValue(drafts[1])
            self.window.power_combo.setCurrentIndex(drafts[2])
            self.window.cw_power_combo.setCurrentIndex(drafts[3])
            self.window.refresh_button.click()
            self.status(self.snapshot)
            self.assertEqual((self.window.channel_spin.value(), self.window.cw_channel_spin.value(),
                self.window.power_combo.currentData(), self.window.cw_power_combo.currentData()), drafts)
            self.assertEqual(self.window.channel_value.text(), "76")
            self.assertEqual(self.window.power_value.text(), "3")

    def test_failed_apply_keeps_draft_and_success_requires_confirmed_value(self):
        self.window.channel_spin.setValue(83)
        with patch.object(QMessageBox, "warning"):
            self.backend.command_finished.emit("CHANNEL 83", False, "Test rejection")
        self.status(self.snapshot)
        self.assertEqual(self.window.channel_spin.value(), 83)
        self.window.apply_channel_button.click()
        self.assertEqual(self.window.channel_value.text(), "83")
        self.status(replace(self.snapshot, channel=84))
        self.assertEqual(self.window.channel_spin.value(), 84)

    def test_new_connection_initializes_drafts_to_other_board(self):
        self.window.channel_spin.setValue(83)
        self.window.cw_power_combo.setCurrentIndex(1)
        self.status(DeviceSnapshot())
        self.status(replace(self.snapshot, channel=90, power_level=2))
        self.assertEqual(self.window.channel_spin.value(), 90)
        self.assertEqual(self.window.cw_power_combo.currentData(), 2)

    def test_unknown_power_and_fault_do_not_offer_rf_transmit(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "source.bin"
            source.write_bytes(b"data")
            self.controller.set_selected_file(source)
            self.status(replace(self.snapshot, power_level=-1, confirmed_radio_state=RadioState.FAULT,
                telemetry=Telemetry(last_fault=3)))
            self.assertEqual(self.window.power_value.text(), "Unknown")
            self.assertIn("Transmit failed", self.window.fault_value.text())
            self.assertFalse(self.window.transmit_button.isEnabled())
            self.assertFalse(self.window.send_text_button.isEnabled())
            self.assertFalse(self.window.cw_start_button.isEnabled())
            self.assertTrue(self.window.apply_channel_button.isEnabled())

    def test_test_mode_capability_gates_checkbox_by_board(self):
        self.assertFalse(self.window.test_mode_checkbox.isEnabled())
        devboard = replace(self.snapshot, profile="esp32-devboard", capabilities=replace(
            self.snapshot.capabilities, supports_test_mode=True))
        self.status(devboard)
        self.assertTrue(self.window.test_mode_checkbox.isEnabled())

    def test_error_disables_modes_and_saved_file_actions(self):
        self.status(replace(self.snapshot, connection_state=ConnectionState.ERROR))
        self.assertFalse(self.window.test_mode_checkbox.isEnabled())
        self.assertFalse(self.window.test_mode_checkbox.isChecked())
        self.assertFalse(self.window.save_received_button.isEnabled())
        self.assertFalse(self.window.storage_transmit_button.isEnabled())
        self.assertEqual(self.window.channel_value.text(), "Unavailable")
        self.assertEqual(self.window.rx_value.text(), "Unavailable")

    def test_closing_window_cancels_pending_command_without_modal(self):
        backend = SerialDeviceBackend()
        backend._serial = SimpleNamespace(close=lambda: None)
        backend._handshake_ok = True
        backend._snapshot = self.snapshot
        controller = AppController(backend)
        window = MainWindow(controller)
        backend.set_channel(83)
        self.assertIn("CHANNEL 83", backend._pending_commands)
        with patch.object(QMessageBox, "warning") as warning:
            window.close()
        self.assertIsNone(backend._serial)
        self.assertFalse(backend._pending_commands)
        warning.assert_not_called()

    def test_stored_file_select_and_transmit_use_selected_row(self):
        filename = 'résumé "one".bin'
        self.backend._stored_files[filename] = b"stored content"
        self.controller._on_storage_changed(StorageListing((StoredFile(filename, 14),), 14, 1024, True))
        self.window.storage_table.selectRow(0)
        self.assertTrue(self.window.storage_select_button.isEnabled())
        self.window.storage_select_button.click()
        self.assertEqual(self.window.selected_file_value.text(), filename)
        self.window.storage_transmit_button.click()
        self.assertEqual(self.window.active_tx_file_value.text(), filename)
        self.assertIn("Mock stored-file TX", self.window.transfer_detail_label.text())

    def test_pc_source_and_esp_selected_file_are_distinct_labels(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "long source with spaces.bin"
            source.write_bytes(b"a")
            with patch("rf3_gui.ui.main_window.QFileDialog.getOpenFileName", return_value=(str(source), "")):
                self.window._choose_file()
            self.status(replace(self.snapshot, selected_file="other.bin", active_tx_file="active.bin"))
            self.assertEqual(self.window.file_path_edit.text(), str(source))
            self.assertIn("SPIFFS upload name:", self.window.upload_name_value.text())
            self.assertNotIn("long source with spaces", self.window.upload_name_value.text())
            self.assertEqual(self.window.selected_file_value.text(), "other.bin")
            self.assertEqual(self.window.active_tx_file_value.text(), "active.bin")

    def test_save_dialog_disconnect_does_not_start_download(self):
        self.status(replace(self.snapshot, telemetry=Telemetry(rx_saved=1, last_rx_file="rx.bin")))
        def disconnect_during_dialog(*args):
            self.status(DeviceSnapshot())
            return "unopened-output.bin", ""
        with patch("rf3_gui.ui.main_window.QFileDialog.getSaveFileName", side_effect=disconnect_during_dialog), \
                patch.object(self.backend, "save_received_file") as save:
            self.window._save_received_file()
        save.assert_not_called()

    def test_dialog_disconnect_and_reconnect_cannot_target_another_board(self):
        self.status(replace(self.snapshot, telemetry=Telemetry(rx_saved=1, last_rx_file="rx.bin")))
        def switch_during_dialog(*args):
            self.status(DeviceSnapshot())
            self.status(replace(self.snapshot, telemetry=Telemetry(rx_saved=1, last_rx_file="rx.bin")))
            return "unopened-output.bin", ""
        with patch("rf3_gui.ui.main_window.QFileDialog.getSaveFileName", side_effect=switch_during_dialog), \
                patch.object(self.backend, "save_received_file") as save:
            self.window._save_received_file()
        save.assert_not_called()


if __name__ == "__main__":
    unittest.main()

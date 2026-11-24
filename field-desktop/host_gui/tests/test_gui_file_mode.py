import unittest
from dataclasses import replace
from unittest.mock import patch

from PySide6.QtWidgets import QApplication
from rf3_gui.app.controller import AppController
from rf3_gui.app.state_model import ConnectionState, RadioState
from rf3_gui.device.mock_device import MockDeviceBackend
from rf3_gui.ui.main_window import MainWindow


class TestGUIFileMode(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def setUp(self):
        self.backend = MockDeviceBackend()
        self.backend._connected = True
        self.controller = AppController(self.backend)
        self.window = MainWindow(self.controller)
        self.snapshot = replace(self.backend._snapshot,
                                connection_state=ConnectionState.CONNECTED,
                                confirmed_radio_state=RadioState.STANDBY)
        self.backend._snapshot = self.snapshot
        self.backend.status_changed.emit(self.snapshot)
        self.addCleanup(self.window.close)

    def test_default_is_reliable_with_clear_choices(self):
        self.assertEqual(self.window.file_mode_combo.currentText(), "Reliable (ACK)")
        self.assertEqual(self.window.file_mode_combo.itemText(1), "No ACK (one-way)")
        self.assertFalse(self.backend._no_ack_selected)

    def test_selection_updates_backend_and_both_transmit_controls(self):
        self.window.file_mode_combo.setCurrentIndex(1)
        self.assertTrue(self.backend._no_ack_selected)
        self.assertEqual(self.window.rx_button.text(), "Receive (No ACK)")
        self.assertEqual(self.window.transmit_button.text(), "Transmit (No ACK)")
        self.assertIn("No ACK", self.window.storage_transmit_button.text())
        self.assertIn("unconfirmed", self.window.file_mode_hint.text())

    def test_rejected_mode_change_reverts_selection(self):
        with patch.object(self.backend, "set_no_ack_mode", return_value=False):
            self.window.file_mode_combo.setCurrentIndex(1)
        self.assertFalse(self.window.file_mode_combo.currentData())
        self.assertEqual(self.window.rx_button.text(), "Receive")

    def test_busy_device_disables_mode_selection(self):
        self.backend.status_changed.emit(replace(self.snapshot, connection_state=ConnectionState.BUSY))
        self.assertFalse(self.window.file_mode_combo.isEnabled())

    def test_status_refresh_keeps_selected_mode_and_reliable_can_be_restored(self):
        self.window.file_mode_combo.setCurrentIndex(1)
        self.backend.status_changed.emit(replace(self.snapshot, channel=40))
        self.assertTrue(self.window.file_mode_combo.currentData())
        self.window.file_mode_combo.setCurrentIndex(0)
        self.assertFalse(self.backend._no_ack_selected)
        self.assertEqual(self.window.transmit_button.text(), "Transmit")

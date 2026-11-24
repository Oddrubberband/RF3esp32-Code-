import json
import os
import unittest
from dataclasses import replace
from types import SimpleNamespace
from unittest.mock import patch

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
from PySide6.QtWidgets import QApplication
from rf3_gui.app.controller import AppController
from rf3_gui.app.state_model import ConnectionState, DeviceSnapshot, RadioState
from rf3_gui.device.serial_device import SerialDeviceBackend
from rf3_gui.ui.main_window import MainWindow
from rf3_gui.build_info import DISPLAY_NAME


class TestRxFixCandidate(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def setUp(self):
        warnings = patch("rf3_gui.ui.main_window.QMessageBox.warning")
        warnings.start()
        self.addCleanup(warnings.stop)
        self.backend = SerialDeviceBackend()
        self.backend._serial = SimpleNamespace(close=lambda: None)
        self.backend._identity_ok = True
        self.backend._handshake_ok = True
        self.backend._snapshot = DeviceSnapshot(connection_state=ConnectionState.CONNECTED)
        self.controller = AppController(self.backend)
        self.window = MainWindow(self.controller)
        self.addCleanup(self.window.close)
        self.events = []
        self.backend.command_finished.connect(lambda *args: self.events.append(args))

    def status(self, **changes):
        values = dict(state="RxListening", profile="custom-pcb", channel=76, power=3,
                      rx_id="AABB0001", rx_state="Receiving", rx_bytes="0/8192", rx_error="none")
        values.update(changes)
        self.backend._handle_line(json.dumps(values))

    def test_real_backend_unavailable_keeps_receive_progress_rate_and_guards(self):
        with patch("rf3_gui.app.receive_rate.time.monotonic", return_value=10):
            self.status()
        with patch("rf3_gui.app.receive_rate.time.monotonic", return_value=14):
            self.status(state="Unavailable", rx_bytes="4096/8192")
        self.assertEqual(self.backend._snapshot.confirmed_radio_state, RadioState.UNKNOWN)
        self.assertEqual(self.window.rx_progress.value(), 500)
        self.assertIn("Receiving", self.window.rx_transfer_value.text())
        self.assertIn("1.00 KiB/s", self.window.rx_rate_value.text())
        self.assertIn("4.0 s observed", self.window.rx_rate_value.text())
        self.assertFalse(self.window.apply_channel_button.isEnabled())
        self.assertFalse(self.window.transmit_button.isEnabled())
        self.assertTrue(self.window.stop_button.isEnabled())
        self.assertEqual(self.window.channel_value.text(), "Unavailable")
        with patch("rf3_gui.app.receive_rate.time.monotonic", return_value=16):
            self.status(state="Unavailable", rx_bytes="6144/8192")
        self.assertEqual(self.window.rx_progress.value(), 750)
        self.assertIn("6.0 s observed", self.window.rx_rate_value.text())

    def test_disconnect_clears_even_retained_transfer_and_reconnect_has_no_invented_rate(self):
        self.status(rx_bytes="4096/8192")
        stale = replace(self.backend._snapshot, connection_state=ConnectionState.DISCONNECTED)
        self.backend.status_changed.emit(stale)
        self.assertEqual(self.window.rx_progress.value(), 0)
        self.assertEqual(self.window.rx_rate_value.text(), "Unavailable")
        self.assertFalse(self.window.diagnostics_button.isEnabled())
        self.status(rx_state="Completed", rx_bytes="8192/8192")
        self.assertEqual(self.window.rx_progress.value(), 1000)
        self.assertNotIn("KiB/s", self.window.rx_rate_value.text())

    def test_unavailable_without_transfer_remains_unavailable(self):
        self.status(state="Unavailable", rx_id="00000000", rx_state="Idle", rx_bytes="0/0")
        self.assertEqual(self.window.rx_progress.value(), 0)
        self.assertEqual(self.window.rx_transfer_value.text(), "Unavailable")
        self.assertIn(DISPLAY_NAME, self.window.windowTitle())
        self.assertEqual(self.window.file_mode_combo.count(), 2)

    def identity(self, diagnostics=True):
        self.backend._handle_line(json.dumps(dict(rf3_protocol=1, protocol=2,
            profile="custom-pcb", node_name="rf3-pcb", diagnostics=diagnostics)))

    def diag(self, **changes):
        values = dict(type="diag", v=1, build="RX-FIX-R1-20261102", profile="custom-pcb",
            tick_hz=1000, tx_ds=12, tx_fifo_only=3, tx_recoveries=0,
            tx_attempt_timeouts=0, ack_timeouts=2, rejected_responses=1)
        values.update(changes)
        self.backend._handle_line(json.dumps(values))

    def test_diagnostics_are_on_demand_and_do_not_replace_status_or_finish_on_ok(self):
        self.identity()
        self.status(rx_bytes="4096/8192")
        before = self.backend._snapshot
        self.assertNotIn("DIAG", self.backend._pending_commands)
        self.controller.request_diagnostics()
        self.assertIn("DIAG", self.backend._pending_commands)
        self.backend._handle_line("OK DIAG")
        self.assertIn("DIAG", self.backend._pending_commands)
        self.diag()
        self.assertNotIn("DIAG", self.backend._pending_commands)
        self.assertIs(before, self.backend._snapshot)
        self.assertEqual(self.window.rx_progress.value(), 500)
        self.assertIn("RX-FIX-R1-20261102", self.window.firmware_build_label.text())
        self.assertIn("1000 Hz", self.window.firmware_build_label.text())

    def test_old_firmware_and_pc_transfer_do_not_send_diag(self):
        self.identity(diagnostics=False)
        self.backend.request_diagnostics()
        self.assertNotIn("DIAG", self.backend._pending_commands)
        self.assertFalse(self.events[-1][1])
        self.identity()
        self.backend._storage_kind = "list"
        self.backend.request_diagnostics()
        self.assertNotIn("DIAG", self.backend._pending_commands)
        self.backend._storage_kind = None

    def test_invalid_unsolicited_and_late_diagnostics_cannot_change_ui(self):
        self.identity()
        self.diag()
        self.assertNotIn("1000 Hz", self.window.firmware_build_label.text())

    def test_malformed_diagnostics_profile_and_counters_fail_without_disconnect(self):
        self.identity()
        before = self.backend._snapshot
        for changes in ({"profile": []}, {"profile": {}}, {"tx_ds": -1},
                        {"tx_ds": True}, {"tick_hz": "1000"}, {"v": True}):
            self.backend.request_diagnostics()
            self.diag(**changes)
            self.assertNotIn("DIAG", self.backend._pending_commands)
            self.assertFalse(self.events[-1][1])
            self.assertIsNotNone(self.backend._serial)
            self.assertIs(self.backend._snapshot, before)

    def test_invalid_or_late_diagnostics_cannot_finish_after_disconnect(self):
        self.identity()
        self.backend.request_diagnostics()
        self.diag(tx_ds=True)
        self.assertFalse(self.events[-1][1])
        self.assertNotIn("DIAG", self.backend._pending_commands)
        self.backend.request_diagnostics()
        self.backend.disconnect_from_device()
        self.diag()
        self.assertFalse(self.backend._diagnostics_supported)
        self.assertNotIn("1000 Hz", self.window.firmware_build_label.text())


if __name__ == "__main__":
    unittest.main()

import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from PySide6.QtWidgets import QApplication

from rf3_gui.app.state_model import ConnectionState, RadioState
from rf3_gui.device.serial_device import SerialDeviceBackend


class SerialRepairs(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def setUp(self):
        self.backend = SerialDeviceBackend()
        self.backend._serial = SimpleNamespace(close=lambda: None)
        self.backend._identity_ok = self.backend._handshake_ok = True
        self.addCleanup(self.backend.disconnect_from_device)
        self.outcomes = []
        self.backend.command_finished.connect(lambda *event: self.outcomes.append(event))
        self.status()

    def status(self, **changes):
        values = dict(state="Standby", channel=0, power=3, loop="None",
                      host_storage=True, host_upload=True, host_download=True, host_stored_tx=True,
                      file_guard=True, upload_create=True, no_ack_files=True, tx_no_ack=False,
                      tx_state="Idle", rx_attempt=0, rx_state="Idle")
        self.backend._handle_line(json.dumps(values | changes))

    def sent(self):
        return self.backend._write_queue.get_nowait()

    def test_firmware_busy_retries_status_under_same_watchdog(self):
        for reply in ("Command system is busy.", "ERR STATUS command_busy_retry"):
            self.backend.request_status()
            self.assertEqual(self.sent(), "STATUS")
            self.backend._handle_line(reply)
            self.assertTrue(self.backend._status_response_timer.isActive())
            self.backend.request_status()
            self.assertEqual(self.sent(), "STATUS")
            self.status()
            self.assertFalse(self.backend._status_response_timer.isActive())

    def test_correlated_radio_error_preserves_connection(self):
        self.status(state="Sleep")
        self.backend.wake()
        self.assertEqual(self.sent(), "WAKE")
        self.backend._handle_line("Could not wake radio. state=Fault fault=3")
        self.assertEqual(self.outcomes[-1][:2], ("WAKE", False))
        self.assertEqual(self.sent(), "STATUS")
        self.assertIsNotNone(self.backend._serial)
        self.assertEqual(self.backend._snapshot.requested_radio_state, RadioState.SLEEP)

    def test_rejected_bluetooth_stop_disarms_cancel_watchdog(self):
        current = dict(state="RxListening", rx_state="Receiving", rx_id="00000022",
                       rx_attempt=1, rx_bytes="20/40")
        self.status(**current)
        self.backend.stop()
        self.assertEqual(self.sent(), "STOP")
        self.assertTrue(self.backend._rx_cancel_timer.isActive())
        self.backend._handle_line("ERR STOP")
        self.assertEqual(self.outcomes[-1][:2], ("STOP", False))
        self.assertFalse(self.backend._rx_cancel_timer.isActive())
        self.status(**current)
        self.backend._on_rx_cancel_timeout()
        self.assertIsNotNone(self.backend._serial)

    def test_diagnostics_text_cannot_impersonate_rx_ack(self):
        self.backend.enter_rx()
        self.assertEqual(self.sent(), "RX")
        self.backend._handle_line("I (9203) rf3: Bluetooth command: MORSE RX listening on channel 22")
        self.assertEqual(self.backend._snapshot.channel, 0)
        self.assertIn("RX", self.backend._pending_commands)
        self.backend._handle_line("RX listening on channel 0")
        self.assertNotIn("RX", self.backend._pending_commands)

    def test_busy_loop_is_visible_before_next_status(self):
        self.backend.start_cw_loop(25, 2000, 0, 0)
        self.assertEqual(self.sent(), "CW LOOP 25 2000 0 0")
        self.backend._handle_line("CW LOOP queued on channel 0 at power level 0 (25 ms on, 2000 ms off).")
        self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.BUSY)
        self.assertEqual(self.sent(), "STATUS")
        self.backend.set_channel(1)
        self.assertTrue(self.backend._write_queue.empty())

    def test_unavailable_reply_counts_and_first_fast_rx_is_sampled(self):
        fresh, received = [], []
        self.backend.fresh_status_report.connect(lambda: fresh.append(True))
        self.backend.rf_sample.connect(lambda *report: received.append(report))
        self.backend.request_status()
        self.assertEqual(self.sent(), "STATUS")
        self.status(state="Unavailable", loop="Morse")
        self.assertEqual(len(fresh), 1)
        self.status(state="RxListening", rx_state="Completed", rx_attempt=1,
                    rx_id="00000022", rx_bytes="20/20", rx_seq="1/1")
        self.assertEqual(received, [("RX", "00000022", "Completed", 20, 20)])

    def test_deleted_rx_file_is_cleared_before_status_poll(self):
        self.status(rx_saved=1, rx_last="rx_00000022.bin")
        self.backend.delete_file("rx_00000022.bin")
        self.assertTrue(self.sent().startswith("STORAGE DELETE "))
        self.backend._handle_line('{"storage":{"op":"delete","name":"rx_00000022.bin"}}')
        self.assertEqual(self.sent(), "STORAGE LIST 0")
        self.assertFalse(self.backend._snapshot.telemetry.last_rx_file)

    def test_export_cleanup_blocks_next_export_and_stop(self):
        with tempfile.TemporaryDirectory() as directory:
            self.backend.export_file("old.bin", Path(directory) / "old.bin")
            self.assertTrue(self.sent().startswith("DOWNLOAD BEGINHEX "))
            self.backend._handle_line("DOWNLOAD ERROR file_not_found_or_too_large")
            self.assertEqual(self.sent(), "DOWNLOAD CANCEL")
            self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.BUSY)
            self.backend.export_file("next.bin", Path(directory) / "next.bin")
            self.backend.stop()
            self.assertTrue(self.backend._write_queue.empty())
            self.backend._handle_line("DOWNLOAD CANCELLED")
            self.assertEqual(self.sent(), "STATUS")
            self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.CONNECTED)
            self.assertFalse(any(name == "STOP" for name, _, _ in self.outcomes))

    def test_scoped_host_cancel_rejection_is_reported_immediately(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "notes.bin"
            path.write_bytes(b"hello")
            self.backend.import_file(path)
            self.assertTrue(self.sent().startswith("UPLOAD BEGINCREATE notes.bin "))
            self.backend.stop()
            self.assertEqual(self.sent(), "UPLOAD CANCEL")
            self.backend._handle_line("UPLOAD ERROR transfer_owned_by_another_session")
            self.assertTrue(self.backend._host_transfer_timer.isActive())
            self.backend._handle_line("UPLOAD CANCEL ERROR transfer_owned_by_another_session")
            self.assertEqual(self.outcomes[-1][:2], ("STOP", False))
            self.assertFalse(self.backend._host_transfer_timer.isActive())
            self.assertIsNotNone(self.backend._serial)

    def test_legacy_test_mode_does_not_write_when_lookup_misses(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "notes.bin"
            path.write_bytes(b"hello")
            self.status(file_guard=False, upload_create=False, test_mode=True)
            self.backend.start_tx(path)
            self.assertTrue(self.sent().startswith("DOWNLOAD BEGINHEX "))
            self.backend._handle_line("DOWNLOAD ERROR file_not_found_or_too_large")
            self.assertEqual(self.sent(), "DOWNLOAD CANCEL")
            self.assertTrue(self.backend._write_queue.empty())

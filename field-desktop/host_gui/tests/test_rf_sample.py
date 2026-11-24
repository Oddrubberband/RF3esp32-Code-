import csv
import json
from datetime import datetime, timedelta
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from PySide6.QtWidgets import QApplication

from rf3_gui.app.controller import AppController
from rf3_gui.app.state_model import ConnectionState, DeviceSnapshot, RadioState
from rf3_gui.device.mock_device import MockDeviceBackend
from rf3_gui.device.serial_device import SerialDeviceBackend
from rf3_gui.ui.metrics_page import MetricsPage


class TestRfSamples(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def setUp(self):
        self.backend = SerialDeviceBackend()
        self.backend._serial = SimpleNamespace(close=lambda: None)
        self.backend._handshake_ok = True
        self.backend._identity_ok = True
        self.backend._snapshot = DeviceSnapshot(connection_state=ConnectionState.CONNECTED)
        self.controller = AppController(self.backend)
        self.samples = []
        self.controller.rf_sample.connect(lambda *args: self.samples.append(args))
        self.addCleanup(self.backend.disconnect_from_device)

    def status(self, **changes):
        values = dict(state="RxListening", profile="custom-pcb", channel=76, power=3,
                      rx_state="Idle", rx_id="00000000", rx_bytes="0/0",
                      tx_state="Idle", tx_id="00000000", tx_file="", tx_bytes="0/0",
                      peer_complete=False, host_storage=True, host_stored_tx=True)
        values.update(changes)
        self.backend._handle_line(json.dumps(values))

    def test_export_buttons_write_directional_board_history_after_disconnect(self):
        page = MetricsPage(self.controller)
        self.addCleanup(page.close)
        self.assertFalse(page.export_buttons["TX"].isEnabled())
        self.assertFalse(page.export_buttons["RX"].isEnabled())
        with patch("rf3_gui.ui.metrics_page.monotonic", return_value=1):
            self.tx(tx_bytes="20/100", tx_seq="1/5", tx_crc32="00abcd01", tx_retries=2)
        with patch("rf3_gui.ui.metrics_page.monotonic", return_value=65):
            self.tx(tx_bytes="60/100", tx_seq="3/5", tx_crc32="00abcd01", tx_retries=3)
            self.rx(rx_bytes="80/100", rx_seq="4/5", rx_crc32="00000000")
            self.rx(rx_bytes="100/100", rx_seq="5/5", rx_crc32="00abcd01", rx_state="Completed")
            self.rx(rx_id="ABCD0002", rx_bytes="0/100", rx_seq="bad", rx_crc32="invalid")
        page.on_status(DeviceSnapshot())
        with tempfile.TemporaryDirectory() as temp:
            for direction in ("TX", "RX"):
                dest = Path(temp) / (direction + ".csv")
                button = page.export_buttons[direction]
                self.assertTrue(button.isEnabled())
                with patch("rf3_gui.ui.metrics_page.QFileDialog.getSaveFileName", return_value=(str(dest), "")):
                    button.click()
                with dest.open(newline="") as f:
                    rows = list(csv.DictReader(f))
                self.assertTrue(all(r["direction"] == direction and r["source"] == "board_status" for r in rows))
                self.assertEqual(datetime.fromisoformat(rows[0]["timestamp_utc"]).utcoffset(), timedelta(0))
                if direction == "TX":
                    self.assertEqual([r["packet_number"] for r in rows], ["1", "3"])
                    self.assertEqual(rows[-1]["file_crc32_hex"], "0x00ABCD01")
                    self.assertEqual(rows[-1]["tx_retries"], "3")
                    self.assertEqual(float(rows[-1]["average_Bps"]), 0.625)
                    self.assertEqual(float(rows[-1]["payload_eta_s"]), 64)
                else:
                    self.assertEqual([r["packet_number"] for r in rows], ["4", "5", ""])
                    self.assertEqual([r["file_crc32_hex"] for r in rows], ["0x00000000", "0x00ABCD01", ""])
                    self.assertEqual(rows[1]["phase"], "Completed")
                    self.assertEqual(rows[1]["total_packets"], "5")
                    self.assertEqual(rows[2]["transfer_id"], "ABCD0002")

    def rx(self, **changes):
        values = dict(rx_state="Receiving", rx_id="ABCD0001", rx_bytes="20/100")
        values.update(changes)
        self.status(**values)

    def tx(self, **changes):
        values = dict(tx_state="SendingData", tx_id="12340001", tx_file="report.bin",
                      tx_bytes="20/100", tx_no_ack=False)
        values.update(changes)
        self.status(**values)

    def test_metrics_follow_selected_serial_transfer_in_both_directions(self):
        page = MetricsPage(self.controller)
        self.addCleanup(page.close)
        with patch("rf3_gui.ui.metrics_page.monotonic") as clock:
            clock.return_value = 1
            self.rx(rx_bytes="0/100")
            clock.return_value = 3
            self.rx(rx_bytes="60/100")
            page.refresh()
            self.assertEqual((page.rate.text(), page.average.text()), ("30 B/s", "30 B/s"))
            clock.return_value = 6
            self.rx(rx_state="Completed", rx_bytes="100/100")
            clock.return_value = 10
            self.tx(tx_bytes="0/100")
            clock.return_value = 12
            self.tx(tx_bytes="80/100")
            page.refresh()
            self.assertEqual(page.average.text(), "40 B/s")
            page.track.setCurrentIndex(page.track.findData("RX"))
            self.assertEqual(page.average.text(), "20 B/s")
            self.assertEqual(page.plot.trace.phase, "Completed")
            page.track.setCurrentIndex(page.track.findData("TX"))
            self.assertEqual(page.average.text(), "40 B/s")
            self.assertEqual(page.plot.trace.points[-1].count, 80)

    def test_rx_fresh_polls_allow_zero_rate_but_cached_or_late_data_do_not(self):
        self.rx()
        self.rx()
        self.assertEqual(self.samples, [("RX", "ABCD0001", "Receiving", 20, 100)] * 2)
        self.backend.status_changed.emit(self.backend._snapshot)
        self.backend._handle_line(json.dumps(dict(state="RxListening", channel=76)))
        self.rx(rx_bytes="10/100")
        self.assertEqual(len(self.samples), 2)
        self.rx(rx_state="Completed", rx_bytes="100/100")
        self.assertEqual(self.samples[-1], ("RX", "ABCD0001", "Completed", 100, 100))
        self.rx(rx_state="Completed", rx_bytes="100/100")
        self.rx(rx_bytes="100/100")
        self.assertEqual(len(self.samples), 3)
        self.rx(rx_id="ABCD0002", rx_bytes="0/200")
        self.assertEqual(self.samples[-1], ("RX", "ABCD0002", "Receiving", 0, 200))
        self.rx(rx_id="ABCD0001", rx_bytes="20/100")
        self.assertEqual(len(self.samples), 4)

    def test_unknown_radio_valid_receive_and_malformed_metadata(self):
        self.rx(state="Unavailable")
        self.assertEqual(self.samples[-1], ("RX", "ABCD0001", "Receiving", 20, 100))
        self.rx(rx_bytes="broken")
        self.rx(rx_id="bad", rx_bytes="40/100")
        self.rx(rx_state="Completed", rx_bytes="99/100")
        self.assertEqual(len(self.samples), 1)

    def test_tx_samples_use_validated_rf_identity_and_terminal_evidence(self):
        self.tx()
        self.tx(tx_bytes="40/100")
        self.assertEqual(self.samples[-1], ("TX", "12340001", "Transferring", 40, 100))
        self.tx(tx_bytes="30/100")
        self.tx(tx_state="Completed", tx_bytes="100/100", peer_complete=False)
        self.assertEqual(len(self.samples), 2)
        self.tx(tx_state="Completed", tx_bytes="100/100", peer_complete=True)
        self.assertEqual(len(self.samples), 2)
        self.assertEqual(self.backend._rf_sample_last["TX"][1], "Failed")
        self.tx(tx_id="12340002", tx_bytes="0/100")
        self.tx(tx_id="12340002", tx_state="Completed", tx_bytes="100/100", peer_complete=True)
        self.assertEqual(self.samples[-1], ("TX", "12340002", "Completed", 100, 100))
        self.tx(tx_id="12340002", tx_state="Completed", tx_bytes="100/100", peer_complete=True)
        self.tx(tx_id="12340002", tx_bytes="100/100")
        self.assertEqual(len(self.samples), 4)

    def test_history_host_sessions_and_test_mode_do_not_create_tx_samples(self):
        self.tx(tx_state="Completed", tx_bytes="100/100", peer_complete=True)
        self.assertEqual(self.samples, [])
        self.backend._upload_session = SimpleNamespace()
        self.tx()
        self.backend._upload_session = None
        self.status(test_mode=True)
        self.tx(test_mode=True)
        self.rx(test_mode=True)
        self.assertEqual(self.samples, [])

    def test_mock_host_operations_are_excluded_and_same_file_gets_unique_ids(self):
        mock = MockDeviceBackend()
        mock._connected = True
        mock._snapshot = DeviceSnapshot(connection_state=ConnectionState.CONNECTED,
                                        confirmed_radio_state=RadioState.STANDBY)
        controller = AppController(mock)
        samples = []
        controller.rf_sample.connect(lambda *args: samples.append(args))
        self.addCleanup(mock.disconnect_from_device)
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / "sample.txt"
            source.write_bytes(b"test" * 30)
            mock.import_file(source)
            mock.export_file("sample.txt", Path(folder) / "export.txt")
            mock.request_status()
            self.assertEqual(samples, [])
            mock.start_tx(source)
            first = samples[-1][1]
            self.assertEqual(samples[-1][2:], ("Preparing", 0, 120))
            mock._advance_transfer()
            mock.stop()
            self.assertEqual(samples[-1][2], "Cancelled")
            mock.start_tx(source)
            self.assertNotEqual(first, samples[-1][1])
            mock._transfer_timer.stop()
            with patch("rf3_gui.device.mock_device.time.monotonic",
                       return_value=mock._transfer_started + mock._transfer_profile.elapsed_for_size(120) + 1):
                mock._advance_transfer()
            self.assertEqual(samples[-1][2:], ("Completed", 120, 120))

    def test_status_cadence_counts_only_accepted_inbound_reports(self):
        reports = []
        self.controller.fresh_status_report.connect(lambda: reports.append(True))
        self.status()
        self.backend.status_changed.emit(self.backend._snapshot)
        self.backend._handle_line("OK STATUS")
        self.backend._handle_line("OK STOP")
        self.backend._handle_line(json.dumps(dict(state="banana")))
        self.backend._handle_line(json.dumps(dict(state="Unavailable")))
        self.assertEqual(len(reports), 1)
        self.rx(state="Unavailable")
        self.assertEqual(len(reports), 2)
        self.rx(state="Unavailable", rx_bytes="broken")
        self.assertEqual(len(reports), 2)
        mock = MockDeviceBackend()
        mock_controller = AppController(mock)
        mock_controller.fresh_status_report.connect(lambda: reports.append(True))
        self.addCleanup(mock.disconnect_from_device)
        mock.request_status()
        self.assertEqual(len(reports), 2)
        mock._connected = True
        mock.enter_standby()
        self.assertEqual(len(reports), 2)
        mock.request_status()
        self.assertEqual(len(reports), 3)

    def test_watchdog_checks_board_then_ends_trace_if_silent(self):
        ended = []
        self.controller.rf_ended.connect(lambda *args: ended.append(args))
        self.status(state="Standby")
        self.backend.start_stored_tx("report.bin")
        self.tx(tx_bytes="40/100")
        observed = list(self.samples)
        self.backend._on_rf_progress_timeout()
        self.assertIn("STATUS", self.backend._pending_commands)
        self.assertEqual(ended, [])
        self.backend._on_rf_progress_timeout()
        self.assertEqual(ended, [("TX", "12340001", "Failed")])
        self.assertEqual(self.samples, observed)
        self.tx(tx_bytes="60/100")
        self.assertEqual(self.samples, observed)
        self.assertEqual(self.backend._rf_sample_last["TX"][2:], (40, 100))

    def start_owned_tx(self):
        self.status(state="Standby")
        self.backend.start_stored_tx("report.bin")
        command = self.backend._write_queue.get_nowait()
        self.assertTrue(command.startswith("TXHEX "))
        self.backend._handle_line("OK " + command)
        self.tx()
        return command

    def test_resume_waits_for_idle_board_and_uses_receiver_progress(self):
        command = self.start_owned_tx()
        with patch("rf3_gui.device.serial_device.time.monotonic") as clock:
            clock.return_value = 10
            self.tx(tx_state="Failed", tx_error="retry-exhausted")
            clock.return_value = 12.9
            self.tx(tx_state="Failed", tx_error="retry-exhausted")
            self.assertTrue(self.backend._write_queue.empty())
            clock.return_value = 14
            self.tx(tx_state="Failed", tx_error="retry-exhausted", state="TxBusy")
            self.assertTrue(self.backend._write_queue.empty())
            self.tx(tx_state="Failed", tx_error="retry-exhausted", loop="Tx")
            self.assertTrue(self.backend._write_queue.empty())
            self.tx(tx_state="Failed", tx_error="retry-exhausted")
            self.assertEqual(self.backend._write_queue.get_nowait(), command)
            self.backend._handle_line("OK " + command)
            self.tx(tx_state="Failed", tx_error="retry-exhausted")
            self.assertTrue(self.backend._write_queue.empty())
            self.tx(tx_id="12340002", tx_bytes="40/100", tx_seq="2/5")
            self.assertEqual(self.samples[-1], ("TX", "12340002", "Transferring", 40, 100))
            self.tx(tx_state="Failed", tx_error="retry-exhausted")
            self.assertEqual(self.backend._rf_tx_id, "12340002")
            self.tx(tx_id="12340002", tx_state="Completed", tx_bytes="100/100", peer_complete=True)
            self.assertIsNone(self.backend._rf_tx_name)
            self.assertEqual(self.samples[-1][2], "Completed")

    def test_timeout_retry_budget_is_per_user_transmission(self):
        command = self.start_owned_tx()
        with patch("rf3_gui.device.serial_device.time.monotonic") as clock:
            for index in range(4):
                terminal = dict(tx_id=f"{0x12340001+index:08X}", tx_state="Failed", tx_error="timeout")
                clock.return_value = 10 * index
                self.tx(**terminal)
                clock.return_value += 4
                self.tx(**terminal)
                if index < 3:
                    self.assertEqual(self.backend._write_queue.get_nowait(), command)
                    self.backend._handle_line("OK " + command)
            self.assertTrue(self.backend._write_queue.empty())
            self.assertIsNone(self.backend._rf_tx_name)
        self.start_owned_tx()
        self.assertFalse(self.backend._rf_retried_ids)

    def test_stop_cancels_scheduled_retry_even_on_late_timeout(self):
        self.start_owned_tx()
        self.tx(tx_state="Failed", tx_error="timeout")
        self.backend.stop()
        self.assertEqual(self.backend._write_queue.get_nowait(), "STOP")
        self.tx(tx_state="Failed", tx_error="timeout")
        self.assertTrue(self.backend._write_queue.empty())
        self.assertIsNone(self.backend._rf_resume_due)

    def test_error_and_passively_observed_timeout_do_not_retry(self):
        self.start_owned_tx()
        self.tx(tx_state="Failed", tx_error="crc-mismatch")
        self.assertIsNone(self.backend._rf_tx_name)
        self.tx(tx_id="12340002")
        self.tx(tx_id="12340002", tx_state="Failed", tx_error="timeout")
        self.assertIsNone(self.backend._rf_resume_due)
        self.assertTrue(self.backend._write_queue.empty())

    def test_no_ack_timeout_and_disconnect_leave_no_retry(self):
        self.backend._begin_rf_transfer("report.bin", no_ack=True)
        self.tx(tx_no_ack=True, tx_state="Failed", tx_error="timeout")
        self.assertIsNone(self.backend._rf_resume_due)
        self.start_owned_tx()
        self.tx(tx_state="Failed", tx_error="timeout")
        self.assertIsNotNone(self.backend._rf_resume_due)
        self.backend.disconnect_from_device()
        self.assertIsNone(self.backend._rf_resume_due)

    def test_inferred_receive_failure_and_stop_end_observed_trace_only(self):
        ended = []
        self.controller.rf_ended.connect(lambda *args: ended.append(args))
        self.rx()
        self.status()
        self.assertEqual(ended, [("RX", "ABCD0001", "Failed")])
        self.assertEqual(len(self.samples), 1)
        self.rx(rx_id="ABCD0002", rx_bytes="35/100")
        self.backend._rx_cancel_requested = True
        self.backend._handle_line(json.dumps(dict(state="Standby", channel=76)))
        self.assertEqual(ended[-1], ("RX", "ABCD0002", "Cancelled"))
        self.assertEqual(self.backend._rf_sample_last["RX"][2:], (35, 100))
        self.assertEqual(len(self.samples), 2)

    def test_valid_terminal_sample_does_not_also_send_lifecycle_end(self):
        ended = []
        self.controller.rf_ended.connect(lambda *args: ended.append(args))
        self.status(state="Standby")
        self.backend.start_stored_tx("report.bin")
        self.tx(tx_bytes="40/100")
        self.tx(tx_state="Completed", tx_bytes="100/100", peer_complete=True)
        self.assertEqual(self.samples[-1][2], "Completed")
        self.assertEqual(ended, [])


if __name__ == "__main__":
    unittest.main()

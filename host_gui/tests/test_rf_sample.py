import json
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

    def test_local_tx_timeout_ends_trace_without_a_new_byte_sample(self):
        ended = []
        self.controller.rf_ended.connect(lambda *args: ended.append(args))
        self.status(state="Standby")
        self.backend.start_stored_tx("report.bin")
        self.tx(tx_bytes="40/100")
        observed = list(self.samples)
        self.backend._on_rf_progress_timeout()
        self.assertEqual(ended, [("TX", "12340001", "Failed")])
        self.assertEqual(self.samples, observed)
        self.tx(tx_bytes="60/100")
        self.assertEqual(self.samples, observed)
        self.assertEqual(self.backend._rf_sample_last["TX"][2:], (40, 100))

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

from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
import json
import tempfile
import unittest

from PySide6.QtWidgets import QApplication

from rf3_gui.app.controller import AppController
from rf3_gui.app.state_model import ConnectionState, RadioState, TransferState
from rf3_gui.device.serial_device import SerialDeviceBackend
from rf3_gui.protocol import console_protocol
from rf3_gui.ui.main_window import MainWindow


class R23Repairs(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def setUp(self):
        self.backend = SerialDeviceBackend()
        self.backend._serial = SimpleNamespace(close=lambda: None)
        self.backend._identity_ok = self.backend._handshake_ok = True
        self.addCleanup(self.backend.disconnect_from_device)
        self.progress = []
        self.backend.transfer_changed.connect(self.progress.append)
        self.status()

    def status(self, **changes):
        base = dict(state="Standby", channel=0, power=3, loop="None", text="enabled",
                    host_storage=True, host_upload=True, host_download=True, host_stored_tx=True,
                    file_guard=True, no_ack_files=True, tx_no_ack=False, tx_state="Idle",
                    tx_id="00000000", tx_file="", tx_bytes="0/0", peer_complete=False)
        self.backend._handle_line(json.dumps(base | changes))

    def next_command(self):
        return self.backend._write_queue.get_nowait()

    def stage_to_end(self, source, test_mode=False):
        self.status(test_mode=test_mode)
        self.backend.start_tx(source)
        name = console_protocol.staged_filename(source.name, b"abc", verify_only=test_mode)
        encoded = name.encode().hex().upper()
        first = (source.name if console_protocol.valid_storage_name(source.name)
                 else console_protocol.import_filename(source.name))
        self.assertEqual(self.next_command(), f"DOWNLOAD BEGINHEX {first.encode().hex().upper()}")
        self.backend._handle_line("DOWNLOAD ERROR file_not_found_or_too_large")
        self.assertEqual(self.next_command(), f"DOWNLOAD BEGINHEX {encoded}")
        self.backend._handle_line("DOWNLOAD ERROR file_not_found_or_too_large")
        self.assertEqual(self.next_command(), f"UPLOAD BEGINNEW {name} 3 352441C2")
        self.backend._handle_line(f"UPLOAD READY {name} 3 352441C2")
        self.assertEqual(self.next_command(), "UPLOAD DATA 0 616263")
        self.backend._handle_line("UPLOAD ACK 0 3")
        self.assertEqual(self.next_command(), "UPLOAD END")
        return name, encoded

    def test_create_only_staging_and_guarded_transmit(self):
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder, "draft..PDF")
            source.write_bytes(b"abc")
            name, encoded = self.stage_to_end(source)
            self.assertTrue(name.endswith(".PDF"))
            self.assertTrue(console_protocol.import_filename(source.name).endswith(".PDF"))
            self.backend._handle_line(f"UPLOAD COMPLETE {name} 3 352441C2")
            self.assertEqual(self.next_command(), f"TXHEX {encoded} 3 352441C2")
            self.backend._handle_line(f"OK TXHEX {encoded} 3 352441C2")
            self.status(tx_state="Completed", tx_id="000000AA", tx_file=name, tx_bytes="3/3",
                        tx_crc32="352441C3", tx_error="none", peer_complete=True)
            self.assertEqual(self.progress[-1].state, TransferState.FAILED)

    def test_late_upload_stop_reports_commit(self):
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder, "notes.bin")
            source.write_bytes(b"abc")
            name, _ = self.stage_to_end(source)
            self.backend.stop()
            self.assertEqual(self.next_command(), f"UPLOAD CANCEL RESULT {name} 3 352441C2")
            self.backend._handle_line(f"UPLOAD COMPLETE {name} 3 352441C2")
            self.backend._handle_line(f"UPLOAD STORED {name} 3 352441C2")
            self.assertIn("already stored", self.progress[-1].message)
            self.assertEqual(self.next_command(), "STATUS")

    def test_test_mode_stop_and_guarded_readback(self):
        window = MainWindow(AppController(self.backend))
        self.addCleanup(window.close)
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder, "a.abcdefghijklmnopqrs")
            source.write_bytes(b"abc")
            name, encoded = self.stage_to_end(source, test_mode=True)
            self.assertTrue(window.stop_button.isEnabled())
            self.backend._handle_line(f"UPLOAD COMPLETE {name} 3 352441C2")
            self.assertEqual(self.next_command(), f"DOWNLOAD BEGINHEX {encoded}")
            window.stop_button.click()
            self.assertEqual(self.next_command(), "DOWNLOAD CANCEL")
            self.backend._handle_line("DOWNLOAD CANCELLED")
            self.assertIn("remains on the ESP", self.progress[-1].message)

    def test_test_mode_reads_back_the_existing_verified_name(self):
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder, "notes.bin")
            source.write_bytes(b"abc")
            self.status(test_mode=True)
            self.backend.start_tx(source)
            lookup = "DOWNLOAD BEGINHEX 6E6F7465732E62696E"
            self.assertEqual(self.next_command(), lookup)
            self.backend._handle_line("DOWNLOAD READYHEX 6E6F7465732E62696E 3 352441C2")
            self.assertEqual(self.next_command(), "DOWNLOAD CANCEL")
            self.backend._handle_line("DOWNLOAD CANCELLED")
            self.assertEqual(self.next_command(), lookup)

    def test_stored_transfer_and_resume_carry_crc_with_prior_id(self):
        self.backend.start_stored_tx("notes.bin")
        tag = "notes.bin".encode().hex().upper()
        self.assertEqual(self.next_command(), f"DOWNLOAD BEGINHEX {tag}")
        self.backend._handle_line(f"DOWNLOAD READYHEX {tag} 3 352441C2")
        self.assertEqual(self.next_command(), "DOWNLOAD CANCEL")
        self.backend._handle_line("DOWNLOAD CANCELLED")
        command = f"TXHEX {tag} 3 352441C2"
        self.assertEqual(self.next_command(), command)
        self.backend._handle_line("OK " + command)
        fields = dict(tx_state="Failed", tx_error="timeout", tx_id="000000AB",
                      tx_file="notes.bin", tx_crc32="352441C2", tx_bytes="0/3")
        with patch("rf3_gui.device.serial_device.time.monotonic", side_effect=[20, 24, 24, 24]):
            self.status(**fields)
            self.status(**fields)
        self.assertEqual(self.next_command(), f"{command} 000000AB")

    def test_no_ack_progress_and_reliable_watchdog_probe_are_distinct(self):
        shared = dict(state="TxBusy", tx_state="SendingData", tx_id="000000AC",
                      tx_file="work.bin", tx_bytes="3/3", tx_seq="1/1",
                      tx_crc32="352441C2", tx_error="none")
        self.backend._begin_rf_transfer("work.bin", no_ack=True, identity=(3, 0x352441C2))
        self.status(**shared, tx_no_ack=True, tx_work=1)
        self.status(**shared, tx_no_ack=True, tx_work=2)
        self.assertEqual(self.backend._rf_progress_marker[-1], 2)
        self.backend._finish_rf_transfer(TransferState.CANCELLED, "")
        self.backend._begin_rf_transfer("work.bin")
        active = shared | {"tx_id": "000000AD"}
        self.status(**active)
        self.backend._on_rf_progress_timeout()
        self.assertTrue(self.backend._rf_status_probe)
        self.status(**active)
        self.assertTrue(self.backend._rf_status_probe)
        self.backend._on_rf_progress_timeout()
        self.assertEqual(self.progress[-1].state, TransferState.FAILED)

    def test_new_attempt_for_same_rx_id_can_complete(self):
        fields = dict(state="RxListening", rx_no_ack=True, rx_id="000000AE", rx_seq="1/2")
        self.status(**fields, rx_state="Failed", rx_attempt=5, rx_bytes="20/40", rx_error="timeout")
        self.status(**fields, rx_state="Receiving", rx_attempt=6, rx_bytes="0/40", rx_error="none")
        self.assertEqual(self.backend._snapshot.rx_received_bytes, 0)
        self.status(**fields, rx_state="Completed", rx_attempt=6, rx_bytes="40/40", rx_error="none")
        self.assertEqual(self.backend._snapshot.rx_transfer_state, "Completed")

    def test_conventional_text_and_scope_correlated_channel_rejection(self):
        events = []
        self.backend.text_message_received.connect(lambda *args: events.append(args))
        self.backend.send_text_message("Use profile=bench")
        self.backend._handle_line("TEXT TX: Use profile=bench")
        self.backend._handle_line("TEXT RX: Use profile=bench")
        self.assertEqual(events, [("tx", "Use profile=bench"), ("rx", "Use profile=bench")])
        self.backend.request_status()
        self.status()
        self.backend.set_channel(0)
        self.backend._handle_line("ERR CHANNEL reinitialize_failed state=Fault fault=1")
        self.assertIsNotNone(self.backend._serial)
        self.assertEqual(list(self.backend._pending_commands), ["STATUS"])

    def test_cw_cycle_telemetry_and_morse_busy(self):
        window = MainWindow(AppController(self.backend))
        self.addCleanup(window.close)
        self.backend._handle_line("CW mode active on channel 0 at power level 3 (1 ms on, 1 ms off, loop 1)")
        self.assertEqual(self.backend._snapshot.confirmed_radio_state, RadioState.STANDBY)
        self.status(state="Unavailable", loop="Morse")
        self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.BUSY)
        self.assertFalse(window.storage_refresh_button.isEnabled())

    def test_save_button_requires_a_current_file(self):
        window = MainWindow(AppController(self.backend))
        self.addCleanup(window.close)
        self.status(rx_saved=1, rx_last="rx_00000142.bin")
        self.assertTrue(window.save_received_button.isEnabled())
        self.status(rx_saved=1, rx_last="")
        self.assertFalse(window.save_received_button.isEnabled())


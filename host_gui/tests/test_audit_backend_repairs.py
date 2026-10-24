import json
import binascii
import os
import tempfile
import time
import unittest
from dataclasses import replace
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication
from rf3_gui.app.state_model import (ConnectionState, DeviceInfo, DeviceSnapshot, RadioCapabilities,
                                    RadioState, Telemetry, TransferState)
from rf3_gui.device.serial_device import SerialDeviceBackend
from rf3_gui.protocol import console_protocol
from rf3_gui.protocol.status_parser import parse_status_line


class TestAuditBackendRepairs(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def setUp(self):
        self.backend = SerialDeviceBackend()
        self.backend._serial = SimpleNamespace(close=lambda: None)
        self.backend._active_device = DeviceInfo("RF3 PCB", "COM9", "serial")
        self.backend._handshake_ok = True
        self.backend._identity_ok = True
        self.backend._snapshot = DeviceSnapshot(connection_state=ConnectionState.CONNECTED)
        self.events = []
        self.progress = []
        self.backend.command_finished.connect(lambda *args: self.events.append(args))
        self.backend.transfer_changed.connect(self.progress.append)
        self.status()

    def tearDown(self):
        self.backend.disconnect_from_device()
        self.app.processEvents()

    def status(self, **overrides):
        values = dict(profile="custom-pcb", state="Standby", channel=76, power=3,
                      selected="", host_storage=True, host_upload=True, host_download=True,
                      host_retry=True, host_chunk_max=256, host_stored_tx=True,
                      test_mode_supported=False, tx_state="Idle", tx_id="00000000",
                      tx_file="", tx_bytes="0/0", tx_error="none", peer_complete=False)
        values.update(overrides)
        self.backend._handle_line(json.dumps(values))

    def commands(self):
        result = []
        while not self.backend._write_queue.empty():
            result.append(self.backend._write_queue.get_nowait())
        return result

    def test_F02_json_without_ack_retires_status_and_next_poll_runs(self):
        self.backend.request_status()
        self.assertEqual(self.commands(), ["STATUS"])
        self.status()
        self.assertNotIn("STATUS", self.backend._pending_commands)
        self.assertFalse(self.backend._status_response_timer.isActive())
        self.backend.request_status()
        self.assertEqual(self.commands(), ["STATUS"])

    def test_F03_ack_only_does_not_postpone_deadline(self):
        with patch("rf3_gui.device.serial_device.STATUS_RESPONSE_TIMEOUT_MS", 100):
            self.backend.request_status()
            for _ in range(5):
                self.backend._handle_line("OK STATUS")
                self.backend.request_status()
                QTest.qWait(30)
        self.assertIsNone(self.backend._serial)
        self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.ERROR)

    def test_malformed_status_does_not_retire_watchdog_or_pending(self):
        self.backend.request_status()
        for line in ('{}', '{"state":"Standby","fault":[]}', '{"state":"banana"}', '{broken'):
            self.backend._handle_line(line)
        self.assertIn("STATUS", self.backend._pending_commands)
        self.assertTrue(self.backend._status_response_timer.isActive())

    def test_F05_power_only_confirms_zero_to_three(self):
        for level in (-1, -2, 4, True, "3", None):
            self.status(power=level)
            self.assertIsNone(self.backend._snapshot.power_level)
        for level in range(4):
            self.status(power=level)
            self.assertEqual(self.backend._snapshot.power_level, level)

    def test_F07_empty_selected_clears_and_absence_retains(self):
        self.status(selected="old.bin")
        self.backend._handle_line('{"state":"Standby"}')
        self.assertEqual(self.backend._snapshot.selected_file, "old.bin")
        self.status(selected="")
        self.assertEqual(self.backend._snapshot.selected_file, "")

    def test_F08_schema_preserves_boolean_numeric_and_sentinel_filenames(self):
        for name in ("false", "true", "0", "123", "none", "unknown"):
            parsed = parse_status_line(f'profile=custom-pcb State=Standby selected={name} rx_last={name}')
            self.assertEqual(parsed["selected"], name)
            self.backend._apply_status(parsed)
            self.assertEqual(self.backend._snapshot.selected_file, name)
            self.assertEqual(self.backend._snapshot.telemetry.last_rx_file, name)

    def test_F09_USB_reply_correlates_rx_not_oldest_status(self):
        self.backend.request_status()
        self.backend._send_command("RX")
        self.backend._handle_line("RX listening on channel 76")
        self.assertEqual(list(self.backend._pending_commands), ["STATUS"])
        self.assertTrue(any(c == "RX" and ok for c, ok, _ in self.events))
        self.assertFalse(any(c == "STATUS" and ok for c, ok, _ in self.events))
        self.backend._handle_line("OK STATUS")
        self.assertEqual(list(self.backend._pending_commands), ["STATUS"])

    def test_generic_accepted_json_never_acknowledges_unrelated_action(self):
        self.backend._send_command("CHANNEL 77")
        self.backend._handle_line('{"accepted":true,"status":{"state":"Standby"}}')
        self.assertIn("CHANNEL 77", self.backend._pending_commands)

    def test_F10_late_status_preserves_storage_busy_and_blocks_controls(self):
        self.backend.refresh_storage()
        self.assertEqual(self.commands(), ["STORAGE LIST 0"])
        self.status()
        self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.BUSY)
        self.backend.set_channel(77)
        self.backend.set_power(2)
        self.backend.send_text_message("hello")
        self.assertEqual(self.commands(), [])
        self.assertEqual(self.backend._storage_kind, "list")

    def test_F11_dialog_return_after_disconnect_creates_no_temp_or_session(self):
        self.status(rx_last="received.bin")
        self.backend.disconnect_from_device()
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "out.bin"
            target.write_bytes(b"original")
            self.backend._start_download("received.bin", target, encoded_name=True)
            self.assertEqual(target.read_bytes(), b"original")
            self.assertEqual(list(Path(directory).iterdir()), [target])
        self.assertIsNone(self.backend._download_session)
        self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.DISCONNECTED)

    def test_F12_unscoped_firmware_errors_fail_and_clear_pending(self):
        self.backend._send_command("CHANNEL 77")
        self.backend._handle_line("ERR command queue full")
        self.assertIsNone(self.backend._serial)
        self.assertFalse(self.backend._pending_commands)
        self.assertTrue(any(c == "CHANNEL 77" and not ok for c, ok, _ in self.events))
        self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.ERROR)

    def test_scoped_error_retires_only_the_correct_command(self):
        self.backend.request_status()
        self.backend._send_command("POWER 2")
        self.backend._handle_line("ERR POWER radio_busy")
        self.assertEqual(list(self.backend._pending_commands), ["STATUS"])
        self.assertIsNotNone(self.backend._serial)

    def test_general_command_timeout_disconnects_and_late_reply_ignored(self):
        self.backend._send_command("CHANNEL 77")
        self.backend._command_deadlines["CHANNEL 77"] = time.monotonic() - 1
        self.backend._check_command_deadlines()
        self.backend._handle_line("OK CHANNEL 77")
        self.assertIsNone(self.backend._serial)
        self.assertFalse(any(c == "CHANNEL 77" and ok for c, ok, _ in self.events))

    def test_F14_read_failure_clears_stale_mode_readings_and_caps(self):
        self.status(test_mode=True, power=2, rx_packets=9, selected="old.bin", test_mode_supported=True)
        self.backend._on_read_failed("link lost")
        snapshot = self.backend._snapshot
        self.assertEqual(snapshot.connection_state, ConnectionState.ERROR)
        self.assertFalse(snapshot.test_mode)
        self.assertEqual(snapshot.confirmed_radio_state, RadioState.UNKNOWN)
        self.assertIsNone(snapshot.power_level)
        self.assertEqual(snapshot.telemetry, Telemetry())
        self.assertEqual(snapshot.selected_file, "")
        self.assertFalse(snapshot.capabilities.supports_host_storage)

    def test_F14_write_failure_also_resets_stale_state(self):
        self.status(test_mode=True, rx_packets=9)
        self.backend._on_write_failed("POWER 2", "write lost")
        self.assertFalse(self.backend._snapshot.test_mode)
        self.assertEqual(self.backend._snapshot.telemetry.rx_packets, 0)
        self.assertIsNone(self.backend._serial)

    def test_F13_explicit_devboard_capability_required(self):
        self.backend.set_test_mode(True)
        self.assertEqual(self.commands(), [])
        self.status(profile="esp32-devboard", test_mode_supported=True)
        self.backend.set_test_mode(True)
        self.assertEqual(self.commands(), ["TESTMODE ON"])

    def test_matching_status_retires_testmode_without_text_ack(self):
        self.status(profile="esp32-devboard", test_mode_supported=True)
        self.backend.set_test_mode(True)
        self.assertEqual(self.commands(), ["TESTMODE ON"])
        self.backend._handle_line('{"state":"Standby"}')
        self.assertIn("TESTMODE ON", self.backend._pending_commands)
        self.assertIs(self.backend._awaiting_test_mode, True)
        self.status(profile="esp32-devboard", test_mode_supported=True, test_mode=True)
        self.assertNotIn("TESTMODE ON", self.backend._pending_commands)
        self.assertIsNone(self.backend._awaiting_test_mode)
        self.assertFalse(self.backend._test_mode_timer.isActive())
        self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.CONNECTED)
        self.assertTrue(any(command == "TESTMODE ON" and ok for command, ok, _ in self.events))
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "mode.bin"
            source.write_bytes(b"host verification")
            self.backend.start_tx(source)
            self.assertIsNotNone(self.backend._upload_session)
            self.assertTrue(self.backend._upload_session.verify_only)

    def finish_uploaded_file(self, source, payload):
        self.backend.start_tx(source)
        crc = binascii.crc32(payload) & 0xFFFFFFFF
        chunk = self.backend._upload_session.chunk_bytes
        self.backend._handle_line(f"UPLOAD READY {source.name} {len(payload)} {crc:08X}"
                                  + (f" {chunk}" if chunk != 64 else ""))
        self.backend._handle_line(f"UPLOAD ACK 0 {len(payload)}")
        self.backend._handle_line(f"UPLOAD COMPLETE {source.name} {len(payload)} {crc:08X}")

    def test_uploaded_reserved_names_use_exact_encoded_tx(self):
        for name in ("LOOP", "STOP"):
            self.status()
            with tempfile.TemporaryDirectory() as directory:
                source = Path(directory) / name
                source.write_bytes(b"abc")
                self.finish_uploaded_file(source, b"abc")
                commands = self.commands()
                tx = console_protocol.stored_tx_command(name)
                self.assertEqual(commands[-1], tx)
                self.assertNotIn("TX " + name, commands)
                self.backend._handle_line("OK " + tx)
                self.assertNotIn(tx, self.backend._pending_commands)
                self.status(tx_id="00000001", tx_file=name, tx_state="Completed",
                            tx_bytes="3/3", peer_complete=True)
                self.assertEqual(self.progress[-1].state, TransferState.COMPLETED)

    def test_legacy_reserved_filename_never_emits_ambiguous_tx_command(self):
        self.status(host_stored_tx=False)
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "LOOP"
            source.write_bytes(b"abc")
            self.finish_uploaded_file(source, b"abc")
            commands = self.commands()
        self.assertFalse(any(command.startswith(("TX ", "TXHEX ")) for command in commands))
        self.assertEqual(self.progress[-1].state, TransferState.FAILED)
        self.assertIn("name requires updated", self.progress[-1].message)

    def test_close_clears_all_state_before_reentrant_callbacks(self):
        with tempfile.TemporaryDirectory() as directory:
            self.backend._start_download("a.bin", Path(directory) / "export.bin", encoded_name=True)
            partial = self.backend._download_session.partial_path
            self.assertTrue(partial.exists())
            generation = self.backend._connection_generation
            inspected = []
            reentering = False

            def inspect(*args):
                nonlocal reentering
                if reentering:
                    return
                reentering = True
                try:
                    self.assertIsNone(self.backend._serial)
                    self.assertFalse(self.backend._handshake_ok)
                    self.assertGreater(self.backend._connection_generation, generation)
                    self.assertIsNone(self.backend._upload_session)
                    self.assertIsNone(self.backend._download_session)
                    self.assertIsNone(self.backend._storage_kind)
                    self.assertIsNone(self.backend._awaiting_echo)
                    self.assertIsNone(self.backend._awaiting_test_mode)
                    self.assertIsNone(self.backend._rf_tx_name)
                    self.assertFalse(self.backend._pending_commands)
                    self.assertFalse(partial.exists())
                    self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.DISCONNECTED)
                    self.assertFalse(self.backend._snapshot.capabilities.supports_host_storage)
                    for timer in (self.backend._host_transfer_timer, self.backend._status_poll_timer,
                                  self.backend._storage_timer, self.backend._command_timer,
                                  self.backend._rf_progress_timer, self.backend._status_response_timer):
                        self.assertFalse(timer.isActive())
                    queued = self.backend._write_queue.qsize()
                    self.backend.request_status()
                    self.backend.start_stored_tx("a.bin")
                    self.assertEqual(self.backend._write_queue.qsize(), queued)
                    inspected.append(True)
                finally:
                    reentering = False

            self.backend.storage_changed.connect(inspect)
            self.backend.transfer_changed.connect(inspect)
            self.backend.command_finished.connect(inspect)
            self.backend._close_serial_port()
            self.assertGreaterEqual(len(inspected), 3)
            self.backend.storage_changed.disconnect(inspect)
            self.backend.transfer_changed.disconnect(inspect)
            self.backend.command_finished.disconnect(inspect)

    def begin_stored_tx(self, name="stored file.bin"):
        self.backend.start_stored_tx(name)
        command = console_protocol.stored_tx_command(name)
        self.assertEqual(self.commands(), [command])
        self.backend._handle_line("OK " + command)
        return name

    def test_F06_encoded_select_and_stored_tx_do_not_upload(self):
        name = 'résumé "one".bin'
        self.backend.select_stored_file(name)
        command = console_protocol.select_stored_command(name)
        self.assertEqual(self.commands(), [command])
        self.backend._handle_line("OK " + command)
        self.assertEqual(self.backend._snapshot.selected_file, name)
        self.begin_stored_tx(name)
        self.assertIsNone(self.backend._upload_session)
        self.assertEqual(self.progress[-1].state, TransferState.HANDSHAKING)

    def test_F01_RF_progress_and_peer_confirmed_complete(self):
        name = self.begin_stored_tx()
        self.status(state="TxBusy", tx_id="00000001", tx_file=name, tx_state="WaitingForReady", tx_bytes="0/64")
        self.status(state="TxBusy", tx_id="00000001", tx_file=name, tx_state="SendingData", tx_bytes="32/64")
        self.assertEqual(self.progress[-1].state, TransferState.TRANSFERRING)
        self.assertEqual(self.progress[-1].sent_bytes, 32)
        self.status(tx_id="00000001", tx_file=name, tx_state="Completed", tx_bytes="64/64", peer_complete=True)
        self.assertEqual(self.progress[-1].state, TransferState.COMPLETED)
        self.assertIn("peer confirmed", self.progress[-1].message)
        self.assertIsNone(self.backend._rf_tx_name)
        count = len(self.progress)
        self.status(tx_id="00000001", tx_file=name, tx_state="Completed", tx_bytes="64/64", peer_complete=True)
        self.assertEqual(len(self.progress), count)

    def test_unconfirmed_local_completion_is_not_delivery_success(self):
        name = self.begin_stored_tx()
        self.status(tx_id="00000001", tx_file=name, tx_state="Completed", tx_bytes="64/64", peer_complete=False)
        self.assertEqual(self.progress[-1].state, TransferState.FAILED)
        self.assertIn("not confirmed", self.progress[-1].message)

    def test_inconsistent_RF_completion_metadata_is_failure(self):
        for sent, total, error in ((1, 64, "none"), (64, 64, "crc_mismatch")):
            self.status(tx_id="00000000")
            name = self.begin_stored_tx()
            self.status(tx_id="00000001", tx_file=name, tx_state="Completed",
                        tx_bytes=f"{sent}/{total}", peer_complete=True, tx_error=error)
            self.assertEqual(self.progress[-1].state, TransferState.FAILED)
            self.assertIn("Inconsistent", self.progress[-1].message)
        self.status(tx_id="00000000")
        name = self.begin_stored_tx()
        self.status(tx_id="00000001", tx_file=name, tx_state="Completed",
                    tx_bytes="0/0", peer_complete=True)
        self.assertEqual(self.progress[-1].state, TransferState.COMPLETED)

    def test_USB_human_then_JSON_rich_status_retains_source_and_capabilities(self):
        name = self.begin_stored_tx("stored file.bin")
        self.backend._handle_line(
            'profile=custom-pcb State=TxBusy channel=76 power=3 selected="stored file.bin" '
            'tx_id=00000001 tx_file="stored file.bin" tx_state=WaitingForReady '
            'tx_bytes=0/64 tx_error=none peer_complete=false test_mode_supported=false host_stored_tx=true')
        self.assertEqual(self.backend._rf_tx_name, name)
        self.assertEqual(self.progress[-1].state, TransferState.HANDSHAKING)
        self.assertTrue(self.backend._snapshot.capabilities.supports_stored_file_tx)
        self.status(state="TxBusy", tx_id="00000001", tx_file=name, tx_state="SendingData", tx_bytes="32/64")
        self.assertEqual(self.progress[-1].state, TransferState.TRANSFERRING)

    def test_previous_terminal_cannot_complete_new_request(self):
        self.status(tx_id="00000002", tx_file="old.bin", tx_state="Completed", tx_bytes="8/8", peer_complete=True)
        name = self.begin_stored_tx("new.bin")
        count = len(self.progress)
        self.status(tx_id="00000002", tx_file="old.bin", tx_state="Completed", tx_bytes="8/8", peer_complete=True)
        self.status(tx_id="00000001", tx_file="older.bin", tx_state="Completed", tx_bytes="8/8", peer_complete=True)
        self.assertEqual(len(self.progress), count)
        self.assertEqual(self.backend._rf_tx_name, name)
        self.status(tx_id="00000003", tx_file=name, tx_state="Failed", tx_bytes="0/8", tx_error="RadioFailure")
        self.assertEqual(self.progress[-1].state, TransferState.FAILED)
        self.assertIn("RadioFailure", self.progress[-1].message)

    def test_unsolicited_old_RF_terminal_cannot_overwrite_host_progress(self):
        self.status(tx_id="00000002", tx_file="old.bin", tx_state="Completed", tx_bytes="8/8", peer_complete=True)
        self.assertEqual(self.progress, [])
        self.status(tx_id="00000002", tx_file="old.bin", tx_state="Failed", tx_bytes="8/8")
        self.assertEqual(self.progress, [])

    def test_missing_RF_contract_fails_clearly_not_indefinite_handshaking(self):
        self.begin_stored_tx()
        self.backend._handle_line('{"state":"Standby","power":3}')
        self.assertEqual(self.progress[-1].state, TransferState.FAILED)
        self.assertIn("matching firmware", self.progress[-1].message)
        self.assertIsNone(self.backend._rf_tx_name)

    def test_RF_progress_deadline_and_fault_gates(self):
        self.status(state="Fault", power=-1, fault=3)
        self.backend.start_stored_tx("file.bin")
        self.assertEqual(self.commands(), [])
        self.status()
        self.begin_stored_tx()
        self.backend._rf_progress_timer.timeout.emit()
        self.assertEqual(self.progress[-1].state, TransferState.FAILED)
        self.assertIn("unconfirmed", self.progress[-1].message)

    def test_stored_source_validation_and_test_mode_gate(self):
        for name in ("../config", ".journal", "file.PART", "", "x" * 31):
            self.backend.select_stored_file(name)
            self.backend.start_stored_tx(name)
        self.assertEqual(self.commands(), [])
        self.status(test_mode=True)
        self.backend.start_stored_tx("file.bin")
        self.assertEqual(self.commands(), [])

    def test_unavailable_status_keeps_busy_host_operation(self):
        self.backend.refresh_storage()
        self.backend._handle_line('{"state":"Unavailable"}')
        self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.BUSY)
        self.assertIsNone(self.backend._snapshot.power_level)


if __name__ == "__main__":
    unittest.main()

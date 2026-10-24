import json
import os
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
from PySide6.QtWidgets import QApplication
from rf3_gui.app.state_model import ConnectionState, DeviceSnapshot, RadioState, TransferState
from rf3_gui.device.serial_device import SerialDeviceBackend, RX_ACTIVE_STATES


class TestIncomingReceiveGuard(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def setUp(self):
        self.backend = SerialDeviceBackend()
        self.backend._serial = SimpleNamespace(close=lambda: None)
        self.backend._handshake_ok = True
        self.backend._identity_ok = True
        self.backend._snapshot = DeviceSnapshot(connection_state=ConnectionState.CONNECTED)
        self.events, self.progress = [], []
        self.backend.command_finished.connect(lambda *args: self.events.append(args))
        self.backend.transfer_changed.connect(self.progress.append)
        self.status()

    def tearDown(self):
        self.backend.disconnect_from_device()

    def status(self, **overrides):
        values = dict(profile="custom-pcb", state="RxListening", channel=76, power=3,
                      host_storage=True, host_upload=True, host_download=True, host_retry=True,
                      host_stored_tx=True, host_chunk_max=256, test_mode_supported=False,
                      tx_id="00000000", tx_state="Idle", tx_file="", tx_bytes="0/0",
                      tx_error="none", peer_complete=False,
                      rx_state="Idle", rx_id="00000000", rx_bytes="0/0", rx_error="none")
        values.update(overrides)
        self.backend._handle_line(json.dumps(values))

    def active(self, **overrides):
        values = dict(rx_state="Receiving", rx_id="AABB0001", rx_bytes="128/1024")
        values.update(overrides)
        self.status(**values)

    def commands(self):
        commands = []
        while not self.backend._write_queue.empty():
            commands.append(self.backend._write_queue.get_nowait())
        return commands

    def test_all_receiver_active_phases_are_busy_and_report_byte_progress(self):
        for phase in ("Receiving", "WaitingForEnd", "Verifying", "Publishing"):
            self.active(rx_state=phase)
            snapshot = self.backend._snapshot
            self.assertEqual(snapshot.connection_state, ConnectionState.BUSY)
            self.assertEqual(snapshot.rx_transfer_state, phase)
            self.assertEqual(snapshot.rx_transfer_id, "AABB0001")
            self.assertEqual(snapshot.rx_received_bytes, 128)
            self.assertEqual(snapshot.rx_total_bytes, 1024)
        self.assertEqual(self.progress, [])

    def test_active_rx_blocks_disruptive_direct_backend_operations(self):
        self.active()
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source.bin"
            source.write_bytes(b"abc")
            for action in (
                lambda: self.backend.set_channel(77), lambda: self.backend.set_power(2),
                self.backend.enter_standby, self.backend.enter_sleep, self.backend.wake,
                self.backend.power_down, lambda: self.backend.start_tx(source),
                lambda: self.backend.import_file(source), self.backend.refresh_storage,
                lambda: self.backend.delete_file("stored.bin"),
                lambda: self.backend.export_file("stored.bin", Path(directory)/"out.bin"),
                lambda: self.backend._start_download("stored.bin", Path(directory)/"out2.bin"),
                lambda: self.backend.select_stored_file("stored.bin"),
                lambda: self.backend.start_stored_tx("stored.bin"),
                lambda: self.backend.set_test_mode(True),
                lambda: self.backend.send_morse_text("SOS"),
                lambda: self.backend.send_remote_command("RX"),
                lambda: self.backend.send_text_message("hello"),
                lambda: self.backend.start_cw(76, 3),
                lambda: self.backend.start_cw_loop(100, 100, 76, 3)):
                action()
            self.assertEqual(sorted(p.name for p in Path(directory).iterdir()), ["source.bin"])
        self.assertEqual(self.commands(), [])
        self.assertGreaterEqual(len([event for event in self.events if not event[1]]), 20)
        self.assertIsNone(self.backend._upload_session)
        self.assertIsNone(self.backend._download_session)

    def test_repeated_receive_during_active_file_is_noop(self):
        self.active()
        before = self.backend._snapshot
        for _ in range(5):
            self.backend.enter_rx()
        self.assertEqual(self.commands(), [])
        self.assertEqual(self.backend._snapshot, before)
        self.assertEqual(self.events, [])

    def test_repeated_receive_when_listening_without_file_is_noop(self):
        for _ in range(3):
            self.backend.enter_rx()
        self.assertEqual(self.commands(), [])
        self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.CONNECTED)

    def test_receive_does_not_bypass_active_host_operation(self):
        with tempfile.TemporaryDirectory() as directory:
            self.backend._start_download("a.bin", Path(directory)/"out.bin", encoded_name=True)
            self.commands()
            self.backend.enter_rx()
            self.assertEqual(self.commands(), [])
            self.assertTrue(any(not ok for _, ok, _ in self.events))
            self.assertIsNotNone(self.backend._download_session)
            self.backend.disconnect_from_device()

    def test_stop_remains_allowed_and_idle_cancellation_keeps_partial_progress(self):
        self.active()
        self.backend.stop()
        self.assertEqual(self.commands(), ["STOP"])
        self.backend._handle_line("Stop complete. Radio is in standby.")
        self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.BUSY)
        self.status(state="Standby")
        snapshot = self.backend._snapshot
        self.assertEqual(snapshot.connection_state, ConnectionState.CONNECTED)
        self.assertEqual(snapshot.rx_transfer_state, "Cancelled")
        self.assertEqual(snapshot.rx_transfer_id, "AABB0001")
        self.assertEqual(snapshot.rx_received_bytes, 128)
        self.assertEqual(snapshot.rx_total_bytes, 1024)

    def test_stop_ack_without_receive_terminal_has_bounded_cancellation(self):
        self.active()
        self.backend.stop()
        self.backend._handle_line("OK STOP")
        self.assertTrue(self.backend._rx_cancel_timer.isActive())
        self.backend._handle_line('{"state":"RxListening"}')
        self.assertTrue(self.backend._rx_cancel_timer.isActive())
        self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.BUSY)
        self.backend._rx_cancel_timer.timeout.emit()
        self.assertIsNone(self.backend._serial)
        self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.ERROR)
        self.assertIn("receive cancellation", self.backend._snapshot.connection_detail)

    def test_malformed_terminal_cannot_retire_receive_cancel_deadline(self):
        self.active()
        self.backend.stop()
        self.backend._handle_line("OK STOP")
        for invalid in (dict(rx_state="Completed", rx_bytes="1025/1025"),
                        dict(rx_state="Cancelled", rx_bytes="128/1025"),
                        dict(rx_state="Completed", rx_bytes="1/1024"),
                        dict(rx_state="Failed", rx_id="AABB0002"),
                        dict(rx_state="Idle", rx_id=[])):
            self.active(**invalid)
            self.assertTrue(self.backend._rx_cancel_requested)
            self.assertTrue(self.backend._rx_cancel_timer.isActive())
            self.assertEqual(self.backend._snapshot.rx_transfer_state, "Receiving")
            self.assertEqual(self.backend._snapshot.rx_received_bytes, 128)
        self.backend._rx_cancel_timer.timeout.emit()
        self.assertIsNone(self.backend._serial)

    def test_new_incoming_id_cannot_postpone_pending_stop_confirmation(self):
        self.active()
        self.backend.stop()
        self.backend._handle_line("OK STOP")
        self.active(rx_id="AABB0002", rx_bytes="16/2048")
        self.assertTrue(self.backend._rx_cancel_requested)
        self.assertTrue(self.backend._rx_cancel_timer.isActive())
        self.backend._rx_cancel_timer.timeout.emit()
        self.assertIsNone(self.backend._serial)

    def test_stop_then_confirmed_standby_without_rx_fields_marks_cancelled(self):
        self.active()
        self.backend.stop()
        self.backend._handle_line("OK STOP")
        self.backend._handle_line('{"state":"Standby"}')
        self.assertFalse(self.backend._rx_cancel_timer.isActive())
        self.assertFalse(self.backend._rx_cancel_requested)
        self.assertEqual(self.backend._snapshot.rx_transfer_state, "Cancelled")
        self.assertEqual(self.backend._snapshot.rx_received_bytes, 128)
        self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.CONNECTED)

    def test_cancel_terminal_stops_cancel_timer_without_affecting_next_receive(self):
        self.active()
        self.backend.stop()
        self.active(rx_state="Cancelled", rx_error="cancelled")
        self.assertFalse(self.backend._rx_cancel_timer.isActive())
        self.backend._handle_line("OK STOP")
        self.active(rx_id="AABB0002", rx_bytes="16/2048")
        self.backend._rx_cancel_timer.timeout.emit()
        self.assertIsNotNone(self.backend._serial)
        self.assertEqual(self.backend._snapshot.rx_transfer_id, "AABB0002")

    def test_unexpected_idle_is_reported_as_interruption_not_success(self):
        self.active()
        self.status()
        self.assertEqual(self.backend._snapshot.rx_transfer_state, "Failed")
        self.assertIn("before file completion", self.backend._snapshot.rx_error)
        self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.CONNECTED)

    def test_receive_complete_clears_busy_and_retains_saved_metadata(self):
        self.active()
        self.active(rx_state="Completed", rx_bytes="1024/1024", rx_last="received.bin",
                    rx_last_path="/spiffs/received.bin", rx_saved=1, rx_saved_bytes=1024)
        snapshot = self.backend._snapshot
        self.assertEqual(snapshot.connection_state, ConnectionState.CONNECTED)
        self.assertEqual(snapshot.rx_transfer_state, "Completed")
        self.assertEqual(snapshot.telemetry.last_rx_file, "received.bin")
        self.status()
        self.assertEqual(self.backend._snapshot.rx_transfer_state, "Completed")
        self.assertEqual(self.backend._snapshot.rx_received_bytes, 1024)
        self.assertEqual(self.backend._snapshot.telemetry.last_rx_file, "received.bin")
        self.assertEqual(self.backend._snapshot.telemetry.rx_saved, 1)
        self.assertEqual(self.progress, [])

    def test_receiver_failure_and_cancel_terminal_states_clear_busy(self):
        for index, phase in enumerate(("Failed", "Cancelled"), 1):
            self.active(rx_id=f"AABB000{index}")
            self.active(rx_id=f"AABB000{index}", rx_state=phase, rx_error="retry_exhausted")
            self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.CONNECTED)
            self.assertEqual(self.backend._snapshot.rx_transfer_state, phase)
            self.assertEqual(self.backend._snapshot.rx_error, "retry_exhausted")

    def test_duplicate_and_late_same_id_active_status_never_decrease_progress(self):
        self.active(rx_state="WaitingForEnd", rx_bytes="1024/1024")
        self.active(rx_state="Receiving", rx_bytes="64/1024")
        self.assertEqual(self.backend._snapshot.rx_received_bytes, 1024)
        self.assertEqual(self.backend._snapshot.rx_transfer_state, "WaitingForEnd")
        self.active(rx_state="Completed", rx_bytes="1024/1024")
        self.active(rx_state="Receiving", rx_bytes="32/1024")
        self.assertEqual(self.backend._snapshot.rx_transfer_state, "Completed")
        self.assertEqual(self.backend._snapshot.rx_received_bytes, 1024)
        self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.CONNECTED)

    def test_new_incoming_id_starts_new_progress_after_terminal(self):
        self.active(rx_state="Completed", rx_bytes="1024/1024")
        self.active(rx_id="AABB0002", rx_bytes="16/2048")
        self.assertEqual(self.backend._snapshot.rx_transfer_id, "AABB0002")
        self.assertEqual(self.backend._snapshot.rx_received_bytes, 16)
        self.assertEqual(self.backend._snapshot.rx_total_bytes, 2048)
        self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.BUSY)

    def test_partial_or_error_completed_result_does_not_claim_verified_receive(self):
        self.active(rx_state="WaitingForEnd", rx_bytes="1024/1024")
        self.active(rx_state="Completed", rx_bytes="1/1024")
        self.assertEqual(self.backend._snapshot.rx_transfer_state, "Failed")
        self.assertIn("Inconsistent", self.backend._snapshot.rx_error)
        self.active(rx_id="AABB0002", rx_state="Completed", rx_bytes="1024/1024", rx_error="crc_mismatch")
        self.assertEqual(self.backend._snapshot.rx_transfer_state, "Failed")
        self.active(rx_id="AABB0003", rx_state="Completed", rx_bytes="0/0")
        self.assertEqual(self.backend._snapshot.rx_transfer_state, "Completed")

    def test_invalid_rx_metadata_does_not_retire_status_watchdog_or_clear_active_guard(self):
        self.active()
        self.backend.request_status()
        for invalid in (dict(rx_id="0"), dict(rx_id="00000000"), dict(rx_bytes="1025/1024"),
                        dict(rx_bytes="broken"), dict(rx_error=[]), dict(rx_state="banana")):
            self.active(**invalid)
        self.assertEqual(self.backend._snapshot.rx_received_bytes, 128)
        self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.BUSY)
        self.assertIn("STATUS", self.backend._pending_commands)
        self.assertTrue(self.backend._status_response_timer.isActive())

    def test_unavailable_and_unknown_radio_status_preserve_active_receive_guard(self):
        self.active()
        self.backend._handle_line('{"state":"Unavailable"}')
        self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.BUSY)
        self.assertEqual(self.backend._snapshot.rx_received_bytes, 128)
        self.backend._handle_line('{"state":"Unknown"}')
        self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.BUSY)
        self.backend.set_channel(77)
        self.assertEqual(self.commands(), [])

    def test_unavailable_radio_with_new_rx_metadata_is_still_busy(self):
        self.backend._handle_line(json.dumps(dict(state="Unavailable", rx_state="Receiving",
                                                   rx_id="AABB0001", rx_bytes="128/1024", rx_error="none")))
        self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.BUSY)
        self.assertEqual(self.backend._snapshot.rx_received_bytes, 128)

    def test_status_polling_remains_usable_while_receive_is_busy(self):
        self.active()
        self.backend.request_status()
        self.assertEqual(self.commands(), ["STATUS"])
        self.active(rx_bytes="256/1024")
        self.assertNotIn("STATUS", self.backend._pending_commands)
        self.assertFalse(self.backend._status_response_timer.isActive())
        self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.BUSY)
        self.backend.request_status()
        self.assertEqual(self.commands(), ["STATUS"])

    def test_rx_updates_do_not_override_host_or_outgoing_transfer_progress(self):
        self.backend._begin_rf_transfer("outgoing.bin", total_bytes=64)
        before = list(self.progress)
        self.active(tx_state="WaitingForReady", tx_id="11223344", tx_file="outgoing.bin", tx_bytes="0/64")
        self.assertTrue(all("RF" in p.message or "Starting RF" in p.message for p in self.progress))
        self.assertFalse(any("receive" in p.message.lower() for p in self.progress))
        self.active(rx_state="Completed", rx_bytes="1024/1024",
                    tx_state="WaitingForReady", tx_id="11223344", tx_file="outgoing.bin", tx_bytes="0/64")
        self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.BUSY)
        self.assertEqual(self.backend._rf_tx_name, "outgoing.bin")
        self.assertNotEqual(self.progress[-1].state, TransferState.COMPLETED)

    def test_rx_status_does_not_replace_active_host_download_progress(self):
        with tempfile.TemporaryDirectory() as directory:
            self.backend._start_download("a.bin", Path(directory)/"out.bin", encoded_name=True)
            before = list(self.progress)
            self.active()
            self.assertEqual(self.progress, before)
            self.assertEqual(self.backend._snapshot.rx_received_bytes, 128)
            self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.BUSY)
            self.backend._fail_host_transfer("host operation refused by receiver guard", "DOWNLOAD")
            self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.BUSY)
            self.assertEqual(self.backend._snapshot.rx_transfer_state, "Receiving")
            self.backend.disconnect_from_device()

    def test_failure_detaches_receive_state_before_callbacks(self):
        self.active()
        seen = []
        self.backend.storage_changed.connect(lambda listing: seen.append(
            (self.backend._serial, self.backend._snapshot.rx_transfer_state,
             self.backend._snapshot.rx_received_bytes)))
        self.backend._on_read_failed("lost connection during receive")
        self.assertTrue(seen)
        self.assertTrue(all(port is None and state == "" and received == 0 for port, state, received in seen))
        self.assertEqual(self.backend._snapshot.rx_transfer_state, "")
        self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.ERROR)

    def test_late_RX_ack_or_host_completion_cannot_clear_incoming_busy(self):
        self.active()
        self.backend._pending_commands.append("RX")
        self.backend._handle_line("RX listening on channel 76")
        self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.BUSY)
        self.backend._snapshot = self.backend._replace_snapshot(connection_state=ConnectionState.CONNECTED)
        self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.BUSY)

    def test_hello_and_partial_status_do_not_reset_progress(self):
        self.active()
        self.backend._handle_line('{"rf3_protocol":1,"protocol":2,"profile":"custom-pcb","node_name":"rf3-pcb"}')
        self.backend._handle_line('{"state":"RxListening"}')
        self.assertEqual(self.backend._snapshot.rx_received_bytes, 128)
        self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.BUSY)

    def test_rx_fields_are_cleared_on_disconnect_and_late_status_ignored(self):
        self.active()
        generation = self.backend._connection_generation
        self.backend.disconnect_from_device()
        snapshot = self.backend._snapshot
        self.assertEqual(snapshot.rx_transfer_state, "")
        self.assertEqual(snapshot.rx_transfer_id, "")
        self.assertEqual(snapshot.rx_received_bytes, 0)
        self.backend._on_worker_event(generation, "line", '{"state":"RxListening","rx_state":"Receiving"}')
        self.assertEqual(self.backend._snapshot, snapshot)

    def test_human_rx_status_updates_separate_progress_fields(self):
        self.backend._handle_line('profile=custom-pcb State=RxListening channel=76 power=3 '
                                  'rx_state=Receiving rx_id=AABB0001 rx_bytes=128/1024 rx_error=none')
        self.assertEqual(self.backend._snapshot.rx_received_bytes, 128)
        self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.BUSY)
        self.assertEqual(self.progress, [])

    def test_reentrant_status_callback_cannot_queue_disruptive_settings(self):
        called = []
        def on_status(snapshot):
            if snapshot.rx_transfer_state == "Receiving":
                self.backend.set_channel(77)
                self.backend.start_stored_tx("a.bin")
                called.append(True)
        self.backend.status_changed.connect(on_status)
        self.active()
        self.assertTrue(called)
        self.assertEqual(self.commands(), [])
        self.backend.status_changed.disconnect(on_status)


if __name__ == "__main__":
    unittest.main()

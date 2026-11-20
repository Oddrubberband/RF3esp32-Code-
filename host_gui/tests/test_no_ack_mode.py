import tempfile
import unittest
from pathlib import Path

import test_audit_backend_repairs as fixtures
from rf3_gui.app.state_model import ConnectionState, TransferState
from rf3_gui.protocol import console_protocol


class TestNoAckMode(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        fixtures.TestAuditBackendRepairs.setUpClass.__func__(cls)

    setUp = fixtures.TestAuditBackendRepairs.setUp
    tearDown = fixtures.TestAuditBackendRepairs.tearDown
    status = fixtures.TestAuditBackendRepairs.status
    commands = fixtures.TestAuditBackendRepairs.commands

    def arm(self):
        self.status(no_ack_files=True)
        self.assertTrue(self.backend.set_no_ack_mode(True))

    def start(self):
        self.arm()
        self.backend.start_stored_tx("report.bin")

    def terminal(self, **changes):
        values = dict(tx_state="SentUnconfirmed", tx_id="11112222", tx_file="report.bin",
                      tx_bytes="3/3", tx_no_ack=True, peer_complete=False)
        values.update(changes)
        self.status(no_ack_files=True, **values)

    def test_legacy_firmware_cannot_select_no_ack(self):
        self.status()
        self.assertFalse(self.backend.set_no_ack_mode(True))
        self.assertFalse(self.backend._no_ack_selected)
        self.assertEqual(self.commands(), [])

    def test_stored_file_uses_distinct_command_and_latches_mode(self):
        self.start()
        self.assertEqual(self.commands(), [console_protocol.stored_tx_command("report.bin", no_ack=True)])
        self.assertTrue(self.backend._rf_tx_no_ack)
        self.assertFalse(self.backend.set_no_ack_mode(False))

    def test_silent_receive_command_is_correlated_on_usb(self):
        self.arm()
        self.backend.enter_rx()
        self.assertEqual(self.commands(), ["RXNOACK"])
        self.backend._handle_line("OK RXNOACK")
        self.assertNotIn("RXNOACK", self.backend._pending_commands)

    def test_no_ack_terminal_is_local_unconfirmed_success(self):
        self.start()
        self.terminal()
        self.assertEqual(self.progress[-1].state, TransferState.SENT_UNCONFIRMED)
        self.assertIn("unconfirmed", self.progress[-1].message)
        self.assertIsNone(self.backend._rf_tx_name)

    def test_zero_byte_no_ack_terminal_can_complete_locally(self):
        self.start()
        self.terminal(tx_bytes="0/0")
        self.assertEqual(self.progress[-1].state, TransferState.SENT_UNCONFIRMED)
        self.assertEqual(self.progress[-1].ratio, 1)

    def test_incomplete_no_ack_terminal_is_failure(self):
        self.start()
        self.terminal(tx_bytes="2/3")
        self.assertEqual(self.progress[-1].state, TransferState.FAILED)

    def test_no_ack_cannot_claim_peer_complete(self):
        self.start()
        self.terminal(tx_state="Completed", peer_complete=True)
        self.assertEqual(self.progress[-1].state, TransferState.FAILED)

    def test_reliable_completion_without_peer_confirmation_still_fails(self):
        self.status(no_ack_files=True)
        self.backend.start_stored_tx("report.bin")
        self.terminal(tx_state="Completed", tx_no_ack=False)
        self.assertEqual(self.progress[-1].state, TransferState.FAILED)

    def test_requested_mode_mismatch_fails(self):
        self.start()
        self.terminal(tx_no_ack=False)
        self.assertEqual(self.progress[-1].state, TransferState.FAILED)

    def test_console_summary_then_json_after_stop_keeps_mode_and_reason(self):
        for no_ack in (False, True):
            with self.subTest(no_ack=no_ack):
                self.status(no_ack_files=True, tx_no_ack=False, tx_state="Idle", tx_id="00000000")
                self.assertTrue(self.backend.set_no_ack_mode(no_ack))
                self.backend.start_stored_tx("report.bin")
                self.backend._handle_line("OK " + console_protocol.stored_tx_command("report.bin", no_ack=no_ack))
                self.terminal(tx_state="SendingData", tx_bytes="1/3", tx_no_ack=no_ack)
                self.backend.stop()
                self.backend._handle_line("OK STOP")
                self.backend._handle_line("profile=custom-pcb State=Standby tx_id=11112222 "
                                          "tx_state=Cancelled tx_file=report.bin tx_bytes=1/3 "
                                          "peer_complete=false tx_error=cancelled")
                self.assertIsNotNone(self.backend._rf_tx_name)
                self.assertTrue(self.backend._snapshot.capabilities.supports_no_ack_files)
                self.terminal(tx_state="Cancelled", tx_bytes="1/3", tx_error="cancelled",
                              tx_no_ack=no_ack)
                self.assertEqual(self.progress[-1].state, TransferState.CANCELLED)
                self.assertEqual(self.progress[-1].sent_bytes, 1)
                self.assertNotIn("mode", self.progress[-1].message)

    def test_terminal_failure_reports_esp_reason_even_when_mode_resets(self):
        self.start()
        self.terminal(tx_state="Failed", tx_no_ack=False, tx_error="timeout")
        self.assertEqual(self.progress[-1].state, TransferState.FAILED)
        self.assertIn("timeout", self.progress[-1].message)

    def test_upload_uses_its_latched_mode_after_local_preference_changes(self):
        self.arm()
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source.bin"
            source.write_bytes(b"abc")
            self.backend.start_tx(source)
            session = self.backend._upload_session
            self.assertTrue(session.no_ack)
            session.complete = True
            self.backend._no_ack_selected = False  # Simulate a stale/direct caller.
            self.backend._advance_upload(console_protocol.upload_end_command(), True, "verified")
            self.assertIn(console_protocol.stored_tx_command(session.remote_name, no_ack=True), self.commands())
            self.assertTrue(self.backend._rf_tx_no_ack)

    def test_received_mode_is_kept_in_snapshot(self):
        self.status(no_ack_files=True, rx_no_ack=True)
        self.assertTrue(self.backend._snapshot.rx_no_ack)
        self.backend._snapshot = self.backend._replace_snapshot(channel=40)
        self.assertTrue(self.backend._snapshot.rx_no_ack)

    def test_no_ack_never_falls_back_to_reliable_tx_when_stored_capability_missing(self):
        self.status(no_ack_files=True, host_stored_tx=False)
        self.assertTrue(self.backend.set_no_ack_mode(True))
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source.bin"
            source.write_bytes(b"abc")
            self.backend.start_tx(source)
            self.assertIsNone(self.backend._upload_session)
            self.assertEqual(self.commands(), [])

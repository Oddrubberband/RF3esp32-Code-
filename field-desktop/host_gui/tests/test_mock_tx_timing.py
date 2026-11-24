import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from PySide6.QtWidgets import QApplication

from rf3_gui.app.state_model import ConnectionState, DeviceSnapshot, RadioState, TransferState
from rf3_gui.app.sim_tx import timing_profile
from rf3_gui.device.mock_device import MockDeviceBackend


class TestMockTxTiming(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def setUp(self):
        self.now = 100.0
        clock = patch("rf3_gui.device.mock_device.time.monotonic", side_effect=lambda: self.now)
        clock.start()
        self.addCleanup(clock.stop)
        self.backend = MockDeviceBackend()
        self.backend._connected = True
        self.backend._snapshot = DeviceSnapshot(connection_state=ConnectionState.CONNECTED,
                                                confirmed_radio_state=RadioState.STANDBY)
        self.samples, self.progress, self.ended = [], [], []
        self.backend.rf_sample.connect(lambda *args: self.samples.append(args))
        self.backend.transfer_changed.connect(self.progress.append)
        self.backend.rf_ended.connect(lambda *args: self.ended.append(args))
        self.addCleanup(self.backend.disconnect_from_device)

    def begin_stored(self, size, no_ack=False):
        self.backend._stored_files["file.txt"] = bytes(size)
        self.backend.set_no_ack_mode(no_ack)
        self.backend.start_stored_tx("file.txt")
        self.backend._transfer_timer.stop()
        return self.backend._transfer_started, timing_profile(no_ack)

    def at(self, started, elapsed):
        self.now = started + elapsed
        self.backend._advance_transfer()

    def test_delayed_callback_uses_elapsed_time_and_latches_mode(self):
        started, profile = self.begin_stored(65536, True)
        self.backend.set_no_ack_mode(False)
        self.at(started, profile.data_start / 2)
        self.assertEqual(self.samples[-1][2:], ("Preparing", 0, 65536))
        elapsed = (profile.data_start + profile.data_end(65536)) / 2
        self.at(started, elapsed)
        self.assertEqual(self.samples[-1][3], profile.bytes_at(65536, elapsed))
        self.assertLess(self.samples[-1][3], 65536)
        self.at(started, profile.elapsed_for_size(65536) + 5)
        self.assertEqual(self.samples[-1][2:], ("SentUnconfirmed", 65536, 65536))
        self.assertEqual(self.progress[-1].state, TransferState.SENT_UNCONFIRMED)
        self.assertIsNone(self.backend._transfer_started)

    def test_final_byte_count_does_not_skip_reliable_completion_tail(self):
        started, profile = self.begin_stored(65536)
        tail = (profile.data_end(65536) + profile.elapsed_for_size(65536)) / 2
        self.at(started, tail)
        self.assertEqual(self.samples[-1][2:], ("Verifying", 65536, 65536))
        self.assertIsNotNone(self.backend._transfer_started)
        self.at(started, profile.elapsed_for_size(65536) + 0.001)
        self.assertEqual(self.samples[-1][2], "Completed")
        count = len(self.samples)
        self.backend._advance_transfer()
        self.assertEqual(len(self.samples), count)

    def test_zero_and_partial_packet_sizes_finish_using_shared_profile(self):
        for size in (0, 1, 19, 20, 21):
            for no_ack in (False, True):
                with self.subTest(size=size, no_ack=no_ack):
                    started, profile = self.begin_stored(size, no_ack)
                    self.assertEqual(self.samples[-1][3], 0)
                    self.at(started, max(0, profile.elapsed_for_size(size) - 0.001))
                    self.assertIsNotNone(self.backend._transfer_started)
                    self.at(started, profile.elapsed_for_size(size) + 0.001)
                    self.assertEqual(self.samples[-1][3:], (size, size))

    def test_selected_file_and_stored_file_share_pacing(self):
        with tempfile.TemporaryDirectory() as folder:
            file = Path(folder) / "file.txt"
            file.write_bytes(bytes(123))
            self.backend.start_tx(file)
            self.backend._transfer_timer.stop()
            started = self.backend._transfer_started
            profile = timing_profile(False)
            elapsed = (profile.data_start + profile.data_end(123)) / 2
            self.at(started, elapsed)
            selected_bytes = self.samples[-1][3]
            first_id = self.samples[-1][1]
            self.backend.stop()
            started, profile = self.begin_stored(123)
            self.at(started, elapsed)
            self.assertEqual(self.samples[-1][3], selected_bytes)
            self.assertNotEqual(self.samples[-1][1], first_id)

    def test_stop_disconnect_and_radio_action_prevent_late_completion(self):
        started, profile = self.begin_stored(500)
        self.now = started + (profile.data_start + profile.data_end(500)) / 2
        self.backend.stop()
        self.assertEqual(self.samples[-1][2], "Cancelled")
        observed = len(self.samples)
        self.at(started, profile.elapsed_for_size(500) + 1)
        self.assertEqual(len(self.samples), observed)
        self.begin_stored(500)
        active_id = self.samples[-1][1]
        self.backend.disconnect_from_device()
        self.assertEqual(self.ended[-1], ("TX", active_id, "Failed"))
        self.backend._advance_transfer()
        self.assertEqual(len(self.samples), observed + 1)

    def test_duplicate_start_is_rejected_without_replacing_active_session(self):
        started, profile = self.begin_stored(500)
        active_id = self.samples[-1][1]
        self.backend.start_stored_tx("file.txt")
        self.assertEqual(self.backend._transfer_started, started)
        self.assertEqual(self.samples[-1][1], active_id)
        self.backend.enter_standby()
        self.assertEqual(self.ended[-1], ("TX", active_id, "Cancelled"))
        self.assertIsNone(self.backend._transfer_started)

    def test_stop_after_elapsed_completion_preserves_terminal_result(self):
        started, profile = self.begin_stored(100, True)
        self.now = started + profile.elapsed_for_size(100) + 0.001
        self.backend.stop()
        self.assertEqual(self.samples[-1][2:], ("SentUnconfirmed", 100, 100))
        self.assertEqual(self.progress[-1].state, TransferState.SENT_UNCONFIRMED)


if __name__ == "__main__":
    unittest.main()

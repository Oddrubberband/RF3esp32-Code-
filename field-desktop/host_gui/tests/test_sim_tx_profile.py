import unittest

from rf3_gui.app.sim_tx import timing_profile


class SenderTimingTests(unittest.TestCase):
    def test_known_capture_times_match_payload_and_repeat_at_wall_speed(self):
        for no_ack, ms, count, span, payload in (
            (False, 8968, 1900, 185965, 28180),
            (True, 8990, 13520, 744104, 1129740),
        ):
            with self.subTest(no_ack=no_ack):
                profile = timing_profile(no_ack)
                size = 2 * payload + 109
                self.assertAlmostEqual(profile.data_end(payload) - profile.data_start, span / 1000)
                for repeats in (0, 1, 2):
                    elapsed = profile.data_start + (ms + repeats * span) / 1000
                    self.assertEqual(profile.bytes_at(size, elapsed), min(size, count + repeats * payload))

    def test_partial_frame_and_control_tail(self):
        for no_ack in (True, False):
            with self.subTest(no_ack=no_ack):
                profile = timing_profile(no_ack)
                self.assertEqual(profile.data_end(21), profile.data_end(40))
                self.assertGreater(profile.data_end(21), profile.data_end(20))
                self.assertEqual(profile.bytes_at(21, profile.data_end(21) - 0.0001), 20)
                self.assertEqual(profile.bytes_at(21, profile.data_end(21)), 21)
                self.assertGreater(profile.elapsed_for_size(21), profile.data_end(21))
                self.assertEqual(profile.bytes_at(0, 1000), 0)

    def test_original_file_sizes_finish_within_one_bench_report_of_capture(self):
        for no_ack, size, observed in ((False, 28502, 188.941), (True, 1134034, 747.086)):
            with self.subTest(no_ack=no_ack):
                self.assertLess(abs(timing_profile(no_ack).elapsed_for_size(size) - observed), 3.1)

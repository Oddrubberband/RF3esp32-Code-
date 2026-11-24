from dataclasses import replace
import unittest
from unittest.mock import patch

from PySide6.QtWidgets import QApplication

from rf3_gui.app.controller import AppController
from rf3_gui.app.live_metrics import SessionTraces, StatusCadence
from rf3_gui.app.state_model import ConnectionState, DeviceSnapshot
from rf3_gui.device.mock_device import MockDeviceBackend
from rf3_gui.ui.metrics_page import MetricsPage


class PcRateTests(unittest.TestCase):
    def test_eta_uses_transfer_average_after_a_burst_and_during_a_wait(self):
        book = SessionTraces()
        book.observe("RX", "one", "Receiving", 100, 2000, 0)
        trace = book.traces["RX"]
        self.assertIsNone(trace.eta(1))
        book.observe("RX", "one", "Receiving", 900, 2000, 4)
        book.observe("RX", "one", "Receiving", 1100, 2000, 10)
        self.assertAlmostEqual(trace.points[-1].rate, 200 / 6)
        self.assertEqual(trace.average_rate(10), 100)
        self.assertEqual(trace.eta(10), 9)
        self.assertAlmostEqual(trace.eta(12), 10.8)
        self.assertIsNone(trace.eta(20))

    def test_whole_tx_average_includes_setup_and_verification_after_history_expires(self):
        book = SessionTraces()
        book.tx_command(True, 5)
        book.observe("TX", "one", "Transferring", 200, 2000, 10)
        trace = book.traces["TX"]
        self.assertEqual(trace.average_rate(10), 40)
        for t in range(11, 106):
            book.observe("TX", "one", "Transferring", (t - 10) * 18 + 200, 2000, t)
        book.observe("TX", "one", "Verifying", 2000, 2000, 106)
        book.observe("TX", "one", "Completed", 2000, 2000, 110)
        book.tx_command(False, 110)
        self.assertGreater(trace.points[0].t, 10)
        self.assertAlmostEqual(trace.average_rate(999), 2000 / 105)
        book.tx_command(True, 1000)
        book.observe("TX", "two", "SentUnconfirmed", 1600, 1600, 1002)
        self.assertEqual(book.traces["TX"].average_rate(2000), 800)

    def test_joined_mid_transfer_counts_only_observed_bytes(self):
        book = SessionTraces()
        book.observe("TX", "one", "Transferring", 500, 1500, 8)
        book.observe("TX", "one", "Transferring", 1100, 1500, 10)
        self.assertEqual(book.traces["TX"].average_rate(10), 300)
        book.end_observation("TX", "one", "Failed")
        self.assertIsNone(book.traces["TX"].average_rate(200))

    def test_partial_start_irregular_intervals_and_payload_eta(self):
        book = SessionTraces()
        for t in (0, 1.5, 4, 7):
            book.observe("RX", "A", "Receiving", int(1024*(20+t)), 100*1024, t)
        trace = book.traces["RX"]
        self.assertAlmostEqual(trace.points[-1].rate, 1024)
        self.assertAlmostEqual(trace.eta(7), 73)
        self.assertIsNone(trace.eta(17))
        book.observe("RX", "A", "Receiving", 48*1024, 100*1024, 22)
        self.assertIsNone(trace.points[-1].rate)
        book.observe("RX", "A", "Receiving", 52*1024, 100*1024, 24)
        self.assertEqual(trace.points[-1].rate, 2048)

    def test_stalls_outcomes_and_direction_isolation(self):
        book = SessionTraces()
        for t, n in ((0,0), (2, 20), (5,20), (9,20)):
            book.observe("TX", "A", "Transferring", n, 100, t)
        tx = book.traces["TX"]
        self.assertEqual(tx.points[-1].rate, 0)
        self.assertEqual(tx.eta(9), 36)
        book.observe("RX", "A", "Verifying", 50, 50, 10)
        self.assertIsNone(book.traces["RX"].eta(10))
        book.observe("TX", "A", "SentUnconfirmed", 100, 100, 11)
        self.assertIsNone(tx.eta(11))
        book.observe("TX", "A", "Transferring", 30, 100, 12)
        self.assertEqual(tx.phase, "SentUnconfirmed")
        book.observe("TX", "B", "Preparing", 0, 64, 13)
        self.assertEqual(len(book.traces["TX"].points), 1)
        self.assertEqual(book.traces["RX"].total, 50)

    def test_denser_input_has_bounded_history_and_an_unbiased_rate(self):
        book = SessionTraces()
        for n in range(25000):
            book.observe("RX", "one", "Receiving", n, 30000, n/100)
        points = book.traces["RX"].points
        self.assertLess(len(points), 250)
        self.assertAlmostEqual(points[-1].rate, 100, places=6)

    def test_status_cadence_uses_recent_report_intervals(self):
        cadence = StatusCadence()
        for t in (1, 4, 7, 12, 12, 10):
            cadence.observe(t)
        self.assertEqual(cadence.typical(), 3)
        self.assertEqual(max(cadence.intervals), 5)
        for t in range(20, 44):
            cadence.observe(t)
        self.assertEqual(len(cadence.intervals), 20)
        self.assertEqual(cadence.typical(), 1)

    def test_local_end_retains_last_confirmed_sample(self):
        book = SessionTraces()
        book.observe("TX", "A", "Transferring", 25, 100, 5)
        book.end_observation("TX", "B", "Failed")
        self.assertEqual(book.traces["TX"].phase, "Transferring")
        book.end_observation("TX", "A", "Failed")
        trace = book.traces["TX"]
        self.assertTrue(trace.local_end)
        self.assertEqual((len(trace.points), trace.points[-1].t, trace.points[-1].count), (1,5,25))
        self.assertIsNone(trace.eta(15))


class LiveMetricsPageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def test_chart_scrolls_before_during_and_after_transfer(self):
        backend = MockDeviceBackend()
        controller = AppController(backend)
        page = MetricsPage(controller)
        self.addCleanup(page.close)
        page.show()
        self.assertTrue(page.plot.frames.isActive())
        start = page.plot.clock_origin
        self.assertEqual(page.plot.time_bounds(start+1), (-59, 1))
        with patch("rf3_gui.ui.metrics_page.monotonic", return_value=10):
            backend.rf_tx_command_active.emit(True)
        with patch("rf3_gui.ui.metrics_page.monotonic", return_value=12):
            backend.rf_sample.emit("TX", "A", "Transferring", 600, 1000)
            page.show()
            page.refresh()
            self.assertEqual(page.average.text(), "300 B/s")
            self.assertEqual(page.eta.text(), "2 s")
            self.assertTrue(page.plot.frames.isActive())
            a, b = page.plot.time_bounds(13)
            c, d = page.plot.time_bounds(13.016)
            self.assertAlmostEqual(c-a, 0.016)
            self.assertAlmostEqual(d-b, 0.016)
            self.assertEqual(len(page.plot.trace.points), 1)
        with patch("rf3_gui.ui.metrics_page.monotonic", return_value=15):
            backend.rf_sample.emit("TX", "A", "Completed", 1000, 1000)
            backend.rf_tx_command_active.emit(False)
            page.refresh()
            self.assertEqual(page.average.text(), "200 B/s")
            self.assertTrue(page.plot.frames.isActive())
            self.assertAlmostEqual(page.plot.time_bounds(45)[1]-page.plot.time_bounds(15)[1], 30)
            page.hide()
            self.assertFalse(page.plot.frames.isActive())

    def test_live_reports_and_connection_boundaries(self):
        backend = MockDeviceBackend()
        controller = AppController(backend)
        page = MetricsPage(controller)
        self.addCleanup(page.close)
        backend.status_changed.emit(replace(DeviceSnapshot(), connection_state=ConnectionState.CONNECTED))
        with patch("rf3_gui.ui.metrics_page.monotonic", return_value=20):
            backend.rf_sample.emit("TX","A","Transferring",512,4096)
            backend.fresh_status_report.emit()
        with patch("rf3_gui.ui.metrics_page.monotonic", return_value=22):
            backend.rf_sample.emit("TX","A","Transferring",2560,4096)
            backend.fresh_status_report.emit()
            page.refresh()
            self.assertEqual(page.rate.text(), "1.00 KiB/s")
            self.assertEqual(page.eta.text(), "2 s")
            self.assertIn("2.00 s", page.status_timing.text())
            self.assertEqual(page.source.text().split()[-1], "TX")
            page.track.setCurrentIndex(page.track.findData("RX"))
            self.assertIsNone(page.plot.trace)
            page.track.setCurrentIndex(page.track.findData("TX"))
            self.assertEqual(page.rate.text(), "1.00 KiB/s")
        with patch("rf3_gui.ui.metrics_page.monotonic", return_value=34):
            page.refresh()
            self.assertEqual(page.rate.text(), "—")
            self.assertIn("Reports paused", page.detail.text())
        backend.status_changed.emit(DeviceSnapshot())
        page.refresh()
        self.assertEqual(page.live.traces, {})
        self.assertIsNone(page.cadence.last)

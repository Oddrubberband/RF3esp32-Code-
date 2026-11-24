"""Navigation and persistent controls specific to the experimental window."""
import unittest
from dataclasses import replace
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest.mock import patch

from PySide6.QtCore import Qt
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication

from rf3_gui.app.controller import AppController
from rf3_gui.app.state_model import ConnectionState, RadioState, StorageListing
from rf3_gui.device.mock_device import MockDeviceBackend
from rf3_gui.ui.main_window import MainWindow
from rf3_gui.ui.workbench import PAGE_NAMES
from PySide6.QtWidgets import QBoxLayout
from rf3_gui.ui.workbench import TransferPanels


class WorkbenchNavigation(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def setUp(self):
        self.backend = MockDeviceBackend()
        self.controller = AppController(self.backend)
        self.window = MainWindow(self.controller)
        self.window.resize(1080, 720)
        self.window.show()
        self.addCleanup(self.window.close)

    def visit(self, name):
        self.window.navigation.setCurrentRow(PAGE_NAMES.index(name))
        self.app.processEvents()

    def test_every_destination_and_global_stop_reachable_by_keyboard(self):
        self.assertEqual(self.window.page_title.text(), "Connection")
        self.window.navigation.setFocus()
        QTest.keyClick(self.window.navigation, Qt.Key.Key_Home)
        for name in PAGE_NAMES:
            self.assertEqual(self.window.page_title.text(), name)
            self.assertTrue(self.window.stop_button.isVisible())
            self.assertFalse(self.window.stop_button.isEnabled())
            QTest.keyClick(self.window.navigation, Qt.Key.Key_Down)

    def test_file_panels_reflow_without_recreating_controls(self):
        self.visit("File transfer")
        panels = self.window.pages.currentWidget().findChild(TransferPanels)
        original = self.window.transmit_button
        for width, expected in ((1280, QBoxLayout.Direction.LeftToRight),
                                (800, QBoxLayout.Direction.TopToBottom),
                                (1280, QBoxLayout.Direction.LeftToRight)):
            self.window.resize(width, 720)
            self.app.processEvents()
            self.assertEqual(panels.row.direction(), expected)
            self.assertIs(self.window.transmit_button, original)
            self.assertTrue(self.window.stop_button.isVisible())

    def test_file_draft_and_confirmed_radio_survive_page_changes(self):
        self.controller.discover_devices()
        QTest.qWait(540)
        self.window.connect_button.click()
        QTest.qWait(600)
        with TemporaryDirectory() as directory:
            source = Path(directory) / "sample.txt"
            source.write_bytes(b"RF3 bench")
            self.visit("File transfer")
            with patch("rf3_gui.ui.main_window.QFileDialog.getOpenFileName", return_value=(str(source), "")):
                self.window.browse_button.click()
            self.assertTrue(self.window.transmit_button.isEnabled())
            self.window.rx_button.click()
            self.visit("Diagnostics")
            self.assertIn("Listening", self.window.link_state.text())
            self.assertIn("MOCK-001", self.window.station.text())
            self.window.stop_button.click()
            self.assertIn("Standby", self.window.link_state.text())
            self.visit("File transfer")
            self.assertEqual(self.controller.selected_file, source)
            self.assertIn(source.name, self.window.file_path_edit.text())

    def test_full_byte_count_cannot_advance_the_reported_receive_stage(self):
        base = replace(self.controller.snapshot, connection_state=ConnectionState.BUSY,
                       confirmed_radio_state=RadioState.UNKNOWN, rx_transfer_id="AABB0001",
                       rx_received_bytes=1024, rx_total_bytes=1024)
        self.visit("File transfer")
        for phase, caption in (("WaitingForEnd", "Waiting for end"), ("Verifying", "Verifying file"),
                               ("Publishing", "Saving file"), ("Completed", "File saved")):
            self.backend.status_changed.emit(replace(base, rx_transfer_state=phase))
            self.assertEqual(self.window.rx_progress.value(), 1000)
            active = [item.text() for item in self.window.rx_stages.labels if item.property("visualTone") == "stage"]
            self.assertEqual(active, [caption])
        for phase in ("Failed", "Cancelled"):
            self.backend.status_changed.emit(replace(base, rx_transfer_state=phase))
            self.assertFalse(self.window.rx_stages.isVisible())
            self.assertIn(phase, self.window.rx_transfer_value.text())
        self.backend.status_changed.emit(replace(base, connection_state=ConnectionState.DISCONNECTED))
        self.assertFalse(self.window.rx_progress.isVisible())
        self.assertEqual(self.window.link_state.text(), "Unavailable")

    def test_capacity_shows_only_valid_counts_from_this_connection(self):
        self.visit("Board files")
        base = self.controller.snapshot
        connected = replace(base, connection_state=ConnectionState.CONNECTED)
        self.backend.status_changed.emit(connected)
        for used, total, caption in ((0, 100, "0% used"), (1, 10000, "<1% used"),
                                     (3, 8, "37% used"), (8, 8, "100% used")):
            self.backend.storage_changed.emit(StorageListing((), used, total, True))
            self.assertTrue(self.window.capacity_bar.isVisible())
            self.assertEqual(self.window.capacity_bar.text(), caption)
        for used, total in ((5, 0), (9, 8), (-1, 8)):
            self.backend.storage_changed.emit(StorageListing((), used, total, True))
            self.assertFalse(self.window.capacity_bar.isVisible())
        self.backend.storage_changed.emit(StorageListing((), 7, 8, True))
        self.backend.status_changed.emit(base)
        self.assertFalse(self.window.capacity_bar.isVisible())
        self.backend.status_changed.emit(connected)
        self.assertFalse(self.window.capacity_bar.isVisible())


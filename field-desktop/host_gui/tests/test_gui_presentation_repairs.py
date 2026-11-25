from dataclasses import replace
from datetime import datetime, timedelta, timezone
import csv
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch

from PySide6.QtCore import Qt
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication, QFileDialog, QMessageBox

from rf3_gui.app.controller import AppController
from rf3_gui.app.delivery import COLUMNS, Reading
from rf3_gui.app.report_csv import ReportCapture
from rf3_gui.app.state_model import (
    ConnectionState, DeviceSnapshot, RadioCapabilities, RadioState, StorageListing,
    StoredFile, TransferProgress, TransferState,
)
from rf3_gui.device.base import DeviceBackend
from rf3_gui.ui.main_window import MainWindow
from rf3_gui.ui.workbench import PAGE_NAMES


class PresentationBackend(DeviceBackend):
    def disconnect_from_device(self):
        pass


class GuiPresentationRepairs(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def setUp(self):
        self.backend = PresentationBackend()
        self.controller = AppController(self.backend)
        self.window = MainWindow(self.controller)
        self.addCleanup(self.window.close)
        self.addCleanup(self.window.metrics.capture.close)
        self.snapshot = DeviceSnapshot(
            connection_state=ConnectionState.CONNECTED,
            confirmed_radio_state=RadioState.STANDBY,
            capabilities=RadioCapabilities(
                supports_host_storage=True, supports_stored_file_tx=True,
                supports_host_file_upload=True, supports_host_file_download=True,
                supports_text_messages=True,
            ),
        )
        self.backend.status_changed.emit(self.snapshot)

    def select_page(self, name):
        self.window.navigation.setCurrentRow(PAGE_NAMES.index(name))
        self.window.show()
        self.app.processEvents()

    def test_file_disappearing_during_validation_is_reported(self):
        error = FileNotFoundError("vanished")
        with patch.object(Path, "stat", side_effect=error), patch.object(QMessageBox, "warning") as warning:
            self.assertFalse(self.controller.set_selected_file(Path("vanished.bin")))
        warning.assert_called_once()
        self.assertIsNone(self.controller.selected_file)

    def test_deselected_storage_row_cannot_be_acted_on_or_restored(self):
        self.select_page("Board files")
        listing = StorageListing((StoredFile("old.bin", 3),), 3, 1000, True)
        self.backend.storage_changed.emit(listing)
        table = self.window.storage_table
        table.selectRow(0)
        self.assertTrue(self.window.storage_delete_button.isEnabled())
        cell = table.visualItemRect(table.item(0, 0)).center()
        QTest.mouseClick(table.viewport(), Qt.MouseButton.LeftButton, Qt.KeyboardModifier.ControlModifier, cell)
        self.assertEqual(table.currentRow(), 0)
        self.assertFalse(table.selectedItems())
        for button in (self.window.storage_export_button, self.window.storage_delete_button,
                       self.window.storage_select_button, self.window.storage_transmit_button):
            self.assertFalse(button.isEnabled())
        self.backend.storage_changed.emit(listing)
        self.assertFalse(table.selectedItems())

    def test_import_passes_replace_only_after_explicit_confirmation(self):
        listing = StorageListing((StoredFile("existing.bin", 3),), 3, 1000, True)
        self.backend.storage_changed.emit(listing)
        with patch.object(self.backend, "import_file") as import_file, patch.object(QFileDialog, "getOpenFileName") as choose:
            choose.return_value = ("new.bin", "")
            self.window._import_storage_file()
            import_file.assert_called_once_with(Path("new.bin"), replace_existing=False)
            import_file.reset_mock()
            choose.return_value = ("existing.bin", "")
            with patch.object(QMessageBox, "question", return_value=QMessageBox.StandardButton.No):
                self.window._import_storage_file()
            import_file.assert_not_called()
            with patch.object(QMessageBox, "question", return_value=QMessageBox.StandardButton.Yes):
                self.window._import_storage_file()
            import_file.assert_called_once_with(Path("existing.bin"), replace_existing=True)

    def test_choose_next_file_preserves_active_transfer_progress(self):
        self.select_page("File transfer")
        self.backend.status_changed.emit(replace(self.snapshot, connection_state=ConnectionState.BUSY))
        self.backend.transfer_changed.emit(TransferProgress(
            TransferState.TRANSFERRING, 200, 500, "Sending first file",
        ))
        with TemporaryDirectory() as directory:
            next_file = Path(directory) / "next.bin"
            next_file.write_bytes(b"new")
            with patch.object(QFileDialog, "getOpenFileName", return_value=(str(next_file), "")):
                self.window.browse_button.click()
            self.assertEqual(self.controller.selected_file, next_file)
        self.assertTrue(self.window.progress_bar.isVisible())
        self.assertEqual(self.window.progress_bar.value(), 400)
        self.assertIn("200/500 bytes", self.window.transfer_state_label.text())
        self.assertEqual(self.window.transfer_detail_label.text(), "Sending first file")

    def test_reply_clears_sent_text_but_preserves_new_draft(self):
        self.select_page("Text and CW")
        edit = self.window.live_text_edit
        with patch.object(self.backend, "send_text_message") as send:
            edit.setText("first")
            self.window.send_text_button.click()
            send.assert_called_once_with("first")
            edit.selectAll()
            QTest.keyClicks(edit, "next draft")
            self.backend.command_finished.emit("TEXT first", True, "TEXT TX: first")
            self.assertEqual(edit.text(), "next draft")
            self.window.send_text_button.click()
            self.backend.command_finished.emit("TEXT next draft", True, "TEXT TX: next draft")
            self.assertFalse(edit.text())

    def test_disabled_morse_field_cannot_send_from_sleep_with_return(self):
        self.select_page("Text and CW")
        self.backend.status_changed.emit(replace(self.snapshot, confirmed_radio_state=RadioState.SLEEP))
        self.assertFalse(self.window.send_morse_button.isEnabled())
        self.assertFalse(self.window.morse_text_edit.isEnabled())
        with patch.object(self.backend, "send_morse_text") as send:
            self.window.morse_text_edit.setText("SOS")
            QTest.keyClick(self.window.morse_text_edit, Qt.Key.Key_Return)
        send.assert_not_called()

    def test_cancelled_same_id_rx_attempt_can_restart_and_archive(self):
        page = self.window.metrics
        identity = "000000AE"
        samples = [
            (5, "Receiving", 10, 0),
            (5, "Cancelled", 20, 1),
            (6, "Receiving", 0, 0),
            (6, "Completed", 40, 2),
        ]
        for attempt, phase, received, packet in samples:
            self.backend.status_changed.emit(replace(
                self.snapshot, rx_transfer_id=identity, rx_attempt=attempt,
                rx_transfer_state=phase, rx_received_bytes=received, rx_total_bytes=40,
            ))
            self.backend.publish_rf_sample("RX", identity, phase, received, 40, {"packet_number": packet})
        # A duplicate terminal sample has no new evidence to archive.
        self.backend.publish_rf_sample("RX", identity, "Completed", 40, 40, {"packet_number": 2})
        trace = page.live.traces["RX"]
        self.assertEqual((trace.attempt, trace.phase, trace.points[-1].count), (6, "Completed", 40))
        with TemporaryDirectory() as directory:
            path = Path(directory) / "receive.csv"
            page.capture.save("RX", path)
            with path.open(newline="") as file:
                rows = list(csv.DictReader(file))
        self.assertEqual(
            [(r["rx_attempt"], r["phase"], r["payload_bytes"], r["packet_number"]) for r in rows],
            [(str(a), phase, str(received), str(packet)) for a, phase, received, packet in samples],
        )

    def test_restored_report_uses_latest_summary_and_final_peer_evidence(self):
        sender = ReportCapture()
        peer = ReportCapture()
        self.addCleanup(sender.close)
        self.addCleanup(peer.close)
        early = Reading.parse("DELIVERY1 Custom TX noack 0000A104 00000021 100 100 -1 -1 0 0 1 SentUnconfirmed")
        provisional = Reading.parse("DELIVERY1 Devboard RX noack 0000A104 00000021 100 100 10 10 -1 0 0 Observing")
        later_time = (datetime.now(timezone.utc) + timedelta(seconds=5)).isoformat(timespec="milliseconds")
        later = replace(early, id="0000A105", instance="00000023", time=later_time)
        sender.delivery.observe(early)
        sender.delivery.observe(later)
        peer.delivery.observe(provisional)
        with TemporaryDirectory() as directory:
            old, archived, final = (Path(directory) / name for name in ("old.csv", "archive.csv", "final.csv"))
            peer.save("RX", old)
            sender.delivery.import_csv(old)
            sender.save(None, archived)
            peer.delivery.observe(replace(provisional, received=95, verified=True, closed=True, result="Completed"))
            peer.save("RX", final)
            page = self.window.metrics
            with patch.object(QFileDialog, "getOpenFileName", return_value=(str(archived), "")), patch.object(QMessageBox, "information"):
                page.import_peer()
            self.assertIn(later.id, page.delivery_label.text())
            restored = page.capture.delivery
            self.assertNotIn(provisional.key, restored.local)
            self.assertIn(provisional.key, restored.peer)
            restored.import_csv(final)
            restored.import_csv(old)
            self.assertEqual(restored.summary(restored.local[early.key])["delivery_pct"], 95.0)

    def test_acknowledgements_outweigh_a_provisional_receiver_csv(self):
        page = self.window.metrics
        tx = Reading.parse("DELIVERY1 Custom TX ack 0000A104 00000021 100 100 100 -1 0 1 1 Completed")
        rx = Reading.parse("DELIVERY1 Devboard RX ack 0000A104 00000051 100 -1 31 -1 -1 0 0 Observing")
        page.delivery_update(tx)
        receiver = ReportCapture()
        receiver.delivery.observe(rx)
        with TemporaryDirectory() as directory:
            path = Path(directory) / "peer.csv"
            receiver.save("RX", path)
            with patch.object(QFileDialog, "getOpenFileName", return_value=(str(path), "")), patch.object(QMessageBox, "information"):
                page.import_peer()
        self.assertIn("100/100 delivered", page.delivery_label.text())
        self.assertNotIn("31 received", page.delivery_label.text())

    def test_utc_date_overflow_shows_import_warning(self):
        row = Reading.parse("DELIVERY1 Custom TX ack 0000A104 00000021 100 100 100 -1 0 1 1 Completed")
        page = self.window.metrics
        page.delivery_update(row)
        invalid = dict(page.capture.delivery.rows()[0], timestamp_utc="0001-01-01T00:00:00+01:00")
        with TemporaryDirectory() as directory:
            path = Path(directory) / "invalid.csv"
            with path.open("w", encoding="utf-8", newline="") as file:
                writer = csv.DictWriter(file, COLUMNS)
                writer.writeheader()
                writer.writerow(invalid)
            with patch.object(QFileDialog, "getOpenFileName", return_value=(str(path), "")), patch.object(QMessageBox, "warning") as warning:
                page.import_peer()
        warning.assert_called_once()
        self.assertFalse(page.capture.delivery.peer)

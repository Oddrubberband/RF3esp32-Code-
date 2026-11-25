"""Regressions for Practice state and Windows Bluetooth selection."""

from contextlib import nullcontext
from datetime import datetime, timedelta, timezone
from pathlib import Path
import tempfile
import threading
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication

from rf3_gui.app.state_model import ConnectionState, RadioState
from rf3_gui.device.mock_device import MOCK_STORAGE_CAPACITY, MockDeviceBackend
from rf3_gui.device.windows_bluetooth import SPP_SERVICE_UUID, WindowsBluetooth, matching_spp_ports


class TestPracticeRepairs(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def setUp(self):
        self.backend = MockDeviceBackend()
        self.addCleanup(self.backend.disconnect_from_device)
        self.backend._connect_timer.setInterval(10)
        self.backend._discovery_timer.setInterval(10)
        self.backend._morse_timer.setInterval(10)
        self.commands = []
        self.listings = []
        self.backend.command_finished.connect(lambda *result: self.commands.append(result))
        self.backend.storage_changed.connect(self.listings.append)

    def connect_device(self):
        self.backend.connect_to(self.backend._device)
        QTest.qWait(40)
        self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.CONNECTED)

    def test_cancelled_discovery_and_connect_cannot_publish_after_cancel(self):
        candidates = []
        self.backend.devices_discovered.connect(candidates.append)
        self.backend.discover()
        self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.SEARCHING)
        self.backend.disconnect_from_device()
        self.backend.connect_to(self.backend._device)
        self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.CONNECTING)
        self.backend.disconnect_from_device()
        QTest.qWait(40)
        self.assertFalse(candidates)
        self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.DISCONNECTED)
        self.backend.discover()
        QTest.qWait(40)
        self.assertEqual(candidates, [[self.backend._device]])
        self.connect_device()

    def test_stopped_morse_cannot_change_following_file_transfer(self):
        self.connect_device()
        self.backend.send_morse_text("E")
        self.backend.stop()
        self.backend.start_stored_tx("README.md")
        QTest.qWait(40)
        snapshot = self.backend._snapshot
        self.assertEqual(snapshot.connection_state, ConnectionState.BUSY)
        self.assertEqual(snapshot.confirmed_radio_state, RadioState.TX_BUSY)
        self.assertTrue(self.backend._transfer_timer.isActive())

    def test_morse_cannot_start_while_asleep(self):
        self.connect_device()
        self.backend.enter_sleep()
        self.backend.send_morse_text("E")
        self.assertFalse(self.commands[-1][1])
        self.assertEqual(self.backend._snapshot.confirmed_radio_state, RadioState.SLEEP)
        self.assertFalse(self.backend._morse_timer.isActive())

    def test_deleting_selected_file_and_reconnecting_clear_report_state(self):
        self.connect_device()
        self.backend.select_stored_file("README.md")
        self.backend.delete_file("README.md")
        self.assertEqual(self.backend._snapshot.selected_file, "")
        self.assertTrue(self.listings[-1].available)
        self.assertFalse(self.listings[-1].files)
        self.backend.disconnect_from_device()
        self.connect_device()
        self.assertFalse(self.listings[-1].available)

    def test_storage_capacity_and_replacement_authorization_are_enforced(self):
        self.connect_device()
        old_data = self.backend._stored_files["README.md"]
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "README.md"
            path.write_bytes(b"changed")
            self.backend.import_file(path)
            self.assertFalse(self.commands[-1][1])
            self.assertEqual(self.backend._stored_files[path.name], old_data)
            self.backend.import_file(path, replace_existing=True)
            self.assertEqual(self.backend._stored_files[path.name], b"changed")
            self.assertTrue(self.listings[-1].available)
            path = Path(folder) / "full.bin"
            path.write_bytes(bytes(MOCK_STORAGE_CAPACITY))
            self.backend.import_file(path)
            self.assertFalse(self.commands[-1][1])
            self.assertNotIn(path.name, self.backend._stored_files)
            self.assertEqual(self.listings[-1].used, len(b"changed"))

    def test_rf_transmit_preserves_board_copy_and_rejects_long_suffixes(self):
        self.connect_device()
        old_data = self.backend._stored_files["README.md"]
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "README.md"
            path.write_bytes(b"different")
            self.backend.start_tx(path)
            self.assertTrue(self.commands[-1][1])
            self.assertEqual(self.backend._stored_files[path.name], old_data)
            self.backend.stop()

            path = Path(folder) / "payload.longextension"
            path.write_bytes(b"kept intact")
            self.backend.start_tx(path)
            self.assertFalse(self.commands[-1][1])
            self.backend.import_file(path)
            self.assertEqual(self.backend._stored_files[path.name], b"kept intact")
            self.backend.start_stored_tx(path.name)
            self.assertFalse(self.commands[-1][1])
            self.assertIsNone(self.backend._transfer_started)


class TestBluetoothRepairs(unittest.TestCase):
    def test_explicit_non_spp_device_id_is_rejected_even_with_registry_match(self):
        address = "12:34:56:78:9A:BC"
        suffix = r"_LOCALMFG&000f\7&TEST&0&123456789ABC_C00000000"
        wrong = SimpleNamespace(device="COM5", description="Bluetooth modem",
                                hwid=r"BTHENUM\{00001103-0000-1000-8000-00805f9b34fb}" + suffix)
        spp = SimpleNamespace(device="COM6", description="Bluetooth Serial Port",
                              hwid=f"BTHENUM\\{{{SPP_SERVICE_UUID}}}" + suffix)
        unknown = SimpleNamespace(device="COM7", description="Bluetooth link",
                                  hwid=r"BTHENUM\DEVICE" + suffix)
        self.assertEqual(matching_spp_ports(address, [wrong, spp, unknown], ["COM5"]), ["COM6"])
        self.assertEqual(matching_spp_ports(address, [unknown], ["COM7"]), ["COM7"])

    def test_last_seen_before_inquiry_remains_cached(self):
        start = datetime(2026, 11, 24, 21, tzinfo=timezone.utc)
        finish = start + timedelta(seconds=10)
        cached = SimpleNamespace(address=0x123456789ABC, name="cached", authenticated=True,
                                 last_seen=SimpleNamespace(utc=lambda: start - timedelta(seconds=1.5)))
        live = SimpleNamespace(address=0x223456789ABC, name="live", authenticated=False,
                               last_seen=SimpleNamespace(utc=lambda: start + timedelta(seconds=1)))
        api = SimpleNamespace(radio=nullcontext, inquire=lambda _radio: [cached, live])
        adapter = WindowsBluetooth(api_factory=lambda: api)
        with patch("rf3_gui.device.windows_bluetooth.datetime", wraps=datetime) as clock:
            clock.now.side_effect = [start, finish]
            peers = adapter.scan(threading.Event())
        self.assertEqual([(peer.name, peer.seen_in_scan) for peer in peers],
                         [("live", True), ("cached", False)])

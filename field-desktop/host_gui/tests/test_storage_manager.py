import binascii
import json
import os
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
from PySide6.QtWidgets import QApplication, QMessageBox

from rf3_gui.app.controller import AppController
from rf3_gui.app.state_model import (ConnectionState, RadioCapabilities, RadioState,
                                     StoredFile, StorageListing)
from rf3_gui.device.serial_device import SerialDeviceBackend
from rf3_gui.protocol import console_protocol
from rf3_gui.ui.main_window import MainWindow


class TestStorageManager(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def setUp(self):
        self.backends = []

    def tearDown(self):
        for backend in self.backends:
            backend.disconnect_from_device()

    def backend(self):
        backend = SerialDeviceBackend()
        self.backends.append(backend)
        backend._serial = SimpleNamespace(close=lambda: None)
        backend._handshake_ok = True
        backend._snapshot = backend._replace_snapshot(
            connection_state=ConnectionState.CONNECTED, confirmed_radio_state=RadioState.STANDBY,
            capabilities=RadioCapabilities(supports_host_storage=True, supports_host_file_upload=True,
                                           supports_host_file_download=True, supports_host_chunk_retry=True))
        sent = []
        def capture(command):
            backend._pending_commands.append(command)
            sent.append(command)
        backend._send_command = capture
        return backend, sent

    def reply(self, backend, **values):
        backend._handle_line(json.dumps({"storage": values}))

    def test_paginated_list_reports_names_sizes_and_usage(self):
        backend, sent = self.backend()
        backend.refresh_storage()
        self.assertEqual(sent, ["STORAGE LIST 0"])
        first = [{"name": f"file{i}.bin", "size": i} for i in range(4)]
        self.reply(backend, op="list", offset=0, next=4, total=5, used=128, capacity=1024, files=first)
        self.assertEqual(sent[-1], "STORAGE LIST 4")
        self.reply(backend, op="list", offset=4, next=None, total=5, used=128, capacity=1024,
                   files=[{"name": 'résumé "one".bin', "size": 8}])
        self.assertEqual(len(backend._storage.files), 5)
        self.assertEqual(backend._storage.used, 128)
        self.assertEqual(backend._snapshot.connection_state, ConnectionState.CONNECTED)
        self.assertIsNone(backend._storage_kind)

    def test_empty_list_is_valid(self):
        backend, _ = self.backend()
        backend.refresh_storage()
        self.reply(backend, op="list", offset=0, next=None, total=0, used=0, capacity=1024, files=[])
        self.assertTrue(backend._storage.available)
        self.assertEqual(backend._storage.files, ())

    def test_malformed_or_protected_entries_fail_without_publishing_partial_list(self):
        for values in (
                dict(offset=1, next=None, total=0, files=[]),
                dict(offset=0, next=0, total=1, files=[]),
                dict(offset=0, next=None, total=1, files=[{"name": ".upload_backup", "size": 1}]),
                dict(offset=0, next=None, total=2, files=[{"name": "a", "size": 1}, {"name": "a", "size": 2}]),
                dict(offset=0, next=None, total=1, files=[{"name": "a", "size": True}])):
            backend, _ = self.backend()
            failures = []
            backend.command_finished.connect(lambda c, ok, text: failures.append(text) if not ok else None)
            backend.refresh_storage()
            self.reply(backend, op="list", used=0, capacity=1024, **values)
            self.assertTrue(failures)
            self.assertFalse(backend._storage.available)
            self.assertIsNone(backend._storage_kind)

    def test_changed_total_rejects_inconsistent_pages(self):
        backend, _ = self.backend()
        backend.refresh_storage()
        self.reply(backend, op="list", offset=0, next=1, total=2, used=0, capacity=1024,
                   files=[{"name": "a", "size": 1}])
        self.reply(backend, op="list", offset=1, next=None, total=3, used=0, capacity=1024,
                   files=[{"name": "b", "size": 1}])
        self.assertFalse(backend._storage.available)

    def test_failed_refresh_invalidates_previously_published_listing(self):
        backend, _ = self.backend()
        backend._storage = StorageListing((StoredFile("old.bin", 7),), 7, 1024, True)
        backend.refresh_storage()
        self.assertFalse(backend._storage.available)
        self.reply(backend, op="error", message="radio_busy_retry_list")
        self.assertFalse(backend._storage.available)
        self.assertEqual(backend._snapshot.connection_state, ConnectionState.CONNECTED)

    def test_delete_ack_must_match_selection_and_success_refreshes(self):
        backend, sent = self.backend()
        backend.delete_file("one.bin")
        self.assertEqual(sent[-1], console_protocol.storage_delete_command("one.bin"))
        self.reply(backend, op="delete", name="another.bin")
        self.assertIsNone(backend._storage_kind)
        self.assertNotIn("STORAGE LIST 0", sent)
        backend.delete_file("one.bin")
        self.reply(backend, op="delete", name="one.bin")
        self.assertEqual(sent[-1], "STORAGE LIST 0")

    def test_storage_timeout_disconnects_and_late_response_is_ignored(self):
        backend, _ = self.backend()
        backend.refresh_storage()
        old_generation = backend._connection_generation
        backend._storage_timer.timeout.emit()
        self.assertIsNone(backend._serial)
        backend._on_worker_event(old_generation, "line", '{"storage":{"op":"delete","name":"a"}}')
        self.assertEqual(backend._snapshot.connection_state, ConnectionState.ERROR)

    def test_unavailable_feature_and_protected_names_send_no_commands(self):
        backend, sent = self.backend()
        for name in ("../config", ".journal", "file.PART", "a/b", "a\\b", "a\x00b", "..", "x" * 31):
            backend.delete_file(name)
            backend.export_file(name, Path("unused"))
        self.assertEqual(sent, [])
        backend._snapshot = backend._replace_snapshot(capabilities=RadioCapabilities())
        backend.refresh_storage()
        self.assertEqual(sent, [])

    def test_import_binary_and_empty_files_are_verified_without_rf_tx(self):
        for payload in (b"", bytes(range(256)) * 2):
            backend, sent = self.backend()
            progress = []
            backend.transfer_changed.connect(progress.append)
            with tempfile.TemporaryDirectory() as directory:
                source = Path(directory) / "source file.bin"
                source.write_bytes(payload)
                backend.import_file(source)
                stored = bytearray()
                metadata = None
                cursor = 0
                for _ in range(100):
                    command = sent[cursor]
                    cursor += 1
                    words = command.split()
                    if words[:2] == ["UPLOAD", "BEGIN"]:
                        metadata = words[2:]
                        backend._handle_line("UPLOAD READY " + " ".join(metadata))
                    elif words[:2] == ["UPLOAD", "DATA"]:
                        stored.extend(bytes.fromhex(words[3]))
                        backend._handle_line(f"UPLOAD ACK {words[2]} {len(stored)}")
                    elif words == ["UPLOAD", "END"]:
                        backend._handle_line("UPLOAD COMPLETE " + " ".join(metadata))
                    elif words[:2] == ["DOWNLOAD", "BEGIN"]:
                        backend._handle_line("DOWNLOAD READY " + " ".join(metadata))
                    elif words[:2] == ["DOWNLOAD", "DATA"]:
                        seq = int(words[2]); data = stored[seq * 64:(seq + 1) * 64]
                        backend._handle_line(f"DOWNLOAD DATA {seq} {data.hex()}")
                    elif words == ["DOWNLOAD", "END"]:
                        backend._handle_line("DOWNLOAD COMPLETE " + " ".join(metadata))
                    elif words[:2] == ["STORAGE", "LIST"]:
                        self.reply(backend, op="list", offset=0, next=None, total=1,
                                   used=len(payload), capacity=1024,
                                   files=[{"name": metadata[0], "size": len(payload)}])
                        break
                    else:
                        self.fail("Unexpected command: " + command)
                self.assertEqual(bytes(stored), payload)
                self.assertTrue(any("Imported " in item.message and "byte-for-byte match" in item.message for item in progress))
                self.assertFalse(any(command.startswith(("TX", "TESTTX", "TEXT")) for command in sent))

    def test_export_hex_filename_with_spaces_and_unicode_verifies_bytes(self):
        backend, sent = self.backend()
        payload = b"\x00\xffabc\r\n"
        name = 'résumé "one".bin'
        encoded = name.encode().hex()
        crc = binascii.crc32(payload)
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "export.bin"
            backend.export_file(name, target)
            self.assertEqual(sent[-1], console_protocol.download_begin_hex_command(name))
            backend._handle_line(f"DOWNLOAD READYHEX {encoded} {len(payload)} {crc:08X}")
            self.assertEqual(sent[-1], "DOWNLOAD DATA 0")
            backend._handle_line(f"DOWNLOAD DATA 0 {payload.hex()}")
            backend._handle_line(f"DOWNLOAD COMPLETEHEX {encoded} {len(payload)} {crc:08X}")
            self.assertEqual(target.read_bytes(), payload)

    def test_export_preserves_existing_sidecar_and_cleans_only_its_unique_temp(self):
        backend, sent = self.backend()
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "export.bin"
            sidecar = Path(str(target) + ".rf3.part")
            sidecar.write_bytes(b"unrelated user file")
            target.write_bytes(b"existing destination")
            backend.export_file("one.bin", target)
            owned_temp = backend._download_session.partial_path
            self.assertNotEqual(owned_temp, sidecar)
            self.assertTrue(owned_temp.exists())
            backend._handle_line("DOWNLOAD ERROR read_failed")
            self.assertEqual(sidecar.read_bytes(), b"unrelated user file")
            self.assertEqual(target.read_bytes(), b"existing destination")
            self.assertFalse(owned_temp.exists())

    def test_import_name_normalization_never_creates_protected_files(self):
        for name in (".hidden", "a..b", "x.PART", "name with spaces.bin", "résumé.bin", "x" * 100):
            normalized = console_protocol.import_filename(name)
            self.assertTrue(console_protocol.valid_storage_name(normalized), normalized)

    def test_ui_delete_requires_confirmation_and_import_does_not_call_transmit(self):
        backend, _ = self.backend()
        controller = AppController(backend)
        window = MainWindow(controller)
        backend.status_changed.emit(backend._snapshot)
        backend.storage_changed.emit(StorageListing((StoredFile("a.bin", 5),), 5, 1024, True))
        window.storage_table.selectRow(0)
        self.assertTrue(window.storage_delete_button.isEnabled())
        with patch.object(controller, "delete_file") as delete, patch.object(QMessageBox, "question", return_value=QMessageBox.StandardButton.No):
            window.storage_delete_button.click()
            delete.assert_not_called()
        with patch.object(controller, "delete_file") as delete, patch.object(QMessageBox, "question", return_value=QMessageBox.StandardButton.Yes):
            window.storage_delete_button.click()
            delete.assert_called_once_with("a.bin")
        with patch("rf3_gui.ui.main_window.QFileDialog.getOpenFileName", return_value=("C:/new.bin", "")), patch.object(controller, "import_file") as upload, patch.object(controller, "start_tx") as tx:
            window.storage_import_button.click()
            upload.assert_called_once_with(Path("C:/new.bin"))
            tx.assert_not_called()
        window.close()

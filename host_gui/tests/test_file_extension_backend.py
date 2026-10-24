import binascii
import json
import os
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
from PySide6.QtWidgets import QApplication
from rf3_gui.app.state_model import ConnectionState, DeviceSnapshot, RadioCapabilities, RadioState
from rf3_gui.device.serial_device import SerialDeviceBackend, _safe_remote_filename
from rf3_gui.protocol import console_protocol


class TestFileExtensionBackend(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def setUp(self):
        self.backend = SerialDeviceBackend()
        self.backend._serial = SimpleNamespace(close=lambda: None)
        self.backend._handshake_ok = True
        self.backend._snapshot = DeviceSnapshot(
            connection_state=ConnectionState.CONNECTED, confirmed_radio_state=RadioState.STANDBY,
            capabilities=RadioCapabilities(supports_host_storage=True, supports_host_file_upload=True,
                                           supports_host_file_download=True, supports_host_chunk_retry=True,
                                           supports_stored_file_tx=True, host_chunk_max=256))
        self.events, self.progress = [], []
        self.backend.command_finished.connect(lambda *args: self.events.append(args))
        self.backend.transfer_changed.connect(self.progress.append)

    def tearDown(self):
        self.backend.disconnect_from_device()

    def commands(self):
        result = []
        while not self.backend._write_queue.empty():
            result.append(self.backend._write_queue.get_nowait())
        return result

    def test_invalid_original_extension_rejected_before_read_normalization_and_session(self):
        with tempfile.TemporaryDirectory() as directory:
            for mode in ("normal", "import", "test"):
                self.backend._snapshot = replace(self.backend._snapshot, test_mode=(mode == "test"))
                suffixes = (".file+type", ".é", ".part", ".PART")
                if mode == "normal":
                    suffixes += (".abcdefghij",)
                for suffix in suffixes:
                    source = Path(directory) / ("source" + suffix)
                    source.write_bytes(b"not read")
                    with patch.object(Path, "open", side_effect=AssertionError("invalid file was read")), \
                         patch("rf3_gui.device.serial_device._safe_remote_filename",
                               side_effect=AssertionError("invalid extension was normalized")):
                        if mode == "import":
                            self.backend.import_file(source)
                        else:
                            self.backend.start_tx(source)
                    self.assertIsNone(self.backend._upload_session)
                    self.assertIsNone(self.backend._download_session)
                    self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.CONNECTED)
                    self.assertFalse(self.backend._pending_commands)
                    self.assertEqual(self.commands(), [])
                    self.assertRegex(self.events[-1][2], "File type cannot be preserved|RF file type metadata")
                    self.assertFalse(self.events[-1][1])
            self.assertEqual(self.progress, [])

    def test_normalized_long_unicode_stem_keeps_case_and_full_ten_character_extension(self):
        with tempfile.TemporaryDirectory() as directory:
            for suffix in (".PDF", ".Abcdefghi"):
                source = Path(directory) / ("a very long résumé source file with spaces" + suffix)
                payload = b"abc"
                source.write_bytes(payload)
                self.backend.start_tx(source)
                session = self.backend._upload_session
                self.assertIsNotNone(session)
                self.assertTrue(session.remote_name.endswith(suffix))
                self.assertEqual(Path(session.remote_name).suffix, suffix)
                self.assertLessEqual(len(session.remote_name.encode("ascii")), 30)
                crc = binascii.crc32(payload) & 0xFFFFFFFF
                self.assertEqual(self.commands(), [console_protocol.upload_begin_command(
                    session.remote_name, len(payload), crc, 256)])
                self.backend._discard_host_transfer()
                self.backend._pending_commands.clear()
                self.backend._host_transfer_timer.stop()
                self.backend._snapshot = replace(self.backend._snapshot, connection_state=ConnectionState.CONNECTED)

    def test_host_only_long_extension_upload_and_exact_readback_without_rf(self):
        with tempfile.TemporaryDirectory() as directory:
            for mode in ("import", "test"):
                self.backend._snapshot = replace(self.backend._snapshot, test_mode=(mode == "test"))
                suffix = ".LongType1234"
                source = Path(directory) / ("a long résumé file with spaces" + suffix)
                payload = bytes(range(256)) * 2 + b"!"
                source.write_bytes(payload)
                crc = binascii.crc32(payload) & 0xFFFFFFFF
                if mode == "import":
                    self.backend.import_file(source)
                else:
                    self.backend.start_tx(source)
                upload = self.backend._upload_session
                self.assertIsNotNone(upload)
                self.assertTrue(upload.verify_only)
                remote = upload.remote_name
                self.assertEqual(Path(remote).suffix, suffix)
                self.assertLessEqual(len(remote.encode("ascii")), 30)
                all_commands = self.commands()
                self.assertEqual(all_commands[-1], console_protocol.upload_begin_command(remote, len(payload), crc, 256))
                self.backend._handle_line(f"UPLOAD READY {remote} {len(payload)} {crc:08X} 256")
                for sequence, offset in enumerate(range(0, len(payload), 256)):
                    chunk = payload[offset:offset + 256]
                    queued = self.commands()
                    all_commands += queued
                    self.assertEqual(queued, [console_protocol.upload_data_command(sequence, chunk)])
                    self.backend._handle_line(f"UPLOAD ACK {sequence} {offset + len(chunk)}")
                queued = self.commands()
                all_commands += queued
                self.assertEqual(queued, ["UPLOAD END"])
                self.backend._handle_line(f"UPLOAD COMPLETE {remote} {len(payload)} {crc:08X}")
                queued = self.commands()
                all_commands += queued
                self.assertEqual(queued, [console_protocol.download_begin_command(remote, 256)])
                self.backend._handle_line(f"DOWNLOAD READY {remote} {len(payload)} {crc:08X} 256")
                for sequence, offset in enumerate(range(0, len(payload), 256)):
                    chunk = payload[offset:offset + 256]
                    queued = self.commands()
                    all_commands += queued
                    self.assertEqual(queued, [console_protocol.download_data_command(sequence)])
                    self.backend._handle_line(f"DOWNLOAD DATA {sequence} {chunk.hex().upper()}")
                queued = self.commands()
                all_commands += queued
                self.assertEqual(queued, ["DOWNLOAD END"])
                self.backend._handle_line(f"DOWNLOAD COMPLETE {remote} {len(payload)} {crc:08X}")
                self.assertEqual(self.progress[-1].state.value, "Completed")
                self.assertIn("byte-for-byte match", self.progress[-1].message)
                self.assertFalse(any(command.startswith(("TX ", "TXHEX ")) for command in all_commands))
                self.assertIsNone(self.backend._upload_session)
                self.assertIsNone(self.backend._download_session)
                if mode == "import":
                    queued = self.commands()
                    self.assertEqual(queued, ["STORAGE LIST 0"])
                    self.backend._handle_line(json.dumps({"storage": {
                        "op": "list", "offset": 0, "next": None, "total": 1,
                        "used": len(payload), "capacity": 4096,
                        "files": [{"name": remote, "size": len(payload)}]}}))
                    self.assertIsNone(self.backend._storage_kind)

    def test_unpreservable_normalized_suffix_is_rejected_before_read(self):
        with tempfile.TemporaryDirectory() as directory:
            for mode in ("import", "test"):
                self.backend._snapshot = replace(self.backend._snapshot, test_mode=(mode == "test"))
                source = Path(directory) / "long résumé source.abcdefghijklmnopqrstu"
                source.write_bytes(b"not read")
                with patch.object(Path, "open", side_effect=AssertionError("unpreservable file read")):
                    if mode == "import":
                        self.backend.import_file(source)
                    else:
                        self.backend.start_tx(source)
                self.assertIsNone(self.backend._upload_session)
                self.assertEqual(self.commands(), [])
                self.assertIn("preserve", self.events[-1][2])
                self.assertFalse(self.events[-1][1])

    def test_stored_selection_allowed_but_rf_tx_rejects_unsupported_suffix(self):
        for name in ("legacy.abcdefghij", "legacy.a+b"):
            self.backend.select_stored_file(name)
            command = console_protocol.select_stored_command(name)
            self.assertEqual(self.commands(), [command])
            self.backend._handle_line("OK " + command)
            self.assertEqual(self.backend._snapshot.selected_file, name)
            self.backend.start_stored_tx(name)
            self.assertEqual(self.commands(), [])
            self.assertIsNone(self.backend._rf_tx_name)
            self.assertFalse(self.backend._rf_progress_timer.isActive())
            self.assertRegex(self.events[-1][2], "File type cannot be preserved|RF file type metadata")
            self.assertFalse(self.events[-1][1])
            self.assertEqual(self.progress, [])

    def test_supported_stored_file_suffix_uses_exact_name(self):
        for name in ("document.PDF", "report.Abcdefghi", "README"):
            self.backend.start_stored_tx(name)
            self.assertEqual(self.commands(), [console_protocol.stored_tx_command(name)])
            self.assertEqual(self.backend._rf_tx_name, name)
            self.backend._rf_tx_name = None
            self.backend._rf_progress_timer.stop()
            self.backend._pending_commands.clear()
            self.backend._command_deadlines.clear()
            self.backend._snapshot = replace(self.backend._snapshot, connection_state=ConnectionState.CONNECTED)

    def test_legacy_direct_name_helper_keeps_protected_part_normalization_behavior(self):
        normalized = _safe_remote_filename("a very long report name with spaces.part")
        self.assertFalse(normalized.lower().endswith(".part"))
        self.assertLessEqual(len(normalized.encode("ascii")), 30)


if __name__ == "__main__":
    unittest.main()

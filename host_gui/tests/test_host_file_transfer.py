import binascii
import hashlib
import os
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path
from unittest.mock import patch

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtWidgets import QApplication

from rf3_gui.app.state_model import (
    ConnectionState,
    DeviceInfo,
    DeviceSnapshot,
    RadioCapabilities,
    Telemetry,
    TransferState,
)
from rf3_gui.device.serial_device import SerialDeviceBackend, _safe_remote_filename
from rf3_gui.protocol import console_protocol


class TestHostFileTransfer(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def _connected_backend(self, *, bluetooth=False, received_name=""):
        backend = SerialDeviceBackend()
        backend._serial = object()
        backend._handshake_ok = True
        backend._active_device = DeviceInfo(
            "Bluetooth SPP" if bluetooth else "USB serial",
            "COM9",
            "serial",
        )
        backend._snapshot = DeviceSnapshot(
            connection_state=ConnectionState.CONNECTED,
            capabilities=RadioCapabilities(
                supports_host_file_upload=True,
                supports_host_file_download=True,
                supports_host_chunk_retry=True,
            ),
            telemetry=Telemetry(
                rx_saved=1 if received_name else 0,
                last_rx_file=received_name,
            ),
        )
        sent = []

        def capture(command):
            backend._pending_commands.append(command)
            sent.append(command)

        backend._send_command = capture
        return backend, sent

    def test_upload_chunks_then_starts_radio_tx_over_usb_console(self):
        backend, sent = self._connected_backend()
        progress = []
        backend.transfer_changed.connect(progress.append)

        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "hello world.bin"
            payload = bytes(range(70))
            source.write_bytes(payload)
            crc = binascii.crc32(payload) & 0xFFFFFFFF

            backend.start_tx(source)
            remote_name = _safe_remote_filename(source.name)
            self.assertEqual(
                sent[-1],
                console_protocol.upload_begin_command(remote_name, len(payload), crc),
            )

            backend._handle_line(f"UPLOAD READY {remote_name} 70 {crc:08X}")
            self.assertEqual(sent[-1], console_protocol.upload_data_command(0, payload[:64]))
            backend._handle_line("UPLOAD ACK 0 64")
            self.assertEqual(sent[-1], console_protocol.upload_data_command(1, payload[64:]))
            backend._handle_line("UPLOAD ACK 1 70")
            self.assertEqual(sent[-1], console_protocol.upload_end_command())
            backend._handle_line(f"UPLOAD COMPLETE {remote_name} 70 {crc:08X}")

            self.assertEqual(sent[-1], console_protocol.tx_command(remote_name))
            self.assertIsNone(backend._upload_session)
            self.assertEqual(progress[-1].state, TransferState.HANDSHAKING)

    def test_bluetooth_uses_structured_upload_response_as_ack(self):
        backend, sent = self._connected_backend(bluetooth=True)
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "tiny.bin"
            source.write_bytes(b"abc")
            crc = binascii.crc32(b"abc") & 0xFFFFFFFF

            backend.start_tx(source)
            backend._handle_line(f"UPLOAD READY tiny.bin 3 {crc:08X}")
            self.assertEqual(sent[-1], console_protocol.upload_data_command(0, b"abc"))

    def _begin_validation(self, directory, payload, *, bluetooth=True):
        backend, sent = self._connected_backend(bluetooth=bluetooth)
        self.addCleanup(backend.disconnect_from_device)
        backend._snapshot = replace(backend._snapshot, test_mode=True)
        progress = []
        backend.transfer_changed.connect(progress.append)
        source = Path(directory) / "validate.bin"
        source.write_bytes(payload)
        crc = binascii.crc32(payload) & 0xFFFFFFFF
        backend.start_tx(source)
        self.assertEqual(sent[-1], console_protocol.upload_begin_command("validate.bin", len(payload), crc))
        backend._handle_line(f"UPLOAD READY validate.bin {len(payload)} {crc:08X}")
        for sequence, offset in enumerate(range(0, len(payload), 64)):
            chunk = payload[offset:offset + 64]
            self.assertEqual(sent[-1], console_protocol.upload_data_command(sequence, chunk))
            backend._handle_line(f"UPLOAD ACK {sequence} {offset + len(chunk)}")
        self.assertEqual(sent[-1], "UPLOAD END")
        self.assertNotEqual(progress[-1].state, TransferState.COMPLETED)
        return backend, sent, progress, crc

    def test_test_mode_roundtrip_verifies_binary_and_empty_files_without_rf(self):
        for bluetooth in (False, True):
            for payload in (b"", bytes(range(256)) + b"\x00\r\n\xff"):
                with self.subTest(bluetooth=bluetooth, size=len(payload)), tempfile.TemporaryDirectory() as directory:
                    backend, sent, progress, crc = self._begin_validation(directory, payload, bluetooth=bluetooth)
                    backend._handle_line(f"UPLOAD COMPLETE validate.bin {len(payload)} {crc:08X}")
                    self.assertEqual(sent[-1], "DOWNLOAD BEGIN validate.bin")
                    self.assertEqual(progress[-1].state, TransferState.VERIFYING)
                    backend._handle_line(f"DOWNLOAD READY validate.bin {len(payload)} {crc:08X}")
                    for sequence, offset in enumerate(range(0, len(payload), 64)):
                        self.assertEqual(sent[-1], f"DOWNLOAD DATA {sequence}")
                        backend._handle_line(f"DOWNLOAD DATA {sequence} {payload[offset:offset + 64].hex()}")
                    self.assertEqual(sent[-1], "DOWNLOAD END")
                    self.assertNotEqual(progress[-1].state, TransferState.COMPLETED)
                    backend._handle_line(f"DOWNLOAD COMPLETE validate.bin {len(payload)} {crc:08X}")
                    self.assertEqual(progress[-1].state, TransferState.COMPLETED)
                    self.assertIn(hashlib.sha256(payload).hexdigest(), progress[-1].message)
                    self.assertIn("byte-for-byte match", progress[-1].message)
                    self.assertEqual(progress[-1].sent_bytes, len(payload))
                    self.assertEqual(backend._snapshot.connection_state, ConnectionState.CONNECTED)
                    self.assertTrue(backend._status_poll_timer.isActive())
                    self.assertIsNone(backend._download_session)
                    self.assertEqual(list(Path(directory).iterdir()), [Path(directory) / "validate.bin"])
                    self.assertFalse(any(command.startswith(("TX", "TESTTX")) for command in sent))
                    backend.disconnect_from_device()

    def test_validation_choice_stays_no_rf_when_status_changes(self):
        with tempfile.TemporaryDirectory() as directory:
            backend, sent, progress, crc = self._begin_validation(directory, b"abc")
            backend._handle_line('{"state":"Fault","test_mode":false,"tx_state":"Completed"}')
            self.assertNotEqual(progress[-1].state, TransferState.COMPLETED)
            backend._handle_line(f"UPLOAD COMPLETE validate.bin 3 {crc:08X}")
            self.assertEqual(sent[-1], "DOWNLOAD BEGIN validate.bin")
            self.assertFalse(any(command.startswith("TX") for command in sent))

    def test_validation_rejects_wrong_readback_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            backend, sent, progress, crc = self._begin_validation(directory, b"abc")
            backend._handle_line(f"UPLOAD COMPLETE validate.bin 3 {crc:08X}")
            backend._handle_line(f"DOWNLOAD READY validate.bin 4 {crc:08X}")
            self.assertEqual(progress[-1].state, TransferState.FAILED)
            self.assertIn("metadata", progress[-1].message)
            self.assertIsNone(backend._download_session)
            self.assertEqual(backend._write_queue.get_nowait(), "DOWNLOAD CANCEL")

    def test_validation_rejects_corrupt_bytes_even_if_crc_collides(self):
        for collision in (False, True):
            with self.subTest(collision=collision), tempfile.TemporaryDirectory() as directory:
                backend, sent, progress, crc = self._begin_validation(directory, b"abc")
                backend._handle_line(f"UPLOAD COMPLETE validate.bin 3 {crc:08X}")
                backend._handle_line(f"DOWNLOAD READY validate.bin 3 {crc:08X}")
                # Inject a CRC collision to verify the independent byte comparison.
                if collision:
                    with patch("rf3_gui.device.serial_device.binascii.crc32", return_value=crc):
                        backend._handle_line("DOWNLOAD DATA 0 78797A")
                else:
                    backend._handle_line("DOWNLOAD DATA 0 78797A")
                backend._handle_line(f"DOWNLOAD COMPLETE validate.bin 3 {crc:08X}")
                self.assertEqual(progress[-1].state, TransferState.FAILED)
                self.assertIn("original file bytes" if collision else "CRC32", progress[-1].message)
                self.assertFalse(any(command.startswith("TX") for command in sent))

    def test_validation_readback_retries_duplicates_and_disconnect_cleanup(self):
        with tempfile.TemporaryDirectory() as directory:
            payload = bytes(range(70))
            backend, sent, progress, crc = self._begin_validation(directory, payload)
            backend._handle_line(f"UPLOAD COMPLETE validate.bin 70 {crc:08X}")
            backend._handle_line(f"DOWNLOAD READY validate.bin 70 {crc:08X}")
            backend._on_host_transfer_timeout()
            self.assertEqual(backend._write_queue.get_nowait(), "DOWNLOAD DATA 0")
            first = f"DOWNLOAD DATA 0 {payload[:64].hex()}"
            backend._handle_line(first)
            backend._handle_line(first)
            self.assertEqual(backend._download_session.received_bytes, 64)
            memory_file = backend._download_session.file
            backend.disconnect_from_device()
            self.assertTrue(memory_file.closed)
            self.assertIsNone(backend._download_session)
            self.assertEqual(progress[-1].state, TransferState.FAILED)

    def test_validation_missing_end_response_fails_without_rf(self):
        with tempfile.TemporaryDirectory() as directory:
            backend, sent, progress, crc = self._begin_validation(directory, b"abc")
            backend._handle_line(f"UPLOAD COMPLETE validate.bin 3 {crc:08X}")
            backend._handle_line(f"DOWNLOAD READY validate.bin 3 {crc:08X}")
            backend._handle_line("DOWNLOAD DATA 0 616263")
            backend._on_host_transfer_timeout()
            self.assertEqual(progress[-1].state, TransferState.FAILED)
            self.assertIsNone(backend._download_session)
            self.assertFalse(any(command.startswith("TX") for command in sent))

    def test_validation_failed_upload_never_starts_readback_or_rf(self):
        with tempfile.TemporaryDirectory() as directory:
            backend, sent, progress, crc = self._begin_validation(directory, b"abc")
            backend._handle_line("UPLOAD ERROR crc_mismatch")
            self.assertEqual(progress[-1].state, TransferState.FAILED)
            self.assertIsNone(backend._upload_session)
            self.assertIsNone(backend._download_session)
            self.assertFalse(any(command.startswith(("TX", "DOWNLOAD")) for command in sent))

    def test_validation_upload_completion_requires_matching_crc(self):
        with tempfile.TemporaryDirectory() as directory:
            backend, sent, progress, crc = self._begin_validation(directory, b"abc")
            backend._handle_line("UPLOAD COMPLETE validate.bin 3 00000000")
            self.assertEqual(progress[-1].state, TransferState.FAILED)
            self.assertIsNone(backend._upload_session)
            self.assertFalse(any(command.startswith(("TX", "DOWNLOAD")) for command in sent))

    def test_validation_requires_capabilities_and_confirmed_test_mode(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "validate.bin"
            source.write_bytes(b"abc")
            backend, sent = self._connected_backend()
            self.addCleanup(backend.disconnect_from_device)
            backend._snapshot = replace(backend._snapshot, test_mode=True, capabilities=replace(
                backend._snapshot.capabilities, supports_host_file_download=False
            ))
            backend.start_tx(source)
            self.assertEqual(sent, [])
            backend._snapshot = replace(backend._snapshot, capabilities=replace(
                backend._snapshot.capabilities, supports_host_file_download=True
            ))
            backend._awaiting_test_mode = True
            backend.start_tx(source)
            self.assertEqual(sent, [])
            backend._awaiting_test_mode = None
            backend.start_tx(source)
            backend.set_test_mode(False)
            self.assertTrue(backend._snapshot.test_mode)
            self.assertEqual(len(sent), 1)

    def test_download_verifies_and_atomically_publishes_file(self):
        backend, sent = self._connected_backend(received_name="rx_12345678.bin")
        progress = []
        backend.transfer_changed.connect(progress.append)
        payload = bytes(range(70))
        crc = binascii.crc32(payload) & 0xFFFFFFFF

        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "saved.bin"
            backend.save_received_file(destination)
            self.assertEqual(
                sent[-1],
                console_protocol.download_begin_command("rx_12345678.bin"),
            )

            backend._handle_line(f"DOWNLOAD READY rx_12345678.bin 70 {crc:08X}")
            self.assertEqual(sent[-1], console_protocol.download_data_command(0))
            backend._handle_line(f"DOWNLOAD DATA 0 {payload[:64].hex().upper()}")
            self.assertEqual(sent[-1], console_protocol.download_data_command(1))
            backend._handle_line(f"DOWNLOAD DATA 1 {payload[64:].hex().upper()}")
            self.assertEqual(sent[-1], console_protocol.download_end_command())
            backend._handle_line(f"DOWNLOAD COMPLETE rx_12345678.bin 70 {crc:08X}")

            self.assertEqual(destination.read_bytes(), payload)
            self.assertFalse(destination.with_name("saved.bin.rf3.part").exists())
            self.assertIsNone(backend._download_session)
            self.assertEqual(progress[-1].state, TransferState.COMPLETED)

    def test_remote_filename_is_ascii_bounded_and_not_internal(self):
        sanitized = _safe_remote_filename("a very long report name with spaces.part")
        self.assertLessEqual(len(sanitized.encode("ascii")), 30)
        self.assertNotIn(" ", sanitized)
        self.assertFalse(sanitized.lower().endswith(".part"))

    def test_filename_normalization_preserves_distinct_sources(self):
        self.assertNotEqual(_safe_remote_filename("a b.bin"), _safe_remote_filename("a_b.bin"))
        self.assertNotEqual(_safe_remote_filename("a" * 40 + "1.bin"),
                            _safe_remote_filename("a" * 40 + "2.bin"))

    def test_upload_retries_same_chunk_without_advancing_bytes(self):
        backend, sent = self._connected_backend()
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "retry.bin"
            source.write_bytes(b"abc")
            crc = binascii.crc32(b"abc") & 0xFFFFFFFF
            backend.start_tx(source)
            backend._handle_line(f"UPLOAD READY retry.bin 3 {crc:08X}")
            data_command = sent[-1]
            for _ in range(3):
                backend._on_host_transfer_timeout()
                self.assertEqual(backend._write_queue.get_nowait(), data_command)
                self.assertEqual(backend._upload_session.acknowledged_bytes, 0)
            backend._handle_line("UPLOAD ACK 0 3")
            self.assertEqual(sent[-1], "UPLOAD END")
            backend._handle_line("UPLOAD ACK 0 3")
            self.assertIsNotNone(backend._upload_session)
            backend._handle_line(f"UPLOAD COMPLETE retry.bin 3 {crc:08X}")
            self.assertEqual(sent[-1], "TX retry.bin")

    def test_download_ignores_delayed_duplicate_and_verifies_crc(self):
        backend, sent = self._connected_backend(received_name="rx_test.bin")
        payload = bytes(range(70))
        crc = binascii.crc32(payload) & 0xFFFFFFFF
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "saved.bin"
            backend.save_received_file(destination)
            backend._handle_line(f"DOWNLOAD READY rx_test.bin 70 {crc:08X}")
            backend._on_host_transfer_timeout()
            self.assertEqual(backend._write_queue.get_nowait(), "DOWNLOAD DATA 0")
            first = f"DOWNLOAD DATA 0 {payload[:64].hex()}"
            backend._handle_line(first)
            backend._handle_line(first)
            self.assertIsNotNone(backend._download_session)
            self.assertEqual(backend._download_session.received_bytes, 64)
            backend._handle_line(f"DOWNLOAD DATA 1 {payload[64:].hex()}")
            backend._handle_line(f"DOWNLOAD COMPLETE rx_test.bin 70 {crc:08X}")
            self.assertEqual(destination.read_bytes(), payload)

    def test_disconnect_removes_partial_without_replacing_destination(self):
        backend, _ = self._connected_backend(received_name="rx_test.bin")
        backend._serial = type("Port", (), {"close": lambda _: None})()
        progress = []
        backend.transfer_changed.connect(progress.append)
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "saved.bin"
            destination.write_bytes(b"existing")
            backend.save_received_file(destination)
            backend._handle_line("DOWNLOAD READY rx_test.bin 70 00000000")
            backend._handle_line(f"DOWNLOAD DATA 0 {bytes(64).hex()}")
            backend.disconnect_from_device()
            self.assertEqual(destination.read_bytes(), b"existing")
            self.assertFalse(destination.with_name("saved.bin.rf3.part").exists())
            self.assertEqual(progress[-1].state, TransferState.FAILED)

    def test_download_crc_failure_does_not_publish(self):
        backend, _ = self._connected_backend(received_name="rx_test.bin")
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "saved.bin"
            destination.write_bytes(b"existing")
            backend.save_received_file(destination)
            backend._handle_line("DOWNLOAD READY rx_test.bin 3 00000000")
            backend._handle_line("DOWNLOAD DATA 0 616263")
            backend._handle_line("DOWNLOAD COMPLETE rx_test.bin 3 00000000")
            self.assertEqual(destination.read_bytes(), b"existing")
            self.assertIsNone(backend._download_session)

    def test_missing_chunk_ack_fails_after_bounded_retries(self):
        backend, _ = self._connected_backend()
        progress = []
        backend.transfer_changed.connect(progress.append)
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "retry.bin"
            source.write_bytes(b"abc")
            crc = binascii.crc32(b"abc") & 0xFFFFFFFF
            backend.start_tx(source)
            backend._handle_line(f"UPLOAD READY retry.bin 3 {crc:08X}")
            for _ in range(4):
                backend._on_host_transfer_timeout()
            self.assertIsNone(backend._upload_session)
            self.assertEqual(progress[-1].state, TransferState.FAILED)

    def test_old_firmware_without_retry_capability_fails_without_resending(self):
        backend, _ = self._connected_backend()
        from dataclasses import replace
        backend._snapshot = replace(backend._snapshot, capabilities=replace(
            backend._snapshot.capabilities, supports_host_chunk_retry=False
        ))
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "retry.bin"
            source.write_bytes(b"abc")
            crc = binascii.crc32(b"abc") & 0xFFFFFFFF
            backend.start_tx(source)
            backend._handle_line(f"UPLOAD READY retry.bin 3 {crc:08X}")
            backend._on_host_transfer_timeout()
            self.assertIsNone(backend._upload_session)
            self.assertEqual(backend._write_queue.get_nowait(), "UPLOAD CANCEL")
            self.assertTrue(backend._write_queue.empty())


if __name__ == "__main__":
    unittest.main()

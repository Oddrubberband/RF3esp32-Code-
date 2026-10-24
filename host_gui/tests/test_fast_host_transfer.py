import binascii
import io
import json
import os
import tempfile
import threading
import unittest
from pathlib import Path
from types import SimpleNamespace

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
from PySide6.QtWidgets import QApplication

from rf3_gui.app.state_model import ConnectionState, RadioCapabilities, RadioState
from rf3_gui.device.serial_device import SerialDeviceBackend, SERIAL_READ_CHUNK_BYTES
from rf3_gui.protocol import console_protocol


class MemoryDriver(io.RawIOBase):
    """Driver-shaped byte source; no serial port or Bluetooth access."""
    def __init__(self, payload, stop):
        self.payload = bytearray(payload)
        self.stop = stop
        self.requests = []

    @property
    def in_waiting(self):
        return len(self.payload)

    def read(self, size=1):
        self.requests.append(size)
        if not self.payload:
            self.stop.set()
            return b""
        result = bytes(self.payload[:size])
        del self.payload[:size]
        return result


class TestFastHostTransfer(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def setUp(self):
        self.backends = []

    def tearDown(self):
        for backend in self.backends:
            backend.disconnect_from_device()

    def backend(self, chunk=256):
        backend = SerialDeviceBackend()
        self.backends.append(backend)
        backend._serial = SimpleNamespace(close=lambda: None)
        backend._handshake_ok = True
        backend._snapshot = backend._replace_snapshot(
            connection_state=ConnectionState.CONNECTED,
            confirmed_radio_state=RadioState.STANDBY,
            capabilities=RadioCapabilities(host_chunk_max=chunk, supports_host_storage=True,
                                           supports_host_file_upload=True, supports_host_file_download=True,
                                           supports_host_chunk_retry=True))
        commands = []
        def send(command):
            backend._pending_commands.append(command)
            commands.append(command)
        backend._send_command = send
        return backend, commands

    def test_bulk_reader_reduces_driver_calls_without_changing_line(self):
        packet = b"DOWNLOAD DATA 1000 " + b"AB" * 64 + b"\r\n"
        legacy = MemoryDriver(packet, threading.Event())
        self.assertEqual(legacy.readline(4097), packet)
        backend, _ = self.backend()
        events = []
        backend._worker_event.connect(lambda generation, kind, payload: events.append(payload) if kind == "line" else None)
        stop = threading.Event()
        driver = MemoryDriver(packet, stop)
        backend._read_loop(driver, stop, backend._connection_generation)
        self.assertEqual(events, [packet.decode().strip()])
        self.assertEqual(legacy.requests, [1] * len(packet))
        self.assertEqual(driver.requests, [len(packet), 1])

    def test_bulk_reader_bounds_corrupt_input_and_recovers(self):
        backend, _ = self.backend()
        stop = threading.Event()
        payload = b"x" * 5000 + b'\n{"state":"Standby","channel":42}\r\n'
        driver = MemoryDriver(payload, stop)
        backend._read_loop(driver, stop, backend._connection_generation)
        self.assertEqual(max(driver.requests), SERIAL_READ_CHUNK_BYTES)
        self.assertEqual(backend._snapshot.channel, 42)

    def test_idle_reader_requests_one_byte_and_handles_split_utf8(self):
        backend, _ = self.backend()
        stop = threading.Event()
        payload = '{"state":"Standby","selected":"café.bin"}\r\n'.encode()
        class FragmentedDriver(MemoryDriver):
            @property
            def in_waiting(self):
                return 0  # Simulate one arriving byte per driver operation.
        driver = FragmentedDriver(payload, stop)
        backend._read_loop(driver, stop, backend._connection_generation)
        self.assertTrue(all(size == 1 for size in driver.requests))
        self.assertEqual(backend._snapshot.selected_file, "café.bin")

    def test_capability_negotiation_defaults_and_bounds(self):
        for advertised, expected in ((None, 64), (True, 64), ("256", 64), (-1, 64),
                                     (0, 64), (63, 64), (64, 64), (128, 128),
                                     (256, 256), (8192, 256)):
            backend, _ = self.backend()
            backend._handle_line(json.dumps({"state": "Standby", "host_chunk_max": advertised}))
            self.assertEqual(backend._snapshot.capabilities.host_chunk_max, expected)

    def test_chunk_command_encodings_remain_legacy_when_omitted(self):
        self.assertEqual(console_protocol.upload_begin_command("a", 1, 2), "UPLOAD BEGIN a 1 00000002")
        self.assertEqual(console_protocol.download_begin_command("a"), "DOWNLOAD BEGIN a")
        self.assertEqual(console_protocol.download_begin_hex_command("a"), "DOWNLOAD BEGINHEX 61")
        self.assertTrue(console_protocol.upload_begin_command("a", 1, 2, 256).endswith(" 256"))
        for invalid in (True, 0, 63, 257, "256"):
            with self.assertRaises(ValueError):
                console_protocol.upload_begin_command("a", 1, 2, invalid)

    def test_import_chunk_boundaries_crc_and_verified_readback_at_both_sizes(self):
        for chunk in (64, 256):
            for count in (0, 1, 255, 256, 257, 513, 1041, 1048576):
                backend, commands = self.backend(chunk)
                data = bytes(index % 256 for index in range(count))
                reports = []
                backend.transfer_changed.connect(reports.append)
                with tempfile.TemporaryDirectory() as directory:
                    path = Path(directory) / "test.bin"
                    path.write_bytes(data)
                    backend.import_file(path)
                    uploaded = bytearray()
                    metadata = None
                    upload_chunks, download_chunks = [], []
                    cursor = 0
                    for _ in range(40000):
                        command = commands[cursor]; cursor += 1
                        words = command.split()
                        if words[:2] == ["UPLOAD", "BEGIN"]:
                            metadata = words[2:5]
                            self.assertEqual(len(words), 6 if chunk != 64 else 5)
                            backend._handle_line("UPLOAD READY " + " ".join(metadata)
                                                 + (f" {chunk}" if chunk != 64 else ""))
                            # An unrelated STATUS must not alter the active session size.
                            backend._snapshot = backend._replace_snapshot(capabilities=RadioCapabilities(host_chunk_max=64, supports_host_storage=True))
                        elif words[:2] == ["UPLOAD", "DATA"]:
                            payload = bytes.fromhex(words[3])
                            upload_chunks.append(len(payload)); uploaded.extend(payload)
                            backend._handle_line(f"UPLOAD ACK {words[2]} {len(uploaded)}")
                        elif words == ["UPLOAD", "END"]:
                            self.assertEqual(binascii.crc32(uploaded), int(metadata[2], 16))
                            backend._handle_line("UPLOAD COMPLETE " + " ".join(metadata))
                        elif words[:2] == ["DOWNLOAD", "BEGIN"]:
                            self.assertEqual(len(words), 4 if chunk != 64 else 3)
                            backend._handle_line("DOWNLOAD READY " + " ".join(metadata)
                                                 + (f" {chunk}" if chunk != 64 else ""))
                        elif words[:2] == ["DOWNLOAD", "DATA"]:
                            seq = int(words[2]); payload = uploaded[seq * chunk:(seq + 1) * chunk]
                            download_chunks.append(len(payload))
                            backend._handle_line(f"DOWNLOAD DATA {seq} {payload.hex()}")
                        elif words == ["DOWNLOAD", "END"]:
                            backend._handle_line("DOWNLOAD COMPLETE " + " ".join(metadata))
                        elif words[:2] == ["STORAGE", "LIST"]:
                            backend._handle_line(json.dumps({"storage": {"op": "list", "offset": 0,
                                "next": None, "total": 1, "used": len(data), "capacity": 1048576,
                                "files": [{"name": "test.bin", "size": len(data)}]}}))
                            break
                        else:
                            self.fail("Unexpected command: " + command)
                    expected = (count + chunk - 1) // chunk
                    self.assertEqual(len(upload_chunks), expected)
                    self.assertEqual(len(download_chunks), expected)
                    self.assertTrue(all(0 < size <= chunk for size in upload_chunks + download_chunks))
                    self.assertEqual(bytes(uploaded), data)
                    self.assertTrue(any("byte-for-byte match" in report.message for report in reports))
                    self.assertFalse(any(command.startswith(("TX", "TESTTX", "TEXT")) for command in commands))

    def test_mismatched_ready_chunk_fails_before_payload_is_sent(self):
        backend, commands = self.backend()
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "a.bin"; source.write_bytes(b"a" * 300)
            backend.import_file(source)
            metadata = commands[0].split()[2:5]
            backend._handle_line("UPLOAD READY " + " ".join(metadata) + " 64")
            self.assertIsNone(backend._upload_session)
            self.assertFalse(any(command.startswith("UPLOAD DATA") for command in commands))

    def test_large_export_crc_failure_preserves_destination_and_cleans_temp(self):
        backend, commands = self.backend()
        data = b"z" * 256 + b"tail"
        name = "saved.bin"; encoded = name.encode().hex()
        crc = binascii.crc32(data)
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "export.bin"; target.write_bytes(b"keep")
            backend.export_file(name, target)
            owned = backend._download_session.partial_path
            backend._handle_line(f"DOWNLOAD READYHEX {encoded} {len(data)} {crc:08X} 256")
            backend._handle_line(f"DOWNLOAD DATA 0 {(b'x' * 256).hex()}")
            backend._handle_line(f"DOWNLOAD DATA 1 {data[256:].hex()}")
            backend._handle_line(f"DOWNLOAD COMPLETEHEX {encoded} {len(data)} {crc:08X}")
            self.assertEqual(target.read_bytes(), b"keep")
            self.assertFalse(owned.exists())

    def test_large_upload_retry_keeps_same_chunk_and_offset(self):
        backend, commands = self.backend()
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "retry.bin"; source.write_bytes(bytes(range(256)) * 2 + b"x")
            backend.import_file(source)
            metadata = commands[0].split()[2:5]
            backend._handle_line("UPLOAD READY " + " ".join(metadata) + " 256")
            original = commands[-1]
            self.assertEqual(len(bytes.fromhex(original.split()[3])), 256)
            backend._host_transfer_timer.timeout.emit()
            self.assertEqual(backend._write_queue.get_nowait(), original)
            self.assertEqual(backend._upload_session.acknowledged_bytes, 0)
            backend._handle_line("UPLOAD ACK 0 256")
            self.assertEqual(backend._upload_session.acknowledged_bytes, 256)
            pending = commands[-1]
            backend._handle_line("UPLOAD ACK 0 256")
            self.assertEqual(commands[-1], pending)
            self.assertEqual(backend._upload_session.acknowledged_bytes, 256)

    def test_large_download_retry_and_duplicate_do_not_append_bytes_twice(self):
        backend, commands = self.backend()
        data = bytes(range(256)) + b"tail"
        name = "retry.bin"; encoded = name.encode().hex(); crc = binascii.crc32(data)
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "result.bin"
            backend.export_file(name, target)
            backend._handle_line(f"DOWNLOAD READYHEX {encoded} {len(data)} {crc:08X} 256")
            self.assertEqual(commands[-1], "DOWNLOAD DATA 0")
            backend._host_transfer_timer.timeout.emit()
            self.assertEqual(backend._write_queue.get_nowait(), "DOWNLOAD DATA 0")
            backend._handle_line(f"DOWNLOAD DATA 0 {data[:256].hex()}")
            self.assertEqual(commands[-1], "DOWNLOAD DATA 1")
            backend._handle_line(f"DOWNLOAD DATA 0 {data[:256].hex()}")
            self.assertEqual(backend._download_session.received_bytes, 256)
            backend._handle_line(f"DOWNLOAD DATA 1 {data[256:].hex()}")
            backend._handle_line(f"DOWNLOAD COMPLETEHEX {encoded} {len(data)} {crc:08X}")
            self.assertEqual(target.read_bytes(), data)

import os
import queue
import threading
import unittest
from types import SimpleNamespace

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtWidgets import QApplication

from rf3_gui.app.state_model import ConnectionState, DeviceInfo, RadioState
from rf3_gui.device.serial_device import (
    SERIAL_MAX_RESPONSE_BYTES,
    SerialDeviceBackend,
    _LineBuffer,
    _is_windows_incoming_bluetooth_port,
)


class TestConnectionSafety(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def setUp(self):
        self.backend = SerialDeviceBackend()
        self.backend._serial = SimpleNamespace(close=lambda: None)
        self.backend._snapshot = self.backend._replace_snapshot(
            connection_state=ConnectionState.CONNECTING
        )

    def tearDown(self):
        self.backend.disconnect_from_device()

    def test_reader_accumulates_timeout_fragments_before_handshake(self):
        backend = self.backend
        stop = threading.Event()
        responses = iter([b'{"state":"Stand', b'', b'by","channel":76}\r\n'])

        class FragmentedSerial:
            def readline(self, size=-1):
                try:
                    return next(responses)
                except StopIteration:
                    stop.set()
                    return b""

        backend._read_loop(FragmentedSerial(), stop, backend._connection_generation)
        self.assertTrue(backend._handshake_ok)
        self.assertEqual(backend._snapshot.confirmed_radio_state, RadioState.STANDBY)

    def test_oversize_line_is_discarded_until_newline_and_recovers(self):
        buffer = _LineBuffer()
        self.assertEqual(buffer.feed(b"x" * (SERIAL_MAX_RESPONSE_BYTES + 1)), [])
        self.assertEqual(buffer.feed(b"STATUS\nvalid\r\n"), ["valid"])
        self.assertEqual(buffer.pending, b"")

    def test_empty_unknown_and_invalid_status_do_not_pass_handshake(self):
        for line in ('{}', '{"state":"banana"}', '{"state":"Fault","fault":[]}'):
            self.backend._handle_line(line)
            self.assertFalse(self.backend._handshake_ok)
            self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.CONNECTING)

    def test_late_response_after_disconnect_cannot_restore_connection(self):
        old_generation = self.backend._connection_generation
        self.backend.disconnect_from_device()
        self.backend._on_worker_event(old_generation, "line", '{"state":"Standby"}')
        self.backend._handle_line('{"state":"Standby"}')
        self.assertIsNone(self.backend._serial)
        self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.DISCONNECTED)

    def test_late_error_cannot_close_new_connection(self):
        old_generation = self.backend._connection_generation
        self.backend.disconnect_from_device()
        new_port = SimpleNamespace(close=lambda: self.fail("Old error closed new port"))
        self.backend._serial = new_port
        self.backend._on_worker_event(old_generation, "read_error", "Old connection lost")
        self.backend._on_worker_event(old_generation, "write_error", ("STATUS", "Old write error"))
        self.assertIs(self.backend._serial, new_port)
        self.backend._serial = None

    def test_cancelled_open_closes_late_port(self):
        old_generation = self.backend._connection_generation
        self.backend.disconnect_from_device()
        closed = []
        port = SimpleNamespace(close=lambda: closed.append(True))
        self.backend._on_worker_event(old_generation, "opened", (DeviceInfo("ESP", "COM9", "serial"), port))
        self.assertEqual(closed, [True])
        self.assertIsNone(self.backend._serial)

    def test_old_writer_uses_its_own_queue_and_never_new_port(self):
        old_queue = queue.Queue()
        old_queue.put("STATUS")
        stop = threading.Event()
        written = []

        def write(payload):
            written.append(payload)
            stop.set()

        old_port = SimpleNamespace(write=write, flush=lambda: None)
        self.backend._serial = SimpleNamespace(write=lambda _: self.fail("Used new port"), close=lambda: None)
        self.backend._write_loop(old_port, stop, old_queue, self.backend._connection_generation - 1)
        self.assertEqual(written, [b"STATUS\r\n"])

    def test_localmfg_with_remote_address_is_not_hidden_as_incoming(self):
        outgoing = r"BTHENUM\SPP_LOCALMFG&0000\7&ABC&0&AABBCCDDEEFF_C00000000"
        incoming = r"BTHENUM\SPP_LOCALMFG&0000\7&ABC&0&000000000000_00000005"
        self.assertFalse(_is_windows_incoming_bluetooth_port("Bluetooth link", outgoing))
        self.assertTrue(_is_windows_incoming_bluetooth_port("Bluetooth link", incoming))

    def test_status_poll_is_bounded_and_silence_closes_connection(self):
        backend = self.backend
        backend._handle_line('{"state":"Standby"}')
        for _ in range(10):
            backend.request_status()
        self.assertEqual(list(backend._pending_commands), ["STATUS"])
        self.assertEqual(backend._write_queue.qsize(), 1)
        self.assertTrue(backend._status_response_timer.isActive())
        backend._status_response_timer.timeout.emit()
        self.assertIsNone(backend._serial)
        self.assertEqual(backend._snapshot.connection_state, ConnectionState.ERROR)

    def test_valid_status_cancels_response_watchdog(self):
        self.backend.request_status()
        self.backend._handle_line('{"state":"Fault","fault":1}')
        self.assertFalse(self.backend._status_response_timer.isActive())

    def test_connect_success_observes_confirmed_state_and_capabilities(self):
        reported = []
        self.backend.command_finished.connect(
            lambda command, ok, message: reported.append(self.backend._snapshot)
            if command == "CONNECT" and ok else None
        )
        self.backend._handle_line('{"state":"Fault","host_upload":"enabled","host_retry":"enabled"}')
        self.assertEqual(reported[0].confirmed_radio_state, RadioState.FAULT)
        self.assertTrue(reported[0].capabilities.supports_host_file_upload)
        self.assertTrue(reported[0].capabilities.supports_host_chunk_retry)

    def test_ui_allows_cancelling_connection_attempt(self):
        from rf3_gui.app.controller import AppController
        from rf3_gui.ui.main_window import MainWindow
        window = MainWindow(AppController(self.backend))
        window._apply_snapshot(self.backend._snapshot)
        self.assertTrue(window.disconnect_button.isEnabled())
        self.assertFalse(window.connect_button.isEnabled())
        window.close()

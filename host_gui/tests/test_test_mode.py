import unittest
import os
import sys
import time
from types import SimpleNamespace
from unittest.mock import patch

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtWidgets import QApplication

from rf3_gui.app.controller import AppController
from rf3_gui.app.state_model import DeviceInfo
from rf3_gui.app.state_model import ConnectionState
from rf3_gui.app.state_model import RadioState
from rf3_gui.app.state_model import TransferState
from rf3_gui.device.serial_device import SerialDeviceBackend
from rf3_gui.protocol.console_protocol import (
    cw_loop_command,
    cw_start_command,
    morse_command,
    remote_command,
    text_command,
    test_mode_command,
    test_tx_command,
    echo_command,
)
from rf3_gui.ui.main_window import MainWindow


class TestDevboardMode(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def _backend(self):
        backend = SerialDeviceBackend()
        backend._serial = SimpleNamespace(close=lambda: None)
        return backend

    def test_commands_are_explicit(self):
        self.assertEqual(test_mode_command(True), "TESTMODE ON")
        self.assertEqual(test_mode_command(False), "TESTMODE OFF")
        self.assertEqual(test_tx_command(64), "TESTTX 64")
        self.assertEqual(echo_command("hello ESP"), "ECHO hello ESP")
        self.assertEqual(morse_command("SOS"), "MORSE SOS")
        self.assertEqual(remote_command("RX"), "REMOTE RX")
        self.assertEqual(text_command("hello there"), "TEXT hello there")
        self.assertEqual(cw_start_command(76, 3), "CW START 76 3")
        self.assertEqual(cw_loop_command(500, 250, 76, 3), "CW LOOP 500 250 76 3")

    def test_usb_status_confirms_simulated_tx_separately(self):
        backend = self._backend()
        progress = []
        backend.transfer_changed.connect(progress.append)
        backend._awaiting_test_tx_count = 1

        backend._handle_line(
            "profile=esp32-devboard test_mode=on State=Fault fault=1 "
            "tx_ok=false tx_state=Idle test_tx_count=1 test_tx_bytes=64 "
            "test_tx_state=Completed"
        )

        self.assertTrue(backend._snapshot.test_mode)
        self.assertEqual(backend._snapshot.test_tx_count, 1)
        self.assertFalse(backend._snapshot.telemetry.last_tx_ok)
        self.assertEqual(progress[-1].state, TransferState.COMPLETED)
        self.assertIn("no nRF24 TX", progress[-1].message)

    def test_old_firmware_does_not_claim_test_mode(self):
        backend = self._backend()
        backend._handle_line("profile=esp32-devboard State=Fault fault=1 tx_state=Idle")
        self.assertFalse(backend._snapshot.test_mode)

    def test_bluetooth_json_confirms_simulated_tx(self):
        backend = self._backend()
        progress = []
        backend.transfer_changed.connect(progress.append)
        backend._awaiting_test_tx_count = 1

        backend._handle_line(
            '{"state":"Fault","test_mode":true,"test_tx_count":1,'
            '"test_tx_bytes":128,"test_tx_state":"Completed","tx_ok":false}'
        )

        self.assertTrue(backend._snapshot.test_mode)
        self.assertEqual(backend._snapshot.test_tx_bytes, 128)
        self.assertEqual(progress[-1].state, TransferState.COMPLETED)

    def test_missing_status_fails_simulated_tx(self):
        backend = self._backend()
        progress = []
        backend.transfer_changed.connect(progress.append)
        backend._awaiting_test_tx_count = 1

        backend._on_test_tx_timeout()

        self.assertIsNone(backend._awaiting_test_tx_count)
        self.assertEqual(progress[-1].state, TransferState.FAILED)

    def test_status_reports_remote_capability_and_rx_path(self):
        backend = self._backend()
        backend._handle_line(
            "profile=esp32-devboard State=Standby channel=76 remote=enabled text=enabled "
            "rx_saved=1 rx_saved_bytes=42 rx_last=rx_1234ABCD.bin "
            "rx_last_path=/spiffs/rx_1234ABCD.bin"
        )

        self.assertTrue(backend._snapshot.capabilities.supports_remote_commands)
        self.assertTrue(backend._snapshot.capabilities.supports_text_messages)
        self.assertEqual(backend._snapshot.telemetry.rx_saved, 1)
        self.assertEqual(backend._snapshot.telemetry.last_rx_file, "rx_1234ABCD.bin")
        self.assertEqual(backend._snapshot.telemetry.last_rx_path, "/spiffs/rx_1234ABCD.bin")

    def test_live_text_events_are_decoded_from_console_lines(self):
        backend = self._backend()
        messages = []
        accepted = []
        backend.text_message_received.connect(lambda direction, text: messages.append((direction, text)))
        backend.command_finished.connect(lambda command, ok, _message: accepted.append((command, ok)))

        backend._pending_commands.append(text_command("hello peer"))
        backend._handle_line("TEXT TX: hello peer")
        backend._handle_line("TEXT RX: message received")

        self.assertEqual(messages, [("tx", "hello peer"), ("rx", "message received")])
        self.assertIn((text_command("hello peer"), True), accepted)

    def test_morse_and_cw_responses_mark_backend_busy(self):
        backend = self._backend()
        accepted = []
        backend.command_finished.connect(lambda command, ok, _message: accepted.append((command, ok)))

        backend._pending_commands.append(morse_command("SOS"))
        backend._handle_line("Morse started on channel 76 at dot=120 ms. Use STOP to abort.")

        self.assertEqual(accepted[-1], (morse_command("SOS"), True))
        self.assertEqual(backend._snapshot.connection_state, ConnectionState.BUSY)
        self.assertEqual(backend._snapshot.confirmed_radio_state, RadioState.TX_BUSY)

        backend._pending_commands.append(cw_start_command(80, 2))
        backend._handle_line("CW mode active on channel 80 at power level 2. Use STOP to abort.")

        self.assertEqual(accepted[-1], (cw_start_command(80, 2), True))
        self.assertEqual(backend._snapshot.confirmed_radio_state, RadioState.CW_TEST)
        self.assertEqual(backend._snapshot.channel, 80)

    def test_window_shows_confirmed_mode_and_disables_rf_controls(self):
        backend = self._backend()
        controller = AppController(backend)
        controller._selected_device = DeviceInfo("ESP32", "COM3", "serial")
        window = MainWindow(controller)

        backend._handle_line(
            "profile=esp32-devboard test_mode=on State=Fault fault=1 "
            "test_tx_count=2 test_tx_bytes=64"
        )
        self.assertTrue(window.test_mode_checkbox.isChecked())
        self.assertFalse(window.rx_button.isEnabled())
        self.assertIn("2 acknowledged", window.test_tx_value.text())

        backend._handle_line("profile=esp32-devboard test_mode=off State=Fault fault=1")
        self.assertFalse(window.test_mode_checkbox.isChecked())
        self.assertFalse(window.rx_button.isEnabled())
        self.assertTrue(window.apply_channel_button.isEnabled())  # Explicit reinitialization remains available.
        window.close()

    def test_window_appends_live_peer_text(self):
        backend = self._backend()
        controller = AppController(backend)
        window = MainWindow(controller)

        backend.text_message_received.emit("rx", "decoded over the air")

        self.assertIn("PEER  decoded over the air", window.live_text_output.toPlainText())
        window.close()

    def _echo_backend(self):
        backend = self._backend()
        self.addCleanup(backend.disconnect_from_device)
        backend._handle_line('{"state":"Fault","test_mode":true,"host_echo":"enabled","text":"enabled"}')
        return backend

    def test_test_mode_text_requires_matching_esp_echo_not_generic_acceptance(self):
        backend = self._echo_backend()
        messages = []
        backend.text_message_received.connect(lambda direction, text: messages.append((direction, text)))
        text = "Hello  ESP! " + "x" * 142
        self.assertEqual(len(text), 154)
        backend.send_text_message(text)
        self.assertEqual(backend._write_queue.get_nowait(), "ECHO " + text)
        self.assertEqual(backend._snapshot.connection_state, ConnectionState.BUSY)
        backend._handle_line("OK ECHO " + text)
        backend._handle_line('{"accepted":true,"status":{"state":"Fault","test_mode":true,"host_echo":"enabled"}}')
        self.assertEqual(messages, [])
        self.assertTrue(backend._text_echo_timer.isActive())
        self.assertEqual(backend._snapshot.connection_state, ConnectionState.BUSY)
        backend._handle_line("ECHO TEXT: " + text)
        self.assertEqual(messages, [("test_tx", text), ("echo", text)])
        self.assertEqual(backend._snapshot.connection_state, ConnectionState.CONNECTED)
        self.assertEqual(backend._snapshot.confirmed_radio_state, RadioState.FAULT)
        self.assertFalse(backend._text_echo_timer.isActive())
        self.assertTrue(backend._status_poll_timer.isActive())

    def test_echo_failure_does_not_fall_back_to_rf_text(self):
        backend = self._echo_backend()
        messages = []
        backend.text_message_received.connect(lambda *args: messages.append(args))
        backend.send_text_message("hello")
        self.assertEqual(backend._write_queue.get_nowait(), "ECHO hello")
        backend._handle_line("ECHO ERROR requires_devboard_test_mode")
        self.assertEqual(messages, [])
        self.assertFalse(backend._text_echo_timer.isActive())
        self.assertIsNone(backend._awaiting_echo)
        self.assertTrue(backend._write_queue.empty())

    def test_echo_preserves_prompt_like_message_content(self):
        backend = self._echo_backend()
        messages = []
        backend.text_message_received.connect(lambda *args: messages.append(args))
        backend.send_text_message("hello rf24> ESP")
        backend._handle_line("rf24> ECHO TEXT: hello rf24> ESP")
        self.assertEqual(messages, [("test_tx", "hello rf24> ESP"), ("echo", "hello rf24> ESP")])

    def test_mismatched_or_missing_echo_closes_connection_and_rejects_late_response(self):
        for failure in ("mismatch", "timeout", "disconnect"):
            with self.subTest(failure=failure):
                backend = self._echo_backend()
                messages = []
                backend.text_message_received.connect(lambda *args: messages.append(args))
                backend.send_text_message("hello")
                generation = backend._connection_generation
                if failure == "mismatch":
                    backend._handle_line("ECHO TEXT: wrong text")
                elif failure == "timeout":
                    backend._text_echo_timer.timeout.emit()
                else:
                    backend.disconnect_from_device()
                backend._on_worker_event(generation, "line", "ECHO TEXT: hello")
                self.assertEqual(messages, [])
                self.assertIsNone(backend._serial)
                self.assertIsNone(backend._awaiting_echo)
                self.assertFalse(backend._text_echo_timer.isActive())

    def test_echo_rejects_invalid_input_old_firmware_and_overlapping_requests(self):
        backend = self._echo_backend()
        for text in ("", "x" * 155, "hello\nTX", "\N{SNOWMAN}"):
            backend.send_text_message(text)
        self.assertTrue(backend._write_queue.empty())
        backend._handle_line('{"state":"Fault","test_mode":true,"text":"enabled"}')
        backend.send_text_message("hello")
        self.assertTrue(backend._write_queue.empty())
        backend._handle_line('{"state":"Fault","test_mode":true,"host_echo":"enabled"}')
        backend.send_text_message("hello")
        backend.send_text_message("second")
        backend.set_test_mode(False)
        self.assertEqual(backend._write_queue.qsize(), 1)
        self.assertEqual(backend._write_queue.get_nowait(), "ECHO hello")
        self.assertTrue(backend._snapshot.test_mode)

    def test_ui_explains_echo_and_enables_it_with_faulted_radio(self):
        backend = self._backend()
        self.addCleanup(backend.disconnect_from_device)
        controller = AppController(backend)
        controller._selected_device = DeviceInfo("ESP32", "COM9", "serial")
        window = MainWindow(controller)
        self.addCleanup(window.close)
        backend._handle_line('{"state":"Fault","test_mode":true,"text":"enabled"}')
        self.assertFalse(window.send_text_button.isEnabled())
        self.assertIn("Flash", window.live_text_help.text())
        backend._handle_line('{"state":"Fault","test_mode":true,"host_echo":"enabled","text":"enabled"}')
        self.assertTrue(window.send_text_button.isEnabled())
        self.assertFalse(window.rx_button.isEnabled())
        self.assertFalse(window.cw_stop_button.isEnabled())
        self.assertEqual(window.send_text_button.text(), "Send to ESP")
        window.live_text_edit.setText("hello ESP")
        window.send_text_button.click()
        self.assertFalse(window.send_text_button.isEnabled())
        backend._handle_line("ECHO TEXT: hello ESP")
        self.assertIn("YOU (TEST)", window.live_text_output.toPlainText())
        self.assertIn("ESP ECHO", window.live_text_output.toPlainText())
        self.assertNotIn("PEER", window.live_text_output.toPlainText())
        self.assertEqual(window.live_text_edit.text(), "")
        backend._handle_line('{"state":"Standby","test_mode":false,"text":"enabled"}')
        self.assertEqual(window.send_text_button.text(), "Send to Peer")
        self.assertIn("nRF24", window.live_text_help.text())
        backend.send_text_message("hello peer")
        self.assertEqual(backend._write_queue.get_nowait(), "ECHO hello ESP")
        self.assertEqual(backend._write_queue.get_nowait(), "TEXT hello peer")

    def test_reconnect_uses_fresh_threads_and_write_queue(self):
        class FakeSerial:
            def __init__(self, *args, **kwargs):
                self.port = None
                self.dtr = False
                self.rts = False
                self.closed = False
                self.writes = []

            def open(self):
                pass

            def close(self):
                self.closed = True

            def reset_input_buffer(self):
                pass

            def reset_output_buffer(self):
                pass

            def write(self, payload):
                self.writes.append(payload)
                return len(payload)

            def flush(self):
                pass

            in_waiting = 0
            def read(self, size=1):
                time.sleep(0.005)
                return b""

        created = []

        def make_serial(*args, **kwargs):
            instance = FakeSerial(*args, **kwargs)
            created.append(instance)
            return instance

        backend = SerialDeviceBackend()
        device = DeviceInfo("USB serial", "COM9", "serial")
        fake_module = SimpleNamespace(Serial=make_serial)

        # Simulate a leftover command from an earlier connection. A new
        # connection must not transmit it.
        backend._write_queue.put("STALE")
        with patch.dict(sys.modules, {"serial": fake_module}):
            backend._connect_worker(device, backend._connection_generation)
            backend._write_queue.put("STATUS")
            deadline = time.monotonic() + 1.0
            while not created[0].writes and time.monotonic() < deadline:
                time.sleep(0.01)
            backend._close_serial_port()

            backend._connect_worker(device, backend._connection_generation)
            backend._write_queue.put("STATUS")
            deadline = time.monotonic() + 1.0
            while not created[1].writes and time.monotonic() < deadline:
                time.sleep(0.01)
            backend._close_serial_port()

        self.assertEqual(created[0].writes, [b"STATUS\r\n"])
        self.assertEqual(created[1].writes, [b"STATUS\r\n"])
        self.assertTrue(created[0].closed)
        self.assertTrue(created[1].closed)


if __name__ == "__main__":
    unittest.main()

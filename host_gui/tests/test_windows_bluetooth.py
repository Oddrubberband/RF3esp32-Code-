"""Hardware-free GAP/SSP integration tests; never load native Windows APIs."""
import os
import binascii
import hashlib
import queue
import tempfile
import threading
import ctypes
import time
import uuid
import unittest
from contextlib import contextmanager
from datetime import datetime, timezone
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
from PySide6.QtWidgets import QApplication

from rf3_gui.app.controller import AppController
from rf3_gui.app.state_model import ConnectionState, DeviceInfo, DeviceSnapshot
from rf3_gui.device.serial_device import SerialDeviceBackend, _write_timeout_for
from rf3_gui.device.windows_bluetooth import (
    BluetoothCancelled, BluetoothError, BluetoothPeer, E_INVALIDARG,
    WindowsBluetooth, _DeviceInfo, _SystemTime, compact_address, matching_spp_ports,
    _NativeApi, _SearchParams, SPP_SERVICE_UUID,
)
from rf3_gui.ui.main_window import MainWindow

ADDRESS = "12:34:56:78:9A:BC"


def record(*, paired=False, fresh=True, name="ESP test"):
    value = _DeviceInfo()
    value.address = int(compact_address(ADDRESS), 16)
    value.name = name
    value.authenticated = paired
    now = datetime.now(timezone.utc) if fresh else datetime(2020, 1, 1, tzinfo=timezone.utc)
    value.last_seen = _SystemTime(now.year, now.month, 0, now.day, now.hour, now.minute, now.second, 0)
    return value


def port(name="COM9", address="123456789ABC", *, incoming=False):
    return SimpleNamespace(device=name, description="Bluetooth SPP",
                           hwid="BTHENUM\\SPP\\7&TEST&0&" + ("000000000000" if incoming else address) + "_C00000000")


class FakeApi:
    def __init__(self, *, paired=False):
        self.device = record(paired=paired)
        self.calls = []
        self.pair_error = 0
        self.service_error = 0
        self.confirm_pairing = True
        self.cancel_during_pair = None

    @contextmanager
    def radio(self):
        self.calls.append("radio")
        try:
            yield None
        finally:
            self.calls.append("closed")

    def inquire(self, radio):
        self.calls.append("inquiry")
        return [self.device]

    def info(self, radio, address):
        self.calls.append(("info", address))
        return self.device

    def pair(self, radio, device, parent):
        self.calls.append(("pair", parent))
        if self.confirm_pairing and not self.pair_error:
            self.device.authenticated = True
        if self.cancel_during_pair:
            self.cancel_during_pair.set()
        return self.pair_error

    def enable_spp(self, radio, device):
        self.calls.append("service")
        return self.service_error


class TestWindowsBluetooth(unittest.TestCase):
    def adapter(self, api, ports=None, **kwargs):
        return WindowsBluetooth(api_factory=lambda: api,
                                port_provider=lambda: [port()] if ports is None else ports,
                                registry_lookup=lambda address: [], port_timeout=0, **kwargs)

    def test_fresh_scan_does_not_pair_or_enable_service(self):
        api = FakeApi()
        peers = self.adapter(api).scan(threading.Event())
        self.assertEqual(peers, [BluetoothPeer("ESP test", ADDRESS, False, True)])
        self.assertEqual(api.calls, ["radio", "inquiry", "closed"])

    def test_cached_record_is_not_claimed_as_live(self):
        api = FakeApi()
        api.device = record(fresh=False)
        self.assertFalse(self.adapter(api).scan(threading.Event())[0].seen_in_scan)

    def test_discovery_is_not_hardcoded_to_rf3_names(self):
        api = FakeApi()
        api.device.name = "My ESP serial device"
        self.assertEqual(self.adapter(api).scan(threading.Event())[0].name, "My ESP serial device")

    def test_pair_selected_device_then_resolve_its_port(self):
        api = FakeApi()
        messages = []
        chosen = self.adapter(api, [port("COM4", "AABBCCDDEEFF"), port()]).prepare_serial(
            ADDRESS, threading.Event(), parent_window=42, progress=messages.append)
        self.assertEqual(chosen, "COM9")
        self.assertIn(("pair", 42), api.calls)
        self.assertIn("service", api.calls)
        self.assertTrue(any("Pairing" in message for message in messages))

    def test_already_paired_does_not_request_pairing_again(self):
        api = FakeApi(paired=True)
        self.assertEqual(self.adapter(api).prepare_serial(ADDRESS, threading.Event()), "COM9")
        self.assertFalse(any(isinstance(call, tuple) and call[0] == "pair" for call in api.calls))

    def test_pairing_cancelled_does_not_enable_spp(self):
        api = FakeApi()
        api.pair_error = 1223
        with self.assertRaises(BluetoothCancelled):
            self.adapter(api).prepare_serial(ADDRESS, threading.Event())
        self.assertNotIn("service", api.calls)

    def test_pairing_error_is_reported(self):
        api = FakeApi()
        api.pair_error = 5
        with self.assertRaisesRegex(BluetoothError, "pairing failed"):
            self.adapter(api).prepare_serial(ADDRESS, threading.Event())

    def test_success_without_confirmed_authentication_is_rejected(self):
        api = FakeApi()
        api.confirm_pairing = False
        with self.assertRaisesRegex(BluetoothError, "did not confirm"):
            self.adapter(api).prepare_serial(ADDRESS, threading.Event())
        self.assertNotIn("service", api.calls)

    def test_cancel_during_native_pairing_prevents_later_connection(self):
        api = FakeApi()
        cancel = threading.Event()
        api.cancel_during_pair = cancel
        with self.assertRaises(BluetoothCancelled):
            self.adapter(api).prepare_serial(ADDRESS, cancel)
        self.assertNotIn("service", api.calls)

    def test_no_matching_port_does_not_pick_other_esp(self):
        api = FakeApi(paired=True)
        with self.assertRaisesRegex(BluetoothError, "no outgoing SPP"):
            self.adapter(api, [port("COM7", "AABBCCDDEEFF")]).prepare_serial(ADDRESS, threading.Event())

    def test_incoming_listener_is_never_used(self):
        self.assertEqual(matching_spp_ports(ADDRESS, [port(incoming=True)]), [])

    def test_registry_mapping_requires_existing_bluetooth_port(self):
        unknown = SimpleNamespace(device="COM12", description="Bluetooth link", hwid="BTHENUM\\SPP")
        usb = SimpleNamespace(device="COM13", description="USB UART", hwid="USB\\DEVICE")
        self.assertEqual(matching_spp_ports(ADDRESS, [unknown, usb], ["COM12", "COM13"]), ["COM12"])

    def test_multiple_matching_ports_are_ambiguous(self):
        with self.assertRaisesRegex(BluetoothError, "Multiple outgoing"):
            self.adapter(FakeApi(paired=True), [port("COM7"), port("COM9")]).prepare_serial(ADDRESS, threading.Event())

    def test_unsupported_spp_reports_ble_limitation(self):
        api = FakeApi(paired=True)
        api.service_error = 1060
        with self.assertRaisesRegex(BluetoothError, "BLE-only"):
            self.adapter(api).prepare_serial(ADDRESS, threading.Event())

    def test_already_enabled_service_still_requires_matching_port(self):
        api = FakeApi(paired=True)
        api.service_error = E_INVALIDARG
        self.assertEqual(self.adapter(api).prepare_serial(ADDRESS, threading.Event()), "COM9")

    def test_cancel_before_scan_does_not_touch_native_api(self):
        api = FakeApi()
        cancel = threading.Event()
        cancel.set()
        with self.assertRaises(BluetoothCancelled):
            self.adapter(api).scan(cancel)
        self.assertEqual(api.calls, [])

    def test_invalid_address_is_rejected_before_native_api(self):
        api = FakeApi()
        with self.assertRaises(BluetoothError):
            self.adapter(api).prepare_serial("not a MAC", threading.Event())
        self.assertEqual(api.calls, [])

    def test_port_creation_can_be_delayed(self):
        api = FakeApi(paired=True)
        batches = iter([[], [port()]])
        adapter = WindowsBluetooth(api_factory=lambda: api, port_provider=lambda: next(batches),
                                   registry_lookup=lambda address: [], port_timeout=1)
        self.assertEqual(adapter.prepare_serial(ADDRESS, threading.Event()), "COM9")

    def test_native_bindings_use_expected_structures_and_spp_guid_without_hardware(self):
        class Function:
            def __init__(self):
                self.calls = []
            def __call__(self, *arguments):
                self.calls.append(arguments)
                return 0
        class Library:
            def __getattr__(self, name):
                function = Function()
                setattr(self, name, function)
                return function
        library = Library()
        with patch("rf3_gui.device.windows_bluetooth.ct.WinDLL", return_value=library):
            api = _NativeApi()
            device = record()
            api.pair(None, device, 77)
            api.enable_spp(None, device)
        pairing = library.BluetoothAuthenticateDeviceEx.calls[0]
        self.assertEqual(pairing[0], 77)
        self.assertEqual(pairing[4], 4)
        service = library.BluetoothSetServiceState.calls[0]
        self.assertEqual(ctypes.string_at(service[2], 16), uuid.UUID(SPP_SERVICE_UUID).bytes_le)
        self.assertEqual(service[3], 1)
        self.assertEqual(ctypes.sizeof(_DeviceInfo), 560)
        self.assertEqual(ctypes.sizeof(_SearchParams), 40 if ctypes.sizeof(ctypes.c_void_p) == 8 else 32)


class TestBluetoothGuiIntegration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def setUp(self):
        self.adapter = SimpleNamespace(scan=lambda cancel: [BluetoothPeer("ESP", ADDRESS, False, False)])
        self.backend = SerialDeviceBackend(bluetooth_adapter=self.adapter)

    def tearDown(self):
        self.backend.disconnect_from_device()

    def test_gui_pair_connect_requires_status_over_selected_spp_port(self):
        # Protocol basis: Bluetooth SIG GAP/SSP + SPP, followed by RF3 STATUS.
        # Exercise actual GUI signals and worker threads with both native and
        # serial boundaries injected; this test must never use real hardware.
        api = FakeApi()
        self.backend._bluetooth = WindowsBluetooth(
            api_factory=lambda: api, port_provider=lambda: [port()],
            registry_lookup=lambda address: [], port_timeout=0,
        )
        controller = AppController(self.backend)
        window = MainWindow(controller)
        self.addCleanup(window.close)
        responses = queue.Queue()
        written = []
        closed = threading.Event()
        stored = bytearray()
        metadata = []

        class FakeSerial:
            def open(self):
                pass

            def close(self):
                closed.set()

            def reset_input_buffer(self):
                pass

            def reset_output_buffer(self):
                pass

            def write(self, payload):
                written.append(payload)
                parts = payload.decode("ascii").split()
                if parts[:2] == ["UPLOAD", "BEGIN"]:
                    metadata[:] = parts[2:]
                    stored.clear()
                    responses.put(f"UPLOAD READY {' '.join(metadata)}\r\n".encode())
                elif parts[:2] == ["UPLOAD", "DATA"]:
                    stored.extend(bytes.fromhex(parts[3]))
                    responses.put(f"UPLOAD ACK {parts[2]} {len(stored)}\r\n".encode())
                elif parts == ["UPLOAD", "END"]:
                    responses.put(f"UPLOAD COMPLETE {metadata[0]} {len(stored)} {binascii.crc32(stored):08X}\r\n".encode())
                elif parts[:2] == ["DOWNLOAD", "BEGIN"]:
                    responses.put(f"DOWNLOAD READY {metadata[0]} {len(stored)} {binascii.crc32(stored):08X}\r\n".encode())
                elif parts[:2] == ["DOWNLOAD", "DATA"]:
                    offset = int(parts[2]) * 64
                    responses.put(f"DOWNLOAD DATA {parts[2]} {stored[offset:offset + 64].hex()}\r\n".encode())
                elif parts == ["DOWNLOAD", "END"]:
                    responses.put(f"DOWNLOAD COMPLETE {metadata[0]} {len(stored)} {binascii.crc32(stored):08X}\r\n".encode())
                elif parts[0] == "ECHO":
                    responses.put(b"ECHO TEXT: " + payload[len(b"ECHO "):])
                return len(payload)

            def flush(self):
                pass

            def readline(self, size=-1):
                try:
                    return responses.get(timeout=0.01)
                except queue.Empty:
                    return b""

        def wait_for(predicate):
            deadline = time.monotonic() + 4
            while not predicate() and time.monotonic() < deadline:
                self.app.processEvents()
                closed.wait(0.01)
            self.assertTrue(predicate(), "GUI Bluetooth workflow did not reach expected state")

        serial_port = FakeSerial()
        with patch("serial.tools.list_ports.comports", return_value=[]), patch(
            "serial.Serial", return_value=serial_port
        ) as serial_constructor:
            window.bluetooth_scan_button.click()
            wait_for(lambda: controller.selected_device is not None)
            self.assertEqual(controller.selected_device.address, ADDRESS)
            self.assertEqual(window.connect_button.text(), "Pair && Connect")
            window.connect_button.click()
            wait_for(lambda: b"STATUS\r\n" in written)
            self.assertEqual(serial_port.port, "COM9")
            self.assertFalse(serial_port.dtr)
            self.assertFalse(serial_port.rts)
            self.assertEqual(serial_constructor.call_args.kwargs["write_timeout"], 8.0)
            self.assertTrue(any(isinstance(call, tuple) and call[0] == "pair" for call in api.calls))
            self.assertIn("service", api.calls)
            self.assertEqual(controller.snapshot.connection_state, ConnectionState.CONNECTING)

            responses.put(b"RF3 Bluetooth SPP connected\r\nOK STATUS\r\n")
            responses.put(b'{}\r\n{"state":"banana"}\r\n')
            wait_for(lambda: responses.empty())
            self.app.processEvents()
            self.assertEqual(controller.snapshot.connection_state, ConnectionState.CONNECTING)

            # A missing nRF24 is independent of a working PC-to-ESP host link.
            responses.put(b'{"node_name":"rf3-devboard","state":"Fault","fault":1,')
            responses.put(b'"host_upload":"enabled","host_download":"enabled"}\r\n')
            wait_for(lambda: controller.snapshot.connection_state == ConnectionState.CONNECTED)
            self.assertTrue(controller.snapshot.capabilities.supports_host_file_upload)
            self.assertTrue(controller.snapshot.capabilities.supports_host_file_download)

            with tempfile.TemporaryDirectory() as directory:
                payload = bytes(range(256)) + b"\x00\r\n\xff"
                source = Path(directory) / "validate.bin"
                source.write_bytes(payload)
                controller.set_selected_file(source)
                window.test_mode_checkbox.click()
                wait_for(lambda: b"TESTMODE ON\r\n" in written)
                responses.put(b'{"state":"Fault","fault":1,"test_mode":true,'
                              b'"host_upload":"enabled","host_download":"enabled","host_echo":"enabled"}\r\n')
                wait_for(lambda: window.transmit_button.text() == "Upload && Verify")
                self.assertFalse(window.rx_button.isEnabled())
                window.transmit_button.click()
                wait_for(lambda: "byte-for-byte match" in window.transfer_detail_label.text())
                self.assertEqual(bytes(stored), payload)
                self.assertIn(hashlib.sha256(payload).hexdigest(), window.transfer_detail_label.text())
                self.assertEqual(window.progress_bar.value(), 1000)
                self.assertFalse(any(command.startswith((b"TX ", b"TESTTX ")) for command in written))
                window.live_text_edit.setText("Hello  ESP! " + "x" * 142)
                window.send_text_button.click()
                wait_for(lambda: "ESP ECHO" in window.live_text_output.toPlainText())
                self.assertIn("YOU (TEST)", window.live_text_output.toPlainText())
                self.assertNotIn("PEER", window.live_text_output.toPlainText())
                self.assertFalse(any(command.startswith(b"TEXT ") for command in written))
            window.disconnect_button.click()
            wait_for(closed.is_set)
            self.assertEqual(controller.snapshot.connection_state, ConnectionState.DISCONNECTED)
        for worker in (self.backend._reader_thread, self.backend._writer_thread):
            if worker is not None:
                worker.join(timeout=1)

    def test_scan_includes_bluetooth_candidates_and_usb_ports_without_pairing(self):
        discovered = []
        self.backend.devices_discovered.connect(discovered.append)
        with patch("serial.tools.list_ports.comports", return_value=[]):
            self.backend._discover_worker(True)
        self.assertEqual(discovered[0][0].transport, "bluetooth")
        self.assertEqual(discovered[0][0].address, ADDRESS)
        self.assertFalse(discovered[0][0].seen_in_scan)

    def test_stale_discovery_result_after_cancel_is_ignored(self):
        found = []
        self.backend.devices_discovered.connect(found.append)
        old_generation = self.backend._discovery_generation
        self.backend.disconnect_from_device()
        self.backend._on_discovery_result(old_generation, [DeviceInfo("ESP", ADDRESS, "bluetooth")], "")
        self.assertEqual(found, [])

    def test_cancelled_setup_does_not_open_a_serial_port(self):
        def prepare(address, cancel, **kwargs):
            cancel.set()
            return "COM9"
        self.adapter.prepare_serial = prepare
        with patch("serial.Serial") as serial_constructor:
            self.backend._connect_worker(DeviceInfo("ESP", ADDRESS, "bluetooth"), self.backend._connection_generation)
            serial_constructor.assert_not_called()

    def test_old_connection_worker_does_not_attempt_pairing(self):
        self.adapter.prepare_serial = lambda *args, **kwargs: self.fail("Stale worker requested pairing")
        old_generation = self.backend._connection_generation
        self.backend.disconnect_from_device()
        self.backend._connect_worker(DeviceInfo("ESP", ADDRESS, "bluetooth"), old_generation)

    def test_prepared_connection_uses_existing_serial_backend(self):
        self.adapter.prepare_serial = lambda address, cancel, **kwargs: "COM9"
        opened = []
        self.backend._start_serial_io = lambda device, serial_port, generation: opened.append(device)
        fake_port = SimpleNamespace(open=lambda: None, close=lambda: None,
                                    reset_input_buffer=lambda: None, reset_output_buffer=lambda: None)
        with patch("serial.Serial", return_value=fake_port):
            self.backend._connect_worker(DeviceInfo("ESP", ADDRESS, "bluetooth"), self.backend._connection_generation)
        self.assertEqual(opened[0].address, "COM9")
        self.assertEqual(opened[0].bluetooth_address, ADDRESS)
        self.assertEqual(opened[0].transport, "serial")
        self.assertEqual(_write_timeout_for(opened[0]), 8.0)

    def test_ui_shows_pair_connect_and_distinguishes_cached_results(self):
        controller = AppController(self.backend)
        window = MainWindow(controller)
        controller._on_devices_discovered([DeviceInfo("ESP", ADDRESS, "bluetooth", paired=False)])
        self.assertEqual(window.connect_button.text(), "Pair && Connect")
        self.assertIn("cached", window.device_combo.itemText(0))
        self.assertTrue(window.bluetooth_scan_button.isEnabled())
        window.resize(1080, 720)
        window.show()
        self.app.processEvents()
        self.assertLessEqual(window.minimumSizeHint().width(), 1080)
        self.assertLessEqual(window.minimumSizeHint().height(), 720)
        window._apply_snapshot(DeviceSnapshot(connection_state=ConnectionState.SEARCHING))
        self.assertFalse(window.connect_button.isEnabled())
        self.assertFalse(window.bluetooth_scan_button.isEnabled())
        self.assertTrue(window.disconnect_button.isEnabled())
        window.close()

    def test_controller_rejects_scan_while_connected(self):
        controller = AppController(self.backend)
        controller._snapshot = DeviceSnapshot(connection_state=ConnectionState.CONNECTED)
        with patch.object(self.backend, "scan_bluetooth") as scan:
            controller.scan_bluetooth_devices()
            scan.assert_not_called()
        controller._on_devices_discovered([])
        self.assertEqual(controller.snapshot.connection_state, ConnectionState.CONNECTED)


if __name__ == "__main__":
    unittest.main()

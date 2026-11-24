import os
import unittest
from dataclasses import replace
from pathlib import Path
from unittest.mock import patch

os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
from PySide6.QtWidgets import QApplication
from rf3_gui.app.controller import AppController
from rf3_gui.app.state_model import ConnectionState,DeviceSnapshot,RadioState,RadioCapabilities,Telemetry
from rf3_gui.device.mock_device import MockDeviceBackend
from rf3_gui.ui.main_window import MainWindow


class TestReceiveUI(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def setUp(self):
        self.backend = MockDeviceBackend()
        self.backend._connected = True
        self.controller = AppController(self.backend)
        self.window = MainWindow(self.controller)
        self.addCleanup(self.window.close)
        self.snapshot = DeviceSnapshot(connection_state=ConnectionState.BUSY,
            confirmed_radio_state=RadioState.RX_LISTENING, channel=0, power_level=3,
            rx_transfer_id='06EBD65F', rx_transfer_state='Receiving',
            rx_received_bytes=13700, rx_total_bytes=941219,
            capabilities=RadioCapabilities(supports_host_storage=True, supports_stored_file_tx=True,
                supports_host_file_upload=True, supports_host_file_download=True))

    def emit(self, snapshot):
        self.backend._snapshot = snapshot
        self.backend.status_changed.emit(snapshot)

    def test_incoming_progress_is_separate_from_outgoing(self):
        self.window.transfer_detail_label.setText('Outgoing panel unchanged')
        self.emit(self.snapshot)
        self.assertIn('Receiving',self.window.rx_transfer_value.text())
        self.assertIn('13,700/941,219 bytes',self.window.rx_transfer_value.text())
        self.assertEqual(self.window.rx_progress.value(),14)
        self.assertEqual(self.window.transfer_detail_label.text(),'Outgoing panel unchanged')
        self.emit(replace(self.snapshot,rx_received_bytes=470610))
        self.assertEqual(self.window.rx_progress.value(),500)

    def test_active_receive_disables_disruptive_controls_but_allows_cancel(self):
        for state in ('Receiving','WaitingForEnd','Verifying','Publishing'):
            self.emit(replace(self.snapshot,rx_transfer_state=state))
            for button in (self.window.rx_button,self.window.apply_channel_button,
                self.window.apply_power_button,self.window.standby_button,self.window.sleep_button,
                self.window.wake_button,self.window.power_down_button,self.window.transmit_button,
                self.window.storage_import_button,self.window.storage_delete_button,
                self.window.cw_start_button,self.window.send_text_button):
                self.assertFalse(button.isEnabled(),button.text())
            self.assertTrue(self.window.stop_button.isEnabled())
            self.assertEqual(self.window.stop_button.text(),'Cancel Receive')
            with patch.object(self.backend,'stop') as stop:
                self.window.stop_button.click()
                stop.assert_called_once()

    def test_receive_terminal_and_disconnected_display(self):
        for state,error in (('Completed','none'),('Cancelled','cancelled'),('Failed','sink-write')):
            self.emit(replace(self.snapshot,connection_state=ConnectionState.CONNECTED,
                rx_transfer_state=state,rx_error=error,rx_received_bytes=941219))
            self.assertIn(state,self.window.rx_transfer_value.text())
            if error!='none':self.assertIn(error,self.window.rx_transfer_value.text())
            self.assertEqual(self.window.stop_button.text(),'Stop')
            self.assertTrue(self.window.apply_channel_button.isEnabled())
        self.emit(DeviceSnapshot())
        self.assertEqual(self.window.rx_transfer_value.text(),'Unavailable')
        self.assertEqual(self.window.rx_progress.value(),0)

    def test_rate_elapsed_and_status_bar_follow_received_bytes(self):
        with patch('rf3_gui.app.receive_rate.time.monotonic',return_value=10):
            self.emit(replace(self.snapshot,rx_received_bytes=0))
        with patch('rf3_gui.app.receive_rate.time.monotonic',return_value=14):
            self.emit(replace(self.snapshot,rx_received_bytes=4096))
        self.assertIn('1.00 KiB/s',self.window.rx_rate_value.text())
        self.assertIn('4.0 s observed',self.window.rx_rate_value.text())
        self.assertIn('Receiving 4,096/941,219 bytes',self.window.statusBar().currentMessage())
        with patch('rf3_gui.app.receive_rate.time.monotonic',return_value=20):
            self.emit(DeviceSnapshot())
        self.assertEqual(self.window.rx_rate_value.text(),'Unavailable')

    def test_received_pdf_export_defaults_to_received_extension(self):
        self.emit(replace(self.snapshot,connection_state=ConnectionState.CONNECTED,
            rx_transfer_state='Completed',rx_received_bytes=941219,
            telemetry=Telemetry(rx_saved=1,last_rx_file='rx_06EBD65F.PDF')))
        with patch('rf3_gui.ui.main_window.QFileDialog.getSaveFileName',
                return_value=('exported.PDF','')) as dialog, \
                patch.object(self.backend,'save_received_file') as save:
            self.window._save_received_file()
        self.assertEqual(dialog.call_args.args[2],'rx_06EBD65F.PDF')
        save.assert_called_once_with(Path('exported.PDF'))


if __name__=='__main__':unittest.main()

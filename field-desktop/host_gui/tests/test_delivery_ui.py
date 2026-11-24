import csv
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
from unittest import TestCase
from PySide6.QtWidgets import QApplication
from rf3_gui.app.controller import AppController
from rf3_gui.device.serial_device import SerialDeviceBackend
from rf3_gui.ui.metrics_page import MetricsPage

class DeliveryUI(TestCase):
    def test_serial_to_history_and_both_export_types(self):
        app=QApplication.instance() or QApplication([])
        device=SerialDeviceBackend()
        controller=AppController(device)
        page=MetricsPage(controller)
        device._serial=SimpleNamespace(close=lambda:None)
        device._handle_line("rf24> DELIVERY1 Devboard RX noack 00000102 00000003 8 8 8 7 -1 1 1 Completed")
        self.assertIn("8/8 delivered",page.delivery_label.text())
        self.assertIn("1 missing on first pass",page.delivery_label.text())
        device._handle_line("DELIVERY1 Devboard TX text 01020304 03030303 2 2 -1 -1 0 0 1 SentUnconfirmed")
        self.assertIn("Delivery rate unknown",page.delivery_label.text())
        device._handle_line("DELIVERY1 Devboard TX text 01020304 03030303 2 2 -1 -1 0 0 1 SentUnconfirmed")
        self.assertEqual(page.delivery_history.count(),3)
        page.delivery_history.setCurrentIndex(1)
        self.assertIn("8/8 delivered",page.delivery_label.text())
        with TemporaryDirectory() as temp:
            for direction, n in (("RX",1),("TX",1),(None,2)):
                path=Path(temp)/"report.csv"
                page.capture.save(direction,path)
                with path.open(newline="") as f:r=list(csv.DictReader(f))
                self.assertEqual(len(r),n)
                self.assertTrue(all(i["record_type"]=="summary" for i in r))
        page.close()
        device._serial=None


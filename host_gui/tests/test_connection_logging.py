import logging
import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from rf3_gui.logging_config import configure_logging
from rf3_gui.device.serial_device import SerialDeviceBackend


class TestConnectionLogging(unittest.TestCase):
    def test_saved_log_includes_handshake_but_omits_file_payloads(self):
        root = logging.getLogger()
        previous_handlers = set(root.handlers)
        previous_level = root.level
        try:
            with tempfile.TemporaryDirectory() as directory:
                with patch.dict(os.environ, {"RF3_LOG_DIR": directory}):
                    configure_logging()
                    SerialDeviceBackend._record_log("debug", "TX -> STATUS (handshake)")
                    SerialDeviceBackend._record_log("debug", 'RX <- {"rf3_protocol":1}')
                    SerialDeviceBackend._record_log("debug", "TX -> UPLOAD DATA 0 deadbeef")
                    SerialDeviceBackend._record_log("debug", "RX <- DOWNLOAD DATA 0 deadbeef")
                    SerialDeviceBackend._record_log("debug", "TX -> TEXT private message")
                for handler in set(root.handlers) - previous_handlers:
                    handler.flush()
                    handler.close()
                    root.removeHandler(handler)
                paths = list(Path(directory).glob("connection-*.log"))
                self.assertEqual(len(paths), 1)
                saved = paths[0].read_text(encoding="utf-8")
                self.assertIn("STATUS (handshake)", saved)
                self.assertIn('"rf3_protocol":1', saved)
                self.assertNotIn("deadbeef", saved)
                self.assertNotIn("private message", saved)
        finally:
            for handler in set(root.handlers) - previous_handlers:
                handler.close()
                root.removeHandler(handler)
            root.setLevel(previous_level)

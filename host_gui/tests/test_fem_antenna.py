import unittest
from unittest.mock import patch

import test_audit_backend_repairs as fixtures
from rf3_gui.app.controller import AppController
from rf3_gui.app.state_model import ConnectionState
from rf3_gui.ui.main_window import MainWindow


class TestFemAntenna(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        fixtures.TestAuditBackendRepairs.setUpClass.__func__(cls)
    setUp = fixtures.TestAuditBackendRepairs.setUp
    tearDown = fixtures.TestAuditBackendRepairs.tearDown
    status = fixtures.TestAuditBackendRepairs.status
    commands = fixtures.TestAuditBackendRepairs.commands

    def test_click_requires_standby_and_status_confirmation(self):
        controller = AppController(self.backend)
        window = MainWindow(controller)
        self.addCleanup(window.close)
        self.status(fem_antenna=1)
        self.assertFalse(window.fem_antenna_row.isHidden())
        self.assertIn("ANT1 (J1)", window.fem_antenna_label.text())
        self.assertEqual(window.cw_channel_spin.maximum(), 83)
        window.fem_antenna_button.click()
        self.assertEqual(self.commands(), ["FEM ANT 2", "STATUS"])
        self.backend._handle_line("OK FEM ANT 2")
        self.status(fem_antenna=1)
        self.assertFalse(window.fem_antenna_button.isEnabled())
        self.assertIn("FEM ANT 2", self.backend._pending_commands)
        self.assertIn("ANT1", window.fem_antenna_label.text())
        self.status(fem_antenna=2)
        self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.CONNECTED)
        self.assertIn("ANT2 (J5)", window.fem_antenna_label.text())
        self.assertNotIn("FEM ANT 2", self.backend._pending_commands)
        self.status(state="RxListening", fem_antenna=2)
        self.assertFalse(window.fem_antenna_button.isEnabled())
        with patch("rf3_gui.ui.main_window.QMessageBox.warning") as warning:
            self.backend.set_fem_antenna(1)
        warning.assert_called_once()
        self.assertEqual(self.commands(), [])
        self.backend.disconnect_from_device()
        self.assertTrue(window.fem_antenna_row.isHidden())

    def test_capability_and_channel_limits(self):
        for invalid in (None, True, 0, 3, "1"):
            self.status(fem_antenna=invalid)
            self.backend.set_fem_antenna(2)
            self.assertEqual(self.commands(), [])
            self.assertEqual(self.backend._snapshot.capabilities.max_channel, 125)
        self.status(fem_antenna=1)
        self.backend.set_channel(84)
        self.backend.start_cw(84, 0)
        self.backend.start_cw_loop(100, 100, 84, 0)
        self.assertEqual(self.commands(), [])
        self.backend.set_channel(83)
        self.assertEqual(self.commands(), ["CHANNEL 83"])

    def test_usb_rejection_releases_pending_command_and_keeps_existing_antenna(self):
        self.status(fem_antenna=1)
        self.backend.set_fem_antenna(2)
        self.backend._handle_line("FEM ERROR ANT 2")
        self.assertNotIn("FEM ANT 2", self.backend._pending_commands)
        self.status(fem_antenna=1)
        self.assertEqual(self.backend._snapshot.fem_antenna, 1)
        self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.CONNECTED)
        self.assertIn(("FEM ANT 2", False, "FEM ERROR ANT 2"), self.events)

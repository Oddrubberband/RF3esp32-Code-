import time
import unittest
from dataclasses import replace
from unittest.mock import patch

import test_audit_backend_repairs as fixtures
from rf3_gui.app.controller import AppController
from rf3_gui.app.state_model import ConnectionState
from rf3_gui.ui.main_window import MainWindow
from rf3_gui.ui.workbench import PAGE_NAMES


class TestFemLna(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        fixtures.TestAuditBackendRepairs.setUpClass.__func__(cls)

    setUp = fixtures.TestAuditBackendRepairs.setUp
    tearDown = fixtures.TestAuditBackendRepairs.tearDown
    status = fixtures.TestAuditBackendRepairs.status
    commands = fixtures.TestAuditBackendRepairs.commands

    def test_switch_waits_for_matching_readback_in_both_directions(self):
        controller = AppController(self.backend)
        window = MainWindow(controller)
        self.addCleanup(window.close)
        self.status(fem_lna="gain")
        self.assertFalse(window.fem_lna_row.isHidden())
        self.assertIn("Amplified", window.fem_lna_label.text())
        window.show()
        for index in range(window.pages.count()):
            window.navigation.setCurrentRow(index)
            self.assertEqual(window.fem_lna_button.isVisible(), PAGE_NAMES[index] == "Radio settings")
        window.navigation.setCurrentRow(PAGE_NAMES.index("Radio settings"))
        self.assertEqual(window.fem_lna_button.text(), "Bypass LNA")
        window.fem_lna_button.click()
        self.assertEqual(self.commands(), ["FEM LNA BYPASS", "STATUS"])
        self.backend._handle_line("FEM OK LNA BYPASS")
        self.backend._handle_line("OK FEM LNA BYPASS")
        self.status(fem_lna="gain")
        self.assertFalse(window.fem_lna_button.isEnabled())
        self.assertIn("FEM LNA BYPASS", self.backend._pending_commands)
        self.assertIn("Amplified", window.fem_lna_label.text())
        self.status(fem_lna="bypass")
        self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.CONNECTED)
        self.assertIn("Bypassed", window.fem_lna_label.text())
        self.assertEqual(window.fem_lna_button.text(), "Enable LNA")
        window.fem_lna_button.click()
        self.assertEqual(self.commands(), ["FEM LNA GAIN", "STATUS"])
        self.status(fem_lna="gain")
        self.assertNotIn("FEM LNA GAIN", self.backend._pending_commands)
        self.assertIn("Amplified", window.fem_lna_label.text())

    def test_unsupported_and_invalid_status_clear_control(self):
        controller = AppController(self.backend)
        window = MainWindow(controller)
        self.addCleanup(window.close)
        for invalid in (None, True, 0, "BYPASS", {}, []):
            self.status(fem_lna=invalid)
            self.assertIn("Unavailable", window.fem_lna_label.text())
            self.assertFalse(window.fem_lna_button.isEnabled())
            with patch("rf3_gui.ui.main_window.QMessageBox.warning"):
                self.backend.set_fem_lna("bypass")
            self.assertEqual(self.commands(), [])
        self.status(fem_lna="gain")
        self.status()
        self.assertIsNone(self.backend._snapshot.fem_lna)
        self.assertFalse(window.fem_lna_button.isEnabled())

    def test_active_radio_and_pending_action_reject_switch(self):
        for state in ("RxListening", "TxBusy", "CwTest"):
            self.status(state=state, fem_lna="gain")
            self.backend.set_fem_lna("bypass")
            self.assertEqual(self.commands(), [])
        self.status(fem_lna="gain")
        self.backend.set_fem_lna("bypass")
        self.commands()
        self.backend.set_fem_lna("gain")
        self.backend.enter_rx()
        self.assertEqual(self.commands(), [])

    def test_rejection_and_disconnect_release_selection(self):
        self.status(fem_lna="gain")
        self.backend.set_fem_lna("bypass")
        self.backend._handle_line("FEM ERROR requires_idle_standby")
        self.assertNotIn("FEM LNA BYPASS", self.backend._pending_commands)
        self.status(fem_lna="gain")
        self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.CONNECTED)
        self.assertIn(("FEM LNA BYPASS", False, "FEM ERROR requires_idle_standby"), self.events)
        self.backend.set_fem_lna("bypass")
        self.backend.disconnect_from_device()
        self.assertIsNone(self.backend._snapshot.fem_lna)
        self.assertFalse(self.backend._pending_commands)

    def test_power_cutoff_and_disabled_rail_reject_switch(self):
        controller = AppController(self.backend)
        window = MainWindow(controller)
        self.addCleanup(window.close)
        self.status(fem_lna="gain")
        telemetry = self.backend._snapshot.telemetry
        for power in ({"battery_cutoff": True}, {"nrf_rail": False}):
            self.backend._snapshot = replace(self.backend._snapshot,
                                            telemetry=replace(telemetry, **power))
            self.backend.status_changed.emit(self.backend._snapshot)
            self.assertFalse(window.fem_lna_button.isEnabled())
            with patch("rf3_gui.ui.main_window.QMessageBox.warning"):
                self.backend.set_fem_lna("bypass")
            self.assertEqual(self.commands(), [])

    def test_missing_confirmation_expires_command(self):
        self.status(fem_lna="gain")
        self.backend.set_fem_lna("bypass")
        self.backend._command_deadlines["FEM LNA BYPASS"] = time.monotonic() - 1
        self.backend._check_command_deadlines()
        self.assertIsNone(self.backend._serial)
        self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.ERROR)


if __name__ == "__main__":
    unittest.main()

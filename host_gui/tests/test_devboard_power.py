import json
import unittest
from unittest.mock import patch
from types import SimpleNamespace

from PySide6.QtWidgets import QApplication
from rf3_gui.app.controller import AppController
from rf3_gui.app.state_model import ConnectionState, DeviceInfo
from rf3_gui.device.serial_device import SerialDeviceBackend
from rf3_gui.ui.main_window import MainWindow


class DevboardPowerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def setUp(self):
        self.backend = SerialDeviceBackend()
        self.backend._serial = SimpleNamespace(close=lambda: None)
        self.addCleanup(self.backend.disconnect_from_device)

    def status(self, **changes):
        values = dict(state="Fault", test_mode=True, board_power="enabled",
                      sense_mv=1234, sense_raw=2000, sense_calibrated=True,
                      sense_saturated=False, power_good=True, nrf_rail=True)
        values.update(changes)
        self.backend._handle_line(json.dumps(values))

    def window(self):
        controller = AppController(self.backend)
        controller._selected_device = DeviceInfo("Devboard", "COM9", "serial")
        window = MainWindow(controller)
        self.addCleanup(window.close)
        return window

    def test_voltage_and_outputs_display_with_absent_radio_in_test_mode(self):
        window = self.window()
        self.status()
        self.assertEqual(window.sense_voltage_value.text(), "1.234 V (calibrated)")
        self.assertEqual(window.power_good_value.text(), "HIGH")
        self.assertEqual(window.nrf_rail_value.text(), "ON (HIGH)")
        self.assertTrue(window.nrf_rail_off_button.isEnabled())
        self.assertFalse(window.rx_button.isEnabled())
        self.assertIsNone(self.backend._snapshot.telemetry.battery_percent)

    def test_voltage_updates_even_when_radio_snapshot_is_unavailable(self):
        self.status()
        self.backend._identity_ok = True
        self.status(state="Unavailable", sense_mv=970)
        self.assertEqual(self.backend._snapshot.telemetry.sense_mv, 970)
        self.assertTrue(self.backend._snapshot.telemetry.power_good)

    def test_console_status_has_same_voltage_units_as_bluetooth_json(self):
        self.backend._handle_line("profile=esp32-devboard State=Fault test_mode=on "
            "board_power=enabled sense_mv=1234 sense_raw=2000 sense_calibrated=true "
            "sense_saturated=false power_good=true nrf_rail=false")
        telemetry = self.backend._snapshot.telemetry
        self.assertEqual(telemetry.sense_mv, 1234)
        self.assertFalse(telemetry.nrf_rail)
        self.assertTrue(telemetry.power_good)

    def test_rail_change_waits_for_status_and_samples_continue_when_off(self):
        self.status()
        self.backend.set_nrf_rail(False)
        self.assertEqual(self.backend._write_queue.get_nowait(), "NRFRAIL OFF")
        self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.BUSY)
        self.backend._handle_line("NRFRAIL OK OFF")
        self.assertTrue(self.backend._nrf_rail_timer.isActive())
        self.status(nrf_rail=False, sense_mv=870)
        self.assertFalse(self.backend._nrf_rail_timer.isActive())
        self.assertEqual(self.backend._snapshot.connection_state, ConnectionState.CONNECTED)
        self.assertEqual(self.backend._snapshot.telemetry.sense_mv, 870)
        self.assertTrue(self.backend._snapshot.telemetry.power_good)

    def test_rail_controls_reject_active_receive_and_older_firmware(self):
        self.status(state="RxListening", test_mode=False)
        self.backend.set_nrf_rail(False)
        self.assertTrue(self.backend._write_queue.empty())
        self.backend._handle_line("profile=esp32-devboard State=Standby")
        self.backend.set_nrf_rail(False)
        self.assertTrue(self.backend._write_queue.empty())
        self.assertFalse(self.backend._snapshot.capabilities.supports_board_power)
        self.assertIsNone(self.backend._snapshot.telemetry.sense_mv)

    def test_missing_and_failed_samples_clear_previous_reading(self):
        window = self.window()
        self.status()
        self.status(sense_mv=-1, sense_raw=-1)
        self.assertIsNone(self.backend._snapshot.telemetry.sense_mv)
        self.assertEqual(window.sense_voltage_value.text(), "unavailable")
        self.status(sense_mv=None)
        self.assertIsNone(self.backend._snapshot.telemetry.sense_mv)

    def test_invalid_adc_fields_are_rejected_without_corrupting_state(self):
        self.status()
        for key, value in (("sense_mv", True), ("sense_mv", "1.2"),
                           ("sense_raw", 4096), ("sense_mv", -2), ("nrf_rail", "maybe")):
            with self.subTest(key=key, value=value):
                self.status(**{key: value})
                self.assertEqual(self.backend._snapshot.telemetry.sense_mv, 1234)

    def test_nominal_reference_and_saturated_input_are_labeled(self):
        window = self.window()
        self.status(sense_mv=3200, sense_raw=4095, sense_calibrated=False, sense_saturated=True)
        self.assertIn("estimated", window.sense_voltage_value.text())
        self.assertIn("ADC saturated", window.sense_voltage_value.text())

    def test_off_rail_blocks_rf_controls_but_keeps_test_rail_controls(self):
        window = self.window()
        self.status(state="PowerDown", test_mode=False, nrf_rail=False)
        self.assertFalse(window.rx_button.isEnabled())
        self.assertTrue(window.nrf_rail_on_button.isEnabled())
        self.status(state="Fault", nrf_rail=False)
        self.assertTrue(window.nrf_rail_on_button.isEnabled())

    def test_timeout_disconnects_and_stops_confirmation_timer(self):
        self.status()
        self.backend.set_nrf_rail(False)
        self.backend._nrf_rail_timer.timeout.emit()
        self.assertIsNone(self.backend._serial)
        self.assertIsNone(self.backend._awaiting_nrf_rail)
        self.assertFalse(self.backend._nrf_rail_timer.isActive())

    def test_device_error_does_not_claim_new_output_state(self):
        self.status()
        self.backend.set_nrf_rail(False)
        self.backend._handle_line("NRFRAIL ERROR busy")
        self.assertTrue(self.backend._snapshot.telemetry.nrf_rail)
        self.assertFalse(self.backend._nrf_rail_timer.isActive())

    def gauge(self, **changes):
        fields=dict(battery_profile="soikon_6s_3300",battery_mv=23040,battery_percent=50,
                    battery_remaining_mah=1650,battery_age_ms=0,battery_state="fresh",
                    battery_cutoff=False,battery_persisted=False,sense_mv=2560)
        fields.update(changes)
        self.status(**fields)

    def test_estimated_percentage_pack_voltage_and_nominal_capacity(self):
        window=self.window();self.gauge()
        self.assertIn("~50%",window.battery_value.text())
        self.assertIn("1650 mAh",window.battery_value.text())
        self.assertIn("23.04 V",window.battery_voltage_value.text())
        self.assertTrue(self.backend._snapshot.capabilities.supports_battery_percent)

    def test_tx_hold_and_stale_readings_are_labeled(self):
        window=self.window();self.gauge(battery_state="held",battery_age_ms=4000)
        self.assertIn("Held",window.battery_status_value.text())
        self.assertEqual(self.backend._snapshot.telemetry.battery_percent,50)
        self.gauge(battery_state="stale",battery_age_ms=13000)
        self.assertIn("Stale",window.battery_status_value.text())
        self.assertIn("13.0 s",window.battery_status_value.text())

    def test_latched_cutoff_disables_rail_enable_and_reset_when_low(self):
        window=self.window();self.gauge(battery_cutoff=True,battery_persisted=True,
            battery_state="cutoff",power_good=False,nrf_rail=False,battery_mv=19800,
            battery_percent=0,battery_remaining_mah=0)
        self.assertIn("CUTOFF LATCHED",window.battery_status_value.text())
        self.assertFalse(window.nrf_rail_on_button.isEnabled())
        self.assertFalse(window.battery_reset_button.isEnabled())
        self.assertEqual(window.power_good_value.text(),"LOW")

    def test_recovery_requires_status_confirmation_and_leaves_nrf_off(self):
        window=self.window();self.gauge(battery_cutoff=True,battery_persisted=True,
            battery_state="cutoff",power_good=False,nrf_rail=False)
        self.assertTrue(window.battery_reset_button.isEnabled())
        window.battery_reset_button.click()
        self.assertEqual(self.backend._write_queue.get_nowait(),"BATTERY RESET")
        self.backend._handle_line("BATTERY OK RESET nRF_rail_remains_OFF")
        self.assertTrue(self.backend._awaiting_battery_reset)
        self.assertTrue(self.backend._battery_reset_timer.isActive())
        self.gauge(nrf_rail=False)
        self.assertFalse(self.backend._awaiting_battery_reset)
        self.assertFalse(self.backend._battery_reset_timer.isActive())
        self.assertTrue(window.nrf_rail_on_button.isEnabled())

    def test_custom_pcb_has_battery_reset_without_independent_rail_control(self):
        window = self.window()
        self.gauge(profile="custom-pcb", state="PowerDown", test_mode=False, nrf_rail=None,
                   battery_cutoff=True, battery_persisted=True, battery_state="cutoff", power_good=False)
        self.assertEqual(window.nrf_rail_value.text(), "not available")
        self.assertFalse(window.nrf_rail_on_button.isEnabled())
        self.assertFalse(window.nrf_rail_off_button.isEnabled())
        self.assertFalse(window.wake_button.isEnabled())
        with patch("rf3_gui.ui.main_window.QMessageBox.warning") as warning:
            self.backend.set_nrf_rail(True)
            warning.assert_called_once()
        self.assertTrue(self.backend._write_queue.empty())
        self.assertTrue(window.battery_reset_button.isEnabled())
        window.battery_reset_button.click()
        self.assertEqual(self.backend._write_queue.get_nowait(), "BATTERY RESET")
        self.gauge(profile="custom-pcb", state="Standby", test_mode=False, nrf_rail=None)
        self.assertFalse(self.backend._awaiting_battery_reset)
        self.assertTrue(window.wake_button.isEnabled())
        self.assertFalse(window.nrf_rail_on_button.isEnabled())

    def test_stale_voltage_does_not_allow_reset(self):
        self.gauge(battery_cutoff=True,battery_state="stale",battery_age_ms=20000)
        self.backend.reset_battery_cutoff()
        self.assertTrue(self.backend._write_queue.empty())

    def test_battery_reset_timeout_disconnects_and_clears_pending_operation(self):
        self.gauge(battery_cutoff=True,battery_state="cutoff",power_good=False,nrf_rail=False)
        self.backend.reset_battery_cutoff();self.backend._battery_reset_timer.timeout.emit()
        self.assertIsNone(self.backend._serial)
        self.assertFalse(self.backend._awaiting_battery_reset)
        self.assertIsNone(self.backend._snapshot.telemetry.battery_mv)

    def test_invalid_gauge_fields_do_not_corrupt_last_reading(self):
        self.gauge()
        for key,value in (("battery_percent",101),("battery_percent",True),
                          ("battery_mv","bad"),("battery_age_ms",-2),
                          ("battery_remaining_mah",3301),("battery_state","invalid")):
            with self.subTest(key=key,value=value):
                self.gauge(**{key:value})
                self.assertEqual(self.backend._snapshot.telemetry.battery_percent,50)

    def test_noisy_input_is_not_presented_as_clean_measurement(self):
        window=self.window();self.gauge(battery_state="sensor_error",sense_noisy=True)
        self.assertIn("noisy sample rejected",window.sense_voltage_value.text())
        self.assertIn("ADC unavailable/noisy",window.battery_status_value.text())

    def test_verified_unavailable_radio_can_report_cached_battery(self):
        self.gauge();self.backend._identity_ok=True
        self.gauge(state="Unavailable",battery_state="held",battery_age_ms=4000)
        self.assertEqual(self.backend._snapshot.telemetry.battery_percent,50)
        self.assertTrue(self.backend._snapshot.capabilities.supports_battery_percent)

    def test_stale_gauge_blocks_new_rf_but_standby_remains_available(self):
        window=self.window();self.gauge(state="Standby",test_mode=False,
            battery_state="stale",battery_age_ms=20000)
        self.assertFalse(window.rx_button.isEnabled())
        self.assertTrue(window.standby_button.isEnabled())
        with patch("rf3_gui.ui.main_window.QMessageBox.warning") as warning:
            self.backend.enter_rx()
            self.assertTrue(self.backend._write_queue.empty())
            warning.assert_called_once()
        self.gauge(state="Standby",test_mode=False)
        self.assertTrue(window.rx_button.isEnabled())
        self.backend.enter_rx();self.assertEqual(self.backend._write_queue.get_nowait(),"RX")


    def test_corrected_display_keeps_sensed_protection_and_firmware_charge(self):
        window=self.window()
        self.gauge(battery_estimated_mv=22940,battery_percent=46,battery_remaining_mah=1518)
        self.assertIn("22.94 V",window.battery_voltage_value.text())
        self.assertIn("23.040 V",window.battery_voltage_value.toolTip())
        self.assertIn("~46%",window.battery_value.text())
        self.assertIn("1518 mAh",window.battery_value.text())
        self.gauge()
        self.assertIsNone(self.backend._snapshot.telemetry.battery_estimated_mv)
        self.assertIn("23.04 V",window.battery_voltage_value.text())

    def test_invalid_optional_estimate_clears_without_discarding_status(self):
        for value in (True,"22940",-2,35001,None):
            with self.subTest(value=value):
                self.gauge(battery_estimated_mv=22940)
                self.gauge(battery_estimated_mv=value,battery_mv=23100)
                self.assertIsNone(self.backend._snapshot.telemetry.battery_estimated_mv)
                self.assertEqual(self.backend._snapshot.telemetry.battery_mv,23100)

    def test_cutoff_event_warns_once_and_does_not_invent_output_levels(self):
        window=self.window();self.gauge()
        event="BATTERY SHUTOFF turn_physical_power_boot_switch_OFF_connect_charged_replacement_source"
        self.backend._handle_line(event)
        t=self.backend._snapshot.telemetry
        self.assertTrue(t.battery_cutoff)
        self.assertEqual(t.battery_percent,0)
        self.assertEqual(t.battery_remaining_mah,0)
        self.assertTrue(t.power_good)
        self.assertTrue(t.nrf_rail)
        self.assertTrue(t.battery_shutdown_pending)
        self.assertIn("awaiting output confirmation",window.battery_status_value.text())
        self.assertNotIn("ERROR",window.battery_status_value.text())
        self.assertNotIn("failed",window.battery_status_value.text())
        notice=window._battery_cutoff_notice
        self.assertIsNotNone(notice)
        self.assertFalse(notice.isModal())
        self.assertIn("physical power switch OFF",notice.text())
        self.backend._handle_line(event)
        self.assertIs(window._battery_cutoff_notice,notice)
        self.backend.disconnect_from_device()
        self.assertTrue(window._battery_cutoff_notified)
        self.assertIs(window._battery_cutoff_notice,notice)
        self.assertTrue(notice.isVisible())

    def test_warning_rearms_only_after_confirmed_recovery(self):
        window=self.window()
        self.gauge(battery_cutoff=True,battery_state="cutoff")
        first=window._battery_cutoff_notice
        self.gauge(battery_cutoff=False,battery_state="startup")
        self.assertTrue(window._battery_cutoff_notified)
        self.gauge()
        self.assertFalse(window._battery_cutoff_notified)
        self.assertIsNone(window._battery_cutoff_notice)
        self.gauge(battery_cutoff=True,battery_state="cutoff")
        self.assertIsNot(window._battery_cutoff_notice,first)

    def test_gui_and_backend_use_sensed_20400_boundary(self):
        window=self.window()
        self.gauge(state="Standby",test_mode=False,battery_mv=20399,battery_estimated_mv=23000)
        self.assertFalse(window.rx_button.isEnabled())
        with patch("rf3_gui.ui.main_window.QMessageBox.warning"):
            self.backend.enter_rx()
        self.assertTrue(self.backend._write_queue.empty())
        self.gauge(state="Standby",test_mode=False,battery_mv=20400,battery_estimated_mv=20300)
        self.assertTrue(window.rx_button.isEnabled())
        self.backend.enter_rx()
        self.assertEqual(self.backend._write_queue.get_nowait(),"RX")

    def test_reset_uses_sensed_voltage_not_display_estimate(self):
        self.gauge(battery_cutoff=True,battery_state="cutoff",battery_mv=20999,battery_estimated_mv=25000)
        self.backend.reset_battery_cutoff()
        self.assertTrue(self.backend._write_queue.empty())
        self.gauge(battery_cutoff=True,battery_state="cutoff",battery_mv=21000,battery_estimated_mv=20900)
        self.backend.reset_battery_cutoff()
        self.assertEqual(self.backend._write_queue.get_nowait(),"BATTERY RESET")

    def test_new_sampling_states_are_labeled_and_block_rf(self):
        window=self.window()
        for state,expected in (("startup","10 s"),("averaging","4 s")):
            with self.subTest(state=state):
                self.gauge(state="Standby",test_mode=False,battery_state=state)
                self.assertIn(expected,window.battery_status_value.text())
                self.assertFalse(window.rx_button.isEnabled())
                with patch("rf3_gui.ui.main_window.QMessageBox.warning"):
                    self.backend.enter_rx()
                self.assertTrue(self.backend._write_queue.empty())

    def test_console_status_reports_separate_protection_and_estimated_voltage(self):
        self.backend._handle_line("profile=esp32-devboard State=Standby board_power=enabled "
            "battery_profile=soikon_6s_3300 battery_mv=23040 battery_estimated_mv=22940 "
            "battery_state=fresh battery_percent=46 battery_remaining_mah=1518 "
            "battery_age_ms=0 battery_cutoff=false battery_persisted=false")
        self.assertEqual(self.backend._snapshot.telemetry.battery_mv,23040)
        self.assertEqual(self.backend._snapshot.telemetry.battery_estimated_mv,22940)


    def test_post_event_status_confirms_outputs_and_reports_actual_failure(self):
        window=self.window();self.gauge()
        self.backend._handle_line("BATTERY SHUTOFF turn_physical_power_boot_switch_OFF_connect_charged_replacement_source")
        self.gauge(battery_cutoff=True,battery_state="cutoff",battery_persisted=True,
                   power_good=False,nrf_rail=False)
        self.assertFalse(self.backend._snapshot.telemetry.battery_shutdown_pending)
        self.assertNotIn("awaiting",window.battery_status_value.text())
        self.assertNotIn("ERROR",window.battery_status_value.text())
        self.gauge(battery_cutoff=True,battery_state="cutoff",battery_persisted=False,
                   power_good=True)
        self.assertIn("persistence write failed",window.battery_status_value.text())
        self.assertIn("ENABLE OUTPUT ERROR",window.battery_status_value.text())

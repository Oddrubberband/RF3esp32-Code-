## Battery telemetry

Both battery-enabled profiles include `battery_estimated_mv` beside `battery_mv`. Both are integer
millivolts: `battery_mv` is the sensed protection value, and the estimate is
100 mV lower, floored at zero. Missing values remain unavailable. Battery states
include startup, averaging, settling, fresh, held, stale, low_pending,
sensor_error and cutoff. Existing status is cached; requests do not sample ADC.

USB text and Classic Bluetooth SPP may emit this asynchronous event:
`BATTERY SHUTOFF turn_physical_power_boot_switch_OFF_connect_charged_replacement_source`
It means shutdown has started. STATUS reports the persisted latch and output requests. The 4 MB USB FEM build uses independent radio power. The custom PCB uses GPIO21 for antenna selection.
No host acknowledgement is required. Message delivery is not guaranteed if device
or connection power disappears. The event does not change RF Protocol v2.

# Bluetooth SPP control

Both ESP32 profiles enable Classic Bluetooth Serial Port Profile. Pair in Windows, then choose the correct paired device in the GUI:

| Profile | Bluetooth name |
|---|---|
| Custom PCB | RF3-rf3-pcb |
| Devboard | RF3-rf3-devboard |

The service is RF3-SPP. This firmware does not add a BLE transport. Each GUI process connects to one selected board. Separate instances may connect to different boards, with independent rotating diagnostics. USB may provide board power while Bluetooth carries control; use the board's established power wiring.

HELLO establishes host identity independently of radio health. The GUI shows port setup and handshake stages with deadlines. Only a unique outgoing COM mapping for the selected address is accepted. A successful host connection does not prove radio power, wiring or RF delivery.

For flashing, disconnect that board in the GUI, close serial monitors, and use its USB programming COM port with the manual helper. GUI control over Bluetooth and manual USB firmware programming are separate operations.

See [CHANGELOG](../CHANGELOG.md) for error 87, handshake, queue and stale-session fixes and their verification.

# RF3 R28 board setup

| Board | Build ID shown by HELLO | Flash | Radio CE / CSN / IRQ |
| --- | --- | --- | --- |
| Custom ESP32 PCB with external Skyworks FEM and voltage sense | `RF3-R28-CUSTOM` | 16 MB | 17 / 27 / 16 |
| USB ESP32 devboard with EBYTE SPI module | `RF3-R28-EBYTE` | 4 MB | 27 / 5 / 26 |

Both profiles use GPIO 18, 23, and 19 for SPI SCK, MOSI, and MISO. The custom build expects the ANT1 path and starts with the receive LNA bypassed. Gain can be selected in Standby for subsequent receive windows. Use the EBYTE profile for the devboard with its commercial SPI module.

## Build from source

With PlatformIO installed, open PowerShell at the repository root or in the extracted R28 board source package.

```powershell
.\Build_Both.cmd
```

Use `.\Build_Both.cmd --board custom` or `.\Build_Both.cmd --board ebyte` to build a single target. The build helper checks image integrity, firmware identity, application size and the expected partition addresses before staging images and their hashes. With PlatformIO installed in the active Python environment, the equivalent is `python build_boards.py --build`.

## Flash the matching board

Use a successfully staged local build or extract the R28 firmware ZIP. Close the GUI connection and any serial monitor on the board's USB programming port. Keep the independent FEM supply off through flashing, reset, and startup of the custom board.

```powershell
.\boards\custom-pcb-sense\Flash_Custom_PCB.cmd
.\boards\devboard-ebyte\Flash_Devboard.cmd
```

Run the matching line and enter that board's USB programming COM port. The launcher uses PlatformIO's Python and esptool, verifies the image hashes, and prompts for manual BOOT and EN. Release BOOT once writing begins, then press EN when programming finishes. The launchers preserve the SPIFFS partition.

Reconnect using [R24 Field Desktop](https://github.com/Oddrubberband/RF3esp32-Code-/releases/tag/v2026.11.24-field-r24) and check the build ID before testing. Set both boards to the same channel, starting with channel 0 for the RF3 bench setup. Select Reliable (ACK), start Receive on the destination, and send a known file. Check peer completion, CRC, elapsed time, and retries. For a rollback use the matching, previously verified image for that board.

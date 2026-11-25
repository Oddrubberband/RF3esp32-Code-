# RF3 file transfer for ESP32 and nRF24

RF3 Desktop controls the two RF3 boards over USB or Classic Bluetooth. The current Windows app is **RF3 Field Desktop R24**. The current firmware source is **R28** for the 16 MB custom PCB and the 4 MB ESP32 devboard with an EBYTE radio.

- [Download the R24 Windows GUI](https://github.com/Oddrubberband/RF3esp32-Code-/releases/tag/v2026.11.24-field-r24)
- [Firmware downloads and previous releases](https://github.com/Oddrubberband/RF3esp32-Code-/releases)
- [Choose the board, build or flash, and verify the firmware ID](START_HERE.md)
- [Build the GUI or run the software checks](docs/build_and_test.md)
- [Changes by revision](CHANGELOG.md)

## Use the Windows GUI

Extract the whole R24 ZIP and open **RF3Desktop.exe**. Keep the `_internal` folder beside the executable. Open **Practice.cmd** to try the GUI without hardware. With hardware, open one GUI window per connected board and choose its USB COM port or outgoing Bluetooth serial port.

Select the same channel and file mode on both boards. Start **Receive** on the destination before sending. In **Reliable (ACK)** mode, look for peer completion and a matching CRC. In **No ACK** mode, sender completion reports that the local board finished sending. The SPIFFS page moves files between the PC and the attached board.

## Source layout

| Component | Source |
| --- | --- |
| R24 Field Desktop | [field-desktop](field-desktop) |
| R28 custom ESP32 PCB with external FEM and voltage sense | [boards/custom-pcb-sense](boards/custom-pcb-sense) |
| R28 USB ESP32 devboard with EBYTE SPI module | [boards/devboard-ebyte](boards/devboard-ebyte) |

R28 is a bench candidate. Its software checks cover the Reliable transfer changes. Over-the-air validation of R28 and a fix for the custom FEM TX issue are still pending. The root-level legacy firmware and the older USB FEM profile belong to the [November 20 release](https://github.com/Oddrubberband/RF3esp32-Code-/releases/tag/v2026.11.20-firmware-audit).

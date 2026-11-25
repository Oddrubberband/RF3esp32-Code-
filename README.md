# RF3 file transfer for ESP32 and nRF24

RF3 Field Desktop controls the 16 MB custom PCB and 4 MB USB devboard over USB or Classic Bluetooth. The current Windows GUI is **RF3 Field Desktop R24**, with its source in [field-desktop](field-desktop). The November 20 firmware release provides **RF3-CBT-R4-20261119** for the custom PCB and **RF3-NFEM-R3** for the devboard with an external FEM. Check the board and radio profile before flashing.

- [Download the R24 Windows GUI](https://github.com/Oddrubberband/RF3esp32-Code-/releases/tag/v2026.11.24-field-r24)
- [All 11 firmware fixes explained](FIRMWARE_FIXES.md)
- [Choose your board, check the wiring and install the firmware](START_HERE.md)
- [Firmware and previous releases](https://github.com/Oddrubberband/RF3esp32-Code-/releases)
- [Build and test from source](docs/build_and_test.md)
- [Changelog and previous releases](CHANGELOG.md)

## Transfer a file

Extract the whole R24 Windows ZIP and open **RF3Desktop.exe**. Keep the `_internal` folder beside the executable. Use **Practice.cmd** to try the GUI without hardware. Open one desktop window per board. Select the board’s USB port or use **Scan Bluetooth** to find it. For direct Bluetooth port selection, use that board’s outgoing serial port.

Select the same channel and transfer mode on both boards and start **Receive** on the destination first. In **Reliable** mode, the receiver checks the file size and checksum before confirming completion. In **No ACK** mode the sender reports local completion. Use Reliable mode when you need confirmation from the other board.

The SPIFFS tab copies files between the PC and a board. The radio transfer controls send a file between the two boards. See the [protocol guide](docs/protocol_v2.md) and [one-way transfer guide](docs/no_ack_mode.md) for details.

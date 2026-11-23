# RF3 file transfer for ESP32 and nRF24

## Field Edition R8 desktop for Windows

[Download the portable GUI ZIP](https://github.com/Oddrubberband/RF3esp32-Code-/raw/refs/heads/field-edition-r8/downloads/RF3-Field-R8-Windows-x64.zip). Extract the whole ZIP and open **RF3Desktop.exe**. It opens in hardware mode and includes its Python and Qt runtime.

For a two-PC test, plug one board into each PC. Connect to the local COM port in each GUI and confirm **Physical radio** on the Connection page. Use the same channel and **Reliable (ACK)** mode on both boards. Start **Receive** on the destination before sending a file from the other PC. Check the received file and the sender's peer confirmation.

ZIP SHA-256: `86c88b3dfb8a21bb42234119905609153f44bfb5315fd9adb602adaa677d3351`

### Firmware snapshot on this branch

RF3 Desktop controls the 16 MB custom PCB and 4 MB USB devboard over USB or Classic Bluetooth. The current source builds **RF3-CBT-R4-20261119** for the custom PCB and **RF3-NFEM-R3** for the devboard. The desktop source on this branch is **RF3-FIELD-R8**.

- [All 11 firmware fixes explained](FIRMWARE_FIXES.md)
- [Choose your board, check the wiring and install the firmware](START_HERE.md)
- [Download the Windows desktop and firmware](https://github.com/Oddrubberband/RF3esp32-Code-/releases)
- [Build and test from source](docs/build_and_test.md)
- [Changelog and previous releases](CHANGELOG.md)

## Transfer a file

Extract the Field Edition ZIP and open **RF3Desktop.exe**. Open one desktop window per board. Select the board’s USB port or use **Scan Bluetooth** to find it. For direct Bluetooth port selection, use that board’s outgoing serial port.

Select the same channel and transfer mode on both boards and start **Receive** on the destination first. In **Reliable** mode, the receiver checks the file size and checksum before confirming completion. In **No ACK** mode the sender reports local completion. Use Reliable mode when you need confirmation from the other board.

The SPIFFS tab copies files between the PC and a board. The radio transfer controls send a file between the two boards. See the [protocol guide](docs/protocol_v2.md) and [one-way transfer guide](docs/no_ack_mode.md) for details.

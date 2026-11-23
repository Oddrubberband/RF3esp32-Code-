# RF3 peer text bench pair

Use Field Edition R17. Extract the entire kit and disconnect the GUI and serial monitor before running the matching launcher on the USB programming port. Keep the battery disconnected on the custom USB-bench board and its independent FEM supply off through flash, reset and startup. Follow the existing bench power-up procedure. Keep the previous matching kit for rollback.

| Board | Launcher | Expected DIAG build |
| --- | --- | --- |
| 16 MB USB-bench custom ESP | custom-pcb-usb-bench/Flash_Custom_PCB.cmd | RF3-P2P-C1-20261123 |
| 4 MB direct EBYTE devboard | devboard-ebyte/Flash_Devboard.cmd | RF3-P2P-E1-20261123 |

1. In both GUIs select physical radio mode, match RF settings and select Receive. Start with Reliable file mode.
2. Send one character from the custom board, check its exact arrival on the EBYTE GUI, then send a reply. YOU indicates local transmission only.
3. Repeat with 22, 23 and 154 printable characters. Select No ACK on the receiving board and press Receive again. Repeat in both directions. Take turns sending.
4. Record both DIAG IDs, RF channels, message length, and both logs if a message is missing. Live text has no peer delivery acknowledgement. SPI, file CRC and transfer timing follow the previous bench revision.

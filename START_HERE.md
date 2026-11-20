# Use the firmware for your board

| Board | Build shown in Read RF Diagnostics | Firmware installer option |
|---|---|---|
| Old custom ESP32 PCB, 16 MB | RF3-CBT-R4-20261119 | custom |
| USB ESP32 devboard, 4 MB | RF3-NFEM-R3 | devboard |

Download and extract the firmware ZIP for **v2026.11.20-firmware-audit**. Turn off the separate front-end supply before resetting or flashing. Disconnect the board in RF3 Desktop and close its serial monitor. In the extracted firmware folder, run the line for your board and enter its **USB programming port** when prompted.

```powershell
.\Flash_Manual.ps1 -Board custom
.\Flash_Manual.ps1 -Board devboard
```

The helper checks the image files and prompts you to operate BOOT and EN. Release BOOT and press EN after verification. Check the build ID in the desktop before using the radio. Re-enable the separately powered front end only after verifying the idle control levels and correct wiring on your assembly.

## Which wiring the firmware expects

| Signal | Old custom PCB GPIO | USB devboard GPIO |
|---|---:|---:|
| Radio CE / CSN / IRQ | 17 / 27 / 16 | 27 / 5 / 26 |
| Front end CTX / CRX | 32 / 33 | 32 / 33 |
| Front end CSD / CPS / ANT_SEL | 26 / 25 / 21 | 14 / 13 / 17 |

The custom PCB uses GPIO15 for battery enable, GPIO34 for sensing, and supports antenna selection in standby. The USB devboard uses independent radio and front-end supplies and selects ANT1. The board and front end require a common ground. [Fixes and test results](FIRMWARE_FIXES.md)

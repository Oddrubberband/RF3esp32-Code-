# Staged File Workflow

1. Stage a file for transfer.

PlatformIO sidebar:
- Run `Stage Demo File`.
- Pick any file when prompted.

CLI:

```bash
python tools/stage_demo_file.py path/to/input.bin
```

The helper script:
- accepts arbitrary input files
- writes the staged copy into this `data/` folder
- checks that the resulting SPIFFS image still fits
- can optionally validate the fit with `mkspiffs`

2. Upload the filesystem image from PlatformIO.

In the PlatformIO sidebar, use `Upload Filesystem Image`.

3. Flash the firmware and boot the board.

4. Open the serial monitor and use commands such as:
- `FILES`
- `SELECT your_file.bin`
- `TX`
- `RX`
- `STATUS`

Notes:
- The firmware lists every regular staged file from `/spiffs`.
- Partial RX saves use `.part` temporarily and are hidden from `FILES`.
- Completed RX streams are saved as `rx_<8-hex-digit-transfer-id>.bin`, with a
  collision suffix when needed.
- Protocol v2 uses fixed 32-byte nRF24 frames with a 12-byte DATA header and up
  to 20 data bytes. One transfer is limited to 1,310,720 bytes (65,536 DATA
  packets).
- SPIFFS filenames are limited to 31 ASCII bytes; the staging helper sanitizes
  and bounds destination names before copying.

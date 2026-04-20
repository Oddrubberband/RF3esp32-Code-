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
- Completed RX streams are saved as `rx_####.bin`.
- Each packet uses 4 bytes of header and up to 28 bytes of payload.

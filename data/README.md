# Audio Upload Workflow

1. Prepare an MP3 for the demo.

PlatformIO sidebar:
- Run `Prepare Demo Audio`.
- Pick an `.mp3` file when prompted.

CLI:

```bash
python tools/prepare_demo_audio.py path/to/input.mp3
```

The helper script:
- accepts only `.mp3` input
- converts it to raw `8000 Hz`, `8-bit`, `mono` PCM
- writes the converted track into this `data/` folder as a `.u8` file
- rejects files that do not fit the configured SPIFFS partition

2. Upload the filesystem image from PlatformIO.
   In the PlatformIO sidebar, use `Upload Filesystem Image`.

3. Flash the firmware and boot the board.

4. Open the serial monitor and use commands such as:
- `FILES`
- `SELECT your_track.u8`
- `TX`
- `RX`
- `SLEEP`
- `STATUS`

Notes:
- The firmware now lists and selects `.u8` tracks from `/spiffs`.
- Each packet uses 4 bytes of header and up to 28 bytes of audio.
- The conversion helper requires `ffmpeg` on the host machine.

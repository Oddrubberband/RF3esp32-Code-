# Audio Upload Workflow

1. Convert a song to raw 8-bit mono PCM:

```bash
ffmpeg -i input.mp3 -ac 1 -ar 8000 -f u8 song.u8
```

2. Put the generated `song.u8` file in this `data/` folder.

3. Upload the filesystem image from PlatformIO.
   In the PlatformIO sidebar, use `Upload Filesystem Image`.

4. Flash the firmware and boot the board.
   The app reads `/spiffs/song.u8` and transmits it in 32-byte radio packets.

Notes:
- The current sender expects the file path `/spiffs/song.u8`.
- Audio format is fixed at `8000 Hz`, `8-bit`, `mono`.
- Each packet uses 4 bytes of header and up to 28 bytes of audio.

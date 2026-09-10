# RF3 beginner README

This guide explains RF3 from the beginning. You do not need to understand radio
protocols or embedded programming to follow it. If you want implementation
details, use [README_TECHNICAL.md](README_TECHNICAL.md).

## What RF3 does

RF3 lets one ESP32 board send a file to another ESP32 board through an nRF24
radio module.

Think of the two boards as:

- **Sender:** reads a file from its flash memory and transmits it.
- **Receiver:** listens, rebuilds the file, checks it, and saves it.

The file can contain anything. The included `.u8` audio files are treated as
ordinary binary files. The receiver does not trust a transfer merely because
radio packets arrived: it verifies the exact byte count and CRC checksum before
declaring success.

## What is in the project now

Phase 1 is the current completed software phase. It provides:

- Reliable file transfer with acknowledgements and retries.
- Progress messages at 10-percent intervals.
- Final byte-count and CRC verification.
- Safe temporary files that are hidden until a transfer succeeds.
- Commands to inspect, delete, clean, or format board storage.
- A readable STATUS screen that remains responsive during a transfer.
- Protection against several radio, cleanup, replay, and JSON errors.
- Separate builds for the custom PCB and the ESP32 devboard.

The current GitHub branch is `codex/phase-one-laptop`.

## The two supported boards

The boards use different control pins. The firmware build must match the board.

| Wire | Custom PCB | ESP32 devboard |
|---|---:|---:|
| CE | GPIO17 | GPIO27 |
| CSN | GPIO27 | GPIO5 |
| IRQ | GPIO16 | GPIO26 |
| SCK | GPIO18 | GPIO18 |
| MOSI | GPIO23 | GPIO23 |
| MISO | GPIO19 | GPIO19 |

### Important pin warning

For the custom PCB, **CSN is GPIO27 and IRQ is GPIO16**.

An older document and older commit used CSN=GPIO5 and IRQ=GPIO27 for the custom
PCB. That mapping is wrong for the confirmed board and caused `fault=1`. GPIO5
is the correct CSN only on the devboard. Do not swap the profiles.

## Words you will see

| Term | Plain-English meaning |
|---|---|
| ESP32 | The main computer on each board |
| nRF24 | The small radio chip/module |
| SPI | The wired connection between the ESP32 and its radio |
| SPIFFS | The board's flash area used like a small disk |
| TX | Transmit or send |
| RX | Receive or listen |
| ACK | A reply saying a packet arrived |
| CRC32 | A checksum used to detect changed or missing data |
| Channel | The selected radio frequency number |
| Power | The nRF24 output-power setting from 0 through 3 |
| COM port | The Windows serial connection to a board |
| Flash | Install firmware on a board |

## Safety before connecting hardware

- Confirm both radio antennas or appropriate RF loads are attached before
  transmitting.
- Use the board's intended power input and 3.3 V logic.
- Do not assume a USB-UART adapter's 3.3 V output can power an ESP32 and a radio.
- Close a serial monitor before uploading through the same COM port.
- Start packet tests at `POWER 0`.
- Do not use CW/continuous-wave mode until the hardware and test setup are
  appropriate for intentional continuous RF output.

For a first-ever board power-up, stop here and follow
[docs/board_bringup.md](docs/board_bringup.md).

## Get the correct code on another PC

For a new clone:

```powershell
git clone https://github.com/Oddrubberband/RF3esp32-Code-.git
Set-Location .\RF3esp32-Code-
git switch --track origin/codex/phase-one-laptop
```

For an existing clone:

```powershell
git fetch origin
git switch codex/phase-one-laptop
git pull --ff-only
```

Before changing anything, `git status --short --branch` should show the Phase 1
branch and no unexpected modified files.

## Install the build tools

From the repository directory:

```powershell
py -m venv .venv
& .\.venv\Scripts\python.exe -m pip install -r requirements.txt
```

You can call PlatformIO directly from that environment; activation is optional.

## Build both firmware versions

```powershell
& .\.venv\Scripts\platformio.exe run -e rf3_custom_pcb -e rf3_esp32_devboard
```

Success for both environments means the source compiled. It does not prove that
the radios, wiring, antennas, or power supply work.

## Find the COM ports

In PowerShell:

```powershell
Get-PnpDevice -Class Ports | Format-Table Status,FriendlyName,InstanceId -AutoSize
```

Unplug and reconnect one board if you are unsure which entry belongs to it.
The examples below use the ports from the latest test setup:

- Custom PCB: COM5
- Devboard: COM3

Replace those values if Windows assigned different ports.

## Flash the custom PCB

The custom PCB requires manual bootloader entry.

1. Close its serial monitor.
2. Hold the **BOOT** button.
3. Press and release **EN/RESET** while still holding BOOT.
4. Start the upload:

```powershell
& .\.venv\Scripts\platformio.exe run -e rf3_custom_pcb -t upload --upload-port COM5
```

5. Release BOOT when the upload begins.
6. After success, press EN/RESET to run the firmware.

Open its monitor:

```powershell
& .\.venv\Scripts\platformio.exe device monitor --port COM5 --baud 115200 --dtr 0 --rts 0
```

Use `Ctrl+C` to close the monitor.

## Flash the ESP32 devboard

```powershell
& .\.venv\Scripts\platformio.exe run -e rf3_esp32_devboard -t upload --upload-port COM3
```

Open its monitor:

```powershell
& .\.venv\Scripts\platformio.exe device monitor --port COM3 --baud 115200 --dtr 0 --rts 0
```

If automatic upload entry fails, use the board's BOOT/EN procedure.

## Firmware and files are separate

Uploading firmware changes the program. It normally does not replace the SPIFFS
files. Uploading SPIFFS is a separate, destructive action:

```powershell
& .\.venv\Scripts\platformio.exe run -e rf3_custom_pcb -t uploadfs --upload-port COM5
```

Use the matching environment and port. `uploadfs` replaces that board's current
filesystem with the contents of the repository's `data` directory. Back up
anything you want to keep first.

To add a host file to the next SPIFFS image:

```powershell
& .\.venv\Scripts\python.exe tools\stage_demo_file.py C:\path\to\your-file.bin
```

Then deliberately run `uploadfs` on the sender. Do not upload a large sender
image to the receiver unless you intend to; stored files reduce the safe receive
capacity.

## Check each board after flashing

At each `rf24>` prompt:

```text
STATUS
FILES
```

A healthy board should show:

- The correct `profile`.
- `filesystem=ready`.
- `fault=0`.
- A sensible STATUS/FIFO register value.
- The expected pinset in the boot log.

The radio normally starts on channel 76. If you select another channel, both
boards must use the same number.

## Perform a first file transfer

Use two PowerShell windows so both serial monitors remain visible.

### On the receiver

```text
CHANNEL 0
POWER 0
RX
STATUS
```

Confirm `state=RxListening` and `fault=0`.

### On the sender

```text
CHANNEL 0
POWER 0
FILES
TX README.md
```

Use a filename that actually appears under `FILES`. Start with a small file.
After that succeeds, try `speech_test.u8` or `song.u8`.

During a large transfer, both monitors print 10%, 20%, and later milestones.
The sender temporarily shows `WaitingForDataAck` while its radio listens for
the receiver's reply. That is expected.

## How to recognize success

The receiver should print a message similar to:

```text
Protocol v2 verified and published rx_1234ABCD.bin
```

The sender should print:

```text
Protocol v2 remote receiver verified and published
```

The transfer is successful only when:

- Both sides show the same transfer ID.
- The byte and packet totals match.
- The final CRC32 values match.
- The receiver published the file.
- The sender accepted `COMPLETE`.

`tx_ok=true` alone means only that the sender's radio transmitted locally. It
does not prove the other board received or saved the file.

The first recorded large Phase 1 test transferred 1,112,701 bytes in 55,636
packets. Both boards reported CRC32 `5138505B`. TX measured 1,908 B/s, RX
measured 1,919 B/s, and only one retry was required.

## Inspect the received file

On the receiver:

```text
STANDBY
FILES
STATUS
```

The saved name is based on the transfer ID, for example
`rx_90757F3C.bin`. Sending the same source name again does not overwrite a
completed file; a collision-safe suffix is used when necessary.

## Storage commands

```text
FILES
FS INFO
FS LIST ALL
FS DELETE rx_90757F3C.bin
FS CLEAN PARTIALS
FS FORMAT CONFIRM
```

- `FILES` shows normal files.
- `FS INFO` shows used, free, and safe receive space.
- `FS LIST ALL` also shows hidden temporary files.
- `FS DELETE` removes one visible file.
- `FS CLEAN PARTIALS` removes abandoned receive fragments.
- `FS FORMAT CONFIRM` erases every stored file.

Run `STOP` before storage mutation if a transfer or loop is active. Formatting
cannot be undone from the console.

## Other useful commands

| Command | Purpose |
|---|---|
| `HELP` | Print the complete command list |
| `STATUS` | Show current radio, file, TX, and RX state |
| `STOP` | Stop active TX, RX, CW, or Morse work |
| `SELECT <file>` | Choose the default sender file |
| `TX [file]` | Send one file |
| `TX LOOP <count> [file]` | Send repeatedly |
| `RX` | Listen for a transfer |
| `STANDBY` | Stop listening and remain powered |
| `SLEEP` / `WAKE` | Enter or leave radio sleep |
| `POWER <0-3>` | Select packet transmit power |
| `CHANNEL <0-125>` | Change the channel |
| `CHANNEL PREVIEW <0-125>` | Show nominal frequency without changing it |
| `MORSE <text>` | Transmit supported text as Morse |

`CW` commands intentionally create continuous or repeated RF output for bench
testing. Do not experiment with them casually.

## Common problems

### Upload cannot connect

- Close the monitor using `Ctrl+C`.
- Confirm the COM port.
- For the custom PCB, repeat the BOOT-plus-EN sequence.
- Check that the USB-UART adapter uses 3.3 V logic and shares ground.

### `filesystem=unavailable`

The SPIFFS image may never have been uploaded or may be damaged. Preserve any
wanted data before running `uploadfs` or formatting.

### Custom PCB starts with `fault=1`

Confirm the custom firmware environment was used and verify:

```text
CE=17, CSN=27, IRQ=16, SCK=18, MOSI=23, MISO=19
```

Do not restore CSN=5/IRQ=27 on the custom PCB.

### Receiver reports fault 4 after a completed transfer

The current build has one recorded transient post-transfer RX read failure.
Recover with:

```text
STANDBY
RX
STATUS
```

If STATUS returns to RxListening with `fault=0`, the radio recovered. Confirm
the completed file with `FILES`. This issue is documented and still needs a
software correction before Phase 2.

### Sender times out

- Confirm both boards use the same channel.
- Put the receiver into `RX` before starting TX.
- Start close together at `POWER 0`.
- Check antennas, 3.3 V power, common ground, and SPI wiring.
- Inspect the sender retry count and both STATUS screens.

## What happens next

Phase 2 is called **Measurement Foundation**. It will make throughput, protocol
exchange counts, first-attempt success, retry distribution, completion results,
stage timings, CRCs, and RF settings available in structured CSV/JSON form.

After that comes a physical RF and power baseline. Changes such as acknowledging
multiple DATA packets at once or switching to a faster RF data rate should be
compared using those measurements. Phase 2 has not started.

For current project state and continuation instructions, read
[docs/project_handoff.md](docs/project_handoff.md).


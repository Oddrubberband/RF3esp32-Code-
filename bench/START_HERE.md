# RF3 no-ACK recovery bench build R1

Use the 16 MB USB-bench custom controller with its custom radio and separately powered FEM, and the 4 MB devboard fitted with the direct EBYTE module. Install the matching image on both boards. This test pair uses Field Edition R8's existing file, no-ACK and Stop controls. The original reliable mode uses its existing radio CRC16 and final file CRC32.

## Install and check

Extract the entire package. Close the radio's GUI connection and serial monitor. For the custom USB-bench PCB run `custom-pcb-usb-bench/Flash_Custom_PCB.cmd`. For the EBYTE board run `devboard-ebyte/Flash_Devboard.cmd`. Each launcher uses the existing Windows PlatformIO Python/esptool installation, checks the three image hashes, asks for the USB programming COM port and prompts for the board's manual boot sequence. It writes the bootloader, partition table and application. The filesystem and NVS are outside those writes for the matching baseline partitions.

Reconnect and read DIAG. Expect `RF3-NAG-C1-20261122` on the custom PCB and `RF3-NAG-E1-20261122` on EBYTE. Retain your previous matching flash packages for rollback. R1 is an experimental build for a physical test. The custom version inherits the USB-bench battery configuration.

## First transfer

1. Use the same RF channel and matching radio settings. Choose **no-ACK receive** on the listening board. Choose a small file on the transmitting board, select **no-ACK send** once. The firmware automatically makes three passes with the same ID; use Stop to end early. Starting a new send creates a new session.
2. A successful run produces one received file. Download it and compare its size and SHA-256 with the original. TX shows sent locally. RX alone can verify delivery. The transmit byte count reaches the source size before the repair portion has finished.
3. Repeat in the reverse direction. Then start RX during a running repeated transmission. Matching headers appear every 32 data or repair frames. The receiver retains unique packets between passes and can finish without seeing END. Stop the active receive locally when deliberately abandoning a partial file.
4. Try the 21,632-byte file, followed by the 117,395-byte file. Allow room for the output plus 160 bytes per 16 source packets and filesystem overhead. A 117,395-byte transfer can occupy 176,115 bytes of temporary file storage. Record radio frames read, unique accepted bytes, CRC, elapsed time, RX poll gaps and failures. The file preallocation, repair-sidecar allocation, scattered flash writes and file CRC readback run on the receiver and can delay radio servicing. If raw reads never advance after the first few frames, record STATUS and DIAG on RX.

The boards calculate repair packets themselves so the existing desktop can send any supported file. An attached PC is optional for parity calculation in the design; this build uses the embedded implementation. The transmitter sends one original symbol per block in turn, across the full file, then eight Cauchy repair rounds per 16 originals and rotates subsequent passes. The new frames require this candidate on both ends for the no-ACK experiment. Keep both radios' CRC settings matched.

## Rebuild

With PlatformIO Core and the pinned Espressif platform available, run `python pack_images.py --build` from the extracted source directory. The script builds the two profiles, checks the app identifiers, image digests and partitions, and produces matching image manifests. It performs no programming.

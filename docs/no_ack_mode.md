## Battery update RF3-BAT-R1-20261105

The devboard may pause listening RX for a battery check. No ACK has no delivery
feedback or retransmission, so packets can be lost during that pause. The existing
size and CRC checks reject an incomplete received file. Sender completion remains
unconfirmed delivery. Cutoff cancels the local sender before any further RF attempt.
Use Reliable mode when recovery from receiver pauses is needed.

# Separate no-ACK file mode

The No ACK implementation originated with NOACK-R3 and is included in this release.
Both profiles retain separate GPIO/flash/partition contracts and manual flashing.

The default mode remains reliable file transfer, with the selected 1,000 ms
software DATA ACK timeout. Under **Radio Controls**, choose **File transfer mode**
then **No ACK (one-way)** in both GUI windows. Choose **Reliable (ACK)** for normal
ACK-based operation. The option covers PC-file upload/transmit,
stored-file transmit, and Receive. Stop an active receive before switching modes.

The RETRY10-R1 update raises reliable mode to 10 retries (11 total attempts) and
uses 20-second sender/receiver inactivity limits. One-way mode still has no
retries; its shared receive inactivity cleanup also uses 20 seconds.

## Radio behavior

- `TXNOACKHEX <UTF8hex>` sends one START, each DATA and one END, with no radio
  READY/ACK/COMPLETE waits and no hardware or application retransmissions.
- `RXNOACK` accepts only the distinct `RFN` one-way frames. It sends no RF READY,
  ACK, NACK, COMPLETE, ERROR or cancellation/timeout response. It ignores normal
  reliable, live-text and remote-command frames while in this receive mode.
- nRF hardware ACK/retry registers are written to zero and read back before
  no-ACK operation. Local SPI/register/TX-completion checks still detect local
  faults; they are not evidence of peer reception. The driver's recovery resend
  is disabled for each one-way packet; reliable mode retains its existing recovery.
- The file sender stays out of RX while sending. A local Stop can send a forward
  one-way CANCEL once, without waiting for a return response.
- Host USB/Bluetooth upload and control replies remain enabled. They do not
  cross the nRF link and are needed to control and verify the local device.

The module is the E01-ML01DP5 with its internal PA/LNA, controlled by the ESP32;
the separate Skyworks amplifier is not part of the current test setup. No PA
power, RF channel, air-rate or Bluetooth policy change is included.

## Integrity and truthful status

The sender reports **Sent (unconfirmed)** after local transmission. It never
reports peer confirmation in this mode. This terminal state is distinct from
reliable Completed; GUI validation requires the matching no-ACK mode, full local
byte count, no local error and `peer_complete=false`. A no-ACK request cannot
fall back to a reliable TX command if required firmware capabilities are absent.

The receiver retains size/sequence/CRC32 checks and the existing collision-safe
temporary-file/atomic-publication path. It preserves supported file suffixes.
Only a complete, correct file is published. A lost packet cannot be requested
again in this mode: reception fails locally or times out and discards the partial
file. A lost START or END can therefore prevent a completed file even if the
sender reports all frames sent. Inspect the receiving GUI and export/compare
the file separately to establish delivery. Empty files remain valid.

## Pacing and compatibility

The sender allows a 250 ms guard after START and a 10 ms gap after each DATA
frame to give the receiver/storage time to run. These are conservative initial
software settings, not measured timing guarantees or claimed throughput. Input
file DATA still carries 20 bytes in each fixed 32-byte frame at the existing
250 kbps air rate. No automatic loss recovery is promised.

`RFN` reuses the v2 field layout and integrity checks but has a distinct magic.
An ordinary reliable receiver ignores it. A no-ACK receiver ignores reliable
frames. Use matching new firmware and select the matching mode at both ends;
older firmware is not silently treated as compatible. Normal RX/TX continues
using the original `RF2` reliable wire format.

## Use and rollback

1. Flash this folder's matching custom-PCB and devboard images with its manual
   helper. Use each board's actual USB programming port and the existing manual
   BOOT/EN sequence. Do not upload/erase SPIFFS.
2. Launch this folder's GUI twice. Connect to the two boards and select No ACK
   in both windows. Put the devboard in Receive before transmitting from the PCB.
3. Start at the user's working power-2/channel baseline. Compare sender local
   progress with receiver verification and exported bytes. No agent-operated RF
   testing or hardware qualification is performed during software verification.
4. To return to reliable mode, Stop reception, select Reliable (ACK) on both GUIs,
   and press Receive again. To restore the original package/images, use the
   corresponding older release GUI and firmware package.

See the root README and docs/build_and_test.md for current checks and commands.

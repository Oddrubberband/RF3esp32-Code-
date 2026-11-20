# RF3 firmware fixes explained

This guide covers the 19 November firmware audit for the **16 MB custom PCB** (RF3-CBT-R4-20261119) and the **4 MB USB devboard** (RF3-NFEM-R3). It includes all 11 findings. Each numbered item describes the symptom and what changed.

## Bluetooth and the desktop

1. **Bluetooth could get stuck after a disconnect.** Sometimes the Bluetooth software declined the first request to close a connection. The board could then keep the old connection slot occupied, preventing the next connection. Both boards now retry the close after a quarter of a second, even when idle. A retry for an old connection is cancelled when that connection closes.

2. **The devboard could carry old Bluetooth traffic into a new session or report the wrong transfer mode.** It now clears pending traffic when a connection closes and gives a busy connection a bounded time to catch up. If a one-way file transfer fails before sending, the devboard still tells the desktop which mode you chose. That addresses one cause of the “file transfer mode disagrees” message.

## Sending files and using Stop

3. **The devboard could miss a reply that arrived too quickly.** Reliable mode works by sending a packet and listening for the other board to acknowledge it. The devboard now switches to listening as soon as transmission finishes and keeps replies that have already arrived. Both boards allow 5 milliseconds before sending the next reply or follow-up packet. This gives the peer time to switch direction.

4. **A second Send command could interrupt a file copy.** The older send-once and send-on-repeat commands could interfere while the desktop was copying a file to or from the board. Both boards now decline the conflicting command before changing the transfer already in progress.

5. **Stop could report idle too early.** When the radio was busy updating its state, the firmware could mistake that delay for a completed stop. Both boards now check whether the operation still owns the radio. If the wait expires, the stop request stays active.

6. **Starting an upload could discard a partial file.** Under load, the board could disturb an existing upload before checking whether it was free to start another. Both boards now preserve the partial upload and ask the desktop to retry when they cannot establish that it is safe to proceed.

7. **A send-once or Morse transmission could run twice.** A delay after transmission could prevent the firmware from recording that the job had finished. Both boards now finish that bookkeeping before looking for another job. A Stop that arrives during that interval is also respected.

## Text, USB commands and logs

8. **A repeated piece of a text message could erase progress.** Long messages arrive in pieces. If the first piece arrived twice, later pieces already received could be lost. Both boards now recognize a repeated first piece and keep assembling the message.

9. **An overlong USB command could run in shortened form.** Both boards now discard the entire line when it is too long. The next correctly sized command is read normally.

10. **Invalid numbers could become valid-looking settings.** A negative number or one too large to store could be interpreted as a large positive value. Both boards now reject the entire invalid value and leave the previous value in place.

11. **The log reader could reject a valid empty packet.** The computer-side tool can now read back a log entry with no payload. It also rejects malformed entries without returning a partly decoded record and reports errors encountered while finishing a log write. This fix affects reading and writing logs on the computer.

## Other changes since the previous GitHub release

The stable desktop now retries a timed-out Bluetooth connection once after looking up the selected device again. After Stop, it uses the board’s full transfer report to show the cancellation and byte count. A one-way send can finish locally without proving that the peer received the file.

Both firmware builds control the Skyworks radio front end. The custom PCB can switch antennas in standby and retains its battery sensing and cutoff. The USB devboard uses ANT1 and separately powered radio hardware. [Board and installation guide](START_HERE.md)

## What was checked

Two independent software reviewers examined the proposed fixes before implementation and checked the finished changes. All 208 custom-board firmware tests, 207 devboard firmware tests and 273 stable desktop tests passed locally. The reviewers also checked that the six programming files matched the builds and fit their respective boards.

A timing model gave both boards about 5.75 milliseconds to spare in the nominal test case. Neither board was flashed or tested over the air during the audit. Physical timing and delivery reliability still need measurement. One Bluetooth question also remains open: what happens if a connection closes and its identifier is reused while a write is pending? That sequence was not reproduced.

To check the update on the actual pair, confirm each build ID in **Read RF Diagnostics**, reconnect over Bluetooth several times, then send a known file in Reliable mode in each direction and compare the received files with the original. During a separate transfer, press **Stop**, check the cancellation report, and start a fresh transfer.

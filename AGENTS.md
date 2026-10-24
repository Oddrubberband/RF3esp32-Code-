# RF3 project instructions

Read README.md, CHANGELOG.md and docs/build_and_test.md before changing this project.
Keep existing custom-PCB and devboard GPIO profiles separate. Never infer the fitted
PCB pinout from a different hardware revision. Flashing is manual and user-operated:
BOOT/EN, no_reset before and after, original partition layouts. Do not open hardware
ports or transmit RF as part of software tests without explicit user authorization.

Use the hardware-free native and GUI checks described in docs/build_and_test.md.
Preserve CRC/readback, atomic exports/publication, bounded transactions, receive
ownership guards, legacy RF compatibility and filename validation. Keep credentials,
user PDFs, local logs, virtual environments and generated build directories out of Git.
Distinguish software checks, user-reported hardware observations and physical qualification.

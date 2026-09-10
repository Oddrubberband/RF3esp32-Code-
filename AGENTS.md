# RF3 continuity instructions

## Authoritative board pin mapping - do not revert

The user-confirmed mapping below is the required configuration for these boards.
Following correction `74654ce`, the September 10 custom-PCB status screenshot
shows Standby and fault=0. The devboard also passed its startup check.

| Board | CE | CSN | IRQ | SCK | MOSI | MISO |
|---|---:|---:|---:|---:|---:|---:|
| Custom PCB (`rf3_custom_pcb`) | 17 | 27 | 16 | 18 | 23 | 19 |
| Devboard (`rf3_esp32_devboard`) | 27 | 5 | 26 | 18 | 23 | 19 |

**CUSTOM PCB: CSN=GPIO27 and IRQ=GPIO16. The old CSN=GPIO5 / IRQ=GPIO27
custom-PCB mapping is superseded and must not be restored.** It survived in
the committed pre-Phase-1 baseline even though the working laptop firmware had
already corrected it; flashing that baseline produced a radio probe fault.
The devboard's CSN=GPIO5 remains correct. Do not swap the two board profiles.

Keep `platformio.ini`, `include/hardware_profile.hpp`, native pin assertions,
`tools/prepare_board_handoff.py`, and current pin documentation synchronized.
Older commits, schematics, PDFs, and historical handoffs do not override this
mapping. Any future pin change requires explicit user confirmation for the
actual board. Successful startup is not evidence of completed RF transfers.

## Continuity workflow

The user wants to resume this project in Codex on another computer without
reconstructing context from chat history. Documentation is part of the work.

1. At the start of a task, read `docs/project_handoff.md`, inspect the current
   branch/status, and read the relevant phase or subsystem document.
2. After each meaningful change, decision, investigation, or validation batch,
   update the appropriate document before finishing. Record what changed and
   why, what was actually verified, remaining limits/blockers, and the next
   concrete step. Do not log every routine read or unchanged poll.
3. Keep `docs/project_handoff.md` as the current entry point. Keep the detailed
   plan in `docs/firmware_roadmap.md`, Phase 1 evidence in
   `docs/phase_one_reliability.md`, and portable reports in `docs/reports/`.
   Link other subsystem documents from the handoff as work expands.
4. Document branch/revision context and reproducible commands using relative
   paths. Do not rely on machine-specific worktree paths, local ports, chat
   history, credentials, or ignored build logs as the only handoff evidence.
   Clearly distinguish passed tests, proposed tests, and untested hardware.
5. Keep documentation with the relevant implementation commit when possible.
   State whether work is committed and whether the branch has actually been
   synchronized; never imply that a local-only commit is available on a laptop.
   If a PDF changes, update its maintained Markdown counterpart as well.
6. Preserve the user's active-checkout isolation constraints. The existing code
   pin profiles are authoritative per the user's clarification; older schematic
   discrepancies alone are not grounds for changing them. A planning or
   documentation request does not authorize firmware changes, merging, or
   flashing. Follow any newer explicit user instructions.

Do not replace the current handoff with a chronological transcript. Preserve
useful historical reports, and make the next action easy to find.

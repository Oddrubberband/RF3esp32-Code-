# RF3 project handoff - start here

Last substantive documentation update: 2026-09-10.

## Authoritative pin mapping - do not revert

**Custom PCB: CE=17, CSN=27, IRQ=16. Devboard: CE=27, CSN=5, IRQ=26.
Both: SCK=18, MOSI=23, MISO=19.** The custom-PCB CSN=5 / IRQ=27 mapping in
older commits and handoffs is superseded; the devboard's CSN=5 is correct.
Future changes require explicit user confirmation for the actual board. The
standing instruction and full table are at the top of [AGENTS.md](../AGENTS.md).

## Current state

### Laptop pin correction - 2026-09-10

Pin correction `74654ce` is on local branch `codex/phase-one-laptop`, based on
`5a22f91`. The separate older active checkout and its uncommitted changes are
preserved. The correction and these notes are committed locally; no push was
performed in this session. Use `git log -2 --oneline` for the latest revisions.

The user reported uploading Phase 1 to both boards. The devboard boot log shows
`Radio boot OK`, Standby, and `fault=0`. The custom PCB initially reported
startup `fault=1` with SPIFFS mounted. After the corrected firmware was uploaded,
the user's September 10 screenshot shows `profile=custom-pcb`, `State=Standby`,
`fault=0`, `power=3`, `last_status=0x0E`, `fifo=0x11`, and `irq=high`.
SPIFFS remains ready and lists README.md, song.u8, and speech_test.u8.
This confirms recovery of radio initialization; it is not a successful RF
transfer or a physical throughput result.

The previous local custom-PCB firmware uses CE=17, CSN=27, IRQ=16. The committed
Phase 1 baseline instead preserved CE=17, CSN=5, IRQ=27. On the user's direction,
the custom-PCB build flags, checked hardware profile, native expectations,
offline handoff profile, and current pin documentation now restore **CE=17,
CSN=27, IRQ=16**. The devboard remains **CE=27, CSN=5, IRQ=26**; both use
SCK=18, MOSI=23, MISO=19. The prior statement that the preserved committed pins
were sufficient for this laptop's custom PCB was incorrect.

Fresh validation passed 211 native tests, 16 Python tests (including the offline
handoff configuration checks), repository hygiene, whitespace checks, and both
canonical firmware builds. The corrected custom-PCB startup is now confirmed
by the user's status screenshot. Phase 2 and all later work remain paused at
the user's request. Earlier custom-PCB images with CSN=5/IRQ=27 are superseded.

### Integrated baseline - 2026-09-07

- Active development branch: `agent/rf3-pre-hardware-readiness`.
- Phase 1 firmware implementation: `6ba0315276a46c27e15a4b7cde044c1c8764d14c`
  (`Fix Phase 1 transfer reliability and status reporting`).
- The active branch was fast-forwarded through Phase 1 documentation revision
  `8698618` on 2026-09-07. Use `git log -3 --oneline` for this handoff update's
  final commit.
- Phase 1 is software-complete, integrated into the active checkout, and freshly
  validated. It has not been flashed to or exercised on hardware.
- The annotated rollback tag `rf3-before-phase-one-integration-20260907` points
  to pre-integration revision `aa911c3`.
- Both committed pin profiles, tracked Wi-Fi defaults, partitions, and board
  definitions were preserved then. The laptop-specific custom-PCB pin
  correction above supersedes the prior pinout assumption.
- Phases 2-7 are plans. The full browser file manager and SoftAP file workflow
  have not been implemented.
- This handoff refresh records integration and fresh validation; it changes no
  firmware source.

Use `git log -3 --oneline` for the latest documentation revision rather than
assuming the implementation commit above is also the current branch tip.
Confirm remote availability with the steps below; a local commit is not proof
that another computer has it.

Synchronization at this handoff: work is local; no remote push was performed.
Before this handoff commit, the active branch was three commits ahead of its
local `origin/agent/rf3-pre-hardware-readiness` tracking ref at `5300d40` and
zero commits behind. The handoff commit adds one more local commit. A local
tracking ref is not proof that the remote server has the work. Push explicitly
or create a fresh Git bundle before moving to another computer.

## Read in this order

1. [Detailed phase guide](firmware_roadmap.md) - all seven phases, dependencies,
   work packages, deliverables, failure cases, and completion gates.
2. [PDF phase guide](reports/RF3_Detailed_Phase_Guide.pdf) - portable reading copy.
3. [Phase 1 implementation and validation](phase_one_reliability.md).
4. [Board bring-up](board_bringup.md) and
   [qualification specification](firmware_qualification.md) for Phase 2.
5. [Historical optimization handoff](rf3_transfer_optimization_handoff.md) for
   earlier implementation context. Its branch/revision statements are historical.

## What Phase 1 changed

Invalid SPI snapshots no longer report TX success. Cleanup errors reach the
peer, and repeated cancellation retries failed cleanup. Four recent completed
transfers are remembered in RAM so delayed START frames do not reopen storage.
Serial/HTTP status uses owned snapshots under a separate lock, with sender
progress and periodic idle diagnostics. JSON strings are escaped, and the
repository hygiene failure from a machine-specific document path is fixed.

## Validation completed after integration

On 2026-09-07, the integrated active checkout passed **211 native tests**, **16
Python tests**, and **264 software qualification cases**. Qualification evidence
is under ignored path `.pio/qualification/run-0g43fdix`. Both canonical firmware
builds and both SPIFFS builds passed. The offline checker built and inspected
both board packages at `.pio/board_handoff/run-wh6j6pl5` and reported
`HOST_ARTIFACT_CHECKS_PASS`; it did not connect to, erase, or flash a board.

The ignored Wi-Fi-enabled custom-PCB validation environment compiled and linked
the HTTP status server using dummy validation-only credentials. Tracked Wi-Fi
remains disabled. Independent Python JSON decoding is included in the Python
suite. Repository hygiene and whitespace checks passed before integration and
are rerun for the final handoff commit.

These are recorded results from the Phase 1 session, not a fresh test run each
time this file is read. Live RF, power stability, ESP32 scheduling, and HTTP
responsiveness on the boards remain unverified. Reproduce checks after relevant
changes using the commands in the Phase 1 document.

## Next concrete work

The custom-PCB pin correction and radio startup verification are complete.
Retain the authoritative pin mapping above and await the user's next task.
Phase 2 and later work are paused. The user's requested future sequence is
Measurement Foundation, then the physical RF/power baseline; the older phase
guide's numbering and permission to overlap storage work do not override that
instruction. Do not begin storage, browser, protocol extensions, resume,
Bluetooth, or optimization during this correction.

## Resume on the laptop

If the active branch has not been pushed, create a fresh portable bundle on the
source computer after the final handoff commit:

```sh
git bundle create ../RF3_Phase_One.bundle agent/rf3-pre-hardware-readiness
```

Copy it beside an existing laptop clone, then verify and fetch it without
changing the laptop's active checkout:

```sh
git status --short
git bundle verify ../RF3_Phase_One.bundle
git fetch ../RF3_Phase_One.bundle refs/heads/agent/rf3-pre-hardware-readiness
git worktree add -b codex/phase-one-laptop ../rf3-phase-one-laptop FETCH_HEAD
```

Use an unused branch/directory name, or inspect and reuse an existing worktree.
Without an existing clone, use:

```sh
git clone -b agent/rf3-pre-hardware-readiness RF3_Phase_One.bundle rf3-phase-one-laptop
```

A bundle clone's origin points to the bundle; inspect `git remote -v` before
configuring a GitHub remote. The bundle does not include uncommitted files,
ignored build artifacts, or credentials. To recreate it on the source computer
after committing later work, rerun the bundle creation command above.

Alternatively, if the branch has subsequently been pushed, use the remote:

The repository remote is `https://github.com/Oddrubberband/RF3esp32-Code-.git`.
From an existing clone, inspect its working tree before switching anything:

```sh
git status --short
git fetch origin
git log -3 --oneline origin/agent/rf3-pre-hardware-readiness
```

If that remote branch is absent, the branch has not reached this clone's remote;
sync it from the source computer or transfer the repository before continuing.
To preserve the laptop's active checkout, create another worktree using an
unused local branch name:

```sh
git worktree add -b codex/phase-one-laptop ../rf3-phase-one-laptop origin/agent/rf3-pre-hardware-readiness
```

If the laptop worktree or branch already exists, inspect and reuse it rather
than deleting or resetting it. Open that worktree in Codex and say:

> Read AGENTS.md and docs/project_handoff.md, summarize the current state, and
> continue the next authorized task without changing another active checkout.

Python/PlatformIO dependencies must be installed on the laptop; local toolchain
paths and `.pio/` artifacts are not portable requirements. Use `README.md` and
`requirements.txt` for setup. Re-run relevant checks locally instead of assuming
that ignored logs or generated firmware were transferred with Git.

## Documentation maintenance

The 2026-09-07 documentation batch produced the eight-page phase guide and its
3,395-word Markdown counterpart, this handoff, README navigation, and standing
`AGENTS.md` instructions. All eight PDF pages were visually reviewed; the final
overview table was corrected and reviewed again. Phase 1 was subsequently
fast-forwarded into the active checkout and the full software validation matrix
above was rerun there. The pre-existing untracked `output/` directory was
preserved and is not part of the Phase 1 commits.

After meaningful work, update this state/next-action summary and the relevant
phase or subsystem document. Record passed and blocked checks honestly, link
portable deliverables, and keep changes committed with the work. Verify branch
synchronization before claiming the laptop can retrieve it. This is the user's
standing requirement, also recorded in the repository's `AGENTS.md`.

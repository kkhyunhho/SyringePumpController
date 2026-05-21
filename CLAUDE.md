# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Authority order

This project inherits from [coport-uni/CommonClaude/CLAUDE.md](https://github.com/coport-uni/CommonClaude/blob/2a8a597ec93132ef401b6f0e446255b6f65e5424/CLAUDE.md) (pinned at upstream commit `2a8a597`, 2026-05-19; previous pin `898ecf3`, 2026-05-18). **CommonClaude takes precedence** over anything in this file when the two conflict — the inverse of CommonClaude's own §1 "Rule Priority". This override was set by the project owner on 2026-05-18 (see ToDo.md §15 and issue #1) and the pin was advanced on 2026-05-21 (see ToDo.md §18 and issue #6).

Operational implications, in order of priority:

1. **CommonClaude/CLAUDE.md §1 Rule Priority** — local CLAUDE.md sections below override upstream when the two conflict (this is the project's documented inversion of upstream §1).
2. **CommonClaude/CLAUDE.md §2 MIT Code Convention** — applies to any C code in the repo (none today); Python code is governed by the equivalent specialization in the "Build, lint, test" section below (ruff line-length 80, ruff format, mypy strict).
3. **CommonClaude/CLAUDE.md §3 Debug File Management** — bench scripts live in [claude_test/](claude_test/) (production tests live in [tests/](tests/)); [claude_test/README.md](claude_test/README.md) indexes the directory.
4. **CommonClaude/CLAUDE.md §4 Task Management** — every task gets a `ToDo.md` append + `gh issue create` + working branch + PR. Read-only diagnose sessions are exempt per LearnedPatterns/W1.
5. **CommonClaude/CLAUDE.md §5 Testing Rules** — no magic numbers, no hardcoding to match test inputs; quality first.
6. **CommonClaude/CLAUDE.md §6 Linting** — `clang-format` / `cppcheck` for C; Python code uses the ruff/mypy specialization in "Build, lint, test" below.
7. **CommonClaude/CLAUDE.md §7 Research Before Coding** — consult docs/repo before guessing at unfamiliar APIs.
8. **CommonClaude/CLAUDE.md §8 Exceptions** — `claude_test/` is exempt from the 80-column limit and Doxygen blocks; ToDo.md checkbox flips are exempt from the append-only rule.
9. **CommonClaude/CLAUDE.md §9 Learned Patterns Reference** — consult [LearnedPatterns.md](LearnedPatterns.md) before drafting ToDo entries; cite as `(see LP §X)`; append new patterns post-task.
10. **CommonClaude/CLAUDE.md §10 Learned Patterns Bootstrap** — bootstrap has exited (the file exists). New entries use Problem / Cause / Fix / Rule. Grandfathered entries (G1–G6, Q1, W1–W6, E1–E4) retain their original format.
11. **CommonClaude/CLAUDE.md §11 Commit Messages** — Conventional Commits (`<type>(<scope>): <description>`, imperative mood, no trailing period). English only.
12. **CommonClaude/CLAUDE.md §12 Branching Strategy** — GitHub Flow; branches named `<type>/<short-description>` cut from `main`; deleted after merge.
13. **CommonClaude/CLAUDE.md §13 .gitignore** — base template is C-focused upstream; Python `.gitignore` in this repo is the language specialization.
14. **CommonClaude/CLAUDE.md §14 Versioning** — Semantic Versioning. Currently `0.2.0.dev0` — initial development; public API is unstable until `1.0.0`.
15. **CommonClaude/CLAUDE.md §15 Pull Request Guidelines** — PR title uses Conventional Commits format; description follows the Changes / Why / Testing / Related Issues template; keep PRs under 400 lines when possible.
16. **CommonClaude/CLAUDE.md §16 Git Automation** — `pre-commit` is documented but not currently installed in this repo; tracked as a future ToDo candidate.
17. **CommonClaude/CLAUDE.md §17 References** — primary specs (Conventional Commits, GitHub Flow, SemVer, .gitignore templates, pre-commit) are linked upstream.
18. **CommonClaude/LearnedPatterns.md format** — referenced from §10 above; project's local LP is independently scoped to SY-01B hardware specifics, not a mirror of upstream LP content.
19. **Hardware-specific rules in this file** (sections below) — the SY-01B protocol, error model, build/lint/test commands, and commit boundaries. These do not conflict with CommonClaude; they specialize it per §1 above.

Explicit project waivers from CommonClaude (must be cited when used):
- *(none — full compliance with CommonClaude as of issue #6, commit pin `2a8a597`)*

Upstream §2 (C naming) and §6 (clang-format/cppcheck) apply to any C code in the repo; Python code is governed by the equivalent specializations in the "Build, lint, test" block below (ruff, ruff format, mypy). This is **specialization**, not waiver — the upstream rule remains in force for any future C file.

## Repository status

Active development (`0.2.0.dev0`, pre-alpha). The driver lives at [src/sy01b/](src/sy01b/) as a single `SyringePumpController` class (consolidation §14); read-only commissioning + valve motion + plunger init + step moves + µL volume API are shipped and HIL-verified on `/dev/ttyUSB1`. Production unit tests live in [tests/](tests/) (121 tests, ~0.1 s); bench scripts that drive real hardware live in [claude_test/](claude_test/) per CommonClaude §3. See [README.md](README.md) for the current API surface, [DESIGN.md](DESIGN.md) for the architecture, and [ToDo.md](ToDo.md) for outstanding work.

## What this project will control

The target hardware is the **Runze SY-01B Smart Syringe Pump** (Nanjing Runze Fluid Control). It is a programmable precision-liquid-handling module driven over a serial bus from an external controller. The included [EUSB-30.pdf](EUSB-30.pdf) is the USB↔RS-232/RS-485 dongle that bridges a development host to the pump.

Key device facts every controller implementation must respect:

- **Power:** 24 V DC, ≥1.5 A, via DB-15 connector. Never connect/disconnect the pump while powered.
- **Communication:** RS-232 / RS-485 / CAN bus (non-isolated). The firmware auto-detects the interface. Default UART: **9600 bps, 8N1**; 38400 also supported via `[U47]`.
- **Address switch:** 16-position rotary (0–F). Positions 0–E → addresses `1`..`?` (ASCII), position **F is self-test**. Address must be set before powering on. Multi-pump bus supports up to 15 pumps.
- **Plunger resolution:** 30 mm stroke = **12 000 half-steps** in normal mode (`N0`), 96 000 micro-steps in fine/`N1` or micro/`N2` modes. Example: 5 mL syringe → 0.4167 µL/step.
- **Command buffer:** 255 chars. A new command sent without `[R]` overwrites the buffered one.

## Communication protocol overview

The SY-01B speaks **two top-level protocol families**, selectable via a binary configuration command (see [SY01BE.pdf](SY01BE.pdf) Appendix C "Switching Protocol"):

1. **RUNZE protocol** (binary, framed `91 EB …` with CRC). Query/switch commands are in the appendix.
2. **ASCII protocol**, with three sub-flavors auto-detected on first command:
   - **DT (Data Terminal)** — recommended for human/terminal debugging. Frame: `/<addr><cmds><CR>`. Reply: `/0<status><data><ETX><CR><LF>`. No checksum.
   - **OEM** — recommended for production. Frame: `STX <addr> <seq> <cmds> ETX <checksum>` where the checksum is XOR of all bytes except line-sync and the checksum itself. Sequence number supports retransmit.
   - **CAN** — 11-bit MID with direction/group/device/frame fields; group `2` = syringe pump. Frame types: `1` action, `2` common (single-byte cmds 0–4), `3`/`4` multi-frame, `6` query. Pumps emit a 100 ms boot-request loop on power-up until the host ACKs.

The firmware locks to whichever ASCII variant (DT vs OEM) it sees first until power cycle.

**Canonical first command after power-on / abnormal state: `/1ZR`** — initialize plunger and valve (CW polarity).

## SY-01B command set (essentials)

Commands are ASCII; uppercase plunger moves report "busy", lowercase report "ready". Most commands require a trailing `[R]` to execute; report commands and most control commands do not. Multi-command strings are valid: e.g. `IA6000OA0R` = input port → fill to 6000 → output port → empty to 0.

| Group | Commands | Notes |
|---|---|---|
| Init | `Z<n1,n2,n3>` / `Y<…>` (CCW) / `W<n1>` plunger-only / `w<n1,n2>` valve-only / `z` simulated | `n1`: 0=full, 1=half, 2=⅓ force; or 10–40 = speed code. `z` resets logical zero without moving — recovery from power loss, must be followed by full `Z`/`Y`. |
| Valve (non-distribution) | `I` input, `O` output, `B` bypass, `E` extra | Shortest-path. **In bypass, plunger moves error 11.** |
| Valve (distribution / `T-xx`) | `I<n>` CW to port n, `O<n>` CCW to port n | `B`/`E` are no-ops kept for back-compat. |
| Plunger move | `A<n>` absolute, `P<n>` relative pickup (down), `D<n>` relative dispense (up); lowercase `a`/`p`/`d` = "not busy" variant | Range 0–12000 (N0) or 0–96000 (N1/N2). Final position out of range → error 3. |
| Speed | `v<n>` start (1–1000), `V<n>` top (1–6000, hw allows 12000), `c<n>` cutoff (dispense only), `S<n>` preset 0–40, `L<n>` slope code 1–20, `K<n>` backlash, `k<n>` zero-gap offset, `N<n>` step mode | Must satisfy `v ≤ c ≤ V`. `V` can change on-the-fly mid-move; others cannot. |
| Control | `R` execute, `X` repeat last, `G<n>` loop (nested up to 10), `g` loop start, `M<n>` delay ms, `H<n>` halt (wait for `R` or TTL), `T` terminate | `T` does not terminate valve moves. After `T` on a plunger move, **re-initialize** — increments may be lost. |
| I/O | `J<n>` set 3 TTL outputs (n=0..7 bitmask), `?13`/`?14` read TTL inputs | Inputs on P11 pins 7/8; outputs on 13/14/15. |
| EEPROM programs | `s<n>` store (0–14, ≤128 chars each), `e<n>` execute, `U30` auto-run on power-on, `U31` clear auto-run | Address switch position selects which stored program runs in auto mode. |
| Reports (no `[R]`) | `Q` / `?29` status+error, `?` position, `?2` top speed, `?6` valve, `?10`/`F` buffer, `?76` config, `*` supply voltage | `Q` is **the only valid way to determine busy/ready in serial mode**. |

**Stall current must match syringe size** before reset (`U200,<n>` then `R`):

| Syringe | Command |
|---|---|
| 25 µL | `/1U200,4R` |
| 50 µL – 1.25 mL | `/1U200,5R` |
| 2.5 mL / 5 mL | `/1U200,6R` |

If initialization still fails due to internal pressure, bump `n` by +1.

The controller does **not** expose this as a public API — `U200,n` is a one-shot commissioning step that must match the physically installed syringe, and the project's bench currently runs a single fixed syringe size. Operators set stall current out-of-band (terminal, vendor tool, or a future `raw()` API) before running this code.

## Error model

The status byte returned in every reply has bit 5 = busy/ready and bits 0–3 = error code:

| Code | Meaning | Recovery |
|---|---|---|
| 1 | Init failed | Clear blockage, re-init. Pump rejects all commands until cleared by successful init. |
| 2 | Invalid command | Send a valid command. No re-init. |
| 3 | Invalid operand | Fix parameter. No re-init. |
| 7 | Device not initialized | Send `Z`/`Y`/`W`. |
| 9 | Plunger overload (backpressure) | **Must re-init** before any further move. |
| 10 | Valve overload | Sending another valve command re-homes the valve. Repeated → valve needs replacement. |
| 11 | Plunger move not allowed | Valve is in bypass — move valve off bypass first. |
| 15 | Command overflow | Sent a move while still moving. Wait for `Q` to report ready. |

Always poll `Q` between commands in serial mode; the answer-block status bit on non-`Q` replies is not reliable for busy/ready.

## Working with the manuals

The three PDFs are the only specs available. Extracted plain-text copies are kept alongside them so they can be grep'd quickly without re-running OCR:

- [SY01BE.pdf](SY01BE.pdf) / `SY01BE.txt` — v1.4 (2025-04-16), the **authoritative** SY-01B reference. Includes the RUNZE↔ASCII protocol-switch frames in Appendix C.
- [sy-01b-ascii-code-instruction-manuall-v1-1.pdf](sy-01b-ascii-code-instruction-manuall-v1-1.pdf) / `sy01b-ascii.txt` — v1.1, earlier and ASCII-protocol-only; use `SY01BE` if the two disagree.
- [EUSB-30.pdf](EUSB-30.pdf) / `EUSB-30.txt` — USB↔RS-232/RS-485 dongle wiring. RS-232 DIP pins: 5V/TXD/TXD/GND; RS-485 DIP pins: 5V/A+/B+/GND.

To regenerate the `.txt` files: `pdftotext -layout <file>.pdf` (requires `poppler-utils`).

## Build, lint, test

Stack: **Python ≥ 3.12**, **pyserial 3.x**, **DT ASCII protocol** (locked — never emit OEM frames from this codebase; the firmware locks to the first variant per power cycle). Architecture and rationale are in [DESIGN.md](DESIGN.md).

```bash
python3 -m venv .venv
.venv/bin/pip install -e ".[dev]"

.venv/bin/ruff check src tests claude_test main.py          # lint
.venv/bin/ruff format --check src tests claude_test main.py # format check
.venv/bin/mypy                                              # strict types on src/sy01b
.venv/bin/pytest                                            # full suite (103 tests)
.venv/bin/pytest tests/test_protocol.py::TestBuildCommand::test_plunger_init_frames  # single test
.venv/bin/pytest --cov=sy01b --cov-report=term-missing      # with coverage
```

The CLI is installed as `sy01b-diagnose` — a read-only commissioning probe. Run with `--help` for usage. It **never** sends `R`, `Z`, `Y`, or `W` (enforced by the test suite, not just convention).

Coverage: pure-logic paths in `src/sy01b/` (frame builder, parser, status decode, config, errors) sit at ~95 %. Motion methods and the I/O loop in `_send_and_receive` are intentionally **not** unit-tested — they are HIL-verified via [claude_test/](claude_test/) (LearnedPatterns W5). Overall raw coverage is therefore ~58 % post-consolidation (§14 removed the fake-pump layer; Path C tracks a possible re-introduction).

## Commit boundaries seen so far

- **Planning trio commit:** DESIGN.md, ToDo.md, LearnedPatterns.md.
- **Read-only API commit:** scaffolding + everything needed to open a port, run diagnose, retrieve software version (`?23`) and serial number (`?202`).
- **Valve motion commit** (`2cabf13`): non-distribution + distribution valve API (`initialize_valve`, `set_valve_position`, `move_valve_to_port`, `wait_until_ready`) targeting the MCC-4 dual-selection valve. `claude_test/valve_toggle.py` drives real port-to-port toggling against `/dev/ttyUSB1` (per CommonClaude §3, bench/debug scripts live in `claude_test/`, indexed in `claude_test/README.md`).
- **CommonClaude reconciliation commit** (`898ecf3`, closes #1): project subordinates itself to [coport-uni/CommonClaude](https://github.com/coport-uni/CommonClaude); `examples/` → `claude_test/` rename; line-length 100 → 80; LearnedPatterns E5/E6 reformatted to Problem/Cause/Fix/Rule.
- **Plunger init + step move commit** (`5d40437` + `8f7ac72`, closes #2): `initialize` (`Z<force>R` / `Y<force>R`, polls `?6 != "?"`) and `move_to_steps` (`A<n>R`, polls `?`). HIL-verified end-to-end at force=2 over a 125 µL syringe. `main.py` rewritten as an end-to-end tutorial covering every shipped public method; `claude_test/plunger_cycle.py` covers max/mid/min cycling. The µL-volume wrappers shipped later (see µL volume API commit below); the remaining plunger-side surface — `abort` + `requires_reinit` / `set_step_mode` / `raw()` — is intentionally absent, pinned by `TestNoPlungerMotionExposed` in `tests/test_plunger_motion_absent.py`.
- **Stall-current removal commit** (`a777af3`): `set_stall_current_for_syringe` (`U200,<n>R` EEPROM write) was shipped in commit `5d40437` but later removed — the bench runs a single fixed syringe size and the EEPROM is set out-of-band, so an in-driver helper added more risk than value (wrong `syringe_uL` would write a damaging stall current). `Config.syringe_uL` is retained for future µL↔step conversion. `claude_test/syringe_init.py` (which depended on it) was deleted in the same commit.
- **µL volume API commit** (closes #7): `aspirate_uL(target_uL)` and `dispense_uL(target_uL=0)` ship as thin wrappers over `move_to_steps` — both target an absolute *contained* volume and convert via `round(target_uL / Config.syringe_uL * full_stroke_steps)`. Both names share one wire frame; the split exists so the call site reads correctly (fill vs. drain). `Config.syringe_uL` default flipped `5000` → `125` to match the fixed bench syringe. `main.py` section 4 and `claude_test/plunger_cycle.py` rewritten to drive in µL; expected-step comparison verifies the rounding. Remaining plunger-side surface — `abort` + `requires_reinit` / `set_step_mode` / `raw()` — stays intentionally absent, pinned by `TestNoPlungerMotionExposed`.
- **FastAPI HTTP bridge server commit** (closes #10, Phase A of the ESP32 controller initiative): new `server/` top-level Python package exposing the driver over `/v1/*` JSON for a remote ESP32-S3 client. Endpoints: `health`, `diagnose`, `initialize`, `valve`, `aspirate`, `dispense`, `move_steps`, `prime`, `status`. Single uvicorn worker + `asyncio.Lock` preserves the driver's single-in-flight contract; long ops (prime ~30 s) block the issuing request — no job-id queue. `pyproject.toml` adds `[project.optional-dependencies].server`, ships `sy01b-server` console script, extends ruff/mypy targets. Public driver API surface is unchanged — server is a thin adapter (`aspirate_uL`, `dispense_uL`, `move_valve_to_port`, `diagnose`, `initialize`, queries). Driver exceptions map to HTTP status codes with stable JSON `{error, code, command, raw_reply_hex, message}` (no traceback leak). `tests/server/` (24 tests against an in-memory `FakePump`) brings the suite to 145 tests; existing 121 baseline unchanged. HIL on `/dev/ttyUSB1` pending. ESP32-S3 firmware client ships in later phase.
- **Firmware skeleton commit** (closes #12, Phase B of the ESP32 controller initiative, stacked on #11): new `firmware/` top-level ESP-IDF v5.3+ project for the ESP32-S3-BOX-3. `main/` split into 6 small modules — `main.c`, `wifi.{c,h}`, `config_store.{c,h}`, `pump_client.{c,h}`, `state.{c,h}`, `ui.{c,h}`. Boot flow: NVS → BSP (LVGL via `espressif/esp-box-3 ^4.0`) → UI (4-tab tabview, only Status live) → WiFi STA (auto-reconnect with backoff, Kconfig defaults + NVS override) → `GET /v1/diagnose` once → `GET /v1/status` every 2 s in a dedicated FreeRTOS task. **Read-only client — no motion endpoints called**, preserving the [HIL stays read-only] discipline through Phase B. Valve / Move / Prime tabs ship as "Phase C" placeholders. Server URL default `http://192.168.1.129:17046`. Repo gets a `.clang-format` (LLVM 80-col) for the first C code; `clang-format --dry-run --Werror` and `cppcheck --enable=warning,style` both clean. Build/flash recipe in `firmware/README.md`; `idf.py` flash performed by the user (out-of-band). Phase C wires the motion endpoints + Valve/Move/Prime tabs (planned ToDo §22).
- **Firmware motion UI commit** (closes #15, Phase C of the ESP32 controller initiative): `pump_client.{c,h}` adds six synchronous POST wrappers (`pump_initialize`, `pump_valve`, `pump_aspirate`, `pump_dispense`, `pump_move_steps`, `pump_prime`) plus a `pump_error_t` parser for the server's `{error, code, command, raw_reply_hex, message}` envelope and a recoverable-vs-fatal classifier mirroring `server/errors.py` (fatal = `PlungerOverloadError` code 9, `InitFailedError` code 1). `state.{c,h}` gains `APP_STATE_READY` / `BUSY` / `ERROR_FATAL` plus a `requires_reinit` latch that blocks motion until a fresh `initialize()` succeeds. A new `main.c` `pump_task` (8 KB stack, prio 5) owns a 4-deep `pump_cmd_t` queue; LVGL callbacks enqueue, the task pops one command at a time and posts the result back via `lv_async_call` — matches the server's `asyncio.Lock` single-in-flight contract. New small header `pump_task.h` carries the queue interface (UI imports this, not `main.c`). `ui.{c,h}` builds the full Valve / Move / Prime tabs and adds error/toast modals (`Retry`/`Dismiss` for recoverable, `Re-initialize` only for fatal); auto-retry once on `ValveOverloadError` since the server re-homes on next valve cmd (CLAUDE.md "Error model" code 10). Front BSP buttons wired: left = jump to Status tab, right = Initialize (active only in `NEEDS_INIT`). `clang-format --dry-run --Werror` and `cppcheck --enable=warning,style` both clean. Build/flash by user.

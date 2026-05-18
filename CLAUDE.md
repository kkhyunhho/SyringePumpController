# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository status

This repo is a **greenfield project** — no source code, build system, or commits exist yet. The working tree currently contains only the hardware/protocol manuals for the device being controlled. The first source code committed should determine language, build tool, and layout.

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

.venv/bin/ruff check src tests          # lint
.venv/bin/ruff format --check src tests # format check (no rewrites)
.venv/bin/mypy                          # strict types on src/sy01b
.venv/bin/pytest                        # full suite
.venv/bin/pytest tests/test_identity.py::TestIndividualIdentityProbes::test_serial_number_round_trips  # single test
.venv/bin/pytest --cov=sy01b --cov-report=term-missing                                                  # with coverage
```

The CLI is installed as `sy01b-diagnose` — a read-only commissioning probe. Run with `--help` for usage. It **never** sends `R`, `Z`, `Y`, or `W` (enforced by the test suite, not just convention).

Coverage targets: 90 % on `src/sy01b/` excluding `transport.py`'s real-serial paths (HIL only). Current: 87 % overall, ~95 % excluding transport.

## Commit boundaries seen so far

- **Planning trio commit:** DESIGN.md, ToDo.md, LearnedPatterns.md.
- **Read-only API commit:** scaffolding + everything needed to open a port, run diagnose, retrieve software version (`?23`) and serial number (`?202`).
- **Valve motion commit:** non-distribution valve API (`initialize_valve`, `set_valve_position`, `wait_until_ready`) targeting the MCC-4 dual-selection valve. `examples/valve_toggle.py` is the bench-verification script that drives real toggling against `/dev/ttyUSB1`. Plunger motion (`initialize`, `aspirate_uL`, `dispense_uL`, `abort`, `move_to_steps`, `set_step_mode`, `set_stall_current`) is intentionally still absent and pinned by `TestNoPlungerMotionExposed` in `tests/test_plunger_motion_absent.py`.

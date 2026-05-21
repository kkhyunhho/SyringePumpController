# claude_test/ — Bench scripts and one-off diagnostics

This directory holds debug, exploratory, and diagnostic scripts per [CommonClaude/CLAUDE.md §3](https://github.com/coport-uni/CommonClaude/blob/main/CLAUDE.md). Scripts here are NOT part of CI/CD; production-quality tests live in [`tests/`](../tests/).

When adding a new file, append a row to the table below describing what it does and the key finding(s) that came out of running it.

| File | Purpose | Findings |
|---|---|---|
| [valve_toggle.py](valve_toggle.py) | Toggle the SY-01B valve between two distribution ports (default: 1 ↔ 3), verifying each move by polling `?6`. Runs `diagnose()` first; plunger never moves. | 2026-05-18 HIL on `/dev/ttyUSB1` (firmware 8.33): 20/20 moves verified, ~907 ms per port-to-port transition. Confirmed [LearnedPatterns.md](../LearnedPatterns.md) E5 (Q.busy unreliable post-init) and E6 (firmware treats MCC-4 as 4-way distribution; `?6` returns digits). |
| [plunger_cycle.py](plunger_cycle.py) | After init, cycle the plunger through max (= syringe_uL) → mid (= syringe_uL/2) → min (= 0 µL) absolute contained volumes via `aspirate_uL` / `dispense_uL`, verifying each move's reported step against the expected conversion (`round(uL/syringe_uL * full_stroke)`). Default 3 cycles in NORMAL step mode. Syringe must be empty; valve must not be in bypass. | 2026-05-18 step-based variant HIL on `/dev/ttyUSB1` (physical 125 µL syringe, empty): **9/9 moves verified** across 3 cycles. Full stroke (0↔12 000) ≈ 3.26 s; half stroke ≈ 1.6 s — consistent with post-init V=4000 pps default ([LearnedPatterns.md](../LearnedPatterns.md) E8). _Volume-API HIL rerun pending after §19._ |
| [prime_line.py](prime_line.py) | Prime a tube by repeatedly aspirating from a source port (default 3) and dispensing through a sink port (default 1). Each cycle = `move_valve_to_port(src)` → full-stroke aspirate → `move_valve_to_port(sink)` → dispense to 0, with `?6`/`?` verification on every move. Composes existing API only; ends parked at sink port, plunger=0. | _Pending first HIL run — fill in after `python claude_test/prime_line.py --cycles 3` on `/dev/ttyUSB1`. Expected per-cycle: ~900 ms × 2 valve moves + ~3.26 s × 2 full strokes ≈ 8.3 s/cycle._ |

**Historical note (2026-05-18):** `syringe_init.py` (capacity-sweep stall-current demo) was removed alongside the `set_stall_current_for_syringe()` API — see [ToDo.md §17](../ToDo.md). The HIL finding it surfaced (U200 is EEPROM-only and per-session init uses the prior-power-up value, SY01BE.txt:1622) is still valid as a hardware fact.

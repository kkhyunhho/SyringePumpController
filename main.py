"""End-to-end demo of every SyringePumpController operation in one script.

Hardware assumption: 125 µL syringe at address 1 on /dev/ttyUSB1, 4-way
distribution valve (firmware 8.33). The syringe must be **empty** and
fluid lines open to atmosphere — the plunger moves through its full
stroke and the valve homes / toggles between ports.

The script walks through every public method on `SyringePumpController`
that the controller currently ships, in the order an operator would
naturally invoke them. Each section is self-contained — comment out any
section to skip it.

    1. Open + diagnose            (read-only)
    2. Identity / status queries  (read-only)
    3. Stall current              (EEPROM, no motion)
    4. initialize()               (Z = plunger + valve home)
    5. move_to_steps()            (plunger absolute moves)
    6. move_valve_to_port()       (valve distribution moves)

Methods deliberately not shown here:
- `initialize_valve(home_port, direction_ccw)` — valve-only init; a
  subset of step 4. Use it on a valveless pump or when only the valve
  needs re-homing.
- `set_valve_position(I/O/B/E)` — for non-distribution valves; this
  bench's pump reports as 4-way (LearnedPatterns E6), so distribution
  syntax via `move_valve_to_port` is the right call.
- `wait_until_ready` — kept for parity with the manual but unreliable on
  firmware 8.33 (LearnedPatterns E5). Position polling is the workaround.

For narrower per-feature bench scripts, see claude_test/.
"""

from __future__ import annotations

import logging
import sys
import time

from sy01b import SyringePumpController


def main() -> int:
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)-7s %(name)s: %(message)s",
        stream=sys.stderr,
    )

    cfg = SyringePumpController.Config(
        port="/dev/ttyUSB1",
        address=1,
        baud=9600,
        syringe_uL=125,
        step_mode=SyringePumpController.StepMode.NORMAL,
        reply_timeout_s=2.0,
    )

    with SyringePumpController.open(cfg) as pump:
        # --- 1. Diagnose (read-only commissioning probe) -----------------
        # Sends only side-effect-free queries (Q, ?23, ?202, ?76, *, ?6, ?).
        # Refuses to proceed if the supply rail is low or the pump replies
        # with an unexpected error code. ALWAYS run before motion (W1 rule).
        report = pump.diagnose()
        print(report.render())
        if not report.ok_to_initialize:
            print("Pump not safe to drive — aborting.", file=sys.stderr)
            return 2

        # --- 2. Identity / status queries -------------------------------
        # Each of these can be called independently at any time; none
        # require initialization. Use them for logging, health checks, or
        # post-motion verification.
        print()
        print("Identity & status:")
        print(f"  ?23   software version : {pump.query_software_version()}")
        print(f"  ?202  serial number    : {pump.query_serial_number()}")
        print(f"  ?76   config blob      : {pump.query_config()}")
        print(
            f"  *     supply voltage   : {pump.query_supply_voltage_v():.1f} V"
        )
        status = pump.query_status()
        print(
            f"  Q     status byte      : busy={status.busy} "
            f"error={status.error.name}"
        )
        print(f"  ?6    valve position   : {pump.query_valve_position()!r}")
        print(
            f"  ?     plunger position : {pump.query_plunger_position()} steps"
        )

        # --- 3. Stall current (EEPROM, no plunger motion) ---------------
        # Persists across power cycles; takes effect on the NEXT power-up.
        # Operand is derived from cfg.syringe_uL via the manual's table
        # (25 µL -> 4, 50 µL-1.25 mL -> 5, 2.5/5 mL -> 6).
        print()
        print(f"Setting stall current: U200,{cfg.stall_current_operand()}R")
        pump.set_stall_current_for_syringe()

        # --- 4. Initialize plunger + valve (Z) --------------------------
        # Mechanically homes both. Plunger ends at step 0; valve homes to
        # its assigned input port. force=2 (third) is the conservative pick
        # for the 125 µL bench syringe. Z (CW) vs Y (CCW) is a per-rig
        # decision; ccw=False is correct for the user's bench. After Z,
        # speed parameters (v/V/c/S/L) reset to defaults — V=4000 pps top
        # speed on firmware 8.33 (LearnedPatterns E8).
        print()
        print("Initializing (Z2R, third force, CW)...")
        t0 = time.monotonic()
        pump.initialize(force=2)
        print(
            f"  init complete in {time.monotonic() - t0:.2f} s — "
            f"?={pump.query_plunger_position()} "
            f"?6={pump.query_valve_position()!r}"
        )

        # --- 5. Plunger absolute moves (A<n>R) --------------------------
        # move_to_steps(n) sends A<n>R and polls ? until the reported
        # position equals n. Range: 0..cfg.step_mode.full_stroke_steps
        # (12 000 in NORMAL/N0). For 125 µL, each half-step ≈ 0.0104 µL.
        stroke = cfg.step_mode.full_stroke_steps
        print()
        print(f"Plunger absolute moves (stroke = {stroke} half-steps):")
        targets = [
            ("max", stroke),  # full aspirate (~125 µL drawn)
            ("mid", stroke // 2),  # half aspirate (~62.5 µL drawn)
            ("min", 0),  # fully dispensed (post-init home)
        ]
        for label, steps in targets:
            t0 = time.monotonic()
            pump.move_to_steps(steps)
            dt_ms = (time.monotonic() - t0) * 1000.0
            print(
                f"  → {label} ({steps:>5} steps)  {dt_ms:6.1f} ms  "
                f"now ?={pump.query_plunger_position()}"
            )

        # --- 6. Valve distribution moves (I<n>R / O<n>R) ----------------
        # move_valve_to_port(n) uses CW (I<n>R) by default; pass
        # direction_ccw=True to force CCW (O<n>R). For the user's MCC-4 on
        # a 4-way-configured pump, the two physical states correspond to
        # ports 1 (C-1) and 3 (C-3) — LearnedPatterns E6.
        print()
        print("Valve distribution moves (MCC-4: C-1 ↔ C-3 ↔ C-1):")
        for port in (1, 3, 1):
            t0 = time.monotonic()
            pump.move_valve_to_port(port)
            dt_ms = (time.monotonic() - t0) * 1000.0
            print(
                f"  → port {port}  {dt_ms:6.1f} ms  "
                f"?6={pump.query_valve_position()!r}"
            )
            time.sleep(0.3)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

"""Identity probe + 2-cycle valve toggle (port 1 ↔ port 3) on a real SY-01B pump.

Hardware assumption: 125 uL syringe at address 1 on /dev/ttyUSB1, 4-way valve.
Reads ?23 (software version) and ?202 (serial number), then homes the valve and
toggles between port 1 and port 3 twice with ?6 verification. Plunger is NOT moved.
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
        software_version = pump.query_software_version()
        serial_number = pump.query_serial_number()
        print(f"Software version: {software_version}")
        print(f"Serial number   : {serial_number}")

        pump.initialize_valve(home_port=1, direction_ccw=False)
        print(f"Valve initialized at: {pump.query_valve_position()!r}")

        for cycle in range(2):
            for target in (1, 3):
                pump.move_valve_to_port(target)
                observed = pump.query_valve_position().strip()
                ok = observed == str(target)
                print(
                    f"  cycle {cycle} → port {target}  ?6={observed!r}  "
                    f"{'OK' if ok else 'MISMATCH'}"
                )
                time.sleep(0.5)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

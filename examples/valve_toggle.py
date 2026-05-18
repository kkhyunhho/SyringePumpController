"""Toggle a 4-way distribution valve between two ports, verifying each move via ?6.

Bench script — runs against real hardware on /dev/ttyUSB1. The plunger is NOT moved
(valve-only init via `w<port>,<dir>R`). Runs `diagnose()` first per LearnedPatterns W1,
then loops N cycles toggling between two distribution ports with `?6` verification.

The HIL pump (firmware 8.33) reports its valve as "4 way" in `?76` and answers `?6`
with ASCII digits 1..4 — i.e. it operates in distribution mode regardless of the
attached MCC-4's physical 2-state behavior. The MCC-4 dual-selection states (C-1 and
C-3 connected) correspond to distribution ports 1 and 3 respectively on this firmware.

Usage:
    /opt/conda/envs/syringe/bin/python examples/valve_toggle.py
    /opt/conda/envs/syringe/bin/python examples/valve_toggle.py --cycles 5 --delay-s 1.0 -v
    /opt/conda/envs/syringe/bin/python examples/valve_toggle.py --port-a 1 --port-b 3

Exit codes:
    0 — all cycles verified
    1 — at least one mismatch between commanded and observed valve position
    2 — diagnose() reported the pump is not safe to drive
"""

from __future__ import annotations

import argparse
import logging
import sys
import time

from sy01b import SyringePumpController


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="valve_toggle", description=__doc__.splitlines()[0])
    parser.add_argument(
        "--cycles", type=int, default=10, help="Number of A→B→A cycles (default 10)."
    )
    parser.add_argument(
        "--delay-s",
        type=float,
        default=0.5,
        dest="delay_s",
        help="Seconds to sleep between moves (default 0.5).",
    )
    parser.add_argument(
        "--port-a", type=int, default=1, dest="port_a", help="First toggle port (default 1)."
    )
    parser.add_argument(
        "--port-b", type=int, default=3, dest="port_b", help="Second toggle port (default 3)."
    )
    parser.add_argument("--port", default="/dev/ttyUSB1")
    parser.add_argument("--address", type=int, default=1)
    parser.add_argument("--baud", type=int, default=9600)
    parser.add_argument("--syringe-uL", type=int, dest="syringe_uL", default=125)
    parser.add_argument("--reply-timeout-s", type=float, dest="reply_timeout_s", default=2.0)
    parser.add_argument("-v", "--verbose", action="store_true")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)-7s %(name)s: %(message)s",
        stream=sys.stderr,
    )

    cfg = SyringePumpController.Config(
        port=args.port,
        address=args.address,
        baud=args.baud,
        syringe_uL=args.syringe_uL,
        step_mode=SyringePumpController.StepMode.NORMAL,
        reply_timeout_s=args.reply_timeout_s,
    )

    targets = [args.port_a, args.port_b]

    with SyringePumpController.open(cfg) as pump:
        report = pump.diagnose()
        print(report.render(), file=sys.stderr)
        if not report.ok_to_initialize:
            print("diagnose() reports the pump is NOT safe to drive — aborting.", file=sys.stderr)
            return 2

        print(f"initializing valve (home_port={args.port_a}, CW) ...", file=sys.stderr)
        pump.initialize_valve(home_port=args.port_a, direction_ccw=False)
        start_raw = pump.query_valve_position()
        print(f"post-init valve position: {start_raw!r}", file=sys.stderr)

        mismatches = 0
        total = args.cycles * len(targets)
        for cycle in range(args.cycles):
            for target_port in targets:
                t0 = time.monotonic()
                pump.move_valve_to_port(target_port)
                elapsed_ms = (time.monotonic() - t0) * 1000.0
                raw = pump.query_valve_position()
                ok = raw.strip() == str(target_port)
                mismatches += int(not ok)
                status = "OK" if ok else "MISMATCH"
                print(
                    f"cycle {cycle:>3} → port {target_port}  ?6={raw!r:>6}  "
                    f"{elapsed_ms:6.1f} ms  {status}"
                )
                time.sleep(args.delay_s)

    print(
        f"\nsummary: {total - mismatches}/{total} moves verified, {mismatches} mismatch(es)",
        file=sys.stderr,
    )
    return 1 if mismatches else 0


if __name__ == "__main__":
    raise SystemExit(main())

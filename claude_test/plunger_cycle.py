"""Cycle the plunger through max (full) → middle → min volumes.

Bench script — runs against real hardware on /dev/ttyUSB1. After running
`diagnose()` and `initialize(force=2)`, the plunger loops between three
absolute contained volumes driven by aspirate_uL / dispense_uL:

    max = syringe_uL     (full aspirate, syringe fully drawn)
    mid = syringe_uL / 2 (half aspirate)
    min = 0              (fully dispensed, post-init home)

Each move's reported step position is compared to the expected step
position derived from the same volume → step conversion the driver uses
internally (round(uL / syringe_uL * full_stroke)). A mismatch flags a
rounding-boundary or polling-termination issue.

Wall-clock elapsed time is recorded per move. The valve must NOT be in
bypass; after initialize() it sits at the default input port, so plunger
motion draws/expels air through that port. Do not run with fluid lines
under pressure.

Usage:
    /opt/conda/envs/syringe/bin/python claude_test/plunger_cycle.py
    /opt/conda/envs/syringe/bin/python claude_test/plunger_cycle.py --cycles 5
    /opt/conda/envs/syringe/bin/python claude_test/plunger_cycle.py --delay-s 1.0 -v

Exit codes:
    0 — all moves verified
    1 — at least one move's reported position did not match the target
    2 — diagnose() reported the pump is not safe to drive
"""

from __future__ import annotations

import argparse
import logging
import sys
import time

from sy01b import SyringePumpController


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="plunger_cycle", description=__doc__.splitlines()[0]
    )
    parser.add_argument(
        "--cycles",
        type=int,
        default=3,
        help="Number of max-mid-min cycles (default 3).",
    )
    parser.add_argument(
        "--delay-s",
        type=float,
        default=0.5,
        dest="delay_s",
        help="Seconds to sleep between moves (default 0.5).",
    )
    parser.add_argument(
        "--force",
        type=int,
        default=2,
        choices=[0, 1, 2],
        help="Z init force code (default 2 for 125 µL bench syringe).",
    )
    parser.add_argument(
        "--settle-timeout-s",
        type=float,
        default=10.0,
        dest="settle_timeout_s",
        help="Max seconds to wait for each move to settle (default 10).",
    )
    parser.add_argument("--port", default="/dev/ttyUSB1")
    parser.add_argument("--address", type=int, default=1)
    parser.add_argument("--baud", type=int, default=9600)
    parser.add_argument(
        "--syringe-uL", type=int, dest="syringe_uL", default=125
    )
    parser.add_argument(
        "--reply-timeout-s",
        type=float,
        dest="reply_timeout_s",
        default=2.0,
    )
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
    full_stroke = cfg.step_mode.full_stroke_steps
    targets: list[tuple[str, float]] = [
        ("max", float(cfg.syringe_uL)),
        ("mid", cfg.syringe_uL / 2),
        ("min", 0.0),
    ]

    def expected_steps(target_uL: float) -> int:
        return round(target_uL / cfg.syringe_uL * full_stroke)

    with SyringePumpController.open(cfg) as pump:
        report = pump.diagnose()
        print(report.render(), file=sys.stderr)
        if not report.ok_to_initialize:
            print(
                "diagnose() reports the pump is NOT safe to drive — aborting.",
                file=sys.stderr,
            )
            return 2

        print(
            f"initializing: Z{args.force}R (CW, force={args.force}) ...",
            file=sys.stderr,
        )
        t0 = time.monotonic()
        pump.initialize(force=args.force)
        init_elapsed = time.monotonic() - t0
        print(
            f"init done in {init_elapsed:.2f} s — plunger at "
            f"{pump.query_plunger_position()}, valve at "
            f"{pump.query_valve_position()!r}",
            file=sys.stderr,
        )

        mismatches = 0
        total = args.cycles * len(targets)
        for cycle in range(args.cycles):
            for label, target_uL in targets:
                t0 = time.monotonic()
                if label == "min":
                    pump.dispense_uL(settle_timeout_s=args.settle_timeout_s)
                else:
                    pump.aspirate_uL(
                        target_uL, settle_timeout_s=args.settle_timeout_s
                    )
                elapsed_ms = (time.monotonic() - t0) * 1000.0
                pos = pump.query_plunger_position()
                want = expected_steps(target_uL)
                ok = pos == want
                mismatches += int(not ok)
                status = "OK" if ok else "MISMATCH"
                print(
                    f"cycle {cycle:>2} → {label:>3} "
                    f"({target_uL:>6.1f} µL, want={want:>5} steps)  "
                    f"?={pos:>5}  {elapsed_ms:7.1f} ms  {status}"
                )
                time.sleep(args.delay_s)

    print(
        f"\nsummary: {total - mismatches}/{total} moves verified, "
        f"{mismatches} mismatch(es)",
        file=sys.stderr,
    )
    return 1 if mismatches else 0


if __name__ == "__main__":
    raise SystemExit(main())

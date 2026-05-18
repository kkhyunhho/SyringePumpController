"""Initialize the syringe pump (Z<force>R) and verify completion via `?`.

Bench script — runs against real hardware on /dev/ttyUSB1. Sets the EEPROM
stall current for the configured syringe (`U200,<n>R`) and then sends `Z<force>R`
to home both plunger and valve. Polls `?` (plunger position) until it reads 0
rather than `Q.busy`, which is unreliable on firmware 8.33 (LearnedPatterns E5).

The 125 µL bench syringe is unspecified in the manual's force table (lines
1648-1652 jump from 50/100 µL=third to 250/500 µL=half), so this script
defaults to third (force=2) — the conservative pick for the smaller-end
neighbour. Override with --force if needed.

**Safety**: Z homes the plunger across its full stroke. Run only with the
syringe empty and with the fluid lines disconnected or open to atmosphere.

Usage:
    /opt/conda/envs/syringe/bin/python claude_test/syringe_init.py
    /opt/conda/envs/syringe/bin/python claude_test/syringe_init.py --force 1
    /opt/conda/envs/syringe/bin/python claude_test/syringe_init.py -v

Exit codes:
    0 — init reported success and plunger settled to position 0
    1 — init succeeded but post-init `?` was non-zero (should not happen)
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
        prog="syringe_init", description=__doc__.splitlines()[0]
    )
    parser.add_argument(
        "--force",
        type=int,
        default=2,
        choices=[0, 1, 2],
        help=(
            "Z init force code: 0=full, 1=half, 2=third (default 2 for "
            "125 µL bench syringe)."
        ),
    )
    parser.add_argument(
        "--ccw",
        action="store_true",
        help="Send Y instead of Z (valve homes counter-clockwise).",
    )
    parser.add_argument(
        "--settle-timeout-s",
        type=float,
        default=30.0,
        dest="settle_timeout_s",
        help="Max wall-clock seconds to wait for plunger to reach 0.",
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

    with SyringePumpController.open(cfg) as pump:
        report = pump.diagnose()
        print(report.render(), file=sys.stderr)
        if not report.ok_to_initialize:
            print(
                "diagnose() reports the pump is NOT safe to drive — aborting.",
                file=sys.stderr,
            )
            return 2

        pre_plunger = pump.query_plunger_position()
        pre_valve = pump.query_valve_position()
        print(
            f"pre-init  ?={pre_plunger}  ?6={pre_valve!r}",
            file=sys.stderr,
        )

        operand = cfg.stall_current_operand()
        print(
            f"setting stall current: U200,{operand}R "
            f"(for {cfg.syringe_uL} µL syringe)",
            file=sys.stderr,
        )
        pump.set_stall_current_for_syringe()

        direction = "CCW" if args.ccw else "CW"
        cmd_letter = "Y" if args.ccw else "Z"
        print(
            f"initializing: {cmd_letter}{args.force}R ({direction}, "
            f"force={args.force}) ...",
            file=sys.stderr,
        )
        t0 = time.monotonic()
        pump.initialize(
            force=args.force,
            ccw=args.ccw,
            settle_timeout_s=args.settle_timeout_s,
        )
        elapsed_s = time.monotonic() - t0

        post_plunger = pump.query_plunger_position()
        post_valve = pump.query_valve_position()
        print(
            f"post-init ?={post_plunger}  ?6={post_valve!r}  "
            f"elapsed={elapsed_s:.2f} s",
            file=sys.stderr,
        )

        if post_plunger != 0:
            print(
                f"WARNING: plunger position is {post_plunger}, expected 0",
                file=sys.stderr,
            )
            return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

"""Defensive test guarding the milestone boundary (LearnedPatterns W4).

Valve motion (initialize_valve, set_valve_position, wait_until_ready) has shipped,
but plunger motion methods are still intentionally absent. If a future commit
accidentally adds e.g. ``aspirate_uL`` before the plunger-motion HIL plan is ready,
this test fails loudly rather than letting the half-finished method ship.
"""

from __future__ import annotations

import pytest

from sy01b import SyringePumpController


class TestNoPlungerMotionExposed:
    @pytest.mark.parametrize(
        "attr",
        [
            "initialize",
            "aspirate_uL",
            "dispense_uL",
            "abort",
            "move_to_steps",
            "set_step_mode",
            "set_stall_current",
        ],
    )
    def test_attr_absent(self, attr: str) -> None:
        assert not hasattr(SyringePumpController, attr), (
            f"{attr} appeared on the public API — was a plunger-motion commit landed prematurely?"
        )


class TestValveMotionPresent:
    """Positive guard: the valve-motion surface this milestone ships must be present."""

    @pytest.mark.parametrize(
        "attr",
        [
            "initialize_valve",
            "set_valve_position",
            "move_valve_to_port",
            "wait_until_ready",
        ],
    )
    def test_attr_present(self, attr: str) -> None:
        assert hasattr(SyringePumpController, attr), (
            f"{attr} missing from the public API — was a valve-motion method removed?"
        )

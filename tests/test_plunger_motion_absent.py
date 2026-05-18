"""Defensive test guarding the milestone boundary (LearnedPatterns W4).

Plunger initialization (``initialize``, ``set_stall_current_for_syringe``)
and absolute step-based motion (``move_to_steps``) have shipped. Volume-
based aspirate/dispense, abort, and step-mode reconfiguration are still
intentionally absent — if a future commit accidentally adds one before
its HIL plan is ready, this test fails loudly rather than letting a
half-finished method ship.
"""

from __future__ import annotations

import pytest

from sy01b import SyringePumpController


class TestNoPlungerMotionExposed:
    @pytest.mark.parametrize(
        "attr",
        [
            "aspirate_uL",
            "dispense_uL",
            "abort",
            "set_step_mode",
        ],
    )
    def test_attr_absent(self, attr: str) -> None:
        assert not hasattr(SyringePumpController, attr), (
            f"{attr} appeared on the public API — was a plunger-motion "
            f"commit landed prematurely?"
        )


class TestValveMotionPresent:
    """Positive guard: the valve-motion surface this milestone ships
    must be present."""

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
            f"{attr} missing from the public API — was a valve-motion "
            f"method removed?"
        )


class TestPlungerInitPresent:
    """Positive guard: the plunger-init + step-move surface this
    milestone ships must be present."""

    @pytest.mark.parametrize(
        "attr",
        [
            "initialize",
            "set_stall_current_for_syringe",
            "move_to_steps",
        ],
    )
    def test_attr_present(self, attr: str) -> None:
        assert hasattr(SyringePumpController, attr), (
            f"{attr} missing from the public API — was a plunger method "
            f"removed?"
        )

"""Pure tests for the ValvePosition StrEnum. No I/O, no transport."""

from __future__ import annotations

import pytest

from sy01b import SyringePumpController

ValvePosition = SyringePumpController.ValvePosition


class TestValvePositionMembers:
    @pytest.mark.parametrize(
        ("member", "value"),
        [
            (ValvePosition.INPUT, "I"),
            (ValvePosition.OUTPUT, "O"),
            (ValvePosition.BYPASS, "B"),
            (ValvePosition.EXTRA, "E"),
        ],
    )
    def test_value(self, member: ValvePosition, value: str) -> None:
        assert member.value == value

    def test_str_construction(self) -> None:
        assert ValvePosition("I") is ValvePosition.INPUT
        assert ValvePosition("O") is ValvePosition.OUTPUT

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


class TestFromQueryReply:
    @pytest.mark.parametrize(
        ("reply", "expected"),
        [
            ("i", ValvePosition.INPUT),
            ("o", ValvePosition.OUTPUT),
            ("b", ValvePosition.BYPASS),
            ("e", ValvePosition.EXTRA),
            ("I", ValvePosition.INPUT),
            ("O", ValvePosition.OUTPUT),
            ("  i\r\n  ", ValvePosition.INPUT),
        ],
    )
    def test_valid(self, reply: str, expected: ValvePosition) -> None:
        assert ValvePosition.from_query_reply(reply) is expected

    @pytest.mark.parametrize("reply", ["?", "", "   ", "\r\n"])
    def test_pre_init_returns_none(self, reply: str) -> None:
        # LearnedPatterns E3: pre-init the pump returns the literal '?' for ?6.
        assert ValvePosition.from_query_reply(reply) is None

    @pytest.mark.parametrize("reply", ["X", "1", "io"])
    def test_unknown_raises(self, reply: str) -> None:
        with pytest.raises(ValueError, match="is not a valid"):
            ValvePosition.from_query_reply(reply)

"""Regression: server/pump.toml.example must parse via Config.from_toml.

Catches drift between the example file and the StepMode enum's wire-code
values (e.g. someone writing the member name "NORMAL" instead of the
wire value "N0"). See issue #1.
"""

from __future__ import annotations

from pathlib import Path

from sy01b import SyringePumpController

EXAMPLE_PATH = (
    Path(__file__).resolve().parents[2] / "server" / "pump.toml.example"
)


class TestPumpTomlExample:
    def test_example_file_exists(self) -> None:
        assert EXAMPLE_PATH.is_file(), f"missing: {EXAMPLE_PATH}"

    def test_example_parses_without_error(self) -> None:
        SyringePumpController.Config.from_toml(EXAMPLE_PATH)

    def test_example_step_mode_resolves_to_enum_member(self) -> None:
        cfg = SyringePumpController.Config.from_toml(EXAMPLE_PATH)
        assert cfg.step_mode is SyringePumpController.StepMode.NORMAL

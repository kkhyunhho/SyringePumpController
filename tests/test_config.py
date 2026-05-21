"""Tests for SyringePumpController.Config — validation and TOML loading."""

from __future__ import annotations

from pathlib import Path

import pytest

from sy01b import SyringePumpController


class TestValidation:
    def test_defaults_accepted(self) -> None:
        cfg = SyringePumpController.Config(port="/dev/ttyUSB0")
        assert cfg.address == 1
        assert cfg.baud == 9600
        assert cfg.syringe_uL == 125
        assert cfg.step_mode is SyringePumpController.StepMode.NORMAL

    @pytest.mark.parametrize("bad_addr", [0, -1, 16, 100])
    def test_address_out_of_range_raises(self, bad_addr: int) -> None:
        with pytest.raises(ValueError, match="address"):
            SyringePumpController.Config(port="x", address=bad_addr)

    def test_unsupported_syringe_raises(self) -> None:
        with pytest.raises(ValueError, match="syringe_uL"):
            SyringePumpController.Config(port="x", syringe_uL=750)

    @pytest.mark.parametrize(
        "syr", sorted(SyringePumpController.ALLOWED_SYRINGES_UL)
    )
    def test_every_allowed_syringe_is_acceptable(self, syr: int) -> None:
        cfg = SyringePumpController.Config(port="x", syringe_uL=syr)
        assert cfg.syringe_uL == syr

    def test_invalid_baud_raises(self) -> None:
        with pytest.raises(ValueError, match="baud"):
            SyringePumpController.Config(port="x", baud=115200)

    def test_zero_timeout_raises(self) -> None:
        with pytest.raises(ValueError, match="reply_timeout_s"):
            SyringePumpController.Config(port="x", reply_timeout_s=0)


class TestStepMode:
    def test_normal_stroke(self) -> None:
        assert SyringePumpController.StepMode.NORMAL.full_stroke_steps == 12_000

    def test_fine_stroke(self) -> None:
        assert SyringePumpController.StepMode.FINE.full_stroke_steps == 96_000

    def test_micro_stroke(self) -> None:
        assert SyringePumpController.StepMode.MICRO.full_stroke_steps == 96_000


class _NeverUsedTransport:
    """Stub Transport for tests that exercise pure-logic methods only."""

    is_open = True

    def read(self, size: int = 1, /) -> bytes:
        raise AssertionError("read() must not be called in pure-logic tests")

    def write(self, data: bytes, /) -> int:
        raise AssertionError("write() must not be called in pure-logic tests")

    def flush(self) -> None:
        raise AssertionError("flush() must not be called in pure-logic tests")

    def reset_input_buffer(self) -> None:
        raise AssertionError(
            "reset_input_buffer() must not be called in pure-logic tests"
        )

    def close(self) -> None:
        self.is_open = False


def _pump(syringe_uL: int, step_mode: SyringePumpController.StepMode):
    cfg = SyringePumpController.Config(
        port="x", syringe_uL=syringe_uL, step_mode=step_mode
    )
    return SyringePumpController(_NeverUsedTransport(), cfg)


class TestVolumeToStepsConversion:
    """``_uL_to_steps`` is private but worth pinning — it's the math that
    governs every aspirate/dispense call."""

    @pytest.mark.parametrize(
        ("syringe_uL", "step_mode", "volume_uL", "expected_steps"),
        [
            # Exact division: 5000 µL / 12 000 steps grid
            (5000, SyringePumpController.StepMode.NORMAL, 0, 0),
            (5000, SyringePumpController.StepMode.NORMAL, 2500, 6000),
            (5000, SyringePumpController.StepMode.NORMAL, 5000, 12000),
            # Exact division: 125 µL / 12 000 steps grid (96 steps per µL)
            (125, SyringePumpController.StepMode.NORMAL, 62.5, 6000),
            (125, SyringePumpController.StepMode.NORMAL, 125, 12000),
            # Fine mode (N1) — 96 000-step grid
            (5000, SyringePumpController.StepMode.FINE, 2500, 48000),
            (5000, SyringePumpController.StepMode.FINE, 5000, 96000),
            # Rounding: 0.1 µL on 125 µL syringe → 0.1 * 12000 / 125 = 9.6 → 10
            (125, SyringePumpController.StepMode.NORMAL, 0.1, 10),
            # Rounding banker's-default-doesn't-matter case: 9.4 → 9
            (125, SyringePumpController.StepMode.NORMAL, 9.4 / 96, 9),
        ],
    )
    def test_conversion(
        self,
        syringe_uL: int,
        step_mode: SyringePumpController.StepMode,
        volume_uL: float,
        expected_steps: int,
    ) -> None:
        pump = _pump(syringe_uL, step_mode)
        assert pump._uL_to_steps(volume_uL) == expected_steps

    @pytest.mark.parametrize("bad", [-1, -0.1, 5001, 5000.5])
    def test_out_of_range_raises(self, bad: float) -> None:
        pump = _pump(5000, SyringePumpController.StepMode.NORMAL)
        with pytest.raises(ValueError, match="volume_uL"):
            pump._uL_to_steps(bad)


class TestVolumeAPIDelegation:
    """``aspirate_uL`` / ``dispense_uL`` are thin wrappers; they must
    delegate to ``move_to_steps`` with the conversion result and must
    surface the conversion's ``ValueError`` before any I/O happens."""

    def test_aspirate_delegates_with_converted_steps(self) -> None:
        pump = _pump(5000, SyringePumpController.StepMode.NORMAL)
        captured: dict[str, object] = {}

        def fake(steps: int, **kwargs: object) -> None:
            captured["steps"] = steps
            captured["kwargs"] = kwargs

        pump.move_to_steps = fake  # type: ignore[method-assign]
        pump.aspirate_uL(2500, settle_timeout_s=7.5)
        assert captured["steps"] == 6000
        assert captured["kwargs"] == {
            "settle_timeout_s": 7.5,
            "poll_interval_s": 0.1,
        }

    def test_dispense_default_target_is_zero(self) -> None:
        pump = _pump(5000, SyringePumpController.StepMode.NORMAL)
        captured: dict[str, object] = {}

        def fake(steps: int, **kwargs: object) -> None:
            captured["steps"] = steps

        pump.move_to_steps = fake  # type: ignore[method-assign]
        pump.dispense_uL()
        assert captured["steps"] == 0

    def test_dispense_explicit_target(self) -> None:
        pump = _pump(125, SyringePumpController.StepMode.NORMAL)
        captured: dict[str, object] = {}

        def fake(steps: int, **kwargs: object) -> None:
            captured["steps"] = steps

        pump.move_to_steps = fake  # type: ignore[method-assign]
        pump.dispense_uL(62.5)
        assert captured["steps"] == 6000

    def test_aspirate_out_of_range_raises_before_io(self) -> None:
        """Range validation must fire *before* delegating — the stub
        transport raises if any I/O happens, so a successful ValueError
        here proves no frame was sent."""
        pump = _pump(125, SyringePumpController.StepMode.NORMAL)
        with pytest.raises(ValueError, match="volume_uL"):
            pump.aspirate_uL(200)

    def test_dispense_out_of_range_raises_before_io(self) -> None:
        pump = _pump(125, SyringePumpController.StepMode.NORMAL)
        with pytest.raises(ValueError, match="volume_uL"):
            pump.dispense_uL(-1)


class TestTomlLoading:
    def test_loads_pump_section(self, tmp_path: Path) -> None:
        toml = tmp_path / "pump.toml"
        toml.write_text(
            '[pump]\nport = "/dev/ttyUSB2"\naddress = 3\nbaud = 38400\n'
            'syringe_uL = 1000\nstep_mode = "N1"\nreply_timeout_s = 2.5\n',
            encoding="utf-8",
        )
        cfg = SyringePumpController.Config.from_toml(toml)
        assert cfg.port == "/dev/ttyUSB2"
        assert cfg.address == 3
        assert cfg.baud == 38400
        assert cfg.syringe_uL == 1000
        assert cfg.step_mode is SyringePumpController.StepMode.FINE
        assert cfg.reply_timeout_s == 2.5

    def test_loads_top_level_keys(self, tmp_path: Path) -> None:
        toml = tmp_path / "pump.toml"
        toml.write_text('port = "/dev/ttyUSB3"\n', encoding="utf-8")
        cfg = SyringePumpController.Config.from_toml(toml)
        assert cfg.port == "/dev/ttyUSB3"

    def test_ignores_unknown_keys(self, tmp_path: Path) -> None:
        toml = tmp_path / "pump.toml"
        toml.write_text(
            '[pump]\nport = "/dev/ttyUSB0"\nfuture_field = 42\n',
            encoding="utf-8",
        )
        cfg = SyringePumpController.Config.from_toml(toml)
        assert cfg.port == "/dev/ttyUSB0"

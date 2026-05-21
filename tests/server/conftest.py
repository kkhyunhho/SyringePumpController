"""Pytest fixtures: FakePump + FastAPI TestClient.

FakePump quacks like SyringePumpController but uses in-memory state. The
real driver's nested exception classes (PlungerOverloadError, …) are
reachable via SyringePumpController.<Name>, so isinstance checks in
server.errors continue to work against FakePump-raised exceptions.

NOT a re-introduction of the deleted FakeTransport (DESIGN.md §14 Path
C) — that lived in src/sy01b/. This fake lives only under tests/server/
and is intentionally specific to the bridge layer.
"""

from __future__ import annotations

from collections.abc import Iterator
from typing import Any

import pytest
from fastapi.testclient import TestClient

from server.app import create_app
from sy01b import SyringePumpController


class FakePump:
    """In-memory stand-in for SyringePumpController."""

    def __init__(self) -> None:
        self.config = SyringePumpController.Config(
            port="/tmp/fake-pump",
            address=1,
            baud=9600,
            syringe_uL=125,
            step_mode=SyringePumpController.StepMode.NORMAL,
            reply_timeout_s=1.0,
        )
        self._valve_pos = "?"
        self._plunger_pos = 0
        self._initialized = False
        self._next_error: Exception | None = None
        self.calls: list[tuple[str, tuple[Any, ...], dict[str, Any]]] = []

    # ----- test-side controls -------------------------------------------
    def inject_error(self, exc: Exception) -> None:
        """Make the NEXT non-query call raise ``exc``."""
        self._next_error = exc

    def _record(
        self, name: str, args: tuple[Any, ...], kw: dict[str, Any]
    ) -> None:
        self.calls.append((name, args, kw))

    def _maybe_raise(self) -> None:
        if self._next_error is not None:
            exc, self._next_error = self._next_error, None
            raise exc

    # ----- driver-API surface used by server/routes.py ------------------
    def diagnose(self) -> SyringePumpController.DiagnosticsReport:
        self._record("diagnose", (), {})
        self._maybe_raise()
        status = SyringePumpController.StatusByte(
            busy=False,
            error=SyringePumpController.ErrorCode.NOT_INITIALIZED,
            raw=0x47,
        )
        return SyringePumpController.DiagnosticsReport(
            software_version="FAKE-8.33",
            serial_number="FAKE-SN-0001",
            config="4 way|125uL",
            supply_volts=24.0,
            valve_position=self._valve_pos,
            plunger_steps=self._plunger_pos,
            pre_init_status=status,
            warnings=(),
        )

    def initialize(self, *, force: int = 0, ccw: bool = False) -> None:
        self._record("initialize", (), {"force": force, "ccw": ccw})
        self._maybe_raise()
        self._initialized = True
        self._valve_pos = "1"
        self._plunger_pos = 0

    def move_valve_to_port(
        self, port: int, *, direction_ccw: bool = False
    ) -> None:
        self._record(
            "move_valve_to_port", (port,), {"direction_ccw": direction_ccw}
        )
        self._maybe_raise()
        if not 1 <= port <= 16:
            raise ValueError(f"port must be 1..16, got {port}")
        self._valve_pos = str(port)

    def move_to_steps(self, steps: int) -> None:
        self._record("move_to_steps", (steps,), {})
        self._maybe_raise()
        stroke = self.config.step_mode.full_stroke_steps
        if not 0 <= steps <= stroke:
            raise ValueError(f"steps must be 0..{stroke}, got {steps}")
        self._plunger_pos = steps

    def aspirate_uL(self, target_uL: float) -> None:
        self._record("aspirate_uL", (target_uL,), {})
        self._maybe_raise()
        if not 0 <= target_uL <= self.config.syringe_uL:
            raise ValueError(
                f"target_uL must be 0..{self.config.syringe_uL}, "
                f"got {target_uL}"
            )
        stroke = self.config.step_mode.full_stroke_steps
        self._plunger_pos = round(target_uL / self.config.syringe_uL * stroke)

    def dispense_uL(self, target_uL: float = 0.0) -> None:
        self._record("dispense_uL", (target_uL,), {})
        self._maybe_raise()
        if not 0 <= target_uL <= self.config.syringe_uL:
            raise ValueError(
                f"target_uL must be 0..{self.config.syringe_uL}, "
                f"got {target_uL}"
            )
        stroke = self.config.step_mode.full_stroke_steps
        self._plunger_pos = round(target_uL / self.config.syringe_uL * stroke)

    def query_status(self) -> SyringePumpController.StatusByte:
        self._maybe_raise()
        error = (
            SyringePumpController.ErrorCode.OK
            if self._initialized
            else SyringePumpController.ErrorCode.NOT_INITIALIZED
        )
        raw = 0x40 if self._initialized else 0x47
        return SyringePumpController.StatusByte(
            busy=False, error=error, raw=raw
        )

    def query_valve_position(self) -> str:
        return self._valve_pos

    def query_plunger_position(self) -> int:
        return self._plunger_pos

    def close(self) -> None:
        self._record("close", (), {})


@pytest.fixture
def fake_pump() -> FakePump:
    return FakePump()


@pytest.fixture
def client(fake_pump: FakePump) -> Iterator[TestClient]:
    app = create_app(pump_factory=lambda: fake_pump, config=fake_pump.config)
    with TestClient(app) as c:
        yield c

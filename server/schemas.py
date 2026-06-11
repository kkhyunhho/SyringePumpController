"""Pydantic request/response models for the /v1 API."""

from __future__ import annotations

from pydantic import BaseModel, Field


class HealthResponse(BaseModel):
    pump_open: bool
    diagnose_ok: bool | None
    driver_version: str


class DiagnoseResponse(BaseModel):
    software_version: str
    serial_number: str
    config: str
    supply_volts: float
    valve_position: str
    plunger_steps: int
    pre_init_busy: bool
    pre_init_error_name: str
    pre_init_error_code: int
    ok_to_initialize: bool
    warnings: list[str]
    syringe_uL: float = Field(
        description=(
            "Installed syringe volume in µL, from server-side config. "
            "Clients (e.g. ESP32 UI) use this to size volume sliders "
            "without hard-coding a bench-specific default."
        )
    )
    stroke_steps: int = Field(
        description=(
            "Full plunger stroke in steps for the configured step mode "
            "(12 000 in N0/NORMAL, 96 000 in N1/N2)."
        )
    )


class InitializeRequest(BaseModel):
    force: int = Field(
        default=2, description="0/1/2 or 10..40 (Z-init force code)."
    )
    ccw: bool = False


class InitializeResponse(BaseModel):
    valve: str
    plunger_steps: int


class ValveRequest(BaseModel):
    port: int = Field(ge=1, le=16)
    ccw: bool = False


class ValveResponse(BaseModel):
    valve: str


class VolumeRequest(BaseModel):
    """Request body for /v1/aspirate and /v1/dispense.

    Both endpoints take an absolute contained-volume target; the wire frame is
    identical for the same value. The endpoint split exists only so the caller
    can express intent (fill vs. drain) at the call site.
    """

    target_uL: float = Field(
        ge=0.0, description="Absolute contained volume, µL."
    )


class VolumeResponse(BaseModel):
    plunger_steps: int
    target_uL: float


class DispenseRequest(BaseModel):
    target_uL: float = Field(default=0.0, ge=0.0)


class MoveStepsRequest(BaseModel):
    steps: int = Field(ge=0)


class MoveStepsResponse(BaseModel):
    plunger_steps: int


class PrimeRequest(BaseModel):
    cycles: int = Field(default=1, ge=1, le=100)
    source_port: int = Field(default=3, ge=1, le=16)
    sink_port: int = Field(default=1, ge=1, le=16)
    # Per-cycle aspirate volume. ``None`` means a full syringe stroke; the
    # route resolves it against ``Config.syringe_uL``. Lets the operator
    # cycle less than a full stroke from the Aspirate/Dispense tab slider.
    volume_uL: float | None = Field(default=None, ge=0)


class PrimeResponse(BaseModel):
    cycles_done: int
    ul_per_stroke: int
    final_valve: str
    final_plunger: int


class StatusResponse(BaseModel):
    valve: str
    plunger_steps: int
    busy: bool
    error_name: str
    error_code: int


class ErrorResponse(BaseModel):
    error: str
    code: int | None = None
    command: str | None = None
    raw_reply_hex: str | None = None
    message: str

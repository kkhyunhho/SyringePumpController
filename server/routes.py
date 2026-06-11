"""HTTP routes for the /v1 API.

Every handler acquires ``app.state.pump_lock`` for the entire driver
interaction (single in-flight matches the driver's one-command-at-a-time
contract). Blocking driver calls are pushed to a worker thread via
``run_in_threadpool`` so the asyncio loop stays responsive for unrelated
GET /v1/health pings during a long prime.
"""

from __future__ import annotations

from typing import Any

from fastapi import APIRouter, Request
from fastapi.concurrency import run_in_threadpool

from server.schemas import (
    DiagnoseResponse,
    DispenseRequest,
    HealthResponse,
    InitializeRequest,
    InitializeResponse,
    MoveStepsRequest,
    MoveStepsResponse,
    PrimeRequest,
    PrimeResponse,
    StatusResponse,
    ValveRequest,
    ValveResponse,
    VolumeRequest,
    VolumeResponse,
)
from sy01b import SyringePumpController, __version__

router = APIRouter(prefix="/v1")


def _pump(request: Request) -> Any:
    return request.app.state.pump


@router.get(
    "/health",
    response_model=HealthResponse,
    tags=["Discovery"],
    summary="Liveness probe",
)
async def health(request: Request) -> HealthResponse:
    pump = getattr(request.app.state, "pump", None)
    last = getattr(request.app.state, "last_diagnose", None)
    return HealthResponse(
        pump_open=pump is not None,
        diagnose_ok=(last.ok_to_initialize if last is not None else None),
        driver_version=__version__,
    )


@router.get(
    "/diagnose",
    response_model=DiagnoseResponse,
    tags=["Discovery"],
    summary="One-shot commissioning probe (Status tab — Reconnect)",
)
async def diagnose(request: Request) -> DiagnoseResponse:
    pump = _pump(request)
    async with request.app.state.pump_lock:
        report = await run_in_threadpool(pump.diagnose)
    request.app.state.last_diagnose = report
    return DiagnoseResponse(
        software_version=report.software_version,
        serial_number=report.serial_number,
        config=report.config,
        supply_volts=report.supply_volts,
        valve_position=report.valve_position,
        plunger_steps=report.plunger_steps,
        pre_init_busy=report.pre_init_status.busy,
        pre_init_error_name=report.pre_init_status.error.name,
        pre_init_error_code=int(report.pre_init_status.error),
        ok_to_initialize=report.ok_to_initialize,
        warnings=list(report.warnings),
        syringe_uL=pump.config.syringe_uL,
        stroke_steps=pump.config.step_mode.full_stroke_steps,
    )


@router.post(
    "/initialize",
    response_model=InitializeResponse,
    tags=["Motion"],
    summary="Home plunger + valve (right BSP button / Re-initialize modal)",
)
async def initialize(
    req: InitializeRequest, request: Request
) -> InitializeResponse:
    pump = _pump(request)
    async with request.app.state.pump_lock:
        await run_in_threadpool(
            lambda: pump.initialize(force=req.force, ccw=req.ccw)
        )
        valve = await run_in_threadpool(pump.query_valve_position)
        plunger = await run_in_threadpool(pump.query_plunger_position)
    return InitializeResponse(valve=valve, plunger_steps=plunger)


@router.post(
    "/valve",
    response_model=ValveResponse,
    tags=["Motion"],
    summary="Rotate valve to a port (Valve tab — port buttons)",
)
async def valve(req: ValveRequest, request: Request) -> ValveResponse:
    pump = _pump(request)
    async with request.app.state.pump_lock:
        await run_in_threadpool(
            lambda: pump.move_valve_to_port(req.port, direction_ccw=req.ccw)
        )
        position = await run_in_threadpool(pump.query_valve_position)
    return ValveResponse(valve=position)


@router.post(
    "/aspirate",
    response_model=VolumeResponse,
    tags=["Motion"],
    summary="Move plunger to an absolute volume (Move tab — C Actuation)",
    description=(
        "Sends `A<n>R` where `n = round(target_uL / syringe_uL * stroke)`. "
        "The wire frame is identical for fill and drain — the firmware's "
        "single **C Actuation** button uses this endpoint for both "
        "directions. The /dispense endpoint exists only for legacy callers."
    ),
)
async def aspirate(req: VolumeRequest, request: Request) -> VolumeResponse:
    pump = _pump(request)
    async with request.app.state.pump_lock:
        await run_in_threadpool(lambda: pump.aspirate_uL(req.target_uL))
        plunger = await run_in_threadpool(pump.query_plunger_position)
    return VolumeResponse(plunger_steps=plunger, target_uL=req.target_uL)


@router.post(
    "/dispense",
    response_model=VolumeResponse,
    tags=["Low-level (deprecated)"],
    deprecated=True,
    summary="Alias of /aspirate (default target_uL=0 → drain to empty)",
    description=(
        "Identical wire frame to /aspirate for the same `target_uL`. "
        "Kept for back-compat with older clients; new callers should "
        "use /aspirate."
    ),
)
async def dispense(req: DispenseRequest, request: Request) -> VolumeResponse:
    pump = _pump(request)
    async with request.app.state.pump_lock:
        await run_in_threadpool(lambda: pump.dispense_uL(req.target_uL))
        plunger = await run_in_threadpool(pump.query_plunger_position)
    return VolumeResponse(plunger_steps=plunger, target_uL=req.target_uL)


@router.post(
    "/move_steps",
    response_model=MoveStepsResponse,
    tags=["Low-level (deprecated)"],
    deprecated=True,
    summary="Raw absolute step move",
    description=(
        "Sends `A<steps>R` directly without the µL → steps conversion. "
        "Bypasses the syringe-size guard that /aspirate applies — "
        "callers must respect the step-mode stroke limit themselves. "
        "Not used by the firmware UI; kept for scripted bench use."
    ),
)
async def move_steps(
    req: MoveStepsRequest, request: Request
) -> MoveStepsResponse:
    pump = _pump(request)
    async with request.app.state.pump_lock:
        await run_in_threadpool(lambda: pump.move_to_steps(req.steps))
        plunger = await run_in_threadpool(pump.query_plunger_position)
    return MoveStepsResponse(plunger_steps=plunger)


@router.post(
    "/prime",
    response_model=PrimeResponse,
    tags=["Motion"],
    summary="Cycle source → sink (Aspirate/Dispense tab — START button)",
    description=(
        "Aligns the valve to the sink and empties the syringe first, so "
        "every dispense leaves via the sink (not back out the source). "
        "Each cycle then = `valve→source, aspirate volume_uL, valve→sink, "
        "dispense→0`. `volume_uL` defaults to a full syringe stroke. The "
        "driver lock is held for the whole sequence so other endpoints "
        "cannot race it."
    ),
)
async def prime(req: PrimeRequest, request: Request) -> PrimeResponse:
    pump = _pump(request)
    cfg: SyringePumpController.Config = request.app.state.config
    # Resolve the per-cycle aspirate volume: None → full stroke. Clamp to
    # the syringe size — you physically cannot aspirate past full travel.
    vol_uL = cfg.syringe_uL if req.volume_uL is None else req.volume_uL
    vol_uL = min(vol_uL, float(cfg.syringe_uL))

    def _run_prime() -> tuple[int, int, str, int]:
        # Precondition: valve on the sink and plunger emptied to 0, so the
        # first aspirate draws cleanly from the source and any residual is
        # expelled to the sink rather than the source.
        pump.move_valve_to_port(req.sink_port)
        pump.dispense_uL(0)
        for _ in range(req.cycles):
            pump.move_valve_to_port(req.source_port)
            pump.aspirate_uL(vol_uL)
            pump.move_valve_to_port(req.sink_port)
            pump.dispense_uL(0)
        final_valve = pump.query_valve_position()
        final_plunger = pump.query_plunger_position()
        return (req.cycles, round(vol_uL), final_valve, final_plunger)

    async with request.app.state.pump_lock:
        (
            cycles_done,
            vol_done,
            final_valve,
            final_plunger,
        ) = await run_in_threadpool(_run_prime)
    return PrimeResponse(
        cycles_done=cycles_done,
        ul_per_stroke=vol_done,
        final_valve=final_valve,
        final_plunger=final_plunger,
    )


@router.get(
    "/status",
    response_model=StatusResponse,
    tags=["Discovery"],
    summary="Poll valve/plunger/busy/error (Status tab — 2 s loop)",
)
async def status(request: Request) -> StatusResponse:
    pump = _pump(request)
    async with request.app.state.pump_lock:
        st = await run_in_threadpool(pump.query_status)
        valve_pos = await run_in_threadpool(pump.query_valve_position)
        plunger = await run_in_threadpool(pump.query_plunger_position)
    return StatusResponse(
        valve=valve_pos,
        plunger_steps=plunger,
        busy=st.busy,
        error_name=st.error.name,
        error_code=int(st.error),
    )

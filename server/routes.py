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


@router.get("/health", response_model=HealthResponse)
async def health(request: Request) -> HealthResponse:
    pump = getattr(request.app.state, "pump", None)
    last = getattr(request.app.state, "last_diagnose", None)
    return HealthResponse(
        pump_open=pump is not None,
        diagnose_ok=(last.ok_to_initialize if last is not None else None),
        driver_version=__version__,
    )


@router.get("/diagnose", response_model=DiagnoseResponse)
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
    )


@router.post("/initialize", response_model=InitializeResponse)
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


@router.post("/valve", response_model=ValveResponse)
async def valve(req: ValveRequest, request: Request) -> ValveResponse:
    pump = _pump(request)
    async with request.app.state.pump_lock:
        await run_in_threadpool(
            lambda: pump.move_valve_to_port(req.port, direction_ccw=req.ccw)
        )
        position = await run_in_threadpool(pump.query_valve_position)
    return ValveResponse(valve=position)


@router.post("/aspirate", response_model=VolumeResponse)
async def aspirate(req: VolumeRequest, request: Request) -> VolumeResponse:
    pump = _pump(request)
    async with request.app.state.pump_lock:
        await run_in_threadpool(lambda: pump.aspirate_uL(req.target_uL))
        plunger = await run_in_threadpool(pump.query_plunger_position)
    return VolumeResponse(plunger_steps=plunger, target_uL=req.target_uL)


@router.post("/dispense", response_model=VolumeResponse)
async def dispense(req: DispenseRequest, request: Request) -> VolumeResponse:
    pump = _pump(request)
    async with request.app.state.pump_lock:
        await run_in_threadpool(lambda: pump.dispense_uL(req.target_uL))
        plunger = await run_in_threadpool(pump.query_plunger_position)
    return VolumeResponse(plunger_steps=plunger, target_uL=req.target_uL)


@router.post("/move_steps", response_model=MoveStepsResponse)
async def move_steps(
    req: MoveStepsRequest, request: Request
) -> MoveStepsResponse:
    pump = _pump(request)
    async with request.app.state.pump_lock:
        await run_in_threadpool(lambda: pump.move_to_steps(req.steps))
        plunger = await run_in_threadpool(pump.query_plunger_position)
    return MoveStepsResponse(plunger_steps=plunger)


@router.post("/prime", response_model=PrimeResponse)
async def prime(req: PrimeRequest, request: Request) -> PrimeResponse:
    """Replicate ``claude_test/prime_line.py`` over the wire.

    Each cycle is 4 verified moves: valve→source, plunger→full stroke,
    valve→sink, plunger→0. The lock is held for the whole sequence so
    no other endpoint can race the driver during prime.
    """
    pump = _pump(request)
    cfg: SyringePumpController.Config = request.app.state.config
    stroke = cfg.step_mode.full_stroke_steps
    ul_per_stroke = cfg.syringe_uL

    def _run_prime() -> tuple[int, str, int]:
        for _ in range(req.cycles):
            pump.move_valve_to_port(req.source_port)
            pump.move_to_steps(stroke)
            pump.move_valve_to_port(req.sink_port)
            pump.move_to_steps(0)
        final_valve = pump.query_valve_position()
        final_plunger = pump.query_plunger_position()
        return (req.cycles, final_valve, final_plunger)

    async with request.app.state.pump_lock:
        cycles_done, final_valve, final_plunger = await run_in_threadpool(
            _run_prime
        )
    return PrimeResponse(
        cycles_done=cycles_done,
        ul_per_stroke=ul_per_stroke,
        final_valve=final_valve,
        final_plunger=final_plunger,
    )


@router.get("/status", response_model=StatusResponse)
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

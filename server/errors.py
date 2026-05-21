"""Driver exception → HTTP JSON response mapping.

The single ``register_exception_handlers`` function wires every driver
exception class to a JSONResponse with a stable schema (see
``schemas.ErrorResponse``). No traceback ever leaks to the client; the
driver attaches ``command_sent``, ``raw_reply``, and ``error_code`` to
``DeviceError`` instances and we serialize those verbatim.
"""

from __future__ import annotations

from fastapi import FastAPI, Request
from fastapi.responses import JSONResponse

from sy01b import SyringePumpController


def _device_error_body(
    exc: SyringePumpController.DeviceError,
) -> dict[str, object]:
    return {
        "error": type(exc).__name__,
        "code": int(exc.error_code),
        "command": exc.command_sent,
        "raw_reply_hex": exc.raw_reply.hex(),
        "message": str(exc),
    }


def _plain_body(exc: Exception) -> dict[str, object]:
    return {
        "error": type(exc).__name__,
        "code": None,
        "command": None,
        "raw_reply_hex": None,
        "message": str(exc),
    }


_DEVICE_ERROR_STATUS: dict[type[SyringePumpController.DeviceError], int] = {
    SyringePumpController.InvalidCommandError: 400,
    SyringePumpController.InvalidOperandError: 400,
    SyringePumpController.NotInitializedError: 409,
    SyringePumpController.PlungerBlockedByBypassError: 409,
    SyringePumpController.CommandOverflowError: 409,
    SyringePumpController.InitFailedError: 500,
    SyringePumpController.PlungerOverloadError: 500,
    SyringePumpController.ValveOverloadError: 500,
}


def register_exception_handlers(app: FastAPI) -> None:
    """Install handlers that map driver exceptions to JSON responses."""

    @app.exception_handler(SyringePumpController.DeviceError)
    async def _device_error(_req: Request, exc: Exception) -> JSONResponse:
        assert isinstance(exc, SyringePumpController.DeviceError)
        status = _DEVICE_ERROR_STATUS.get(type(exc), 500)
        return JSONResponse(status_code=status, content=_device_error_body(exc))

    @app.exception_handler(SyringePumpController.ProtocolError)
    async def _protocol_error(_req: Request, exc: Exception) -> JSONResponse:
        return JSONResponse(status_code=502, content=_plain_body(exc))

    @app.exception_handler(SyringePumpController.TransportTimeout)
    async def _transport_timeout(_req: Request, exc: Exception) -> JSONResponse:
        return JSONResponse(status_code=504, content=_plain_body(exc))

    @app.exception_handler(SyringePumpController.TransportClosed)
    async def _transport_closed(_req: Request, exc: Exception) -> JSONResponse:
        return JSONResponse(status_code=503, content=_plain_body(exc))

    @app.exception_handler(SyringePumpController.DiagnosticError)
    async def _diagnostic_error(_req: Request, exc: Exception) -> JSONResponse:
        return JSONResponse(status_code=503, content=_plain_body(exc))

    @app.exception_handler(ValueError)
    async def _value_error(_req: Request, exc: Exception) -> JSONResponse:
        return JSONResponse(status_code=400, content=_plain_body(exc))

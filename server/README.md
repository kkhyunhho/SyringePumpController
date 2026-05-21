# sy01b-server

HTTP bridge from a remote client (ESP32-S3 on the LAN) to the local
`SyringePumpController` driver over the CH340 dongle on `/dev/ttyUSB1`.
Phase A of the ESP32 controller initiative — firmware is shipped in a
later phase. See the parent [README.md](../README.md) and
[DESIGN.md](../DESIGN.md) for the broader architecture.

## Architecture

```
ESP32-S3 (LVGL UI) -- HTTP/JSON --> PC host (FastAPI) -- pyserial --> SY-01B
                                    0.0.0.0:17046
```

Single uvicorn worker, single `asyncio.Lock` serializing every driver
interaction (the pump is one-command-at-a-time and the driver is
synchronous-blocking). Long operations (prime, ~30 s) block the request
that issued them; there is no job-id queue.

## Install

```bash
.venv/bin/pip install -e ".[dev,server]"
```

## Run

```bash
cp server/pump.toml.example server/pump.toml  # edit for your bench
.venv/bin/python -m server --config server/pump.toml
# or via the console script:
.venv/bin/sy01b-server --config server/pump.toml
# or via env var:
SY01B_SERVER_CONFIG=server/pump.toml .venv/bin/sy01b-server
```

The pump is opened once at uvicorn startup and closed on shutdown. The
diagnostic probe (`/v1/diagnose`) runs lazily on first request and the
result is cached on `app.state.last_diagnose` so `GET /v1/health`
reports `diagnose_ok`.

## Endpoints (all under `/v1`)

| Method | Path             | Body                                         | Reply                                                      |
|--------|------------------|----------------------------------------------|------------------------------------------------------------|
| GET    | `/v1/health`     | —                                            | `{pump_open, diagnose_ok, driver_version}`                 |
| GET    | `/v1/diagnose`   | —                                            | flattened `DiagnosticsReport`                              |
| POST   | `/v1/initialize` | `{force?: int=2, ccw?: bool=false}`          | `{valve, plunger_steps}`                                   |
| POST   | `/v1/valve`      | `{port: 1..16, ccw?: bool=false}`            | `{valve}`                                                  |
| POST   | `/v1/aspirate`   | `{target_uL: float}`                         | `{plunger_steps, target_uL}`                               |
| POST   | `/v1/dispense`   | `{target_uL: float=0}`                       | `{plunger_steps, target_uL}`                               |
| POST   | `/v1/move_steps` | `{steps: int}`                               | `{plunger_steps}`                                          |
| POST   | `/v1/prime`      | `{cycles?:1, source_port?:3, sink_port?:1}`  | `{cycles_done, ul_per_stroke, final_valve, final_plunger}` |
| GET    | `/v1/status`     | —                                            | `{valve, plunger_steps, busy, error_name, error_code}`     |

## Error mapping

Driver exceptions are mapped to HTTP status codes with a stable JSON
body `{error, code, command, raw_reply_hex, message}` — no Python
traceback leaks to the client.

| Status | Driver exceptions                                                                  |
|--------|------------------------------------------------------------------------------------|
| 400    | `ValueError`, `InvalidCommandError`, `InvalidOperandError`                         |
| 409    | `NotInitializedError`, `PlungerBlockedByBypassError`, `CommandOverflowError`       |
| 500    | `InitFailedError`, `PlungerOverloadError`, `ValveOverloadError`, generic `DeviceError` |
| 502    | `ProtocolError`                                                                    |
| 503    | `TransportClosed`, `DiagnosticError` family (e.g. `LowSupplyVoltageError`)         |
| 504    | `TransportTimeout`                                                                 |

## Safety patterns preserved

- **No auto-init.** The server never calls `initialize()` itself —
  the client must explicitly POST `/v1/initialize`. Matches the
  diagnose-before-init rule (DESIGN.md §12 Q4).
- **Position polling.** Driver already polls `?` / `?6` instead of
  `Q.busy` (firmware 8.33 unreliable per LearnedPatterns E5). The
  server only forwards calls.
- **Single in-flight.** `--workers 1` + `asyncio.Lock`. Concurrent
  requests are queued; they never interleave inside a `prime` cycle.

## HIL smoke check

After starting the server, exercise it against the real pump on
`/dev/ttyUSB1`:

```bash
curl -s http://localhost:17046/v1/health | jq
curl -s http://localhost:17046/v1/diagnose | jq
curl -s -X POST http://localhost:17046/v1/initialize \
  -H 'Content-Type: application/json' -d '{"force":2}'
curl -s -X POST http://localhost:17046/v1/valve \
  -H 'Content-Type: application/json' -d '{"port":3}'
curl -s -X POST http://localhost:17046/v1/aspirate \
  -H 'Content-Type: application/json' -d '{"target_uL":125}'
curl -s -X POST http://localhost:17046/v1/valve \
  -H 'Content-Type: application/json' -d '{"port":1}'
curl -s -X POST http://localhost:17046/v1/dispense \
  -H 'Content-Type: application/json' -d '{"target_uL":0}'
curl -s -X POST http://localhost:17046/v1/prime \
  -H 'Content-Type: application/json' -d '{"cycles":1}'
# From LAN (as the ESP32 sees it):
curl -s http://192.168.1.129:17046/v1/health | jq
```

The auto-generated OpenAPI docs are at `/docs` and the schema at
`/openapi.json`.

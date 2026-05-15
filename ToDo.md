# ToDo

Implementation checklist for the SY-01B controller. Derived from [DESIGN.md](DESIGN.md). Sections numbered to mirror DESIGN.md where possible. Check items off as they land.

## 0. Project scaffolding

- [ ] `pyproject.toml` with `requires-python = ">=3.12"`, `hatchling` backend, package = `sy01b`
- [ ] Dev dependencies: `ruff`, `mypy`, `pytest`, `pytest-cov`, `pyserial`
- [ ] Console script entry: `sy01b-diagnose = sy01b.cli.diagnose:main`
- [ ] `ruff.toml` (or `[tool.ruff]` in pyproject) — line length 100, all rules on except known-noisy
- [ ] `mypy.ini` — strict on `src/sy01b/`, relaxed on `tests/`
- [ ] `.github/workflows/ci.yml` — ruff + mypy + pytest matrix on 3.12
- [ ] Empty `src/sy01b/__init__.py`, `tests/__init__.py`, `tests/conftest.py`
- [ ] Update [CLAUDE.md](CLAUDE.md) §"When code lands here" with the actual `pytest` / `ruff` / `mypy` invocations once they exist

## 4. Transport layer (DT, CH340)

- [ ] `transport.py`: `DTTransport` class wrapping `serial.Serial`
- [ ] Open with `baudrate=9600, bytesize=8, parity='N', stopbits=1, dsrdtr=False, rtscts=False, xonxoff=False`
- [ ] Drop DTR + RTS explicitly after open to neutralize the CH340 open-glitch
- [ ] `send(frame: bytes, deadline_s: float) -> bytes` — write, then read until ETX or deadline
- [ ] Raise `TransportTimeout` with elapsed time on deadline miss
- [ ] Honor the by-id udev path (`/dev/serial/by-id/...`) — just pass through to `serial.Serial(port=…)`
- [ ] `FakeTransport` for tests: takes a scripted list of `(expected_frame, reply_bytes)` pairs

## 5. Protocol layer (pure)

- [ ] `protocol.py`: `build_command(cmds: str, address: int, *, execute: bool) -> bytes`
- [ ] Address byte formatter: int 1–15 → ASCII `'1'..'?'`; raise on out-of-range
- [ ] `Reply` dataclass + `parse_reply(frame: bytes) -> Reply`
- [ ] `StatusByte` with `busy: bool`, `error: ErrorCode`
- [ ] `ErrorCode` `IntEnum` with all codes from CLAUDE.md error table + `UNKNOWN`
- [ ] Builder helpers: `init_cw()`, `init_ccw()`, `abs_move(n)`, `rel_pickup(n)`, `rel_dispense(n)`, `valve_in()`, `valve_out()`, `valve_bypass()`, `valve_to(port)`, `set_step_mode(mode)`, `set_stall_current(n)`, `query_status()`, `query_position()`, `query_voltage()`, `query_valve()`, `query_config()`
- [ ] Reject command strings > 255 chars before they go on the wire
- [ ] No I/O, no global state in this module — easy to test exhaustively

## 6. Pump (high-level)

- [ ] `pump.py`: `Pump` class, `Pump.open(cfg) -> Pump` classmethod
- [ ] Context manager: `__enter__` returns self, `__exit__` closes transport only (no `T`, no re-init)
- [ ] `initialize(force=0, *, ccw=False)` → `ZR` / `YR`; waits via `_wait_until_ready`
- [ ] `_wait_until_ready(deadline_s)`: poll `Q`, exponential backoff capped at 100 ms, raise on error code or deadline
- [ ] `aspirate_uL(uL)`, `dispense_uL(uL)`, `move_to_steps(steps)` — volume↔step conversion from cfg
- [ ] `valve_to(port)` / `valve_in()` / `valve_out()` / `valve_bypass()` — single-shot, wait until ready
- [ ] `abort()` — sends `T`, then sets `requires_reinit = True`
- [ ] `requires_reinit` latch: error 1 and error 9 set it; move methods raise `RequiresReinitError` until `initialize()` clears it
- [ ] `set_stall_current_for_syringe()` — derive `U200,<n>` from `cfg.syringe_uL` (see CLAUDE.md table)
- [ ] `raw(cmds: str) -> Reply` escape hatch for commands not modelled here (logs at WARN)

## 7. Diagnostic / commissioning flow

- [ ] `diagnostics.py`: `DiagnosticsReport` dataclass, `diagnose(pump) -> DiagnosticsReport`
- [ ] Step 1: echo probe — send `Q`, confirm reply parses as DT
- [ ] Step 2: status — accept `OK busy=0` or `NotInitialized`; anything else is a hard fail
- [ ] Step 3: `?76` config readback, store firmware string
- [ ] Step 4: `*` supply voltage, fail if < 22 V
- [ ] Step 5: `?6` valve position, warn if bypass
- [ ] Step 6: `?` plunger position, log only
- [ ] Specific exceptions: `DiagnosticTimeoutError`, `DiagnosticGarbledReplyError`, `LowSupplyVoltageError`, `WrongAddressError`
- [ ] CLI script `sy01b-diagnose`: reads TOML config, prints one-screen summary, exits non-zero on hard fail
- [ ] Never emit `R`, `Z`, `Y`, `W` from the diagnostic path — enforce in code, not just docs

## 8. Errors

- [ ] `errors.py`: full hierarchy per DESIGN.md §8
- [ ] Each `DeviceError` stores `command_sent: str`, `raw_reply: bytes`, `error_code: ErrorCode`
- [ ] `__str__` formats as `"<ClassName>: code=<n> cmd=<...> reply=<hex>"` for log readability
- [ ] Map function: `device_error_for(code) -> type[DeviceError]` with fallback to base `DeviceError`

## 9. Configuration

- [ ] `config.py`: `PumpConfig` frozen dataclass (slots=True)
- [ ] Fields per DESIGN.md §9 table
- [ ] `from_toml(path) -> PumpConfig` classmethod using `tomllib`
- [ ] Validate `syringe_uL ∈ ALLOWED_SYRINGES`, `1 ≤ address ≤ 15`, `step_mode ∈ StepMode`
- [ ] `stall_current_code()` method that returns the `U200,<n>` operand for the syringe size

## 10. Testing

- [ ] `test_protocol.py`: round-trip every builder; parse every status code; reject malformed frames
- [ ] `test_pump_fake.py`: scripted sequence for init/aspirate/dispense; error-mapping for each code; `requires_reinit` latch test
- [ ] `test_diagnostics.py`: each hard-fail path raises the right exception; happy path emits a clean report
- [ ] `examples/hil_smoke.md`: manual HIL checklist — **read-only only** (firmware, serial number, supply voltage, status, valve, plunger position). No `R`, no init, no moves. Mechanical end-to-end testing is a separate operator-driven activity, not part of this script.
- [ ] `examples/hil_identity.py`: read-only identity probe script that drives the HIL checklist programmatically and prints a one-block summary
- [ ] Coverage gate at 90 % on `src/sy01b/` excluding `transport.py` real-serial paths

## 11. Logging

- [ ] `_logging.py`: `get_logger(name)` wrapper, no handler registration at import
- [ ] Frame send/receive at DEBUG, with hex dump helper
- [ ] `_wait_until_ready` logs at INFO if elapsed > 2 s
- [ ] Document `LOG=DEBUG sy01b-diagnose ...` recipe in `examples/repl_session.md`

## 13. Documentation hygiene (when code lands)

- [ ] Update [CLAUDE.md](CLAUDE.md) §"When code lands here": fill in build/test commands; remove placeholder line about choosing DT vs OEM (we chose DT)
- [ ] Move §11 open questions from DESIGN.md to GitHub issues once the first one needs an answer
- [ ] Add a `CHANGELOG.md` at the first tagged release
- [ ] Append to [LearnedPatterns.md](LearnedPatterns.md) as each ToDo item completes — categorized lessons (R/G/Q/W/E prefixes per §1–5), provenance `(from ToDo#N)`. Format mirrors https://github.com/coport-uni/CommonClaude/blob/main/LearnedPatterns.md

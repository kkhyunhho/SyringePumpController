# ToDo

Implementation checklist for the SY-01B controller. Derived from [DESIGN.md](DESIGN.md). Sections numbered to mirror DESIGN.md where possible. Check items off as they land.

## 0. Project scaffolding

- [x] `pyproject.toml` with `requires-python = ">=3.12"`, `hatchling` backend, package = `sy01b`
- [x] Dev dependencies: `ruff`, `mypy`, `pytest`, `pytest-cov`, `pyserial`
- [x] Console script entry: `sy01b-diagnose = sy01b.cli.diagnose:main`
- [x] Tool configs consolidated into `pyproject.toml` (`[tool.ruff]`, `[tool.mypy]`, `[tool.pytest.ini_options]`) — separate `ruff.toml`/`mypy.ini` not used
- [x] `.github/workflows/ci.yml` — ruff + mypy + pytest on 3.12
- [x] `src/sy01b/__init__.py`, `tests/__init__.py`, `tests/conftest.py`
- [x] Update [CLAUDE.md](CLAUDE.md) §"Build, lint, test" with the actual `pytest` / `ruff` / `mypy` invocations

## 4. Transport layer (DT, CH340)

- [x] `transport.py`: `DTTransport` class wrapping `serial.Serial`
- [x] Open with `baudrate=9600, bytesize=8, parity='N', stopbits=1, dsrdtr=False, rtscts=False, xonxoff=False`
- [x] Drop DTR + RTS explicitly after open to neutralize the CH340 open-glitch (uses pyserial 3.x property setters)
- [x] `send(frame: bytes, deadline_s: float) -> bytes` — write, then read until ETX or deadline
- [x] Raise `TransportTimeout` with elapsed time on deadline miss
- [x] Honor the by-id udev path (`/dev/serial/by-id/...`) — passes through to `serial.Serial(port=…)`
- [x] `FakeTransport` for tests: takes a scripted list of `(expected_frame, reply_bytes)` pairs (in `tests/conftest.py`)
- [ ] Real-hardware HIL probe for transport (post-shipping)

## 5. Protocol layer (pure)

- [x] `protocol.py`: `build_command(address: int, cmds: str, *, execute: bool) -> bytes`
- [x] Address byte formatter: int 1–15 → ASCII `'1'..'?'`; raise on out-of-range
- [x] `Reply` dataclass + `parse_reply(frame: bytes) -> Reply`
- [x] `StatusByte` with `busy: bool`, `error: ErrorCode`
- [x] `ErrorCode` `IntEnum` with all codes from CLAUDE.md error table + `UNKNOWN`
- [x] Read-only command constants: `CMD_QUERY_STATUS`, `CMD_QUERY_SOFTWARE_VERSION`, `CMD_QUERY_SERIAL_NUMBER`, `CMD_QUERY_CONFIG`, `CMD_QUERY_SUPPLY_VOLTAGE`, `CMD_QUERY_VALVE_POSITION`, `CMD_QUERY_PLUNGER_POSITION`
- [ ] Motion builders (later commit): `init_cw()`, `init_ccw()`, `abs_move(n)`, `rel_pickup(n)`, `rel_dispense(n)`, `valve_in()`, `valve_out()`, `valve_bypass()`, `valve_to(port)`, `set_step_mode(mode)`, `set_stall_current(n)`
- [x] Reject command strings > 255 chars before they go on the wire
- [x] No I/O, no global state in this module — easy to test exhaustively

## 6. Pump (high-level)

- [x] `pump.py`: `Pump` class, `Pump.open(cfg) -> Pump` classmethod
- [x] Context manager: `__enter__` returns self, `__exit__` closes transport only (no `T`, no re-init)
- [x] Read-only query methods: `query_status`, `query_software_version`, `query_serial_number`, `query_config`, `query_supply_voltage_v`, `query_valve_position`, `query_plunger_position`
- [ ] `initialize(force=0, *, ccw=False)` → `ZR` / `YR`; waits via `_wait_until_ready`
- [ ] `_wait_until_ready(deadline_s)`: poll `Q`, exponential backoff capped at 100 ms, raise on error code or deadline
- [ ] `aspirate_uL(uL)`, `dispense_uL(uL)`, `move_to_steps(steps)` — volume↔step conversion from cfg
- [ ] `valve_to(port)` / `valve_in()` / `valve_out()` / `valve_bypass()` — single-shot, wait until ready
- [ ] `abort()` — sends `T`, then sets `requires_reinit = True`
- [ ] `requires_reinit` latch: error 1 and error 9 set it; move methods raise `RequiresReinitError` until `initialize()` clears it
- [ ] `set_stall_current_for_syringe()` — derive `U200,<n>` from `cfg.syringe_uL` (see CLAUDE.md table)
- [ ] `raw(cmds: str) -> Reply` escape hatch for commands not modelled here (logs at WARN)

## 7. Diagnostic / commissioning flow

- [x] `diagnostics.py`: `DiagnosticsReport` dataclass, `diagnose(pump) -> DiagnosticsReport`
- [x] Step 1: echo probe — send `Q`, confirm reply parses as DT
- [x] Step 2: status — accept `OK busy=0` or `NotInitialized`; anything else logs warning, doesn't raise
- [x] Step 3: `?23` software version, `?202` serial number, `?76` config readback
- [x] Step 4: `*` supply voltage, fail if < 22 V
- [x] Step 5: `?6` valve position, warn if bypass
- [x] Step 6: `?` plunger position, log only
- [x] Specific exceptions: `DiagnosticTimeoutError`, `DiagnosticGarbledReplyError`, `LowSupplyVoltageError`, `WrongAddressError`
- [x] CLI script `sy01b-diagnose`: reads TOML config or CLI flags, prints one-screen summary, exits non-zero on hard fail
- [x] Never emit `R`, `Z`, `Y`, `W` from the diagnostic path — enforced by `TestNoMotionCommandsExposed` and `test_diagnose_never_sends_R_or_init_command`

## 8. Errors

- [x] `errors.py`: full hierarchy per DESIGN.md §8
- [x] Each `DeviceError` stores `command_sent: str`, `raw_reply: bytes`, `error_code: ErrorCode`
- [x] `__str__` formats as `"<ClassName>: code=<n> cmd=<...> reply=<hex>"` for log readability
- [x] Map function: `device_error_for(code) -> type[DeviceError]` with fallback to base `DeviceError`

## 9. Configuration

- [x] `config.py`: `PumpConfig` frozen dataclass (`slots=True`)
- [x] Fields per DESIGN.md §9 table
- [x] `from_toml(path) -> PumpConfig` classmethod using `tomllib`
- [x] Validate `syringe_uL ∈ ALLOWED_SYRINGES`, `1 ≤ address ≤ 15`, baud, timeout
- [x] `stall_current_operand()` method that returns the `U200,<n>` operand for the syringe size

## 10. Testing

- [x] `test_protocol.py`: round-trip every builder; parse every status code; reject malformed frames
- [x] `test_pump_fake.py`: scripted sequence for read-only queries; defensive `TestNoMotionCommandsExposed` asserts motion methods are absent
- [x] `test_diagnostics_failures.py`: each hard-fail path raises the right exception; happy path emits a clean report
- [x] `test_identity.py`: **the verification deliverable** — proves serial number + software version retrieval through the full stack
- [x] `test_config.py`: validation + TOML loading; covers stall-current operand table
- [x] `test_cli.py`: argument parsing + happy/failure exits with stubbed Pump
- [x] `test_errors.py`: device_error_for mapping + exception field carriage
- [x] Coverage gate at 90 % on `src/sy01b/` excluding `transport.py` real-serial paths (current: ~95 %)
- [ ] `examples/hil_smoke.md`: manual HIL checklist — **read-only only** (firmware, serial number, supply voltage, status, valve, plunger position). No `R`, no init, no moves.
- [ ] `examples/hil_identity.py`: read-only identity probe script that drives the HIL checklist programmatically and prints a one-block summary

## 11. Logging

- [x] `_logging.py`: `logger = logging.getLogger("sy01b")` + `hex_preview()` helper, no handler registration at import
- [x] Frame send/receive at DEBUG with hex preview
- [ ] `_wait_until_ready` logs at INFO if elapsed > 2 s (will land with motion methods)
- [ ] Document `LOG=DEBUG sy01b-diagnose ...` recipe in `examples/repl_session.md`

## 13. Documentation hygiene (when code lands)

- [x] Update [CLAUDE.md](CLAUDE.md) §"Build, lint, test": filled in actual invocations; removed DT-vs-OEM placeholder (we chose DT)
- [ ] Move §11 open questions from DESIGN.md to GitHub issues once the first one needs an answer
- [ ] Add a `CHANGELOG.md` at the first tagged release
- [x] Append to [LearnedPatterns.md](LearnedPatterns.md) as each ToDo item completes — categorized lessons (R/G/Q/W/E prefixes per §1–5), provenance `(from ToDo#N)`. Format mirrors https://github.com/coport-uni/CommonClaude/blob/main/LearnedPatterns.md

## 14. Refactors after consolidation

- [x] **Consolidation** (commit `7ff8a5f`, 2026-05-15): 6 source modules collapsed into one `SyringePumpController` class in `src/sy01b/syringe_pump_controller.py`. Fake-pump test layer removed; real pump on `/dev/ttyUSB1` is the ground truth.
- [x] **Class + file rename** (commit `ef5edf9`): `Pump → SyringePumpController`; `pump.py → syringe_pump_controller.py`. Inner `PumpError` kept temporarily.
- [x] **OOP cleanup, Path B** (in progress, 2026-05-15): Plan agent audit confirmed the single-class design is defensible. Two cosmetic fixes applied:
  - `PumpError → Error` (5 token replacements) so `except SyringePumpController.Error:` no longer reads as a typo after the class rename.
  - `Transport` Protocol nested in the class; `__init__` accepts `SyringePumpController.Transport` instead of concrete `serial.Serial`. Private attribute renamed `_serial → _transport`. Runtime behavior unchanged; `serial.Serial` satisfies the Protocol structurally and `serial.serial_for_url('loop://')` is now type-compatible for future testing.
- [ ] **Path C** (defer): re-introduce concrete `_DTSerialTransport` nested class + restore fake-pump unit tests. Revisit only if motion-method iteration cycles prove impractical against real hardware.
- [ ] **Path D** (avoid): full un-consolidation back to 6 modules. Only if requirements double.

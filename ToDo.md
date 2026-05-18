# ToDo

Implementation checklist for the SY-01B controller. Derived from [DESIGN.md](DESIGN.md). Sections numbered to mirror DESIGN.md where possible. Check items off as they land.

## 0. Project scaffolding

- [x] `pyproject.toml` with `requires-python = ">=3.12"`, `hatchling` backend, package = `sy01b`
- [x] Dev dependencies: `ruff`, `mypy`, `pytest`, `pytest-cov`, `pyserial`
- [x] Console script entry: `sy01b-diagnose = sy01b.cli.diagnose:main`
- [x] Tool configs consolidated into `pyproject.toml` (`[tool.ruff]`, `[tool.mypy]`, `[tool.pytest.ini_options]`) — separate `ruff.toml`/`mypy.ini` not used
- [x] `.github/workflows/ci.yml` — ruff + mypy + pytest on 3.12
- [x] `src/sy01b/__init__.py`, `tests/__init__.py` (`tests/conftest.py` removed during consolidation refactor — see §14 commit `7ff8a5f`; not reintroduced)
- [x] Update [CLAUDE.md](CLAUDE.md) §"Build, lint, test" with the actual `pytest` / `ruff` / `mypy` invocations

## 4. Transport layer (DT, CH340)

- [x] `transport.py`: `DTTransport` class wrapping `serial.Serial`
- [x] Open with `baudrate=9600, bytesize=8, parity='N', stopbits=1, dsrdtr=False, rtscts=False, xonxoff=False`
- [x] Drop DTR + RTS explicitly after open to neutralize the CH340 open-glitch (uses pyserial 3.x property setters)
- [x] `send(frame: bytes, deadline_s: float) -> bytes` — write, then read until ETX or deadline
- [x] Raise `TransportTimeout` with elapsed time on deadline miss
- [x] Honor the by-id udev path (`/dev/serial/by-id/...`) — passes through to `serial.Serial(port=…)`
- [ ] `FakeTransport` for tests (deferred): consolidation refactor (§14) removed the fake-pump test layer; real pump on `/dev/ttyUSB1` is the ground truth. Re-introduce only if motion-method iteration against real hardware proves impractical.
- [ ] Real-hardware HIL probe for transport (post-shipping)

## 5. Protocol layer (pure)

- [x] `protocol.py`: `build_command(address: int, cmds: str, *, execute: bool) -> bytes`
- [x] Address byte formatter: int 1–15 → ASCII `'1'..'?'`; raise on out-of-range
- [x] `Reply` dataclass + `parse_reply(frame: bytes) -> Reply`
- [x] `StatusByte` with `busy: bool`, `error: ErrorCode`
- [x] `ErrorCode` `IntEnum` with all codes from CLAUDE.md error table + `UNKNOWN`
- [x] Read-only command constants: `CMD_QUERY_STATUS`, `CMD_QUERY_SOFTWARE_VERSION`, `CMD_QUERY_SERIAL_NUMBER`, `CMD_QUERY_CONFIG`, `CMD_QUERY_SUPPLY_VOLTAGE`, `CMD_QUERY_VALVE_POSITION`, `CMD_QUERY_PLUNGER_POSITION`
- [ ] Motion builders (later commit, plunger side): `init_cw()`, `init_ccw()`, `abs_move(n)`, `rel_pickup(n)`, `rel_dispense(n)`, `set_step_mode(mode)`, `set_stall_current(n)`
- [x] Valve motion (non-distribution): `set_valve_position(I/O/B/E)` shipped via `_execute` + `wait_until_ready` on `SyringePumpController`. Distribution `valve_to(port)` deferred (MCC-4 uses non-distribution syntax).
- [x] Reject command strings > 255 chars before they go on the wire
- [x] No I/O, no global state in this module — easy to test exhaustively

## 6. Pump (high-level)

- [x] `pump.py`: `Pump` class, `Pump.open(cfg) -> Pump` classmethod
- [x] Context manager: `__enter__` returns self, `__exit__` closes transport only (no `T`, no re-init)
- [x] Read-only query methods: `query_status`, `query_software_version`, `query_serial_number`, `query_config`, `query_supply_voltage_v`, `query_valve_position`, `query_plunger_position`
- [x] `initialize(force=0, *, ccw=False, settle_timeout_s=30.0)` → `Z<force>R` / `Y<force>R`; polls `?` until plunger=0 (LearnedPatterns E5 makes `Q.busy` unreliable on firmware 8.33)
- [x] `wait_until_ready(timeout_s, poll_interval_s)`: poll `Q`, raise on error code or deadline. Public method, post-motion only (LearnedPatterns E4).
- [ ] `aspirate_uL(uL)`, `dispense_uL(uL)`, `move_to_steps(steps)` — volume↔step conversion from cfg
- [x] `set_valve_position(I/O/B/E)` + `initialize_valve(home_port, direction_ccw)` — single-shot, wait until ready. Distribution `valve_to(port)` not yet needed (MCC-4 is non-distribution).
- [ ] `abort()` — sends `T`, then sets `requires_reinit = True`
- [ ] `requires_reinit` latch: error 1 and error 9 set it; move methods raise `RequiresReinitError` until `initialize()` clears it
- [x] `set_stall_current_for_syringe()` — derive `U200,<n>` from `cfg.syringe_uL` (see CLAUDE.md table)
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
- [x] `test_pump_fake.py`: removed during §14 consolidation; replaced by `test_plunger_motion_absent.py` (asserts plunger motion still absent, valve motion present).
- [x] `test_diagnostics_failures.py`: each hard-fail path raises the right exception; happy path emits a clean report
- [x] `test_identity.py`: **the verification deliverable** — proves serial number + software version retrieval through the full stack
- [x] `test_config.py`: validation + TOML loading; covers stall-current operand table
- [x] `test_cli.py`: argument parsing + happy/failure exits with stubbed Pump
- [x] `test_errors.py`: device_error_for mapping + exception field carriage
- [x] Coverage gate at 90 % on `src/sy01b/` excluding `transport.py` real-serial paths (current: ~95 %)
- [ ] `claude_test/hil_smoke.md`: manual HIL checklist — **read-only only** (firmware, serial number, supply voltage, status, valve, plunger position). No `R`, no init, no moves.
- [ ] `claude_test/hil_identity.py`: read-only identity probe script that drives the HIL checklist programmatically and prints a one-block summary
- [x] `claude_test/valve_toggle.py`: bench script that toggles MCC-4 valve between INPUT and OUTPUT, verifying each move via `?6`. Plunger never moved.

## 11. Logging

- [x] `_logging.py`: `logger = logging.getLogger("sy01b")` + `hex_preview()` helper, no handler registration at import
- [x] Frame send/receive at DEBUG with hex preview
- [x] `wait_until_ready` logs at INFO if elapsed > 2 s (shipped with valve motion).
- [ ] Document `LOG=DEBUG sy01b-diagnose ...` recipe in `claude_test/repl_session.md`

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

## 15. CommonClaude reconciliation (2026-05-18)

User direction: inherit from [coport-uni/CommonClaude](https://github.com/coport-uni/CommonClaude) and let CommonClaude take precedence over project-specific conventions when they conflict (inverts CommonClaude §1).

- [ ] Project [CLAUDE.md](CLAUDE.md): top-level statement that the project inherits CommonClaude/CLAUDE.md and CommonClaude wins in conflicts.
- [ ] [pyproject.toml](pyproject.toml): `line-length = 100` → `80` (CommonClaude §6).
- [ ] `examples/` → `claude_test/` rename with index README per CommonClaude §3. Update references in CLAUDE.md, ToDo.md, LearnedPatterns.md.
- [ ] Reformat new [LearnedPatterns.md](LearnedPatterns.md) entries E5/E6 from `Note/Rule` to `Problem/Cause/Fix/Rule` per CommonClaude §10. Provenance changed to `(from ToDo#6)`.
- [ ] Run `ruff format` to reflow all code to 80 cols; resolve any remaining `ruff check` / `mypy` / `pytest` failures.
- [ ] Create GitHub issue documenting this reconciliation per CommonClaude §4 (mandatory).
- [ ] Going forward: every new task gets a `ToDo.md` append + `gh issue create` BEFORE work begins. Older LP entries (G1–G6, Q1, W1–W6, E1–E4) keep their existing format per CommonClaude §10 "Once the file exists, this bootstrap procedure no longer applies".

## 16. Plunger initialization (2026-05-18, #2)

First plunger-motion API. Lands the canonical `/1ZR` init path designed in [DESIGN.md §6](DESIGN.md) and drops the corresponding entries from §6 below. Bench target: 125 µL syringe (empty), `/dev/ttyUSB1`, address 1, firmware 8.33. Force=2 (third) chosen for the 125 µL bench syringe (between manual's 50/100 µL=third and 250/500 µL=half bands). Tracked in [#2](https://github.com/coport-uni/SyringePumpController/issues/2).

- [x] Refactor `SyringePumpController.__init__` to take `config: Config`; `open()` passes `config=cfg`. Address and reply_timeout cached on the instance for hot-path convenience.
- [x] `set_stall_current_for_syringe()` → `U200,<n>R` derived from `Config.syringe_uL` (idempotent EEPROM write; no plunger motion).
- [x] `initialize(*, force=0, ccw=False, settle_timeout_s=30.0)` → `Z<force>R` (or `Y<force>R`); poll `?6` until non-`?` (LearnedPatterns E7 — `? == 0` is unsafe when pre-init plunger is already at 0; `Q.busy` is unreliable per E5).
- [x] Update [tests/test_plunger_motion_absent.py](tests/test_plunger_motion_absent.py): drop `"initialize"` and `"set_stall_current"` from forbidden list; add `TestPlungerInitPresent` for `initialize`, `set_stall_current_for_syringe`, and `move_to_steps`. Remaining plunger-move symbols (`aspirate_uL`, `dispense_uL`, `abort`, `set_step_mode`) stay forbidden.
- [x] Extend [tests/test_protocol.py](tests/test_protocol.py) with wire-frame round-trips for `U200,4/5/6 R`, `Z0/Z2/Y0/Z16 R`, and `A0/A6000/A12000/A96000 R`.
- [x] New `claude_test/syringe_init.py`: open `/dev/ttyUSB1`, run `diagnose()` (W1 rule), set stall current, `initialize(force=2)`, log pre- and post-init `?`/`?6` and elapsed time. No further motion. Capacity sweep (25/50/100/125 µL) verified the U200 operand table on real hardware.
- [x] **Extended in session**: added `move_to_steps(steps, *, settle_timeout_s=10.0)` → `A<steps>R`, polls `?` until target matches. Added `claude_test/plunger_cycle.py` exercising max(12 000) → mid(6 000) → min(0) cycles. HIL: 9/9 cycles verified.
- [x] Append [claude_test/README.md](claude_test/README.md) index rows for `syringe_init.py` and `plunger_cycle.py`, including HIL findings.
- [x] HIL run produced real timings/observations → appended E7 (Z completion signal) and E8 (post-init V=4000 pps default) to [LearnedPatterns.md](LearnedPatterns.md) in CommonClaude §10 form.
- [x] Tick §6 lines for `initialize(...)` and `set_stall_current_for_syringe()`.
- [ ] Remaining §6 plunger-side: `aspirate_uL` / `dispense_uL` (volume↔step conversion), `abort` + `requires_reinit` latch, `set_step_mode`, `raw(cmds)`. Defer to next milestone.

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
- [ ] Motion builders (later commit, plunger side): `init_cw()`, `init_ccw()`, `abs_move(n)`, `rel_pickup(n)`, `rel_dispense(n)`, `set_step_mode(mode)` (stall-current setter removed in §17 — handled out-of-band)
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

- [x] Project [CLAUDE.md](CLAUDE.md): top-level statement that the project inherits CommonClaude/CLAUDE.md and CommonClaude wins in conflicts.
- [x] [pyproject.toml](pyproject.toml): `line-length = 100` → `80` (CommonClaude §6).
- [x] `examples/` → `claude_test/` rename with index README per CommonClaude §3. Update references in CLAUDE.md, ToDo.md, LearnedPatterns.md.
- [x] Reformat new [LearnedPatterns.md](LearnedPatterns.md) entries E5/E6 from `Note/Rule` to `Problem/Cause/Fix/Rule` per CommonClaude §10. Provenance changed to `(from ToDo#6)`.
- [x] Run `ruff format` to reflow all code to 80 cols; resolve any remaining `ruff check` / `mypy` / `pytest` failures.
- [x] Create GitHub issue documenting this reconciliation per CommonClaude §4 (mandatory). Closed as [#1](https://github.com/coport-uni/SyringePumpController/issues/1).
- [x] Going forward: every new task gets a `ToDo.md` append + `gh issue create` BEFORE work begins. Older LP entries (G1–G6, Q1, W1–W6, E1–E4) keep their existing format per CommonClaude §10 "Once the file exists, this bootstrap procedure no longer applies". Adopted: §16 / #2 / #3 / #4 / #5 all follow this flow.

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
- [x] §6 plunger-side `aspirate_uL` / `dispense_uL` (volume↔step conversion) — shipped in §19.
- [ ] Remaining §6 plunger-side: `abort` + `requires_reinit` latch, `set_step_mode`, `raw(cmds)`. Defer.

## 17. Stall-current removal (2026-05-18)

After §16 shipped `set_stall_current_for_syringe()`, the user direction
flipped: the bench runs one fixed syringe size and stall current is set
out-of-band once at commissioning time. An in-driver helper that derives
`U200,<n>` from `Config.syringe_uL` adds *more* risk than value — if the
config value diverges from the physically installed syringe, the helper
writes a stall current that can damage a small syringe on the next init.

- [x] Remove `set_stall_current_for_syringe()` from `SyringePumpController`.
- [x] Remove `Config._STALL_CURRENT_TABLE` and `Config.stall_current_operand()`.
- [x] `Config.syringe_uL` field retained for future µL↔step conversion (`aspirate_uL` / `dispense_uL` in §6).
- [x] Delete `tests/test_config.py::TestStallCurrentOperand`.
- [x] Move `set_stall_current_for_syringe` from `TestPlungerInitPresent` to `TestNoPlungerMotionExposed` in [tests/test_plunger_motion_absent.py](tests/test_plunger_motion_absent.py).
- [x] Drop `U200,4/5/6` rows from `test_plunger_init_frames` in [tests/test_protocol.py](tests/test_protocol.py).
- [x] Delete `claude_test/syringe_init.py`; drop the stall-current step from `claude_test/plunger_cycle.py`.
- [x] Drop the stall-current section from `main.py`; renumber the demo sections.
- [x] Document hardware-protocol fact still in [CLAUDE.md](CLAUDE.md) "Stall current" section (kept as a reference table) but note the controller does not expose it.
- [x] Update [README.md](README.md), [DESIGN.md](DESIGN.md), [claude_test/README.md](claude_test/README.md) to remove API references.

## 18. CommonClaude resync to 2a8a597 (2026-05-21, #6)

Refresh the local CommonClaude inheritance pin. Upstream `main` merged
`feat/c-language-support` (PR #26) on 2026-05-19, pivoting `CLAUDE.md`
from language-neutral to C-focused and adding §11–§17 (Conventional
Commits, GitHub Flow, .gitignore, SemVer, PR guidelines, pre-commit,
References). The original local reconciliation (§15, commit `898ecf3`)
predates this drift.

Decisions (confirmed 2026-05-21):

1. **C-only sections (§2 naming, §6 clang-format/cppcheck, §13 C
   .gitignore) are mirrored verbatim.** Python tooling (ruff/mypy/PEP-8)
   is treated as a "specialization" under CLAUDE.md L13, not a conflict
   or a waiver. The Explicit waivers list stays empty.
2. **§11–§17 are adopted in full** with the upstream commit SHA pinned
   in `Authority order` so future drift is detectable.
3. **All 5 upstream hooks are mirrored verbatim and registered in
   `.claude/settings.json`** (PreToolUse Write|Edit/Bash/Read,
   PostToolUse Write|Edit, Stop).
4. **No vendor snapshot** — keep CommonClaude as a URL reference; pin
   via commit SHA only.

Pinned upstream commit:

```
SHA:     2a8a597ec93132ef401b6f0e446255b6f65e5424
Short:   2a8a597
Date:    2026-05-19
Subject: feat(c): switch CLAUDE.md to C convention (PR #26)
```

- [x] Cut working branch `feature/commonclaude-resync-2a8a597` from
  `main` (CommonClaude §12.2 naming).
- [x] Open GitHub issue [#6](https://github.com/coport-uni/SyringePumpController/issues/6)
  per CommonClaude §4 mandate.
- [ ] Update [CLAUDE.md](CLAUDE.md) `Authority order` — replace the
  `main` permalink with the `2a8a597...` permalink; add a
  pinned-at clause noting the previous pin `898ecf3`; expand the
  numbered operational-implications list from §1–§10 to §1–§17;
  add one sentence stating that C-only upstream rules (§2, §6, §13)
  apply via Python specialization (ruff/mypy/PEP-8 + Python
  .gitignore).
- [ ] Fetch and check in 5 hook scripts at the pinned SHA:
  `pre-write-guard.sh`, `pre-bash-secret-scan.sh`,
  `pre-read-env-guard.sh`, `post-write-lint.sh`,
  `post-write-debug-remind.sh`. Then `chmod +x .claude/hooks/*.sh`.
- [ ] Pre-flight read each hook for Python-compat issues. Specifically,
  confirm `post-write-lint.sh` either no-ops on `.py` files or
  branches on extension; if it unconditionally invokes `clang-format`,
  log as §19 candidate (do not patch the local mirror).
- [ ] Verify `jq` is on PATH (upstream LP/E2 dependency for several
  hooks); if absent, log as §19 candidate.
- [ ] Merge upstream `env` + `hooks` blocks into
  [.claude/settings.json](.claude/settings.json) while preserving the
  local `permissions` allowlist verbatim. Validate with
  `python -m json.tool`.
- [ ] [LearnedPatterns.md](LearnedPatterns.md) header drift check —
  `Last updated` / `Total patterns` / `Provenance format` fields. No
  change expected; only edit if drift is observed.
- [ ] Verification: `python -m json.tool .claude/settings.json`,
  `bash -n .claude/hooks/*.sh`, `.venv/bin/pytest`,
  `.venv/bin/ruff check src tests claude_test main.py`,
  `gh api repos/coport-uni/CommonClaude/commits/<SHA> --jq .sha`.
- [ ] Single commit per CommonClaude §11 Conventional Commits, closing
  #6.
- [ ] `gh pr create` per CommonClaude §15.2 template.

Out-of-scope (deliberate):

- Upstream `LearnedPatterns.md` is **not** mirrored — its scope is
  CommonClaude-self (Docker, jq, secret-scan) while local LP is
  hardware-specific (CH340, SY-01B, pyserial 3.x). Scopes differ.
- `Concept.md` and `CLAUDECowork.md` are not mirrored (meta /
  other-workspace).
- `pre-commit` (§16) tooling is documented but not installed — track
  as a future candidate (renumbered after §19 ships).

## 19. µL volume API (2026-05-21, #7)

Add `aspirate_uL` / `dispense_uL` as the user-facing plunger motion API,
on top of the existing step-based `move_to_steps`. Both methods take an
**absolute** contained-volume target in µL and convert to a half-step
position via `round(target_uL / Config.syringe_uL * full_stroke_steps)`,
then delegate to `move_to_steps` so the polling / timeout / error path
stays single-sourced. Lifts the `aspirate_uL` / `dispense_uL` pin from
`TestNoPlungerMotionExposed` (LearnedPatterns W4 — the symbols were
pre-reserved precisely so this milestone could flip them in one commit).
Also flips `Config.syringe_uL` default `5000` → `125` µL to match the
fixed bench syringe (the only physical syringe ever attached). Tracked
in [#7](https://github.com/coport-uni/SyringePumpController/issues/7).

- [x] Add `_uL_to_steps(volume_uL)` private helper in
  [src/sy01b/syringe_pump_controller.py](src/sy01b/syringe_pump_controller.py)
  — range-validates against `Config.syringe_uL` (raises `ValueError`
  before any I/O), converts via `round(volume_uL / syringe * full_stroke)`.
- [x] Add `aspirate_uL(target_uL, *, settle_timeout_s, poll_interval_s)`
  and `dispense_uL(target_uL=0, *, settle_timeout_s, poll_interval_s)`
  as thin wrappers — both call `move_to_steps(_uL_to_steps(target_uL))`.
- [x] Flip `Config.syringe_uL` default `5000` → `125`; update
  [tests/test_config.py](tests/test_config.py)
  `test_defaults_accepted` accordingly.
- [x] Extend [pyproject.toml](pyproject.toml) ruff ignore with
  `N802` / `N803` / `N806` (same `µL`-suffix justification as the
  existing `N815`) so `aspirate_uL` / `_uL_to_steps` / `target_uL` pass.
- [x] Update [tests/test_plunger_motion_absent.py](tests/test_plunger_motion_absent.py):
  remove `aspirate_uL` / `dispense_uL` from `TestNoPlungerMotionExposed`;
  rename `TestPlungerInitPresent` → `TestPlungerMotionPresent` and add
  the two new methods to its present-list.
- [x] Add `TestVolumeToStepsConversion` (parametrized — exact-divide,
  N0/N1 modes, rounding boundary at 0.1 µL on 125 µL syringe, range
  validation) and `TestVolumeAPIDelegation` (delegates with converted
  steps, default `target_uL=0` on dispense, raises before any I/O
  via a `_NeverUsedTransport` stub) to
  [tests/test_config.py](tests/test_config.py).
- [x] Rewrite [main.py](main.py) section 4 — drive via `aspirate_uL` /
  `dispense_uL` keyed off `cfg.syringe_uL`; use `dispense_uL()` for the
  fully-empty step.
- [x] Rewrite [claude_test/plunger_cycle.py](claude_test/plunger_cycle.py)
  to compute targets in µL and verify each move against the same
  `round(uL / syringe_uL * full_stroke)` expectation the driver uses.
- [x] Refresh [DESIGN.md §6](DESIGN.md) — example uses 125 µL syringe;
  add "Absolute semantics" design point; reconcile points 3–6 with the
  shipped `?`/`?6` polling (the planned `_wait_until_ready(Q)` never
  shipped per LearnedPatterns E5).
- [x] Update [README.md](README.md) — public-API surface adds the two
  wrappers; "What's not yet implemented" trimmed to `abort`,
  `set_step_mode`, `raw(cmds)`.
- [x] Update [claude_test/README.md](claude_test/README.md) row for
  `plunger_cycle.py` to describe the new µL interface; HIL marked
  "rerun pending".
- [x] Append commit-boundary bullet to
  [CLAUDE.md](CLAUDE.md) "Commit boundaries seen so far"; refresh the
  pinned-absent list in the repo-status paragraph.
- [ ] HIL on `/dev/ttyUSB1` (125 µL syringe, empty, force=2): run
  `claude_test/plunger_cycle.py --cycles 3` and `main.py`; confirm
  reported `?` after each `aspirate_uL` / `dispense_uL` matches
  `round(uL / 125 * 12 000)` across every cycle. A mismatch on the
  rounding boundary (62.5 µL → 6 000 steps) is the watch-item.
- [ ] If HIL surfaces a non-obvious behaviour (rounding drift,
  polling-termination edge), append a Problem / Cause / Fix / Rule
  entry to [LearnedPatterns.md](LearnedPatterns.md). Don't pre-write.
- [ ] Single Conventional Commits commit per CommonClaude §11 closing
  #7; `gh pr create` per CommonClaude §15.2 template.

Memory: `project-syringe-default` saved so future demos default to
125 µL unless arithmetic clarity calls for another size.

Out-of-scope (deliberately deferred — remain pinned-absent in
`TestNoPlungerMotionExposed`):

- `abort` + `requires_reinit` latch
- `set_step_mode`
- `raw(cmds)` escape hatch

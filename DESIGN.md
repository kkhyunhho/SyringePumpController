# SY-01B Controller — Design

Companion to [CLAUDE.md](CLAUDE.md). CLAUDE.md is the **protocol/hardware reference**; this document records the **architecture decisions** for the Python controller and the rationale behind them. When the two disagree, CLAUDE.md wins on protocol facts and this document wins on code structure.

## 1. Scope & non-goals

**In scope**

- Drive a single SY-01B over a USB↔RS-232 dongle (EUSB-30, **CH340-based USB-serial bridge**) from a Python host. See §4.1 for what the CH340 means for the host setup.
- ASCII **DT** protocol only (firmware locks to first-seen variant per power cycle — mixing DT/OEM at runtime is impossible, so supporting both adds cost with no payoff for this controller).
- Synchronous, single-threaded API. One pump, one open serial handle, one in-flight command at a time.
- Programmatic API suitable for embedding in lab scripts and Jupyter notebooks.

**Out of scope (for v1)**

- Multi-pump bus arbitration. The frame format already carries the address byte, so adding a second pump later is a config-level change — but bus contention, retransmit, and concurrent `Q` polling are not designed for now.
- OEM and RUNZE binary protocols. The transport layer leaves room for them (see §4) but no implementation.
- CAN bus.
- Real-time / hard-deadline use. `time.sleep`-based polling is fine for liquid handling at human-perceivable timescales.
- GUI, web dashboard, scheduler. The library is the product; an application sits on top.

## 2. Stack decisions

| Decision | Choice | Why |
|---|---|---|
| Language | Python ≥ 3.12 | Brings PEP 695 type-alias syntax, per-interpreter GIL groundwork, faster CPython, and inherits everything from 3.11 (`Self`, `StrEnum`, `tomllib`). 3.12 is also the version with the longest remaining upstream support window when this project starts. |
| Serial library | `pyserial` | De-facto standard. Sync API matches our sync model. `serial.serial_for_url("loop://")` and `"spy://"` are useful for tests without writing a custom fake. |
| Concurrency | Synchronous, blocking | Pump executes one command at a time; the protocol requires the host to poll `Q` between commands. Async buys nothing here and complicates error/retry logic. |
| Packaging | `pyproject.toml` + `hatchling` | Minimal modern build backend, no setuptools cruft. |
| Lint/format | `ruff` (both lint + format) | One tool, fast. |
| Type checking | `mypy --strict` on the library package only | Tests can be looser. |
| Test runner | `pytest` | Standard. |

## 3. Module layout

```
src/sy01b/
    __init__.py          # public API: Pump, PumpError subclasses, enums
    transport.py         # DT framing: send bytes, read until CR/LF/ETX
    protocol.py          # command builders + reply parser + status byte decode
    pump.py              # high-level Pump class: init(), move_abs(), valve_to(), etc.
    diagnostics.py       # diagnose() flow + DiagnosticsReport (see §7)
    errors.py            # exception hierarchy mapped from pump error codes
    config.py            # PumpConfig dataclass (port, baud, address, syringe, step mode)
    cli/
        __init__.py
        diagnose.py      # `sy01b-diagnose` console script — runs §7 flow, no side effects
    _logging.py          # structured logger setup, opt-in via env var
tests/
    test_protocol.py     # pure-function tests for frame build / parse
    test_pump_fake.py    # Pump driven against a fake transport
    test_diagnostics.py  # diagnose() against scripted fake replies
    conftest.py          # fake-transport fixture
docs/
    DESIGN.md            # this file lives at repo root for visibility; longer-form docs go here
examples/
    fill_and_dispense.py
    repl_session.md
```

Rationale for the split:

- `transport.py` knows about bytes, framing, timeouts, and the serial handle. It does **not** know what commands mean.
- `protocol.py` is pure: it builds command strings and parses replies. No I/O. This is the layer with the heaviest test coverage and the one that needs to mirror the manual most faithfully.
- `pump.py` is the only place where stateful concerns live (current step mode, last issued command for `X`, syringe size for volume↔step conversion, whether we believe the pump is initialized).
- `errors.py` is small but central — every error code from the manual gets a class so callers can `except PlungerOverloadError` instead of switching on integers.

## 4. Transport layer (DT)

DT frames per [SY01BE.pdf](SY01BE.pdf) §6.1:

```
Host → Pump:  '/' <addr-byte> <command-string> CR
Pump → Host:  '/' '0' <status-byte> [<data>] ETX CR LF
```

- `<addr-byte>` is ASCII `'1'..'?'` (rotary 0–E). Stored in `PumpConfig.address`; the transport just formats it.
- No checksum. Read loop terminates on `ETX` (`0x03`) — `CR LF` follow but we don't need them to know the frame ended.
- Replies that omit data (e.g., after `R` with no report request) still carry the status byte. Position queries (`?`) interleave digits between status and ETX.

**Timeout strategy.** Reads use a short per-byte timeout (`serial.Serial(timeout=…)` configured at open time) and the read loop accumulates until ETX or a wall-clock deadline. Two deadline tiers:

- *Reply deadline* (default ~1 s): for non-move commands. Generous because USB latency on Linux can add 10–50 ms easily.
- *Move deadline*: caller-supplied for the high-level move methods, because a full-stroke aspiration at slow speed can take many seconds.

The transport never silently retries. Retransmission is OEM's job; DT has no sequence number and a duplicate `R` could re-trigger a move. On timeout we raise `TransportTimeout` and let the caller decide.

**Why a separate file from `protocol.py`:** to keep the future door open for an OEM transport or a mock transport without touching command construction. The `Transport` protocol is informal — just `send(frame: bytes) -> bytes` returning the raw reply — so duck-typed substitution works for tests.

### 4.1 USB-serial bridge: WCH CH340

The EUSB-30 dongle ships with a **WCH CH340** USB-to-serial chip (USB VID:PID `1a86:7523` for CH340G, `1a86:55d3` for CH343). This is not mentioned in [EUSB-30.pdf](EUSB-30.pdf) but is what shows up on `lsusb` once the dongle is plugged in. Practical consequences for the controller:

- **Driver.** Linux kernels ≥ 3.4 ship `ch341.ko` in-tree; the device enumerates as `/dev/ttyUSB*` with no install step. macOS and Windows need the WCH-CH34X driver. The controller assumes the kernel module is present and surfaces `FileNotFoundError` on the port path otherwise — it does not try to install drivers.
- **Latency.** CH340 has a hard-coded ~16 ms USB poll interval in many firmware revisions. Round-trip for a single ASCII frame floors around 20–30 ms regardless of how fast the host writes. This is why the reply-deadline default in §4 is 1 s rather than 10 ms: small, but enough headroom that we never falsely time out on a healthy line.
- **DTR/RTS quirk.** CH340 toggles DTR low on `open()` on most Linux drivers. The pump ignores these lines, but if the dongle's RS-232 DIP is mis-wired (e.g., RTS bridged to a logic input), an open could glitch a peripheral. The controller opens with `dsrdtr=False, rtscts=False` and explicitly drops both control lines after open. Documented as a TODO to revisit if a customer reports a glitch on open.
- **Flow control.** None — the pump speaks half-duplex framed text, not flowed bytestream. Both XON/XOFF and RTS/CTS stay off.
- **Identification on multi-port hosts.** A bench with several CH340 dongles enumerates them in arrival order, not by physical port, so `/dev/ttyUSB0` is not stable across reboots. The config accepts a `port` field but the diagnostic flow (§7) also accepts a `by-id` udev path (`/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0`) which *is* stable. The README example uses the by-id form.

## 5. Protocol layer

Pure functions and small dataclasses, no I/O:

- `build_command(cmds: str, *, execute: bool = True) -> bytes` — wraps with `/`, address, optional trailing `R`, and `CR`. Address is injected by the caller (the `Pump`) so this function stays stateless.
- `parse_reply(frame: bytes) -> Reply` — returns `Reply(status: StatusByte, data: bytes)`.
- `StatusByte` decodes the byte into `busy: bool` and `error: ErrorCode` (enum, including `OK = 0`).

Command *builders* live here as thin string helpers (`abs_move(n) -> "A{n}"`, `valve_in() -> "I"`, `valve_to(port) -> f"I{port}"`, etc.) so the parser/builder tests can pin the exact bytes that go on the wire. The `Pump` composes these into multi-command strings where appropriate (the manual encourages `IA6000OA0R`-style batching to reduce round-trips).

## 6. Pump (high-level API)

```python
from sy01b import Pump, PumpConfig, StepMode

cfg = PumpConfig(port="/dev/ttyUSB0", address=1, syringe_uL=5000, step_mode=StepMode.NORMAL)
with Pump.open(cfg) as pump:
    pump.initialize()                     # /1ZR  + wait until ready
    pump.set_stall_current_for_syringe()  # /1U200,6R for 5 mL, derived from cfg.syringe_uL
    pump.aspirate_uL(2500)                # I, A<steps>, R  (computed from syringe size + step mode)
    pump.dispense_uL(2500)                # O, A0,        R
```

Design points:

1. **Volume ↔ step conversion** lives in the Pump, not the protocol layer, because it depends on syringe size and step mode held in `PumpConfig`. Step counts are exposed via `move_to_steps()` for callers that need raw control.
2. **Busy polling** is centralized in one private method, `_wait_until_ready(deadline)`. It sends `Q`, parses the busy bit, sleeps with an exponential backoff capped at ~100 ms, and raises on deadline or error status. The manual is unambiguous: `Q` is the only reliable way to read busy/ready, so no other call path is allowed to make that decision.
3. **Error handling.** After `_wait_until_ready` reads the final status, it converts non-zero error codes to typed exceptions. Two error codes carry a recovery obligation that the Pump enforces:
   - Error 1 (init failed) and error 9 (plunger overload) flip an internal `requires_reinit` flag. Subsequent move calls raise `RequiresReinitError` until `initialize()` succeeds.
   - Error 11 (move in bypass) does *not* set the flag; the caller can recover by moving the valve.
4. **`Pump.open()` is a context manager.** `__exit__` closes the serial handle and does *not* send `T` — terminating an active plunger move requires re-initialization per the manual, and silent re-init on cleanup would hide bugs. Aborts are an explicit `pump.abort()` call.
5. **No reconnect logic.** If the serial handle drops, the Pump is dead and the caller restarts. Hot-replug while powered is forbidden by the hardware anyway.

## 7. Diagnostic / commissioning flow

**Never send `ZR` as the first command on a freshly plugged pump.** `Z` mechanically homes the plunger; if the link is mis-wired, the address-switch position is wrong, or the wrong protocol variant is in use, you'll learn that *after* the syringe has slammed into a hard stop or a closed valve. The controller exposes a **diagnostic stage** that runs only side-effect-free queries first, surfaces what it found, and refuses to proceed to `initialize()` until the operator confirms.

### 7.1 Goals

1. Prove the serial path end-to-end: bytes leave the host, the pump answers, the reply is well-formed DT.
2. Prove the addressing: the configured `address` matches the pump that is replying.
3. Prove the power rail: supply voltage is in spec.
4. Capture pre-init state for the log so a post-mortem has something to anchor on.

The diagnostic stage does **not** prove the pump is mechanically healthy — only an init can do that, and an init has side effects. The whole point of separating the stages is so that the operator decides when to commit to the side-effect step.

### 7.2 Commands used

All read-only, none require `R`:

| Step | Command | What we check |
|---|---|---|
| 1. Open | (serial open at 9600 8N1) | Port path resolves; CH340 enumerates; `dsrdtr/rtscts` disabled. |
| 2. Echo probe | `/<addr>Q\r` | Reply parses as DT (`/0` … `ETX CR LF`). Any garbled bytes here mean wrong baud, RS-485 wiring backwards, or another device on the bus. |
| 3. Status | `Q` | Decode busy-bit and error code. Pre-init expectation: error 7 (`NotInitialized`) **is the success case** — it proves the firmware is alive and honest. Error 0 with busy=0 is also acceptable (means already initialized, e.g., from a prior session). Any other code is a hard fail. |
| 4. Config readback | `?76` | Firmware/build identification logged for support tickets. |
| 5. Supply voltage | `*` | Must read ≥ 22 V (24 V nominal, allow 8 % droop). Below threshold → fail with `LowSupplyVoltageError`. |
| 6. Valve position | `?6` | Logged, used to warn if the valve is in bypass (which would block plunger moves). |
| 7. Plunger position | `?` | Logged. After a clean power cycle this is undefined; the value is informational only. |

### 7.3 API surface

```python
from sy01b import Pump, PumpConfig

with Pump.open(cfg) as pump:
    report = pump.diagnose()       # returns a DiagnosticsReport, raises on hard fails
    print(report)                  # human-readable summary, also goes to the logger
    if report.ok_to_initialize:
        pump.initialize()          # only now do we send ZR
```

`DiagnosticsReport` fields: `firmware: str`, `supply_volts: float`, `valve_position: ValveState`, `plunger_steps: int`, `pre_init_status: StatusByte`, `warnings: list[str]`, `ok_to_initialize: bool`. The fail modes raise specific exceptions (`DiagnosticTimeoutError`, `DiagnosticGarbledReplyError`, `LowSupplyVoltageError`, `WrongAddressError`) so a CLI can map them to actionable messages.

### 7.4 CLI entry point

A `sy01b-diagnose` console script (declared in `pyproject.toml`) runs the above against a config and prints a one-screen summary. Intended use: plug in the dongle, run this once, paste the output into the issue tracker if anything fails. The script never sends `R`, never sends `Z`/`Y`/`W`, and exits non-zero on any hard fail.

### 7.5 Where this fits relative to §6

`Pump.initialize()` is documented in §6 as "send `ZR` and wait." That stays true, but the recommended call order is **always** `diagnose()` → inspect → `initialize()`. The Pump does not automatically run diagnose on open (per §12 open-question #4 — auto-init would mask operator choices, and auto-diagnose would mask serial-link problems behind a single boolean). Diagnostics are an explicit step.

## 8. Errors

```
PumpError
├── TransportError
│   ├── TransportTimeout
│   └── TransportClosed
├── ProtocolError              # malformed frame, unknown status bits
└── DeviceError                # raised from a pump-reported error code
    ├── InitFailedError                 # code 1  — sets requires_reinit
    ├── InvalidCommandError             # code 2
    ├── InvalidOperandError             # code 3
    ├── NotInitializedError             # code 7
    ├── PlungerOverloadError            # code 9  — sets requires_reinit
    ├── ValveOverloadError              # code 10
    ├── PlungerBlockedByBypassError     # code 11
    └── CommandOverflowError            # code 15
```

Every `DeviceError` carries the offending command string and the raw reply bytes for log/PR debugging. Codes not listed in the manual map to `DeviceError` with the numeric code preserved on the exception, instead of being silently treated as success.

## 9. Configuration

`PumpConfig` is a frozen dataclass loaded from either kwargs or a TOML file. Fields:

| Field | Type | Notes |
|---|---|---|
| `port` | `str` | e.g. `/dev/ttyUSB0`, `COM3`. Required. |
| `baud` | `int` | Default 9600. Set to 38400 only if `U47` was previously programmed into the pump's EEPROM. |
| `address` | `int` | 1–15. Must match the rotary switch position +1. |
| `syringe_uL` | `int` | One of {25, 50, 100, 125, 250, 500, 1000, 1250, 2500, 5000}. Drives stall-current default and volume conversion. |
| `step_mode` | `StepMode` | `NORMAL` (12 000) / `FINE` (96 000) / `MICRO` (96 000). Drives volume conversion and move range checks. |
| `reply_timeout_s` | `float` | Default 1.0. |

The Pump validates `syringe_uL ∈ allowed_set` at construction. No silent rounding.

## 10. Testing strategy

Three tiers:

1. **Pure unit tests (`test_protocol.py`)** — exhaustive frame build/parse coverage, including all documented status-byte values and at least one undocumented value to confirm graceful failure. Fast, no I/O.
2. **Fake-transport tests (`test_pump_fake.py`)** — a `FakeTransport` implements the duck-typed transport interface and scripts replies. Verifies the Pump's command sequences for high-level operations (initialize, aspirate, dispense, abort), the `Q`-polling loop, and the error → exception mapping including the `requires_reinit` latch.
3. **Hardware-in-the-loop, read-only (manual, not in CI)** — a script under `examples/` that exercises the real pump using **only side-effect-free queries**: firmware/build identifier (`?76`), serial number (if exposed by firmware — see §10.1), supply voltage (`*`), status byte (`Q`), valve position (`?6`), plunger position (`?`). **The HIL test never sends `R`, never sends an init (`Z`/`Y`/`W`), never moves the plunger, never re-homes the valve.** Its job is to prove "the host can talk to the pump and read its identity," not "the pump can move." Documented as a checklist in `examples/hil_smoke.md`. Mechanical end-to-end testing is done by a human operator on the bench, not by an automated script.

### 10.1 Read-only identity probes

The HIL script collects these fields and prints them; CI never runs this script.

| Field | Source | Notes |
|---|---|---|
| Firmware string | `?76` | Already exposed by the diagnostic flow. |
| Serial number | `?76` parsed sub-field if present; otherwise the EEPROM dump command if [SY01BE.pdf](SY01BE.pdf) exposes one — confirm against the manual before relying on it. If the firmware does not expose a serial number, log "n/a" rather than failing the test. |
| Supply voltage | `*` | Float, volts. |
| Status byte | `Q` | Decoded busy + error code. |
| Valve position | `?6` | Enum (`Input`/`Output`/`Bypass`/`Extra`/port number). |
| Plunger position | `?` | Step count; informational. |
| Address byte echoed in reply | every reply's frame header | Sanity-check it matches `cfg.address`. |

The HIL run prints a single block of these values and exits zero if every probe replied within timeout. Any timeout or unexpected error code fails the run — *without* trying to "fix" anything (no auto-init, no valve nudges).

Coverage target: 90 % on `src/sy01b/`, excluding `transport.py`'s real-serial paths which can only be hit by HIL.

## 11. Logging

Single `logging.getLogger("sy01b")`. Each command sent at `DEBUG` with framed bytes; each reply at `DEBUG` with parsed status + data; `_wait_until_ready` logs the poll count and elapsed time at `INFO` if it exceeded a threshold (default 2 s). No logging at module import time. The library does not configure handlers; the application does.

## 12. Open questions

1. **Default behavior on error 10 (valve overload).** The manual says "sending another valve command re-homes the valve." Do we auto-retry once on error 10, or always raise and let the caller decide? Current plan: raise. Auto-retry hides a mechanical issue that the operator should see.
2. **`X` (repeat last command).** Useful for cycled aspirate/dispense, but maintaining "last command" state at the Pump layer duplicates pump-side state and risks divergence after a power blip. Provisionally omit from the v1 API; expose only as `pump.raw("X")` if a caller really wants it.
3. **Multi-pump.** When the second pump arrives, do we share one serial handle across `Pump` instances (cleaner, requires a small bus arbiter) or open one handle per pump (simpler, only works on RS-232 point-to-point — not on the shared RS-485 bus the device targets)? Defer until there's actually a second pump.
4. **Boot-time auto-init.** Should `Pump.open()` automatically send `ZR` and wait? Convenient but masks the operator's choice of init flags (`Z<n1,n2,n3>` controls force and speed). Provisionally: no auto-init; require an explicit `initialize()` call. The cost is one extra line; the benefit is no surprises.

## 13. Out-of-band notes

- The repo is greenfield: there is no `pyproject.toml`, `src/`, or `tests/` directory yet. This document is the spec; the next commit creates the skeleton.
- After the skeleton lands, update [CLAUDE.md](CLAUDE.md) §"When code lands here" with the actual `pytest` / `ruff` / `mypy` invocations and remove the placeholder section.

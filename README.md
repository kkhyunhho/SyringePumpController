# SyringePumpController

Python driver for the **Runze SY-01B Smart Syringe Pump** over RS-232 via a CH340-based USB-to-serial bridge (EUSB-30 dongle). Speaks the **DT ASCII** protocol.

Companion to [CLAUDE.md](CLAUDE.md) (protocol/hardware reference) and [DESIGN.md](DESIGN.md) (architecture rationale).

## Status

Pre-alpha (`0.2.0.dev0`). The following surfaces are shipped and HIL-verified on the CH340 dongle (USB identity `1A86:7523`, firmware `8.33`, serial `32656`, 125 µL syringe):

- **Read-only commissioning** — `diagnose()`, identity / status / voltage / valve / plunger queries, `sy01b-diagnose` CLI.
- **Valve motion** — `initialize_valve`, `set_valve_position`, `move_valve_to_port`.
- **Plunger initialization** — `initialize` (`Z<force>R` / `Y<force>R`, polls `?6 != "?"` for completion). Stall current (`U200,n`) must be set out-of-band before connecting this driver — see [CLAUDE.md](CLAUDE.md) "Stall current" section.
- **Plunger absolute moves** — `move_to_steps` (`A<n>R`, polls `?` until target).
- **Plunger volume moves** — `aspirate_uL(target_uL)` / `dispense_uL(target_uL=0)` — absolute contained-volume targets converted via `Config.syringe_uL` and `Config.step_mode.full_stroke_steps`; thin wrappers over `move_to_steps`.

`abort` + `requires_reinit` latch and `set_step_mode` remain intentionally absent. The test suite pins this boundary via `TestNoPlungerMotionExposed` / `TestPlungerMotionPresent` in [tests/test_plunger_motion_absent.py](tests/test_plunger_motion_absent.py).

## If you've never used a syringe pump before

The SY-01B is a programmable precision dispenser. A stepper-driven **plunger** moves a syringe up and down to aspirate or dispense fluid in the nL–mL range; a motor-driven **valve** routes the fluid between an input port, an output port, and (sometimes) a bypass. A host PC drives both over serial.

Before plugging anything in:

- **Power:** 24 V DC, ≥ 1.5 A on the DB-15 header. Never connect or disconnect the pump while powered.
- **Serial:** default 9600 bps, 8N1. The EUSB-30 (CH340) dongle enumerates as `/dev/ttyUSB*` on Linux; identify it by USB identity `1A86:7523` rather than the arrival-order index (see "Finding the serial port").
- **Address switch:** 16-position rotary (0–F) on the pump body sets the bus ID — position 0 → address `1`, position 1 → `2`, …, position E → `15`. Must be set **before** power-on. Position F is self-test.
- **Syringe size:** declare the installed syringe volume (µL) in code (`Config.syringe_uL`). Stall current must already match this size at the firmware level — set it once via a terminal session with `/1U200,<n>R` per the table in [CLAUDE.md](CLAUDE.md) before running this driver.
- **Diagnose first, move second.** The `diagnose()` API and `sy01b-diagnose` CLI never emit `R`/`Z`/`Y`/`W` — they verify wiring, address, voltage, and identity without moving anything. Run this before any motion code.

## Communication protocol (DT ASCII)

The SY-01B locks to the first ASCII variant it sees per power cycle (DT vs. OEM). This codebase emits **only DT** frames — no checksum, framed by `\r` and `ETX`.

### Host → pump (request)

```
   /      1        <commands>      \r
  0x2F  '1'..'?'   ASCII string   0x0D
        address    command body    CR
```

- **`/`** — every request starts with slash.
- **Address byte** — integer pump address 1..15 mapped to ASCII via `SyringePumpController.format_address`:
  - 1 → `'1'` (0x31), …, 9 → `'9'`, 10 → `':'` (0x3A), …, 15 → `'?'` (0x3F).
- **Command body** — concatenated ASCII commands. E.g. `IA6000OA0R` = input port → aspirate to step 6000 → output port → dispense to step 0.
- **Execute trigger `R`** — motion commands (`Z`, `Y`, `A`, `P`, `D`, `I`, `O`, …) only run when the body ends in `R`. Without `R` the command sits in the pump's 255-byte buffer waiting for the next `R`. Report/query commands (`Q`, `?23`, `?202`, `*`, …) do not need `R`.
- **`\r`** — single carriage return terminates the frame.

The builder is `SyringePumpController.build_command(address, cmds, *, execute=False)`. Only `execute=True` appends `R`, and the read-only call sites always pass `execute=False`.

> **Codebase safeguard:** the diagnostic path never appends `R`/`Z`/`Y`/`W`. The test `test_diagnose_never_sends_R_or_init_command` inspects every transmitted frame to enforce this.

### Pump → host (reply)

```
   /     0    <status>   <data...>    ETX    \r    \n
  0x2F  '0'   1 byte      ASCII      0x03   0x0D  0x0A
        host
       master
```

- **`/0`** — replies always carry slash + ASCII `'0'` (host = master address). Anything else raises `ProtocolError`.
- **Status byte** — single byte, bit-mapped. Decoded by `SyringePumpController.StatusByte.decode`:

  | Bit | Meaning |
  |---|---|
  | 7 | always `0` |
  | 6 | always `1` (frame identifier) |
  | 5 | `1` = busy, `0` = ready |
  | 4 | reserved |
  | 3..0 | **error code (0 = OK)** |

  Examples: `0x40` = ready + OK, `0x60` = busy + OK, `0x47` = ready + "not initialized" (error 7).

- **Data** — payload depends on the command: `?23` → version string, `?202` → serial number, `*` → supply voltage × 10 (integer), `Q` → empty.
- **ETX (0x03)** — end-of-data. The driver reads until ETX or `reply_timeout_s`.
- **`\r\n`** — trailing CRLF.

### Error codes (status byte bits 3..0)

| Code | Meaning | Recovery |
|---|---|---|
| 0 | OK | — |
| 1 | Init failed | Clear blockage, **must re-init**. Pump rejects everything until cleared. |
| 2 | Invalid command | Send a valid command. No re-init. |
| 3 | Invalid operand | Fix the parameter. No re-init. |
| 7 | Not initialized | Send `Z`/`Y`/`W`. |
| 9 | Plunger overload (backpressure) | **Must re-init** before further moves. |
| 10 | Valve overload | Next valve command auto-homes; repeated → valve needs replacement. |
| 11 | Plunger move blocked (valve in bypass) | Move valve off bypass first. |
| 15 | Command buffer overflow (sent a move while still moving) | Poll `Q` until ready. |

Each code maps to a subclass of `SyringePumpController.Error` (`InitFailedError`, `PlungerOverloadError`, `CommandOverflowError`, …). `SyringePumpController.device_error_for(code)` exposes the mapping.

> **Bus rule:** the manual designates `Q` as the canonical busy/ready signal. On the bench pump's firmware 8.33, however, `Q.busy` is **permanently latched True** after the first valve home and never clears ([LearnedPatterns E5](LearnedPatterns.md)). This driver polls position queries (`?6` for valve, `?` for plunger) instead — see [LearnedPatterns E5 / E7](LearnedPatterns.md).

### Frames you'll see most often

| Sent | Meaning | Reply (example) | Notes |
|---|---|---|---|
| `/1Q\r` | Query pump 1 status | `/0` + status + ETX | Busy/ready + error. **No `R`.** |
| `/1?23\r` | Software version | `/0` + status + `8.33` + ETX | Cheapest connectivity check. |
| `/1?202\r` | Serial number | `/0` + status + `32656` + ETX | Good first roundtrip. |
| `/1*\r` | Supply voltage × 10 | `/0` + status + `240` + ETX | `240` → 24.0 V. Diagnose fails below 22 V. |
| `/1?6\r` | Valve position | `/0` + status + `I`/`O`/digit + ETX | Pre-init returns the literal byte `?` (LearnedPatterns E3). On a 4-way-configured pump, post-init returns ASCII digit `1..4`. |
| `/1?\r` | Plunger position (steps) | `/0` + status + integer + ETX | 0..12000 in `N0`. |
| `/1U200,5R\r` | Set stall current for 50 µL – 1.25 mL syringe | `/0` + status + ETX | EEPROM-persistent; effective on the next power-up. Set out-of-band before using this driver — not exposed as a public method. |
| `/1Z2R\r` | **Initialize** (CW, third force) | `/0` + status + ETX | Canonical first motion. After `Z`, poll `?6` until it stops returning `?` (LearnedPatterns E7). |
| `/1A6000R\r` | Move plunger to absolute step 6000 | `/0` + status + ETX | Post-init top speed is 4000 pps → full stroke ≈ 3 s (LearnedPatterns E8). |
| `/1I3R\r` | Move valve CW to distribution port 3 | `/0` + status + ETX | After move, poll `?6` until it equals `"3"`. |

## Install

Requires Python ≥ 3.12. All projects share one conda env, **`elec`**
(new terminals activate it); install this package editable into it:

```bash
conda activate elec
pip install -e ".[dev,server]"
```

## Finding the serial port

The driver resolves the port by USB identity, so prefer the `VID:PID`
form (`"1A86:7523"`) over a `/dev/ttyUSB*` index, which renumbers across
reboots and USB re-plugs. To list attached USB serial devices:

```bash
python -m serial.tools.list_ports -v   # shows VID:PID per port
ls -l /dev/ttyUSB*
# If absent, check kernel logs for the CH340 attach:
dmesg | tail
```

`Permission denied` on the port usually means the user is missing the `dialout` group.

## First run — read-only diagnose

The safest first action: verify wiring, voltage, and identity without moving anything.

### CLI

```bash
sy01b-diagnose --port 1A86:7523 --address 1 --syringe-uL 125
```

Successful output (bench pump, 2026-05-18):

```
SY-01B diagnostic report
  software version : 8.33
  serial number    : 32656
  config           : 4 way|9600|100K|TSY|high|XLP|AUTO
  supply voltage   : 24.0 V
  valve position   : 4
  plunger position : 0 steps
  pre-init status  : busy=True error=OK
  ok to initialize : True
```

`valve position` returns the literal `?` byte until the pump is initialized; afterwards it returns either a digit (`1..N`) on a distribution-configured valve or one of `i`/`o`/`b`/`e` on a non-distribution valve. `busy=True` on a *non*-`Q` reply or in pre-init contexts is firmware-quirky and not a reliable busy signal — see [LearnedPatterns E4 / E5](LearnedPatterns.md).

### Python

```python
from sy01b import SyringePumpController

cfg = SyringePumpController.Config(
    port="1A86:7523",  # USB VID:PID — survives /dev renumber; an explicit path also works
    address=1,
    syringe_uL=125,
)

with SyringePumpController.open(cfg) as pump:
    report = pump.diagnose()
    print(report.render())
```

[main.py](main.py) is an end-to-end tutorial that exercises every shipped public method on real hardware (diagnose → identity queries → `initialize` → `aspirate_uL`/`dispense_uL` max/mid/min → `move_valve_to_port` 1↔3↔1). Narrower per-feature bench scripts live in [claude_test/](claude_test/) (see [Bench scripts](#bench-scripts)).

## When diagnose fails

| Symptom | Likely cause |
|---|---|
| `DiagnosticTimeoutError` (no reply) | Wrong port, pump unpowered, dongle LED off, cable, or **address switch ≠ `cfg.address`**. |
| `LowSupplyVoltageError` (< 22 V) | Underpowered PSU, loose DB-15. |
| `valve is in bypass` warning | Plunger moves will trip error 11 until the valve leaves bypass. |
| `pre-init status` = `NOT_INITIALIZED` | **Normal** right after power-on. Init lands in a later commit. |
| `DiagnosticGarbledReplyError` | Another serial client is on the port, or the pump locked into OEM/RUNZE this power cycle — power-cycle and let the driver send DT first. |

## What's implemented

Everything is reachable from a single import: `from sy01b import SyringePumpController`.

**Protocol & framing**
- DT ASCII frame builder/parser and status-byte decode (`build_command`, `parse_reply`, `StatusByte`).
- Pump error code → exception mapping under `SyringePumpController.Error` (`DeviceError`, `InitFailedError`, `PlungerOverloadError`, `CommandOverflowError`, …).
- Transport- and diagnostic-layer exceptions (`TransportError`, `ProtocolError`, `DiagnosticError`).

**Configuration**
- `SyringePumpController.Config` — frozen dataclass with TOML loader and step-mode → full-stroke-step lookup.

**Read-only queries** (no `R` ever appended)
- `query_status` (`Q`), `query_software_version` (`?23`), `query_serial_number` (`?202`), `query_config` (`?76`), `query_supply_voltage_v` (`*`), `query_valve_position` (`?6`), `query_plunger_position` (`?`).
- `diagnose()` → `DiagnosticsReport` and `sy01b-diagnose` console script (both refuse to send `R`/`Z`/`Y`/`W`).

**Valve motion**
- `initialize_valve(home_port, direction_ccw)` — valve-only home (`w<port>,<dir>R`), polls `?6` until non-`?`.
- `set_valve_position(I/O/B/E)` — non-distribution shortest-path move.
- `move_valve_to_port(port, direction_ccw)` — distribution CW (`I<n>R`) / CCW (`O<n>R`), polls `?6` until target.

**Plunger initialization & step moves**
- `initialize(*, force, ccw, settle_timeout_s)` — `Z<force>R` / `Y<force>R`, polls `?6` until non-`?` (LearnedPatterns E7). Stall current must already be set in EEPROM for the installed syringe — see [CLAUDE.md](CLAUDE.md) "Stall current" section.
- `move_to_steps(steps, *, settle_timeout_s, poll_interval_s)` — `A<n>R`, polls `?` until target matches.

**Plunger volume moves**
- `aspirate_uL(target_uL, *, settle_timeout_s, poll_interval_s)` — absolute "fill to `target_uL` µL"; converts via `round(target_uL / Config.syringe_uL * full_stroke_steps)` and delegates to `move_to_steps`.
- `dispense_uL(target_uL=0, *, settle_timeout_s, poll_interval_s)` — same conversion + delegation, named for the emptying direction; default `0` is the common "fully dispense" case.

**Other**
- `wait_until_ready()` — `Q`-polling with backoff. Retained for parity with the manual; unreliable on firmware 8.33 (LearnedPatterns E5).

**Remote HTTP bridge** (Phase A of the ESP32 controller initiative, [#10](https://github.com/kkhyunhho/SyringePumpController/issues/10))
- [server/](server/) — thin FastAPI bridge exposing the driver over `/v1/*` JSON for a remote ESP32-S3 client. Run `sy01b-server --config server/pump.toml` (binds `0.0.0.0:17046`, host PC LAN IP `192.168.1.129` on this bench). Single uvicorn worker + `asyncio.Lock` preserves the driver's single-in-flight contract; long ops (prime ~30 s) block the issuing request. See [server/README.md](server/README.md) for endpoint catalog and error mapping.

**ESP32-S3 client firmware** (Phases B + C of the ESP32 controller initiative, [#12](https://github.com/kkhyunhho/SyringePumpController/issues/12) + [#15](https://github.com/kkhyunhho/SyringePumpController/issues/15))
- [firmware/](firmware/) — ESP-IDF v5.3+ project (verified through v6.0.1) for the ESP32-S3-BOX-3 (LVGL touch dashboard). All four tabs live: **Valve** (Port 1–4 buttons), **Move** (slider 0–125 µL + Aspirate / Dispense to absolute target), **Prime** (port 3 → port 1 single-cycle), **Status** (2 s refresh). FSM gates motion behind `NEEDS_INIT → READY`; `PlungerOverloadError` / `InitFailedError` flip the `requires_reinit` latch and require an explicit Re-initialize. Managed components pinned at `espressif/esp-box-3 ^3.2` (transitively LVGL 9.5) + `espressif/cjson ^1.7`; `ui.c` targets the LVGL 9.x msgbox / spinner APIs. Build / flash recipe, the `CONFIG_ESP_MAIN_TASK_STACK_SIZE=12288` requirement, and the boot-order dependency (server up before ESP32) are in [firmware/README.md](firmware/README.md).

## What's not yet implemented

The remaining plunger-side surface (see [ToDo.md §6](ToDo.md)):

- `abort()` — `TR` plus the `requires_reinit` latch (errors 1 and 9 also set it).
- `set_step_mode(mode)` — `N0`/`N1`/`N2` configuration.
- `raw(cmds)` — escape hatch for commands not modelled above.

Read-only HIL identity probes (`claude_test/hil_smoke.md`, `hil_identity.py`) are tracked in [#4](https://github.com/kkhyunhho/SyringePumpController/issues/4); doc hygiene (`claude_test/repl_session.md`, `CHANGELOG.md`) in [#5](https://github.com/kkhyunhho/SyringePumpController/issues/5).

## Bench scripts

[claude_test/](claude_test/) holds debug and bench-verification scripts that drive real hardware (per the CommonClaude debug-file rule). They are **not** part of CI — production unit tests live in [tests/](tests/). The index with HIL findings is in [claude_test/README.md](claude_test/README.md).

| Script | Purpose |
|---|---|
| [valve_toggle.py](claude_test/valve_toggle.py) | Toggle a distribution valve between two ports (default 1 ↔ 3) and verify each move via `?6`. Plunger never moves. |
| [plunger_cycle.py](claude_test/plunger_cycle.py) | After init, cycle the plunger through max → mid → min absolute contained volumes via `aspirate_uL`/`dispense_uL` for N cycles, verifying each move against the converted step count. |

## Develop

```bash
conda activate elec                                       # shared env
ruff check src tests claude_test server main.py            # lint
ruff format --check src tests claude_test server main.py   # format check
mypy                                                       # strict types on src/sy01b + server
pytest                                                     # full suite (incl. tests/server/)
pytest --cov=sy01b --cov=server --cov-report=term-missing
```

Bench-learned lessons are collected in [LearnedPatterns.md](LearnedPatterns.md). Workflow conventions (claude_test/ vs tests/, CommonClaude reference, ToDo + GitHub-issue policy) are in [CLAUDE.md](CLAUDE.md).

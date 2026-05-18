# LearnedPatterns.md

> Patterns extracted from [ToDo.md](ToDo.md) Completed items. Consult the relevant sections before drafting new ToDo entries. Append new patterns after each task completes.
>
> Last updated: 2026-05-18
> Total patterns: 17
>
> Provenance format: `(from ToDo#N)` where N is the 1-based index of the top-level `##` section in `ToDo.md` at the time of extraction. Patterns extracted from design rather than from completed work use `(from DESIGN.md §N)` until a corresponding ToDo item lands.

---

## §1. Recurring Issues

*(none yet — populate as failure modes recur during implementation)*

---

## §2. Solved Gotchas

### G1. CH340 USB-serial bridge drops DTR low on `open()`

- **Problem**: A bare `serial.Serial(port=…)` against the EUSB-30 dongle toggles DTR low at the moment the port opens. The SY-01B ignores DTR/RTS, but any peripheral wired into the dongle's DB-9 control pins could glitch on the transition.
- **Cause**: The Linux `ch341.ko` driver and many CH340 driver implementations on other platforms assert DTR=low on open by default; the chip echoes the line state on the RS-232 side.
- **Fix**: Open with `dsrdtr=False, rtscts=False, xonxoff=False`, and explicitly drop both DTR and RTS after the handle exists. Documented in [DESIGN.md §4.1](DESIGN.md#41-usb-serial-bridge-wch-ch340).
- **Rule**: Never open a CH340-based dongle without first neutralizing DTR/RTS, even when the protocol itself does not use them. (from DESIGN.md §4.1)

### G2. Firmware locks to the first ASCII variant it sees per power cycle

- **Problem**: Mixing DT and OEM ASCII frames in the same session corrupts state — the second variant gets rejected with no clean recovery short of a power cycle.
- **Cause**: The SY-01B auto-detects ASCII variant on the first command after boot and refuses any other variant until the next power cycle (per [SY01BE.pdf](SY01BE.pdf) §6).
- **Fix**: Pick one variant (this project uses DT) and enforce it at the transport layer. Don't build a "transport-agnostic" frame builder that could accidentally emit the other variant.
- **Rule**: Treat ASCII-variant choice as a project-wide constant, not a per-command parameter. A "switch protocol mid-session" feature is a footgun, not a feature. (from DESIGN.md §1)

### G3. `bytearray[i:j]` is a `bytearray`, not `bytes` — mypy catches the slip

- **Problem**: `while end < len(buf) and buf[end:end+1] in (b"\r", b"\n"):` mypy-failed with `comparison-overlap` because `buf` was `bytearray` and the slice is `bytearray`, not `bytes`; tuple membership against `bytes` literals never holds.
- **Cause**: bytes/bytearray are distinct types under mypy strict, even though they compare equal at runtime in many situations.
- **Fix**: Compare integer bytes: `buf[end] in (0x0D, 0x0A)`. Simpler, faster, and types correctly.
- **Rule**: When walking a `bytearray` read buffer, index it for single-byte comparisons; slice it only when you need a sub-buffer. (from ToDo#0, ToDo#4)

### G4. Frozen + slots dataclass has no `__dict__` — use `dataclasses.replace()`

- **Problem**: Tried to merge CLI overrides into a TOML-loaded `PumpConfig` with `PumpConfig(**{**cfg.__dict__, **overrides})`. mypy ignored the type:ignore as "unused" because `cfg.__dict__` would raise `AttributeError` at runtime anyway (slots).
- **Cause**: `@dataclass(frozen=True, slots=True)` instances have no `__dict__`. Copy-with-overrides is `dataclasses.replace(cfg, **overrides)`, which is also more readable.
- **Fix**: `cfg = dataclasses.replace(cfg, **overrides)` in `cli/diagnose.py`.
- **Rule**: For frozen dataclasses, the supported "modify a field" idiom is `dataclasses.replace()`, never `__dict__` reconstruction. (from ToDo#7)

### G5. CH340 emits a stray byte (often `0xFF`) before the first reply after open

- **Problem**: First HIL run (`main.py` against `/dev/ttyUSB1`) failed with `ProtocolError: reply missing leading '/': b'\xff/0\`8.33\x03\r\n'`. The pump's actual reply was well-formed DT starting at byte 1; byte 0 was a stray `\xff`.
- **Cause**: CH340 USB-serial bridges occasionally emit a stray byte (commonly `0xFF`, sometimes NUL) on the very first reply after a port open, while the chip's UART receiver settles. The pump did not send this byte — the dongle injected it. This is not in the EUSB-30 manual but is a documented CH340 quirk.
- **Fix**: `DTTransport.send` now finds the first `/` in the buffer and drops anything before it; `parse_reply` stays strict (still rejects malformed frames). Strip happens only at the transport boundary so the parser remains a useful sentinel for genuinely corrupt frames. Pinned by `TestLeadingGarbageTolerance` in `tests/test_protocol.py`.
- **Rule**: Always treat the first reply after open as potentially preceded by line garbage on a CH340 dongle. Strip pre-start bytes at the transport layer, never silently in the parser. (from ToDo#4, ToDo#10 HIL)

### G6. Frozen+slots dataclass nested in another class can't reference enclosing types for defaults

- **Problem**: `step_mode: Pump.StepMode = Pump.StepMode.NORMAL` inside `class Pump.Config` failed at class-definition time because the outer name `Pump` did not yet exist when the body of `Config` ran. (Python's class body scopes do *not* enclose nested class bodies the way function scopes enclose nested functions.)
- **Cause**: Field-default expressions on `@dataclass` are evaluated when the dataclass class is being constructed — which is *during* the outer class's body, before the outer class has been bound to its name. `Pump.StepMode.NORMAL` is therefore unresolvable. Same problem applies to any class-attribute access on the enclosing class for use as a default.
- **Fix**: Use `field(default_factory=lambda: Pump.StepMode.NORMAL)`. The factory closure is evaluated at instance-construction time, by which point `Pump` is fully defined.
- **Rule**: When nesting a frozen+slots dataclass inside another class, any default that references an enclosing-class attribute must go through `field(default_factory=lambda: ...)`. Bare-name defaults from the *same* class body (e.g. `_DEVICE_ERROR_BY_CODE = {...}` after every DeviceError subclass is defined) still work because they resolve in the body's local namespace. (from ToDo#0, consolidation refactor)

---

## §3. Library Quirks

### Q1. pyserial 3.x dropped the `setDTR()`/`setRTS()` methods

- **Problem**: `port.setDTR(False)` / `port.setRTS(False)` worked at runtime but mypy reported `"Serial" has no attribute "setDTR"` with `types-pyserial` 3.5.x stubs.
- **Cause**: pyserial 3.x exposes DTR/RTS as **properties** (`port.dtr = False`, `port.rts = False`). The old camelCase methods are still around for compatibility but are not in the type stubs, and using them would be deprecated.
- **Fix**: Use the property setters: `port.dtr = False; port.rts = False`. Matches the documented pyserial 3.x API and types correctly.
- **Rule**: When neutralizing CH340 DTR/RTS on open, use the property syntax (`port.dtr = False`), never the legacy `setDTR()` method. (from ToDo#4)

---

## §4. Workflow Lessons

### W1. Always run `diagnose()` before `initialize()` on a freshly plugged pump

- **Lesson**: `Z` (init) mechanically homes the plunger. If the serial link is mis-wired, the rotary address switch is set wrong, or the wrong ASCII variant is locked in, a blind `ZR` as the first command can slam the plunger into a hard stop or a closed valve. The diagnostic stage (echo `Q`, `?76`, `*`, `?6`, `?`) confirms communication, addressing, and power *without* moving anything.
- **Rule**: Always call `pump.diagnose()` first, inspect the `DiagnosticsReport`, and only call `pump.initialize()` after the report's `ok_to_initialize` is true. Never auto-init from `Pump.open()`. Document this order in every example and README snippet. (from DESIGN.md §7)

### W2. HIL tests are read-only — never move the plunger or valve from automation

- **Lesson**: Hardware-in-the-loop scripts that drive the real pump must restrict themselves to side-effect-free queries (firmware/build via `?76`, serial number if exposed, supply voltage `*`, status `Q`, valve `?6`, plunger position `?`). Motion testing is a separate, human-supervised activity on the bench. An automated script that moves a syringe risks damaging the plunger, the valve, or whatever fluid line is connected — and is the wrong abstraction for proving "the host can talk to the pump."
- **Rule**: Every HIL-tier test and `claude_test/hil_*.py` script proves identity, not motion. The `sy01b-diagnose` CLI must refuse to emit `R`/`Z`/`Y`/`W` by code, not by convention. (from DESIGN.md §10.1)

### W3. Use specific identity commands (`?23`, `?202`), not the broad config dump (`?76`)

- **Lesson**: It is tempting to read everything off `?76` (pump configuration) and parse fields out of it. The manual exposes **dedicated** commands for the two identity fields that matter most: `?23` (or `&`) returns the firmware/software version string, and `?202` returns the unique device serial number. Querying these directly is more reliable than parsing a multi-field config blob whose layout differs across firmware revisions.
- **Rule**: For identity verification, prefer single-purpose query commands over multi-field dumps. `?76` is useful for log context, but `?23` and `?202` are the source of truth for version and serial number respectively. (from ToDo#7)

### W4. Defensive test asserting "method is absent" guards future scope creep

- **Lesson**: The read-only commit shipped a `TestNoMotionCommandsExposed` class that asserts `not hasattr(pump, "initialize")`, `aspirate_uL`, `abort`. Looks weird (you don't usually test for absence), but it caught one local-branch experiment that prematurely added an `initialize()` method and would have shipped if not for this guard.
- **Rule**: When a public API is intentionally narrow at a milestone, add tests that assert the *negative* — "this method does NOT exist yet" — so accidental additions land as test failures rather than as silent feature creep. (from ToDo#6)

### W5. FakeTransport tests are necessary but not sufficient — always close the loop with HIL

- **Lesson**: 104 unit tests, including a `test_identity.py` suite that "verified" software version + serial number retrieval, all passed against a `FakeTransport` whose replies I wrote myself based on the manual. The first HIL run against a real pump immediately surfaced two facts the fakes could not: (1) the CH340 dongle prefaces the first reply with `0xFF`, and (2) the real firmware reports `8.33` for `?23`, not the manual's `V1.4`. Calling FakeTransport-based tests "verification" without a HIL pass overstates what the green tests prove.
- **Rule**: Distinguish "software-path verified" (FakeTransport tests pass) from "hardware-verified" (a real-pump run produced the expected behavior). Use the former label only for the former state. Run a HIL probe before promising the latter to anyone. (from ToDo#10 HIL)

### W6. A targeted refactor surfaces latent bugs that the original author missed

- **Lesson**: While moving `DTTransport.send` into `Pump._send_and_receive`, a rename of the local `start` variable surfaced that the original code rebinds the same name to two completely different values: a `time.monotonic()` wall-clock anchor at loop entry, and a `buf.find(b"/")` byte offset inside the ETX-found branch. The function happened to return immediately after the second binding, so no active bug — but a future edit that moved the timeout check below the second binding would have silently broken timeout enforcement. The bug only became visible because the refactor forced us to name the *roles* (`deadline_anchor` vs `frame_start`), not the local placeholders.
- **Rule**: When moving code, rename ambiguous locals to describe their semantic role even if the original code "works." A variable whose lifetime spans two different semantic roles is a refactoring footgun; surfacing it during a consolidation is an unforced win. (from consolidation refactor)

---

## §5. Environment Specifics

### E1. SY-01B firmware reports a numeric version for `?23`, not the manual's `V1.x` strings

- **Note**: Manual [SY01BE.pdf](SY01BE.pdf) Chapter 8 lists pump versions as `V1.0`, `V1.3`, `V1.4` — but those are the **product version** (the manual's own revision history), not what the firmware emits over the wire. Empirically (`main.py` on the lab pump, 2026-05-15), `?23` returned `8.33`. The serial-number command `?202` returned a 5-digit integer (`32656`), not a structured "RZ-..." string.
- **Rule**: Treat manual content tables as documentation of capabilities and command names, but never as oracles for *response strings*. Write parsers against the documented framing only; trust the wire for the actual payload format. When the FakeTransport scripts a "realistic-looking" reply for a test, mark that string as illustrative-only, not a contract. (from ToDo#10 HIL)

### E2. `?76` config reply is a pipe-delimited 7-field blob

- **Note**: Empirically (`sy01b-diagnose` against the lab pump on 2026-05-15), `?76` returns `4 way|9600|100K|TSY|high|XLP|AUTO`. The fields appear to mean: valve type (`4 way` = 4-port distribution), baud (`9600`), some capacity/buffer marker (`100K`), motor encoder mode (`TSY`), power level (`high`), motor protocol (`XLP`), boot mode (`AUTO`). The manual documents `?76` only as "Report pump configuration" with no field-by-field layout, so this is the first concrete record of what the format actually looks like.
- **Rule**: Do not try to parse `?76` fields by index until a second pump confirms the layout is stable across units and firmware versions. For now, log the raw blob into the `DiagnosticsReport.config` field as-is and use `?23`/`?202` for any identity-driven logic. (from consolidation HIL)

### E3. Pre-init `?6` (valve position) returns the literal byte `?`

- **Note**: After power-on and before `ZR`, the lab pump answered `?6` with the data byte `?` (ASCII 0x3F). This is neither a numeric port (1..N) nor one of the documented `I`/`O`/`B`/`E` codes. The firmware appears to use `?` to mean "valve position unknown until initialized" — consistent with the SY-01B's design that valve indexing is meaningless until the encoder homes.
- **Rule**: `pump.query_valve_position()` must tolerate the literal `?` return and not try to coerce it into an enum. The `diagnose()` flow already passes it through as a string into `DiagnosticsReport.valve_position`; do not tighten that type to an enum or to `int`. (from consolidation HIL)

### E4. `Q` reports `busy=True, error=OK` pre-init even though the pump is mechanically idle

- **Note**: After power-on and before `ZR`, the lab pump's `Q` reply decoded to `busy=True, error=OK` (status byte `0x60`). The pump is not actually moving — it is sitting uninitialized — yet the busy bit is set. CLAUDE.md already warns that the busy bit on *non-Q* replies is unreliable; this run shows the bit on `Q` itself is also misleading in the pre-init state.
- **Rule**: Do not use `Q`'s busy bit alone to decide "is it safe to send a move?" in pre-init contexts. The diagnostic flow's `ok_to_initialize` criterion correctly looks only at the error nibble (`error in {OK, NOT_INITIALIZED}`), not at busy. When `_wait_until_ready` is implemented for motion commands, treat the busy bit as informative only on `Q` replies that follow a known motion command, and even then cross-check against elapsed time. (from consolidation HIL)

### E5. `Q` busy bit is permanently True on firmware 8.33 — even post-init, even when buffer is empty

- **Problem**: The first valve toggle attempt against the real pump returned `CommandOverflowError` (error 15) when `IR` was sent immediately after `wait_until_ready` reported ready. Cross-checks (`?10`=`0`, `?6` stable, motor silent) confirmed the pump was mechanically idle, yet `Q` continued reporting `busy=True, error=OK` (status byte `0x60`) indefinitely.
- **Cause**: Firmware 8.33 latches `Q.busy` on after the first valve home and never clears it, even when the command buffer is empty and no motion is in progress. This extends `E4` (pre-init `Q` lies) into post-init: `Q.busy` is unreliable in all phases on this firmware.
- **Fix**: Added `SyringePumpController._wait_for_valve_position(target)` that polls `?6` until the target port is observed; called by `move_valve_to_port` after each motion. `initialize_valve` polls `?6` until it stops returning the literal `?` (E3). The public `wait_until_ready` is retained for parity with the manual but its docstring now flags the unreliability.
- **Rule**: Never use `Q.busy` as the sole motion-completion signal on firmware 8.33. For valve moves, poll `?6` against the expected port; for valve init, poll `?6` until non-`?`. For future plunger work, prefer elapsed-time bounds + `?` (plunger position) over `Q.busy`. (from ToDo#6)

### E6. Firmware 8.33 treats the attached MCC-4 as a 4-way distribution valve

- **Problem**: User's bench valve is a Runze MCC-4 (non-distribution, two rotor states: `C-1 & 2-3 connected` and `C-3 & 1-2 connected`). The manual claims non-distribution valves report `?6` as `i`/`o`/`b`/`e` and accept bare `I`/`O`/`B`/`E`. The user-approved plan toggled `IR ↔ OR` against this assumption. On the real pump it produced wrong physical states.
- **Cause**: Firmware 8.33's `?76` reports `4 way|9600|100K|TSY|high|XLP|AUTO` — a distribution-valve configuration set at the factory independent of which physical valve is attached. `?6` then answers with ASCII digit `1..4`. Bare `IR` resolves to "default input port" (port 1) and `OR` to "default output port" (port 4), neither matching the MCC-4's C-1 / C-3 dual-selection states.
- **Fix**: Added `SyringePumpController.move_valve_to_port(port, *, direction_ccw)` using distribution syntax `I<n>R`/`O<n>R`. `claude_test/valve_toggle.py` toggles port 1 ↔ port 3 (the digits that correspond to the MCC-4's C-1 and C-3 states); HIL run verified 20/20 moves with `?6` returning `'1'`/`'3'`.
- **Rule**: Never assume `?76`'s valve-type field matches the physically attached valve. When `?6` returns digits, use `move_valve_to_port(n)` (distribution syntax); when it returns letters, use `set_valve_position(ValvePosition.X)`. For MCC-4 on a 4-way-configured pump, port 1 ↔ port 3 is the right toggle pair. (from ToDo#6)

### E7. `Z` init completion signal: poll `?6 != "?"`, not `? == 0`

- **Problem**: First HIL run of `claude_test/syringe_init.py` returned in 0.10 s reporting init success — but `?6` came back as the literal `?` byte (meaning init was still in progress), and the plunger had not actually moved through a full stroke. The "verified" run was a false positive.
- **Cause**: Z mechanically homes the plunger to top-of-stroke and back to position 0, then homes the valve. `initialize()` was polling `?` (plunger position) for `== 0` as the completion signal. When a prior session left the plunger at 0, that condition is satisfied trivially on the first poll — long before the firmware has even started executing the Z command. `?` does not transition through intermediate values reliably enough to be a safe completion signal when pre-init position is already 0.
- **Fix**: Switched the `initialize()` poll target to `?6` (valve position), waiting for it to stop returning the literal `?` byte (the same pattern as `initialize_valve`). The firmware sequences plunger then valve, so `?6` becoming a real port number is the unambiguous "Z complete" signal. After the fix, HIL init runs take ~3.86 s on the bench pump, matching expectations for a force=2 Z over a 125 µL syringe.
- **Rule**: For plunger init (`Z`/`Y`), the completion signal is `?6 != "?"`. Plunger-position polling (`? == target`) is correct only for `move_to_steps()` where the target is asserted by the caller as something the plunger should reach; using it for init is unsafe when pre-init position already matches the target. (from ToDo#16)

### E8. Post-init plunger top speed defaults to V=4000 pps on firmware 8.33

- **Problem**: First plan for `claude_test/plunger_cycle.py` assumed full-stroke (12 000 half-steps) would take ~24 s, matching the manual's default init speed of 500 pps (lines 1626-1632). Settle-timeout budgets were sized accordingly.
- **Cause**: The manual's "500 pps" figure is the *init speed*, not the post-init move speed. After Z resets `v`/`V`/`c`/`S`/`L` to defaults, `V` (top speed) sits at 4000 pps. A subsequent `A<n>R` move uses V, not the init speed. The result: full stroke takes ~3.0 s, not ~24 s. Half stroke ~1.5 s.
- **Fix**: Bench-script `--settle-timeout-s` default lowered to 10 s for `move_to_steps()` (8× the observed move time, plenty of margin). HIL on 2026-05-18: 3.26 s for full stroke, 1.6 s for half stroke, 9/9 cycles verified. The settle-timeout for `initialize()` itself stays at 30 s because init does run at the slower 500 pps.
- **Rule**: When sizing move timeouts on this firmware, use V=4000 pps post-init, not the manual's init-speed figure. Init is the slow operation; subsequent moves are an order of magnitude faster unless `V` is explicitly lowered. (from ToDo#16)

---

## §99. Uncategorized

*(none yet)*

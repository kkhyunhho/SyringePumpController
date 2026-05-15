# LearnedPatterns.md

> Patterns extracted from [ToDo.md](ToDo.md) Completed items. Consult the relevant sections before drafting new ToDo entries. Append new patterns after each task completes.
>
> Last updated: 2026-05-15
> Total patterns: 4
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

---

## §3. Library Quirks

*(none yet — populate when a library surprise costs more than ten minutes)*

---

## §4. Workflow Lessons

### W1. Always run `diagnose()` before `initialize()` on a freshly plugged pump

- **Lesson**: `Z` (init) mechanically homes the plunger. If the serial link is mis-wired, the rotary address switch is set wrong, or the wrong ASCII variant is locked in, a blind `ZR` as the first command can slam the plunger into a hard stop or a closed valve. The diagnostic stage (echo `Q`, `?76`, `*`, `?6`, `?`) confirms communication, addressing, and power *without* moving anything.
- **Rule**: Always call `pump.diagnose()` first, inspect the `DiagnosticsReport`, and only call `pump.initialize()` after the report's `ok_to_initialize` is true. Never auto-init from `Pump.open()`. Document this order in every example and README snippet. (from DESIGN.md §7)

### W2. HIL tests are read-only — never move the plunger or valve from automation

- **Lesson**: Hardware-in-the-loop scripts that drive the real pump must restrict themselves to side-effect-free queries (firmware/build via `?76`, serial number if exposed, supply voltage `*`, status `Q`, valve `?6`, plunger position `?`). Motion testing is a separate, human-supervised activity on the bench. An automated script that moves a syringe risks damaging the plunger, the valve, or whatever fluid line is connected — and is the wrong abstraction for proving "the host can talk to the pump."
- **Rule**: Every HIL-tier test and `examples/hil_*.py` script proves identity, not motion. The `sy01b-diagnose` CLI must refuse to emit `R`/`Z`/`Y`/`W` by code, not by convention. (from DESIGN.md §10.1)

---

## §5. Environment Specifics

*(none yet — populate when a host- or OS-specific behavior bites)*

---

## §99. Uncategorized

*(none yet)*

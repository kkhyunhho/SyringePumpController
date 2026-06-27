# sy01b-client (ESP32-S3-BOX-3 firmware)

LVGL touch client that drives the SY-01B syringe pump over WiFi via
the [server/](../server/) FastAPI bridge.

**Phase C (this folder ships):** full motion UI. Boot → WiFi →
diagnose → user-driven Initialize → READY ⇄ BUSY. Valve tab, Move
tab (slider 0–125 µL + Aspirate / Dispense), Asp/Disp tab
(cycles + source/sink valve positions [Port n to Path m, the same
4-way set as the Valve tab] + per-cycle Target µL slider), Status
tab (2 s refresh). Error modals
(recoverable: Retry / Dismiss; fatal: Re-initialize) with
`requires_reinit` latch on `PlungerOverloadError` /
`InitFailedError`. Single-in-flight `pump_task` consumes a
4-deep command queue from the LVGL thread.

## Hardware

- **Board:** Espressif ESP32-S3-BOX-3 (320×240 LCD, capacitive touch,
  GT911 controller, 2 front buttons, octal PSRAM, 16 MB QIO flash).
- Drivers via the [`espressif/esp-box-3`](https://components.espressif.com/components/espressif/esp-box-3) BSP, pinned at
  **`^3.2`** in [main/idf_component.yml](main/idf_component.yml).
  The registry's `4.x` line never shipped publicly; staying on `^3.2`
  resolves to `3.2.0` and transitively pulls **LVGL 9.5.0**.
- JSON parsing via `espressif/cjson ^1.7` (also a managed component).
  ESP-IDF v6 dropped the built-in `json` component, so `cjson` is
  declared explicitly.

## Tooling

- **ESP-IDF v5.3+** — verified working on v6.0.1. `pump_client.c`
  consumes the registry `espressif/cjson` (not a built-in component
  on v6), and `ui.c` targets LVGL 9.x APIs (`lv_msgbox_create(parent)`
  + `lv_msgbox_add_*`, per-button click handlers, two-call spinner
  setup). Older ESP-IDF / LVGL combinations need a back-port.
- **clang-format** (LLVM 18, 80 col) — repo `.clang-format` at root.
- **cppcheck** ≥ 2.13.

## Build & flash

```bash
cd firmware

# One-time per shell:
. $HOME/esp/esp-idf/export.sh

idf.py set-target esp32s3
idf.py menuconfig
#   "Syringe Pump Client" submenu:
#     PUMP_SERVER_URL  = http://192.168.1.129:17046   (default — change if your PC IP / port differs)
#     PUMP_WIFI_SSID   = <your LAN SSID>
#     PUMP_WIFI_PASSWORD = <WPA2 PSK>
#
#   Component config → ESP System Settings → Main task stack size:
#     CONFIG_ESP_MAIN_TASK_STACK_SIZE = 12288
#     The 3584-byte default overflows during the first HTTP call
#     (esp_http_client + cJSON parsing on the main task). Bumping to
#     12 KiB clears it. Local-only — sdkconfig is gitignored.
idf.py build
idf.py -p /dev/ttyACM0 flash monitor   # adjust port to your dev kit
```

First build pulls managed components and writes `dependencies.lock`;
commit that file in a follow-up once the build is reproducible on the
bench machine.

### No `/dev/ttyACM0`? (containers without udev)

On a dev host with no udev (e.g. inside the container), plugging in or
resetting the BOX-3 enumerates its USB-Serial/JTAG interface in sysfs
but never creates the `/dev` node `idf.py flash` needs — flash then
fails with *"Could not open /dev/ttyACM0 … No such file or directory"*.
Run the helper to (re)create it from the kernel-assigned major:minor:

```bash
sudo firmware/acm_node.sh        # finds the Espressif (303a) ACM tty, mknod's /dev/ttyACMn
```

It is idempotent and reads the live USB topology each run, so it works
even after the port index changes between replugs. The node is **not**
persistent — re-run after every unplug/reset.

## Runtime behaviour

1. `app_main` initialises NVS, the BSP (I²C + display + buttons),
   the FSM, and creates the LVGL UI (all four tabs live).
2. WiFi STA connects using the credentials from menuconfig (or NVS
   override). State transitions: `BOOT → WIFI_CONNECTING →
   DIAGNOSING`.
3. The firmware issues a single `GET /v1/diagnose`. On success,
   state advances to `NEEDS_INIT` (amber banner) and the Status
   tab is seeded with the diagnose payload. **The Initialize step
   does not auto-run** — preserves the diagnose-before-init
   discipline (root DESIGN.md §12 Q4).
4. The operator taps the **right BSP button** (or, when wired,
   the on-screen Initialize control) to send `POST /v1/initialize`.
   On success, state advances to `READY` (green banner) and motion
   controls become enabled.
5. UI taps enqueue a `pump_cmd_t` onto a 4-deep FreeRTOS queue.
   The dedicated `pump_task` pops one command at a time, calls the
   matching `pump_client` HTTP wrapper synchronously, and posts
   results back to LVGL via `lv_async_call`. State cycles
   `READY ⇄ BUSY` per command.
6. A separate `status_task` polls `GET /v1/status` every 2 s; the
   Status tab and the cached valve-highlight / current-volume
   labels refresh automatically.
7. **BSP buttons:** left = jump to Status tab; right = Initialize
   (only active in `NEEDS_INIT`).
8. **Error modals.** Recoverable errors (HTTP 4xx, `ValveOverloadError`,
   `TransportTimeout`, etc.) show a *Retry / Dismiss* modal —
   `ValveOverloadError` is auto-retried once because the server
   re-homes on the next valve command (CLAUDE.md "Error model"
   code 10). Fatal errors (`PlungerOverloadError` code 9,
   `InitFailedError` code 1) latch `requires_reinit`, switch to
   the *Fatal* state (dark red banner), and the only modal button
   is *Re-initialize* → drops back to `NEEDS_INIT`.
9. On WiFi disconnect the banner turns red ("WiFi lost — reconnecting")
   and the status poll pauses; commands queued during the outage
   still execute when the link returns (one in flight at a time).
10. **Boot order matters.** If the PC server is *not* reachable when
    the firmware issues its boot-time `GET /v1/diagnose`, `app_main`
    parks the FSM in `ERROR_RECOVERABLE` and returns *without*
    spawning `status_task` — the device is then stuck until reboot.
    Workaround: start `bash server_run.sh` on the PC first, confirm
    `curl http://localhost:<port>/v1/health`, *then* power the
    ESP32. A boot-time-diagnose-retry path is tracked as a
    follow-up (ToDo §26 audit item).
11. **Auto-advance to READY.** Once an external client (curl,
    `/docs`, another LAN device) drives `POST /v1/initialize`, the
    next `GET /v1/status` poll sees `error_code == 0` + valve
    homed and `state_update_status` advances `NEEDS_INIT → READY`
    so the banner flips amber → green without requiring the
    on-screen / BSP Initialize path.

## Static analysis

```bash
# Format check (must be clean — SDLClaude §6).
clang-format --dry-run --Werror firmware/main/*.c firmware/main/*.h

# Style/warning check.
cppcheck --enable=warning,style --suppress=missingIncludeSystem \
         --suppress=unusedFunction firmware/main/
```

## Layout

```
firmware/
├── CMakeLists.txt
├── partitions.csv              # custom: nvs / phy_init / factory(3MB) / storage NVS
├── sdkconfig.defaults          # base
├── sdkconfig.defaults.esp32s3  # PSRAM oct 80M, flash QIO 80M 16MB
└── main/
    ├── CMakeLists.txt
    ├── Kconfig.projbuild       # menuconfig "Syringe Pump Client"
    ├── idf_component.yml       # esp-box-3 ^3.2 + cjson ^1.7
    ├── main.c                  # app_main + pump_task + status_task + BSP btns
    ├── wifi.{c,h}              # STA + auto-reconnect, posts FSM events
    ├── config_store.{c,h}      # NVS read of Kconfig-default overrides
    ├── pump_client.{c,h}       # /v1/* HTTP wrappers (Phase C: full motion)
    ├── pump_task.h             # pump_cmd_t + queue interface (UI ↔ pump_task)
    ├── state.{c,h}             # FSM + cached status snapshot + requires_reinit
    └── ui.{c,h}                # LVGL tabview, modals, motion-enabled gating
```

## Architecture link

This firmware is the cell-side half of the [3-tier architecture documented in the root README and DESIGN.md](../README.md#remote-http-bridge):

```
ESP32-S3 (LVGL UI) -- HTTP/JSON --> PC (FastAPI) -- pyserial --> SY-01B
192.168.1.x          0.0.0.0:17046                /dev/ttyUSB1
```

All driver-level safety patterns (diagnose-before-init, position
polling on `?`/`?6` instead of `Q.busy`) live in the Python driver
and are preserved end-to-end — the firmware only forwards user
intent over HTTP.

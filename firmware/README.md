# sy01b-client (ESP32-S3-BOX-3 firmware)

LVGL touch client that drives the SY-01B syringe pump over WiFi via
the [server/](../server/) FastAPI bridge.

**Phase B (this folder ships):** boot, WiFi STA, `/v1/diagnose` once at
startup, `/v1/status` every 2 s, status-only LCD dashboard. Motion
endpoints are intentionally not called yet.

**Phase C (planned, [ToDo §22](../ToDo.md)):** Valve / Move / Prime
tabs, error-recovery modals, full FSM transitions.

## Hardware

- **Board:** Espressif ESP32-S3-BOX-3 (320×240 LCD, capacitive touch,
  GT911 controller, 2 front buttons, octal PSRAM, 16 MB QIO flash).
- Drivers via the [`espressif/esp-box-3`](https://components.espressif.com/components/espressif/esp-box-3) BSP (`^4.0`) which
  pulls a compatible LVGL transitively.

## Tooling

- **ESP-IDF v5.3+** (latest stable recommended).
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
#     PUMP_SERVER_URL  = http://192.168.1.129:17046   (default — change if your PC IP differs)
#     PUMP_WIFI_SSID   = <your LAN SSID>
#     PUMP_WIFI_PASSWORD = <WPA2 PSK>
idf.py build
idf.py -p /dev/ttyACM0 flash monitor   # adjust port to your dev kit
```

First build pulls managed components and writes `dependencies.lock`;
commit that file in a follow-up once the build is reproducible on the
bench machine.

## Runtime behaviour (Phase B)

1. `app_main` initialises NVS, the BSP (I²C + display), and the FSM.
2. The UI is created with the four tabs — Valve / Move / Prime show
   "Phase C (not yet wired)" placeholders; Status is empty until
   diagnose finishes.
3. WiFi STA connects using the credentials from menuconfig (or NVS
   override). Initial state transitions: `BOOT → WIFI_CONNECTING →
   DIAGNOSING`.
4. The firmware issues a single `GET /v1/diagnose` to the configured
   server URL. On success, state advances to `NEEDS_INIT` and the
   Status tab is seeded with the diagnose payload.
5. A background `status_task` polls `GET /v1/status` every 2 s and
   refreshes the LVGL table on the main thread via `lv_async_call`.
6. On WiFi disconnect, the banner switches to red ("WiFi lost —
   reconnecting") and the status poll pauses until the link returns.

Phase B never calls `POST /v1/initialize` / `/valve` / `/aspirate`
/ `/dispense` / `/move_steps` / `/prime`. The Valve / Move / Prime
tabs are placeholders. Phase C closes [#12](https://github.com/coport-uni/SyringePumpController/issues/12) and opens the motion UI.

## Static analysis

```bash
# Format check (must be clean — CommonClaude §6).
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
    ├── idf_component.yml       # esp-box-3 ^4.0
    ├── main.c                  # app_main + status_task
    ├── wifi.{c,h}              # STA + auto-reconnect, posts FSM events
    ├── config_store.{c,h}      # NVS read of Kconfig-default overrides
    ├── pump_client.{c,h}       # diagnose + status only (Phase B)
    ├── state.{c,h}             # FSM, mutex-protected status snapshot
    └── ui.{c,h}                # 4-tab LVGL tabview (Status live only)
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

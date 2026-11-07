# CLAUDE.md

Guidance for Claude Code working in this repo.

**Keep this file current: whenever you change how a subsystem works, add a command, or move a
boundary, update the relevant section here in the same change.**

## What this is
ESP32-C3 SuperMini firmware that switches a Chevy Bolt's charger (relay on **GPIO 4**, signal-level
only) based on ComEd real-time supply prices, with SOC/charge power read from a BLE OBD dongle, a
plain-HTTP local web UI, Pushover notifications, and HTTPS OTA.

**Start with `PLAN.md`** — the full agreed design, owner decisions, reference-code pointers,
milestones and open items. Status: building all milestones on branch `mvp/m1-skeleton`.

## Stack
C++ / Arduino-ESP32 core 3.x / PlatformIO. NimBLE-Arduino, ArduinoJson 7, ESPAsyncWebServer,
LittleFS. Web UI is vanilla HTML/JS in `web/`, gzipped into firmware at build time.

## Commands
PlatformIO lives in the project venv (Python 3.14, Homebrew): `/opt/homebrew/bin/python3.14 -m venv .venv
&& .venv/bin/pip install platformio esptool`. Platform is pioarduino (Arduino core 3.3.x / IDF 5.5).
```
.venv/bin/pio run -e c3                 # build
.venv/bin/pio run -e c3 -t upload       # USB flash (/dev/cu.usbmodem1101)
.venv/bin/pio device monitor -b 115200  # serial
.venv/bin/pio test -e native            # host unit tests
```
Bench: GPIO 4 drives an LED standing in for the relay.

## Hard rules
- Pure logic (parsers, price math, decision engine, record encoding) must not include Arduino/ESP
  headers — it must compile and be tested in `env:native`. Time is passed in, never read inside.
- Never run two TLS sessions, or TLS + an active BLE GATT session, at the same time (RAM).
- OTA URLs must be `https://`; local UI is HTTP with no auth (owner decision).
- Secrets (Wi-Fi password, Pushover token/user) are write-only: never returned by the API or shown in the UI.
- Relay is active-high: GPIO 4 HIGH = charger ON (bench-verified).
- UI design source of truth: `prototypes/option-5-app.html` (app style).
- Unknown state (no clock / no prices) → relay = configured default (factory OFF).
- Notifications only on relay transitions; never repeat; "on but not charging" is page-only.
- Price logic must reproduce `comed-realtime-price-dashboard/charger/Vehicle.php` semantics (see
  PLAN.md §7.3), including UTF-8 state names — **except** entry-condition operators, which are
  honored literally (`>` strict, `>=` inclusive); the PHP's "everything inclusive" was a bug.
- Hotspot runs only when there are no Wi-Fi credentials or STA has been down for 5 min; never
  alongside a working STA connection.
- OTA default manifest: `https://github.com/sergesyrota/charger-cost-control/releases/latest/download/manifest.json`.

## Related repos (reference only; don't edit from here)
- `../comed-realtime-price-dashboard/price-worker/src/{comed,prices}.ts` — price feeds + math.
- `../comed-realtime-price-dashboard/charger/{Vehicle,Bolt}.php`, `cron.php` — decision logic.
- `../bolt-monitor/src/bolt_data.py`, `script.py` — BLE OBD protocol + PIDs.

# CLAUDE.md

Guidance for Claude Code working in this repo.

**Keep this file current: whenever you change how a subsystem works, add a command, or move a
boundary, update the relevant section here in the same change.**

## What this is
ESP32-C3 SuperMini firmware that switches a Chevy Bolt's charger (relay on **GPIO 4**, signal-level
only, **active-high**: HIGH = charger ON) based on ComEd real-time supply prices, with SOC/charge
power read from a BLE OBD dongle, a plain-HTTP local web UI, Pushover notifications, and HTTPS OTA.

**Start with `PLAN.md`** — the full agreed design, owner decisions, reference-code pointers,
milestones and open items. Status: architecture contracts + stub tree in place (builds); modules
are being implemented behind the headers.

## Stack
C++ / Arduino-ESP32 core 3.3.x (pioarduino, IDF 5.5) / PlatformIO. NimBLE-Arduino, ArduinoJson 7,
ESPAsyncWebServer, LittleFS (mount with label `"littlefs"`). Web UI is a single vanilla
HTML/JS file `web/index.html` (design: `prototypes/option-5-app.html`), gzipped into
`src/web/ui_gz.h` at build time (generated, gitignored).

## Layout & boundaries (details: `docs/MODULES.md`, HTTP contract: `docs/API.md`)
- `lib/core/src/` — **pure** library (`core`), built for both envs: `core_types.h`, `util/`
  (ChicagoTime), `price/`, `logic/`, `car/` (ELM/PIDs), `store/` (record + config codecs).
- `src/` — ESP glue: `net/` (net_lock, wifi_mgr, clock, https_client), `io/relay`,
  `price/price_fetcher`, `car/car_link`, `store/` (NVS config/state, event log, stats),
  `notify/pushover`, `ota/updater`, `web/` (server, api), `app/` (orchestrator; only place modules
  are wired), `main.cpp`.
- Blocking work (TLS, BLE, OTA, Wi-Fi changes) runs on the loop task via `app::`; web handlers only
  read thread-safe getters and queue actions through `app::`.
- `tools/version.py` (FW_VERSION from `git describe`), `tools/build_ui.py` — pre-build hooks.

## Commands
PlatformIO lives in the project venv: use `.venv/bin/pio` (never install it globally).
```
.venv/bin/pio run -e c3                 # build (also regenerates ui_gz.h, FW_VERSION)
.venv/bin/pio run -e c3 -t upload       # USB flash (/dev/cu.usbmodem1101)
.venv/bin/pio device monitor -b 115200  # serial
.venv/bin/pio test -e native            # host unit tests (lib/core only)
```

## Hard rules
- Pure logic (parsers, price math, decision engine, record encoding) lives in `lib/core` and must not
  include Arduino/ESP headers — it must compile and be tested in `env:native`. Time is passed in,
  never read inside.
- Never run two TLS sessions, or TLS + an active BLE GATT session, at the same time (RAM) — use
  `netlock::`.
- OTA URLs must be `https://`; local UI is HTTP with no auth (owner decision).
- Secrets (Wi-Fi password, Pushover token/user) are write-only: never returned by any API, rendered,
  or logged; APIs expose `<name>_set` booleans; empty/omitted on PUT keeps the current value.
- Unknown state (no clock / no prices) → relay = configured default (factory OFF).
- Notifications only on relay transitions; never repeat; "on but not charging" is page-only.
- Price logic must reproduce `comed-realtime-price-dashboard/charger/Vehicle.php` semantics (see
  PLAN.md §7.3), including UTF-8 state names — **except** entry-condition operators, which are
  honored literally (`>` strict, `>=` inclusive); the PHP's "everything inclusive" was a bug.
- Hotspot runs only when there are no Wi-Fi credentials or STA has been down for 5 min; never
  alongside a working STA connection.
- OTA default manifest: `https://github.com/sergesyrota/charger-cost-control/releases/latest/download/manifest.json`.
- Keep the image lean (libs alone ≈ 1.275 MB of the 1.6 MB slot): no std::regex/iostream, no new
  libraries without asking. Tests are sparse: only high-value pure-logic cases.

## Related repos (reference only; don't edit from here)
- `../comed-realtime-price-dashboard/price-worker/src/{comed,prices}.ts` — price feeds + math.
- `../comed-realtime-price-dashboard/charger/{Vehicle,Bolt}.php`, `cron.php` — decision logic.
- `../bolt-monitor/src/bolt_data.py`, `script.py` — BLE OBD protocol + PIDs.

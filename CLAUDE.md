# CLAUDE.md

Guidance for Claude Code working in this repo.

**Keep this file current: whenever you change how a subsystem works, add a command, or move a
boundary, update the relevant section here in the same change.**

## What this is
ESP32-C3 SuperMini firmware that switches a Chevy Bolt's charger (relay on **GPIO 4**, signal-level
only, **active-high**: HIGH = charger ON) based on ComEd real-time supply prices, with SOC/charge
power read from a BLE OBD dongle, a plain-HTTP local web UI, Pushover notifications, and HTTPS OTA.

**Start with `PLAN.md`** — the full agreed design, owner decisions, reference-code pointers,
milestones and open items. Status: all modules integrated and running on the board (hotspot,
relay default, scheduler). Not yet exercised on hardware: STA Wi-Fi/TLS/ComEd fetch, BLE dongle,
Pushover, OTA.

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
  read thread-safe getters and queue actions through `app::` (exception: `app::setOverride`
  evaluates synchronously — pure decide + GPIO + NVS — so the API reply shows the new relay state).
- `app.cpp` is a cooperative scheduler: wifi -> prices::tick (one fetch max) -> derive/decide on
  fetch, config change and every 60 s -> car poll cadence (post-ON probe, poll_on_s / poll_off_min)
  -> stats -> pushover -> queued jobs -> OTA health check. Task watchdog 60 s on the loop task; long
  waits in car_link / https_client call `wdtFeed()` (`src/app/wdt.h`); https caps connect, TLS
  handshake and header wait at 15 s each.
- BLE is initialised **only for the duration of a car poll** (`carlink::end()` after each poll):
  keeping NimBLE up leaves too little heap for TLS.
- Wi-Fi credentials live in NVS namespace `"wifi"` (wifi_mgr); config/logic/state in `"cc"`.
  Captive DNS is owned by wifi_mgr (web::loop does nothing).
- `include/nimble_trim.h` is force-included (`-include`) to compile out NimBLE peripheral/broadcaster
  roles and the security manager/bond store (via `MYNEWT_VAL_*`). PIO doesn't track it: after editing
  it run `.venv/bin/pio run -e c3 -t clean`.
- Size levers in `platformio.ini`: `-fno-exceptions` (never use try/catch), project board variant
  `variants/supermini` (no RGB_BUILTIN → no RMT), linker `--wrap`s in `src/size_wraps.c` (error names
  print as numeric codes).
- `tools/version.py` (FW_VERSION from `git describe`), `tools/build_ui.py` (light minify + gzip,
  UI ≈ 37 KB gz) — pre-build hooks. `tools/make_release.py` writes `dist/` (gitignored).

## Commands
PlatformIO lives in the project venv: use `.venv/bin/pio` (never install it globally).
```
.venv/bin/pio run -e c3                 # build (also regenerates ui_gz.h, FW_VERSION)
.venv/bin/pio run -e c3 -t upload       # USB flash (/dev/cu.usbmodem1101)
.venv/bin/pio device monitor -b 115200  # serial
.venv/bin/pio test -e native            # host unit tests (lib/core only)
.venv/bin/python tools/make_release.py  # dist/firmware-<ver>.bin + manifest.json for a GitHub release
```
Release (OTA): `git tag -a vX.Y.Z` on a clean tree → `pio run -e c3` → `make_release.py` → `git push origin
vX.Y.Z` → `gh release create vX.Y.Z dist/firmware-X.Y.Z.bin dist/manifest.json`. Must be a **full** release:
`releases/latest` (the device default) skips pre-releases. Test builds go on a `--prerelease` and are installed
by pasting that release's `manifest.json` URL into the UI. OTA + rollback were verified on the board
2026-11-07 (v0.1.0; deliberate-abort build rolled back by the bootloader).

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
- Language: everything the owner sees is **Russian** (UI, decision reasons, event log, Pushover,
  API error texts); serial-only debug output stays English. API keys/enums/JSON field names stay
  English — the UI maps them to Russian labels. Russian is 2 bytes/letter: size fixed buffers for it
  and pass any possibly-truncated text through `utf8FixTail()` (`lib/core/src/util/utf8.h`) so it
  never ends in half a character. Prices use `¢`, times are 24 h, weeks start on Monday.
- OTA default manifest: `https://github.com/sergesyrota/charger-cost-control/releases/latest/download/manifest.json`.
- Keep the image lean: `firmware.bin` is 1.64 MB = 86.1 % of the 1.81 MB app slot (partitions.csv,
  no coredump partition). Check the real .bin size, not PlatformIO's "Flash:" line. No
  std::regex/iostream, no new libraries without asking. Tests are sparse: only high-value
  pure-logic cases.

## Related repos (reference only; don't edit from here)
- `../comed-realtime-price-dashboard/price-worker/src/{comed,prices}.ts` — price feeds + math.
- `../comed-realtime-price-dashboard/charger/{Vehicle,Bolt}.php`, `cron.php` — decision logic.
- `../bolt-monitor/src/bolt_data.py`, `script.py` — BLE OBD protocol + PIDs.

# Modules, ownership and boundaries

The headers are the contract. Implement behind them; if a contract must change, change the
header *and* tell the integrator. Design reference: `PLAN.md`; HTTP contract: `docs/API.md`;
UI source of truth: `prototypes/option-5-app.html`.

## Ownership map

| Builder | Owns (may create/edit) |
|---|---|
| **price** | `lib/core/src/price/*`, `src/price/*`, `test/test_price/` |
| **logic** | `lib/core/src/logic/*`, `test/test_logic/` |
| **car** | `lib/core/src/car/*`, `src/car/*`, `test/test_car/` |
| **net** | `src/net/*`, `src/io/relay.*` |
| **store** | `src/store/*`, `lib/core/src/store/*` |
| **notify_ota** | `src/notify/*`, `src/ota/*`, `tools/make_release.py` |
| **web_api** | `src/web/server.*`, `src/web/api.*` |
| **ui** | `web/*`, `tools/build_ui.py` |
| **integrator** (later) | `src/app/*`, `src/main.cpp`, `lib/core/src/util/*`, `lib/core/src/core_types.*`, `platformio.ini`, `tools/version.py` |

Stub files named `*_stubs.cpp` may be split/renamed by their owner.

## Layout

```
lib/core/src/            PURE C++ (no Arduino/ESP headers; ArduinoJson allowed). Built for c3 + native.
  core_types.h           shared types: Config, Override, RuntimeState, PriceSnapshot, CarSnapshot, Decision, Mode…
  util/chicago_time.h    ChicagoTime (passed in, never read), chicagoOffset(), addDays()
  price/                 comed_parse (push parsers), price_math (currentSupply, lowestBy7am, derive)
  logic/                 logic_config (logic.json parse/validate/overlaps), decision_engine (decide)
  car/                   elm_parse, obd_pids, car_status
  store/                 records (binary codecs + JSON rows), config_codec (Config <-> API/NVS JSON)
src/                     ESP-only glue
  net/                   net_lock, wifi_mgr, clock (sysclock::), https_client (https::)
  io/relay               GPIO 4
  price/price_fetcher    prices:: fetch loop
  car/car_link           carlink:: NimBLE + ELM transport
  store/                 config_store (cfgstore::), state_store (statestore::), event_log (evlog::), stats::
  notify/pushover        pushover::
  ota/updater            ota::
  web/                   server (web::), api (api::), ui_gz.h (generated, gitignored)
  app/app                app:: orchestrator (the only place modules are wired together)
web/index.html           UI (single file, vanilla JS/CSS) -> tools/build_ui.py -> src/web/ui_gz.h
test/test_{price,logic,car}/  Unity tests (env:native), fixtures in test/fixtures/
```

## Rules

- **Pure core.** Nothing under `lib/core` includes Arduino/ESP/FreeRTOS headers or reads time.
  Use `NAN` for unknown floats (JSON `null`), `0` for unknown epochs.
- **Dependencies point inward.** `src/*` modules may use `lib/core` and `src/net/*` and
  `src/store/event_log.h`. They do **not** call each other otherwise — `app` wires them. Exceptions
  allowed by contract: `web/api` reads `wifi::`, `ota::progress()`, `stats::`, `evlog::` directly and
  mutates only through `app::`.
- **Threads.** Arduino loop task runs `app::loop()` and every blocking job (TLS, BLE, OTA, Wi-Fi
  changes). AsyncTCP task runs web handlers: never block, never TLS/BLE, use thread-safe getters.
- **Network lock** (`netlock::`): recursive. `https::` takes it per request; `carlink::poll` holds it
  for the whole GATT session; `ota::` holds it across check/install. Never two TLS sessions, never
  TLS + active GATT.
- **Secrets are write-only.** Wi-Fi password, Pushover token/user never leave the device: APIs expose
  only `*_set` booleans; empty/omitted on PUT = keep; `*_clear: true` clears. Never log them.
- **Lean.** Image budget is tight (libs alone ≈ 1.275 MB of a 1.6 MB slot): no std::regex, no
  iostream, no new libraries without asking. Prefer fixed buffers; avoid large stack objects in the
  AsyncTCP task (copy small structs, use `app::withPrices()` for the big snapshot).
- **Tests are sparse** (owner request): a handful of high-value pure-logic cases per folder. No tests
  for glue code.

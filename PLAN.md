# charger-cost-control — build plan (hand-off document)

A self-contained ESP32-C3 device that turns a Chevy Bolt's home charger on/off based on ComEd
real-time hourly pricing. It replaces three moving parts that exist today:

| Today (being retired) | Where it lives | Replaced by |
|---|---|---|
| Price fetch + "current price" / "lowest by 7am" math | `comed-realtime-price-dashboard/price-worker/` (Cloudflare Worker) | On-device **PriceEngine** |
| Charging state machine + Pushover | `comed-realtime-price-dashboard/charger/Vehicle.php`, `Bolt.php`, `cron.php` (PHP cron) | On-device **DecisionEngine** + **Notifier** |
| Bolt SOC/power via BLE OBD dongle, logic-config editor | `bolt-monitor/` (Python on a Raspberry Pi Zero) | On-device **CarLink** + web UI |
| Homebridge smart outlet | Homebridge | **Relay on GPIO 4** |

Everything in this document was agreed with the owner (Sergey) unless it is listed under
**§14 Open items**. If something here conflicts with the reference code, this document wins for
*behavior requirements*; the reference code wins for *protocol/format details* (feed formats, OBD
byte math) — verify against it.

---

## 1. Decisions (confirmed by owner)

1. **Board:** ESP32-C3 SuperMini — **verified 2026-11-07 via esptool**: ESP32-C3 (QFN32) rev v0.4,
   single core 160 MHz, Wi-Fi + BT 5 (LE), **embedded 4 MB flash (XMC)**, 40 MHz crystal, USB mode
   USB-Serial/JTAG (VID 0x303A / PID 0x1001), MAC `e8:3d:c1:9c:21:18`. Shows up on the owner's Mac
   as `/dev/cu.usbmodem1101`. ~320 KB usable RAM, no PSRAM.
2. **Language/stack:** C++ on the **Arduino-ESP32 core (3.x)**, built with **PlatformIO**.
   Libraries: NimBLE-Arduino (BLE), ArduinoJson 7, ESPAsyncWebServer + AsyncTCP (ESP32-C3
   compatible forks, e.g. `mathieucarbou/ESPAsyncWebServer`), LittleFS, `esp_https_ota` /
   `HTTPUpdate`, `esp_crt_bundle` for TLS roots. No MicroPython/Rust/ESPHome.
3. **Network:** joins a Wi-Fi network the owner doesn't administer but **trusts**.
   - Local web UI is **plain HTTP**, **no password / no auth**.
   - **OTA downloads must be HTTPS** with real certificate validation (CA bundle).
   - Outbound only otherwise: ComEd, NTP, Pushover, OTA host. **No periodic status pings/heartbeats**
     to any server.
4. **Relay:** on **GPIO 4**. It switches only the *signal* to the charger's logic board (enables /
   disables charging), not mains power — no current-rating concerns.
5. **Car data:** the ESP talks to the existing **BLE OBD-II dongle (Veepeak)** in the car directly
   (port of `bolt_data.py`). Used for SOC, confirming charging actually started, and showing live
   charge power.
6. **Pushover:** kept. Keys are entered in the web UI (stored on device), not compiled in.
7. **Notifications policy:**
   - Notify on relay **transitions** (ON / OFF), like today.
   - On an **ON** transition: wait **30 s**, probe the car, and include the result in the *same*
     message ("charging started 1.4 kW, 52%" / "car connected but not charging" / "car not
     detected").
   - **No separate "relay on but car not charging" alert**, and **never repeat** an alert. The car is
     often not in the garage when cheap prices arrive — that status is shown on the web page only.
8. **Time:** no RTC module, and **no time persistence** ("no need to track the time"). On boot the
   clock is unknown until NTP succeeds.
9. **Unknown state → default setting.** When nothing is known (boot before NTP/prices, prices
   stale, etc.) the relay takes the configured **default**, factory value **OFF**.
10. **Repo:** this one, `~/code/sergeysyrota/personal/charger-cost-control/`, separate from the
    price-dashboard repo.

---

## 2. Reference sources (read these; port, don't reinvent)

All paths are absolute under `/Users/sergey/code/sergeysyrota/personal/`.

### Pricing (port to C++)
- `comed-realtime-price-dashboard/price-worker/src/comed.ts` — feed URLs, 5-minute JSON parsing,
  current-hour average + `knownMinutes`, ServletFeed regex parser (`parseServletFeed`).
- `comed-realtime-price-dashboard/price-worker/src/prices.ts` — `currentSupply()` (30-min / 6-point
  rule) and `lowestBy7amSupply()` (window logic). **Port these exactly.**
- `comed-realtime-price-dashboard/price-worker/src/time.ts` — Chicago-time helpers (on the ESP use
  POSIX TZ instead, see §5).
- `comed-realtime-price-dashboard/price-worker/src/snapshot.ts` — which feeds are fetched together.
- `comed-realtime-price-dashboard/price-worker/test/prices.test.ts` — existing test cases; port them
  to the native test env.
- `comed-realtime-price-dashboard/forecast/results.md` — why "30 min / 6 points, fall back to
  day-ahead" is the chosen predictor.

### Charging decision logic (port to C++)
- `comed-realtime-price-dashboard/charger/Vehicle.php` — `controlCharging()` and
  `getApplicableLogic()`. This is the state machine. **Port semantics exactly** (§7.3 spells them out).
- `comed-realtime-price-dashboard/charger/Bolt.php` — Bolt specifics (full = SOC ≥ 100, SOC rounding,
  cached-data fallback).
- `comed-realtime-price-dashboard/charger/cron.php` — note `lowestBy7am = lowestBy7amSupply + 0.5`
  (a fixed 0.5 ¢ allowance; make it a config value, default 0.5).

### Car / OBD (port to C++)
- `bolt-monitor/src/bolt_data.py` — BLE UUIDs, ELM327 command/response handling, response parsing,
  every PID and its formula.
- `bolt-monitor/src/script.py` — polling flow, online / asleep / out-of-range classification,
  cell-voltage capture rules (10 % and 95 % windows, 24 h throttle, current-stability check).
- `bolt-monitor/experiments/notes.md` — dongle identity (Veepeak, MAC `8C:DE:52:DE:B4:A0`), GATT
  table, response-structure notes.
- `bolt-monitor/src/server.py` + `bolt-monitor/src/static/` — the current logic-config editor UI
  (save sorts `charging_logic` by `soc` descending). Reuse ideas for the new UI.

### Live config samples (copied into this repo)
- `docs/reference/bolt-monitor-logic.json` — **the live Bolt config today** (Cyrillic state names:
  Разряд / Заряд / Поездка — UTF-8 state names must work end-to-end, including JSON, NVS, UI and
  Pushover text).
- `docs/reference/charger-Bolt-config.json` — older single-state sample.

---

## 3. Hardware

| Item | Detail |
|---|---|
| MCU | ESP32-C3 SuperMini, 4 MB embedded flash (verified) |
| Relay | **GPIO 4**. Polarity unknown → config `relay_active_high` (default `true`); verify on bench. Drive the pin to the *default/OFF* level as the very first thing in `setup()` (before Wi-Fi/BLE). GPIO 4 is not a C3 strapping pin (those are 2, 8, 9), so boot glitches are not expected. |
| Status LED | On-board LED is GPIO 8 (active-low) on most SuperMinis — optional status blink (Wi-Fi/AP mode/relay). Note GPIO 8 is a strapping pin; only drive it after boot. |
| Button | BOOT = GPIO 9. Optional: long-press at runtime → force setup AP / factory reset Wi-Fi. |
| OBD dongle | Veepeak BLE, MAC `8C:DE:52:DE:B4:A0`, in the car. Must be within BLE range of the charger location (garage). |
| RTC | **None.** Clock is unknown until NTP. |

---

## 4. Firmware architecture

```
                ┌──────────────────────── ESP32-C3 ─────────────────────────┐
 ComEd HTTPS ──►│ PriceEngine ──► PriceSnapshot                             │
 NTP ──────────►│ Clock (valid flag)            │                           │
 Veepeak BLE ──►│ CarLink ──► CarSnapshot ──────┤                           │
                │                               ▼                           │
                │ ConfigStore (NVS) ──► DecisionEngine ──► Relay (GPIO 4)   │
                │                               │                           │
                │                     Notifier (Pushover HTTPS)             │
                │                     Stats + EventLog (LittleFS)           │
 Browser ◄─────►│ WebServer (HTTP :80): UI + JSON API                       │
 OTA host ─────►│ Updater (HTTPS manifest → image → sha256 → swap slot)     │
                └───────────────────────────────────────────────────────────┘
```

### Proposed source layout
```
charger-cost-control/
  platformio.ini            # envs: c3 (esp32-c3-devkitm-1 + USB CDC), native (host tests)
  partitions.csv            # §9.1
  src/main.cpp              # setup/loop, scheduler
  src/net/{wifi_mgr,clock,https_client}.{h,cpp}
  src/price/{comed_parse,price_engine}.{h,cpp}   # comed_parse = pure, testable
  src/car/{elm_parse,obd_pids,car_link}.{h,cpp}  # elm_parse + obd_pids = pure, testable
  src/logic/{decision_engine,logic_config}.{h,cpp}  # pure, testable
  src/io/relay.{h,cpp}
  src/notify/pushover.{h,cpp}
  src/store/{config_store,stats,event_log}.{h,cpp}
  src/web/{server,api}.{h,cpp}
  src/ota/updater.{h,cpp}
  web/                      # index.html, app.js, app.css → build step gzips into src/web/ui_gz.h
  test/                     # Unity tests for env:native
  test/fixtures/            # captured ComEd + ELM327 responses
  docs/reference/           # live config samples
```

**Rule:** anything that is pure logic (parsers, price math, decision engine, record encoding) must
not include Arduino/ESP headers so it compiles in `env:native`. Pass time in as an argument
(`epoch`, Chicago hour/minute), never call `time()` inside pure code.

### Concurrency / memory rules (C3 is tight)
- One cooperative scheduler in `loop()` (or a couple of FreeRTOS tasks with a single
  **"network-heavy" mutex**). **Never run a TLS session and an active BLE GATT session at the same
  time**; never run two TLS sessions at once. Each mbedTLS session is ~40–50 KB.
- NimBLE: central role only, 1 connection, reduce host stack/buffers via build flags.
- Stream-parse HTTP bodies (ArduinoJson filter for the 5-minute feed; streaming tokenizer for the
  ServletFeed) — don't buffer whole bodies when avoidable.
- Expose `ESP.getFreeHeap()` / `getMinFreeHeap()` / largest free block on the status page; it's the
  first thing to look at when something misbehaves.
- Enable the task watchdog (~30 s). Reset reason is logged at boot.

---

## 5. Time

- SNTP (`pool.ntp.org`, `time.google.com`) with POSIX TZ **`CST6CDT,M3.2.0,M11.1.0`** (America/Chicago).
- `Clock::valid()` is false from boot until the first successful sync (epoch > 2024-01-01). Nothing is
  persisted across reboots (owner decision).
- While time is invalid: no schedule mode, no "until" overrides, no price decisions (ComEd fetches
  require TLS, which needs a valid clock anyway) → relay = **default** (§7.1).

---

## 6. PriceEngine

### 6.1 Feeds (all supply-only ¢/kWh — no delivery fees; the existing thresholds are supply-only)
| Feed | URL | Format | Cadence |
|---|---|---|---|
| 5-minute | `https://hourlypricing.comed.com/api?type=5minutefeed` | JSON array, **newest first**, ~12.6 KB, items `{"millisUTC":"1794087900000","price":"16.7"}` (both strings) | every 5 min (aligned to :01, :06, … to catch fresh points) |
| Day-ahead (today) | `https://hourlypricing.comed.com/rrtp/ServletFeed?type=daynexttoday&date=YYYYMMDD` | JS text `[[Date.UTC(2026,10,7,0,0,0), 3.5], ...]` | hourly; cached per date |
| Day-ahead (tomorrow) | same, `date` = tomorrow | same | only at/after 17:00 Chicago; cache per date |
| Actuals (today) | `...ServletFeed?type=day&date=YYYYMMDD` | same | hourly; **display/stats only** |

`date` is the **Chicago** calendar date. In the ServletFeed, the 4th `Date.UTC` arg is the
**hour-ending** index (0..23); index 0 = the 11 pm→midnight slot. The month arg is 0-based (ignore
it; only the hour and price matter). Parse into `float[24]` with NaN for missing.

### 6.2 Derived values (port of `prices.ts`)
- `fiveMin.knownMinutes` = 5 × count of points whose Chicago `YYYYMMDDHH` equals the current one;
  `fiveMin.avg` = mean of those points.
- **currentSupply:**
  1. `knownMinutes ≥ 30` → current-hour average;
  2. else if any 5-min points → mean of the **6 most recent** points (by `millisUTC`);
  3. else → `dayAhead[(chicagoHour + 1) % 24]`;
  4. round to 0.1.
- **lowestBy7amSupply:**
  - hour < 17 → min of `todayDayAhead[0..7]`;
  - hour ≥ 17 → min of `todayDayAhead[(hour+1)%24 .. 23]` ∪ `tomorrowDayAhead[0..7]`;
  - ignore NaN; null if none; round to 0.1.
- **minPrice for logic** = `lowestBy7amSupply + config.min_offset` (default **0.5**, from `cron.php`).

### 6.3 Staleness
`PriceSnapshot` carries `fetched_at`. If the 5-minute feed hasn't been fetched successfully for
**> 20 min** (config) and there is no day-ahead value for the current hour, price is **unknown** →
decision falls to default (§7.1). If 5-min is stale but day-ahead exists, `currentSupply` uses
day-ahead (rule 3) and the UI shows "estimated (day-ahead)".

### 6.4 TLS
Use the Arduino-ESP32 built-in CA bundle (`esp_crt_bundle_attach` / `NetworkClientSecure::setCACertBundle`).
If ComEd changes CA, it's fixed via OTA.

---

## 7. DecisionEngine

### 7.1 Evaluation & precedence
Evaluate (a) right after every 5-minute price fetch, (b) immediately after any config/override
change, (c) on a CarLink SOC update that crosses a block/state boundary, and (d) once a minute for
time-based modes. This mirrors the old cron cadence and avoids flapping.

First applicable wins:
1. **Manual override** — `on` / `off`, optionally `until` an epoch (only offered when the clock is
   valid; reverts to `auto` when reached). Indefinite manual on/off works without a clock.
2. **Schedule** — per-weekday windows, e.g. Mon–Fri 01:00–05:00 ON. Requires valid clock. Outside
   all windows → fall through (configurable: "schedule only" mode makes outside-windows = OFF).
3. **Price logic** — §7.3. Requires a known price.
4. **Default** — `default_relay_state`, factory **OFF**. Used whenever the active mode can't decide
   (boot, no clock, no prices, etc.).

The UI mode selector is: `Auto (price)`, `Schedule`, `Schedule + price` (schedule windows force ON,
price logic otherwise), `Manual ON`, `Manual OFF`. Keep the precedence engine general so modes are
just config.

Every evaluation produces `{relay: bool, mode, reason: string}`; the reason is shown on the page,
written to the event log on transitions, and used in Pushover text.

### 7.2 Overlays (apply in auto/schedule modes, not in Manual ON)
- **SOC target** (optional, e.g. 80 %): if known SOC ≥ target → OFF.
- **Full**: SOC ≥ 100 (rounded, like `Bolt.php`) → OFF, notify once "OFF: Bolt is full". (The PHP
  had a "hold 1 h after full" intent but the check is `> 00`, i.e. immediate — keep immediate;
  optional `full_hold_minutes` config default 0.)

### 7.3 Price logic — exact port of `Vehicle::getApplicableLogic` + `controlCharging`
Config shape (unchanged, import `docs/reference/bolt-monitor-logic.json` as-is):
```json
{
  "state_select": "<optional state name to force>",
  "states": {
    "<name>": {
      "entry_conditions": [ {"type":"soc","operator":">=","value":80} ],
      "charging_logic": [ {"soc":70,"price_on":-1,"price_off":-0.5,"known_minutes":30}, ... ]
    }
  }
}
```
Semantics to preserve:
- **Current state is sticky** (persist `charging_state` in NVS; default `"default"`). This gives
  hysteresis: e.g. "Разряд" entered at SOC ≥ 80 and "Заряд" at SOC ≤ 20 — between them, stay put.
- If `state_select` names an existing state → use it, skip entry checks.
- Else iterate states **in config order**; for each state with `entry_conditions`, all conditions
  must hold (AND). **Operators are honored literally** — `>` is strict, `>=` inclusive, `<` strict,
  `<=` inclusive. ⚠️ This is a **deliberate fix** vs. the PHP, which treated `>` as `>=` and `<` as
  `<=`: with both ends inclusive, a pair like "A: soc ≥ 80" / "B: soc ≤ 80" both match at exactly 80
  and the state can flip back and forth. Exactly one side of a boundary must include equality.
  The config editor must **warn on overlapping entry ranges** between states (e.g. `>= 80` vs
  `<= 80`, or `> 50` vs `< 60`) when saving. First state whose conditions all hold becomes current. A state with an **empty** `entry_conditions` array is never auto-entered (only via
  `state_select`), e.g. "Поездка".
- Missing `charging_logic` in current state → use `states.default.charging_logic` if present.
- Block selection: walk `charging_logic` in order; pick the **first block with `soc_now >= block.soc`**;
  if none matches, the **last** block. (Editor keeps blocks sorted by `soc` descending.)
- `price_on` / `price_off` may be the string `"min"` → substitute minPrice (§6.2).
- `known_minutes` gate: `enough = !block.known_minutes || chicagoMinute >= block.known_minutes`
  (wall-clock minute of the hour, as in PHP).
- Transitions (relay = current physical/commanded state):
  - ON and `price > price_off` → **OFF** ("price is high")
  - ON and `!enough` → **OFF** ("not enough confidence yet")
  - OFF and `price <= price_on` and `enough` → **ON** ("price is low")
  - otherwise no change.
- **SOC input:** latest SOC from CarLink; if the car is currently unreachable use the **last known
  SOC** (kept in RAM and NVS — SOC is not time, so persisting it is fine); if never known, use
  **0** (matches the last/most-permissive block). Show "SOC (last seen …)" in the UI.

### 7.4 Tests (native)
Table-driven tests covering: state stickiness/hysteresis, `state_select`, empty entry conditions,
strict vs inclusive operators exactly at the boundary value, overlap detection,
block selection edges, `"min"` substitution with offset, known_minutes gate at minute boundaries,
each transition branch, full/SOC-target overlays, unknown price → default, UTF-8 state names.

---

## 8. CarLink (BLE OBD) — port of `bolt_data.py`

### 8.1 Transport
- Dongle MAC `8C:DE:52:DE:B4:A0` (config; add a "scan & pick" button in the UI later).
- **Notify** characteristic `0000fff1-0000-1000-8000-00805f9b34fb`;
  **Write** characteristic `0000fff2-0000-1000-8000-00805f9b34fb` (write without response).
- ELM327 text protocol: write `"<cmd>\r"`, accumulate notifications until a chunk contains `'>'`
  (prompt) or timeout (10 s).
- Header switching: send `AT SH <hdr>\r` only when it differs from the last header sent on this
  connection (reset on reconnect).
- The Python code does not send any init (`ATZ`, `ATE0`, …) and relies on **echo being on** — the
  parser locates the echoed command. Keep that behavior first; make the parser tolerant of echo-off
  too (it already handles lines starting with `7E8/7EC/7EF`).

### 8.2 Response parsing (port `obdCmd` exactly; unit-test with fixtures)
1. Strip spaces, turn `\r` into separators, drop `>`; `ERROR` → CommandError (car asleep);
   `NODATA` → NoDataError.
2. Positive response prefix = first byte of command + 0x40 (`22` → `62`).
3. Find the echoed command token; take the next token; if it starts with `7E`/`7B`/`7D`, drop 4
   chars (ECU id + length nibble); else if a token starts with `7EC`/`7EF`/`7E8`, drop 5 chars.
4. Require the positive prefix, drop it, split rest into byte pairs. Result bytes `[0],[1]` echo the
   PID; data starts at index 2 (= `A`).

### 8.3 PIDs
| Value | Header | Cmd | Formula (A = bytes[2], B = bytes[3], …) |
|---|---|---|---|
| SOC % | 7E4 | `228334` | A × 100 / 255 |
| Pack voltage V | 7E4 | `22432D` | (A·256 + B) × 0.52 |
| Pack current A | 7E7 | `2240D4` | (signed8(A)·256 + B) / 20 |
| Power W | — | — | V × I |
| Capacity Ah | 7E4 | `2241A3` | (A·256 + B) / 10 ; kWh = Ah × 0.32 |
| Charge count | 7E4 | `2243A5` | A·256 + B |
| Odometer mi | 7E0 | `2234B2` | (A<<24 \| B<<16 \| C<<8 \| D) × 0.009708816 |
| Cell n V (1–96) | 7E7 | `22{0x4181+n-1}` for n ≤ 31, `22{0x4200+n-32}` for n ≥ 32 | (A·256 + B) × 5 / 65536 |

### 8.4 Car status classification (from `connect()` / `script.py`)
- **not_detected** — BLE connect fails/times out (car not in garage / out of range).
- **asleep** — connected, but `228334` returns `ERROR`.
- **online** — `228334` returns data (or `NO DATA`, which the Python treats as online).
- **charging** — online and charging power ≥ **500 W** (config) in the charging direction.
  ⚠️ **Sign convention is unverified.** An idle snapshot showed −35 W. Add config
  `charging_power_sign` (`+1`/`-1`) and determine it on the first real charge; until then use
  `|power| ≥ threshold`.

### 8.5 Polling
- Relay ON: poll SOC + V + I every **60 s** (car is awake while charging). Disconnect between polls
  to free RAM for TLS (or keep connected if heap allows — measure).
- Relay OFF: poll every **60 min** (the Pi ran hourly) — querying keeps car modules awake and drains
  the 12 V battery. Make both intervals config.
- **Post-ON probe:** 30 s (config `on_probe_delay_s`, default 30) after an OFF→ON transition, run one
  poll and feed the result into the ON notification (§10).
- Cell voltages: capture all 96 when SOC is in 9–11 % or 94–99 %, at most once per 24 h per bucket,
  discard if pack current changed > 10 % between start and end (see `script.py`). Low priority —
  milestone 6.
- Odometer / capacity / charge count: once per day when online.

---

## 9. Storage

### 9.1 Partition table (4 MB) — `partitions.csv`
```
# Name,    Type, SubType,  Offset,   Size
nvs,       data, nvs,      0x9000,   0x5000
otadata,   data, ota,      0xE000,   0x2000
app0,      app,  ota_0,    0x10000,  0x190000
app1,      app,  ota_1,    0x1A0000, 0x190000
littlefs,  data, spiffs,   0x330000, 0xC0000
coredump,  data, coredump, 0x3F0000, 0x10000
```
App slots 1.6 MB each (expected image 1.1–1.3 MB with Wi-Fi + TLS + NimBLE + async web); ~768 KB
LittleFS for stats. Check image size in CI/build output; fail the build if > 90 % of a slot.
Changing this table later requires one USB flash.

### 9.2 NVS (config + small state)
`wifi` (managed by WiFi lib), `cfg` JSON blob (everything user-editable: mode, schedule, logic
config, min_offset, SOC target, default relay state, relay polarity, dongle MAC, poll intervals,
thresholds, pushover token/user/device/enabled, OTA URL), `state` blob (`charging_state`, last known
SOC, relay commanded state, manual override). Validate on load; fall back to factory defaults on
parse error and log it.

### 9.3 LittleFS (history) — binary fixed-size records, ring-buffered files
| File | Record | Size | Retention |
|---|---|---|---|
| `/hourly.bin` | hour epoch, price ×10 (int16), dayahead ×10, relay-on minutes, kWh into car ×100, SOC start/end, avg kW ×100, mode/flags | ~16 B | ~3 years (~420 KB) |
| `/sessions.bin` | start, end, SOC start/end, kWh ×100, cost (¢ ×10), avg price ×10, flags (car detected / charged) | ~32 B | ring of 2000 |
| `/detail.bin` | 5-min: epoch, price ×10, power W, SOC, relay, car status | ~12 B | 30 days (~100 KB) |
| `/events.log` | text line (time or uptime-if-no-clock, message) | ~80 B | last 500 (~40 KB) |
| `/cells.json` | last 10 % and 95 % cell-voltage snapshots | <2 KB | latest each |

- Current hour/day accumulate in RAM; **flush hourly** (and on OTA/reboot request) to limit flash
  wear. A crash loses ≤ 1 h of stats — acceptable.
- Records written only when the clock is valid (stats need timestamps); events before NTP use uptime.
- kWh into car = ∫ power dt from CarLink polls while relay ON; cost = kWh × supply price (supply
  only; optional fee adder config for display).

---

## 10. Notifier (Pushover)

- `POST https://api.pushover.net/1/messages.json` with `token`, `user`, `message`, optional
  `device`. Config from UI; "Send test" button. Disabled when token/user empty.
- Send **only** on relay transitions caused by the decision engine (and manual changes made in the UI,
  prefixed "Manual:"). Never send repeated/follow-up alerts; "relay on but not charging" is page-only.
- Message formats (keep close to `Vehicle.php`):
  - ON (sent after the 30 s probe):
    `ON: Price is low (1.2¢ ≤ 2¢) — Bolt/Заряд soc≥40. Charging started: 1.4 kW, 52%.`
    or `… Car connected, not charging (52%).` or `… Car not detected.`
  - OFF: `OFF: Price is high (3.4¢ > 3¢), Bolt/Заряд (52%, 5m ago).` /
    `OFF: Not enough confidence yet …` / `OFF: Bolt is full.` / `OFF: SOC target 80% reached.`
- Transport failure: log to event log, retry once after 60 s, then drop (no queue growth).
- TLS: runs under the network mutex (never concurrent with BLE/other TLS).

---

## 11. Web UI + API (HTTP :80, no auth)

UI is vanilla HTML/JS/CSS in `web/`, gzipped at build time into a C header and served from flash
(ships atomically with firmware via OTA; no CDN dependency). Small inline-SVG/canvas charts — no
Highcharts. Mobile-first.

### Pages / tabs
1. **Status** — relay state + mode + reason; current supply price (with "estimated" flag),
   lowestBy7am (+offset), price data age; car status (not detected / asleep / online / charging),
   SOC (+ last seen), live kW, session kWh/cost; clock valid?; Wi-Fi RSSI/IP; heap stats; firmware
   version; uptime/reset reason. Banner when relay ON but car not charging.
2. **Control** — Auto / Schedule / Schedule+price / Manual ON / Manual OFF; "for N hours" when clock
   valid; SOC target.
3. **Schedule** — weekday windows editor.
4. **Price logic** — form editor for states/entry conditions/blocks (sorted by soc desc on save, like
   `server.py`), `state_select` dropdown, raw JSON view with import/export (paste the existing
   `logic.json`), min_offset.
5. **Charts / Stats** — today's day-ahead vs actual vs 5-min price, relay-on shading, power/SOC
   overlay; daily/monthly kWh + cost from hourly records; session list.
6. **Settings** — Wi-Fi (rescan/change), Pushover (token, user, device, enable, test), dongle MAC,
   poll intervals, charging threshold/sign, relay polarity, default relay state, probe delay.
7. **Firmware** — current version; manifest URL (default prefilled, editable); "Check" shows
   available version; "Install" runs OTA with progress; reboot.
8. **Log** — event log.

### JSON API (UI is just a client of this)
```
GET  /api/status                 # everything on the Status page
GET  /api/prices                 # today/tomorrow day-ahead, actuals, recent 5-min
GET  /api/config     PUT /api/config        # whole cfg blob (validated)
GET  /api/logic      PUT /api/logic         # price-logic JSON only
POST /api/override   {mode, until?}
GET  /api/stats/hourly?from&to   GET /api/stats/sessions?limit   GET /api/stats/detail?from&to
GET  /api/log?limit
POST /api/pushover/test
POST /api/car/poll               # force a BLE poll now
POST /api/ota/check {url?}       POST /api/ota/install {url?}   GET /api/ota/progress
POST /api/reboot
```
Stats endpoints stream records in chunks (chunked response) — never load a whole file into RAM.

### Connectivity (owner decision — hotspot only when Wi-Fi isn't working)
- **No Wi-Fi credentials saved** → start the setup hotspot **`ChargerControl-XXXX`** immediately
  (captive portal; open AP is fine per "trusted"; optional AP password config). UI at
  `http://192.168.4.1`.
- **Credentials saved** → STA only, keep trying to connect for **5 minutes** (config
  `wifi_connect_timeout_s`, default 300). **No hotspot during this time.**
- Still not connected after 5 min → start the hotspot (AP+STA), and **keep retrying STA in the
  background** (e.g. every 60 s, without dropping the AP's channel). When STA connects: shut the
  hotspot down — but not while a client is associated to it (wait until it disconnects, max 10 min),
  so a user mid-setup isn't kicked off.
- Same rule after a later disconnect: if STA has been down for 5 continuous minutes, bring up the
  hotspot until STA is back.
- **Never run the hotspot while STA is connected** (apart from the client-still-attached grace above).
- Changing Wi-Fi credentials from the UI: save, try the new network for 5 min; on failure the
  hotspot comes up as above (old credentials are not kept — keep it simple).
- mDNS `charger.local` (best effort; guest networks often block it). Show the STA IP on the
  hotspot page, and log it to serial on connect.

---

## 12. OTA

- Default manifest URL compiled in, overridable in UI and persisted:
  **`https://github.com/sergesyrota/charger-cost-control/releases/latest/download/manifest.json`**
  (repo is public). Each GitHub Release attaches `manifest.json` + `firmware-<ver>.bin`; the
  manifest's `url` points at
  `https://github.com/sergesyrota/charger-cost-control/releases/download/v<ver>/firmware-<ver>.bin`.
  GitHub answers both with 302 redirects to `objects.githubusercontent.com` /
  `release-assets.githubusercontent.com` — the HTTPS client must follow redirects, and the CA bundle
  must cover those hosts (the default bundle does).
- Optional later: a GitHub Actions workflow that builds on tag push and publishes the release
  assets.
- Manifest (JSON): `{"version":"1.4.0","url":"https://…/firmware-1.4.0.bin","sha256":"…","size":1234567,"notes":"…"}`.
  The UI may also be given a direct `.bin` URL (then no sha256 check — warn in UI).
- **HTTPS only** — reject `http://` URLs. CA bundle validation; follow redirects (needed for GitHub
  release assets).
- Flow: take network mutex, stop BLE, (relay keeps its current state), stream to inactive slot via
  `esp_ota_*`, verify sha256 + size, set boot partition, flush stats, reboot.
- **Rollback:** enable `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` (Arduino: use a custom sdkconfig /
  `board_build.` flags, or call `esp_ota_mark_app_valid_cancel_rollback()` only after: Wi-Fi up +
  one successful ComEd fetch + web server serving, within 5 min of boot. If not reached → reboot →
  bootloader reverts). Show "running from rollback" in UI.
- Build pipeline: `pio run -e c3` → script emits `firmware-<ver>.bin`, `manifest.json` (sha256, size).
  Version from `git describe --tags`, compiled in.

---

## 13. Boot & failure behavior

1. `setup()` first line: relay pin → **default state** (factory OFF), honoring polarity.
2. Load config/state from NVS. Start Wi-Fi per §11 (hotspot only if no credentials or STA fails
   for 5 min), web server, BLE.
3. Until clock valid **and** prices known: relay = default; UI shows why.
4. Wi-Fi drops: keep last decision while prices are fresh (§6.3); once stale → default.
5. Watchdog reset / crash: boots back into step 1 (default), logs reset reason.
6. Manual override persists across reboot (it's in NVS) **except** timed (`until`) overrides, which
   are dropped on boot because there's no clock until NTP — revert to auto.

---

## 14. Open items (decide during build; don't block milestones 1–3)

Resolved: OTA default URL → GitHub Releases of the public repo (§12); flash = 4 MB (§1);
entry-condition operators → literal strict/inclusive (§7.3); hotspot policy (§11).

1. *(resolved)*
2. *(resolved)*
3. **Relay polarity** (`relay_active_high`) — verify on bench.
4. **Charging power sign** (§8.4) — verify on first real charge.
5. **BLE range** from the charger location to the car's OBD port — verify early (milestone 4 spike).
6. Whether to notify when the car arrives and starts charging *after* an ON transition already
   happened (currently: **no**, page only — owner said don't repeat alerting).

---

## 15. Milestones (each ends with something flashable and testable)

1. **Skeleton** — PlatformIO project, partitions, Wi-Fi (STA with 5-min fallback to hotspot, §11), NTP + TZ, HTTP
   server with a stub status page, relay toggle from UI on GPIO 4, event log, HTTPS OTA with
   manifest + sha256 + rollback. *After this, everything else ships over OTA.*
   Acceptance: OTA from a test manifest works; a deliberately broken build rolls back.
2. **PriceEngine** — parsers + math as pure code with native tests (fixtures captured with `curl`
   into `test/fixtures/`, plus ported `prices.test.ts` cases); on-device fetch loop; `/api/prices`;
   Status shows current/min price.
   Acceptance: values match `https://price-worker.serg-syrota.workers.dev/api/snapshot`
   (`currentSupply`, `lowestBy7amSupply`) at the same moment.
3. **DecisionEngine + Pushover** — full port (§7) with native tests; modes; overrides; config/logic
   editor UI; Pushover settings + test; default-state behavior.
   Acceptance: importing `docs/reference/bolt-monitor-logic.json` reproduces the PHP decisions for a
   table of (SOC, price, minute, prior state) cases.
4. **CarLink** — NimBLE central to the Veepeak, ELM parser (native tests with captured
   responses), SOC/V/I/power, status classification, polling cadence, 30 s post-ON probe feeding the
   ON notification, "on but not charging" banner. Check heap with BLE + TLS alternating.
5. **Stats** — LittleFS records, hourly flush, sessions, charts page, stats endpoints.
6. **Polish** — cell-voltage capture, odometer/capacity, dongle scan UI, mDNS, schedule editor UX.

### Cutover (after milestone 4 is stable for a few days)
- Remove the Bolt branch from `comed-realtime-price-dashboard/charger/cron.php` (keep Tesla) and
  update that repo's `CLAUDE.md`.
- Stop the bolt-monitor services on the Pi Zero (`bolt-monitor` systemd unit + cron).
- Unplug/retire the Homebridge outlet for the Bolt.
- `price-worker` stays (it still serves the dashboard and Tesla logic).

---

## 16. Dev workflow
```
pio run -e c3                    # build
pio run -e c3 -t upload          # USB flash (first time / partition changes); port /dev/cu.usbmodem1101
pio device monitor -b 115200     # serial log (USB CDC on boot enabled)
pio test -e native               # host unit tests (parsers, price math, decision engine)
```
Capture fixtures:
```
curl -s 'https://hourlypricing.comed.com/api?type=5minutefeed' > test/fixtures/5min.json
curl -s "https://hourlypricing.comed.com/rrtp/ServletFeed?type=daynexttoday&date=$(TZ=America/Chicago date +%Y%m%d)" > test/fixtures/daynexttoday.txt
```
For ELM327 fixtures, log raw notification bytes from the device (or from the Pi running
`bolt_data.py` with the `print(data)` in `notification_handler` uncommented) for each PID, in
echo-on form, including `ERROR` and `NO DATA` cases.

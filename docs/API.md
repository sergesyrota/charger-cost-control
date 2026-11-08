# HTTP JSON API (contract for `web/index.html` and `src/web/api.cpp`)

Ownership map and module rules: [`docs/MODULES.md`](MODULES.md). Owners: **web_api** implements,
**ui** consumes. Plain HTTP on :80, no auth (owner decision). The UI is served gzipped at `/`.

## Conventions

- JSON, UTF-8 (state names may be Cyrillic). `Content-Type: application/json` — **required** on every
  PUT/POST (else 415), and a browser `Origin` header must match `Host` (CSRF guard; no auth).
- Times are **UTC epoch seconds**; `null` when unknown (no clock). The UI renders Chicago time using
  `tz_offset_s` from `/api/status` (not the browser zone).
- Prices: supply-only **¢/kWh**, one decimal. Unknown numbers are `null` (C++ `NAN`).
- Human-readable texts (`error`, `errors`, `decision.reason`, `warnings`, job/car/price/OTA errors, log
  `msg`, `reset_reason`) are **Russian** UTF-8. Keys, enums and field names stay English (`manual_on`,
  `not_detected`, …); the UI maps enums to Russian labels itself.
- Errors: 4xx/5xx with `{"error": "human message"}`; validation errors may add `"errors": ["…"]`.
  400 bad input · 404 unknown route · 409 busy/conflict · 503 needs clock/not ready.
  Large GETs (`/api/prices`, `/api/stats/*`, `/api/log`) are limited to 2 in flight (and refused on low heap):
  beyond that they get 503 + `Retry-After: 1`. The UI queues them 2 at a time and retries on `Retry-After`.
- Async actions return **202** `{"queued": true, "seq": 7}`; progress/results appear in `GET /api/status`
  (`jobs`; a finished job whose `seq` ≥ the 202's `seq` is the result of that request) or the dedicated progress endpoint. UI polls `/api/status` every ~5 s (1 s while a job runs).
- **Secrets are write-only**: never in any response. Responses only carry `*_set` booleans. On PUT, an
  empty string / `null` / omitted secret keeps the stored value; `"<name>_clear": true` erases it.
- Large lists (stats, log) are sent with chunked transfer encoding; the body is still one JSON array.

---

## GET /api/status

Everything the Home screen, mode sheet, price sheet and Diagnostics need.

```json
{
  "now": 1794089700,               // epoch or null when clock not valid
  "clock_valid": true,
  "tz_offset_s": -21600,           // Chicago offset now (CST -21600 / CDT -18000)
  "uptime_s": 278000,
  "reset_reason": "Программный сброс",

  "relay": true,                   // commanded state (true = charger enabled)
  "relay_default": false,          // cfg.default_relay
  "mode": "auto",                  // auto | schedule | schedule_price | manual_on | manual_off
  "until": null,                   // epoch for timed manual modes
  "soc_target": {"enabled": false, "value": 80},

  "decision": {
    "src": "price",                // default | manual | overlay | schedule | price
    "reason": "Цена низкая (1.2¢ ≤ 2¢)",
    "state": "Заряд",              // price-logic state used ("" if not price)
    "how": "entered",              // "" | forced | entered | sticky
    "block": 2,                    // index into that state's charging_logic (-1 none)
    "fallback": false,             // blocks came from states.default
    "price": 1.2,
    "price_on": 2.0,               // resolved ("min" substituted)
    "price_off": 3.0,
    "known_minutes": 0,            // block gate, 0 = none
    "at": 1794089640
  },
  "sticky_state": "Заряд",
  "schedule_active": null,         // index of the active schedule window, or null

  "price": {
    "current": 1.2,                // null = unknown
    "estimated": false,            // true = from day-ahead (5-min stale/missing)
    "known": true,                 // current != null
    "age_s": 120,                  // since last successful 5-min fetch, null if never
    "known_minutes": 35,
    "hour_avg": 1.3,
    "lowest7": 0.4,                // lowestBy7amSupply
    "min_price": 0.9,              // lowest7 + min_offset ("min" in rules)
    "min_offset": 0.5,
    "stale_min": 20,
    "last_error": ""
  },

  "car": {
    "status": "charging",          // unknown | not_detected | asleep | online | charging
    "soc": 52,                     // last known (may be from memory), null if never
    "soc_live": true,              // from the latest poll
    "soc_at": 1794089650,          // epoch of SOC reading, null if unknown
    "last_seen": 1794089650,       // car last answered, null
    "last_poll_at": 1794089650,    // null
    "last_poll_up_s": 277950,      // uptime of last poll attempt (works without clock), null if never
    "kw": 6.8,                     // |watts|/1000 while online, null
    "volts": 356.2, "amps": 19.1, "watts": 6803,   // raw sign, null when unknown
    "threshold_w": 500,
    "polling": false,
    "error": ""
  },

  "session": {                     // current charging session (relay ON), else null
    "start": 1794085860, "kwh": 7.1, "cost_c": 9.0, "soc_start": 41, "avg_price": 1.3
  },
  "last_session": {                // most recent finished session, else null
    "start": 1794025000, "end": 1794040000, "kwh": 13.6, "cost_c": 27.0, "soc_start": 31, "soc_end": 52
  },

  "wifi": {
    "ssid": "Syrota-Home", "sta": true, "rssi": -61, "ip": "192.168.1.47",
    "ap": false, "ap_ssid": "ChargerControl-2118", "ap_ip": null, "ap_clients": 0,
    "hostname": "charger.local"
  },
  "heap": {"free": 148200, "min": 96400, "largest": 69620},
  "fs": {"total": 786432, "used": 120000},
  "fw": {"version": "1.3.2", "rollback": false, "pending_verify": false},

  "banners": {                     // UI shows a banner per true flag
    "relay_on_not_charging": false,// relay ON && car status != charging (page only, never notified)
    "no_clock": false,
    "price_stale": false,          // clock ok but price unknown -> default
    "ap_mode": false,
    "rollback": false
  },

  "jobs": {                        // state: idle | queued | running | ok | error
    "car_poll":  {"state": "idle", "error": "", "at": null, "seq": 0},
    "push_test": {"state": "ok",   "error": "", "at": 1794089600, "seq": 1},
    "ota":       {"state": "idle", "error": "", "at": null, "seq": 0},
    "wifi":      {"state": "idle", "error": "", "at": null, "seq": 0}   // ok once STA has an IP; error after ~45 s
  }
}
```

"Next charge" (Home tile) is computed in the UI from `/api/status` + `/api/prices` + `/api/config`
schedule + `/api/logic`, like the prototype's `nextCharge()`.

## GET /api/prices

```json
{
  "date": 20261107, "hour": 22, "minute": 35,      // Chicago now (null fields if no clock)
  "day_ahead_today":    [3.5, 3.1, …24 values, null for missing],   // index = hour-ending (ServletFeed)
  "day_ahead_tomorrow": [ …24 ] ,                   // null (whole field) until published (~17:00)
  "tomorrow_date": 20261108,
  "actual_today":       [ …24, null for future/missing ],
  "five_min": [[1794060000, 2.1], [1794060300, 2.4], …],   // today's Chicago points, OLDEST first
  "fetched": {"five_min": 1794089580, "day_ahead": 1794087000, "actual": 1794087000}  // null = never
}
```

Relay-on shading and kW/SOC overlay for the chart come from `GET /api/stats/detail?from=<midnight>`.

## GET /api/config · PUT /api/config

GET returns everything user-editable except price-logic (see `/api/logic`) and mode (see
`/api/override`). Secrets appear only as `*_set`.

```json
{
  "relay_active_high": true,
  "default_relay": false,
  "stale_min": 20,
  "full_hold_min": 0,
  "min_offset": 0.5,
  "soc_target_enabled": false,
  "soc_target": 80,
  "schedule": [ {"days": [1,2,3,4,5], "start": "01:00", "end": "05:00", "max_price": 3} ],  // days 0=Sun..6=Sat; start>end = overnight; max 8; max_price optional (¢, absent/null = no cap)
  "dongle_mac": "8C:DE:52:DE:B4:A0",
  "poll_on_s": 60,
  "poll_off_min": 60,
  "charge_threshold_w": 500,
  "power_sign": 0,                 // 0 = |W|, 1 or -1 = charging direction
  "probe_delay_s": 30,
  "pushover": {"enabled": true, "device": "", "token_set": true, "user_set": true},
  "wifi": {"ssid": "Syrota-Home", "password_set": true, "connect_timeout_s": 300},
  "ota_url": "https://github.com/sergesyrota/charger-cost-control/releases/latest/download/manifest.json"
}
```

**PUT** = partial merge: send only the fields a screen edits. Response = the full GET body.

- Secrets: `"pushover": {"token": "new…", "user": "new…"}` replaces; `""`/`null`/omitted keeps;
  `"token_clear": true` / `"user_clear": true` erases.
- `wifi.ssid` / password are **not** settable here (use `POST /api/wifi`); `wifi.connect_timeout_s` is.
- `schedule` replaces the whole list.
- Validation → 400 `{"error": "…"}`: ranges (soc 0–100, poll_on_s 10–3600, poll_off_min 5–1440,
  stale_min 5–120, probe 0–600, threshold 0–20000, min_offset −10–10, wifi.connect_timeout_s
  30–3600, schedule max_price −100–100), integer fields reject fractions, MAC `XX:XX:XX:XX:XX:XX`,
  `HH:MM`, days 0–6, `ota_url` must start with `https://`.

## GET /api/logic · PUT /api/logic

GET:
```json
{
  "logic": { "state_select": "Поездка", "states": { "Разряд": { "entry_conditions": [ … ], "charging_logic": [ … ] }, … } },
  "warnings": ["«Разряд» и «Заряд» оба подходят при заряде 80% — выигрывает первое по порядку, состояние может переключаться туда-обратно"],
  "sticky_state": "Заряд",
  "min_offset": 0.5,
  "min_price": 0.9
}
```
`logic` is exactly the bolt-monitor `logic.json` shape (import/export compatible). State order is
significant and preserved. `state_select` omitted/`""` = auto. `price_on`/`price_off` are numbers or
`"min"`. `known_minutes` 0–59, omitted when unused.

PUT body: the logic object itself (same shape as `logic`). Server validates, sorts each state's
blocks by `soc` descending, persists, re-evaluates. Response 200 = the GET body. Invalid → 400
`{"error": "ошибки в правилах", "errors": ["Заряд: диапазон 2: price_on должен быть числом или \"min\"", …]}`.
Overlaps are warnings only (still saved). `min_offset` is saved via `PUT /api/config`.

## POST /api/override

```json
{"mode": "manual_on", "until": 1794128400}     // until optional: epoch, manual modes only
```
Response 200 `{"mode": "manual_on", "until": 1794128400, "relay": true, "reason": "…"}`.
400 unknown mode / until in the past / until on a non-manual mode; 503 `until` without a valid clock.
`{"mode": "auto"}` clears any override. The UI offers Manual ON/OFF until the end of the current hour (+0),
the next hour (+1), three hours ahead (+3), until 07:00, or indefinitely; it computes those `until` epochs
from `now` + `tz_offset_s` (Chicago hour boundaries), the server just takes the epoch.

## Stats (chunked JSON arrays)

- `GET /api/stats/hourly?from=<epoch>&to=<epoch>` → rows
  `[hour_epoch, price, dayahead, relay_on_min, kwh, soc_start, soc_end, avg_kw, flags]`
  (unknowns `null`; flags bits 0–2 mode, 3 car seen, 4 charged).
- `GET /api/stats/detail?from=<epoch>&to=<epoch>` → rows `[epoch, price, watts, soc, relay(0|1), car_status]`
  (`car_status` numeric: 0 unknown, 1 not_detected, 2 asleep, 3 online, 4 charging).
- `GET /api/stats/sessions?limit=20` → newest first,
  `{"start", "end", "soc_start", "soc_end", "kwh", "cost_c", "avg_price", "max_kw", "car_detected", "charged", "manual"}`.
- `GET /api/stats/daily?days=14` → oldest first `[{"date": 20261107, "kwh": 13.6, "cost_c": 27.1, "relay_h": 5.3}]`.
- `GET /api/stats/monthly?months=6` → oldest first `[{"month": 202611, "kwh": 61.0, "cost_c": 108, "relay_h": 30.5}]`.

Defaults: `from` = now − 24 h, `to` = now, `limit` = 20, `days` = 14, `months` = 6. No clock → `[]`.

## GET /api/log?limit=100&kind=relay

Newest first: `[{"t": 1794089400, "up": 277800, "k": "relay", "msg": "ВКЛ: Цена низкая (1.2¢ ≤ 3¢)"}]`.
`t` null when logged before NTP (UI shows `+HH:MM:SS` uptime). `k`: sys | relay | state | push | car | price | err.
`kind` filter optional (`relay` also includes `state`, like the prototype filter). Default limit 100, max 500.

## Actions

| Request | Body | Response | Notes |
|---|---|---|---|
| `POST /api/pushover/test` | `{}` | 202 `{"queued":true}` | 400 if disabled / token / user not set. Result → `jobs.push_test`. |
| `POST /api/car/poll` | `{}` | 202 | Result → `jobs.car_poll` + `car`. |
| `POST /api/ota/check` | `{"url": "https://…"}` (optional; default = config `ota_url`) | 202 | 400 non-https. Poll `/api/ota/progress`. |
| `POST /api/ota/install` | `{"url": "…"}` (optional; default = last checked manifest) | 202 | 409 if no checked manifest and no url. Reboots itself when done. |
| `GET /api/ota/progress` | — | 200 below | |
| `POST /api/reboot` | `{}` | 202 `{"rebooting":true}` | Flushes stats/state first. |
| `GET /api/wifi/scan` | — | 202 `{"scanning":true}` while running, else 200 below | First call starts a scan; results kept ~60 s. `?refresh=1` forces a new scan. |
| `POST /api/wifi` | `{"ssid": "Net", "password": "…"}` | 202 `{"queued":true}` | Password write-only: omitted/`""` keeps current for the same SSID, means open network for a new SSID. Device reconnects; hotspot rules per PLAN §11. |
| `POST /api/wifi/forget` | `{}` | 202 `{"rebooting":true}` | 409 if no network saved. ~1.5 s later erases the saved SSID/password, flushes stats/state and reboots; with no credentials it comes up as the open hotspot (192.168.4.1). |

`GET /api/ota/progress`:
```json
{
  "phase": "downloading",          // idle | checking | available | up_to_date | downloading | verifying | done | error
  "job": "running",                // jobs.ota.state; while queued/running a non-active phase is stale
  "current": "1.3.2",
  "manifest": {"version": "1.4.0", "url": "https://…/firmware-1.4.0.bin", "size": 1214336,
               "sha256": "ab12…", "notes": "…", "direct_bin": false},   // null before a check
  "bytes": 524288, "total": 1214336, "pct": 43,
  "error": ""
}
```

`GET /api/wifi/scan` (done):
```json
{"networks": [{"ssid": "Syrota-Home", "rssi": -61, "secure": true, "ch": 6}]}
```

## Static

- `GET /` → UI (gzip). In hotspot mode, unknown hosts/paths redirect to `http://192.168.4.1/`
  (captive portal), except `/api/*`.

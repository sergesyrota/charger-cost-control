# UI prototype brief (shared by all 5 options)

Project: ESP32-C3 device that switches a Chevy Bolt's charger on/off from ComEd real-time prices.
Read `PLAN.md` §5–§13 (esp. §7 DecisionEngine, §8.4 car status, §10 notifications, §11 Web UI) and
`CLAUDE.md` "Hard rules" for exact behavior. The old logic editor lives in
`../bolt-monitor/src/static/{index.html,script.js,styles.css}` — that is the UI being improved
(it's a flat form of states → entry conditions → charging_logic blocks with raw numbers).
Live logic config: `docs/reference/bolt-monitor-logic.json` (Cyrillic state names Разряд / Заряд /
Поездка — UTF-8 must render).

## Goal
Same functionality, far more intuitive and easier to navigate. The owner should answer
"is it charging, why, and when will it next charge?" within 2 seconds of opening the page, and
editing rules should not require understanding the JSON shape.

## Hard constraints
- ONE self-contained .html file: inline CSS + JS, no external requests (no CDN, no web fonts, no
  images by URL). Inline SVG for icons/charts. It must work offline from `file://`.
  (Real firmware serves it gzipped from flash, so keep it lean — aim < 120 KB.)
- Mobile-first (works at 375px wide, no horizontal scroll), also good on desktop.
- Fully clickable prototype with realistic mock data held in a JS object (simulate `/api/status`,
  `/api/prices`, `/api/config`, `/api/logic`, `/api/log`, `/api/stats/*`). Interactions mutate
  the mock state (mode change updates the hero, saving rules shows a toast, etc.).
- Include a small floating "Demo scenario" switcher (unobtrusive, corner) to flip between:
  1. Charging (relay ON, price low 1.2¢, car charging 6.8 kW, SOC 52%)
  2. Waiting (relay OFF, price high 4.1¢ > off-threshold, car online asleep, SOC 64%)
  3. ON but car not charging (relay ON, car not detected → banner, page-only)
  4. Booting / unknown (no clock or prices stale → relay = default OFF, explain why)
  5. Manual override ON until 06:00
- Light + dark mode via prefers-color-scheme.
- Title tag: "Charger — Option N: <concept name>". A thin header link "← all options" to index.html.

## Functionality that MUST be present (all of PLAN §11)
1. Status: relay state + mode + human reason; current supply price (+ "estimated (day-ahead)"
   flag), lowest-by-7am (+ min_offset → minPrice used for "min"), price data age; car status
   (not detected / asleep / online / charging), SOC (+ "last seen"), live kW, session kWh/cost;
   clock valid?; Wi-Fi RSSI/IP; heap free/min/largest block; firmware version; uptime/reset reason;
   banner when relay ON but car not charging; "Poll car now" action.
2. Control: modes Auto (price) / Schedule / Schedule + price / Manual ON / Manual OFF; "for N
   hours / until" only when clock valid; SOC target (optional, e.g. 80%).
3. Schedule: per-weekday windows editor (e.g. Mon–Fri 01:00–05:00 ON); "schedule only" vs fall
   through to price outside windows.
4. Price logic: states, each with entry conditions (soc, operator > >= < <=, value — honored
   literally; empty conditions = never auto-entered, only via state_select) and charging blocks
   (soc, price_on, price_off — number or "min", optional known_minutes). Current sticky state shown.
   `state_select` (force a state / Auto). Blocks sorted by soc desc on save. Add/delete/rename
   states, conditions, blocks. Raw JSON view with import/export (paste logic.json). min_offset.
   Show which state + block is active right now.
5. Charts/Stats: today's day-ahead vs actual vs 5-min price, relay-on shading, power/SOC overlay;
   daily/monthly kWh + cost; session list.
6. Settings: Wi-Fi (rescan/change; hotspot only when no creds or STA down 5 min), Pushover (token,
   user, device, enabled, Send test), dongle MAC, poll intervals (ON 60 s / OFF 60 min), charging
   threshold (500 W) + sign, relay polarity, default relay state, ON-probe delay (30 s), staleness
   (20 min), full_hold_minutes.
7. Firmware/OTA: current version, manifest URL (default
   https://github.com/sergesyrota/charger-cost-control/releases/latest/download/manifest.json,
   https only — reject http with inline error), Check → shows available version + notes, Install
   with progress bar, Reboot, "running from rollback" indicator.
8. Event log (transitions, Pushover sends, errors; uptime timestamps before NTP).

## Quality bar
Distinct visual identity per option (not the same template recolored). Thoughtful information
hierarchy. Plain-language explanations of rules (e.g. "Below 40% battery: start when ≤ 2¢, stop
above 3¢"). Validate inputs inline. No lorem ipsum. Check your file in a browser-free way at
least by running `node -e` on the extracted script for syntax errors.

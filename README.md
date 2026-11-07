# charger-cost-control

Self-contained ESP32-C3 controller that charges a Chevy Bolt when ComEd real-time electricity
prices are low. Fetches prices directly from ComEd, reads SOC and charge power from the car's BLE
OBD-II dongle, switches the charger's enable signal via a relay, serves a local web UI for
configuration, sends Pushover notifications, and updates itself over HTTPS.

Status: design phase — see [PLAN.md](PLAN.md).

// Force-included into every source compiled by PlatformIO (see platformio.ini build_flags).
// The precompiled Arduino sdkconfig enables every NimBLE role + security manager; NimBLE-Arduino
// reads those from sdkconfig.h, so -D flags alone can't turn them off. sdkconfig.h is `#pragma once`:
// include it first, then override. We are a central/observer only, no pairing (Veepeak dongle).
#pragma once
#include "sdkconfig.h"
#undef CONFIG_BT_NIMBLE_ROLE_PERIPHERAL
#undef CONFIG_NIMBLE_ROLE_PERIPHERAL
#undef CONFIG_NIMBLE_ROLE_BROADCASTER
#undef CONFIG_NIMBLE_SM_LEGACY
#undef CONFIG_NIMBLE_SM_SC
#undef CONFIG_BT_NIMBLE_ROLE_BROADCASTER
#define CONFIG_BT_NIMBLE_ROLE_PERIPHERAL_DISABLED 1
#define CONFIG_BT_NIMBLE_ROLE_BROADCASTER_DISABLED 1
#undef CONFIG_BT_NIMBLE_SM_LEGACY
#undef CONFIG_BT_NIMBLE_SM_SC
#undef CONFIG_BT_NIMBLE_MAX_CONNECTIONS
#define CONFIG_BT_NIMBLE_MAX_CONNECTIONS 1
// nimconfig.h re-#defines CONFIG_BT_NIMBLE_SM_* / NVS_PERSIST to 1 after this file, so the #undefs
// above don't reach NimBLE. esp_nimble_cfg.h guards the MYNEWT_VAL_* with #ifndef: set them here.
// (No pairing / bonding: ~30 KB.) After editing this file: `pio run -e c3 -t clean`.
#define MYNEWT_VAL_BLE_SM_LEGACY (0)
#define MYNEWT_VAL_BLE_SM_SC (0)
#define MYNEWT_VAL_BLE_STORE_CONFIG_PERSIST (0)

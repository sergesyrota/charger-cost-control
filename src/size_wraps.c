// Linker --wrap replacements that drop big tables only used for log text (see platformio.ini).
// Owner: integrator. Error names become numeric codes ("mbedtls -0x2700", "ESP_ERR 0x101").
#include <stddef.h>
#include <stdio.h>

void __wrap_mbedtls_strerror(int code, char* buf, size_t len) {
  if (buf && len) snprintf(buf, len, "mbedtls -0x%04X", (unsigned)(code < 0 ? -code : code));
}

const char* __wrap_esp_err_to_name(int code) {
  static char b[20];
  snprintf(b, sizeof(b), "ESP_ERR 0x%x", (unsigned)code);
  return b;
}

// Only referenced by WiFiGeneric (WiFiProv cleanup). We never use WiFiProv.
void __wrap_network_prov_mgr_deinit(void) {}

// Arduino main.cpp's chip-debug report: never printed at CORE_DEBUG_LEVEL=1.
void __wrap__Z20printBeforeSetupInfov(void) {}
void __wrap__Z19printAfterSetupInfov(void) {}

#include "core_types.h"

#include <string.h>

static const char* const kModeNames[] = {"auto", "schedule", "schedule_price", "manual_on", "manual_off"};

const char* modeName(Mode m) { return kModeNames[(int)m]; }

bool modeFromName(const char* s, Mode& out) {
  if (!s) return false;
  for (int i = 0; i < 5; i++)
    if (strcmp(s, kModeNames[i]) == 0) { out = (Mode)i; return true; }
  return false;
}

const char* carStatusName(CarStatus s) {
  static const char* const n[] = {"unknown", "not_detected", "asleep", "online", "charging"};
  return n[(int)s];
}

const char* decisionSrcName(DecisionSrc s) {
  static const char* const n[] = {"default", "manual", "overlay", "schedule", "price"};
  return n[(int)s];
}

const char* stateHowName(StateHow h) {
  static const char* const n[] = {"", "forced", "entered", "sticky"};
  return n[(int)h];
}

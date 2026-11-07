// STUB — owner: logic. Always returns the default relay.
#include "logic/decision_engine.h"

#include <string.h>

namespace logic {

Decision decide(const DecisionInput& in) {
  Decision d;
  d.relay = in.cfg ? in.cfg->default_relay : false;
  d.src = DecisionSrc::Default;
  strncpy(d.reason, "Not implemented → relay held at default", sizeof(d.reason) - 1);
  strncpy(d.state, in.sticky_state ? in.sticky_state : "default", sizeof(d.state) - 1);
  d.at = in.clock_valid ? in.now.epoch : 0;
  return d;
}

StatePick pickState(const LogicConfig&, float, const char*) { return StatePick(); }
const ScheduleWindow* activeWindow(const Config&, const ChicagoTime&) { return nullptr; }
bool overrideExpired(const Override& o, bool clock_valid, int64_t now) {
  return clock_valid && o.until != 0 && now >= o.until;
}

}  // namespace logic

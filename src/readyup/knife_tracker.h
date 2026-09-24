#pragma once

#include <string>

namespace readyup {

// Log-derived alive/HP bookkeeping for the knife round (no engine events needed).
//
// While active, server log lines feed it:
//   "A<3><[U:1:x]><CT>" [..] killed "B<2><BOT><TERRORIST>" [..] with "knife"
//   "A<3><[U:1:x]><CT>" committed suicide with "world"
//   "A<3><..><CT>" [..] attacked "B<2><..><TERRORIST>" [..] with "knife" (damage "34") ... (health "66") ...
// (`attacked` lines need `mp_logdetail 3`, which the knife round sets.)
//
// Team membership comes from the log-derived human/bot tables (slot_registry):
// a member is alive unless a death line named them, and their HP is the last
// `(health "N")` seen for them (100 if never hit).

struct KnifeSideStats {
  int members = 0;
  int alive = 0;
  int hp = 0;  // sum over alive members
};

void KnifeTrackerReset(bool active);
bool KnifeTrackerActive();

// Cheap for unrelated lines. Call for every non-chat log line while active.
void KnifeTrackerObserveLine(const std::string& line);

// cs team: 2 = T, 3 = CT.
KnifeSideStats KnifeTrackerStats(int csTeam);

}  // namespace readyup

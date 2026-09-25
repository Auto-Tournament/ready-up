#pragma once

// Per-round damage bookkeeping behind the end-of-round damage report (damage_report.h). Pure
// logic, no engine calls (ctest `match_damage_votes`).
//
// Players are keyed by an int (the engine player slot). Damage is the health actually removed:
// player_hurt `dmg_health` capped at what the victim had left (an AWP body shot on a 30 hp
// player counts 30), so the numbers add up to 100 per kill like the scoreboard's.

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace readyup {

class DamageLedger {
 public:
  struct Pair {
    int dmg = 0;
    int hits = 0;
  };

  // Round start: everyone back to 100 hp, no damage.
  void Reset();
  // player_hurt. attacker < 0 (world, fall damage, the bomb) or == victim only moves the
  // victim's health. healthAfter: the event's `health`.
  void OnHurt(int attacker, int victim, int dmgHealth, int healthAfter);
  // player_death: the victim has 0 hp for the report.
  void OnDeath(int victim);

  // Health left this round (100 if never hurt).
  int Health(int key) const;
  // Damage `attacker` did to `victim` this round.
  Pair Dealt(int attacker, int victim) const;
  // Any damage between different players this round (the caller decides what "enemy" means).
  bool AnyPlayerDamage() const { return !dealt_.empty(); }

 private:
  std::map<int, int> health_;
  std::map<std::pair<int, int>, Pair> dealt_;  // (attacker, victim)
};

struct DamageParticipant {
  int key = -1;
  std::string name;
  int team = 0;  // 2 T / 3 CT
};

// The report lines for `self`: one per opponent (the other CS team among `players`), in the
// order given:  "To: 54 in 2 | From: 0 in 0 — Name (46 hp)".
// Empty when `self` is not on T/CT or there are no opponents.
std::vector<std::string> FormatDamageReport(const DamageParticipant& self, const std::vector<DamageParticipant>& players,
                                            const DamageLedger& ledger);

}  // namespace readyup

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
  // burn: a fire tick (weapon "inferno": molotov / incendiary). A fire counts as one hit per
  // attacker and victim, like CS2's own damage report, not one per tick (2026-11-24: "in 6"
  // where CS2 said 3 for a grenade and a molotov).
  void OnHurt(int attacker, int victim, int dmgHealth, int healthAfter, bool burn = false);
  // player_death: the victim has 0 hp for the report.
  void OnDeath(int victim);

  // Health left this round (100 if never hurt).
  int Health(int key) const;
  // Damage `attacker` did to `victim` this round.
  Pair Dealt(int attacker, int victim) const;
  // Damage `key` did to themselves this round: their own grenades and fire, falling (world).
  Pair Self(int key) const;
  // All the health `key` lost this round, from anyone.
  int Taken(int key) const;
  // Any damage between different players this round (the caller decides what "enemy" means).
  bool AnyPlayerDamage() const { return !dealt_.empty(); }

 private:
  std::map<int, int> health_;
  std::map<std::pair<int, int>, Pair> dealt_;  // (attacker, victim)
  std::map<int, Pair> self_;   // victim -> own / world damage
  std::map<int, int> taken_;   // victim -> all health lost
  std::map<std::pair<int, int>, bool> burning_;  // (attacker, victim) already hit by fire this round
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
                                            const DamageLedger& ledger, const std::string& lang);

}  // namespace readyup

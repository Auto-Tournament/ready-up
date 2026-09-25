#pragma once

// Practice feedback lines (ctest `practice_rules`): in practice mode everyone sees in chat how
// long a flash blinded someone and how much each hit did (wallbangs, nade lineups). Grenade
// damage (HE, molotov / incendiary fire) is summed per throw instead of one line per tick of fire.
// Pure formatting and aggregation; the events are in practice_plugin.cpp.

#include <map>
#include <string>
#include <vector>

namespace practice {

// player_hurt hitgroup -> "head", "chest", ... ("" for generic / unknown).
const char* HitgroupName(int hitgroup);

// "Simpert hit Bot Adam: -27 hp, -8 armor (head, ak47), 73 hp left"
std::string FormatHit(const std::string& attacker, const std::string& victim, int dmgHealth, int dmgArmor,
                      int hitgroup, const std::string& weapon, int healthLeft);
// "Bot Adam flashed 2.4 s by Simpert" (self: "... by themselves")
std::string FormatBlind(const std::string& victim, const std::string& attacker, double seconds);

// Grenade weapons whose damage is summed per throw: hegrenade, inferno (molotov / incendiary fire).
bool IsSummedWeapon(const std::string& weapon);

// Sums grenade damage per (attacker, weapon); a group is reported once no new damage came in
// for `quietSeconds` (fire hurts every few ticks while it burns).
class GrenadeDamage {
 public:
  void Add(double now, const std::string& attacker, const std::string& weapon, const std::string& victim, int dmg);
  // Lines for the groups that went quiet: "HE by Simpert: 98 total (Bot Adam -57, Bot Ben -41)".
  std::vector<std::string> Flush(double now, double quietSeconds);
  void Clear() { groups_.clear(); }

 private:
  struct Group {
    double last = 0;
    std::map<std::string, int> byVictim;
  };
  std::map<std::pair<std::string, std::string>, Group> groups_;
};

}  // namespace practice

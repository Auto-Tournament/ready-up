#include "practice_feedback.h"

#include <cstdio>

namespace practice {

const char* HitgroupName(int hitgroup) {
  switch (hitgroup) {
    case 1: return "head";
    case 2: return "chest";
    case 3: return "stomach";
    case 4: return "left arm";
    case 5: return "right arm";
    case 6: return "left leg";
    case 7: return "right leg";
    case 8: return "neck";
    default: return "";
  }
}

std::string FormatHit(const std::string& attacker, const std::string& victim, int dmgHealth, int dmgArmor,
                      int hitgroup, const std::string& weapon, int healthLeft) {
  std::string s = (attacker.empty() || attacker == victim ? victim + " took" : attacker + " hit " + victim + ":") + " -" +
                  std::to_string(dmgHealth) + " hp";
  if (dmgArmor > 0) s += ", -" + std::to_string(dmgArmor) + " armor";
  std::string what = HitgroupName(hitgroup);
  if (!weapon.empty()) what += (what.empty() ? "" : ", ") + weapon;
  if (!what.empty()) s += " (" + what + ")";
  s += ", " + std::to_string(healthLeft < 0 ? 0 : healthLeft) + " hp left";
  return s;
}

std::string FormatBlind(const std::string& victim, const std::string& attacker, double seconds) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.1f", seconds);
  return victim + " flashed " + buf + " s by " + (attacker.empty() || attacker == victim ? "themselves" : attacker);
}

bool IsSummedWeapon(const std::string& weapon) { return weapon == "hegrenade" || weapon == "inferno"; }

void GrenadeDamage::Add(double now, const std::string& attacker, const std::string& weapon, const std::string& victim,
                        int dmg) {
  Group& g = groups_[{attacker, weapon}];
  g.last = now;
  g.byVictim[victim] += dmg;
}

std::vector<std::string> GrenadeDamage::Flush(double now, double quietSeconds) {
  std::vector<std::string> out;
  for (auto it = groups_.begin(); it != groups_.end();) {
    if (now - it->second.last < quietSeconds) {
      ++it;
      continue;
    }
    int total = 0;
    std::string parts;
    for (const auto& kv : it->second.byVictim) {
      total += kv.second;
      parts += (parts.empty() ? "" : ", ") + kv.first + " -" + std::to_string(kv.second);
    }
    const std::string what = it->first.second == "hegrenade" ? "HE" : "Fire";
    out.push_back(what + " by " + (it->first.first.empty() ? std::string("?") : it->first.first) + ": " +
                  std::to_string(total) + " total (" + parts + ")");
    it = groups_.erase(it);
  }
  return out;
}

}  // namespace practice

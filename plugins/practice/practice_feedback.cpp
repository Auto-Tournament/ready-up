#include "practice_feedback.h"

#include <cstdio>

namespace practice {

const char* HitgroupKey(int hitgroup) {
  switch (hitgroup) {
    case 1: return "hitgroup.head";
    case 2: return "hitgroup.chest";
    case 3: return "hitgroup.stomach";
    case 4: return "hitgroup.left_arm";
    case 5: return "hitgroup.right_arm";
    case 6: return "hitgroup.left_leg";
    case 7: return "hitgroup.right_leg";
    case 8: return "hitgroup.neck";
    default: return "";
  }
}

std::string FormatHit(const Tr& tr, const std::string& attacker, const std::string& victim, int dmgHealth,
                      int dmgArmor, int hitgroup, const std::string& weapon, int healthLeft) {
  const std::string hp = std::to_string(dmgHealth);
  std::string s = attacker.empty() || attacker == victim ? tr("hit.took", {{"victim", victim}, {"hp", hp}})
                                                         : tr("hit.hit", {{"attacker", attacker}, {"victim", victim}, {"hp", hp}});
  if (dmgArmor > 0) s += tr("hit.armor", {{"armor", std::to_string(dmgArmor)}});
  const char* group = HitgroupKey(hitgroup);
  std::string what = *group ? tr(group, {}) : "";
  if (!weapon.empty()) what += (what.empty() ? "" : ", ") + weapon;
  if (!what.empty()) s += " (" + what + ")";
  return s + tr("hit.left", {{"left", std::to_string(healthLeft < 0 ? 0 : healthLeft)}});
}

std::string FormatBlind(const Tr& tr, const std::string& victim, const std::string& attacker, double seconds) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.1f", seconds);
  if (attacker.empty() || attacker == victim) return tr("blind.self", {{"victim", victim}, {"seconds", buf}});
  return tr("blind.by", {{"victim", victim}, {"seconds", buf}, {"attacker", attacker}});
}

std::string FormatGrenadeSum(const Tr& tr, const GrenadeSum& s) {
  int total = 0;
  std::string parts;
  for (const auto& kv : s.byVictim) {
    total += kv.second;
    parts += (parts.empty() ? "" : ", ") + kv.first + " -" + std::to_string(kv.second);
  }
  return tr(s.weapon == "hegrenade" ? "dmg.he" : "dmg.fire",
            {{"attacker", s.attacker.empty() ? std::string("?") : s.attacker}, {"total", std::to_string(total)}, {"parts", parts}});
}

bool IsSummedWeapon(const std::string& weapon) { return weapon == "hegrenade" || weapon == "inferno"; }

void GrenadeDamage::Add(double now, const std::string& attacker, const std::string& weapon, const std::string& victim,
                        int dmg) {
  Group& g = groups_[{attacker, weapon}];
  g.last = now;
  g.byVictim[victim] += dmg;
}

std::vector<GrenadeSum> GrenadeDamage::Flush(double now, double quietSeconds) {
  std::vector<GrenadeSum> out;
  for (auto it = groups_.begin(); it != groups_.end();) {
    if (now - it->second.last < quietSeconds) {
      ++it;
      continue;
    }
    out.push_back({it->first.first, it->first.second, it->second.byVictim});
    it = groups_.erase(it);
  }
  return out;
}

}  // namespace practice

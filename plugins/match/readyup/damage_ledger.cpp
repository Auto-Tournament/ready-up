#include "readyup/damage_ledger.h"

#include <algorithm>

namespace readyup {

void DamageLedger::Reset() {
  health_.clear();
  dealt_.clear();
}

int DamageLedger::Health(int key) const {
  auto it = health_.find(key);
  return it == health_.end() ? 100 : it->second;
}

void DamageLedger::OnHurt(int attacker, int victim, int dmgHealth, int healthAfter) {
  if (victim < 0 || dmgHealth <= 0) return;
  const int before = Health(victim);
  const int actual = std::min(dmgHealth, std::max(0, before));
  health_[victim] = std::clamp(healthAfter, 0, before);
  if (attacker < 0 || attacker == victim || actual <= 0) return;
  Pair& p = dealt_[{attacker, victim}];
  p.dmg += actual;
  p.hits += 1;
}

void DamageLedger::OnDeath(int victim) {
  if (victim >= 0) health_[victim] = 0;
}

DamageLedger::Pair DamageLedger::Dealt(int attacker, int victim) const {
  auto it = dealt_.find({attacker, victim});
  return it == dealt_.end() ? Pair{} : it->second;
}

std::vector<std::string> FormatDamageReport(const DamageParticipant& self, const std::vector<DamageParticipant>& players,
                                            const DamageLedger& ledger) {
  std::vector<std::string> out;
  if (self.team != 2 && self.team != 3) return out;
  for (const auto& o : players) {
    if (o.key == self.key || (o.team != 2 && o.team != 3) || o.team == self.team) continue;
    const auto to = ledger.Dealt(self.key, o.key);
    const auto from = ledger.Dealt(o.key, self.key);
    out.push_back("To: " + std::to_string(to.dmg) + " in " + std::to_string(to.hits) + " | From: " +
                  std::to_string(from.dmg) + " in " + std::to_string(from.hits) + " \xE2\x80\x94 " + o.name + " (" +
                  std::to_string(ledger.Health(o.key)) + " hp)");
  }
  return out;
}

}  // namespace readyup

#include "readyup/center_panel_owner.h"

namespace readyup {

bool CenterPanelOwners::Claim(int slot, uint64_t plugin, int priority, double seconds, double now) {
  if (slot < 0 || slot >= kSlots) return false;
  Entry& e = slots_[slot];
  const bool held = e.plugin != 0 && now < e.until;
  if (held && e.plugin != plugin && priority < e.priority) return false;
  e.plugin = plugin;
  e.priority = priority;
  e.until = now + (seconds > 0 ? seconds : 1);
  return true;
}

void CenterPanelOwners::Release(int slot, uint64_t plugin) {
  for (int i = 0; i < kSlots; ++i) {
    if ((slot < 0 || i == slot) && slots_[i].plugin == plugin) slots_[i] = Entry{};
  }
}

void CenterPanelOwners::Reset(int slot) {
  for (int i = 0; i < kSlots; ++i) {
    if (slot < 0 || i == slot) slots_[i] = Entry{};
  }
}

uint64_t CenterPanelOwners::Owner(int slot, double now) const {
  if (slot < 0 || slot >= kSlots) return 0;
  const Entry& e = slots_[slot];
  return e.plugin != 0 && now < e.until ? e.plugin : 0;
}

}  // namespace readyup

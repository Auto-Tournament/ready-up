#pragma once

// One center panel per player: the game has a single center-HTML panel per client, so two plugins
// writing to it (the ready HUD and the workshop download bar) flicker between each other. The
// core keeps who owns each player's panel, at what priority, until when. A send from another
// plugin at a lower priority is refused while the owner's panel is still up; it goes through
// again once that panel has expired. Pure bookkeeping (ctest `center_panel_owner`).

#include <cstdint>

namespace readyup {

class CenterPanelOwners {
 public:
  static constexpr int kSlots = 64;

  // May `plugin` show a panel on `slot` at `priority` for `seconds` at time `now`? On yes, it
  // becomes the owner until now + seconds. The owner itself, an equal or higher priority, or an
  // expired panel always pass.
  bool Claim(int slot, uint64_t plugin, int priority, double seconds, double now);
  // `plugin` gives `slot` up (-1: every slot it owns): others may draw at once.
  void Release(int slot, uint64_t plugin);
  // The player left or the map changed: nobody owns the slot (-1: every slot).
  void Reset(int slot);
  // Who owns `slot` right now (0 = nobody).
  uint64_t Owner(int slot, double now) const;

 private:
  struct Entry {
    uint64_t plugin = 0;
    int priority = 0;
    double until = 0;
  };
  Entry slots_[kSlots];
};

}  // namespace readyup

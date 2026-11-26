// The in-game inventory (`.skins`): the platform's items (skins.inventory) as HUD screens.
// Pure: no engine, no ru_api; tested in tests/inventory_view_test.cpp. The game side (open,
// clicks, the fleet messages) is inventory.cpp.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace skins {
namespace inv {

struct Item {
  int64_t id = 0;
  std::string weapon, weaponName, name, rarity, slot, image, source;
  double wear = -1;      // float, -1 = unknown
  int64_t pattern = -1;  // -1 = unknown
  int64_t stattrak = -1; // -1 = not StatTrak
  int side = 0;          // the one side that buys it (2 T, 3 CT), 0 = both
  bool on[4] = {false, false, false, false};  // equipped for team 0 (both), 2 (T), 3 (CT)
};

struct Inventory {
  bool enabled = false;
  std::string error;
  std::vector<Item> items;
};

// skins.inventory's payload (JSON text). False on bad JSON.
bool ParseInventory(const std::string& json, Inventory* out);

// The HUD class of a game picture name (at_skins.css, hud/tools/gen_skins.py): "sk-" + FNV-1a 32.
std::string PictureClass(const std::string& image);

// Tabs, in order. TabOf: the tab an item belongs to ("all" is every item).
const std::vector<std::string>& Tabs();
std::string TabOf(const Item& it);

// The wear name of a float ("Field-Tested"), short form ("FT").
std::string WearName(double f);
std::string WearShort(double f);

// Text by key in the player's language (the plugin's catalog), with {placeholders}.
using Text = std::function<std::string(const std::string& key, const std::vector<std::pair<std::string, std::string>>& args)>;

// One scrolling grid a tab, up to this many tiles (the HUD has 120: hud_screen.h kMaxTiles).
constexpr int kMaxShown = 120;
constexpr int kCols = 4;

// A tab's items in grid order: rarest first, then by weapon and name.
std::vector<const Item*> Shown(const Inventory& inv, const std::string& tab);
// The item the panel shows when nothing is picked: the first of every item (0: none).
int64_t FirstItem(const Inventory& inv);

// The grid screen (hud_screen.h spec): tabs, one scrolling grid of tiles, close. Clicks come back
// as "#tab <id>", "#pick <id>", "#close". `page` is unused (no pages). `picked` marks its tile.
std::string GridSpec(const Inventory& inv, const std::string& tab, int page, const Text& tr, int64_t picked = 0);
// The item panel beside the grid (drawer.*, appended to GridSpec; always shown): the picture, its
// facts and a toggle per side: CT and T, or the one side that buys it. Clicks: "#side <id> <team>".
std::string DetailSpec(const Item& it, bool pending, const Text& tr);
// The item panel with nothing to show: no items, or skins off.
std::string EmptyDetailSpec(const Inventory& inv, const Text& tr);

// What a click on a side's toggle sends to the platform (skins.equip), in order: one step, or two
// when an item on both sides comes off one of them (off both, then on the other side again).
struct EquipStep {
  int team = 0;  // 0 both, 2 T, 3 CT
  bool unequip = false;
};
std::vector<EquipStep> SideClick(const Item& it, int team);
// While the platform answers.
std::string LoadingSpec(const Text& tr);

}  // namespace inv
}  // namespace skins

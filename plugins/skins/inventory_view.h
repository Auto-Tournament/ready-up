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

constexpr int kTilesPerPage = 12;
constexpr int kCols = 4;

// The grid screen (hud_screen.h spec): tabs, a page of tiles, paging, close. Clicks come back as
// "#tab <id>", "#page <n>", "#pick <id>", "#close".
std::string GridSpec(const Inventory& inv, const std::string& tab, int page, const Text& tr);
// One item: the large picture, its facts, CT / T / Both (or Equip for a one-side weapon) and
// Unequip. Clicks: "#equip <id> <team>", "#unequip <id> <team>", "#back".
std::string DetailSpec(const Item& it, bool pending, const Text& tr);
// While the platform answers.
std::string LoadingSpec(const Text& tr);

}  // namespace inv
}  // namespace skins

// Offline tests for plugins/skins/inventory_view.h: the platform's skins.inventory as HUD screens.
// ctest `skins_inventory_view`.
#include "../inventory_view.h"

#include <cstdio>
#include <string>

using namespace skins::inv;

static int g_failures = 0;
#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (0)

static bool Has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

int main() {
  // The class matches hud/tools/gen_skins.py (FNV-1a 32): checked against a value it printed.
  CHECK(PictureClass("") == "sk-811c9dc5");
  CHECK(PictureClass("weapon_ak47_cu_ak47_cobra_light_png").size() == 11);

  const std::string json = R"({"steamid64":"76561198000000001","enabled":true,"items":[
    {"id":412,"weapon":"weapon_awp","weapon_name":"AWP","name":"Asiimov","rarity":"ancient","slot":"awp",
     "float":0.27,"pattern":412,"stattrak":1204,"side":null,"equipped":[0],"image":"weapon_awp_cu_awp_asimov_medium_png","source":"t50 MVP"},
    {"id":77,"weapon":"weapon_ak47","weapon_name":"AK-47","name":"Redline","rarity":"legendary","slot":"ak47",
     "float":0.18,"pattern":77,"stattrak":null,"side":2,"equipped":[],"image":"weapon_ak47_cu_ak47_cobra_medium_png"},
    {"id":5,"weapon":"weapon_knife_m9_bayonet","weapon_name":"M9 Bayonet","name":"Gamma Doppler","rarity":"ancient","slot":"knife",
     "float":0.01,"equipped":[3],"image":""}]})";
  Inventory inv;
  CHECK(ParseInventory(json, &inv));
  CHECK(inv.enabled);
  CHECK(inv.items.size() == 3);
  CHECK(inv.items[0].on[0] && !inv.items[0].on[3]);
  CHECK(inv.items[1].side == 2);
  CHECK(inv.items[0].stattrak == 1204 && inv.items[1].stattrak == -1);
  CHECK(TabOf(inv.items[0]) == "rifles");
  CHECK(TabOf(inv.items[2]) == "knives");
  CHECK(WearShort(0.27) == "FT" && WearName(0.01) == "Factory New");

  const Text tr = [](const std::string& key, const std::vector<std::pair<std::string, std::string>>&) { return "[" + key + "]"; };

  // Grid: tabs only where there are items, a tile per item with picture and marks, close.
  const std::string grid = GridSpec(inv, "all", 0, tr);
  CHECK(Has(grid, "id=skins-grid\n"));
  CHECK(Has(grid, "where=inv\n"));
  CHECK(Has(grid, "tab=[inv.tab.all]|3|#tab all|1\n"));
  CHECK(Has(grid, "tab=[inv.tab.rifles]|2|#tab rifles|0\n"));
  CHECK(!Has(grid, "inv.tab.pistols"));
  // label | tone | state | cmd | weapon (sub) | picture | marks | wear (note)
  CHECK(Has(grid, "btn=Asiimov|ghost||#pick 412|AWP|" + PictureClass("weapon_awp_cu_awp_asimov_medium_png") + "|ct t eq r-covert|FT 0.27\n"));
  CHECK(Has(grid, "btn=Redline|ghost||#pick 77|AK-47|"));
  CHECK(Has(grid, "btn=Redline|ghost||#pick 77|AK-47|" + PictureClass("weapon_ak47_cu_ak47_cobra_medium_png") + "|r-classified|"));  // not equipped: no eq
  CHECK(Has(grid, "|sk-none|ct eq r-covert|FN 0.01\n"));  // the knife: on CT only, no picture
  CHECK(Has(grid, "close=\xC3\x97|#close\n"));
  CHECK(Has(grid, "counter=[inv.count]\n") && Has(grid, "hint=[inv.close_hint]\n"));  // the footer

  // Tab filter and paging.
  CHECK(Has(GridSpec(inv, "knives", 0, tr), "title=[inv.tab.knives]\n"));
  CHECK(!Has(GridSpec(inv, "knives", 0, tr), "#pick 412"));
  Inventory many;
  many.enabled = true;
  for (int i = 1; i <= 30; ++i) {
    Item it;
    it.id = i;
    it.weapon = "weapon_glock";
    it.weaponName = "Glock-18";
    it.name = "Fade";
    many.items.push_back(it);
  }
  // No pages: one grid that scrolls, every item of the tab.
  const std::string all = GridSpec(many, "all", 0, tr);
  CHECK(Has(all, "#pick 1|") && Has(all, "#pick 30|") && !Has(all, "#page"));

  // Detail: both sides buy the AWP: CT, T, Both (equipped for both: all picked), Unequip.
  const std::string awp = DetailSpec(inv.items[0], false, tr);
  CHECK(Has(awp, "img=" + PictureClass("weapon_awp_cu_awp_asimov_medium_png") + "\n"));
  CHECK(Has(awp, "title=AWP | Asiimov\n"));  // the title is one field: its `|` stays
  CHECK(Has(awp, "btn=CT|ct|picked|#equip 412 3\n"));
  CHECK(Has(awp, "btn=T|t|picked|#equip 412 2\n"));
  CHECK(Has(awp, "btn=[inv.both]|ghost|picked|#equip 412 0\n"));
  CHECK(Has(awp, "#unequip 412 0\n"));
  CHECK(Has(awp, "where=invd\n") && Has(awp, "counter=[inv.equip_on]\n"));
  CHECK(Has(awp, "line=[inv.from]|t50 MVP\n"));
  // Only T buys the AK-47: one Equip, no side buttons, not equipped: no Unequip.
  const std::string ak = DetailSpec(inv.items[1], true, tr);
  CHECK(Has(ak, "btn=[inv.equip]|green||#equip 77 0\n"));
  CHECK(!Has(ak, "btn=CT|"));
  CHECK(!Has(ak, "#unequip"));
  CHECK(Has(ak, "hint=[inv.saving]\n"));

  // Skins off.
  Inventory off;
  CHECK(ParseInventory(R"({"steamid64":"1","enabled":false,"error":"Skins are off","items":[]})", &off));
  CHECK(!off.enabled && Has(GridSpec(off, "all", 0, tr), "sub=Skins are off\n"));

  if (g_failures) {
    std::fprintf(stderr, "inventory_view: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("inventory_view: ok\n");
  return 0;
}

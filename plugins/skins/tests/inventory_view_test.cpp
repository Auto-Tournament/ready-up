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

  // Rarest first: the covert AWP and knife before the classified AK, then by weapon (AWP < M9).
  CHECK(grid.find("#pick 412|") < grid.find("#pick 5|") && grid.find("#pick 5|") < grid.find("#pick 77|"));

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

  // The picked tile is marked; the panel's default is the first item of all (rarest first).
  CHECK(Has(GridSpec(inv, "all", 0, tr, 77), "btn=Redline|ghost|picked|#pick 77|"));
  CHECK(FirstItem(inv) == 412);
  CHECK(FirstItem(Inventory{}) == 0);

  // The item panel: both sides buy the AWP, on both: CT and T lit, toggles (no Both, Unequip, Back).
  const std::string awp = DetailSpec(inv.items[0], false, tr);
  CHECK(Has(awp, "drawer.img=" + PictureClass("weapon_awp_cu_awp_asimov_medium_png") + "\n"));
  CHECK(Has(awp, "drawer.kind=AWP\n") && Has(awp, "drawer.title=Asiimov\n"));
  CHECK(Has(awp, "drawer.btn=CT|ct|picked|#side 412 3\n"));
  CHECK(Has(awp, "drawer.btn=T|t|picked|#side 412 2\n"));
  CHECK(!Has(awp, "inv.both") && !Has(awp, "unequip") && !Has(awp, "drawer.close"));
  CHECK(!Has(awp, "where=") && !Has(awp, "id=") && Has(awp, "drawer.label=[inv.equip_on]\n"));
  CHECK(Has(awp, "drawer.line=[inv.from]|t50 MVP\n"));
  CHECK(Has(awp, "drawer.hint=[inv.toggle_hint] [inv.saved_hint]\n"));
  // Only T buys the AK-47: its one toggle (off), and why.
  const std::string ak = DetailSpec(inv.items[1], true, tr);
  CHECK(Has(ak, "drawer.btn=T|t||#side 77 2\n"));
  CHECK(!Has(ak, "drawer.btn=CT|"));
  CHECK(Has(ak, "drawer.line=[inv.one_side]|\n"));
  CHECK(Has(ak, "drawer.hint=[inv.saving]\n"));

  // What a toggle sends. On both, CT off: off both, then back on T.
  auto steps = SideClick(inv.items[0], 3);
  CHECK(steps.size() == 2 && steps[0].team == 0 && steps[0].unequip && steps[1].team == 2 && !steps[1].unequip);
  Item k = inv.items[2];  // the knife: on CT only
  steps = SideClick(k, 3);  // CT again: off CT
  CHECK(steps.size() == 1 && steps[0].team == 3 && steps[0].unequip);
  steps = SideClick(k, 2);  // T as well: both
  CHECK(steps.size() == 1 && steps[0].team == 0 && !steps[0].unequip);
  k.on[3] = false;
  steps = SideClick(k, 2);  // on neither: T
  CHECK(steps.size() == 1 && steps[0].team == 2 && !steps[0].unequip);
  steps = SideClick(inv.items[1], 2);  // the AK-47, one side, off: on (kept as both)
  CHECK(steps.size() == 1 && steps[0].team == 0 && !steps[0].unequip);
  Item ak2 = inv.items[1];
  ak2.on[0] = true;
  steps = SideClick(ak2, 2);  // on: off
  CHECK(steps.size() == 1 && steps[0].team == 0 && steps[0].unequip);
  CHECK(SideClick(k, 0).empty());

  // No items, or skins off: the panel says so; the grid has no sub.
  Inventory none;
  none.enabled = true;
  CHECK(Has(EmptyDetailSpec(none, tr), "drawer.title=[inv.empty_title]\ndrawer.sub=[inv.empty]\n"));
  CHECK(!Has(GridSpec(none, "all", 0, tr), "sub="));
  Inventory off;
  CHECK(ParseInventory(R"({"steamid64":"1","enabled":false,"error":"Skins are off","items":[]})", &off));
  CHECK(!off.enabled && Has(EmptyDetailSpec(off, tr), "drawer.title=[inv.off_title]\ndrawer.sub=Skins are off\n"));

  if (g_failures) {
    std::fprintf(stderr, "inventory_view: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("inventory_view: ok\n");
  return 0;
}

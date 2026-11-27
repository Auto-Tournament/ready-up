#include "inventory_view.h"

#include "readyup/json_store.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace skins {
namespace inv {
namespace {

using readyup::json_store::Json;

std::string Str(const Json& o, const char* k) {
  const Json* v = o.Find(k);
  return v && v->type() == Json::Type::String ? v->AsString() : std::string();
}
bool Num(const Json& o, const char* k, double* out) {
  const Json* v = o.Find(k);
  if (!v || (v->type() != Json::Type::Int && v->type() != Json::Type::Double)) return false;
  *out = v->AsDouble();
  return true;
}

// A value inside a `key=value` spec line: no newlines, and no `|` (it splits the fields).
std::string Clean(std::string s) {
  for (char& c : s)
    if (c == '\n' || c == '\r' || c == '|') c = ' ';
  return s;
}
// A whole-line value (title, sub): only no newlines; its `|` stays.
std::string Line(std::string s) {
  for (char& c : s)
    if (c == '\n' || c == '\r') c = ' ';
  return s;
}

bool In(const std::string& w, std::initializer_list<const char*> names) {
  for (const char* n : names)
    if (w == n) return true;
  return false;
}

std::string RarityMark(const std::string& r) {
  if (r == "common" || r == "consumer") return "r-consumer";
  if (r == "uncommon" || r == "industrial") return "r-industrial";
  if (r == "rare" || r == "milspec" || r == "mil-spec") return "r-milspec";
  if (r == "mythical" || r == "restricted") return "r-restricted";
  if (r == "legendary" || r == "classified") return "r-classified";
  if (r == "ancient" || r == "covert") return "r-covert";
  if (r == "immortal" || r == "contraband" || r == "gold") return "r-gold";
  return "";
}

// Consumer (0) .. contraband (6); -1 unknown.
int RarityRank(const std::string& r) {
  static const char* const kOrder[] = {"r-consumer", "r-industrial", "r-milspec", "r-restricted", "r-classified", "r-covert", "r-gold"};
  const std::string m = RarityMark(r);
  for (int i = 0; i < 7; ++i)
    if (m == kOrder[i]) return i;
  return -1;
}

std::string Marks(const Item& it) {
  std::string m;
  if (it.on[0] || it.on[3]) m += "ct ";
  if (it.on[0] || it.on[2]) m += "t ";
  if (it.on[0] || it.on[2] || it.on[3]) m += "eq ";  // equipped: the tile's border in the accent
  m += RarityMark(it.rarity);
  return m;
}

std::string Fmt(const char* f, double v) {
  char b[32];
  std::snprintf(b, sizeof(b), f, v);
  return b;
}

}  // namespace

bool ParseInventory(const std::string& json, Inventory* out) {
  Json p;
  if (!Json::Parse(json, &p) || !p.IsObject()) return false;
  Inventory inv;
  const Json* en = p.Find("enabled");
  inv.enabled = en && en->type() == Json::Type::Bool && en->AsBool();
  inv.error = Str(p, "error");
  if (const Json* items = p.Find("items"); items && items->type() == Json::Type::Array) {
    for (const Json& j : items->Items()) {
      if (!j.IsObject()) continue;
      Item it;
      double d = 0;
      if (!Num(j, "id", &d) || d < 1) continue;
      it.id = static_cast<int64_t>(d);
      it.weapon = Str(j, "weapon");
      it.weaponName = Str(j, "weapon_name");
      it.name = Str(j, "name");
      it.rarity = Str(j, "rarity");
      it.slot = Str(j, "slot");
      it.image = Str(j, "image");
      it.source = Str(j, "source");
      if (Num(j, "float", &d)) it.wear = d;
      if (Num(j, "pattern", &d)) it.pattern = static_cast<int64_t>(d);
      if (Num(j, "stattrak", &d)) it.stattrak = static_cast<int64_t>(d);
      if (Num(j, "side", &d) && (d == 2 || d == 3)) it.side = static_cast<int>(d);
      if (const Json* eq = j.Find("equipped"); eq && eq->type() == Json::Type::Array) {
        for (const Json& t : eq->Items()) {
          const long long team = t.AsInt();
          if (team == 0 || team == 2 || team == 3) it.on[team] = true;
        }
      }
      inv.items.push_back(std::move(it));
    }
  }
  *out = std::move(inv);
  return true;
}

std::string PictureClass(const std::string& image) {
  uint32_t h = 0x811C9DC5u;
  for (unsigned char c : image) {
    h ^= c;
    h *= 0x01000193u;
  }
  char b[16];
  std::snprintf(b, sizeof(b), "sk-%08x", h);
  return b;
}

const std::vector<std::string>& Tabs() {
  static const std::vector<std::string> k = {"all", "knives", "gloves", "pistols", "rifles", "smgs", "heavy", "agents", "other"};
  return k;
}

std::string TabOf(const Item& it) {
  std::string w = it.weapon;
  if (w.rfind("weapon_", 0) == 0) w = w.substr(7);
  if (it.slot == "knife" || w.rfind("knife", 0) == 0 || w == "bayonet") return "knives";
  if (it.slot == "gloves" || w.find("gloves") != std::string::npos || w.find("handwraps") != std::string::npos) return "gloves";
  if (it.slot.rfind("agent", 0) == 0 || w.rfind("agent", 0) == 0) return "agents";
  if (In(w, {"glock", "usp_silencer", "hkp2000", "p250", "fiveseven", "tec9", "cz75a", "deagle", "revolver", "elite"})) return "pistols";
  if (In(w, {"ak47", "m4a1", "m4a1_silencer", "famas", "galilar", "aug", "sg556", "awp", "ssg08", "scar20", "g3sg1"})) return "rifles";
  if (In(w, {"mac10", "mp9", "mp7", "mp5sd", "ump45", "p90", "bizon"})) return "smgs";
  if (In(w, {"nova", "xm1014", "sawedoff", "mag7", "m249", "negev"})) return "heavy";
  return "other";
}

std::string WearName(double f) {
  if (f < 0) return "";
  if (f < 0.07) return "Factory New";
  if (f < 0.15) return "Minimal Wear";
  if (f < 0.38) return "Field-Tested";
  if (f < 0.45) return "Well-Worn";
  return "Battle-Scarred";
}

std::string WearShort(double f) {
  if (f < 0) return "";
  if (f < 0.07) return "FN";
  if (f < 0.15) return "MW";
  if (f < 0.38) return "FT";
  if (f < 0.45) return "WW";
  return "BS";
}

std::vector<const Item*> Shown(const Inventory& inv, const std::string& tab) {
  std::vector<const Item*> shown;
  for (const Item& it : inv.items)
    if (tab == "all" || TabOf(it) == tab) shown.push_back(&it);
  // Rarest first (Sivert, 2026-11-26), then by weapon and name; the platform's order (newest) breaks ties.
  std::stable_sort(shown.begin(), shown.end(), [](const Item* a, const Item* b) {
    const int ra = RarityRank(a->rarity), rb = RarityRank(b->rarity);
    if (ra != rb) return ra > rb;
    if (a->weaponName != b->weaponName) return a->weaponName < b->weaponName;
    return a->name < b->name;
  });
  return shown;
}

int64_t FirstItem(const Inventory& inv) {
  const auto all = Shown(inv, "all");
  return all.empty() ? 0 : all.front()->id;
}

std::string GridSpec(const Inventory& inv, const std::string& tab, int page, const Text& tr, int64_t picked) {
  // The inventory's own panel (at_ui ui_inv): kind = INVENTORY over the tabs, the title over the
  // grid, counter and how to close in the footer; the item panel (drawer.*) beside it.
  std::string s = "id=skins-grid\nwhere=inv\nmouse=on\n";
  s += "kind=" + Clean(tr("inv.kind", {})) + "\n";
  const std::vector<const Item*> shown = Shown(inv, tab);
  int counts[16] = {0};
  const auto& tabs = Tabs();
  for (const Item& it : inv.items) {
    const std::string t = TabOf(it);
    for (size_t i = 0; i < tabs.size(); ++i)
      if (tabs[i] == "all" || tabs[i] == t) ++counts[i];
  }
  for (size_t i = 0; i < tabs.size(); ++i) {
    if (counts[i] == 0 && tabs[i] != "all" && tabs[i] != tab) continue;  // only tabs with items
    s += "tab=" + Clean(tr("inv.tab." + tabs[i], {})) + "|" + std::to_string(counts[i]) + "|#tab " + tabs[i] + "|" +
         (tabs[i] == tab ? "1" : "0") + "\n";
  }
  s += "title=" + Clean(tr("inv.tab." + tab, {})) + "\n";
  // An empty tab says so; no items at all (or skins off) is the item panel's to say (EmptyDetailSpec).
  if (inv.enabled && shown.empty() && !inv.items.empty()) s += "sub=" + Clean(tr("inv.empty_tab", {})) + "\n";
  // One grid that scrolls (Sivert, 2026-11-26: no pages): the tab's items, up to kMaxShown.
  (void)page;
  for (size_t i = 0; i < shown.size() && i < static_cast<size_t>(kMaxShown); ++i) {
    // A tile: weapon (sub) over the name (label), the wear (note) beside the CT / T chips (marks).
    const Item& it = *shown[i];
    const std::string wear = it.wear >= 0 ? WearShort(it.wear) + " " + Fmt("%.2f", it.wear) : std::string();
    // No picture class still makes a tile (has-img): an empty picture box, as for an item the game has none of.
    const std::string img = it.image.empty() ? "sk-none" : PictureClass(it.image);
    s += "btn=" + Clean(it.name.empty() ? it.weaponName : it.name) + "|ghost|" + (it.id == picked ? "picked" : "") +
         "|#pick " + std::to_string(it.id) + "|" +
         Clean(it.weaponName) + "|" + img + "|" + Marks(it) + "|" + Clean(wear) + "\n";
  }
  // The footer, as drafted: how many items this tab has, and how to close.
  s += "counter=" + Clean(tr("inv.count", {{"n", std::to_string(shown.size())}})) + "\n";
  s += "hint=" + Clean(tr("inv.close_hint", {})) + "\n";
  s += "close=\xC3\x97|#close\n";  // × (drawn by the addon; the label is only the button's name)
  return s;
}

std::string DetailSpec(const Item& it, bool pending, const Text& tr) {
  // The item panel right of the grid (drawer.*), always there (Sivert, 2026-11-26): the weapon (kind)
  // with the close beside it, the picture, the name (title), wear / pattern / StatTrak (sub), where it
  // came from (line), EQUIP ON (label) over a toggle per side: lit = this item is on that side, a
  // click takes it off again. A note under them (hint).
  std::string s;
  if (!it.image.empty()) s += "drawer.img=" + PictureClass(it.image) + "\n";
  s += "drawer.kind=" + Clean(it.weaponName) + "\n";
  s += "drawer.title=" + Line(it.name.empty() ? it.weaponName : it.name) + "\n";
  std::string sub;
  if (it.wear >= 0) sub = WearName(it.wear) + " " + Fmt("%.3f", it.wear);
  if (it.pattern >= 0) sub += (sub.empty() ? "" : " \xC2\xB7 ") + tr("inv.pattern", {{"n", std::to_string(it.pattern)}});
  if (it.stattrak >= 0) sub += (sub.empty() ? "" : " \xC2\xB7 ") + tr("inv.stattrak", {{"n", std::to_string(it.stattrak)}});
  if (!sub.empty()) s += "drawer.sub=" + Clean(sub) + "\n";
  if (!it.source.empty()) s += "drawer.line=" + Clean(tr("inv.from", {})) + "|" + Clean(it.source) + "\n";
  const std::string id = std::to_string(it.id);
  s += "drawer.label=" + Clean(tr("inv.equip_on", {})) + "\n";
  if (it.side == 0) {
    // Both sides buy it: CT and T, each lit while this item is on it ("both" lights both).
    s += "drawer.btn=CT|ct|" + std::string(it.on[0] || it.on[3] ? "picked" : "") + "|#side " + id + " 3\n";
    s += "drawer.btn=T|t|" + std::string(it.on[0] || it.on[2] ? "picked" : "") + "|#side " + id + " 2\n";
  } else {
    // One side buys it: that side's toggle alone, and why there is only one.
    const bool any = it.on[0] || it.on[2] || it.on[3];
    const char* side = it.side == 3 ? "CT" : "T";
    s += std::string("drawer.btn=") + side + "|" + (it.side == 3 ? "ct" : "t") + "|" + (any ? "picked" : "") +
         "|#side " + id + " " + std::to_string(it.side) + "\n";
    s += "drawer.line=" + Clean(tr("inv.one_side", {{"side", side}, {"weapon", it.weaponName}})) + "|\n";
  }
  s += "drawer.hint=" + Clean(pending ? tr("inv.saving", {}) : tr("inv.toggle_hint", {}) + " " + tr("inv.saved_hint", {})) + "\n";
  return s;
}

std::string EmptyDetailSpec(const Inventory& inv, const Text& tr) {
  // Nothing to show: the panel keeps its place (an empty picture box) and says why.
  if (!inv.enabled)
    return "drawer.title=" + Clean(tr("inv.off_title", {})) + "\ndrawer.sub=" +
           Clean(inv.error.empty() ? tr("inv.off", {}) : inv.error) + "\n";
  return "drawer.title=" + Clean(tr("inv.empty_title", {})) + "\ndrawer.sub=" + Clean(tr("inv.empty", {})) + "\n";
}

std::vector<EquipStep> SideClick(const Item& it, int team) {
  if (team != 2 && team != 3) return {};
  if (it.side != 0) {
    // One side buys it: the platform keeps it as "both"; on, or off from wherever it is.
    if (it.on[0] || it.on[2] || it.on[3]) return {{it.on[0] ? 0 : it.on[3] ? 3 : 2, true}};
    return {{0, false}};
  }
  const int other = team == 3 ? 2 : 3;
  if (it.on[0]) return {{0, true}, {other, false}};  // on both: off both, back on the other side
  if (it.on[team]) return {{team, true}};
  if (it.on[other]) return {{0, false}};  // on the other side already: both
  return {{team, false}};
}

std::string LoadingSpec(const Text& tr) {
  return "id=skins-loading\nwhere=inv\nmouse=on\nkind=" + Clean(tr("inv.kind", {})) + "\ntitle=" +
         Clean(tr("inv.loading", {})) + "\nclose=\xC3\x97|#close\n";
}

}  // namespace inv
}  // namespace skins

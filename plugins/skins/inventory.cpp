// The in-game inventory (`.skins`): the player's items from the platform as HUD screens
// (inventory_view.h), equipped with a click. Game thread only.
//
//   .skins               opens it, any time (Sivert, 2026-11-26: also mid-round)
//   skins.inventory_request / skins.inventory / skins.equip   over the fleet link (protocol v1)
//
// The platform holds a change made during a live round until the round ends (fleet/push/skins.ts),
// so a skin picked mid-round is in the player's hands from the next round, never at once.
#include "skins.h"

#include "inventory_view.h"
#include "readyup/fleet_iface.h"
#include "readyup/hud_iface.h"
#include "readyup/i18n_api.h"
#include "readyup/json_store.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace skins {
namespace {

using readyup::json_store::Json;

struct View {
  uint64_t steamid64 = 0;
  std::string tab = "all";
  int page = 0;
  int64_t picked = 0;     // the item in the panel; 0 = the first one (inv::FirstItem)
  bool waiting = true;    // no answer from the platform yet
  bool pending = false;   // an equip sent, its answer not back
  int64_t queueSkin = 0;  // the skin of the steps still to send (a side click's second step)
  std::vector<inv::EquipStep> queue;
  double sentAt = 0;
};

std::map<int, View> g_views;                    // slot -> open inventory
std::map<uint64_t, inv::Inventory> g_inventory;  // steamid64 -> the platform's last answer
double g_now = 0;

const ru_fleet_v1* Fleet() {
  auto* f = static_cast<const ru_fleet_v1*>(g_api->get_interface(g_api->self, RU_FLEET_IFACE_NAME, 1));
  if (!f || f->struct_size < offsetof(ru_fleet_v1, add_capability) + sizeof(f->add_capability)) return nullptr;
  return f->connection_state() == RU_FLEET_LINK_STANDALONE ? nullptr : f;
}

// The inventory is a HUD menu (hud_iface.h `menu`), not the base screen: the match plugin keeps its
// own screen on every player (the Ready chip in warmup) and redraws it, which replaced the inventory
// at once. A menu is drawn over that screen, which comes back when the menu closes.
constexpr const char* kMenuOwner = "skins";

const ru_hud_v1* Hud() {
  auto* h = static_cast<const ru_hud_v1*>(g_api->get_interface(g_api->self, RU_HUD_IFACE_NAME, 1));
  return h && h->struct_size >= offsetof(ru_hud_v1, menu) + sizeof(h->menu) && h->menu ? h : nullptr;
}

bool HudOff(uint64_t sid) {
  const ru_hud_v1* h = Hud();
  if (!h || h->struct_size < offsetof(ru_hud_v1, player_off) + sizeof(h->player_off) || !h->player_off) return false;
  return h->player_off(sid) != 0;
}

inv::Text TextFor(int slot) {
  const std::string lang = readyup::i18n::LanguageOf(g_api, slot);
  return [lang](const std::string& key, const std::vector<std::pair<std::string, std::string>>& args) {
    return SkinsText(lang, key, args);
  };
}

void Chat(int slot, const std::string& key) {
  g_api->chat_to_slot(g_api->self, slot, SkinsText(readyup::i18n::LanguageOf(g_api, slot), key, {}).c_str());
}

void OnClick(void* user, int slot, const char* command);

void Draw(int slot) {
  auto it = g_views.find(slot);
  const ru_hud_v1* h = Hud();
  if (it == g_views.end() || !h) return;
  View& v = it->second;
  const inv::Text tr = TextFor(slot);
  std::string spec;
  auto inv = g_inventory.find(v.steamid64);
  if (v.waiting || inv == g_inventory.end()) {
    spec = inv::LoadingSpec(tr);
  } else {
    // The grid and, always beside it, the item panel: the picked item, else the first one
    // (Sivert, 2026-11-26). A tab change keeps the item.
    const inv::Item* item = nullptr;
    for (int pass = 0; pass < 2 && !item; ++pass) {
      if (pass) v.picked = inv::FirstItem(inv->second);
      for (const auto& i : inv->second.items)
        if (i.id == v.picked) item = &i;
    }
    spec = inv::GridSpec(inv->second, v.tab, v.page, tr, item ? item->id : 0);
    spec += item ? inv::DetailSpec(*item, v.pending, tr) : inv::EmptyDetailSpec(inv->second, tr);
  }
  h->menu(slot, kMenuOwner, spec.c_str(), &OnClick, nullptr);
}

void Close(int slot) {
  if (!g_views.erase(slot)) return;
  if (const ru_hud_v1* h = Hud()) h->menu(slot, kMenuOwner, "", nullptr, nullptr);
}

bool Send(const char* type, const Json& payload) {
  const ru_fleet_v1* f = Fleet();
  return f && f->send_event(type, payload.Dump().c_str(), 0, 0);
}

void Request(View& v) {
  Json p = Json::Object();
  p["steamid64"] = std::to_string(v.steamid64);
  Send("skins.inventory_request", p);
  v.sentAt = g_now;
}

bool SendStep(View& v, int64_t skin, const inv::EquipStep& st) {
  Json p = Json::Object();
  p["steamid64"] = std::to_string(v.steamid64);
  p["skin_id"] = static_cast<long long>(skin);
  p["team"] = static_cast<long long>(st.team);
  if (st.unequip) p["unequip"] = true;
  if (!Send("skins.equip", p)) return false;
  v.pending = true;
  v.sentAt = g_now;
  return true;
}

void OnClick(void*, int slot, const char* command) {
  auto it = g_views.find(slot);
  if (it == g_views.end() || !command) return;
  View& v = it->second;
  const std::string c = command;
  auto arg = [&](size_t n) -> std::string {
    size_t a = 0;
    for (size_t i = 0; i <= n; ++i) {
      a = c.find(' ', a);
      if (a == std::string::npos) return "";
      ++a;
    }
    const size_t b = c.find(' ', a);
    return c.substr(a, b == std::string::npos ? std::string::npos : b - a);
  };
  if (c == "#close") return Close(slot);
  if (c.rfind("#tab ", 0) == 0) {
    v.tab = arg(0);
    v.page = 0;
  } else if (c.rfind("#page ", 0) == 0) {
    v.page = std::max(0, std::atoi(arg(0).c_str()));
  } else if (c.rfind("#pick ", 0) == 0) {
    v.picked = std::atoll(arg(0).c_str());
  } else if (c.rfind("#side ", 0) == 0) {
    // A side's toggle: on, or off again (Sivert, 2026-11-26). Ignored while a change is on its way.
    if (v.pending) return;
    auto inv = g_inventory.find(v.steamid64);
    if (inv == g_inventory.end()) return;
    const int64_t skin = std::atoll(arg(0).c_str());
    const inv::Item* item = nullptr;
    for (const auto& i : inv->second.items)
      if (i.id == skin) item = &i;
    if (!item) return;
    const std::vector<inv::EquipStep> steps = inv::SideClick(*item, std::atoi(arg(1).c_str()));
    if (steps.empty() || !SendStep(v, skin, steps.front())) return;
    v.queue.assign(steps.begin() + 1, steps.end());
    v.queueSkin = skin;
  } else {
    return;
  }
  Draw(slot);
}

}  // namespace

std::string InventoryFleetType() { return "skins.inventory"; }

void InventoryFleetMessage(void*, const ru_fleet_msg* m) {
  if (!m || !m->payload_json) return;
  try {
    inv::Inventory got;
    if (!inv::ParseInventory(m->payload_json, &got)) return;
    Json p;
    if (!Json::Parse(m->payload_json, &p)) return;
    const Json* s = p.Find("steamid64");
    const uint64_t sid = s && s->type() == Json::Type::String ? std::strtoull(s->AsString().c_str(), nullptr, 10) : 0;
    if (!sid) return;
    Log(RU_LOG_INFO, "inventory: %zu item(s) for %llu%s%s", got.items.size(), static_cast<unsigned long long>(sid),
        got.enabled ? "" : " (off: ", got.enabled ? "" : (got.error + ")").c_str());
    g_inventory[sid] = std::move(got);
    for (auto& [slot, v] : g_views) {
      if (v.steamid64 != sid) continue;
      v.waiting = false;
      v.pending = false;
      if (!v.queue.empty()) {  // the next step of a side click
        const inv::EquipStep st = v.queue.front();
        v.queue.erase(v.queue.begin());
        SendStep(v, v.queueSkin, st);
      }
      Draw(slot);
    }
  } catch (...) {
  }
}

void OpenInventory(int slot, uint64_t steamid64) {
  if (slot < 0 || !steamid64) return;
  SKINS_DEBUG("inventory: .skins from slot %d", slot);
  if (!Fleet()) return Chat(slot, "inv.no_platform");
  if (!Hud()) return Chat(slot, "inv.no_hud");
  if (HudOff(steamid64)) return Chat(slot, "inv.hud_off");
  // `.skins` again closes it (Sivert, 2026-11-26).
  if (g_views.count(slot)) return Close(slot);
  View& v = g_views[slot];
  v = View{};
  v.steamid64 = steamid64;
  v.waiting = !g_inventory.count(steamid64);  // the last answer draws at once, the new one replaces it
  Request(v);
  Draw(slot);
  Log(RU_LOG_INFO, "inventory: opened for slot %d (%llu)%s", slot, static_cast<unsigned long long>(steamid64),
      v.waiting ? ", asking the platform" : "");
}

void InventoryTick(double now) {
  g_now = now;
  for (auto it = g_views.begin(); it != g_views.end();) {
    const int slot = it->first;
    ru_player p{};
    p.struct_size = sizeof(p);
    const bool here = g_api->get_player(g_api->self, slot, &p) && p.connected && p.steamid64 == it->second.steamid64;
    if (!here) {
      it = g_views.erase(it);
      continue;
    }
    ++it;
    View& v = g_views[slot];
    if ((v.waiting || v.pending) && now - v.sentAt > 6.0) {  // no answer: ask again
      v.pending = false;
      v.queue.clear();
      Request(v);
    }
  }
}

void InventoryForgetAll() {
  if (!g_views.empty())
    if (const ru_hud_v1* h = Hud()) h->menu(RU_HUD_ALL, kMenuOwner, "", nullptr, nullptr);
  g_views.clear();
  g_inventory.clear();
}

}  // namespace skins

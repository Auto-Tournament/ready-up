// The in-game inventory (`.skins`): the player's items from the platform as HUD screens
// (inventory_view.h), equipped with a click. Game thread only.
//
//   .skins               opens it (warmup, freeze time, or while dead / spectating)
//   skins.inventory_request / skins.inventory / skins.equip   over the fleet link (protocol v1)
//
// The platform holds a change made during a live round until the round ends (fleet/push/skins.ts),
// so nothing changes in a player's hands mid-round, whichever way it was equipped.
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
  int64_t picked = 0;     // the item shown on its own; 0 = the grid
  bool waiting = true;    // no answer from the platform yet
  bool pending = false;   // an equip sent, its answer not back
  double sentAt = 0;
};

std::map<int, View> g_views;                    // slot -> open inventory
std::map<uint64_t, inv::Inventory> g_inventory;  // steamid64 -> the platform's last answer
bool g_freeze = false;                           // round_prestart .. round_freeze_end
double g_now = 0;

const ru_fleet_v1* Fleet() {
  auto* f = static_cast<const ru_fleet_v1*>(g_api->get_interface(g_api->self, RU_FLEET_IFACE_NAME, 1));
  if (!f || f->struct_size < offsetof(ru_fleet_v1, add_capability) + sizeof(f->add_capability)) return nullptr;
  return f->connection_state() == RU_FLEET_LINK_STANDALONE ? nullptr : f;
}

const ru_hud_v1* Hud() {
  auto* h = static_cast<const ru_hud_v1*>(g_api->get_interface(g_api->self, RU_HUD_IFACE_NAME, 1));
  return h && h->struct_size >= offsetof(ru_hud_v1, screen) + sizeof(h->screen) && h->screen ? h : nullptr;
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

// Alive on T or CT (the controller's m_bPawnIsAlive).
bool AliveOnTeam(int slot) {
  ru_player p{};
  p.struct_size = sizeof(p);
  if (!g_api->get_player(g_api->self, slot, &p) || !p.connected) return false;
  if (p.team != RU_TEAM_T && p.team != RU_TEAM_CT) return false;
  static int alive = -2;
  if (alive == -2) alive = SchemaOffset("CCSPlayerController", "m_bPawnIsAlive");
  void* ctrl = g_api->entity_by_index(g_api->self, slot + 1);
  if (!ctrl || alive < 0) return true;  // can't tell: treat as alive
  return *(static_cast<uint8_t*>(ctrl) + alive) != 0;
}

// When the inventory may be open: never with a cursor while the player plays a round.
bool MayOpen(int slot) { return WarmupLike() || g_freeze || !AliveOnTeam(slot); }

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
  } else if (v.picked) {
    const inv::Item* item = nullptr;
    for (const auto& i : inv->second.items)
      if (i.id == v.picked) item = &i;
    if (item) spec = inv::DetailSpec(*item, v.pending, tr);
    else v.picked = 0;
  }
  if (spec.empty()) spec = inv::GridSpec(inv->second, v.tab, v.page, tr);
  h->screen(slot, spec.c_str(), &OnClick, nullptr);
}

void Close(int slot) {
  if (!g_views.erase(slot)) return;
  if (const ru_hud_v1* h = Hud()) h->screen(slot, "", nullptr, nullptr);
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
  if (c == "#back") {
    v.picked = 0;
  } else if (c.rfind("#tab ", 0) == 0) {
    v.tab = arg(0);
    v.page = 0;
  } else if (c.rfind("#page ", 0) == 0) {
    v.page = std::max(0, std::atoi(arg(0).c_str()));
  } else if (c.rfind("#pick ", 0) == 0) {
    v.picked = std::atoll(arg(0).c_str());
  } else if (c.rfind("#equip ", 0) == 0 || c.rfind("#unequip ", 0) == 0) {
    if (!MayOpen(slot)) return Close(slot);
    Json p = Json::Object();
    p["steamid64"] = std::to_string(v.steamid64);
    p["skin_id"] = static_cast<long long>(std::atoll(arg(0).c_str()));
    p["team"] = static_cast<long long>(std::atoi(arg(1).c_str()));
    if (c.rfind("#unequip ", 0) == 0) p["unequip"] = true;
    if (Send("skins.equip", p)) v.pending = true;
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
    g_inventory[sid] = std::move(got);
    for (auto& [slot, v] : g_views) {
      if (v.steamid64 != sid) continue;
      v.waiting = false;
      v.pending = false;
      Draw(slot);
    }
  } catch (...) {
  }
}

void OpenInventory(int slot, uint64_t steamid64) {
  if (slot < 0 || !steamid64) return;
  if (!Fleet()) return Chat(slot, "inv.no_platform");
  if (!Hud()) return Chat(slot, "inv.no_hud");
  if (HudOff(steamid64)) return Chat(slot, "inv.hud_off");
  if (!MayOpen(slot)) return Chat(slot, "inv.not_now");
  View& v = g_views[slot];
  v = View{};
  v.steamid64 = steamid64;
  v.waiting = !g_inventory.count(steamid64);  // the last answer draws at once, the new one replaces it
  Request(v);
  Draw(slot);
}

void InventoryRoundEvent(const char* name) {
  if (!name) return;
  if (std::strcmp(name, "round_prestart") == 0) g_freeze = true;
  else if (std::strcmp(name, "round_freeze_end") == 0) g_freeze = false;
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
    if (!MayOpen(slot)) {
      Close(slot);  // the round started with them alive: no cursor mid-round
      continue;
    }
    View& v = g_views[slot];
    if ((v.waiting || v.pending) && now - v.sentAt > 6.0) {  // no answer: ask again
      v.pending = false;
      Request(v);
    }
  }
}

void InventoryForgetAll() {
  g_views.clear();
  g_inventory.clear();
}

}  // namespace skins

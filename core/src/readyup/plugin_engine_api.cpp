// Engine-facing members of the plugin API (v1.1): output, players, raw game event accessors,
// schema and entities, round-termination suppression, admin check. The loader
// (plugin_loader.cpp) owns registration bookkeeping; this file only adapts core services to
// the C ABI in core/include/readyup/plugin_api.h. Every member except is_admin is game thread
// only (detail::CheckGameThread).

#include "readyup/steam_ugc.h"
#include "readyup/admin_check.h"
#include "readyup/center_html.h"
#include "readyup/features.h"
#include "readyup/plugin_loader.h"
#include "readyup/round_termination_hook.h"
#include "readyup/schema.h"
#include "readyup/sdk/igameevents.h"
#include "readyup/entity.h"
#include "readyup/slot_registry.h"

#include "readyup/plugin_api.h"

#include <cstring>
#include <string>
#include <vector>

namespace readyup::plugins {
namespace {

using detail::CheckGameThread;
using sdk::CKV3MemberName;
using sdk::IGameEvent;

// ---- output ------------------------------------------------------------------------

int ApiCenterHtmlToSlot(ru_plugin* self, int slot, const char* html, int seconds) {
  if (!CheckGameThread(self, "center_html_to_slot") || !html || !*html) return 0;
  return PrintCenterHtmlToClientOnly(slot, html, seconds > 0 ? seconds : 1) ? 1 : 0;
}

int ApiCenterHtmlAll(ru_plugin* self, const char* html, int seconds) {
  if (!CheckGameThread(self, "center_html_all") || !html || !*html) return 0;
  int sent = 0;
  for (const auto& h : ListHumans()) {
    if (h.slot >= 0 && PrintCenterHtmlToClientOnly(h.slot, html, seconds > 0 ? seconds : 1)) ++sent;
  }
  return sent;
}

// ---- players -----------------------------------------------------------------------

void Fill(ru_player* out, int slot, uint64_t steamid64, int team, bool bot, const std::string& name, int userid) {
  // Fill only what the caller's struct has room for (struct_size is the caller's sizeof).
  ru_player p{};
  p.struct_size = out->struct_size;
  p.slot = slot;
  p.steamid64 = steamid64;
  p.team = team;
  p.is_bot = bot ? 1 : 0;
  p.connected = 1;
  std::strncpy(p.name, name.c_str(), sizeof(p.name) - 1);
  p.userid = userid;  // v1.2
  const size_t n = out->struct_size < sizeof(p) ? out->struct_size : sizeof(p);
  std::memcpy(out, &p, n);
}

bool ValidOut(const ru_player* out) {
  return out && out->struct_size >= offsetof(ru_player, connected) + sizeof(out->connected);
}

int ApiGetPlayer(ru_plugin* self, int slot, ru_player* out) {
  if (!CheckGameThread(self, "get_player") || !ValidOut(out) || slot < 0) return 0;
  for (const auto& h : ListHumans()) {
    if (h.slot == slot) {
      Fill(out, h.slot, h.steamid64, h.team, false, h.name, h.userid);
      return 1;
    }
  }
  return 0;
}

int ApiGetPlayerBySteam(ru_plugin* self, uint64_t steamid64, ru_player* out) {
  if (!CheckGameThread(self, "get_player_by_steamid") || !ValidOut(out) || steamid64 == 0) return 0;
  for (const auto& h : ListHumans()) {
    if (h.steamid64 == steamid64) {
      Fill(out, h.slot, h.steamid64, h.team, false, h.name, h.userid);
      return 1;
    }
  }
  return 0;
}

int ApiForEachPlayer(ru_plugin* self, ru_player_fn fn, void* user) {
  if (!CheckGameThread(self, "for_each_player") || !fn) return 0;
  int n = 0;
  ru_player p{};
  p.struct_size = sizeof(p);
  for (const auto& h : ListHumans()) {
    Fill(&p, h.slot, h.steamid64, h.team, false, h.name, h.userid);
    ++n;
    if (!fn(user, &p)) return n;
  }
  for (const auto& b : ListBots()) {
    Fill(&p, -1, 0, b.team, true, b.name, b.userid);
    ++n;
    if (!fn(user, &p)) return n;
  }
  return n;
}

// ---- raw game events ---------------------------------------------------------------

IGameEvent* Ev(const ru_game_event* ev) { return reinterpret_cast<IGameEvent*>(const_cast<ru_game_event*>(ev)); }

int ApiEvInt(ru_plugin* self, const ru_game_event* ev, const char* key, int def) {
  if (!CheckGameThread(self, "ev_get_int") || !ev || !key) return def;
  return Ev(ev)->GetInt(CKV3MemberName(key), def);
}

double ApiEvFloat(ru_plugin* self, const ru_game_event* ev, const char* key, double def) {
  if (!CheckGameThread(self, "ev_get_float") || !ev || !key) return def;
  return Ev(ev)->GetFloat(CKV3MemberName(key), static_cast<float>(def));
}

uint64_t ApiEvUint64(ru_plugin* self, const ru_game_event* ev, const char* key, uint64_t def) {
  if (!CheckGameThread(self, "ev_get_uint64") || !ev || !key) return def;
  return Ev(ev)->GetUint64(CKV3MemberName(key), def);
}

const char* ApiEvString(ru_plugin* self, const ru_game_event* ev, const char* key, const char* def) {
  const char* fallback = def ? def : "";
  if (!CheckGameThread(self, "ev_get_string") || !ev || !key) return fallback;
  const char* s = Ev(ev)->GetString(CKV3MemberName(key), fallback);
  return s ? s : fallback;
}

int ApiEvPlayerSlot(ru_plugin* self, const ru_game_event* ev, const char* key) {
  if (!CheckGameThread(self, "ev_get_player_slot") || !ev || !key) return -1;
  const int slot = Ev(ev)->GetPlayerSlot(CKV3MemberName(key)).value;
  return slot >= 0 && slot < 64 ? slot : -1;
}

void* ApiEvController(ru_plugin* self, const ru_game_event* ev, const char* key) {
  if (!CheckGameThread(self, "ev_get_player_controller") || !ev || !key) return nullptr;
  return Ev(ev)->GetPlayerController(CKV3MemberName(key));
}

void* ApiEvPawn(ru_plugin* self, const ru_game_event* ev, const char* key) {
  if (!CheckGameThread(self, "ev_get_player_pawn") || !ev || !key) return nullptr;
  return Ev(ev)->GetPlayerPawn(CKV3MemberName(key));
}

// ---- schema and entities -----------------------------------------------------------

int ApiSchemaOffset(ru_plugin* self, const char* cls, const char* field) {
  if (!CheckGameThread(self, "schema_offset") || !cls || !field) return -1;
  const auto v = SchemaFindOffset("server", cls, field);
  return v ? *v : -1;
}

int ApiEntitySystemStatus(ru_plugin* self) {
  if (!CheckGameThread(self, "entity_system_status")) return RU_ENTSYS_PENDING;
  return entity::EntitySystemStatus(nullptr);
}

void* ApiEntityByIndex(ru_plugin* self, int index) {
  if (!CheckGameThread(self, "entity_by_index")) return nullptr;
  return entity::EntityByIndex(index);
}

void* ApiEntityFromHandle(ru_plugin* self, uint32_t handle) {
  if (!CheckGameThread(self, "entity_from_handle")) return nullptr;
  return entity::EntityFromHandle(handle);
}

uint32_t ApiEntityHandleOf(ru_plugin* self, void* ent) {
  if (!CheckGameThread(self, "entity_handle_of") || !ent) return 0xFFFFFFFFu;
  return entity::EntityHandleOf(ent);
}

const char* ApiEntityClassname(ru_plugin* self, void* ent) {
  if (!CheckGameThread(self, "entity_classname") || !ent) return nullptr;
  return entity::EntityDesignerName(ent);
}

int ApiEntityMarkChanged(ru_plugin* self, void* ent) {
  if (!CheckGameThread(self, "entity_mark_changed") || !ent) return 0;
  return entity::MarkEntityFullyChanged(ent) ? 1 : 0;
}

int ApiEconAttrSet(ru_plugin* self, void* list, const char* name, double value) {
  if (!CheckGameThread(self, "econ_attr_set_by_name") || !list || !name) return 0;
  return entity::AttrSetOrAddByName(list, name, static_cast<float>(value)) ? 1 : 0;
}

int ApiChangeSubclass(ru_plugin* self, void* ent, const char* subclass) {
  if (!CheckGameThread(self, "entity_change_subclass") || !ent || !subclass) return 0;
  return entity::ChangeSubclass(ent, subclass) ? 1 : 0;
}

int ApiSetModel(ru_plugin* self, void* ent, const char* model) {
  if (!CheckGameThread(self, "entity_set_model") || !ent || !model || !*model) return 0;
  return entity::SetModel(ent, model) ? 1 : 0;
}

int ApiSetAbsOrigin(ru_plugin* self, void* ent, const float* origin) {
  if (!CheckGameThread(self, "entity_set_abs_origin") || !ent || !origin) return 0;
  return entity::SetAbsOrigin(ent, origin) ? 1 : 0;
}

int ApiSetBodygroup(ru_plugin* self, void* ent, const char* group, int value) {
  if (!CheckGameThread(self, "entity_set_bodygroup_by_name") || !ent || !group) return RU_BODYGROUP_UNAVAILABLE;
  switch (entity::SetBodygroupByName(ent, group, value)) {
    case entity::BodygroupResult::kOk: return RU_BODYGROUP_OK;
    case entity::BodygroupResult::kNoModel: return RU_BODYGROUP_NO_MODEL;
    case entity::BodygroupResult::kNoGroup: return RU_BODYGROUP_NO_GROUP;
    default: return RU_BODYGROUP_UNAVAILABLE;
  }
}

// ---- match control / admins --------------------------------------------------------

int ApiSetRoundTermSuppressed(ru_plugin* self, int suppress) {
  if (!CheckGameThread(self, "set_round_termination_suppressed")) return 0;
  SetRoundTerminationSuppressed(suppress != 0);
  return RoundTerminationSuppressed() == (suppress != 0) ? 1 : 0;
}

// ---- v1.2 ---------------------------------------------------------------------------

int ApiFeatureState(ru_plugin* self, const char* name) {
  if (!CheckGameThread(self, "feature_state") || !name || !*name) return -1;
  return FeatureStateByName(name);
}

int ApiIsAdmin(ru_plugin* self, uint64_t steamid64) {
  if (!self || steamid64 == 0) return 0;
  return IsReadyUpAdmin(steamid64) ? 1 : 0;  // asks the plugin admin provider first
}

int ApiWorkshopDownloadProgress(ru_plugin* self, uint64_t id, uint64_t* downloaded, uint64_t* total) {
  if (!self) return 0;
  return steam_ugc::DownloadProgress(id, downloaded, total) ? 1 : 0;
}

}  // namespace

void detail::FillEngineApi(ru_api* a) {
  a->center_html_to_slot = &ApiCenterHtmlToSlot;
  a->center_html_all = &ApiCenterHtmlAll;
  a->get_player = &ApiGetPlayer;
  a->get_player_by_steamid = &ApiGetPlayerBySteam;
  a->for_each_player = &ApiForEachPlayer;
  a->ev_get_int = &ApiEvInt;
  a->ev_get_float = &ApiEvFloat;
  a->ev_get_uint64 = &ApiEvUint64;
  a->ev_get_string = &ApiEvString;
  a->ev_get_player_slot = &ApiEvPlayerSlot;
  a->ev_get_player_controller = &ApiEvController;
  a->ev_get_player_pawn = &ApiEvPawn;
  a->schema_offset = &ApiSchemaOffset;
  a->entity_system_status = &ApiEntitySystemStatus;
  a->entity_by_index = &ApiEntityByIndex;
  a->entity_from_handle = &ApiEntityFromHandle;
  a->entity_handle_of = &ApiEntityHandleOf;
  a->entity_classname = &ApiEntityClassname;
  a->entity_mark_changed = &ApiEntityMarkChanged;
  a->econ_attr_set_by_name = &ApiEconAttrSet;
  a->entity_change_subclass = &ApiChangeSubclass;
  a->entity_set_model = &ApiSetModel;
  a->entity_set_bodygroup_by_name = &ApiSetBodygroup;
  a->set_round_termination_suppressed = &ApiSetRoundTermSuppressed;
  a->is_admin = &ApiIsAdmin;
  a->feature_state = &ApiFeatureState;
  a->entity_set_abs_origin = &ApiSetAbsOrigin;  // v1.3
  a->workshop_download_progress = &ApiWorkshopDownloadProgress;  // v1.4
}

}  // namespace readyup::plugins

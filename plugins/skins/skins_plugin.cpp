// readyup-skins: weapon paints, knives, gloves and agent models from the web-managed
// readyup_weapon_* tables (docs/db-contract.md).
//
// Ships separately from the core (csgo/readyup/plugins/skins.so plus the gamedata fragment
// engine-surface.skins.json next to the core) because servers running skin changers risk GSLT
// bans. Everything engine-facing goes through ru_api; the plugin has no signatures, offsets or
// hooks of its own.
//
//   skins_status               (console)  DB / cache / apply counters
//   skins_refresh [steamid64]  (console)  refetch one player's (or everyone's) loadout
//   skins_debug_as <slot> <steamid64>  (console, debug=1 only) decorate a bot with a player's
//                              loadout, to test the apply path without a human client
#include "skins.h"

#include "apply_internal.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

#ifndef SKINS_VERSION
#define SKINS_VERSION "0.0.0-dev"
#endif

namespace skins {

const ru_api* g_api = nullptr;

void Log(int level, const char* fmt, ...) {
  if (!g_api) return;
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  g_api->log(g_api->self, level, buf);
}

bool DebugOn() { return g_api && g_api->debug_enabled(g_api->self) != 0; }

int SchemaOffset(const char* cls, const char* field) { return g_api->schema_offset(g_api->self, cls, field); }

namespace {

bool g_haveDb = false;

void OnTick(void*, const ru_tick_info*) {
  try {
    GameFrameTick();
  } catch (const std::exception& e) {
    Log(RU_LOG_ERROR, "tick threw: %s", e.what());
  }
}

uint64_t SteamOfEventPlayer(const ru_game_event* ev, const char* key) {
  const int slot = g_api->ev_get_player_slot(g_api->self, ev, key);
  if (slot < 0) return 0;
  ru_player p{};
  p.struct_size = sizeof(p);
  return g_api->get_player(g_api->self, slot, &p) == 1 ? p.steamid64 : 0;
}

// Spawn / pickup / equip: prefetch the loadout so it is cached by the time GameFrameTick sees the
// new pawn or weapon (item_equip's "item" is a classname string, not an entity).
void OnPrefetchEvent(void*, const char*, const ru_game_event* ev) {
  if (const uint64_t sid = SteamOfEventPlayer(ev, "userid")) MaybeRefreshAsync(sid);
}

void OnDeathEvent(void*, const char*, const ru_game_event* ev) {
  try {
    OnPlayerDeath(ev);
  } catch (...) {
  }
}

void OnStatus(void*, const ru_command_ctx*) {
  Log(RU_LOG_INFO, "status: version " SKINS_VERSION "; loadouts: %s; apply: %s; entity system: %s", LoadoutStatus().c_str(),
      ApplyStatus().c_str(),
      g_api->entity_system_status(g_api->self) == RU_ENTSYS_OK       ? "ok"
      : g_api->entity_system_status(g_api->self) == RU_ENTSYS_FAILED ? "FAILED"
                                                                      : "pending (no map yet)");
}

void OnRefresh(void*, const ru_command_ctx* ctx) {
  if (ctx->argc >= 2) {
    const uint64_t sid = std::strtoull(ctx->argv[1], nullptr, 10);
    Invalidate(sid);
    MaybeRefreshAsync(sid);
    Log(RU_LOG_INFO, "refreshing loadout for %llu", static_cast<unsigned long long>(sid));
    return;
  }
  int n = 0;
  g_api->for_each_player(
      g_api->self,
      [](void* user, const ru_player* p) {
        if (p->steamid64 != 0) {
          Invalidate(p->steamid64);
          MaybeRefreshAsync(p->steamid64);
          ++*static_cast<int*>(user);
        }
        return 1;
      },
      &n);
  Log(RU_LOG_INFO, "refreshing loadouts for %d connected player(s)", n);
}

void OnDebugAs(void*, const ru_command_ctx* ctx) {
  if (!DebugOn()) {
    Log(RU_LOG_WARN, "skins_debug_as needs readyup.cfg debug=1");
    return;
  }
  if (ctx->argc < 3) {
    Log(RU_LOG_INFO, "usage: skins_debug_as <bot slot> <steamid64|0>");
    return;
  }
  const int slot = std::atoi(ctx->argv[1]);
  const uint64_t sid = std::strtoull(ctx->argv[2], nullptr, 10);
  if (!SetDebugAs(slot, sid)) {
    Log(RU_LOG_WARN, "bad slot %d", slot);
    return;
  }
  if (sid) MaybeRefreshAsync(sid);
  Log(RU_LOG_INFO, "debug: slot %d uses the loadout of %llu (bots only; takes effect on respawn / new weapons)",
      slot, static_cast<unsigned long long>(sid));
}

}  // namespace
}  // namespace skins

using namespace skins;

extern "C" {

READYUP_PLUGIN_EXPORT const ru_plugin_info* readyup_plugin_info(void) {
  static const ru_plugin_info info = {
      sizeof(ru_plugin_info),
      (1u << 16) | 1u,  // needs API 1.1 (entities, schema, raw events, is_admin, config_dir)
      "skins",
      SKINS_VERSION,
      "Ready Up",
      "weapon paints, knives, gloves and agents from the readyup_weapon_* tables",
  };
  return &info;
}

READYUP_PLUGIN_EXPORT int readyup_plugin_load(const ru_api* api, uint32_t core_api_version) {
  if (RU_API_VERSION_MAJOR(core_api_version) != 1 || !RU_API_HAS(api, stash_get)) return 1;
  try {
    g_api = api;
    detail::ResetOffsets();
    g_haveDb = LoadoutStart();
    if (!g_haveDb) Log(RU_LOG_WARN, "loadouts unavailable (%s); skins stay idle", LoadoutStatus().c_str());
    api->on_tick(api->self, OnTick, nullptr);
    api->subscribe_game_event(api->self, "player_spawn", OnPrefetchEvent, nullptr);
    api->subscribe_game_event(api->self, "item_equip", OnPrefetchEvent, nullptr);
    api->subscribe_game_event(api->self, "item_pickup", OnPrefetchEvent, nullptr);
    api->subscribe_game_event(api->self, "player_death", OnDeathEvent, nullptr);
    api->register_console_command(api->self, "skins_status", OnStatus, nullptr);
    api->register_console_command(api->self, "skins_refresh", OnRefresh, nullptr);
    api->register_console_command(api->self, "skins_debug_as", OnDebugAs, nullptr);
    Log(RU_LOG_INFO, "loaded " SKINS_VERSION " (%s)", LoadoutStatus().c_str());
    return 0;
  } catch (const std::exception& e) {
    Log(RU_LOG_ERROR, "load failed: %s", e.what());
    LoadoutStop();
    return 2;
  }
}

READYUP_PLUGIN_EXPORT void readyup_plugin_unload(void) {
  // Join the DB worker before the image is unmapped. Paints already applied stay on live
  // entities (ordinary entity state; they reset on respawn / map change).
  try {
    LoadoutStop();
  } catch (...) {
  }
  Log(RU_LOG_INFO, "unloaded");
  g_api = nullptr;
}

}  // extern "C"
